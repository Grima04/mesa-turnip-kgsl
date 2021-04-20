#!/usr/bin/env bash
##########################################################################
#
# Copyright 2011 Jose Fonseca
# All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
##########################################################################/

set -e


fatal()
{
	echo "ERROR: $1"
	exit 1
}


do_cleanup()
{
	if test -d "$FIFODIR"; then
		rm -rf "$FIFODIR"
	fi
}


strip_dump()
{
	INFILE="$1"
	shift
	OUTFILE="$1"
	shift

	python3 "$TRACEDUMP" --plain --suppress "$@" "$INFILE" \
	| sed \
		-e '/pipe_screen::is_format_supported/d' \
		-e '/pipe_screen::get_\(shader_\)\?paramf\?/d' \
		-e 's/\r$//g' \
		-e 's/pipe = \w\+/pipe/g' \
		-e 's/screen = \w\+/screen/g' \
		-e 's/, /,\n\t/g' \
		-e 's/) = /)\n\t= /' \
	> "$OUTFILE"
}


### Main code starts
trap do_cleanup HUP INT TERM

TRACEDUMP="${TRACEDUMP:-$(dirname "$0")/dump.py}"

if test $# -lt 2; then
	echo "Usage: $0 <tracefile1> <tracefile2> [extra dump.py args]"
	exit 0
fi

FIFODIR="$(mktemp -d)"
FIFO1="${FIFODIR}/1"
FIFO2="${FIFODIR}/2"

mkfifo "$FIFO1" || fatal "Could not create fifo 1"
mkfifo "$FIFO2" || fatal "Could not create fifo 2"

INFILE1="$1"
shift
INFILE2="$1"
shift

strip_dump "$INFILE1" "$FIFO1" "$@" &
strip_dump "$INFILE2" "$FIFO2" "$@" &

sdiff \
	--left-column \
	--width="$(tput cols)" \
	--speed-large-files \
	"$FIFO1" "$FIFO2" \
| less
