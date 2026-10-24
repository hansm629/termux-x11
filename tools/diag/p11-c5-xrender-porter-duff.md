# P11-C5-A: additional XRender GPU operators + conservative cost gate

> This is the synchronous P11-C5-A baseline. The successor
> [P11-C5-B](p11-c5-b-async-root-xrender.md) adds separate opt-in switches for
> deferred root Present and bounded XRender GPU batches with CPU replay.

This is the follow-up to P11-C4. **Keep Termux-Desktop's native XFCE4 compositor ON** and the
Root Double Buffer ON. The Debian PRoot session is the benchmark application, *not* the desktop
that owns `xfwm4`.

## Implemented

When `TERMUX_X11_XRENDER_GPU=1` and Vulkan is active:

* The existing AHB GPU-copy queue handles **12 unmasked Porter–Duff operators**:
  `Src`, `Over`, `Clear`, `OverReverse`, `In`, `InReverse`, `Out`, `OutReverse`,
  `Atop`, `AtopReverse`, `Xor`, and `Add`.
* Vulkan pipelines and the GLES compatibility path share one factor table for **premultiplied
  alpha**, applying the factors to **both RGB and alpha**. Repeated writes to the same
  imported Vulkan attachment have an explicit colour-attachment memory barrier between passes.
* Internal 32-bit **ARGB pixmaps retain real alpha**: under this opt-in only, depth-32 server
  pixmaps use an RGBA AHB instead of the older RGBX AHB (also on regular-to-AHB conversion).
  Existing 24-bit RGBX pixmaps and the default flag-off path remain unchanged.
* Alpha-required pictures in RGBX AHB memory are conservatively rejected (CPU fallback);
  operations reading destination alpha also reject XRGB destinations, whose attachment
  alpha bits may be undefined.
* Small composites are **not accelerated by default**. GPU submission/fence overhead may
  outweigh software pixman for individual decorations or glyphs.
  `TERMUX_X11_XRENDER_GPU_MIN_PIXELS` defaults to `8192` destination pixels. Valid values
  are 0..16777216; `0` disables this size gate for an A/B performance test.
* An unavailable optional blend pipeline is skipped, letting the X server use CPU fallback
  instead of turning off Vulkan globally. Both new flags remain opt-in.

**NOT implemented yet:** mask and component-alpha sampling, glyph A8 uploads, geometric
transform/bilinear/repeat handling, source gradients, saturate, disjoint/conjoint. Those
still follow the existing XRender/pixman CPU fallback.

**Also not removed yet:** the P11-C4 synchronous `lorieWaitGpuCopyRecorded()` for GPU
operations. This stage avoids many unnecessary short round trips but does **not** remove
CPU fence waits. Eliminating them requires tracking queued/in-flight GPU reads and writes
by AHB ID, deferred destruction, asynchronous damage and ordered CPU fallback. Do not simply
delete the wait: that would race GPU writes with pixman accesses or root buffer swaps.

## S24+ comparison

Update the ARM64 APK **and** its Companion from this branch's successful CI. Use the same
Vulkan and Termux-Desktop options as P11-C4:

```sh
export TERMUX_X11_RENDERER_BACKEND=vulkan
export TERMUX_X11_RENDERER_ROOT_DOUBLE=1
export TERMUX_X11_RENDERER_ROOT_GPU_COPY=1
export TERMUX_X11_EXA_COMPOSITE_OPT=1
export TERMUX_X11_EXA_LOCK_OPT=1
export TERMUX_X11_ROOT_PRESENT_GPU=1
export TERMUX_X11_XRENDER_GPU=1

# Arm A: default GPU cost gate (recommended starting configuration)
unset TERMUX_X11_XRENDER_GPU_MIN_PIXELS
# Arm B: compare large-only acceleration
# export TERMUX_X11_XRENDER_GPU_MIN_PIXELS=32768
# Arm C: stress-test 0 threshold, NOT recommended as faster by default
# export TERMUX_X11_XRENDER_GPU_MIN_PIXELS=0

tx11stop -f
tx11start
```

Use the exact same PRoot Debian glmark2 scenario as P11-C4, including 120 Hz and continuous
dragging of native XFCE windows. Only the named `TERMUX_X11_XRENDER_GPU_MIN_PIXELS` variable
should change across the cost-gate runs. Termux-X11 reads it on startup.

`XlorieCompositeGpu` now prints:

* `pd_done` — completed accelerated composites (not number of Android display frames)
* `cpu_fallback` — an otherwise eligible operation that was queued unsuccessfully/skipped
* `tiny_cpu` — operations left to CPU under the current pixel threshold
* `min_pixels` — chosen threshold

Compare new-content frame rate, mean/99th percentile content latency, dragging
smoothness and device stability. These are measurements **not established by host tests**.
Any image corruption, alpha artefact, window stretching, or freeze means revert the new
experimental flags and retain the verified P11-C4 APK.

## Validation

`tporterduff` checks 12 modes against an independent premultiplied-alpha mathematical
reference on 23,232 pixel combinations. `tvkframe` records and submits every supported mode
on a fake Vulkan GPU with AHB ownership checking. `tpresentcopy` checks the GLES blend
factors and restored blend state. `txrendergpu` validates AHB-alpha correctness gating,
cost gate, damage geometry, failed-job CPU fallback and root-buffer opt-in.
