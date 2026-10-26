# P11-Xclipse Vulkan compositor: one screen frame per display tick (v6)

The user's S24+ (Xclipse 940, 1920x1080 desktop, 120Hz on-screen) had
**365 vkmark FPS** with the v5 Vulkan display backend, whereas GLES
compositing reached roughly 2606 FPS with the same vendor Vulkan client.
Setting MAILBOX and early Present did not solve this regression.

The actual S24+ X-server logs for one busy 5-second period showed:

| Counter | Five-second delta |
|---|---:|
| `XlorieTick: ticks` | 599 |
| `XlorieVk: frames_presented` | 885 |
| `XlorieCopyLatency: copy_frame_entries` | 494 |
| `XlorieVkTiming: fence_wait_us` | 2,948,603 us |
| `XlorieBackend: x_gpu_wait_us` | 2,063,356 us |

The large GPU-fence wait and X-server AHB CPU wait **overlap** in time;
do not add them together. Present GPU-copies took about 3.42ms average
GPU fence time in that interval. Present activity was about 1.48 times
the physical vsync tick rate, which is unnecessary for one scanout.
This is direct measurement, not a claim that every excess present
necessarily came from one source.

## Code path and hypothesis

`lorieRedraw` in `InitOutput.c` had TWO distinct ways to set
`waitForNextFrame=false` in one vblank tick:

1. *Before* `loriePerformVblanks()` on a Vulkan PER_ACCESS session
   without the experimental root twin. This must stay: Present vblank
   callbacks sometimes wait for their queued GPU-copy to be submitted.
   If the gate remains closed while such a callback runs, the renderer
   may never execute the copy and the X thread deadlocks.
2. *After* `loriePerformVblanks()`, unconditionally. If the renderer
   consumed the pre-vblank release, this could allow another full-screen
   frame before the next display tick. It also increases imported AHB
   acquire/release traffic and global Vulkan queue competition.

This branch adds **an experimental, OFF-by-default option**:

```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=1 \
TX11_GPU_KEEPALIVE=0 tx11start
```

Only in a PER_ACCESS Vulkan session with a single root buffer, the
second release is suppressed. If the renderer used the early release,
new screen damage waits for the next tick; if not, that release
remains available. The GPU Present-copy queue still runs via
`rendererVkApplyPendingGpuCopies` while no screen frame is allowed.
GLES sessions, root-double sessions, and the option-OFF legacy Vulkan
path retain existing behavior. No explicit AHB CPU/GPU fence is
removed; no changes to Vulkan swapchain or queue priorities.

## How to verify

Save open desktop programs before restarting `tx11start`. The Cohost
APK may restart Termux when upgraded; do NOT auto-install it from
DevSpace. With the v6 APK installed:

```sh
vkmark --winsys xcb -s 800x600 -p mailbox -b 'vertex:device-local=true'
logcat -d -s LorieNative:I '*:S' |
  grep -E 'XlorieVkTickGate:|XlorieVk:|XlorieCopyLatency:|XlorieBackend:' |
  tail -n 16
```

`XlorieVkTickGate` logs `enabled=on`, the count of prevented second
frame rearms, X-server tick count, and screen Present count with
five-second deltas. Compare during the same busy benchmark window.
The desired first-order result is fewer GPU screen frames per tick,
lower Vulkan GPU fence and X CPU wait costs, and **higher vkmark FPS**.
Also check that XFCE remains visually correct (especially Present,
cursor, moving/resizing windows, and fullscreen SuperTuxKart Vulkan).
No major content/flicker regression is acceptable just for a score.

Rollback without changing APK (requires restarting desktop):

```sh
TERMUX_X11_RENDERER_BACKEND=vulkan \
TERMUX_X11_RENDERER_VK_SINGLE_FRAME_PER_TICK=0 \
TX11_GPU_KEEPALIVE=0 tx11start
```

Host regression includes a model for both possible timing
interleavings (renderer consumes the first frame gate or does not);
strict Android C syntax and the normal Vulkan/Present-copy/ownership
suite must pass. Those tests are **not** proof of an on-device 5,000
FPS outcome. If Vulkan performance remains around 365 FPS, investigate
the single-root PER_ACCESS AHB contention itself, especially the
Present-copy and X-server CPU wait chains; root-double and deferred
damage are existing independently selectable candidates. Do not
replace correctness-critical fence waits with an unconditional skip.
