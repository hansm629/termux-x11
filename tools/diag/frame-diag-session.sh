#!/bin/sh
# P11-C3: one device session for the frame diagnostics - three phases marked in the log (the screen left alone 10 s,
# glmark2 alone 20 s, glmark2 while XFCE windows are moved 60 s), then the X server's LorieNative lines from the
# session's start (--all: all the log still holds) saved to one file, the marks with them, and a summary: the freezes
# the watchdog logged (TERMUX_X11_FRAME_FREEZE_DIAG=1, P11-C3-C) - nothing to do when one happens, it is logged as it
# begins and ends - the frame gaps by cause (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1), the X server's threads' last line;
# and, P11-C3-D, each phase from the 5-second reports made wholly within it: the screen's frames and those with new
# client content, the content's path and wait (TERMUX_X11_CONTENT_FRAME_DIAG=1), the X server's waits and composites,
# what TERMUX_X11_EXA_COMPOSITE_OPT and TERMUX_X11_ROOT_COPY_SCHED_OPT did.
# Run it from Termux with the X server already started with those options; glmark2 and the windows are yours.
#
# usage: frame-diag-session.sh [--move SECONDS] [--all] [OUT]   (default: 60 s, ~/frame-diag-<date>-<time>.log)
set -e
usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; }
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

