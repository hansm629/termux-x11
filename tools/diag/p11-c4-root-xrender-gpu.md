# P11-C4 experimental GPU XRender + twinned Root GPU Present-copy

> This document describes the P11-C4 baseline. The follow-up
> [P11-C5-A](p11-c5-xrender-porter-duff.md) extends Porter-Duff GPU operators,
> preserves alpha in ARGB32 AHBs, adds a GPU size gate and renames
> `XlorieCompositeGpu: src_over_done` to `pd_done`.

## What this build does

This **experimental** branch extends the existing Vulkan renderer's AHardwareBuffer GPU-copy queue:

* `TERMUX_X11_ROOT_PRESENT_GPU=1` permits a GPU Present-copy into the **back** pixmap of the
  two-buffer root while its independent **front** remains the Android renderer's source.
  The server waits for the GPU operation's *completed fence and recorded-success result* before
  allowing any later CPU access; the front is not replaced while a root Present acknowledgement
  is outstanding. **The old CPU fallback remains available.**
* `TERMUX_X11_XRENDER_GPU=1` intercepts only XRender **Src** and premultiplied-alpha **Over**
  with a real, nonrepeating, untransformed ARGB32/XRGB32 source and destination, no mask or alpha
  map, distinct AHBs, and no open CPU reservations. Vulkan performs a framebuffer shader draw,
  with appropriate Src or Over blending; unsupported cases remain on EXA/pixman CPU fallback.
* Neither flag is necessary for the original implementation. Both default to **OFF**.
  XRender GPU can operate without `TERMUX_X11_RENDERER_VK_FRAME_DIAG` (the additional clock
  sampling is only for diagnostics). The premultiplied Over pipelines are not created when disabled.
* This is **not** compositor zero-copy or a CPU-wait-free async renderer. The first integration
  deliberately waits for GPU fence completion on the X server thread for new root Present and
  accelerated XRender operations. It must be measured on the S24+ before judging performance;
  very small XRender operations may be *slower* than pixman. There is no production default change.

## S24+ topology and test

The **desktop / xfwm4** runs natively in **Termux-Desktop XFCE4**; Mesa/Zink/`glmark2` runs in
**PRoot Debian** as an application connected to that desktop's X server. Do not start Debian XFCE
or disable the native xfwm4 compositor for this test. Install **both** the ARM64 Termux-X11 APK and
matching Companion from this branch's successful Android CI.

In the **native Termux** shell that starts the X server, keep your previous tested Vulkan, EXA,
Root Double, and Root GPU-copy settings, and make sure the compositor is **ON**:

```sh
xfconf-query -c xfwm4 -p /general/use_compositing -s true
export TERMUX_X11_RENDERER_BACKEND=vulkan
export TERMUX_X11_RENDERER_ROOT_DOUBLE=1
export TERMUX_X11_RENDERER_ROOT_GPU_COPY=1
export TERMUX_X11_EXA_COMPOSITE_OPT=1
export TERMUX_X11_EXA_LOCK_OPT=1

# Compare:
# A (baseline): unset TERMUX_X11_ROOT_PRESENT_GPU TERMUX_X11_XRENDER_GPU
# B (root GPU Present): export TERMUX_X11_ROOT_PRESENT_GPU=1
# C (root and XRender): export TERMUX_X11_ROOT_PRESENT_GPU=1 TERMUX_X11_XRENDER_GPU=1
tx11stop -f
tx11start
```

Run the same PRoot Debian `glmark2` workload, window sizes, 120Hz mode and XFCE window dragging
in each arm. A restart is needed between arms to apply X server environment variables. No
repo checkout in PRoot is needed.

For a **quick proof of path**, optionally enable `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1` and read
`XlorieCompositeGpu` in the native X server's `LorieNative` logcat. The
`src_over_done` counter should increase **only for successfully GPU-accelerated composites**.
This counter does not mean end-to-end zero-copy or higher FPS.

Observe both **new-content FPS and subjective tearing/stutter**, not the Android Vulkan
presented-frame count alone. On a black frame, wrong-colour alpha, stretching, dropped windows,
device loss or GPU hang, stop this experimental build; unset both flags and restart X11.

## Validation boundaries

Host fake-Vulkan tests check the GPU queue ownership, root completion, Src/Over pipeline blend
factors, source/destination region mapping, fallback and buffer release. Android CI verifies
APK/Companion compilation. These tests **cannot certify GPU/CPU pixel-order correctness on the
S24+'s Samsung Vulkan driver, nor that synchronous GPU work is faster than the original pixman
compositor**. A later stage must replace per-operation completion waits with an explicit
deferred-write / batch flush protocol before CPU-fence-free desktop zero-copy can be claimed.
