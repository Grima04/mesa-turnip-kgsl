#!/usr/bin/env python3

import argparse
import difflib
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--i965_asm',
                    help='path to i965_asm binary')
parser.add_argument('--gen_name',
                    help='name of the hardware generation (as understood by i965_asm)')
parser.add_argument('--gen_folder',
                    type=pathlib.Path,
                    help='name of the folder for the generation')
args = parser.parse_args()

success = True

for asm_file in args.gen_folder.glob('*.asm'):
    expected_file = asm_file.stem + '.expected'
    expected_path = args.gen_folder / expected_file
    out_path = tempfile.NamedTemporaryFile()

    subprocess.run([args.i965_asm,
                    '--type', 'hex',
                    '--gen', args.gen_name,
                    '--output', out_path.name,
                    asm_file],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.STDOUT)

    with expected_path.open() as f:
        lines_before = f.readlines()
    lines_after = [line.decode('ascii') for line in out_path]

    diff = ''.join(difflib.unified_diff(lines_before, lines_after,
                                        expected_file, asm_file.stem + '.out'))

    if diff:
        print('Output comparison for {}:'.format(asm_file.name))
        print(diff)
        success = False
    else:
        print('{} : PASS'.format(asm_file.name))

if not success:
    exit(1)
