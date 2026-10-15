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
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        return;
    }""",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (!priv->wasLocked) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
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
        if (pixels) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    }""",
     """    if (perAccess) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (pixels) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
        }
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
     """                (externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&""",
     "",
     "tvkimport"),
    ("the buffer's size not checked against the format query", "vk.c",
     """                formatOut.imageFormatProperties.maxExtent.width >= desc.width &&""",
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
     """    }
    lorieVkImageDestroy(vk, &img);
    lorieVkImportFailed(images, st, reason, r, id, &desc);""",
     """    }
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
     """        idle = vk->DeviceWaitIdle(vk->device);""",
     """        idle = VK_SUCCESS;""",
     "tvkframe"),
    ("the acquire fence not reset", "vk.c",
     """                (wait = vk->ResetFences(vk->device, 1, &r->acquireFence)) != VK_SUCCESS) {""",
     """                (wait = VK_SUCCESS) != VK_SUCCESS) {""",
     "tvkframe"),
    ("a swapchain in MAILBOX", "vk.c",
     """                .presentMode = presentMode, .clipped = VK_TRUE, .oldSwapchain = old };""",
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
    ("a suboptimal acquire making the swapchain again", "vk.c",
     """            if (res == VK_SUBOPTIMAL_KHR)
                LORIE_DIAG_ADD(st, vkAcquireSuboptimal, 1);
            else""",
     """            if (res == VK_SUBOPTIMAL_KHR) {
                LORIE_DIAG_ADD(st, vkAcquireSuboptimal, 1);
                r->recreate = true;
            } else""",
     "tvkframe"),
    ("a suboptimal present making the swapchain again", "vk.c",
     """        case VK_SUBOPTIMAL_KHR:     // presented: nothing to make again (see LORIE_VK_MAX_OUT_OF_DATE)""",
     """        case VK_SUBOPTIMAL_KHR:     r->recreate = true; /* fallthrough */""",
     "tvkframe"),
    ("out of date made again for ever", "vk.c",
     """    if (++r->outOfDateStreak > LORIE_VK_MAX_OUT_OF_DATE) {""",
     """    if (0) {""",
     "tvkframe"),
    ("a presented frame not starting the out-of-date count over", "vk.c",
     """            r->outOfDateStreak = 0;
""",
     "",
     "tvkframe"),
    ("the window's size taken over the surface's currentExtent", "vk.c",
     """    if (caps.currentExtent.width != UINT32_MAX && caps.currentExtent.height != UINT32_MAX)""",
     """    if (0)""",
     "tvkframe"),
    ("a frame comparing the window's size with the swapchain's", "renderer.c",
     """    if (vkr.recreate && !lorieVkSwapchainCreate(&vk, &vkr, win, state))""",
     """    if ((vkr.recreate || vkr.extent.width != (uint32_t) ANativeWindow_getWidth(win)) && !lorieVkSwapchainCreate(&vk, &vkr, win, state))""",
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
    ("a copy's y as GLES computes it before its framebuffer flip", "renderer.c",
     """2.f * (float) (r.y1 + yOff) / (float) dst->height - 1.f,""",
     """1.f - 2.f * (float) (r.y1 + yOff) / (float) dst->height,""",
     "tpresentcopy"),
    ("a copy's offset left out", "renderer.c",
     """2.f * (float) (r.x1 + xOff) / (float) dst->width - 1.f,""",
     """2.f * (float) r.x1 / (float) dst->width - 1.f,""",
     "tpresentcopy"),
    ("a copy's texture coordinates over the destination's size", "renderer.c",
     """(float) r.x1 / (float) src->width,""",
     """(float) r.x1 / (float) dst->width,""",
     "tpresentcopy"),
    ("red and blue swapped when the two formats agree", "renderer.c",
     """entry.xOff, entry.yOff, sd, dd, s->rgbaOrder != d->rgbaOrder);""",
     """entry.xOff, entry.yOff, sd, dd, s->rgbaOrder == d->rgbaOrder);""",
     "tpresentcopy"),
    ("a copy drawn from its destination into its source", "renderer.c",
     """if (!lorieVkFrameCopy(&vk, &vkr, s, d, quads, n, filter)) {""",
     """if (!lorieVkFrameCopy(&vk, &vkr, d, s, quads, n, filter)) {""",
     "tpresentcopy"),
    ("only a copy's first rectangle drawn", "renderer.c",
     """uint32_t n = entry.numRects < LORIE_GPU_COPY_MAX_RECTS ? entry.numRects : LORIE_GPU_COPY_MAX_RECTS, k, count;""",
     """uint32_t n = 1, k, count;""",
     "tpresentcopy"),
    ("the copies alone published though their submission failed", "renderer.c",
     """            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
    }
    if (res == VK_SUCCESS) {
        // Only now that the device finished (and gave the buffers back) may the X server reuse the source.
        uint64_t queued[LORIE_GPU_COPY_QUEUE_CAPACITY], publishedUs;
        uint32_t n = rendererVkQueuedTimes(readIndex, queued);""",
     """            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
    }
    if (1) {
        // Only now that the device finished (and gave the buffers back) may the X server reuse the source.
        uint64_t queued[LORIE_GPU_COPY_QUEUE_CAPACITY], publishedUs;
        uint32_t n = rendererVkQueuedTimes(readIndex, queued);""",
     "tpresentcopy"),
    ("the queue not advanced after the copies alone", "renderer.c",
     """        uint32_t n = rendererVkQueuedTimes(readIndex, queued);
        state->gpuCopyQueue.readIndex = readIndex;""",
     """        uint32_t n = rendererVkQueuedTimes(readIndex, queued);""",
     "tpresentcopy"),
    ("a frame drawing the root without the copies queued", "renderer.c",
     """        if (!rendererVkCopiesLocked(&serial, &readIndex) ||
            !(root = lorieVkImport(""",
     """        if (!(root = lorieVkImport(""",
     "tpresentcopy"),
    ("a frame moving the queue though it made no copy", "renderer.c",
     """        if (owned)
            state->gpuCopyQueue.readIndex = readIndex;""",
     """        state->gpuCopyQueue.readIndex = readIndex;""",
     "tpresentcopy"),
    ("the copies alone made without the X server's PER_ACCESS answer", "renderer.c",
     """    if (!rendererVkOwnedLocked()) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        vkOwned = false;""",
     """    if (0) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        vkOwned = false;""",
     "tpresentcopy"),
    ("the X server's queue taken as full one entry late", "InitOutput.c",
     """    if (writeIndex - readIndex >= LORIE_GPU_COPY_QUEUE_CAPACITY) {""",
     """    if (writeIndex - readIndex > LORIE_GPU_COPY_QUEUE_CAPACITY) {""",
     "tpresentcopy"),
    ("a copy queued with no surface to draw it", "InitOutput.c",
     """    if (!lorieConnectionAlive() || !lorieRendererAvailable()) {""",
     """    if (!lorieConnectionAlive()) {""",
     "tpresentcopy"),
    ("more rectangles queued than an entry holds", "InitOutput.c",
     """    if (numRects <= 0 || numRects > LORIE_GPU_COPY_MAX_RECTS) {""",
     """    if (numRects <= 0 || numRects > LORIE_GPU_COPY_MAX_RECTS + 1) {""",
     "tpresentcopy"),
    ("the acknowledgement keeping the source's reference", "InitOutput.c",
     """    if (priv && priv->buffer)
        LorieBuffer_release(priv->buffer);""",
     "",
     "tpresentcopy"),
    ("the self-test's duration from unsigned parts", "vk.c",
     """    int64_t ns = ((int64_t) t1->tv_sec - (int64_t) t0->tv_sec) * 1000000000 + ((int64_t) t1->tv_nsec - (int64_t) t0->tv_nsec);
    return ns > 0 ? (uint64_t) ns / 1000 : 0;""",
     """    return (uint64_t) (t1->tv_sec - t0->tv_sec) * 1000000 + (uint64_t) (t1->tv_nsec - t0->tv_nsec) / 1000;""",
     "tvkselftest"),
    ("a failed self-test's duration taken as before", "vk.c",
     """        st->diag.vkSelftestUs = lorieVkElapsedUs(&t0, &t1);
        st->diag.vkSelftestFailStep = step;""",
     """        st->diag.vkSelftestUs = (uint64_t) (t1.tv_sec - t0.tv_sec) * 1000000 + (uint64_t) (t1.tv_nsec - t0.tv_nsec) / 1000;
        st->diag.vkSelftestFailStep = step;""",
     "twiring"),
    ("a buffer given its GLES texture when it comes, though Vulkan draws", "renderer.c",
     """            if (rendererGlesEager())
                rendererAttachToGL(buf);""",
     """            rendererAttachToGL(buf);""",
     "twiring"),
    ("a buffer's GLES texture made again at each use", "renderer.c",
     """    if (buf && !LorieBuffer_getGLTextureId(buf))
        rendererAttachToGL(buf);""",
     """    if (buf)
        rendererAttachToGL(buf);""",
     "tpresentcopy"),
    ("GLES textures made when buffers come, though Vulkan may still be started", "renderer.c",
     """    return !vkActive && (!state || rendererBackendFor(state->backend.requested, vkFailed) == RENDERER_USE_GLES);""",
     """    return !vkActive;""",
     "tvkrenderer"),
    ("a frame's fence waited for holding the X server's lock", "renderer.c",
     """    state->waitForNextFrame = true;
    lorie_mutex_unlock(&state->lock, &state->lockingPid);

    if (res == VK_SUCCESS) {
        // The device runs the frame; the X server goes on, without the frame's buffers (rendererVkInFlight).
        bool over;
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
    }""",
     """    state->waitForNextFrame = true;

    if (res == VK_SUCCESS) {
        // The device runs the frame; the X server goes on, without the frame's buffers (rendererVkInFlight).
        bool over;
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
    }
    lorie_mutex_unlock(&state->lock, &state->lockingPid);""",
     "tpresentcopy"),
    ("a frame's buffers not listed in flight", "renderer.c",
     """    if (res == VK_SUCCESS)
        rendererVkInFlight();
    state->waitForNextFrame = true;""",
     """    state->waitForNextFrame = true;""",
     "tpresentcopy"),
    ("a frame's buffers never given back to the X server", "renderer.c",
     """state->gpuInFlight.pid == getpid() && __atomic_exchange_n(&state->gpuInFlight.busy, 0, __ATOMIC_RELEASE))""",
     """state->gpuInFlight.pid == getpid() && 0 && __atomic_exchange_n(&state->gpuInFlight.busy, 0, __ATOMIC_RELEASE))""",
     "tpresentcopy"),
    ("an access CPU-locking an AHB a submitted Vulkan frame uses", "InitOutput.c",
     """            lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
            __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, GetTimeInMicros() - gpuStartUs, __ATOMIC_RELAXED);""",
     """            __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, GetTimeInMicros() - gpuStartUs, __ATOMIC_RELAXED);""",
     "townership"),
    ("drawSquares into a root a submitted Vulkan frame uses", "InitOutput.c",
     """        lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
        lockStartUs = GetTimeInMicros();""",
     """        lockStartUs = GetTimeInMicros();""",
     "townership"),
    ("PERSISTENT taken again while a Vulkan frame may still run", "InitOutput.c",
     """        if (mode == LORIE_OWNERSHIP_PERSISTENT)
            lorieGpuWaitIdle(pvfb->state, true, 0);
""",
     "",
     "townership"),
    ("the wait for a gone submitter leaving its list set", "lorie.h",
     """            lorieGpuSubmitterGone(st))
            __atomic_store_n(&st->gpuInFlight.busy, 0, __ATOMIC_RELEASE);""",
     """            lorieGpuSubmitterGone(st))
            (void) 0;""",
     "townership"),
    ("AHB id 0 taken as every AHB", "lorie.h",
     """            return true;
    return false;
}

// Whether any AHB is.""",
     """            return true;
    return !id;
}

// Whether any AHB is.""",
     "townership"),
    ("a slice without an answer taken as the frame over", "lorie.h",
     """            lorieGpuSubmitterGone(st))""",
     """            (lorieGpuSubmitterGone(st) || 1))""",
     "townership"),
    ("vkDeviceWaitIdle out of memory taken as the frame over", "vk.c",
     """        *over = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST || res == VK_ERROR_DEVICE_LOST;""",
     """        *over = true;""",
     "tpresentcopy"),
    ("a lost device's frame kept as not over", "vk.c",
     """        *over = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST || res == VK_ERROR_DEVICE_LOST;""",
     """        *over = idle == VK_SUCCESS;""",
     "tpresentcopy"),
    ("teardown not asking vkDeviceWaitIdle again", "renderer.c",
     """    while ((r = vk.DeviceWaitIdle(vk.device)) != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) {""",
     """    if ((r = vk.DeviceWaitIdle(vk.device)) != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) {""",
     "tpresentcopy"),
    ("a frame recorded over another process's listed frame", "renderer.c",
     """        inflightUs = lorieMonotonicUs();
        lorieGpuWaitIdle(state, true, 0);""",
     """        inflightUs = lorieMonotonicUs();""",
     "tpresentcopy"),
    ("this process clearing another's list", "renderer.c",
     """    if (state && state->gpuInFlight.pid == getpid() && __atomic_exchange_n""",
     """    if (state && __atomic_exchange_n""",
     "tpresentcopy"),
    ("Vulkan's tick frame allowed only after the vblanks queued their copies", "InitOutput.c",
     """    if (lorieAhbPerAccess() && !twinned)
        pvfb->state->waitForNextFrame = false;
    loriePerformVblanks();""",
     """    loriePerformVblanks();""",
     "twiring"),
    ("FIFO copies held up by the frame's swapchain image", "renderer.c",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        rendererVkApplyPendingGpuCopies();""",
     """    if (0) {
        rendererVkApplyPendingGpuCopies();""",
     "tpresentcopy"),
    ("copies in a submission of their own whatever the present mode", "renderer.c",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
        rendererVkApplyPendingGpuCopies();""",
     """    if (1) {
        rendererVkApplyPendingGpuCopies();""",
     "tpresentcopy"),
    ("the root swap writing the front while a frame samples it", "InitOutput.c",
     """    lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(front)->id);     // the last frame sampling it
""",
     "",
     "townership"),
    ("a root gone under the lock, the old one drawn", "renderer.c",
     """        } else {
            // Not one the renderer has yet, or of another size: neither it nor the one looked up before is sampled.""",
     """        } else if (0) {
            // Not one the renderer has yet, or of another size: neither it nor the one looked up before is sampled.""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: an RGBX source copied as is into alpha", "renderer.c",
     """    if (s->rgbx && !d->rgbx)
        why |= 1u << LORIE_VK_XFER_NOT_RGBX_ALPHA;
    if (s->rgbx && d->rgbx && entry->dstDepth >= 32)
        why |= 1u << LORIE_VK_XFER_NOT_DEPTH32;""",
     "",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: a copy into an image a copy wrote before not ordered after it", "vk.c",
     """        if (r->used[i].img == dst && r->used[i].use == LORIE_VK_USE_TRANSFER_DST)""",
     """        if (0)""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: an import the transfer usage was refused for fails", "vk.c",
     """            if (info.usage == img.usage) {
                reason = LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED;""",
     """            if (1) {
                reason = LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED;""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: an import whose image failed with the transfer usage fails", "vk.c",
     """    if (img.usage & optional) {
        VkImageUsageFlags kept = images->optionalUsage;""",
     """    if (0) {
        VkImageUsageFlags kept = images->optionalUsage;""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: an image the usage was refused for counted as if never asked for it", "vk.c",
     """            img.optionalVerdict = LORIE_VK_XFER_USAGE_REFUSED;""",
     "",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: BGRA into RGBX copied whatever the device check said", "renderer.c",
     """    return vk.transferCheck == LORIE_VK_XFER_CHECK_PASS && entry->dstDepth == 24 &&""",
     """    return entry->dstDepth == 24 &&""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: BGRA into RGBX copied at depth 32", "renderer.c",
     """    return vk.transferCheck == LORIE_VK_XFER_CHECK_PASS && entry->dstDepth == 24 &&""",
     """    return vk.transferCheck == LORIE_VK_XFER_CHECK_PASS &&""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: the device check not comparing the copy", "vk.c",
     """        if ((step = lorieVkSelftestCpu(t.ahb[1], t.expect, NULL, false, &diff, &got)))
            goto done;
    }
    result = LORIE_VK_XFER_CHECK_PASS;""",
     """    }
    result = LORIE_VK_XFER_CHECK_PASS;""",
     "tvkselftest"),
    ("P9-B0: a submission's entries timed as one", "renderer.c",
     """    for (i = 0; i < n; i++) {
        lorieLatencyAdd(&state->diag.copyQueue, startUs > queued[i] ? startUs - queued[i] : 0);""",
     """    for (i = 0; i < 1; i++) {
        lorieLatencyAdd(&state->diag.copyQueue, startUs > queued[i] ? startUs - queued[i] : 0);""",
     "tpresentcopy"),
    ("P9-B0: copies in a frame counted as on their own", "renderer.c",
     """    if (frame) {
        LORIE_DIAG_ADD(state, copyFrameEntries, n);""",
     """    if (0) {
        LORIE_DIAG_ADD(state, copyFrameEntries, n);""",
     "tpresentcopy"),
    ("P9-B0: the wait for a frame listed in flight counted in the recording", "renderer.c",
     """    lorieLatencyAddWeighted(&state->diag.copyRecord, submittedUs - lockedUs - inflightUs, n);""",
     """    lorieLatencyAddWeighted(&state->diag.copyRecord, submittedUs - lockedUs, n);""",
     "tpresentcopy"),
    ("P9-B0: a frame's fence counted as the copies' alone", "renderer.c",
     """    lorieLatencyAddWeighted(frame ? &state->diag.copyFrameGpuWait : &state->diag.copyGpuWait, fencedUs - submittedUs, n);""",
     """    lorieLatencyAddWeighted(&state->diag.copyGpuWait, fencedUs - submittedUs, n);""",
     "tpresentcopy"),
    ("P9-B0: a submission's parts not weighted by its entries", "lorie.h",
     """    __atomic_fetch_add(&l->weightedUs, us * entries, __ATOMIC_RELAXED);""",
     """    __atomic_fetch_add(&l->weightedUs, us, __ATOMIC_RELAXED);""",
     "tpresentcopy"),
    ("P9-B0: the X server's re-execution counted at every event, not once per publish", "InitOutput.c",
     """    if (!published || published == counted)
        return;""",
     """    if (!published)
        return;""",
     "tpresentcopy"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: a region not clipped to the destination", "renderer.c",
     """        if (w > (int32_t) d->width - dx)
            w = (int32_t) d->width - dx;""",
     """        if (0)
            w = (int32_t) d->width - dx;""",
     "tpresentcopy"),
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
