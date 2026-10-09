#!/bin/sh
# Frame clock tests: the host test of frameclock.c's logic (tframeclock.c, test doubles for the Android
# APIs) and the Android syntax check of everything the frame clock touches (syntax.sh).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/lorie-frameclock-test}
CC=${CC:-cc}
mkdir -p "$OUT"
"$CC" -std=gnu11 -g -O1 -Wall -Wno-unused-function -Werror=implicit-function-declaration \
    -I"$HERE/fake" -I"$HERE/../../app/src/main/cpp/lorie" "$HERE/tframeclock.c" -o "$OUT/tframeclock"
"$OUT/tframeclock"
OUT="$OUT/syntax" sh "$HERE/syntax.sh"
