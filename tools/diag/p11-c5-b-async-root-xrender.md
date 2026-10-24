# P11-C5-B — opt-in asynchronous root Present and XRender GPU batches

**Branch:** `experiment/p11-c5-b-async-root-present`. This is an experimental successor to
P11-C5-A, not a change to the stable `feature/vulkan-render-pipeline` branch.

## Runtime and correctness

The desktop is **native Termux-Desktop XFCE4**, with its **xfwm4 compositor ON**.
Debian PRoot contains **Linux applications only**, such as Zink `glmark2` drawing into
the native desktop's X11 server.

* `TERMUX_X11_ROOT_PRESENT_ASYNC=1` (requires `TERMUX_X11_ROOT_PRESENT_GPU=1`):
  GPU Present to the twinned root **returns after queueing**, rather than awaiting its
  fence on the X main thread. Existing Xorg Present owns the source Pixmap and keeps
  its deferred-damage handling and GPU completion acknowledgement. The root's **back**
  buffer does not swap while acknowledgement is outstanding. A CPU access to that
  same root AHB waits for the latest queued GPU serial **before acquiring its EXA
  lock**. Another GPU Present is ordered through the renderer's FIFO queue.
  Renderer disconnection cancels unrecorded writes, never prematurely frees a submitted
  GPU write's buffers.

* `TERMUX_X11_XRENDER_ASYNC=1` (requires `TERMUX_X11_XRENDER_GPU=1`):
  Eligible XRender Porter-Duff operations are dispatched to Vulkan/GLES **without
  waiting for a GPU fence after each Composite**. Up to 8 jobs share a bounded
  transaction. Read-only sources may be reused; RAW/WAR buffer hazards or overlapping
  destination rectangles force a batch flush before the next operation. Each job
  pins its source and destination buffers until the device has finished. If the
  renderer skips/cancels a job, the X server runs its exact saved rectangles and
  Porter-Duff op through **Pixman on the same 32-bit CPU pixel format**, only after
  all other GPU work in that batch has been settled.
  A deferred/destroyed Pixmap or CPU read/write, XRender CPU fallback, ownership
  change, or root swap also flushes its affected batch before CPU access.
  The CPU fallback remains the existing XRender path when GPU eligibility fails.

* **Both new switches default to OFF.** The synchronous P11-C5-A behaviour and
  legacy GLES/Vulkan fallback are still available.

### Important boundary

These switches remove **unconditional per-operation GPU completion waits**, not
all CPU/GPU synchronization. The X server still waits on genuine GPU/CPU conflicts,
the compositor's frame boundary, Root swap, and certain renderer teardown paths.
**No claim of end-to-end CPU-fence-free or scanout zero-copy** is made.
The bounded independent-write batching favours correctness; overlapping fullscreen
XRender writes can still flush per operation, so a performance gain must be
measured. Mask, component-alpha, transform, repeat, and gradient are still on
the original CPU fallback path, not fully GPU supported.

## S24+ test setup

Install the **matching** ARM64 APK and Termux Companion artifact from this branch's
successful GitHub Actions build. Keep your previous known-good renderer and frame pacing
flags identical across baseline and experiment. In the **native Termux shell**:

```sh
# Termux-Desktop's native xfwm4: use the ON compositor for every trial
xfconf-query -c xfwm4 -p /general/use_compositing -s true
export TERMUX_X11_RENDERER_BACKEND=vulkan
export TERMUX_X11_RENDERER_ROOT_DOUBLE=1
export TERMUX_X11_RENDERER_ROOT_GPU_COPY=1
export TERMUX_X11_EXA_COMPOSITE_OPT=1
export TERMUX_X11_EXA_LOCK_OPT=1
export TERMUX_X11_ROOT_PRESENT_GPU=1
export TERMUX_X11_XRENDER_GPU=1

# P11-C5-B: separately opt in to the asynchronous paths
export TERMUX_X11_ROOT_PRESENT_ASYNC=1
export TERMUX_X11_XRENDER_ASYNC=1

# Keep the P11-C5-A small-copy cost gate
unset TERMUX_X11_XRENDER_GPU_MIN_PIXELS

tx11stop -f
tx11start
```

Run the existing **PRoot Debian** glmark2 benchmark and drag a native XFCE window.
First check visual stability, alpha/transparency, non-fullscreen window movement,
the absence of ghost/stretching frames, and whether the output freezes. Then measure
new-content FPS, p95/p99 frame intervals and CPU thread time using the existing
`frame-diag-session.sh` if detailed comparison is necessary.

```sh
logcat -d -s LorieNative:I |
  grep -E 'XlorieAsyncRootPresent:|XlorieAsyncXRender:|XlorieCompositeGpu:' |
  tail -n 9
```

* `XlorieAsyncRootPresent queued`: completed **enqueue calls**, not completed frames.
* `XlorieAsyncRootPresent cpu_flushes`: real CPU conflicts with a root GPU write.
* `XlorieAsyncXRender queued`: GPU work accepted without per-Composite fence waits.
* `XlorieAsyncXRender flushed`: operations whose buffer references were safely
  acknowledged after a batch's device work finished.
* `XlorieAsyncXRender replayed`: GPU-skipped/cancelled operations reissued through
  Pixman (normally should stay near zero).

For the A/B performance comparison keep the same code and options, but unset **only**
`TERMUX_X11_ROOT_PRESENT_ASYNC` and `TERMUX_X11_XRENDER_ASYNC` for the synchronous
baseline, then restart X11. Note the separate effects of each flag if necessary.

### Safe rollback

```sh
unset TERMUX_X11_ROOT_PRESENT_ASYNC TERMUX_X11_XRENDER_ASYNC
tx11stop -f
tx11start
```

For any corruption, hard freeze, device loss or unexpected pixman replay errors,
revert to the S24+-confirmed P11-C5-A APK/Companion (`208f775`) before more testing.

## Validation scope

The real source is exercised by `tpresentcopy` for async root enqueue,
CPU conflict and renderer disconnect; `tasyncxrender` for independent-write
conflict gating, real Pixman replay of a skipped/cancelled job, buffer lifetime
and CPU access barriers; and the existing full X11/Xorg/Vulkan host suite.
Device-specific frame pacing and pixel correctness still require S24+ experiments.
