#!/bin/sh

TRACIE_DIR="$(dirname "$(readlink -f "$0")")/.."
TEST_DIR=""
TEST_EXIT=0

create_repo() {
    repo="$(mktemp -d $TEST_DIR/repo.XXXXXXXXXX)"
    cp -R "$TEST_DIR"/tests/test-data/* "$repo"
    (
     cd "$repo";
     git init -q .;
     git config user.email "me@example.com"
     git config user.name "Me me"
     git lfs track '*.testtrace' > /dev/null;
     git add .;
     git commit -q -a -m 'initial';
    )
    echo $repo
}

destroy_repo() {
    [ -d "$1"/.git ] && rm -rf "$1"
}

assert() {
    if ! $1; then
        echo "Assertion failed:  \"$1\""
        exit 1
    fi
}

run_tracie() {
    # Run tests for the .testtrace types, using the "test-device" device name.
    DEVICE_NAME=test-device CI_PROJECT_DIR="$TEST_DIR" \
        "$TEST_DIR/tracie.sh" "$TEST_DIR/tests/traces.yml" testtrace
}

cleanup() {
    rm -rf "$TEST_DIR"
}

prepare_for_run() {
    TEST_DIR="$(mktemp -d -t tracie.test.XXXXXXXXXX)"
    # Copy all the tracie scripts to the the test dir and later make that the
    # CI_PROJECT_DIR for the run-tests.sh script. This avoids polluting the
    # normal working dir with test result artifacts.
    cp -R "$TRACIE_DIR"/. "$TEST_DIR"
    trap cleanup EXIT
    # Ensure we have a clean environment.
    unset TRACIE_STORE_IMAGES
}

run_test() {
    prepare_for_run
    log=$(mktemp)
    if ($1 > "$log" 2>&1 ;); then
        if [ -t 1 ]; then
            echo "$1: \e[0;32mSuccess\e[0m"
        else
            echo "$1: Success"
        fi
    else
        if [ -t 1 ]; then
            echo "$1: \e[0;31mFail\e[0m"
        else
            echo "$1: Fail"
        fi
        cat "$log"
        TEST_EXIT=1
    fi
    rm "$log"
    cleanup
}

tracie_succeeds_if_all_images_match() {
    repo="$(create_repo)"
    cd "$repo"

    run_tracie
    assert "[ $? = 0 ]"

    destroy_repo "$repo"
}

tracie_fails_on_image_mismatch() {
    repo="$(create_repo)"
    cd "$repo"

    sed -i 's/5efda83854befe0155ff8517a58d5b51/8e0a801367e1714463475a824dab363b/g' \
        "$TEST_DIR/tests/traces.yml"

    run_tracie
    assert "[ $? != 0 ]"

    destroy_repo "$repo"
}

tracie_ignores_unspecified_trace_types() {
    repo="$(create_repo)"
    cd "$repo"

    echo "  - path: trace1/empty.trace" >> "$TEST_DIR/tests/traces.yml"
    echo "    expectations:" >> "$TEST_DIR/tests/traces.yml"
    echo "    - device: test-device" >> "$TEST_DIR/tests/traces.yml"
    echo "      checksum: 000000000000000" >> "$TEST_DIR/tests/traces.yml"
    # For the tests we only scan for the .testtrace type,
    # so the .trace file added below should be ignored.
    echo "empty" > trace1/empty.trace
    git lfs track '*.trace'
    git add trace1
    git commit -a -m 'break'

    run_tracie
    assert "[ $? = 0 ]"

    destroy_repo "$repo"
}

tracie_skips_traces_without_checksum() {
    repo="$(create_repo)"
    cd "$repo"

    echo "  - path: trace1/red.testtrace" >> "$TEST_DIR/tests/traces.yml"
    echo "    expectations:" >> "$TEST_DIR/tests/traces.yml"
    echo "    - device: bla" >> "$TEST_DIR/tests/traces.yml"
    echo "      checksum: 000000000000000" >> "$TEST_DIR/tests/traces.yml"
    # red.testtrace should be skipped, since it doesn't
    # have any checksums for our device
    echo "ff0000ff" > trace1/red.testtrace
    git add trace1
    git commit -a -m 'red'

    run_tracie
    assert "[ $? = 0 ]"

    destroy_repo "$repo"
}

tracie_fails_on_dump_image_error() {
    repo="$(create_repo)"
    cd "$repo"

    # "invalid" should fail to parse as rgba and
    # cause an error
    echo "invalid" > trace1/magenta.testtrace
    git add trace1
    git commit -a -m 'invalid'

    run_tracie
    assert "[ $? != 0 ]"

    destroy_repo "$repo"
}

tracie_stores_only_logs_on_checksum_match() {
    repo="$(create_repo)"
    cd "$repo"

    run_tracie
    assert "[ $? = 0 ]"

    assert "[ -f "$TEST_DIR/results/trace1/test/test-device/magenta.testtrace.log" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/test-device/olive.testtrace.log" ]"

    assert "[ ! -f "$TEST_DIR/results/trace1/test/test-device/magenta.testtrace-0.png" ]"
    assert "[ ! -f "$TEST_DIR/results/trace2/test/test-device/olive.testtrace-0.png" ]"

    ls -lR "$TEST_DIR"

    destroy_repo "$repo"
}

tracie_stores_images_on_checksum_mismatch() {
    repo="$(create_repo)"
    cd "$repo"

    sed -i 's/5efda83854befe0155ff8517a58d5b51/8e0a801367e1714463475a824dab363b/g' \
        "$TEST_DIR/tests/traces.yml"

    run_tracie
    assert "[ $? != 0 ]"

    assert "[ ! -f "$TEST_DIR/results/trace1/test/test-device/magenta.testtrace-0.png" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/test-device/olive.testtrace-0.png" ]"

    destroy_repo "$repo"
}

tracie_stores_images_on_request() {
    repo="$(create_repo)"
    cd "$repo"

    (export TRACIE_STORE_IMAGES=1; run_tracie)
    assert "[ $? = 0 ]"

    assert "[ -f "$TEST_DIR/results/trace1/test/test-device/magenta.testtrace-0.png" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/test-device/olive.testtrace-0.png" ]"

    ls -lR "$TEST_DIR"

    destroy_repo "$repo"
}

run_test tracie_succeeds_if_all_images_match
run_test tracie_fails_on_image_mismatch
run_test tracie_ignores_unspecified_trace_types
run_test tracie_skips_traces_without_checksum
run_test tracie_fails_on_dump_image_error
run_test tracie_stores_only_logs_on_checksum_match
run_test tracie_stores_images_on_checksum_mismatch
run_test tracie_stores_images_on_request

exit $TEST_EXIT
