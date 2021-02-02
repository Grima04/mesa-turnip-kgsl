#!/bin/sh

# Only use GNU time if available, not any shell built-in command
if ! test -f /usr/bin/time; then
    exec "$@"
fi


# If the test times out, meson sends SIGINT & SIGTERM signals to this process.
# Simply exec'ing "time" would result in no output from that in this case.
# Instead, we need to run "time" in the background, catch the signals and
# propagate them to the actual test process.

/usr/bin/time -v "$@" &
TIMEPID=$!
TESTPID=$(ps --ppid $TIMEPID -o pid=)

if test "x$TESTPID" != x; then
    trap 'kill -INT $TESTPID; wait $TIMEPID; exit $?' INT
    trap 'kill -TERM $TESTPID; wait $TIMEPID; exit $?' TERM
fi

wait $TIMEPID
exit $?
