#!/bin/sh
# Host regression tests for logic in app/src/main/cpp/lorie. Each test is built from functions cut out of
# the current sources (gen.py), not from copies, so a change to those functions is what gets tested.
# Not part of the Android build. Needs a host C compiler and python3.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LORIE=$(cd "$HERE/../../app/src/main/cpp/lorie" && pwd)
OUT=${TMPDIR:-/tmp}/lorie-host-tests
mkdir -p "$OUT"
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/gen.py" "$LORIE" "$OUT"
CC=${CC:-cc}
status=0
for t in t07 t19 t21 tstat; do
    "$CC" -Wall -Wno-unused-function -O2 -pthread -I"$OUT" "$HERE/$t.c" -o "$OUT/$t"
    "$OUT/$t" || status=1
done
exit $status
