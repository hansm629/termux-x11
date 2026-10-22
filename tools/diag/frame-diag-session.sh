#!/bin/sh
# P11-C3: one device session for the frame diagnostics - three phases marked in the log (the screen left alone 10 s,
# glmark2 alone 20 s, glmark2 while XFCE windows are moved 60 s), then the X server's LorieNative lines from the
# session's start (--all: all the log still holds) saved to one file, the marks with them, and a summary: the freezes
# the watchdog logged (TERMUX_X11_FRAME_FREEZE_DIAG=1, P11-C3-C) - nothing to do when one happens, it is logged as it
# begins and ends - the frame gaps by cause (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1), the X server's threads' last line.
# Run it from Termux with the X server already started with those options; glmark2 and the windows are yours.
#
# usage: frame-diag-session.sh [--move SECONDS] [--all] [OUT]   (default: 60 s, ~/frame-diag-<date>-<time>.log)
set -e
usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; }
MOVE=60
ALL=
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --move) MOVE=$2; shift 2 ;;
        --all) ALL=1; shift ;;
        *) break ;;
    esac
done
case "$MOVE" in ''|*[!0-9]*) echo "frame-diag-session: --move takes seconds" >&2; exit 2 ;; esac
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
printf '2/3 start glmark2 in the X session; press Enter once it draws (20 s of it alone follow) '
read -r _
mark "phase=glmark2 start"
sleep 20
mark "phase=glmark2 end"
printf '3/3 keep glmark2 running and keep moving XFCE windows; press Enter as you start moving (%s s follow) ' "$MOVE"
read -r _
mark "phase=glmark2+move start"
sleep "$MOVE"
mark "phase=glmark2+move end"
echo "waiting for the X server's next 5-second report"
sleep 6

if [ -n "$ALL" ]; then
    logcat -d -v threadtime -s LorieNative:I > "$OUT" 2>/dev/null || true
else
    logcat -d -v threadtime -T "$START" -s LorieNative:I > "$OUT" 2>/dev/null || true
fi
cat "$MARKS" >> "$OUT"
rm -f "$MARKS"
echo "saved: $OUT ($(grep -c 'XlorieVkFramePacing:' "$OUT" || true) pacing reports)"
grep -q 'XlorieVkFramePacing:' "$OUT" ||
    echo "frame-diag-session: no XlorieVkFramePacing line - was the X server started with TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 (and this build)?" >&2

# the summary: the lines as the X server wrote them, their keys picked out - nothing judged here
awk '
    function val(line, key,    i, s) {
        i = index(line, " " key "=")
        if (!i) return ""
        s = substr(line, i + length(key) + 2)
        sub(/ .*/, "", s)
        return s
    }
    function plus(line, key,    s) { s = val(line, key); if (s ~ /\(\+/) { sub(/.*\(\+/, "", s); sub(/\).*/, "", s); return s + 0 } return 0 }
    /XlorieFreeze: phase=armed/ { armed = 1 }
    /XlorieFreeze: phase=begin/ { begins++; b[val($0, "id")] = $0 }
    /XlorieFreeze: phase=end/ { ends++; e[ends] = $0 }
    /XlorieFrameGaps: / { gaps = 1; unw += plus($0, "gaps_unwoken"); late += plus($0, "gaps_late"); xs += plus($0, "gaps_x_stalled")
                          idle += plus($0, "gaps_idle"); s = val($0, "stall_ge_ms"); n = split(s, ge, ",")
                          for (i = 1; i <= n; i++) { split(ge[i], kv, ":"); v = kv[2]; sub(/.*\(\+/, "", v); sub(/\).*/, "", v); st[i] += v + 0 }
                          if (val($0, "worst_stall_us") + 0 > worst + 0) { worst = val($0, "worst_stall_us"); wc = val($0, "worst_stall_cause") } }
    /XlorieXSched: / { xsched = $0 }
    END {
        if (armed) {
            printf "freezes (TERMUX_X11_FRAME_FREEZE_DIAG): %d begun, %d ended\n", begins, ends
            for (k = 1; k <= ends; k++) {
                id = val(e[k], "id")
                printf "  freeze %s: %s us, side %s, stalled at %s, last at %s, waiting %s, ended by %s\n", id, val(e[k], "duration_us"),
                       val(e[k], "side"), val(e[k], "stalled_stage"), val(e[k], "last_stage"), val(e[k], "pending_at_begin"), val(e[k], "ended_by")
                if (id in b)
                    printf "    at its begin: x_main=%s x_request=%s asks=%s taken=%s copies_queued=%s root=%s lock_pid=%s\n", val(b[id], "x_main"),
                           val(b[id], "x_request"), val(b[id], "asks"), val(b[id], "taken"), val(b[id], "copies_queued"), val(b[id], "root"),
                           val(b[id], "lock_pid")
            }
            if (begins > ends) print "  (a freeze begun and not ended in the log)"
        } else
            print "freezes: no XlorieFreeze armed line - the X server was not started with TERMUX_X11_FRAME_FREEZE_DIAG=1 (or this build)"
        if (gaps)
            printf "frame gaps (100 ms or more): unwoken %d, late %d, x_stalled %d, idle %d; stalls >=100/250/500 ms %d/%d/%d, >=1/2/3 s %d/%d/%d; worst %s us (%s)\n",
                   unw, late, xs, idle, st[1], st[2], st[3], st[4], st[5], st[6], worst + 0, wc ? wc : "none"
        if (xsched) { sub(/.*XlorieXSched: /, "", xsched); print "X server threads (last 5 s): " xsched }
    }' "$OUT"
