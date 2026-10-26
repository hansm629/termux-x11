/*
 * Screen-frame rearm policy for Vulkan PER_ACCESS (single-buffer root).
 *
 * The X server releases its frame gate before Present vblanks so the renderer
 * can service GPU copies queued from callbacks that may wait for them. A
 * second unconditional release after the vblanks can allow a second Vulkan
 * full-screen draw in the same display tick, despite no extra scanout.
 *
 * When selected, retain the pre-vblank release but suppress the second one.
 * An unconsumed release remains open; if the renderer already used it, the
 * new root damage is displayed on the next tick. Pending GPU-copy work still
 * runs without a screen frame via rendererVkApplyPendingGpuCopies().
 */
#pragma once
#include <stdbool.h>

static inline bool lorieFrameGateRearmAfterVblanks(bool singleFramePerTick,
                                                  bool preVblankVulkanRelease) {
    return !(singleFramePerTick && preVblankVulkanRelease);
}
