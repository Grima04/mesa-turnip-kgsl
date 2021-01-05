#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

# Copyright © 2021 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

"""Helper script for manipulating the release calendar."""

from __future__ import annotations
import argparse
import csv
import contextlib
import datetime
import pathlib
import subprocess
import typing

if typing.TYPE_CHECKING:
    import _csv
    from typing_extensions import Protocol

    class RCArguments(Protocol):
        """Typing information for release-candidate command arguments."""

        manager: str

    class ExtendArguments(Protocol):
        """Typing information for extend command arguments."""

        series: str
        count: int


    CalendarRowType = typing.Tuple[typing.Optional[str], str, str, str, typing.Optional[str]]


_ROOT = pathlib.Path(__file__).parent.parent
CALENDAR_CSV = _ROOT / 'docs' / 'release-calendar.csv'
VERSION = _ROOT / 'VERSION'
LAST_RELEASE = 'This is the last planned release of the {}.x series.'
OR_FINAL = 'Or {}.0 final.'


def read_calendar() -> typing.List[CalendarRowType]:
    """Read the calendar and return a list of it's rows."""
    with CALENDAR_CSV.open('r') as f:
        return [typing.cast('CalendarRowType', tuple(r)) for r in csv.reader(f)]


def commit(message: str) -> None:
    """Commit the changes the the release-calendar.csv file."""
    subprocess.run(['git', 'commit', str(CALENDAR_CSV), '--message', message])



def _calculate_release_start(major: str, minor: str) -> datetime.date:
    """Calclulate the start of the release for release candidates.

    This is quarterly, on the second wednesday, in Januray, April, July, and Octobor.
    """
    quarter = datetime.date.fromisoformat(f'20{major}-0{[1, 4, 7, 10][int(minor)]}-01')

    # Wednesday is 3
    day = quarter.isoweekday()
    if day > 3:
        # this will walk back into the previous month, it's much simpler to
        # duplicate the 14 than handle the calculations for the month and year
        # changing.
        return quarter.replace(day=quarter.day - day + 3 + 14)
    elif day < 3:
        quarter = quarter.replace(day=quarter.day + 3 - day)
    return quarter.replace(day=quarter.day + 14)



def release_candidate(args: RCArguments) -> None:
    """Add release candidate entries."""
    with VERSION.open('r') as f:
        version = f.read().rstrip('-devel')
    major, minor, _ = version.split('.')
    date = _calculate_release_start(major, minor)

    data = read_calendar()

    with CALENDAR_CSV.open('w') as f:
        writer = csv.writer(f)
        writer.writerows(data)

        writer.writerow([f'{major}.{minor}', date.isoformat(), f'{major}.{minor}.0-rc1', args.manager])
        for row in range(2, 4):
            date = date + datetime.timedelta(days=7)
            writer.writerow([None, date.isoformat(), f'{major}.{minor}.0-rc{row}', args.manager])
        date = date + datetime.timedelta(days=7)
        writer.writerow([None, date.isoformat(), f'{major}.{minor}.0-rc4', args.manager, OR_FINAL.format(f'{major}.{minor}')])

    commit(f'docs: Add calendar entries for {major}.{minor} release candidates.')


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers()

    rc = sub.add_parser('release-candidate', aliases=['rc'], help='Generate calendar entries for a release candidate.')
    rc.add_argument('manager', help="the name of the person managing the release.")
    rc.set_defaults(func=release_candidate)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
