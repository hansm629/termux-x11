# Termux-X11 Vulkan WSI A/B — Xclipse 940, v5

## Why this branch exists

On the user's S24+ (Exynos 2400 / Xclipse 940, Android 17) the same
`vkmark -b 'vertex:device-local=true'` ran at roughly 348–383 FPS with
Termux-X11's Vulkan display compositor, compared with 2,606 FPS on the
GLES compositor. The driver used by the benchmark itself was the same
Samsung Xclipse 940 Vulkan driver. The result strongly implicates GPU
contention and/or presentation behavior **inside the display compositor**
but does not by itself prove the exact Vulkan WSI suboperation.

The previous Vulkan compositor:

1. Preferred **IMMEDIATE** over MAILBOX when both were advertised, even
   though the benchmark uses MAILBOX.
2. Queued its frame, waited for the GPU's CPU-visible frame fence, and
   then called `vkQueuePresentKHR`, serializing the display presentation
   path. An earlier experiment, `VK_SUBMIT_OPT=1`, allowed earlier
   presentation but also changed the ordering of GPU copy submissions.

This test isolates the two independent changes without enabling
unrelated GPU-copy optimizations.

## v5 behavior (only on Vulkan display backend)

- **Default MAILBOX** if available (fall back to IMMEDIATE, then FIFO).
  Smooth presentation continues to force FIFO.
- **Early Present for frames without pending GPU Present-copy**: submit
  the frame, call `vkQueuePresentKHR` immediately, then wait on the
  already-required frame fence before releasing the X11-owned AHBs.
  Vulkan's render-finished semaphore orders the display's read after
  GPU drawing. Frames with a pending copy still publish its completion
  before Present. GPU buffers are **not** shared/released early.
- Restore previous mode with:
  `TERMUX_X11_RENDERER_VK_PRESENT_IMMEDIATE=1`.
- Independently disable early Present with:
  `TERMUX_X11_RENDERER_VK_PRESENT_EARLY=0`.

The server's 5-second `XlorieBackend` log includes
`vk_present_pref=mailbox|immediate` and
`vk_present_early=on|off`. The `XlorieVkTiming` line reports the
**actual** swapchain Present Mode (`present_mode="..."`) and counts
`early_presents`. Other experimental optimizations stay opt-in.

## Test procedure

**Installation is a user operation**: the S24+ uses the Cohost build,
and replacing its APK may restart Termux and break DevSpace. Save open
work before restarting the X11 desktop. Do not mix in external
`gpu_keepalive` during this WSI A/B.

Use the same 800×600 vkmark scene, device temperature, and output mode
each time. The benchmark below exercises the client Vulkan path, not
the Vulkan display compositor's own FPS:

```sh
vkmark --winsys xcb -s 800x600 -p mailbox -b 'vertex:device-local=true'
logcat -d -s LorieNative:I '*:S' | grep -E 'XlorieBackend:|XlorieVkTiming:' | tail -n 6
cat /sys/kernel/gpu/gpu_clock /sys/kernel/gpu/gpu_busy /sys/kernel/gpu/gpu_tmu
```

The `TERMUX_X11_RENDERER_*` environment variables are read by the X
server on launch, so changing them requires a **desktop restart**:

| Trial | X11 backend | Present preference | Early Present |
|---|---|---|---|
| Reference | `gles` | N/A | N/A |
| A — previous Vulkan | `vulkan` | `IMMEDIATE=1` | `EARLY=0` |
| B — only MAILBOX | `vulkan` | default MAILBOX | `EARLY=0` |
| C — only early Present | `vulkan` | `IMMEDIATE=1` | default early |
| D — v5 default | `vulkan` | default MAILBOX | default early |

For D:
```sh
TERMUX_X11_RENDERER_BACKEND=vulkan TX11_GPU_KEEPALIVE=0 tx11start
```

For B:
```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
  TERMUX_X11_RENDERER_VK_PRESENT_EARLY=0 TX11_GPU_KEEPALIVE=0 tx11start
```

For A:
```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
  TERMUX_X11_RENDERER_VK_PRESENT_IMMEDIATE=1 \
  TERMUX_X11_RENDERER_VK_PRESENT_EARLY=0 \
  TX11_GPU_KEEPALIVE=0 tx11start
```

If the default Vulkan score remains below GLES, do **not** claim the
regression is fixed. Next capture `TERMUX_X11_RENDERER_VK_FRAME_DIAG=1`
to separate the acquisition, submission, fence, and Present costs.
Do not remove the frame fence without a correct deferred AHB ownership
and buffer lifetime mechanism.

This branch has host tests for default / legacy WSI modes, safe
early-presenting of no-copy frames, mandatory copy completion ordering
and the frame fence. Android CI build success is not equivalent to
an actual S24+ FPS or frame pacing improvement.
