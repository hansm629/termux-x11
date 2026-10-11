#!/usr/bin/env python3
"""Negative controls: each breaks one rule of the code under test, in a copy of the sources, and the test
that guards that rule must then fail. A control whose text is no longer in the sources stops with an error
rather than passing, so a control cannot quietly stop testing anything.

usage: tcontrols.py <lorie source dir> <output dir> <3c98ffa lorie source dir>
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
L, OUT, BASE = sys.argv[1], sys.argv[2], sys.argv[3]
CC = os.environ.get("CC", "cc")

# (what breaks, file, text, replacement, test that must then fail)
CONTROLS = [
    ("the answer stored after the shared lock is let go", "InitOutput.c",
     """    __atomic_store_n(&pvfb->state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);
    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);""",
     """    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    __atomic_store_n(&pvfb->state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);""",
     "townership"),
    ("PERSISTENT does not lock the pixmaps again", "InitOutput.c",
     """            } else if (mode == LORIE_OWNERSHIP_PERSISTENT && !priv->locked)
                LorieBuffer_lock(priv->buffer, &priv->locked);""",
     """            }""",
     "townership"),
    ("PER_ACCESS keeps the pixmaps locked", "InitOutput.c",
     """            if (mode == LORIE_OWNERSHIP_PER_ACCESS && priv->locked) {""",
     """            if (0) {""",
     "townership"),
    ("PER_ACCESS FinishAccess lets the shared lock go before the CPU lock", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        if (!priv->wasLocked) {
            LorieBuffer_unlock(priv->buffer);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        return;
    }""",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (!priv->wasLocked) {
            LorieBuffer_unlock(priv->buffer);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        return;
    }""",
     "townership"),
    ("the PER_ACCESS FinishAccess path taken under PERSISTENT too", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        if (!priv->wasLocked) {""",
     """    if (lorieIsAhb(priv)) {
        if (!priv->wasLocked) {""",
     "townership"),
    ("a CPU access to an AHB under PER_ACCESS without the shared lock", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv))
        return TRUE;
""",
     "",
     "townership"),
    ("a failed PrepareAccess under PER_ACCESS keeps the shared lock", "InitOutput.c",
     """            if (perAccessAhb)
                lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
            return FALSE;""",
     """            return FALSE;""",
     "townership"),
    ("PER_ACCESS CPU accesses not counted", "InitOutput.c",
     """        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuAcquires, 1, __ATOMIC_RELAXED);""",
     "",
     "townership"),
    ("a session asked for gles granted PER_ACCESS", "InitOutput.c",
     """(mode != LORIE_OWNERSHIP_PER_ACCESS || pvfb->state->backend.requested == LORIE_BACKEND_GLES)""",
     """(mode != LORIE_OWNERSHIP_PER_ACCESS)""",
     "townership"),
    ("an answered request taken up again", "InitOutput.c",
     """    if (LORIE_OWNERSHIP_SEQ(req) == LORIE_OWNERSHIP_SEQ(ack))
        return;