# the summary: the lines as the X server wrote them, their keys picked out - nothing judged here. The file is read
# twice: the phases' marks first (as the log has them, on its clock; else as this script wrote them), then the lines.
awk '
    function val(line, key,    i, s) {
        i = index(line, " " key "=")
        if (!i) return ""
        s = substr(line, i + length(key) + 2)
        sub(/ .*/, "", s)
        return s
    }
    function plus(line, key,    s) { s = val(line, key); if (s ~ /\(\+/) { sub(/.*\(\+/, "", s); sub(/\).*/, "", s); return s + 0 } return 0 }
    # "MM-DD HH:MM:SS.mmm" as seconds (months of 31 days: only differences within a session are taken)
    function secs(d, t,    a, b) { split(d, a, "-"); split(t, b, ":"); return (a[1] * 31 + a[2]) * 86400 + b[1] * 3600 + b[2] * 60 + b[3] }
    function rate(n) { return sprintf("%.1f", n / (reports[p] * 5)) }
    function ms(n) { return sprintf("%.1f", n / 1000 / (reports[p] * 5)) }
    NR == FNR {
        if ((i = index($0, "XlorieMark: phase=")) == 0) next
        m = substr($0, i + 18)
        split(m, w, " ")
        if ($0 ~ /LorieNative: XlorieMark: /) logged[w[1], w[2]] = secs($1, $2)
        else if ((j = index(m, " at ")) > 0) { split(substr(m, j + 4), at, " "); written[w[1], w[2]] = secs(at[1], at[2]) }
        if (!(w[1] in seen)) { seen[w[1]] = 1; order[++phases] = w[1] }
        next
    }
    FNR == 1 {
        for (k = 1; k <= phases; k++) {
            q = order[k]
            if ((q, "start") in logged && (q, "end") in logged) { from[q] = logged[q, "start"]; to[q] = logged[q, "end"] }
            else if ((q, "start") in written && (q, "end") in written) { from[q] = written[q, "start"]; to[q] = written[q, "end"] }
        }
        # in the order they began (one not both started and ended: last)
        for (k = 2; k <= phases; k++)
            for (j = k; j > 1 && (order[j] in from) && (!(order[j - 1] in from) || from[order[j]] < from[order[j - 1]]); j--) {
                q = order[j]; order[j] = order[j - 1]; order[j - 1] = q
            }
    }
    /XlorieFreeze: phase=armed/ { armed = 1 }
    /XlorieBackend: / && / freeze_diag=on/ { armed = 1 }
    /XlorieFreeze: phase=begin/ { begins++; b[val($0, "id")] = $0 }
    /XlorieFreeze: phase=end/ { ends++; e[ends] = $0 }
    /XlorieFrameGaps: / { gaps = 1; unw += plus($0, "gaps_unwoken"); late += plus($0, "gaps_late"); xs += plus($0, "gaps_x_stalled")
                          idle += plus($0, "gaps_idle"); s = val($0, "stall_ge_ms"); n = split(s, ge, ",")
                          for (i = 1; i <= n; i++) { split(ge[i], kv, ":"); v = kv[2]; sub(/.*\(\+/, "", v); sub(/\).*/, "", v); st[i] += v + 0 }
                          if (val($0, "worst_stall_us") + 0 > worst + 0) { worst = val($0, "worst_stall_us"); wc = val($0, "worst_stall_cause") } }
    /XlorieXSched: / { xsched = $0; armed = 1 }
    # P11-C3-D: a report line in the phase whose start and end hold its whole 5 seconds
    / LorieNative: Xlorie/ {
        t = secs($1, $2); p = ""
        for (k = 1; k <= phases; k++) {
            q = order[k]
            if ((q in from) && t - 5 >= from[q] - 0.25 && t <= to[q] + 0.25) p = q
        }
        if (p == "") next
        if (/XlorieBackend: /) { reports[p]++; gpuw[p] += plus($0, "x_gpu_wait_us"); lockw[p] += plus($0, "x_state_lock_wait_us") }
        if (/XlorieVk: /) frames[p] += plus($0, "frames_presented")
        if (/XlorieRootCopy: /) { rootreq[p] += plus($0, "root_gpu_copy_requests"); rootw[p] += plus($0, "root_copy_cpu_block_us") }
        if (/XlorieContentFrame: /) {
            content[p] = 1; creq[p] += plus($0, "requests"); cskip[p] += plus($0, "skipped"); cgpu[p] += plus($0, "gpu_copies")
            ccpu[p] += plus($0, "cpu_copies"); cdone[p] += plus($0, "copies_done"); cstale[p] += plus($0, "stale_reads")
            cpending[p] += plus($0, "pending_reads"); cunknown[p] += plus($0, "unknown_reads")
            ccomp[p] += plus($0, "compositor_updates"); croot[p] += plus($0, "root_updates"); cpub[p] += plus($0, "root_publishes")
            cnew[p] += plus($0, "screen_new"); creused[p] += plus($0, "screen_reused"); cunk[p] += plus($0, "screen_unknown")
        }
        if (/XlorieContentLatency: /) {
            # count/mean/p95/p99/max/over 16.7 ms/over 33.3 ms (a percentile past the last bucket: ">100000")
            split(val($0, "content_total_us"), h, "/")
            if (h[4] ~ /^>/) { tover[p] = 1; h[4] = substr(h[4], 2) }
            if (h[1] + 0 > 0) { tn[p] += h[1]; tsum[p] += h[1] * h[2]; if (h[4] + 0 > tp99[p] + 0) tp99[p] = h[4]; if (h[5] + 0 > tmax[p] + 0) tmax[p] = h[5] }
        }
        if (/XlorieContentStall: /) {
            gpend[p] += plus($0, "gaps_pending"); gcli[p] += plus($0, "gaps_client")
            if (val($0, "max_gap_us") + 0 > gmax[p] + 0) gmax[p] = val($0, "max_gap_us")
        }
        if (/XlorieComposite: /) {
            comp[p] = 1; cn[p] += plus($0, "composites"); cus[p] += val($0, "us"); ccpuus[p] += val($0, "cpu_us")
            cgpuus[p] += val($0, "gpu_us"); crootus[p] += val($0, "root_us")
        }
        if (/XlorieCopyDamage: / && / exa_composite_opt=on/) {
            dmg[p] = 1; dd[p] += plus($0, "deferred"); de[p] += plus($0, "early"); dr[p] += plus($0, "dropped")
        }
        if (/XlorieRootCopySched: / && / root_copy_sched_opt=on/) {
            rs[p] = 1; ra[p] += plus($0, "settled_acquired"); rl[p] += plus($0, "settled_lock_wait"); rx[p] += plus($0, "settled_x_waiting")
        }
        if (/XlorieXSched: /) { s = val($0, "main"); sub(/.*runq_us=/, "", s); runq[p] += s; sched[p] = 1 }
    }
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
            print "freezes: the watchdog not on - no XlorieFreeze armed line, XlorieBackend freeze_diag=on or XlorieXSched line: the X server was not started with TERMUX_X11_FRAME_FREEZE_DIAG=1 (or this build)"
        if (gaps)
            printf "frame gaps (100 ms or more): unwoken %d, late %d, x_stalled %d, idle %d; stalls >=100/250/500 ms %d/%d/%d, >=1/2/3 s %d/%d/%d; worst %s us (%s)\n",
                   unw, late, xs, idle, st[1], st[2], st[3], st[4], st[5], st[6], worst + 0, wc ? wc : "none"
        if (xsched) { sub(/.*XlorieXSched: /, "", xsched); print "X server threads (last 5 s): " xsched }
        for (k = 1; k <= phases; k++) {
            p = order[k]
            if (!reports[p]) { printf "phase %s: no 5-second report wholly within it\n", p; continue }
            printf "phase %s: %d reports (%d s)\n", p, reports[p], reports[p] * 5
            if (content[p])
                printf "  screen frames %s/s, with new client content %s/s, with the last one again %s/s, not known %s/s\n", rate(frames[p]), rate(cnew[p]), rate(creused[p]), rate(cunk[p])
            else
                printf "  screen frames %s/s\n", rate(frames[p])
            if (content[p]) {
                printf "  content: asked %s/s (skipped %s/s), copied by the GPU %s/s (completed %s/s), by the CPU %s/s; reads of a copy still queued: stale %s/s, pending %s/s, unknown %s/s; into the compositor %s/s, the screen pixmap %s/s, published %s/s\n",
                       rate(creq[p]), rate(cskip[p]), rate(cgpu[p]), rate(cdone[p]), rate(ccpu[p]), rate(cstale[p]), rate(cpending[p]),
                       rate(cunknown[p]), rate(ccomp[p]), rate(croot[p]), rate(cpub[p])
                printf "  content to the screen: mean %d us, p99 up to %s%d us, max %d us; gaps of 50 ms or more: pending %d, client %d, longest %d us\n",
                       tn[p] ? tsum[p] / tn[p] : 0, tover[p] ? ">" : "", tp99[p] + 0, tmax[p] + 0, gpend[p], gcli[p], gmax[p] + 0
            }
            printf "  X server per second: GPU wait %s ms, lock and GPU wait %s ms, root copy wait %s ms", ms(gpuw[p]), ms(lockw[p]), ms(rootw[p])
            if (sched[p]) printf ", main thread runnable %s ms", ms(runq[p])
            if (comp[p]) printf "; composites %s/s, %s ms (own %s, GPU wait %s, root copy wait %s)", rate(cn[p]), ms(cus[p]), ms(ccpuus[p]), ms(cgpuus[p]), ms(crootus[p])
            printf "\n  root copies %s/s", rate(rootreq[p])
            if (dmg[p]) printf "; copy damage put off %s/s (early %s/s, dropped %s/s)", rate(dd[p]), rate(de[p]), rate(dr[p])
            if (rs[p]) printf "; root copies settled early: once acquired %s/s, waiting for the lock %s/s, the X server waiting %s/s", rate(ra[p]), rate(rl[p]), rate(rx[p])
            printf "\n"
        }
    }' "$OUT" "$OUT"
