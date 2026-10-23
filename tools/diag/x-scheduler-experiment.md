# P11-C3-E: Xorg request-scheduler experiment (One UI 8/9)

The S24+ Xclipse 940 diagnostic session showed glmark2's **server-processed** PresentPixmap rate fall from roughly
819/s to 313/s while xfwm4 windows were moved.  Vulkan still submitted about 119 display frames/s; new content fell
from 119/s to 86/s.  This does **not** prove the Android kernel CPU scheduler is responsible.  Xorg's *own* client
request scheduler is an independently testable variable.

`TERMUX_X11_X_SCHED_INTERVAL_MS=2` is an opt-in experiment, accepted only as an integer in **1..10**.  It changes Xorg
`SmartScheduleInterval` and `SmartScheduleSlice` to 2 ms, and its `SmartScheduleMaxSlice` to 6 ms before Xorg installs
its scheduling timer.  The default (unset) leaves all of Xorg's settings untouched (normally 5/5/15 ms).  This is
equivalent in spirit to Xorg's `-schedInterval`/`-schedMax` arguments; it makes no cpuset, uclamp, affinity, nice or
Android process-priority changes.  It may make scheduling fairer at a cost in signal/context-switch overhead; it is
**not** a measured fix.  It also has no effect with Xorg's `-dumbSched` mode.

With the experimental APK and companion installed on the *S24+*, set the environment **before** starting the X server:

```sh
export TERMUX_X11_X_SCHED_INTERVAL_MS=2
tx11stop -f
tx11start
logcat -d -s LorieNative:I | grep 'XlorieXScheduler:' | tail -n 3
```

Run the *same* glmark2 idle/standalone/window-moving phases as before, and compare the content, composite, root-copy
and `XlorieXSched` metrics.  Run the baseline with `unset TERMUX_X11_X_SCHED_INTERVAL_MS` and **restart X11**; merely
unsetting after startup cannot reset a running X server.  Compare identical resolutions, scenes and environment
settings, and note visual smoothness.  If it worsens, revert simply by unsetting the variable and restarting X11.

Evidence for an *Android kernel* CPU scheduling bottleneck still requires scheduling traces for glmark2 and xfwm4,
plus X server main/render threads.  Increasing `uclamp_min` using `sched_setattr` was rejected with `EPERM` on a
non-root Galaxy S26U Termux process, and is intentionally **not** attempted by this patch.

To collect a low-overhead, unprivileged comparison during the original frame test, start this sampler in a second
Termux terminal.  The S24+ **does not need a cloned source checkout**: download just the script from this branch.
It watches for glmark2 appearing after the idle phase:

```sh
curl -fsSL "https://raw.githubusercontent.com/hansm629/termux-x11/experiment/p11-c3-e-xorg-scheduler/tools/diag/cpu-sched-sample.sh" -o "$HOME/cpu-sched-sample.sh"
sh "$HOME/cpu-sched-sample.sh" 120
```

It writes `~/tx11-sched-*.log` with one sample per second for accessible Linux processes.  `run_ns` and
`runqueue_wait_ns` are monotonic per-thread totals; compare **deltas**, not their absolute values.  It does not observe
Android/ANGLE/GPU scheduling or guarantee access to the renderer's distinct UID.  The ordinary `frame-diag-session.sh`
can be run concurrently, and the two logs can be compared by timestamp.
