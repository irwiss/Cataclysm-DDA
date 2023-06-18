#!/usr/bin/env python3

import argparse
import asyncio
import subprocess
import tempfile
import time
import os
from random import getrandbits as random_getrandbits
from math import ceil, log2
from multiprocessing import cpu_count as multiprocessing_cpu_count
from shutil import rmtree as rm_dir_tree
from signal import SIGINT, SIGTERM
from typing import Callable

# requires pip install
from tqdm.auto import tqdm


def filter_useful_log_messages(msg: str):
    if len(msg) <= 0:
        return False
    if msg.count("Randomness seeded to") > 0:
        return False
    #if msg.count("") > 0:
    #    return False
    if msg == "12:00:00AM" or msg == "12:00:00PM":
        return False
    return True


class TestWorker:
    process: asyncio.subprocess.Process | None
    stdout: list[str]
    stderr: list[str]
    stdout_reader: asyncio.Task[None]
    stderr_reader: asyncio.Task[None]
    cli: list[str]
    user_dir: str
    bar: tqdm
    time_started: time.time
    time_finished: time.time
    report: str
    reproduction_cmd: str

    def __init__(self, cli: list[str], user_dir: str):
        self.time_started = 0
        self.time_finished = 0
        self.stdout = []
        self.stderr = []
        self.cli = cli
        self.user_dir = user_dir
        self.process = None
        # using unit instead of postfix due to bug https://github.com/tqdm/tqdm/issues/712  # noqa E501
        self.bar = tqdm(total=1, leave=True)
        self.bar.bar_format = "{bar:10}{unit}"            # noqa E501
        self.report = ""
        self.reproduction_cmd = ""

    async def start(self):
        self.time_started = time.time()
        self.clean_user_dir()

        self.process = await asyncio.create_subprocess_exec(
            *self.cli, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        self.stdout_reader = asyncio.create_task(
            self.pipe_reader(self.stdout, self.process.stdout))

        self.stderr_reader = asyncio.create_task(
            self.pipe_reader(self.stderr, self.process.stderr))

    async def pipe_reader(self, storage: list[str],
                          pipe: asyncio.StreamReader):
        try:
            while True:
                line = await pipe.readline()
                if line:
                    storage.append(line.decode().strip())
                else:
                    break
        except asyncio.exceptions.CancelledError:
            pass
        except Exception as ex:
            print(ex)
        finally:
            self.time_finished = time.time()

    def update_progress(self, cases_left: int):
        tests_done = self.get_executed_tests()
        exit_code = self.process.returncode if self.process else None
        last_test = ""
        status = ""
        ret = True

        if len(tests_done) > 0:
            last_test = tests_done[-1]

        self.bar.total = cases_left + len(tests_done)
        self.bar.n = len(tests_done)

        if exit_code is None:
            # Still running
            interval = time.time() - self.time_started
            self.bar.colour = "yellow"
            if len(last_test) > 0:
                status = "Testing"
            else:
                status = "Starting"
            ret = True
            # keep status from above
        elif exit_code == 0:
            # Exit with success
            interval = self.time_finished - self.time_started
            self.bar.colour = "green"
            status = "Finished"
            ret = False
        elif exit_code != 0:
            # Exit with failure
            interval = self.time_finished - self.time_started
            self.bar.colour = "red"
            status = f"Exit {exit_code}"
            ret = False
        else:
            exit(1)

        self.bar.unit = ""
        self.bar.unit += f"│ {self.bar.format_interval(interval)}"
        self.bar.unit += f" │ {self.bar.n:4d} / {self.bar.total:4d}"
        #self.bar.unit += f" │ {100 * self.bar.n / self.bar.total:2.0f}%"
        self.bar.unit += f" │ {status.ljust(10)}"
        self.bar.unit += f" │ {last_test}"
        self.bar.refresh()

        return ret

    def set_progress_max(self, total: int):
        self.bar.total = total

    def is_alive(self):
        return self.process is not None and self.process.returncode is None

    def get_executed_tests(self):
        executed = os.path.join(self.user_dir, 'tests_executed_order.txt')
        if not os.path.isfile(executed):
            return []
        return [x.strip() for x in open(executed)]

    def clean_user_dir(self):
        if os.path.isdir(self.user_dir):
            rm_dir_tree(self.user_dir)

    async def stop(self):
        try:
            if self.is_alive():
                self.stdout_reader.cancel()
                self.stderr_reader.cancel()
                self.process.kill()
        except Exception as ex:
            print(ex)

    def ret_code(self):
        return self.process.returncode

    async def finish(self):
        try:
            if self.process.returncode is None:
                await self.process.communicate()
            if self.process.returncode != 0:
                executed_tests = self.get_executed_tests()
                if len(self.stderr) > 0:
                    self.report += f"STDERR (exit code {self.ret_code()}):\n"
                    for x in filter(filter_useful_log_messages, self.stderr):
                        self.report += x + "\n"
                if len(self.stdout) > 0:
                    self.report += f"STDOUT (exit code {self.ret_code()}):\n"
                    for x in filter(filter_useful_log_messages, self.stdout):
                        self.report += x + "\n"

                def filter_useful_args(arg):
                    if arg.startswith("--tests-pool-dir=") or \
                       arg.startswith("--user-dir="):
                        return False
                    return True

                cli = [x for x in self.cli if filter_useful_args(x)]
                command = " ".join(cli) + " "
                tests = ",".join(executed_tests)
                if len(executed_tests) > 0:
                    self.reproduction_cmd = command + tests
                    self.report += "Command to replicate failed job:"
                    self.report += self.reproduction_cmd
            else:
                self.clean_user_dir()
        except Exception as ex:
            print(ex)


def make_worker(idx: int, args: dict[str, any], pool_dir: str):
    user_dir = f"test_user_dir_{idx:02d}"

    cli: list[str] = [
        args["wine"],
        args["test_executable"],
        "--rng-seed " + str(args["rng_seed"]),
        "--user-dir=" + user_dir,
        "--tests-pool-dir=" + pool_dir,
    ]
    cli = [x for x in cli if len(x.strip()) > 0]

    return TestWorker(cli, user_dir)


def get_tests_list(args: dict[str, any],
                   extra_args: list[str] = []) -> list[str]:
    ret = []
    try:
        cli: list[str] = [
            args["wine"],
            args["test_executable"],
            "--list-test-names-only",
            "--drop-world",
            "--user-dir=test_user_dir_list_only"
            "--rng-seed " + str(args["rng_seed"])
        ] + extra_args
        worker = subprocess.run([x for x in cli if len(x) > 0],
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,
                                text=True, encoding='utf-8')
        ret = worker.stdout.splitlines()
    finally:
        return [x for x in ret if len(x.strip()) > 1]


def pool_count(dir: str):
    ret = 0
    for x in os.listdir(dir):
        file = os.path.join(dir, x)
        if not os.path.isfile(file):
            continue
        _, ext = os.path.splitext(file)
        if ext != "":
            continue  # ignore in-progress tests
        ret += 1
    return ret


def make_test_pool_dir(test_cases: list[str]):
    dir = tempfile.TemporaryDirectory()
    for i, tc in enumerate(test_cases):
        with open(dir.name + "/{:05d}".format(i), "w") as f:
            f.write(tc.strip())
    return dir


async def drive_test_suite(args: dict[str, any]):
    tests_list = get_tests_list(args)
    with make_test_pool_dir(tests_list) as pool_dir:
        workers: list[TestWorker] = []
        try:
            worker_idx = 0

            while True:
                workers_alive = 0
                cases_remaining = pool_count(pool_dir)

                for _, w in enumerate(workers):
                    if w.update_progress(cases_remaining):
                        workers_alive += 1

                workers_missing = min(cases_remaining,
                                      args["threads"] - workers_alive)
                if cases_remaining == 0 and \
                   workers_missing == 0 and \
                   workers_alive == 0:
                    break  # no test cases remaining
                else:  # start workers up to a fixed amount
                    if workers_missing > 0:
                        w = make_worker(worker_idx, args, pool_dir)
                        worker_idx += 1
                        w.set_progress_max(len(tests_list))
                        await w.start()
                        w.update_progress(cases_remaining)
                        workers.insert(0, w)

                await asyncio.sleep(0.25)

            return sum([abs(w.process.returncode) for w in workers])
        except asyncio.exceptions.CancelledError:
            return 1
        finally:
            for w in workers:
                try:
                    w.bar.leave = True
                    w.bar.close()
                    await w.stop()
                except Exception as ex:
                    print(ex)
            for w in workers:
                await w.finish()
                print(w.report)


# search by chopping off 1 test at a time from the sides as long
# as the test fails; once both sides succeed the prev_report is
# the reproduction command
def bisect_slice_sides(
        w1: TestWorker | None, w2: TestWorker | None,
        prev_report: str, cases1: list[str], cases2: list[str]):
    try:
        if len(cases2) == 0:  # Starting off
            c1 = cases1[+1:]
            c2 = cases1[:-1]
            return [bisect_slice_sides, prev_report, c1, c2]
        if w1.ret_code() == 0 and w2.ret_code() == 0:
            # bail out (could try toggling individual tests)
            return [None, prev_report, [], []]
        elif w1.ret_code() != 0 and w2.ret_code() != 0:
            # both failed, chop both sides
            c1 = cases1[+1:]
            c2 = cases2[:-1]
            return [bisect_slice_sides, prev_report, c1, c2]
        elif w1.ret_code() != 0:
            c1 = cases1[+1:]
            c2 = cases2
            return [bisect_slice_sides, w1.reproduction_cmd, c1, c2]
        elif w2.ret_code() != 0:
            c1 = cases1
            c2 = cases2[:-1]
            return [bisect_slice_sides, w2.reproduction_cmd, c1, c2]
        else:
            return [None, "-- failed in a weird state --", [], []]
    except Exception as e:
        print(e)


# binary search - slice in the middle, check if which half fails;
# slice failing half etc until both halves succeed, then take the
# half that fails and feed to bisect_slice_sides
def bisect_slice_middle(
    w1: TestWorker | None, w2: TestWorker | None,
        prev_report: str, cases1: list[str], cases2: list[str]):

    if len(cases2) == 0:  # Starting off
        c1 = cases1[:len(cases1) // 2]
        c2 = cases1[len(cases1) // 2:]
        return [bisect_slice_middle, prev_report, c1, c2]
    if (w1.ret_code() != 0 and w2.ret_code() != 0) or \
       (w1.ret_code() == 0 and w2.ret_code() == 0):
        # Exhausted middle split, try slicing from sides
        c1 = cases1 + cases2
        c2 = []
        return bisect_slice_sides(w1, w2, prev_report, c1, c2)
    elif w1.ret_code() != 0:
        c1 = cases1[:len(cases1) // 2]
        c2 = cases1[len(cases1) // 2:]
        return [bisect_slice_middle, w1.reproduction_cmd, c1, c2]
    elif w2.ret_code() != 0:
        c1 = cases2[:len(cases2) // 2]
        c2 = cases2[len(cases2) // 2:]
        return [bisect_slice_middle, w2.reproduction_cmd, c1, c2]
    else:
        return [None, "-- failed in a weird state --", [], []]


async def drive_bisect(args: dict[str, any]):
    tests_list = str(args["test_sequence"]).split(',')
    cases1 = tests_list
    cases2 = []
    bisect_strat = bisect_slice_middle if len(tests_list) > 1 else None
    w1: TestWorker | None = None
    w2: TestWorker | None = None
    report = ""
    while bisect_strat is not None:
        bisect_strat, report, cases1, cases2 = bisect_strat(
            w1, w2, report, cases1, cases2)
        if len(cases1) + len(cases2) < 2:
            break
        print('─' * 20 +
              f" {str(bisect_strat.__name__)} " +
              '─' * 20)
        try:
            with make_test_pool_dir(cases1) as pool_dir1, \
                 make_test_pool_dir(cases2) as pool_dir2:
                w1 = make_worker(0, args, pool_dir1)
                w2 = make_worker(1, args, pool_dir2)

                w1.set_progress_max(len(cases1))
                w2.set_progress_max(len(cases2))

                w1.update_progress(pool_count(pool_dir1))
                w2.update_progress(pool_count(pool_dir2))

                await w1.start()
                await w2.start()

                try:
                    while w1.is_alive() or w2.is_alive():
                        w1.update_progress(pool_count(pool_dir1))
                        w2.update_progress(pool_count(pool_dir2))

                        await asyncio.sleep(0.25)

                    # workers completed, gather results below
                    w1.update_progress(pool_count(pool_dir1))
                    w2.update_progress(pool_count(pool_dir2))
                except asyncio.exceptions.CancelledError as ex:
                    print(ex)
                    return 1
                except Exception as e:
                    print(e)
                    return 2
                finally:
                    for w in [w1, w2]:
                        if w is None:
                            continue
                        try:
                            w.bar.leave = True
                            w.bar.close()
                            await w.stop()
                        except Exception as ex:
                            print(ex)
                    for w in [w1, w2]:
                        if w is None:
                            continue
                        await w.finish()
        except Exception as e:
            print(e)
            return 3

    # just one of the workers failed and only one test left - print report
    if report != "":
        print("Command to reproduce:\n" + report)
    else:
        print("The given test sequence is failing, nothing to bisect")
    print("\nBisect finished.")
    return 0


def run_async(main_coroutine: Callable[[], int]) -> int:
    loop = asyncio.get_event_loop()
    main_task = asyncio.ensure_future(main_coroutine)

    for signal in [SIGINT, SIGTERM]:
        #  handle signals to cancel tasks
        loop.add_signal_handler(signal, main_task.cancel)

    try:
        return loop.run_until_complete(main_task)
    except Exception as ex:
        print(ex)
        return 1
    finally:
        loop.close()


os.system('cls' if os.name == 'nt' else 'clear')

args_parser = argparse.ArgumentParser()
args_parser.formatter_class = argparse.ArgumentDefaultsHelpFormatter
args_parser.add_argument("--test-executable", type=str,
                         help="test executable to run",
                         default="tests/cata_test")
args_parser.add_argument("--test-opts", type=str,
                         help="extra options to pass to test executable",
                         default="")
args_parser.add_argument("--threads", type=int,
                         help="max threads to spawn, Use all cores if -1",
                         default=-1)
args_parser.add_argument("--wine", type=str,
                         help="wine command (optional)",
                         default="")
args_parser.add_argument("--rng-seed", type=int,
                         help="rng seed, if 0 a random value is rolled",
                         default=0)
args_parser.add_argument("--test-sequence", type=str,
                         help="test sequence to bisect",
                         default=0)
args_parser.add_argument("action")

args = vars(args_parser.parse_args())

if args["threads"] < 0:
    args["threads"] = int(multiprocessing_cpu_count())
if args["rng_seed"] == 0:
    args["rng_seed"] = random_getrandbits(32)

if args["action"] == "test":
    ret = run_async(drive_test_suite(args))
    exit(ret)
if args["action"] == "bisect":
    ret = run_async(drive_bisect(args))
    exit(ret)
else:
    args_parser.print_help()
    exit(0)
