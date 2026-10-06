#!/bin/sh
# Host regression tests for app/src/main/cpp/lorie. Each C test is built from functions cut out of the
# current sources by gen.py (extract.py), not from copies, so a change to those functions is what gets
# tested. Not part of the Android build. Needs a host C compiler and python3.
#
#   tests/host/run.sh        all host tests
#   tests/host/syntax.sh     separately: the lorie sources compiled for Android with the CI's error flags
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LORIE=$(cd "$HERE/../../app/src/main/cpp/lorie" && pwd)
OUT=${TMPDIR:-/tmp}/lorie-host-tests
BASE=$OUT/base-3c98ffa
mkdir -p "$OUT" "$BASE"
# 3c98ffa's InitOutput.c, which tests comparing with 3c98ffa's behaviour run beside the current code
git -C "$HERE" show 3c98ffa1714bb7dfe7b9188e4ccfaa982b65177b:app/src/main/cpp/lorie/InitOutput.c > "$BASE/InitOutput.c"
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/gen.py" "$LORIE" "$OUT" "$BASE"
CC=${CC:-cc}
status=0

PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/textract.py" "$HERE/extract.py" || status=1

# C tests built from generated sources: tests/host/<name>.c, compiled with -I"$OUT".
C_TESTS="tdiaglog townership"
rm -f "$OUT/diag-vulkan.log" "$OUT/diag-gles.log"     # written by tdiaglog: never judge an earlier run's
for t in $C_TESTS; do
    "$CC" -Wall -Wno-unused-function -O1 -pthread -I"$OUT" -I"$HERE" "$HERE/$t.c" -o "$OUT/$t" -lm || { status=1; continue; }
    OUT="$OUT" "$OUT/$t" || status=1
done

# the diagnostics scripts, on synthetic logs and on the ones tdiaglog wrote with the real formatter
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tdiag.py" "$HERE/../../tools/diag" "$OUT" || status=1

# where the X server calls the AHB ownership code
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/twiring.py" "$LORIE" || status=1

# each rule broken in a copy of the sources: the test guarding it must fail
CC="$CC" PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tcontrols.py" "$LORIE" "$OUT" "$BASE" || status=1

exit $status
