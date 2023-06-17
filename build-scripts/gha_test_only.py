#!/usr/bin/env python3

import argparse
import asyncio
import subprocess
import tempfile
import time
import os
from random import getrandbits as random_getrandbits
from multiprocessing import cpu_count as multiprocessing_cpu_count
from shutil import rmtree as rm_dir_tree
from signal import SIGINT, SIGTERM
from typing import Any, Callable

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
        self.bar.unit += f" │ {self.bar.format_interval(interval)}"
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

    async def finish(self):
        try:
            if self.process.returncode is None:
                await self.process.communicate()
            if self.process.returncode != 0:
                executed_tests = self.get_executed_tests()
                if len(self.stderr) > 0:
                    print(f"STDERR (exit code {self.process.returncode}): ")
                    for x in filter(filter_useful_log_messages, self.stderr):
                        print(x)
                if len(self.stdout) > 0:
                    print(f"STDOUT (exit code {self.process.returncode}): ")
                    for x in filter(filter_useful_log_messages, self.stdout):
                        print(x)

                def filter_useful_args(arg):
                    if arg.startswith("--tests-pool-dir=") or \
                       arg.startswith("--user-dir="):
                        return False
                    return True

                cli = [x for x in self.cli if filter_useful_args(x)]
                command = " ".join(cli) + " "
                tests = ",".join(executed_tests)
                if len(executed_tests) > 0:
                    print("Command to replicate:")
                    print(command + tests)
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


def count_test_cases_remaining(dir: str):
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


async def drive_test_suite(args: dict[str, any]):
    with tempfile.TemporaryDirectory() as pool_dir:
        workers: list[TestWorker] = []
        try:
            worker_idx = 0
            tests_list = get_tests_list(args)

            for i, tc in enumerate(tests_list):
                with open(pool_dir + "/{:05d}".format(i), "w") as f:
                    f.write(tc.strip())
            print(f"Put tests into {pool_dir}")

            while True:
                workers_alive = 0
                cases_remaining = count_test_cases_remaining(pool_dir)

                for i, w in enumerate(workers):
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
args_parser.add_argument("action")

args = vars(args_parser.parse_args())

if args["threads"] < 0:
    args["threads"] = int(multiprocessing_cpu_count())
if args["rng_seed"] == 0:
    args["rng_seed"] = random_getrandbits(32)

if args["action"] == "test":
    ret = run_async(drive_test_suite(args))
    exit(ret)
else:
    args_parser.print_help()
