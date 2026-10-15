#!/bin/sh
# Replays every recording made with the Win16 port (tests/*/record.txt) and
# compares its frames (terep2-headless -C), and again at each scale it has
# saved hashes for (hashes_Nx.txt).  The map is the directory of the
# "#selected path:" line of the recording, under data/.
#
#   sh tests/run_tests.sh [path/to/terep2-headless]
cd "$(dirname "$0")/.." || exit 1
BIN=${1:-./terep2-headless}
fail=0

check() {   # recdir map scale
    out=$("$BIN" -s "$3" -C "$1" "$2")
    rc=$?
    if [ $rc = 0 ]; then
        echo "$1 ($2) $3x: ok, $(echo "$out" | tail -1)"
    else
        echo "$1 ($2) $3x: DIFERENTE (exit $rc)"
        echo "$out" | grep -v ': same$\|: similar' | head -5
        fail=1
    fi
}

for rec in tests/*/record.txt; do
    dir=$(dirname "$rec")
    map=$(sed -n 's/^#selected path: *//p' "$rec" | head -1 | tr '\\' / | tr -d '\r')
    map=data/$(basename "$(dirname "$map")")
    if [ ! -d "$map" ]; then
        echo "$dir: map $map not found"
        fail=1
        continue
    fi
    check "$dir" "$map" 1
    # higher scales: frames similar to the recorded ones, hashes as saved by
    # the port itself (terep2-headless -s N -R, which writes hashes_Nx.txt)
    for h in "$dir"/hashes_*x.txt; do
        [ -f "$h" ] || continue
        s=${h##*hashes_}; s=${s%x.txt}
        check "$dir" "$map" "$s"
    done
done
exit $fail
