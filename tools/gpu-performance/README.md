# Termux-X11 Universal Adaptive GPU Performance Controller (experimental)

## Why this exists

Samsung OneUI 8/9 and some Qualcomm/Samsung GPU firmwares show much worse
performance when the GPU's normal power-management policy returns to low-power
states between very short Vulkan/Zink/virpipe frames. The earlier external
`gpu_keepalive` program continuously executes a 16×16 off-screen EGL
`glClear() + glFinish()`, with no sleep. Device observations:

- S24+ (Exynos / Xclipse 940), user-reported: vkmark ~1500–2000 -> 3500–4000,
  glmark2 ~400–500 -> 700–900; however a big CPU core was pinned at ~100%,
  with heavy battery and thermal costs.
- S26U (Adreno), local previous A/B: Zink glmark2 980 -> 1527 with 0µs keepalive,
  861 -> 1400 with another 0µs trial, 1223 at 500µs and 1014 at 2000µs.

These are **not** universal performance predictions. Kernel GPU scheduler, DVFS,
CPU cpuset, driver overhead and off-screen frames are distinct concerns.
On S24+, shared-process Termux-X11 already restored big CPU core use for
native Shotcut render without keepalive. The new GPU controller **does not**
rewrite Android CPU/GPU scheduling policies or replace the kernel governor.

## Architecture

The Android app's `libXlorie.so` now owns one dedicated auxiliary thread
(`LorieGpuPerformance`), with its **own** EGL/GLES context and 16×16 Pbuffer.
It never touches Vulkan command buffers, renderer EGL contexts, X11 buffers,
X server locks or fd synchronization chains.

- A successful Vulkan or GLES screen presentation refreshes an atomic activity
  timestamp. **No** auxiliary jobs are dispatched to the screen.
- MainActivity's onResume/onPause indicates visible foreground state.
- Android API 29+ thermal callbacks immediately suspend synthetic work at
  SEVERE / CRITICAL / EMERGENCY / SHUTDOWN. Earlier Android releases rely on
  screen/foreground gating; they do not receive the optional thermal callback.
- No frames for at least 1.1 seconds, no foreground, severe thermal, or Off mode:
  helper **stops sending GPU work** and waits using pthread_cond_timedwait.
- Balanced/Performance modes add nanosleep after each GLES job, cap the helper's
  **per-pulse CPU duty** using CLOCK_THREAD_CPUTIME_ID, then adjust the inter-job
  sleep on a 250 ms feedback interval. CPU utilization is the HELPER ONLY,
  not the whole CPU package, and can still vary with driver sleep accounting.
- Maximum is the historical unlimited 0µs loop, for benchmark comparison.
  This can occupy a big core, worsen heat and battery, and should not be a
  production default.
- EGL ES3 16×16 Pbuffer is attempted first; ES2 is used if ES3 is not available.
  GPU detection is not required. Missing telemetry is tolerated. Clock
  readouts from readable Qualcomm KGSL / Samsung sysfs are diagnostic only.

**The governor remains the source of truth.** If the device responds to tiny
GPU jobs, performance may improve. The controller neither guarantees peak GPU
frequency nor overrides cpusets, scheduler classes or thermal throttling.
Do not claim it restores full hardware performance without A/B measurements.

## How to use

In the X11 Android app:

`Preferences -> Output -> Adaptive GPU Performance (experimental)`.

Modes:

| Mode | Auxiliary GPU requests | Nominal helper CPU policy |
|---|---|---|
| Off | Never | 0 |
| Balanced | Off when idle; dynamic, min 250 µs inter-job sleep | ~12% duty target |
| Performance | Off when idle; dynamic, min 50 µs inter-job sleep | ~28% duty target |
| Maximum | Off when idle or app hidden; 0 µs when active | Unlimited; diagnostic only |

**Default OFF on experimental builds.** The user's installed custom
`$PREFIX/bin/tx11start` (Termux-Desktop) already launches
`$HOME/alroot-virpipe/gpu_keepalive`. Running both at once invalidates the
CPU/power comparison and may reduce performance. To test the *new integrated*
controller without starting the old helper:

```sh
# In native Termux, after selecting Balanced or Performance in X11 Preferences:
TX11_GPU_KEEPALIVE=0 tx11start
# Or: tx11start --no-gpu-keepalive
```

There is **no need** to add GPU boost environment variables to the X server,
XFCE4, Shotcut, FFmpeg, vkmark or glmark2 processes. The preference belongs
to the visible X11 Android app, and the helper uses standard EGL/GLES for any
GPU that can create a small Pbuffer. This setting is orthogonal to the
existing OneUI shared-process/cpuset fix.

After confirming that the experimental APK works, an optional, reversible
Termux-Desktop migration permanently disables the OLD sidecar's default
launch, so subsequent plain `tx11start` invocations do not require a manual
environment variable:

```sh
bash tools/gpu-performance/disable-external-keepalive.sh install
# Roll back later:
bash tools/gpu-performance/disable-external-keepalive.sh restore
```

This script creates `$PREFIX/bin/tx11start.before-integrated-gpu-controller`
with the original file mode/contents and **only** changes its default
`TX11_GPU_KEEPALIVE` from `auto` or `1` to `0`. It does **not**
alter cohost detection, the existing `spawn` option, the X server or
audio settings. It is never run automatically. Setting
`TX11_GPU_KEEPALIVE=1` explicitly will still override the default.

Diagnostics from Termux:

```sh
logcat -d -s LorieGpuPerf:I | tail -n 30
pidof gpu_keepalive  # should print nothing during integrated-mode testing
```

Log fields: `mode`, `pulses/5s`, `cpu_pct` (auxiliary thread only),
`sleep_us`, `frames/5s`, optional `gpu_clock_raw` (vendor units), `thermal`,
`foreground`. If there are no presentation events, the helper stays idle.

## A/B procedure for S24+ and S26U

Keep the device cooled, power mode, display refresh, desktop session,
resolution, graphics driver and benchmark item identical across runs.
Use representative light and heavy vkmark and glmark2 scenes; compare:

1. Off, legacy external helper also disabled (baseline).
2. Balanced, legacy disabled.
3. Performance, legacy disabled.
4. Maximum, legacy disabled (short benchmark only; highest battery demand).
5. Historical external `gpu_keepalive -a -p -s 0` (baseline for maximum),
   with the integrated controller Off.

Collect FPS/score, `LorieGpuPerf` log, helper CPU duty (htop), GPU
clock/load if readable, CPU core usage, temperature and a few minutes of
active/idle energy. Compare new-content FPS and p95/p99 frame intervals
separately from GPU benchmark scores. The controller may be less effective
than old unlimited keepalive but should reduce CPU consumption in Balanced
and Performance modes.

## Rollback

Select Off in X11 Preferences, or reinstall the known-good Android APK.
Keep the old Termux-Desktop binary and native X11 wrapper unchanged.
All Vulkan/XRender/DRI3 optimizations, CPU affinity, the kernel GPU
governor and device clock limits remain unchanged.
