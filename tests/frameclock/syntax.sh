#!/bin/sh
# Syntax check of the lorie sources the frame clock touches, for Android, with the CI's error flags,
# against a COPY of the xserver tree with patches/xserver.patch applied (the submodule itself is never
# patched) and the headers CMake would generate. Not the build: no linking.
#
# Needs a clang that targets Android with its sysroot - Termux's own does. Skips if there is none.
# WARN=1 also prints the warnings the CI's -W flags give for frameclock.c (they are not errors there).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$(cd "$HERE/../../app/src/main/cpp" && pwd)
OUT=${OUT:-${TMPDIR:-/tmp}/lorie-frameclock-syntax}
CLANG=${CLANG:-/data/data/com.termux/files/usr/bin/clang}
SYSINC=$(dirname "$(dirname "$CLANG")")/include
if [ ! -x "$CLANG" ] || [ ! -f "$SYSINC/android/choreographer.h" ]; then
    echo "syntax: skipped (no Android-targeting clang at $CLANG)"
    exit 0
fi
if [ -L "$CPP/xserver" ] || [ ! -f "$CPP/xserver/dix/dixutils.c" ]; then
    echo "syntax: app/src/main/cpp/xserver must be a real submodule checkout (git submodule update --init)"
    exit 1
fi

rm -rf "$OUT" && mkdir -p "$OUT/gen/X11" "$OUT/gen/xserver/GL" "$OUT/xs"
cp "$CPP/patches/dix-config.h.in" "$OUT/gen/dix-config.h"
printf '#pragma once\n#define VENDOR_NAME "The X.Org Foundation"\n#define VENDOR_RELEASE 12101099\n' > "$OUT/gen/version-config.h"
printf '#pragma once\n#define XKB_BASE_DIRECTORY "/usr/share/X11/xkb/"\n#define XKB_BIN_DIRECTORY ""\n#define XKB_DFLT_LAYOUT "us"\n#define XKB_DFLT_MODEL "pc105"\n#define XKB_DFLT_OPTIONS ""\n#define XKB_DFLT_RULES "evdev"\n#define XKB_DFLT_VARIANT ""\n#define XKM_OUTPUT_DIR (getenv("TMPDIR") ?: "/tmp")\n' > "$OUT/gen/xkb-config.h"
sed 's/@USE_FDS_BITS@/__fds_bits/' "$CPP/xorgproto/include/X11/Xpoll.h.in" > "$OUT/gen/X11/Xpoll.h"
echo '#include <GL/gl.h>' > "$OUT/gen/xserver/GL/glext.h"
sed -e 's/@PIXMAN_VERSION_MAJOR@/0/g' -e 's/@PIXMAN_VERSION_MINOR@/43/g' -e 's/@PIXMAN_VERSION_MICRO@/4/g' \
    "$CPP/pixman/pixman/pixman-version.h.in" > "$OUT/gen/pixman-version.h"
cp -r "$CPP/xserver" "$OUT/xs/"
(cd "$OUT/xs/xserver" && patch -p1 -N -s < "$CPP/patches/xserver.patch")

X="$OUT/xs/xserver"
INC="-I$OUT/gen -I$OUT/gen/xserver -I$CPP/libxfont/include -I$CPP/pixman/pixman -I$CPP/xorgproto/include
     -I$CPP/libxkbfile/include -I$X/Xext -I$X/Xi -I$X/composite -I$X/damageext -I$X/fb -I$X/mi
     -I$X/miext/damage -I$X/miext/shadow -I$X/miext/sync -I$X/dbe -I$X/dri3 -I$X/include -I$X/present
     -I$X/randr -I$X/render -I$X/xfixes -I$X/glx -I$X/exa -I$CPP/libxcvt/include -I$CPP/lorie
     -I$CPP/lorie/shm -I$SYSINC/libdrm"
ERRS="-Werror=implicit -Werror=implicit-function-declaration -Werror=incompatible-pointer-types
      -Werror=int-conversion -Werror=nonnull -Werror=return-type -Werror=int-to-pointer-cast
      -Werror=pointer-to-int-cast -Werror=missing-braces -Werror=array-bounds -Werror=address"
DEFS="-DHAVE_DIX_CONFIG_H -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XSERVER64=1 -DEGL_NO_PLATFORM_SPECIFIC_TYPES"
status=0
cd "$CPP"
for target in aarch64-linux-android26 armv7a-linux-androideabi26 i686-linux-android26; do
    for f in lorie/frameclock.c lorie/flowstats.c lorie/inputflow.c lorie/InitInput.c lorie/InitOutput.c lorie/cmdentrypoint.c lorie/activity.c lorie/renderer.c lorie/buffer.c; do
        out=$("$CLANG" --target=$target -fsyntax-only -std=gnu11 -Wno-everything $ERRS $DEFS \
              -include lorie/shm/shm.h $INC "$f" 2>&1 | grep -E "error" || true)
        # The frame clock's own 64 bit atomics must stay lock-free on every ABI (shared across processes).
        # Clang only diagnoses this while generating code, hence -emit-llvm instead of -fsyntax-only.
        atomics=$("$CLANG" --target=$target -S -emit-llvm -o /dev/null -std=gnu11 -Wno-everything -Watomic-alignment \
              $DEFS -include lorie/shm/shm.h $INC "$f" 2>&1 | grep -A2 -E "warning: misaligned atomic" | grep -E "^ *[0-9]+ \|" | grep -v "gpuCopyQueue" || true)
        if [ -n "$atomics" ]; then
            echo "== $target $f: misaligned atomics"; echo "$atomics" | head -10; status=1
        fi
        if [ -n "$out" ]; then
            echo "== $target $f"; echo "$out" | head -20; status=1
        fi
    done
done
if [ "${WARN:-0}" = 1 ]; then
    "$CLANG" --target=aarch64-linux-android26 -fsyntax-only -std=gnu11 -Wall -Wpointer-arith -Wformat=2 \
        -Wstrict-prototypes -Wbad-function-cast -Wold-style-definition -Wunused -Wmissing-format-attribute \
        -Wredundant-decls -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-missing-prototypes \
        $DEFS -include lorie/shm/shm.h $INC lorie/frameclock.c lorie/flowstats.c lorie/inputflow.c 2>&1 | grep -E "warning|error" || true
fi
[ $status = 0 ] && echo "syntax (Android arm64 + armv7 + x86, CI error flags, aligned atomics): PASS"
exit $status
