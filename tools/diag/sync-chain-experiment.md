# P11-C3-E2: XFCE Compositor vs CPU/GPU sync-chain test

## Why

On S24+ / Xclipse 940 (One UI 8/9), dragging an XFCE window while glmark2 runs drops the rate of *new* content on
the screen from ~119/s to ~85/s, although the Vulkan renderer still presents ~119/s.  Switching Xorg's client request
scheduler from 5 ms to 2 ms **did not help**; it reduced new content further and increased GPU/root-copy wait.  The
next hypothesis is CPU XRender Composite and its AHB/GPU synchronization, possibly aggravated by Android scheduling.

The preexisting `XlorieComposite: cpu_us` is *inferred*: Composite wall time less GPU, root-copy, AHB lock and shared
lock waits.  It is **not** actual CPU execution time.  With the opt-in `TERMUX_X11_SYNC_CHAIN_DIAG=1`, this build now
samples `CLOCK_THREAD_CPUTIME_ID` around each Composite.  `XlorieSyncChain: thread_cpu_us` is actual X-thread CPU
execution, and `offcpu_us = wall_us - thread_cpu_us` covers both CPU runqueue waits *and* intentional sleeping.  Root
and GPU waits frequently **overlap** off-CPU; do NOT add them to it.  `XlorieSyncWait` separately classifies root-copy
waits (inside/outside a Composite, taken/not-taken at entry) and per-access AHB GPU waits (source/destination/mask).
The latter excludes root swap's own GPU waits and the Android renderer's Vulkan fence waits.

Only opt in with **both** `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1` and `TERMUX_X11_SYNC_CHAIN_DIAG=1`.  Without the new
flag, no thread CPU clock samples are taken and no rendering/synchronization decision is changed.  It does not enable
GPU compositing or AHB zero-copy; the purpose is diagnosis.  Existing tests for the 2ms Xorg scheduler are **not** in
this branch, which is based on `d1fe278`.

## S24+ test steps (new ARM64 APK AND matching Companion from this branch's CI)

**Actual workload topology (important):** the graphical desktop is **Termux-Desktop XFCE4 running in native Termux**.
Its `xfwm4` is the compositor under test.  The benchmark/application (`glmark2`, etc.) runs **inside a Debian PRoot
rootfs**, drawing windows into that same Termux-Desktop X11 display.  The X server/Companion and the Android Vulkan
Activity are outside that PRoot environment.  Do **not** launch a second Debian XFCE desktop or try to change xfwm4's
configuration from a Debian PRoot terminal: that terminal may have a different (or no) xfconf D-Bus session.  In this
test, **only the application belongs in PRoot; xfwm4 configuration belongs to native Termux-Desktop XFCE**.

Download these two scripts into Termux.  There is no need to clone the repository on the S24+:

```sh
BASE="https://raw.githubusercontent.com/hansm629/termux-x11/experiment/p11-c3-e2-sync-chain"
curl -fsSL "$BASE/tools/diag/frame-diag-session.sh" -o "$HOME/frame-diag-session.sh"
curl -fsSL "$BASE/tools/diag/cpu-sched-sample.sh" -o "$HOME/cpu-sched-sample.sh"
```

Use the **same Vulkan/Root-double/EXA options and screen resolution as the previous 5ms baseline**; in particular:

```sh
unset TERMUX_X11_X_SCHED_INTERVAL_MS  # leave Xorg's default request scheduler unchanged
export TERMUX_X11_RENDERER_VK_FRAME_DIAG=1
export TERMUX_X11_FRAME_FREEZE_DIAG=1
export TERMUX_X11_CONTENT_FRAME_DIAG=1
export TERMUX_X11_SYNC_CHAIN_DIAG=1
tx11stop -f
tx11start
```

To check the flag after starting X11, the 5-second Logcat reports should contain `XlorieSyncChain:` and
`XlorieSyncWait:`.  If they are absent, check that **Companion** was updated and the flags reached its X server.

### A: XFCE Compositor ON

Open a terminal **inside the native Termux-Desktop XFCE session**, not inside Debian PRoot.  Check that the command
resolves to Termux's prefix and the requested value actually changes the running Termux-Desktop xfwm4:

```sh
command -v xfconf-query
xfconf-query -c xfwm4 -p /general/use_compositing -s true
xfconf-query -c xfwm4 -p /general/use_compositing
```

Run **glmark2 from the existing Debian PRoot environment**, using exactly the same application launch and GL/Vulkan
settings as the earlier measurements.  Do **not** replace it with a native Termux build of glmark2: that would change
the client and invalidate the A/B comparison.  The diagnostic scripts run from a **native Termux shell** (they observe
the existing XFCE desktop and the PRoot application's X11 activity):

```sh
sh "$HOME/cpu-sched-sample.sh" 150 "$HOME/tx11-sched-compositor-on.log" &
SAMP_PID=$!
sh "$HOME/frame-diag-session.sh" "$HOME/frame-diag-compositor-on.log"
wait "$SAMP_PID"
```

Follow the script: idle 10 s, glmark2 alone 20 s, keep moving an XFCE window over glmark2 for 60 s.  Use the same
benchmark scene, window size, drag area and refresh settings in both arms.  The window movement and Compositor
state must stay consistent; avoid a thermal state change between trials.

### B: XFCE Compositor OFF

Again, in the **native Termux-Desktop XFCE terminal** (same xfconf instance as arm A):

```sh
xfconf-query -c xfwm4 -p /general/use_compositing -s false
xfconf-query -c xfwm4 -p /general/use_compositing
```

Close and relaunch the same **Debian PRoot glmark2** scene, then in **native Termux**:

```sh
sh "$HOME/cpu-sched-sample.sh" 150 "$HOME/tx11-sched-compositor-off.log" &
SAMP_PID=$!
sh "$HOME/frame-diag-session.sh" "$HOME/frame-diag-compositor-off.log"
wait "$SAMP_PID"
```

After the test, restore your original compositor setting (normally `true`) in the **native Termux-Desktop XFCE session**:

```sh
xfconf-query -c xfwm4 -p /general/use_compositing -s true
```

Attach the **four original `.log` files**, rather than pasted/truncated contents, and report subjective drag
smoothness.  The 5-second `XlorieSyncChain` and `XlorieSyncWait` lines are included in the frame logs and summarized
per phase by the updated `frame-diag-session.sh`.

## What the result means

If Compositor OFF recovers near-120 new-content frames/s *and* sharply cuts real X-thread CPU time, xfwm4's XRender
compositing and its waits are an important bottleneck.  If frame updates remain low with Compositor OFF, investigate
the X11 Present request dispatch and Android-side renderer synchronization independent of xfwm4.  If CPU execution
stays low but off-CPU and GPU/root waits rise, target those waits instead of pixman arithmetic.  This comparison
changes the window redirection/presentation path as well as CPU workload, so it does NOT isolate the pixman kernel
alone.  These are hypotheses, not a declaration that One UI/ANGLE caused the regression.

Disabling native Termux-Desktop xfwm4's compositor does **not** disable the separate Android Vulkan renderer's own
final rendering/composition.  The comparison tests the host desktop compositor's contribution to the same PRoot
client's frames, not the Linux distro's desktop performance.
