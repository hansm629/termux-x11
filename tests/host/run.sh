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

# Vulkan headers for the tests of vk.c: their own two directories only, so the host compiler keeps its own libc
# headers. Termux's by default (VULKAN_INCLUDE to use others); without them the run fails rather than skips.
VKINC=${VULKAN_INCLUDE:-/data/data/com.termux/files/usr/include}
if [ ! -f "$VKINC/vulkan/vulkan_core.h" ]; then
    echo "FAIL no Vulkan headers in $VKINC (set VULKAN_INCLUDE)"
    exit 1
fi
rm -rf "$OUT/vkinc" && mkdir -p "$OUT/vkinc" && cp -r "$VKINC/vulkan" "$OUT/vkinc/"
[ ! -d "$VKINC/vk_video" ] || cp -r "$VKINC/vk_video" "$OUT/vkinc/"

PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/textract.py" "$HERE/extract.py" || status=1

# C tests built from generated sources: tests/host/<name>.c, compiled with -I"$OUT".
C_TESTS="tdiaglog townership tvkinit tvkimport tvkbarrier tvkselftest tvkframe tvkrenderer tpresentcopy tpresentsafety tdispatchyield tbufferlock twake tsharedlock tfreeze tcontentframe texacomposite txrendergpu"
rm -f "$OUT/diag-vulkan.log" "$OUT/diag-gles.log"     # written by tdiaglog: never judge an earlier run's
for t in $C_TESTS; do
    "$CC" -Wall -Wno-unused-function -O1 -pthread -I"$OUT" -I"$OUT/vkinc" -I"$HERE" "$HERE/$t.c" -o "$OUT/$t" -lm || { status=1; continue; }
    OUT="$OUT" "$OUT/$t" || status=1
done

# the diagnostics scripts, on synthetic logs and on the ones tdiaglog wrote with the real formatter
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tdiag.py" "$HERE/../../tools/diag" "$OUT" || status=1

# where the X server calls the AHB ownership code, and the order of the renderer's Vulkan steps
PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/twiring.py" "$LORIE" || status=1

# the committed SPIR-V is what the shaders compile to now (fails without glslc/spirv-val rather than skips)
if sh "$HERE/../../tools/vk/gen-spirv.sh" "$OUT/vk_shaders.regenerated.h" && cmp -s "$OUT/vk_shaders.regenerated.h" "$LORIE/vk_shaders.h"; then
    echo "SPIR-V (vk_shaders.h from shaders/quad.*): PASS"
else
    echo "SPIR-V (vk_shaders.h from shaders/quad.*): FAIL - run tools/vk/gen-spirv.sh"
    status=1
fi

# each rule broken in a copy of the sources: the test guarding it must fail
CC="$CC" PYTHONDONTWRITEBYTECODE=1 python3 "$HERE/tcontrols.py" "$LORIE" "$OUT" "$BASE" || status=1

exit $status
