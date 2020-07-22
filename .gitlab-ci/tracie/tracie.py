import argparse
import base64
import datetime
import enum
import glob
import hashlib
import hmac
import json
import os
import requests
import sys
import tempfile
import time
import yaml
import shutil

from email.utils import formatdate
from pathlib import Path
from PIL import Image
from urllib import parse

import dump_trace_images

TRACES_DB_PATH = "./traces-db/"
RESULTS_PATH = "./results/"

def replay(trace_path, device_name):
    success = dump_trace_images.dump_from_trace(trace_path, [], device_name)

    if not success:
        print("[check_image] Trace %s couldn't be replayed. See above logs for more information." % (str(trace_path)))
        return None, None, None
    else:
        base_path = trace_path.parent
        file_name = trace_path.name
        files = glob.glob(str(base_path / "test" / device_name / (file_name + "-*" + ".png")))
        assert(files)
        image_file = files[0]
        files = glob.glob(str(base_path / "test" / device_name / (file_name + ".log")))
        assert(files)
        log_file = files[0]
        return hashlib.md5(Image.open(image_file).tobytes()).hexdigest(), image_file, log_file

def gitlab_ensure_trace(project_url, trace):
    trace_path = TRACES_DB_PATH + trace['path']
    if project_url is None:
        if not os.path.exists(trace_path):
            print("{} missing".format(trace_path))
            sys.exit(1)
        return

    os.makedirs(os.path.dirname(trace_path), exist_ok=True)

    if os.path.exists(trace_path):
        return

    print("[check_image] Downloading trace %s" % (trace['path']), end=" ", flush=True)
    download_time = time.time()
    r = requests.get(project_url + trace['path'])
    open(trace_path, "wb").write(r.content)
    print("took %ds." % (time.time() - download_time), flush=True)

def sign_with_hmac(key, message):
    key = key.encode("UTF-8")
    message = message.encode("UTF-8")

    signature = hmac.new(key, message, hashlib.sha1).digest()

    return base64.encodebytes(signature).strip().decode()

def upload_artifact(file_name, key, content_type):
    with open('.minio_credentials', 'r') as f:
        credentials = json.load(f)["minio-packet.freedesktop.org"]
        minio_key = credentials["AccessKeyId"]
        minio_secret = credentials["SecretAccessKey"]
        minio_token = credentials["SessionToken"]

    resource = '/artifacts/%s/%s/%s/%s' % (os.environ['CI_PROJECT_PATH'],
                                           os.environ['CI_PIPELINE_ID'],
                                           os.environ['CI_JOB_ID'],
                                           key)
    date = formatdate(timeval=None, localtime=False, usegmt=True)
    url = 'https://minio-packet.freedesktop.org%s' % (resource)
    to_sign = "PUT\n\n%s\n%s\nx-amz-security-token:%s\n%s" % (content_type, date, minio_token, resource)
    signature = sign_with_hmac(minio_secret, to_sign)

    with open(file_name, 'rb') as data:
        headers = {'Host': 'minio-packet.freedesktop.org',
                   'Date': date,
                   'Content-Type': content_type,
                   'Authorization': 'AWS %s:%s' % (minio_key, signature),
                   'x-amz-security-token': minio_token}
        print("Uploading artifact to %s" % url);
        r = requests.put(url, headers=headers, data=data)
        #print(r.text)
        r.raise_for_status()

def gitlab_check_trace(project_url, device_name, trace, expectation):
    gitlab_ensure_trace(project_url, trace)

    result = {}
    result[trace['path']] = {}
    result[trace['path']]['expected'] = expectation['checksum']

    trace_path = Path(TRACES_DB_PATH + trace['path'])
    checksum, image_file, log_file = replay(trace_path, device_name)
    if checksum is None:
        result[trace['path']]['actual'] = 'error'
        return False, result
    elif checksum == expectation['checksum']:
        print("[check_image] Images match for %s" % (trace['path']))
        ok = True
    else:
        print("[check_image] Images differ for %s (expected: %s, actual: %s)" %
                (trace['path'], expectation['checksum'], checksum))
        print("[check_image] For more information see "
                "https://gitlab.freedesktop.org/mesa/mesa/blob/master/.gitlab-ci/tracie/README.md")
        ok = False

    trace_dir = os.path.split(trace['path'])[0]
    dir_in_results = os.path.join(trace_dir, "test", device_name)
    results_path = os.path.join(RESULTS_PATH, dir_in_results)
    os.makedirs(results_path, exist_ok=True)
    shutil.move(log_file, os.path.join(results_path, os.path.split(log_file)[1]))
    if not ok and os.environ.get('TRACIE_UPLOAD_TO_MINIO', '0') == '1':
        upload_artifact(image_file, 'traces/%s.png' % checksum, 'image/png')
    if not ok or os.environ.get('TRACIE_STORE_IMAGES', '0') == '1':
        image_name = os.path.split(image_file)[1]
        shutil.move(image_file, os.path.join(results_path, image_name))
        result[trace['path']]['image'] = os.path.join(dir_in_results, image_name)

    result[trace['path']]['actual'] = checksum

    return ok, result

def run(filename, device_name):

    with open(filename, 'r') as f:
        y = yaml.safe_load(f)

    if "traces-db" in y:
        project_url = y["traces-db"]["download-url"]
    else:
        project_url = None

    traces = y['traces'] or []
    all_ok = True
    results = {}
    for trace in traces:
        for expectation in trace['expectations']:
            if expectation['device'] == device_name:
                ok, result = gitlab_check_trace(project_url,
                                                device_name, trace,
                                                expectation)
                all_ok = all_ok and ok
                results.update(result)

    os.makedirs(RESULTS_PATH, exist_ok=True)
    with open(os.path.join(RESULTS_PATH, 'results.yml'), 'w') as f:
        yaml.safe_dump(results, f, default_flow_style=False)
    if os.environ.get('TRACIE_UPLOAD_TO_MINIO', '0') == '1':
        upload_artifact(os.path.join(RESULTS_PATH, 'results.yml'), 'traces/results.yml', 'text/yaml')

    return all_ok

def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument('--file', required=True,
                        help='the name of the traces.yml file listing traces and their checksums for each device')
    parser.add_argument('--device-name', required=True,
                        help="the name of the graphics device used to replay traces")

    args = parser.parse_args(args)
    return run(args.file, args.device_name)

if __name__ == "__main__":
    all_ok = main(sys.argv[1:])
    sys.exit(0 if all_ok else 1)
