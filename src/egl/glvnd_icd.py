# Copyright 2019 Intel Corporation
# SPDX-License-Identifier: MIT

import argparse
import json
import os.path

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', help='Output json file.', required=True)
    parser.add_argument('--lib-path', help='Path to libEGL_mesa.so', required=True)
    args = parser.parse_args()

    path = os.path.join(args.lib_path, 'libEGL_mesa.so')

    json_data = {
        'file_format_version': '1.0.0',
        'ICD': {
            'library_path': path,
        },
    }

    with open(args.out, 'w') as f:
        json.dump(json_data, f, indent=4, sort_keys=True, separators=(',', ': '))
