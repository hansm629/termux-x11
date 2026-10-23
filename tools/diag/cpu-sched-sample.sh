#!/data/data/com.termux/files/usr/bin/sh
# P11-C3-E: rootless, low-frequency Linux scheduler snapshots for the Termux-owned X server, xfwm4, and glmark2.
# Compare the deltas (run_ns and runqueue_wait_ns) between glmark2 alone and window movement.  The X11 app's
# Android-side renderer can be a *different UID*: its /proc tasks may be unreadable, so the XlorieXSched log remains
# the source for the renderer and watchdog.  This script never changes scheduling, priorities or CPU affinity.
#
# Usage: sh cpu-sched-sample.sh [seconds=120] [output=$HOME/tx11-sched-YYYYMMDD-HHMMSS.log]
# Start before frame-diag-session.sh, in another Termux shell; time stamps let the logs be aligned afterwards.
set -eu

DURATION=${1:-120}
case "$DURATION" in ''|*[!0-9]*) echo "duration must be an integer number of seconds" >&2; exit 2;; esac
[ "$DURATION" -ge 1 ] && [ "$DURATION" -le 600 ] || { echo "duration must be 1..600" >&2; exit 2; }
OUT=${2:-"$HOME/tx11-sched-$(date +%Y%m%d-%H%M%S).log"}
case "$OUT" in */*) :;; *) OUT="./$OUT";; esac

printf '# tx11 scheduler snapshots; epoch_ms pid tid comm run_ns runqueue_wait_ns timeslices\n' > "$OUT"
printf '# uptime and cgroup are not synchronized to Android logcat clocks; align by epoch_ms\n' >> "$OUT"
printf '# unreadable /proc tasks are skipped (Android per-UID privacy)\n' >> "$OUT"

end=$(( $(date +%s) + DURATION ))
seen_pids=' '
while :; do
    now=$(date +%s)
    stamp=$(date +%s%3N)
    [ "$now" -lt "$end" ] || break

    # /proc scanning at only 1 Hz, no ptrace, tracing, or shell command per thread.
    for process in /proc/[0-9]*; do
        [ -r "$process/comm" ] || continue
        IFS= read -r comm < "$process/comm" || continue
        case "$comm" in
            xfwm4|glmark2|glmark2-es2|glmark2-gl|termux-x11|Xorg|Xwayland) ;;
            *) continue ;;
        esac
        pid=${process##*/}
        case "$seen_pids" in
            *" $pid "*) ;;
            *)
                seen_pids="$seen_pids$pid "
                # Process-wide scheduler group and permitted cores, reported once per discovered PID.
                # Android may restrict the information; failure is diagnostic, never fatal.
                group=$(tr '\n' ';' < "$process/cgroup" 2>/dev/null || true)
                allowed=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' "$process/status" 2>/dev/null || true)
                printf '# pid=%s comm=%s cpu_allowed=%s cgroups=%s\n' "$pid" "$comm" "${allowed:-unknown}" "${group:-unreadable}" >> "$OUT"
                ;;
        esac
        for task in "$process"/task/[0-9]*; do
            [ -r "$task/schedstat" ] || continue
            IFS=' ' read -r run_ns runq_ns timeslices < "$task/schedstat" || continue
            case "$run_ns:$runq_ns:$timeslices" in *[!0-9:]*|::*|*::*|*:) continue;; esac
            printf '%s %s %s %s %s %s %s\n' "$stamp" "$pid" "${task##*/}" "$comm" "$run_ns" "$runq_ns" "$timeslices" >> "$OUT"
        done
    done
    sleep 1
done

printf 'saved: %s\n' "$OUT"
