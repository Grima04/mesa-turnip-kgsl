#!/usr/bin/env python3

from jinja2 import Environment, FileSystemLoader
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("--template")
parser.add_argument("--base-artifacts-url")
parser.add_argument("--arch")
parser.add_argument("--device-type")
parser.add_argument("--kernel-image-name")
parser.add_argument("--gpu-version")
args = parser.parse_args()

env = Environment(loader = FileSystemLoader('.'), trim_blocks=True, lstrip_blocks=True)
template = env.get_template(args.template)

values = {}
values['base_artifacts_url'] = args.base_artifacts_url
values['arch'] = args.arch
values['device_type'] = args.device_type
values['kernel_image_name'] = args.kernel_image_name
values['gpu_version'] = args.gpu_version

print(template.render(values))
