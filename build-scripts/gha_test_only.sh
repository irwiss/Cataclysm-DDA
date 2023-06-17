#!/bin/bash

# Script made specifically for running tests on GitHub Actions

echo "Using bash version $BASH_VERSION"
set -exo pipefail

export NUM_TEST_JOBS="${NUM_TEST_JOBS:-2}"

# We might need binaries installed via pip, so ensure that our personal bin dir is on the PATH
export PATH=$HOME/.local/bin:$PATH

function run_test_job
{
    set -o pipefail
    test_exit_code=0 exit_code=0
    job_number=$1
    test_binary=$2
    rng_seed=$3
    cases_dir=$4
    shift 4

    SECONDS=0
    set -x
    $WINE "$test_binary" \
        --min-duration 5 \
        --use-colour yes \
        --rng-seed $rng_seed \
        --tests-pool-dir="$cases_dir" ${EXTRA_TEST_OPTS} \
        --user-dir=test_user_dir_$job_number "$@"
    local ret=$?
    set +x

    echo $SECONDS > ~job_duration_${job_number}
    echo $ret > ~job_exit_code_${job_number}
    return $?
}
export -f run_test_job

function run_test
{
    set -eo pipefail
    test_exit_code=0 exit_code=0
    test_jobs=$1
    test_binary=$2
    shift 2

    rng_seed=$(date +%s)
    cases_dir=$(mktemp -d)
    test_num=0

    if [ -n "$ONLY_TEST_LOADING" ]; then
        $WINE "$test_binary" --user-dir=test_user_dir --rng-seed $rng_seed "$@"
        return 0 # just checking mods can load, not running actual tests
    else
        SECONDS=0
        rm test_list_pipe 2>/dev/null || true
        mkfifo test_list_pipe
        $WINE "$test_binary" --user-dir=test_user_dir_list_only --rng-seed $rng_seed --list-test-names-only "$@" 2>/dev/null > test_list_pipe &
        set +x
        while IFS= read -r line; do
            if [ ! -z "$line" ]; then
                fname=$(printf "%05d" $test_num)
                echo -n "$line" > "$cases_dir/$fname"
                test_num=$((test_num+1))
            fi
        done < test_list_pipe
        set -x
        rm test_list_pipe
        rm -r test_user_dir_list_only || true

        echo "Random seed is $rng_seed, pool of $test_num tests is at $cases_dir"
        echo "Test cases generated in $SECONDS seconds"
    fi

    SECONDS=0

    ( #subshell
        set -x
        seq -f "%02g" $test_jobs | \
            parallel -j "$test_jobs" --verbose --line-buffer --tagstring "[{}]" \
                    run_test_job {} $test_binary $rng_seed "$cases_dir" "$@"
    )

    set +x
    C_RED='\033[0;31m'
    C_GREEN='\033[1;32m'
    C_DEFAULT='\033[0m' # clears color

    jobs_failed=$test_jobs
    failed_job_suffixes=()
    finished_at=$SECONDS
    my_exit_code=0
    for (( i=1; i<=$test_jobs; i++ )); do
        job_suffix=$(printf '%02d' $i)
        job_exit_file="~job_exit_code_${job_suffix}"
        job_time_file="~job_duration_${job_suffix}"
        if [ ! -s "$job_exit_file" ]; then
            my_exit_code=1
            echo -e "Job ${job_suffix} - ${C_RED}Failed: Missing exit code file, likely worker crashed.${C_DEFAULT}"
            failed_job_suffixes+=(${job_suffix})
        elif [ ! "0" == "$(cat ${job_exit_file})" ] ; then
            my_exit_code=1
            job_time=$(cat ${job_time_file})
            echo -e "Job ${job_suffix} - ${C_RED}Failed in ${job_time}: Exit code $(cat ${job_exit_file}).${C_DEFAULT}"
            failed_job_suffixes+=(${job_suffix})
        else
            job_time=$(cat ${job_time_file})
            echo -e "Job ${job_suffix} - ${C_GREEN}Completed in ${job_time} seconds.${C_DEFAULT}"
            rm -r "test_user_dir_${job_suffix}"
        fi
        rm "${job_exit_file}" 2> /dev/null
        rm "${job_time_file}" 2> /dev/null
    done

    for job_suffix in "${failed_job_suffixes[@]}"; do
        echo "To reproduce job ${job_suffix} run the following command:"
        echo -n "$test_binary --rng-seed $rng_seed "
        readarray -t test_sequence < "test_user_dir_${job_suffix}/tests_executed_order.txt"
        printf -v test_sequence_str '"%s",' "${test_sequence[@]}"
        echo -n $test_sequence_str
        echo $@
    done

    if find "${cases_dir}" -mindepth 1 -maxdepth 1 | read; then
        echo -e "${C_RED}Tests that haven't been executed, likely worker crashed:${C_DEFAULT}"
        for f in $cases_dir/*; do ( echo "${f}: $(cat "${f}")" ); done
        echo
        echo -e "${C_RED}$(ls -1q $cases_dir | wc -l) tests got lost (listed above)${C_DEFAULT}"
        rm -r "$cases_dir"
    fi
    set -x

    return $my_exit_code
}
export -f run_test

if [ "$CMAKE" = "1" ]
then
    bin_path="./"
    if [ "$RELEASE" = "1" ]
    then
        build_type=MinSizeRel
        bin_path="build/tests/"
    else
        build_type=Debug
    fi

    # Run regular tests
    [ -f "${bin_path}cata_test" ] && run_test $NUM_TEST_JOBS $(printf %q "${bin_path}/cata_test")
    [ -f "${bin_path}cata_test-tiles" ] && run_test $NUM_TEST_JOBS $(printf %q "${bin_path}/cata_test-tiles")
else
    export ASAN_OPTIONS=detect_odr_violation=1
    export UBSAN_OPTIONS=print_stacktrace=1

    run_test $NUM_TEST_JOBS "./tests/cata_test"
    if [ -n "$MODS" ]
    then
        run_test $NUM_TEST_JOBS "./tests/cata_test" $(printf %q "${MODS}")
    fi

    if [ -n "$TEST_STAGE" ]
    then
        # Run the tests with all the mods, without actually running any tests,
        # just to verify that all the mod data can be successfully loaded.
        # Because some mods might be mutually incompatible we might need to run a few times.

        export ONLY_TEST_LOADING=1

        ./build-scripts/get_all_mods.py | \
            while read mods
            do
                run_test 1 ./tests/cata_test --mods="${mods}" "~*"
            done
    fi
fi

# vim:tw=0
