#!/bin/sh
# Prints the X server's latest renderer diagnostics: the XlorieBackend, XlorieGles, XlorieVk, XlorieVkTiming,
# XlorieVkSelftest, XlorieFallback, XlorieRootCopy, XlorieSchedule, XlorieRootCopySched and XlorieCopyDamage lines it
# logs every 5 seconds (and, with TERMUX_X11_RENDERER_VK_FRAME_DIAG=1, XlorieVkFramePacing, XlorieFrameGaps,
# XlorieVkPipeline, XlorieRootCopyTiming, XlorieXLatency, XlorieComposite and XlorieCompositeClasses; with
# TERMUX_X11_CONTENT_FRAME_DIAG=1, XlorieContentFrame, XlorieContentLatency, XlorieContentStall and XlorieContentClients;
# with TERMUX_X11_FRAME_FREEZE_DIAG=1, XlorieXSched and the last XlorieFreeze line), and the renderer's GLES driver line.
# Nothing is computed or assumed here - these are the lines as the X server wrote them.
#
# usage: renderer-status.sh [--file LOG]
#   --file LOG   read a saved log instead of `logcat -d` (the X server's log is readable from Termux)
set -e
usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; }
FILE=
while [ $# -gt 0 ]; do
    case "$1" in
        --file) FILE=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "renderer-status: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
if [ -n "$FILE" ]; then
    [ -r "$FILE" ] || { echo "renderer-status: cannot read $FILE" >&2; exit 2; }
    SRC() { cat "$FILE"; }
else
    command -v logcat >/dev/null 2>&1 || { echo "renderer-status: no logcat here; pass --file" >&2; exit 2; }
    SRC() { logcat -d -s LorieNative:I 2>/dev/null; }
fi
SRC | awk '
    /XlorieFrames: renderer GLES driver:/ { drv = $0 }
    /XlorieBackend: / { b = $0 } /XlorieGles: / { g = $0 } /XlorieVk: / { v = $0 }
    /XlorieVkTiming: / { t = $0 } /XlorieVkSelftest: / { s = $0 } /XlorieFallback: / { f = $0 } /XlorieRootCopy: / { rc = $0 }
    /XlorieSchedule: / { sc = $0 } /XlorieVkFramePacing: / { fp = $0 } /XlorieVkPipeline: / { vp = $0 } /XlorieRootCopyTiming: / { rt = $0 } /XlorieXLatency: / { xl = $0 }
    /XlorieFrameGaps: / { fg = $0 } /XlorieXSched: / { xs = $0 } /XlorieFreeze: / { fz = $0 }
    /XlorieRootCopySched: / { rs = $0 } /XlorieCopyDamage: / { cd = $0 } /XlorieComposite: / { co = $0 } /XlorieCompositeClasses: / { cc = $0 }
    /XlorieContentFrame: / { kf = $0 } /XlorieContentLatency: / { kl = $0 } /XlorieContentStall: / { ks = $0 } /XlorieContentClients: / { kc = $0 }
    END {
        n = 0
        if (drv) { print drv; n++ }
        if (b) { print b; n++ } if (g) { print g; n++ } if (v) { print v; n++ }
        if (t) { print t; n++ } if (s) { print s; n++ } if (f) { print f; n++ } if (rc) { print rc; n++ }
        if (sc) { print sc; n++ } if (rs) { print rs; n++ } if (cd) { print cd; n++ }
        if (fp) { print fp; n++ } if (fg) { print fg; n++ } if (vp) { print vp; n++ } if (rt) { print rt; n++ }
        if (xl) { print xl; n++ } if (co) { print co; n++ } if (cc) { print cc; n++ }
        if (kf) { print kf; n++ } if (kl) { print kl; n++ } if (ks) { print ks; n++ } if (kc) { print kc; n++ }
        if (xs) { print xs; n++ } if (fz) { print fz; n++ }
        if (!b) { print "renderer-status: no XlorieBackend line found (X server not running this build, or no renderer connected yet)"; exit 1 }
    }'
