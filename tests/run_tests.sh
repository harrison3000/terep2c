#!/bin/sh
# Replays every recording made with the Win16 port (tests/*/record.txt) and
# compares its frames (terep2-headless -C).  The map is the directory of the
# "#selected path:" line of the recording, under data/.
#
#   sh tests/run_tests.sh [path/to/terep2-headless]
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./terep2-headless}
fail=0

for rec in tests/*/record.txt; do
    dir=$(dirname "$rec")
    map=$(sed -n 's/^#selected path: *//p' "$rec" | head -1 | tr '\\' / | tr -d '\r')
    map=data/$(basename "$(dirname "$map")")
    if [ ! -d "$map" ]; then
        echo "$dir: map $map not found"
        fail=1
        continue
    fi
    if summary=$("$BIN" -C "$dir" "$map" | tail -1); then
        echo "$dir ($map): ok, $summary"
    else
        echo "$dir ($map): DIFERENTE, $summary"
        fail=1
    fi
done
exit $fail
