#!/bin/sh

# Only use GNU time if available, not any shell built-in command
if test -f /usr/bin/time; then
    exec /usr/bin/time -v "$@"
fi

exec "$@"
