# S24+ Xclipse 940 v10: visible-region GPU Present + CopyWindow destination fix

## Baseline and root-cause limits

Branch: `experiment/xclipse-present-visible-gpu-copy-v10`, based on v9
`492f112` (v9's `TERMUX_X11_RENDERER_VK_PRESENT_COPY_EARLY` stays OFF
unless explicitly requested). The best demonstrated CPU-lock profile remains
**v8 STEP 2**: `TERMUX_X11_EXA_LOCK_OPT=1`,
`TERMUX_X11_EXA_EARLY_RESERVE=1`, `TERMUX_X11_EXA_EARLY_RESERVE_ROOT=0`,
with the Vulkan single-frame-per-tick gate enabled.

Previous S24+ capture `~/tx11-v9-content-stall.log` showed 130.3 final
screen frames/s during window dragging but just 26.6 *new-content*
frames/s; the vsync presenter itself was **not** blocked at 26.6 Hz. The
main Xorg thread performed more pixman/XRender CPU work and waited for
AHB GPU ownership. The capture also showed `exa_lock_opt=off` and
`XlorieVkTickGate: enabled=off`, hence was **not** an A/B of the best
v8 STEP 2 configuration. Its primary Present client was named `vkmark`,
not `glmark2`. Never label these as measurements under STEP 2.

**Important:** v10 does not make `lorieGpuWaitIdle` asynchronous for
arbitrary XRender/EXA requests. Such requests read real AHB pixels before
returning, and releasing the buffer before the GPU's completion would
corrupt pixels or violate Vulkan foreign-memory ownership. A future
dependency-aware GPU XRender/masked-CPU compositor architecture is a
separate project; a clipped Present-copy alone cannot fix all stuttering.

## Changes

1. [Upstream concept: window.clipList](https://github.com/termux/termux-x11/commit/29d4460b26e69b39af023d255a255e2778251952):
   optional `TERMUX_X11_PRESENT_GPU_VISIBLE_CLIP=1`. Before accepting a
   *new* GPU Present-copy, clone its update region (or create the full
   source-pixmap region if no update was given), translate the window's
   screen-coordinate clipList into the presented source-pixmap coordinates
   using `-x_off - window.x`, and intersect. The Vulkan GPU-copy queue
   receives these **smaller rectangles**. Only after queue acceptance is
   the source update replaced with the clipped update; deferred or
   immediate Damage uses the exact GPU copied pixels. If the region is
   empty, allocation fails, or GPU scheduling refuses the operation,
   keep the original Present update and its full CPU fallback behavior.
   An in-flight or retired GPU operation is not rescheduled or modified.
   **Defaults OFF**; no impact on the v8/v9 known-good path unless set.
2. [Upstream CopyWindow DEST fix](https://github.com/termux/termux-x11/commit/09194f26292bac4e1de853eb651f2af92f1e2306):
   patch Xorg `exa/exa_unaccel.c` to prepare and finish the
   `ExaCheckCopyWindow` destination as `EXA_PREPARE_DEST` instead of
   `SRC`. This is a correctness/ownership-role fix on the window-movement
   path, not a guaranteed FPS optimization.
3. `XloriePresentClip` 5-second evidence, with values `N(+delta)`:
   `enabled`, `gpu_copies` (successfully clipped GPU submissions),
   `before_pixels`, `after_pixels`, `saved_pixels` (cumulative original
   minus copied pixel area for accepted, nonempty regions). No optimistic
   assumptions about GPU clocks or present-to-scanout zero-copy.

## S24+ controlled test

Install the **matching v10 APK and Termux Companion** from branch CI,
by the device owner, not by remote DevSpace. Do not restart running
Termux, X11 or XFCE remotely: they maintain the development connection.
Save the desktop's open work first.

In the **same native Termux launch shell**, establish the known-good
baseline explicitly; these variables are consumed at **X server start**:

```sh
export TERMUX_X11_RENDERER_BACKEND=vulkan
export TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1
export TERMUX_X11_EXA_LOCK_OPT=1
export TERMUX_X11_EXA_EARLY_RESERVE=1
export TERMUX_X11_EXA_EARLY_RESERVE_ROOT=0
export TERMUX_X11_RENDERER_VK_PRESENT_COPY_EARLY=0
export TERMUX_X11_RENDERER_VK_FRAME_DIAG=1
export TERMUX_X11_CONTENT_FRAME_DIAG=1
export TERMUX_X11_SYNC_CHAIN_DIAG=1
export TX11_GPU_KEEPALIVE=0
```

Arm A: `export TERMUX_X11_PRESENT_GPU_VISIBLE_CLIP=0`, then start
`tx11start` normally. Arm B: stop/restart the old X session *yourself*,
change only `TERMUX_X11_PRESENT_GPU_VISIBLE_CLIP=1`, then `tx11start`
again. Both arms must use the **same APK**, desktop compositor setting,
120Hz mode, benchmark window size, thermal conditions, benchmark scene,
and identical window dragging.

**Before** any FPS comparison, verify the flags in the *current* Lorie
process, rather than reading old logcat entries from an earlier PID:

```sh
logcat -d -s LorieNative:I '*:S' |
  grep -E 'XlorieBackend:|XlorieVkTickGate:|XloriePresentClip:' |
  tail -n 12
```

Required: `requested=vulkan active=VULKAN`,
`exa_lock_opt=on`, `XlorieVkTickGate enabled=on`. On the B arm,
`XloriePresentClip enabled=on` and `gpu_copies(+d)>0` confirm that
the code ran. If `saved_pixels` never rises, this specific workload
does not benefit from visibility clipping, and the option may cause
extra Region allocation overhead; switch it OFF.

Use only one lightweight vkmark item and the same Debian glmark2 item
with/without dragging, not different full benchmark sets. Measure **FPS
during drag, glmark2 frame continuity, new-content screen frames/s,
content gap p99, X thread CPU and AHB GPU wait**. The existing
`~/frame-diag-session.sh` may capture the three phases; no need to
repeat previously completed XFCE compositor ON/OFF, Game Booster or
GPU keepalive tests. Retain original logfile and comparison flags.

For rollback, set `TERMUX_X11_PRESENT_GPU_VISIBLE_CLIP=0` and restart
X11 in the usual native shell. Report any newly black/incorrect regions,
partial repaint on window unocclusion, X11 freezes or renderer fallback
immediately; clip is an experimental visibility optimization.

## Local verification

The patched Xorg C sources pass native Android syntax validation.
`TPRESENTSAFETY` extracts and tests the **real patched Present code**,
including region-coordinate clipping, deferred Damage, CPU fallback,
failed allocations and buffer ownership/retirement. `TDIAGLOG` checks
the *actual* formatter with synthetic cumulative clip statistics.
The remaining v9 Vulkan/EXA host suite must still pass. This tests
source correctness, not real S24+ frame pacing: no FPS or stutter
improvement is claimed until on-device A/B evidence exists.
