#!/usr/bin/env bash
#
# The headline comparison: ordinary -pthread programs calling plain malloc and
# free, with the default allocator and with tsmalloc injected. No source
# changes, no relinking. Takes the same flags as bench.sh.
exec "$(dirname "$0")/bench.sh" --override "$@"
