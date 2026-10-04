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
# T05 needs pixman's region code; built from the tree's own copy with the Android build's settings.
PIXMAN="$HERE/../../app/src/main/cpp/pixman/pixman"
printf '#define PACKAGE "pixman"\n' > "$OUT/pixman-config.h"
sed -e 's/@PIXMAN_VERSION_MAJOR@/0/g' -e 's/@PIXMAN_VERSION_MINOR@/43/g' -e 's/@PIXMAN_VERSION_MICRO@/4/g' \
    "$PIXMAN/pixman-version.h.in" > "$OUT/pixman-version.h"   # the version recipes/pixman.cmake writes
"$CC" -c -O1 -w -DHAVE_CONFIG_H -DTLS=__thread -I"$OUT" -I"$PIXMAN" "$PIXMAN/pixman-region16.c" -o "$OUT/region16.o"
"$CC" -Wall -Wno-unused-function -O1 -DHAVE_GPU_PENDING -DHAVE_OWED -I"$OUT" -I"$PIXMAN" "$HERE/t05.c" "$OUT/region16.o" -o "$OUT/t05"
"$OUT/t05" || status=1
"$CC" -Wall -Wno-unused-function -O1 -DHAVE_GPU_PENDING -DHAVE_OWED -I"$OUT" -I"$PIXMAN" "$HERE/t26.c" "$OUT/region16.o" -o "$OUT/t26"
"$OUT/t26" || status=1
"$CC" -Wall -Wno-unused-function -O1 -DHAVE_GPU_PENDING -DHAVE_OWED -I"$OUT" -I"$PIXMAN" "$HERE/t30.c" "$OUT/region16.o" -o "$OUT/t30"
"$OUT/t30" || status=1
"$CC" -Wall -Wno-unused-function -O1 -DHAVE_GPU_PENDING -DHAVE_OWED -I"$OUT" -I"$PIXMAN" "$HERE/t33.c" "$OUT/region16.o" -o "$OUT/t33"
"$OUT/t33" || status=1
"$CC" -Wall -Wno-unused-function -O1 -I"$OUT" -I"$PIXMAN" "$HERE/t25.c" "$OUT/region16.o" -o "$OUT/t25"
"$OUT/t25" || status=1
"$CC" -Wall -Wno-unused-function -O1 -I"$OUT" -I"$PIXMAN" "$HERE/tcorecopy.c" "$OUT/region16.o" -o "$OUT/tcorecopy"
"$OUT/tcorecopy" || status=1

PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tanalyze.py" "$HERE/../../tools/trace/analyze.py" "$OUT" || status=1
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tpatch.py" "$HERE/../../app/src/main/cpp/xserver" \
    "$HERE/../../app/src/main/cpp/patches/xserver.patch" "$OUT" || status=1

for t in t07 t10 t19 t27 t28 tsession t32 tlogcat tcpucopy tcarryq t21 t29 tstat ttrace; do
    "$CC" -Wall -Wno-unused-function -O2 -pthread -I"$OUT" "$HERE/$t.c" -o "$OUT/$t"
    OUT="$OUT" "$OUT/$t" || status=1
done
exit $status
