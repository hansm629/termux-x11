#!/bin/sh
# Syntax check of the lorie sources for Android, with the CI's error flags (-Werror=implicit and the
# rest), against a copy of the xserver tree with patches/xserver.patch applied and the headers CMake
# would generate. Catches what the host tests cannot, since they only compile the functions they cut
# out. Not the build itself: no linking, and only the files listed.
#
# Needs a clang that targets Android with its sysroot - Termux's own does. Skips if there is none.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CPP=$(cd "$HERE/../../app/src/main/cpp" && pwd)
OUT=${TMPDIR:-/tmp}/lorie-syntax
CLANG=${CLANG:-/data/data/com.termux/files/usr/bin/clang}
SYSINC=$(dirname "$(dirname "$CLANG")")/include
if [ ! -x "$CLANG" ] || [ ! -f "$SYSINC/android/surface_control.h" ]; then
    echo "syntax: skipped (no Android-targeting clang at $CLANG)"
    exit 0
fi

rm -rf "$OUT" && mkdir -p "$OUT/gen/X11" "$OUT/gen/xserver/GL" "$OUT/xs"
cp "$CPP/patches/dix-config.h.in" "$OUT/gen/dix-config.h"      # recipes/xserver.cmake: no substitutions
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
status=0
cd "$CPP"
for f in lorie/InitOutput.c lorie/cmdentrypoint.c lorie/activity.c lorie/renderer.c lorie/buffer.c; do
    out=$("$CLANG" --target=aarch64-linux-android30 -fsyntax-only -std=gnu99 -Wno-everything \
          -Werror=implicit -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
          -Werror=int-conversion -DHAVE_DIX_CONFIG_H -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XSERVER64=1 \
          -DEGL_NO_PLATFORM_SPECIFIC_TYPES -include lorie/shm/shm.h $INC "$f" 2>&1 | grep -E "error" || true)
    if [ -n "$out" ]; then
        echo "== $f"; echo "$out" | head -20; status=1
    fi
done
[ $status = 0 ] && echo "syntax (Android, CI error flags): PASS"
exit $status
