#!/bin/sh
# P11-C3: one device session for TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 - three phases marked in the log (the screen
# left alone 10 s, glmark2 alone 10 s, glmark2 while XFCE windows are moved 20 s), then the X server's LorieNative
# lines from the session's start saved to one file, the marks with them. Run it from Termux with the X server already
# started with TERMUX_X11_RENDERER_VK_FRAME_DIAG=1; starting glmark2 and moving the windows are yours, when it asks.
#
# usage: frame-diag-session.sh [OUT]   (default: ~/frame-diag-<date>-<time>.log)
set -e
usage() { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; }
case "${1:-}" in -h|--help) usage; exit 0 ;; esac
OUT=${1:-$HOME/frame-diag-$(date +%Y%m%d-%H%M%S).log}
command -v logcat >/dev/null 2>&1 || { echo "frame-diag-session: no logcat here" >&2; exit 2; }
START=$(date '+%m-%d %H:%M:%S.000')
MARKS="$OUT.marks"
: > "$MARKS"

# a mark in the log, beside the X server's lines (Android's log tool, when there is one), and in the file either way
mark() {
    log -t LorieNative "XlorieMark: $1" 2>/dev/null || true
    echo "XlorieMark: $1 at $(date '+%m-%d %H:%M:%S.%3N')" >> "$MARKS"
}

echo "1/3 idle: leave the screen alone for 10 s"
mark "phase=idle start"
sleep 10
mark "phase=idle end"
printf '2/3 start glmark2 in the X session; press Enter once it draws (10 s of it alone follow) '
read -r _
mark "phase=glmark2 start"
sleep 10
mark "phase=glmark2 end"
printf '3/3 keep glmark2 running and keep moving XFCE windows; press Enter as you start moving (20 s follow) '
read -r _
mark "phase=glmark2+move start"
sleep 20
mark "phase=glmark2+move end"
echo "waiting for the X server's next 5-second report"
sleep 6

logcat -d -v threadtime -T "$START" -s LorieNative:I > "$OUT" 2>/dev/null || true
cat "$MARKS" >> "$OUT"
rm -f "$MARKS"
echo "saved: $OUT ($(grep -c 'XlorieVkFramePacing:' "$OUT" || true) pacing reports)"
grep -q 'XlorieVkFramePacing:' "$OUT" ||
    echo "frame-diag-session: no XlorieVkFramePacing line - was the X server started with TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 (and this build)?" >&2
