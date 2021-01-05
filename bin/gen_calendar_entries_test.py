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

from __future__ import annotations
from unittest import mock
import argparse
import csv
import tempfile
import os
import pathlib

import pytest

from . import gen_calendar_entries


@pytest.fixture(autouse=True, scope='module')
def disable_git_commits() -> None:
    """Mock out the commit function so no git commits are made durring testing."""
    with mock.patch('bin.gen_calendar_entries.commit', mock.Mock()):
        yield


class TestReleaseStart:

    def test_first_is_wednesday(self) -> None:
        d = gen_calendar_entries._calculate_release_start('20', '0')
        assert d.day == 15
        assert d.month == 1
        assert d.year == 2020

    def test_first_is_before_wednesday(self) -> None:
        d = gen_calendar_entries._calculate_release_start('19', '0')
        assert d.day == 16
        assert d.month == 1
        assert d.year == 2019

    def test_first_is_after_wednesday(self) -> None:
        d = gen_calendar_entries._calculate_release_start('21', '0')
        assert d.day == 13
        assert d.month == 1
        assert d.year == 2021


class TestRC:

    ORIGINAL_DATA = [
        ('20.3', '2021-01-13', '20.3.3', 'Dylan Baker', ''),
        ('',     '2021-01-27', '20.3.4', 'Dylan Baker', 'Last planned release of the 20.3.x series'),
    ]

    @pytest.fixture(autouse=True, scope='class')
    def mock_version(self) -> None:
        """Keep the version set at a specific value."""
        with tempfile.TemporaryDirectory() as d:
            v = os.path.join(d, 'version')
            with open(v, 'w') as f:
                f.write('21.0.0-devel\n')

            with mock.patch('bin.gen_calendar_entries.VERSION', pathlib.Path(v)):
                yield

    @pytest.fixture(autouse=True)
    def mock_data(self) -> None:
        """inject our test data.."""
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, 'calendar.csv')
            with open(c, 'w') as f:
                writer = csv.writer(f)
                writer.writerows(self.ORIGINAL_DATA)

            with mock.patch('bin.gen_calendar_entries.CALENDAR_CSV', pathlib.Path(c)):
                yield

    def test_basic(self) -> None:
        args = argparse.Namespace()
        args.manager = "Dylan Baker"
        gen_calendar_entries.release_candidate(args)

        expected = self.ORIGINAL_DATA.copy()
        expected.append(('21.0', '2021-01-13', f'21.0.0-rc1', 'Dylan Baker'))
        expected.append((    '', '2021-01-20', f'21.0.0-rc2', 'Dylan Baker'))
        expected.append((    '', '2021-01-27', f'21.0.0-rc3', 'Dylan Baker'))
        expected.append((    '', '2021-02-03', f'21.0.0-rc4', 'Dylan Baker', 'Or 21.0.0 final.'))

        actual = gen_calendar_entries.read_calendar()

        assert actual == expected
