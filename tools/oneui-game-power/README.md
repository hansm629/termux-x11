# Xclipse 940 GPU DVFS: Android GAME Power HAL A/B (v7)

This branch inherits Vulkan WSI v5 and the optional tick gate v6,
but **does not change their rendering code or frame fences**.

## On-device evidence

S24+ / Exynos 2400, Xclipse 940, Android 17, 120Hz on-screen,
TERMUX_X11_RENDERER_BACKEND=vulkan and SINGLE_FRAME_PER_TICK=1:

| Condition | vkmark vertex FPS | Observed GPU clock |
|---|---:|---|
| v6, integrated Maximum, no external keepalive | 621 | usually 252MHz |
| v6 plus native gpu_keepalive -s 0 for 10s | 3101 | frequently 800–900MHz |

Keepalive raised GPU temperature from 36C to 45C in the 10-second test.
Its CPU spin is NOT an acceptable power-efficient default.
Multiple Samsung GPU Present-copy fences averaged about 4ms and can
delay X11 CPU AHB access. More clock headroom would plausibly shorten
that wait, especially during XFCE window movement, though graphics
submission / memory ownership contention remains a separate bottleneck.

**Rootless Android ADPF was tested directly in the benchmark**:
APerformanceHint_createSession succeeded; a one-shot GPU workload
increase and repeated *measured* vkQueueSubmit CPU time reports were
accepted. They did NOT raise score or keep clocks boosted in short A/B
tests (v6 vertex scores 829 control / 792 one-shot / 678 per-submit
reporting, affected by run-to-run variation). S24+ reports
APERF_HINT_SESSIONS available but GRAPHICS_PIPELINE and AUTO_GPU
unsupported. Do not claim that ADPF hints solve Samsung GPU DVFS.

## Why the GAME category experiment is different

Android 14 and newer can set Power HAL's GAME power mode for an active
package whose manifest declares android:appCategory="game".
The Power HAL is then free to apply its own CPU/GPU power policies.
See Android AOSP official documentation:
https://source.android.com/docs/core/perf/boost
https://developer.android.com/games/optimize/adpf/gamemode/gamemode-api

**It is NOT a guaranteed clock boost**: Samsung's implementation may
ignore the mode, throttle it, or apply Game Booster restrictions. This
experiment changes only the Android package classification, not the
actual GPU driver, native X server, shader, game client or clock sysfs.

Production/ordinary CI keeps the category **absent**. Only the opt-in
workflow input game_power_mode=true adds it in an isolated CI checkout
and builds an arm64 artifact with name
termux-x11-arm64-v8a-GAME-POWER-EXPERIMENT. It is never combined with
cohost_termux. Build verification inspects the real merged manifest.
The S24+ normal, non-Cohost package keeps process=com.termux.x11.

The game-category APK logs
LorieGamePower: foreground=true app_category=0 game_mode=N
on Android Activity start (0 is ApplicationInfo.CATEGORY_GAME; Game Mode
STANDARD is 1, PERFORMANCE is 2; unsupported is 0). This is read-only
and does not forcibly set the mode. If the API does not exist, the
experimental logger is skipped.

## S24+ controlled A/B

1. Record installed **normal** v6 APK baseline at matched temperature,
   DISPLAY=:0, 1920x1080 desktop, 120Hz on-screen, with integrated
   Maximum and *external keepalive off*. The vkmark vertex test itself
   defaults to an 800x600 window. Keep exactly the same scene.
2. Install the **GAME-POWER-EXPERIMENT** arm64 APK from the separately
   labeled GitHub Actions artifact. Normal and experimental builds have
   the same package ID/signing, so an update may restart Android X11.
   Save open work; do not install remotely via DevSpace.
3. Launch the same X11 session:

       TERMUX_X11_RENDERER_BACKEND=vulkan \
         TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1 \
         TX11_GPU_KEEPALIVE=0 tx11start

4. Compare:

       vkmark -b 'vertex:device-local=true'
       logcat -d -s LorieGamePower:I '*:S' | tail -n 8
       logcat -d -s LorieNative:I '*:S' |
         grep -E 'XlorieVkTiming:|XlorieCopyLatency:|XlorieVkTickGate:' |
         tail -n 9

   Sample /sys/kernel/gpu/gpu_clock and gpu_busy **during the benchmark**
   (not just after it has exited). Check frame pacing during window
   movement, and if safe a Vulkan demo plus moving the window.
5. If the Android GameManager returns STANDARD, Samsung's Gaming Hub
   may allow a Performance mode for the newly categorized package.
   This is user-visible system configuration: do not change it without
   the user's choice. Re-test only if actually selectable.
6. If the package category is correctly reported but performance
   remains close to baseline, the Android GAME power-mode hypothesis
   is not sufficient on this firmware. Reinstall the ordinary v7 APK
   (game_power_mode=false) to revert classification.

Avoid claiming that higher score proves stutter is fixed. Check
presentation interval/jank and thermals. A Vulkan GPU-copy waiting on
its fence has a real dependency; a clock hint does not make it safe to
release foreign AHBs early.
