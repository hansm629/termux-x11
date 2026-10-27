# Termux-X11 P11-Xclipse: AHB GPU/CPU wait lock splitting (v8)

## Baseline and observed problem

Based on `experiment/xclipse-vsync-gate-v6` (`197255f`) and **not**
the abandoned Game Booster/GOS `v7` branch. No power daemon, CPU spin,
clock governor writes, libhybris bridge or APK Bionic server added.

On the S24+ Xclipse 940 Vulkan display backend, window movement and
vkmark are still affected by a GPU completing Present-copies and the X
server trying to CPU-access the AHBs. Recorded S24+ examples:

* v6 with the original single root AHB: 621 FPS; frame-carried
  Present-copy GPU fence waits commonly **3–4 ms** and X server CPU
  accesses waiting for that fence while holding the shared global mutex.
* The later Game Booster/GOS tests still produced approximately 742 FPS
  at the lowest GPU frequencies. This is not evidence that DVFS was
  changed or that frame-copy synchronization no longer matters.

**Important:** `XlorieVkTiming: fence_wait_us` measures CPU time spent
waiting on a real Vulkan submission. `XlorieBackend: x_gpu_wait_us`
measures the X server waiting for *a shared AHB*. They can overlap:
do not sum the values and pretend they are two independent delays.

## V8 change

Existing `TERMUX_X11_EXA_LOCK_OPT=1` (P11-C3-B1) *already* allows an X
server CPU access to reserve a non-root AHB and release the mutex during
its subsequent software rendering. However, it reserved the AHB only
**after** waiting for the prior GPU frame to finish, so the global mutex
was still held across the entire GPU futex wait.

When `TERMUX_X11_EXA_EARLY_RESERVE=1` is also enabled, a CPU access
encountering a prior GPU frame that is actively using a given AHB can:

1. Under `state->lock`, reserve exactly that AHB in `cpuAccess`.
   Continue only if the reservation succeeds, the AHB has not already
   been CPU-locked, and no other nested X CPU access holds the mutex.
2. Release `state->lock` **before** `lorieGpuWaitIdle(st, false, id)`.
   No other renderer submission may access that AHB while reserved.
   In-flight previous GPU work remains recorded in `gpuInFlight`.
3. Wait for that previous GPU frame to complete, then CPU-lock the AHB.
   The AHB remains reserved while the X server reads/writes its pixels.
4. On success, finish CPU drawing and unlock the AHB *before* releasing
   the reservation, as before. On CPU lock failure, release the
   reservation before returning failure. Renderer wakeup is unchanged.

If the AHB is not in flight, all reservation slots are busy, a nested
CPU access holds the global lock, or the opt-in is disabled, the code
uses the **existing** path. The GLES renderer and PERSISTENT ownership
path are unchanged. GPU Present-copy only work can be deferred until
the reservation ends, without waiting while holding the global mutex.

The published root normally cannot be reserved. An **independent,
higher-risk** opt-in `TERMUX_X11_EXA_EARLY_RESERVE_ROOT=1` allows it
ONLY if the previous Vulkan frame is still using that root AHB and the
two required options above are enabled. `rendererVkRedraw` already
recognizes a reserved published root: it does not sample it, holds the
acquired image, runs independent copies and waits for CPU release.
No previous frame's GPU fence can be skipped.

With either option the X server's 5-second log emits:

```text
XlorieEarlyReserve: enabled=on root=off|on reservations_before_gpu_wait=N(+d) gpu_wait_outside_global_lock_us=N(+d)
```

The old `x_gpu_wait_us` still includes time waiting for actual GPU
completion. A lower `x_state_lock_wait_us` means less global-lock
contention, not necessarily fewer GPU instructions.

## Controlled S24+ trial

Use the matching new ARM64 v8 APK and the existing desktop runtime
from the same branch. **Save XFCE session work and install manually:**
installing or restarting the Termux shared-UID app remotely can sever
the DevSpace connection. No remote installation was performed.

First test the less invasive, **non-root** improvement:

```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1 \
TERMUX_X11_EXA_LOCK_OPT=1 \
TERMUX_X11_EXA_EARLY_RESERVE=1 \
TX11_GPU_KEEPALIVE=0 tx11start
```

Only after confirming no freezes, artifacts or missed Present callbacks,
try the separately guarded root experiment:

```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1 \
TERMUX_X11_EXA_LOCK_OPT=1 \
TERMUX_X11_EXA_EARLY_RESERVE=1 \
TERMUX_X11_EXA_EARLY_RESERVE_ROOT=1 \
TX11_GPU_KEEPALIVE=0 tx11start
```

Keep `ROOT_DOUBLE` and `ROOT_PRESENT_ASYNC` **off** for this A/B,
so the two changes are not conflated. Later, the already implemented
split-root and asynchronous Present path can be evaluated separately.

After each launch, run the same fixed single-scene vkmark and move an
XFCE window while a Vulkan/Zink demo is drawing:

```sh
vkmark -b 'vertex:device-local=true'
logcat -d -s LorieNative:I '*:S' |
  grep -E 'XlorieEarlyReserve:|XlorieBackend:|XlorieCopyLatency:|XlorieVkTickGate:' |
  tail -n 16
```

Compare identical resolution, compositor state, refresh rate, device
temperature, and external keepalive OFF. Check stutter and p95/p99
frame intervals, not vkmark FPS alone. A ZERO
`reservations_before_gpu_wait` delta means the early-reservation path
did not activate in that workload; do not claim an FPS improvement
from inactive code.

## Rollback and safety

Both new options default to OFF. Restore the v6 runtime path on the
same v8 APK by restarting X11 with:

```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1 \
TERMUX_X11_EXA_LOCK_OPT=0 \
TERMUX_X11_EXA_EARLY_RESERVE=0 \
TERMUX_X11_EXA_EARLY_RESERVE_ROOT=0 \
TX11_GPU_KEEPALIVE=0 tx11start
```

Source tests exercise the actual `loriePrepareAccess`/reservation code:
lock held vs released, previous GPU frame not complete at reservation,
multiple futex timeouts, all in-flight work ended before CPU lock,
reservation release, and the published root unchanged without opt-in.
The renderer's separate host tests already verify a reserved published
root cannot be sampled and must be retried.

**This is not an end-to-end no-fence implementation.** Hardware buffer
lifetimes, EXA read/write hazards, and Vulkan foreign ownership
transfers still require real synchronization. Improvements on the
device are not confirmed by Android CI or host tests alone.
