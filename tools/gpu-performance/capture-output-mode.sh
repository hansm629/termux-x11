#!/data/data/com.termux/files/usr/bin/bash
# Snapshot the real Termux shell / client scheduling group and X11's GPU helper.
# Run from native Termux DURING a vkmark or glmark2 scene (not from PRoot XFCE).
set -u
tag="${1:-unknown}"
case "$tag" in
    *[!a-zA-Z0-9_-]* | '') echo "usage: $0 onscreen|dex" >&2; exit 2 ;;
esac
out="$HOME/tx11-gpu-${tag}-$(date '+%Y%m%d-%H%M%S').log"
{
    printf 'tag=%s date=%s\n' "$tag" "$(date -Is)"
    printf '\n=== current Termux process cgroups ===\n'
    cat /proc/self/cgroup 2>/dev/null || true
    printf '\n=== live benchmark cgroups ===\n'
    for name in vkmark glmark2; do
        pid="$(pgrep -n -x "$name" 2>/dev/null || true)"
        if [ -n "$pid" ]; then
            printf '%s pid=%s\n' "$name" "$pid"
            cat "/proc/$pid/cgroup" 2>/dev/null || true
        fi
    done
    printf '\n=== system GPU telemetry (read-only, device-dependent units) ===\n'
    for node in /sys/kernel/gpu/gpu_busy /sys/kernel/gpu/gpu_clock \
                /sys/class/kgsl/kgsl-3d0/gpubusy /sys/class/kgsl/kgsl-3d0/gpuclk; do
        if [ -r "$node" ]; then
            printf '%s=' "$node"
            head -c 120 "$node" 2>/dev/null || true
            printf '\n'
        fi
    done
    printf '\n=== integrated helper / mode + output display info ===\n'
    logcat -d -s LorieGpuPerf:I LorieGpuPerf:W LorieView:I LorieView:W \
        2>/dev/null | tail -n 70 || true
    printf '\n=== external gpu_keepalive (should be absent) ===\n'
    pgrep -af '^/data/data/com.termux/files/home/alroot-virpipe/gpu_keepalive ' || true
} | tee "$out"
printf '\nSaved %s\n' "$out"
