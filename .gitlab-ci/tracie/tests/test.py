import logging
import pytest
import re
import shutil
import xml.etree.ElementTree as ET

from os import environ, chdir
from os.path import dirname, exists, realpath

import tracie


RESULTS_YAML = "results/results.yml"
JUNIT_XML = "results/junit.xml"
TRACE_LOG_TEST1 = "results/trace1/test/gl-test-device/magenta.testtrace.log"
TRACE_LOG_TEST2 = "results/trace2/test/vk-test-device/olive.testtrace.log"
TRACE_PNG_TEST1 = "results/trace1/test/gl-test-device/magenta.testtrace-0.png"
TRACE_PNG_TEST2 = "results/trace2/test/vk-test-device/olive.testtrace-0.png"
TRACIE_DIR = dirname(realpath(__file__)) + "/.."

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger()


def write_to(content, filename):
    with open(filename, 'w') as f:
        f.write(content)


def read_from(filename):
    with open(filename) as f:
        content = f.read()
    return content


def run_tracie():
    '''
    Run tests for the .testtrace types, using the "gl-test-device" and
    "vk-test-device" device names.
    '''
    result = tracie.main(["--device-name", "gl-test-device",
                          "--file", "./tests/traces.yml"])
    if not result:
        return False
    result = tracie.main(["--device-name", "vk-test-device",
                          "--file", "./tests/traces.yml"])
    return result


def prepare_for_run(tmp_path):
    '''
    Copy all the tracie scripts to the test dir for the unit tests.
    This avoids polluting the normal working dir with test result artifacts.
    '''
    test_dir = str(tmp_path) + "/run"
    shutil.copytree(TRACIE_DIR, test_dir)
    # Change the working dir to the test_dir
    chdir(test_dir)
    # Set the traces-db
    shutil.move("./tests/test-data", "./traces-db")
    # Disable trace storing
    environ["TRACIE_STORE_IMAGES"] = "0"
    environ["TRACIE_UPLOAD_TO_MINIO"] = "0"
    environ["CI_PROJECT_PATH"] = "test-project"
    environ["CI_PIPELINE_ID"] = "667"
    environ["CI_JOB_ID"] = "42"

def cleanup(tmp_path):
    '''
    Performs the clean up of the test dir.
    '''
    if exists(tmp_path):
        shutil.rmtree(tmp_path)


@pytest.fixture(autouse=True)
def run_test(tmp_path):
    '''
    Wraps the execution of each test as follows:

      prepare_for_run()
      test()
      cleanup()
    '''
    logger.debug("Working dir: %s", tmp_path)
    prepare_for_run(tmp_path)
    yield
    cleanup(tmp_path)


def check_results_yaml_content(filename, expectations):
    '''
    Checks the content of the filename with the list of expectations
    passed as parameter.

    Arguments:
        filename (str): The path of the file to check
        expectations (list): A list with the content to find in the file

    Returns:
        bool: The return value. True if the content of the filename satisfies
              the expectations, False otherwise.
    '''
    content = read_from(filename)
    for e in expectations:
        ocurrencies = re.findall(e, content)
        if not len(ocurrencies):
            logger.error("Expectation not found in %s: %s", filename, e)
            return False
    return True


def test_tracie_succeeds_if_all_images_match():
    assert run_tracie()
    expectations = [
        "actual: 5efda83854befe0155ff8517a58d5b51",
        "expected: 5efda83854befe0155ff8517a58d5b51",
    ]
    assert check_results_yaml_content(RESULTS_YAML, expectations)


def test_tracie_fails_on_image_mismatch():
    filename = "./tests/traces.yml"
    content = read_from(filename)
    content = content.replace("5efda83854befe0155ff8517a58d5b51",
                              "8e0a801367e1714463475a824dab363b")
    write_to(content, filename)
    assert not run_tracie()
    expectations = [
        "actual: 5efda83854befe0155ff8517a58d5b51",
        "expected: 8e0a801367e1714463475a824dab363b",
        "trace2/test/vk-test-device/olive.testtrace-0.png"
    ]
    assert check_results_yaml_content(RESULTS_YAML, expectations)


