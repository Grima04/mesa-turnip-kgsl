#!/bin/sh

TRACIE_DIR="$(dirname "$(readlink -f "$0")")/.."
TEST_DIR=""
TEST_EXIT=0

assert() {
    if ! $1; then
        echo "Assertion failed:  \"$1\""
        exit 1
    fi
}

run_tracie() {
    # Run tests for the .testtrace types, using the "gl-test-device" and "vk-test-device" device names.
    python3 $TEST_DIR/tracie.py --file $TEST_DIR/tests/traces.yml --device-name gl-test-device && \
    python3 $TEST_DIR/tracie.py --file $TEST_DIR/tests/traces.yml --device-name vk-test-device
}

cleanup() {
    [ "$TEST_DIR" = "/tmp/*" ] && rm -rf "$TEST_DIR"
}

prepare_for_run() {
    TEST_DIR="$(mktemp -d -t tracie.test.XXXXXXXXXX)"
    # Copy all the tracie scripts to the test dir for the run-tests.sh script.
    # This avoids polluting the normal working dir with test result artifacts.
    cp -R "$TRACIE_DIR"/. "$TEST_DIR"
    cd "$TEST_DIR"
    mkdir traces-db
    mv tests/test-data/* traces-db/.
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
    run_tracie
    assert "[ $? = 0 ]"
}

tracie_fails_on_image_mismatch() {
    sed -i 's/5efda83854befe0155ff8517a58d5b51/8e0a801367e1714463475a824dab363b/g' \
        "$TEST_DIR/tests/traces.yml"

    run_tracie
    assert "[ $? != 0 ]"
}

tracie_skips_traces_without_checksum() {
    echo "  - path: trace1/red.testtrace" >> "$TEST_DIR/tests/traces.yml"
    echo "    expectations:" >> "$TEST_DIR/tests/traces.yml"
    echo "    - device: bla" >> "$TEST_DIR/tests/traces.yml"
    echo "      checksum: 000000000000000" >> "$TEST_DIR/tests/traces.yml"
    # red.testtrace should be skipped, since it doesn't
    # have any checksums for our device
    echo "ff0000ff" > traces-db/trace1/red.testtrace

    run_tracie
    assert "[ $? = 0 ]"
}

tracie_fails_on_dump_image_error() {
    # "invalid" should fail to parse as rgba and
    # cause an error
    echo "invalid" > traces-db/trace1/magenta.testtrace

    run_tracie
    assert "[ $? != 0 ]"
}

tracie_stores_only_logs_on_checksum_match() {
    run_tracie
    assert "[ $? = 0 ]"

    assert "[ -f "$TEST_DIR/results/trace1/test/gl-test-device/magenta.testtrace.log" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/vk-test-device/olive.testtrace.log" ]"

    assert "[ ! -f "$TEST_DIR/results/trace1/test/gl-test-device/magenta.testtrace-0.png" ]"
    assert "[ ! -f "$TEST_DIR/results/trace2/test/vk-test-device/olive.testtrace-0.png" ]"

    ls -lR "$TEST_DIR"
}

tracie_stores_images_on_checksum_mismatch() {
    sed -i 's/5efda83854befe0155ff8517a58d5b51/8e0a801367e1714463475a824dab363b/g' \
        "$TEST_DIR/tests/traces.yml"

    run_tracie
    assert "[ $? != 0 ]"

    assert "[ ! -f "$TEST_DIR/results/trace1/test/gl-test-device/magenta.testtrace-0.png" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/vk-test-device/olive.testtrace-0.png" ]"
}

tracie_stores_images_on_request() {
    (export TRACIE_STORE_IMAGES=1; run_tracie)
    assert "[ $? = 0 ]"

    assert "[ -f "$TEST_DIR/results/trace1/test/gl-test-device/magenta.testtrace-0.png" ]"
    assert "[ -f "$TEST_DIR/results/trace2/test/vk-test-device/olive.testtrace-0.png" ]"

    ls -lR "$TEST_DIR"
}

run_test tracie_succeeds_if_all_images_match
run_test tracie_fails_on_image_mismatch
run_test tracie_skips_traces_without_checksum
run_test tracie_fails_on_dump_image_error
run_test tracie_stores_only_logs_on_checksum_match
run_test tracie_stores_images_on_checksum_mismatch
run_test tracie_stores_images_on_request

exit $TEST_EXIT