""",
     "",
     "townership"),
    ("a new AHB pixmap locked whatever the mode", "InitOutput.c",
     """    if (!priv->locked && !(lorieIsAhb(priv) && lorieAhbPerAccess()))
        LorieBuffer_lock(priv->buffer, &priv->locked);""",
     """    if (!priv->locked)
        LorieBuffer_lock(priv->buffer, &priv->locked);""",
     "townership"),
    ("a pixmap made GPU-sampleable locked as 3c98ffa did, whatever the mode", "InitOutput.c",
     """            priv->locked = NULL;
            lorieHoldPersistent(priv);""",
     """            LorieBuffer_lock(priv->buffer, &priv->locked);""",
     "townership"),
    ("the root window's new pixmap not remembered", "InitOutput.c",
     """        if (new && new->buffer && !new->locked) {
            lorieHoldPersistent(new);""",
     """        if (new && new->buffer && !new->locked) {
            LorieBuffer_lock(new->buffer, &new->locked);""",
     "townership"),
    ("a destroyed pixmap left on the list", "InitOutput.c",
     """    LoriePixmapPriv *priv = driverPriv;
    if (priv->persistentWanted) {
        xorg_list_del(&priv->persistentLink);
        priv->persistentWanted = FALSE;
    }""",
     """    LoriePixmapPriv *priv = driverPriv;""",
     "townership"),
    ("drawSquares lets the shared lock go before the CPU lock", "InitOutput.c",
     """    if (perAccess) {
        if (pixels)
            LorieBuffer_unlock(priv->buffer);
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    }""",
     """    if (perAccess) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (pixels)
            LorieBuffer_unlock(priv->buffer);
    }""",
     "townership"),
    ("a lost connection does not reset the ownership", "InitOutput.c",
     """static Bool lorieRendererGoneWorkProc(__unused ClientPtr client, __unused void *closure) {
    lorieResetOwnership();""",
     """static Bool lorieRendererGoneWorkProc(__unused ClientPtr client, __unused void *closure) {""",
     "townership"),
    ("TERMUX_X11_RENDERER_BACKEND matched without case", "InitOutput.c",
     """    if (value && !strcmp(value, "vulkan"))""",
     """    if (value && !strcasecmp(value, "vulkan"))""",
     "townership"),
    ("TERMUX_X11_RENDERER_VK_FAIL_AT matched without case", "InitOutput.c",
     """        if (!strcmp(value, lorieVkFailAtNames[i]))""",
     """        if (!strcasecmp(value, lorieVkFailAtNames[i]))""",
     "townership"),
    ("TERMUX_X11_RENDERER_VK_FAIL_AT=none accepted, though it is no injection point", "InitOutput.c",
     """    for (i = LORIE_VK_FAIL_AT_NONE + 1; value && i < LORIE_VK_FAIL_AT_COUNT; i++)""",
     """    for (i = LORIE_VK_FAIL_AT_NONE; value && i < LORIE_VK_FAIL_AT_COUNT; i++)""",
     "townership"),
    ("VK_EXT_queue_family_foreign not required", "vk.c",
     """        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME };""",
     """        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME };""",
     "tvkinit"),
    ("a Vulkan 1.0 device accepted", "vk.c",
     """    if (props.apiVersion < VK_API_VERSION_1_1)
        return LORIE_FALLBACK_VK_VERSION;
""",
     "",
     "tvkinit"),
    ("the device counted before vkCreateDevice succeeds", "vk.c",
     """    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_DEVICE, LORIE_FALLBACK_VK_DEVICE))
        goto fail;""",
     """    LORIE_DIAG_ADD(st, vkDeviceCreated, 1);
    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_DEVICE, LORIE_FALLBACK_VK_DEVICE))
        goto fail;""",
     "tvkinit"),
    ("injected at instance, vkCreateInstance still called", "vk.c",
     """    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_INSTANCE, LORIE_FALLBACK_VK_INSTANCE))
        goto fail;""",
     """    lorieVkInjected(st, LORIE_VK_FAIL_AT_INSTANCE, LORIE_FALLBACK_VK_INSTANCE);""",
     "tvkinit"),
    ("injected at device, recorded as an ordinary failure", "vk.c",
     """    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_DEVICE, LORIE_FALLBACK_VK_DEVICE))
        goto fail;""",
     """    if (st && st->backend.vkFailAt == LORIE_VK_FAIL_AT_DEVICE) {
        lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE, VK_SUCCESS);
        goto fail;
    }""",
     "tvkinit"),
    ("the instance left behind when the device fails", "vk.c",
     """    if (vk->instance && vk->DestroyInstance)   // without it (a broken loader) the instance is left to the process' end
        vk->DestroyInstance(vk->instance, NULL);""",
     "",
     "tvkinit"),
    ("the device destroyed while it may still be busy", "vk.c",
     """    if (vk->device) {
        vk->DeviceWaitIdle(vk->device);
""",
     """    if (vk->device) {
""",
     "tvkinit"),
    ("the first device used whatever it lacks", "vk.c",
     """        if (!(reason = lorieVkDeviceUnsuitable(vk, devices[i], &family)))
            break;""",
     """        if (!(reason = lorieVkDeviceUnsuitable(vk, devices[i], &family)) || 1)
            break;""",
     "tvkinit"),
    ("an external-format buffer let through to the format check", "vk.c",
     """    if (formatProps.format == VK_FORMAT_UNDEFINED) {""",
     """    if (0) {""",
     "tvkimport"),
    ("the format query's importable bit not required", "vk.c",
     """            !(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||""",
     "",
     "tvkimport"),
    ("the buffer's size not checked against the format query", "vk.c",
     """            formatOut.imageFormatProperties.maxExtent.width < desc.width ||""",
     "",
     "tvkimport"),
    ("the first allowed memory type used, device-local or not", "vk.c",
     """        if ((props.memoryTypeBits & (1u << i)) && (memoryType == UINT32_MAX ||""",
     """        if ((props.memoryTypeBits & (1u << i)) && (memoryType == UINT32_MAX || 0 &&""",
     "tvkimport"),
    ("the import not a dedicated allocation", "vk.c",
     """                .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .pNext = &dedicated, .buffer = ahb };""",
     """                .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = ahb };""",
     "tvkimport"),
    ("RGBX alpha sampled as stored", "vk.c",
     """                                img.rgbx ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY },""",
     """                                VK_COMPONENT_SWIZZLE_IDENTITY },""",
     "tvkimport"),
    ("an attachment view for a buffer that cannot be rendered to", "vk.c",
     """        if ((img.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&""",
     """        if (1 &&""",
     "tvkimport"),
    ("a failed import leaving its objects behind", "vk.c",
     """    fail:
    lorieVkImageDestroy(vk, &img);
    lorieVkImportFailed(images, st, reason, r, id, &desc);""",
     """    fail:
    lorieVkImportFailed(images, st, reason, r, id, &desc);""",
     "tvkimport"),
    ("an import counted as a success before it is one", "vk.c",
     """    LORIE_DIAG_ADD(st, vkAhbImportAttempts, 1);
    AHardwareBuffer_describe(ahb, &desc);""",
     """    LORIE_DIAG_ADD(st, vkAhbImportAttempts, 1);
    LORIE_DIAG_ADD(st, vkAhbImportSuccess, 1);
    AHardwareBuffer_describe(ahb, &desc);""",
     "tvkimport"),
    ("injected at import, the buffer's properties still asked for", "vk.c",
     """    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_IMPORT, LORIE_FALLBACK_AHB_IMPORT)) {
        lorieVkImportFailed(images, st, LORIE_FALLBACK_NONE, VK_SUCCESS, id, &desc);
        return NULL;
    }""",
     """    if (lorieVkInjected(st, LORIE_VK_FAIL_AT_IMPORT, LORIE_FALLBACK_AHB_IMPORT)) {
        VkAndroidHardwareBufferPropertiesANDROID unused = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
        vk->GetAndroidHardwareBufferPropertiesANDROID(vk->device, ahb, &unused);
        lorieVkImportFailed(images, st, LORIE_FALLBACK_NONE, VK_SUCCESS, id, &desc);
        return NULL;
    }""",
     "tvkimport"),
    ("an acquire from VK_IMAGE_LAYOUT_UNDEFINED", "vk.c",
     """    b.barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;""",
     """    b.barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;""",
     "tvkbarrier"),
    ("an acquire from VK_QUEUE_FAMILY_EXTERNAL", "vk.c",
     """    b.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;""",
     """    b.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;""",
     "tvkbarrier"),
    ("a release to VK_QUEUE_FAMILY_EXTERNAL", "vk.c",
     """    b.barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;""",
     """    b.barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;""",
     "tvkbarrier"),
    ("a release leaving the image in the layout of its use", "vk.c",
     """    b.barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;""",
     """    b.barrier.newLayout = b.barrier.oldLayout;""",
     "tvkbarrier"),
    ("a release not making the writes available", "vk.c",
     """    b.barrier.dstAccessMask = 0;
    b.barrier.srcQueueFamilyIndex = vk->queueFamily;""",
     """    b.barrier.dstAccessMask = 0;
    b.barrier.srcAccessMask = 0;
    b.barrier.srcQueueFamilyIndex = vk->queueFamily;""",
     "tvkbarrier"),
    ("an acquire with a source access mask", "vk.c",
     """    b.barrier.srcAccessMask = 0;
    b.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;""",
     """    b.barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;""",
     "tvkbarrier"),
    ("rendering that loads the attachment without reading it", "vk.c",
     """            *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;""",
     """            *access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;""",
     "tvkbarrier"),
    ("the self-test comparing no alpha byte, RGBA's included", "vk.c",
     """                if ((alphaDefined || i % 4 != 3) && got[y * rowBytes + i] != expect[y * w + i])""",
     """                if (i % 4 != 3 && got[y * rowBytes + i] != expect[y * w + i])""",
     "tvkselftest"),
    ("the self-test comparing RGBX's undefined X byte", "vk.c",
     """                if ((alphaDefined || i % 4 != 3) && got[y * rowBytes + i] != expect[y * w + i])""",
     """                if (got[y * rowBytes + i] != expect[y * w + i])""",
     "tvkselftest"),
    ("an import of a format the self-test did not verify", "vk.c",
     """    if (!(vk->verifiedFormats & (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ? 1 :""",
     """    if (0 && !(vk->verifiedFormats & (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ? 1 :""",
     "tvkimport"),
    ("the self-test counting a compare before it passed", "vk.c",
     """            if ((step = lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, lorieVkSelftestFormats[f].alphaDefined, &diff, &got)))
                goto fail;
            LORIE_DIAG_ADD(st, vkSelftestCompares, 1);""",
     """            LORIE_DIAG_ADD(st, vkSelftestCompares, 1);
            if ((step = lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, lorieVkSelftestFormats[f].alphaDefined, &diff, &got)))
                goto fail;""",
     "tvkselftest"),
    ("the self-test ignoring a mismatch", "vk.c",
     """            if ((step = lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, lorieVkSelftestFormats[f].alphaDefined, &diff, &got)))
                goto fail;""",
     """            lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, lorieVkSelftestFormats[f].alphaDefined, &diff, &got);""",
     "tvkselftest"),
    ("the self-test writing the same pattern every iteration", "vk.c",
     """            lorieVkSelftestPattern(t.expect, it % 5, it);""",
     """            lorieVkSelftestPattern(t.expect, 1, 0);""",
     "tvkselftest"),
    ("the self-test reading before the fence was waited for", "vk.c",
     """    if ((r = vk->WaitForFences(vk->device, 1, &t->fence, VK_TRUE, LORIE_VK_SELFTEST_TIMEOUT_NS)) != VK_SUCCESS)
        return r;""",
     "",
     "tvkselftest"),
    ("the self-test not giving the copy's destination back", "vk.c",
     """            lorieVkCmdBarrier(vk, t.cmd, lorieVkReleaseBarrier(vk, img[1]->image, LORIE_VK_USE_TRANSFER_DST));""",
     "",
     "tvkselftest"),
    ("the self-test reading a download not made visible to the host", "vk.c",
     """                                                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT,""",
     """                                                              .dstAccessMask = 0,""",
     "tvkselftest"),
    ("an acquire from UNDEFINED, run through the self-test", "vk.c",
     """    b.barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;""",
     """    b.barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;""",
     "tvkselftest"),
    ("RGBX allowed to be unavailable", "vk.c",
     """        { AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 2, true, false },""",
     """        { AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 2, false, false },""",
     "tvkselftest"),
    ("a frame acquiring a buffer at each use", "vk.c",
     """        if (r->used[i].img == img) {""",
     """        if (0) {""",
     "tvkframe"),
    ("a frame not giving its buffers back", "vk.c",
     """    for (i = 0; i < r->usedCount; i++)
        lorieVkCmdBarrier(vk, r->cmd, lorieVkReleaseBarrier(vk, r->used[i].img->image, r->used[i].use));""",
     "",
     "tvkframe"),
    ("a frame counted before its submission", "vk.c",
     """    start = lorieVkNowUs();
    if ((res = vk->QueueSubmit(vk->queue, 1, &submit, r->frameFence)) != VK_SUCCESS) {""",
     """    LORIE_DIAG_ADD(st, vkQueueSubmits, 1);
    start = lorieVkNowUs();
    if ((res = vk->QueueSubmit(vk->queue, 1, &submit, r->frameFence)) != VK_SUCCESS) {""",
     "tvkframe"),
    ("a late fence not waited out", "vk.c",
     """        vk->DeviceWaitIdle(vk->device);
        vk->ResetFences(vk->device, 1, &r->frameFence);
        return res;""",
     """        return res;""",
     "tvkframe"),
    ("the acquire fence not reset", "vk.c",
     """                (wait = vk->ResetFences(vk->device, 1, &r->acquireFence)) != VK_SUCCESS) {""",
     """                (wait = VK_SUCCESS) != VK_SUCCESS) {""",
     "tvkframe"),
    ("a swapchain in MAILBOX", "vk.c",
     """                .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE, .oldSwapchain = old };""",
     """                .presentMode = VK_PRESENT_MODE_MAILBOX_KHR, .clipped = VK_TRUE, .oldSwapchain = old };""",
     "tvkframe"),
    ("the surface's first format taken", "vk.c",
     """    for (i = 0; i < count && format != VK_FORMAT_R8G8B8A8_UNORM; i++)""",
     """    for (i = 0; i < count && format == VK_FORMAT_UNDEFINED; i++)""",
     "tvkframe"),
    ("a pre-rotated swapchain", "vk.c",
     """                .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?""",
     """                .preTransform = 0 ?""",
     "tvkframe"),
    ("a copy pass loading from UNDEFINED", "vk.c",
     """            .initialLayout = screen ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,""",
     """            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,""",
     "tvkframe"),
    ("a copy rendering into a buffer left in its sampling layout", "vk.c",
     """!lorieVkFrameUse(vk, r, dst, LORIE_VK_USE_RENDER)""",
     """!lorieVkFrameUse(vk, r, dst, LORIE_VK_USE_SAMPLE)""",
     "tvkframe"),
    ("the root sampled in the layout a copy left it in", "vk.c",
     """    if (root && lorieVkFrameUse(vk, r, root, LORIE_VK_USE_SAMPLE))""",
     """    if (root)""",
     "tvkframe"),
    ("the surface destroyed before its swapchain", "vk.c",
     """    if (r->swapchain)
        vk->DestroySwapchainKHR(vk->device, r->swapchain, NULL);
    lorieVkScreenPassDestroy(vk, r);
    if (r->surface)
        vk->DestroySurfaceKHR(vk->instance, r->surface, NULL);""",
     """    if (r->surface)
        vk->DestroySurfaceKHR(vk->instance, r->surface, NULL);
    if (r->swapchain)
        vk->DestroySwapchainKHR(vk->device, r->swapchain, NULL);
    lorieVkScreenPassDestroy(vk, r);""",
     "tvkframe"),
    ("a frame not checking the X server's answer under its lock", "renderer.c",
     """    owned = rendererVkOwnedLocked();""",
     """    owned = true;""",
     "twiring"),
    ("a session that asked for Vulkan only falling back to GLES", "renderer.c",
     """    return requested == LORIE_BACKEND_VULKAN ? RENDERER_USE_NONE : RENDERER_USE_GLES;""",
     """    return RENDERER_USE_GLES;""",
     "tvkrenderer"),
    ("an unanswered ownership request taken as granted", "renderer.c",
     """    if (LORIE_OWNERSHIP_SEQ(ack) != seq || LORIE_OWNERSHIP_MODE(ack) != mode) {""",
     """    if (0) {""",
     "tvkrenderer"),
    ("a window let go of before its swapchain", "renderer.c",
     """    if (vkActive) {
        lorieVkSwapchainDestroy(&vk, &vkr);
        vkActive = false;
    }
    releaseWinAndSurface(&win, &sfc);""",
     """    vkActive = false;
    releaseWinAndSurface(&win, &sfc);""",
     "twiring"),
    ("a Vulkan failure not torn down", "renderer.c",
     """    vkFailed = true;
    rendererVkTeardown();
    if (state) {""",
     """    vkFailed = true;
    if (state) {""",
     "twiring"),
    ("the last session's imported images kept for the next", "renderer.c",
     """                vk.DeviceWaitIdle(vk.device);
                lorieVkImagesClear(&vk, &vkImages);
            }
            vkOwned = false;""",
     """            }
            vkOwned = false;""",
     "twiring"),
    ("the last session's ownership answer kept for the next", "renderer.c",
     """            }
            vkOwned = false;

            if (state) {""",
     """            }

            if (state) {""",
     "twiring"),
    ("lorieRedraw without the ownership request", "InitOutput.c",
     """
    lorieProcessOwnershipRequest();
""",
     "\n",
     "twiring"),
    ("a new connection without the reset", "InitOutput.c",
     """    lorieResetOwnership();      // a new renderer starts on GLES, with 3c98ffa's ownership
""",
     "",
     "twiring"),
    ("a lost connection not reported", "cmdentrypoint.c",
     """        lorieRendererDisconnected();    // AHB ownership back to 3c98ffa's (InitOutput.c)
""",
     "",
     "twiring"),
]

def copy_sources(dst):
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(L):
        p = os.path.join(L, f)
        if os.path.isfile(p) and not os.path.islink(p) and f.endswith((".c", ".h")):
            shutil.copyfile(p, os.path.join(dst, f))

fails = 0
for n, (what, file, old, new, test) in enumerate(CONTROLS):
    work = os.path.join(OUT, "controls", str(n))
    src, gen = os.path.join(work, "lorie"), os.path.join(work, "gen")
    shutil.rmtree(work, ignore_errors=True)
    copy_sources(src)
    os.makedirs(gen)
    path = os.path.join(src, file)
    text = open(path, encoding="utf-8").read()
    if text.count(old) != 1:
        sys.exit("control %r: its text is in %s %d times, not once - update the control" % (what, file, text.count(old)))
    open(path, "w", encoding="utf-8").write(text.replace(old, new))

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", OUT=gen)
    if test == "twiring":
        r = subprocess.run([sys.executable, os.path.join(HERE, "twiring.py"), src], capture_output=True, text=True, env=env)
    else:
        subprocess.run([sys.executable, os.path.join(HERE, "gen.py"), src, gen, BASE], check=True, env=env)
        exe = os.path.join(gen, test)
        b = subprocess.run([CC, "-Wall", "-Wno-unused-function", "-O1", "-pthread", "-I" + gen,
                            "-I" + os.path.join(OUT, "vkinc"), "-I" + HERE,
                            os.path.join(HERE, test + ".c"), "-o", exe, "-lm"], capture_output=True, text=True)
        if b.returncode != 0:
            fails += 1
            print("  FAIL control %r: does not build, so it shows nothing\n%s" % (what, b.stderr[-2000:]))
            continue
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
    if r.returncode == 0:
        fails += 1
        print("  FAIL control %r: %s still passes" % (what, test))
    else:
        first = next((l.strip() for l in r.stdout.splitlines() if l.strip().startswith("FAIL")), "(no FAIL line)")
        print("  control %d, %s: %s" % (n, what, first[:220]))
    shutil.rmtree(work, ignore_errors=True)

print("negative controls (%d, each must fail its test): %s (%d not detected)" % (
    len(CONTROLS), "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