def test_tracie_traces_with_and_without_checksum():
    filename = "./tests/traces.yml"
    content = read_from(filename)
    content += '''  - path: trace1/red.testtrace
    expectations:
    - device: bla
      checksum: 000000000000000'''
    write_to(content, filename)

    # red.testtrace should be skipped, since it doesn't
    # have any checksums for our device
    filename = "./traces-db/trace1/red.testtrace"
    content = "ff0000ff"
    write_to(content, filename)
    assert run_tracie()


def test_tracie_only_traces_without_checksum():
    filename = "./tests/traces.yml"
    content = '''traces:
  - path: trace1/red.testtrace
    expectations:
    - device: bla
      checksum: 000000000000000'''
    write_to(content, filename)

    # red.testtrace should be skipped, since it doesn't
    # have any checksums for our device
    filename = "./traces-db/trace1/red.testtrace"
    content = "ff0000ff"
    write_to(content, filename)
    assert run_tracie()


def test_tracie_with_no_traces():
    filename = "./tests/traces.yml"
    content = 'traces:'
    write_to(content, filename)
    assert run_tracie()
    expectations = [
        "{}",
    ]
    assert check_results_yaml_content(RESULTS_YAML, expectations)


def test_tracie_fails_on_dump_image_error():
    # "invalid" should fail to parse as rgba and
    # cause an error
    filename = "./traces-db/trace1/magenta.testtrace"
    write_to("invalid\n", filename)
    run_tracie()
    expectations = [
        "actual: error",
        "expected: 8e0a801367e1714463475a824dab363b",
        "trace1/magenta.testtrace",
    ]
    assert check_results_yaml_content(RESULTS_YAML, expectations)


def test_tracie_stores_only_logs_on_checksum_match():
    assert run_tracie()
    assert exists(TRACE_LOG_TEST1)
    assert exists(TRACE_LOG_TEST2)
    assert not exists(TRACE_PNG_TEST1)
    assert not exists(TRACE_PNG_TEST2)


def test_tracie_stores_images_on_checksum_mismatch():
    filename = "./tests/traces.yml"
    content = read_from(filename)
    content = content.replace("5efda83854befe0155ff8517a58d5b51",
                              "8e0a801367e1714463475a824dab363b")
    write_to(content, filename)
    assert not run_tracie()
    assert not exists(TRACE_PNG_TEST1)
    assert exists(TRACE_PNG_TEST2)


def test_tracie_stores_images_on_request():
    environ["TRACIE_STORE_IMAGES"] = "1"
    assert run_tracie()
    assert exists(TRACE_PNG_TEST1)
    assert exists(TRACE_PNG_TEST2)

def test_tracie_writes_junit_xml():
    assert run_tracie()
    junit_xml = ET.parse(JUNIT_XML)
    assert junit_xml.getroot().tag == 'testsuites'
    testsuites = junit_xml.findall("./testsuite")
    testcases_gl = junit_xml.findall("./testsuite[@name='traces.yml:gl-test-device']/testcase")
    testcases_vk = junit_xml.findall("./testsuite[@name='traces.yml:vk-test-device']/testcase")

    assert len(testsuites) == 2
    assert len(testcases_gl) == 1
    assert len(testcases_vk) == 1
    assert testcases_gl[0].get("name") == "trace1/magenta.testtrace"
    assert testcases_gl[0].get("classname") == "traces.yml:gl-test-device"
    assert testcases_vk[0].get("name") == "trace2/olive.testtrace"
    assert testcases_vk[0].get("classname") == "traces.yml:vk-test-device"

def test_tracie_writes_dashboard_url_in_junit_xml_failure_tag():
    filename = "./tests/traces.yml"
    content = read_from(filename)
    content = content.replace("5efda83854befe0155ff8517a58d5b51",
                              "8e0a801367e1714463475a824dab363b")
    write_to(content, filename)

    assert not run_tracie()

    junit_xml = ET.parse(JUNIT_XML)
    failures_gl = junit_xml.findall("./testsuite[@name='traces.yml:gl-test-device']/testcase/failure")
    failures_vk = junit_xml.findall("./testsuite[@name='traces.yml:vk-test-device']/testcase/failure")

    assert len(failures_gl) == 0
    assert len(failures_vk) == 1
    dashboard_url = "https://tracie.freedesktop.org/dashboard/imagediff/test-project/42/trace2/olive.testtrace"
    assert dashboard_url in failures_vk[0].text
