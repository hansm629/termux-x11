#!/data/data/com.termux/files/usr/bin/bash
# Non-root S24+/S26U OneUI scheduling diagnostic. Nothing here changes policy.
set -u

report() {
    local label="$1" pid="$2" f
    [ -n "$pid" ] && [ -r "/proc/$pid/cgroup" ] || return 0
    printf '\n%-21s PID=%s (%s)\n' "$label" "$pid" "$(cat "/proc/$pid/comm" 2>/dev/null)"
    grep -E '^([0-9]+:cpuset:|[0-9]+:cpu:)' "/proc/$pid/cgroup" 2>/dev/null || true
    grep -E '^Cpus_allowed_list:' "/proc/$pid/status" 2>/dev/null || true
    grep -E '^0::/apps/' "/proc/$pid/cgroup" 2>/dev/null || true
}

echo '=== Android / Device ==='
printf 'model=%s android=%s oneui=%s\n' "$(getprop ro.product.model)" \
    "$(getprop ro.build.version.release)" "$(getprop ro.build.version.oneui)"
echo '=== Shared UID check ==='
cmd package list packages -U 2>/dev/null | grep -E 'package:com\.termux(\.x11)? uid:' || true

termux_pid="$(pidof com.termux 2>/dev/null | awk '{print $1}')"
x11_pid="$(pidof com.termux.x11 2>/dev/null | awk '{print $1}')"
report 'Termux Android' "$termux_pid"
report 'X11 Android separate' "$x11_pid"
report 'Calling Termux shell' "$$"

for program in xfce4-session xfwm4 ffmpeg melt; do
    # Match process name, not the grep command line itself.
    pid="$(pgrep -x "$program" 2>/dev/null | head -n 1)"
    report "$program" "$pid"
done
for pid in "$@"; do
    case "$pid" in *[!0-9]*|'') echo "Ignored nonnumeric PID: $pid" ;; *)
        report 'Additional workload' "$pid" ;;
    esac
done

echo
echo '=== Experimental APK runtime proof (logcat) ==='
logcat -d -s OneUiProcessHost:I 2>/dev/null | tail -n 4 || true
echo
if [ -n "$x11_pid" ]; then
    echo 'NOTE: a separate com.termux.x11 Android process exists. The cohost experiment may be off.'
elif [ -n "$termux_pid" ]; then
    echo 'NOTE: no separate X11 Android process. Confirm the cohost=true runtime log and that X11 is on-screen.'
fi
echo 'If X11 is top-app: expected good result is Termux, XFCE, and Termux shell all in /top-app, with CPU6-7 allowed.'
