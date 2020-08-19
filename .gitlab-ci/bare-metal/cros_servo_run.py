#!/usr/bin/env python3
#
# Copyright © 2020 Google LLC
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

import argparse
import re
from serial_buffer import SerialBuffer
import sys

class CrosServoRun:
    def __init__(self, cpu, ec):
        self.ec_ser = SerialBuffer(ec, "artifacts/serial-ec.txt", "R SERIAL-EC> ")
        self.cpu_ser = SerialBuffer(cpu, "artifacts/serial.txt", "R SERIAL-CPU> ")

    def ec_write(self, s):
        print("W SERIAL-EC> %s" % s)
        self.ec_ser.serial.write(s.encode())

    def cpu_write(self, s):
        print("W SERIAL-CPU> %s" % s)
        self.cpu_ser.serial.write(s.encode())

    def run(self):
        # Flush any partial commands in the EC's prompt, then ask for a reboot.
        self.ec_write("\n")
        self.ec_write("reboot\n")

        # This is emitted right when the bootloader pauses to check for input.
        # Emit a ^N character to request network boot, because we don't have a
        # direct-to-netboot firmware on cheza.
        for line in self.cpu_ser.lines():
            if re.match("load_archive: loading locale_en.bin", line):
                self.cpu_write("\016")
                break

        tftp_failures = 0
        for line in self.cpu_ser.lines():
            if re.match("---. end Kernel panic", line):
                return 1

            # The Cheza boards have issues with failing to bring up power to
            # the system sometimes, possibly dependent on ambient temperature
            # in the farm.
            if re.match("POWER_GOOD not seen in time", line):
                return 2

            # The Cheza firmware seems to occasionally get stuck looping in
            # this error state during TFTP booting, possibly based on amount of
            # network traffic around it, but it'll usually recover after a
            # reboot.
            if re.match("R8152: Bulk read error 0xffffffbf", line):
                tftp_failures += 1
                if tftp_failures >= 100:
                    return 2

            result = re.match("bare-metal result: (\S*)", line)
            if result:
                if result.group(1) == "pass":
                    return 0
                else:
                    return 1

        print("Reached the end of the CPU serial log without finding a result")
        return 1

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cpu', type=str, help='CPU Serial device', required=True)
    parser.add_argument('--ec', type=str, help='EC Serial device', required=True)
    args = parser.parse_args()

    servo = CrosServoRun(args.cpu, args.ec)

    while True:
        retval = servo.run()
        if retval != 2:
            break

    # power down the CPU on the device
    servo.ec_write("power off\n")

    sys.exit(retval)

if __name__ == '__main__':
    main()
