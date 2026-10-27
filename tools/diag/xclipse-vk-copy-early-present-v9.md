# Xclipse 940 / Termux-X11 v9: Present before CPU fence on copy-bearing frames

## Reason for this isolated change

Base: `experiment/xclipse-ahb-sync-v8` (`ae1d316`). This branch remains
independent of the official Termux-X11 EGL/GLES renderer. It builds upon our
VkImage-based Android Vulkan backend and AHB ownership protocol.

S24+ under One UI 9 showed that v8 STEP 2 improved vkmark to approximately
1180 FPS but **did not eliminate XFCE window dragging stutter**. During one
actual benchmark window the v8 Vulkan renderer reported 599 screen frames
and only 56 early Presents in five seconds; 543 Present GPU-copy entries
were carried by screen frames. Earlier frame-carried GPU fence waits were
typically 3-4 ms. These numbers concern the renderer, **not actual Android
display presentation timestamps**.

The v8 Vulkan path calls `vkQueuePresentKHR` *after* the CPU waited for
`vkWaitForFences` whenever the screen frame includes any queued Present
GPU-copy. Even though the frame's Vulkan render-finished semaphore already
guarantees that the presentation engine waits for all commands, this
repeats a synchronous CPU wait before the compositor is allowed to enqueue
the swapchain frame. This could miss the next 120 Hz presentation deadline.

## v9 opt-in

```sh
TERMUX_X11_RENDERER_VK_PRESENT_COPY_EARLY=1
```

Opt-in only; **defaults to OFF**. When ON, and a Vulkan screen frame
contains a Present-copy serial:

1. Record imported AHB GPU-copy commands, the root/cursor draw, and
   FOREIGN ownership releases in one Vulkan command buffer.
2. Submit it with the existing `rendered[index]` binary semaphore and
   frame fence. Publish the normal `gpuInFlight` AHB references.
3. Immediately enqueue `vkQueuePresentKHR` for the **swapchain image**,
   waiting on `rendered[index]` on the GPU. This does **not** wait for the
   AHB fence on the CPU before placing the present request.
4. Keep the in-flight list and every GPU-copy source/destination reference
   protected while the GPU frame executes. **Wait for the actual fence.**
5. Only once it has completed: remove the AHBs from `gpuInFlight`, publish
   `gpuCopyQueue.completedSerial`/wake the X server, and let its Present
   completion/retirement logic release the client's source buffer.

The choice to enqueue the screen present early is independent of v5's
`TERMUX_X11_RENDERER_VK_PRESENT_EARLY`, which applies to frames **without**
Present copies. Off mode preserves v8 behaviour for both frame classes.
The existing `VK_SUBMIT_OPT`, FIFO, root-twin, EXA and GLES paths are
unchanged.

A `VK_ERROR_OUT_OF_DATE_KHR` returned by the early Present does **not**
complete the imported AHB copies. Their completion still goes through the
frame fence before the X server is notified, and the swapchain is recreated
for a future frame. Fatal submit/fence/device errors still take the existing
teardown path.

This does **not** remove the CPU fence or guarantee that a particular
Android Vulkan WSI implementation returns immediately from
`vkQueuePresentKHR`. The CPU still waits to reclaim imported AHBs. It
tests whether removing a *pre-Present* wait reduces frame-deadline misses
and stuttering.

## S24+ controlled A/B (same APK; only one environment variable differs)

Install the matching v9 ARM64 experimental APK and its Termux Companion
from a successful branch CI build. The official Termux-X11 APK does **not**
have this Vulkan flag. Do **not** install or restart remotely while the
device's DevSpace connection is in use.

Preserve the previously measured best v8 STEP 2 settings:

```sh
export TERMUX_X11_RENDERER_BACKEND=vulkan
export TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1
export TERMUX_X11_EXA_LOCK_OPT=1
export TERMUX_X11_EXA_EARLY_RESERVE=1
export TERMUX_X11_EXA_EARLY_RESERVE_ROOT=0
export TERMUX_X11_RENDERER_VK_SUBMIT_OPT=0
export TERMUX_X11_RENDERER_ROOT_DOUBLE=0
export TERMUX_X11_ROOT_PRESENT_ASYNC=0
export TERMUX_X11_RENDERER_VK_FRAME_DIAG=1
export TX11_GPU_KEEPALIVE=0
```

First run (baseline, existing v8 presentation timing):

```sh
export TERMUX_X11_RENDERER_VK_PRESENT_COPY_EARLY=0
tx11start
```

Second run (opt-in v9 change only, after stopping the previous session
normally and allowing the device to cool):

```sh
export TERMUX_X11_RENDERER_VK_PRESENT_COPY_EARLY=1
tx11start
```

At each run, start the **same** short vkmark or Debian glmark2 scene in
the native XFCE desktop, then drag a Thunar or terminal window over it.
Keep the panel, compositor mode (do not re-test it), refresh rate, window
sizes, and temperature comparable. Compare the *perceived drag stutter*
and demo FPS **during the drag**, rather than only idle FPS. No continuous
GPU keepalive, CPU spin, Power HAL changes or Game Booster tweaks.

Verify the flag actually reaches the **X server** (not just the shell):

```sh
logcat -d -s LorieNative:I '*:S' |
  grep -E 'XlorieVkCopyPresent:|XlorieVkTiming:|XlorieCopyLatency:|XlorieFrameGaps:|XlorieTick:' |
  tail -n 24
```

The new line is:

```text
XlorieVkCopyPresent: enabled=on|off copy_frames=N(+d) copy_early_presents=N(+d) copy_frame_fence_wait_us=N(+d)
```

When the option is enabled, `copy_early_presents(+d)` should be greater
than zero **during the drag**; zero means the path was not exercised.
The old `XlorieVkTiming: early_presents` now includes the copy-bearing
ones, so compare it together with the new per-class counter.
The GPU copy's completion fence time is **still counted** by
`copy_frame_fence_wait_us`; the purpose is to move the Present request
ahead of that wait, not to hide it.

If the image glitches, Vulkan falls back, frames stall or drag is worse,
turn the new flag OFF and restart X11 using the otherwise identical
baseline command. Neither flag requires changing the APK to roll back.

## Validated locally on S26U source (host fake Vulkan)

Tests in `tpresentcopy` run **real extracted renderer source** with fake
Vulkan queue and semaphore APIs. They enforce:

* Baseline copy-bearing frames still wait before Present when OFF.
* ON: present calls occur while the frame's fence is pending, with a
  signalled-on-submit `rendered[index]` semaphore; GPU-copy serial
  notification is recorded **after** the present call and fence wait.
* All imported root/source/destination AHB references remain in-flight
  at the fence, are given back after completion, and CPU cannot receive
  a premature completion.
* An OUT_OF_DATE present does not prematurely complete or abandon the
  GPU copy, and the next swapchain recreation remains possible.

Host tests and a successful Android build validate functional wiring,
not device-specific DVFS or actual screen frame pacing. **Do not label
stuttering fixed until the S24+ A/B confirms it.**
