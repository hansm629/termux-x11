#!/usr/bin/env python3
"""Negative controls: each breaks one rule of the code under test, in a copy of the sources, and the test
that guards that rule must then fail. A control whose text is no longer in the sources stops with an error
rather than passing, so a control cannot quietly stop testing anything. A control of "xserver.patch" changes a copy
of patches/xserver.patch (line for line, so its hunks still apply), which gen.py then applies instead.

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
        Bool reserved = priv->reserved, wake = FALSE;
        if (!priv->wasLocked) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        // P11-C3-B1: likewise the CPU lock goes before the reservation, let go under the shared lock taken again
        if (reserved) {
            uint64_t lockedUs;
            lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
            lockedUs = priv->accessStartUs ? GetTimeInMicros() : 0;
            wake = lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
            priv->reserved = FALSE;
            if (lockedUs)
                priv->accessHoldUs += (uint32_t) (GetTimeInMicros() - lockedUs);
        } else if (priv->accessStartUs)
            priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);
        if (priv->accessStartUs) {
            uint64_t us = GetTimeInMicros() - priv->accessStartUs;
            lorieHistAdd(&pvfb->state->diag.xAccess, us);
            lorieHistAdd(&pvfb->state->diag.xAccessHold, priv->accessHoldUs);
            if (us > pvfb->worstAccess.us) {
                pvfb->worstAccess.us = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
                pvfb->worstAccess.width = pPix->drawable.width;
                pvfb->worstAccess.height = pPix->drawable.height;
                pvfb->worstAccess.index = index;
                pvfb->worstAccess.reserved = reserved;
            }
            priv->accessStartUs = 0;
        }
        if (!reserved)
            pvfb->accessLocks--;
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (wake)
            lorieCpuReleasedWake();
        return;
    }""",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        Bool reserved = priv->reserved, wake = FALSE;
        if (!reserved) {
            pvfb->accessLocks--;
            lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        }
        if (!priv->wasLocked) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        // P11-C3-B1: likewise the CPU lock goes before the reservation, let go under the shared lock taken again
        if (reserved) {
            uint64_t lockedUs;
            lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
            lockedUs = priv->accessStartUs ? GetTimeInMicros() : 0;
            wake = lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
            priv->reserved = FALSE;
            if (lockedUs)
                priv->accessHoldUs += (uint32_t) (GetTimeInMicros() - lockedUs);
        } else if (priv->accessStartUs)
            priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);
        if (priv->accessStartUs) {
            uint64_t us = GetTimeInMicros() - priv->accessStartUs;
            lorieHistAdd(&pvfb->state->diag.xAccess, us);
            lorieHistAdd(&pvfb->state->diag.xAccessHold, priv->accessHoldUs);
            if (us > pvfb->worstAccess.us) {
                pvfb->worstAccess.us = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
                pvfb->worstAccess.width = pPix->drawable.width;
                pvfb->worstAccess.height = pPix->drawable.height;
                pvfb->worstAccess.index = index;
                pvfb->worstAccess.reserved = reserved;
            }
            priv->accessStartUs = 0;
        }
        if (reserved)
            lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (wake)
            lorieCpuReleasedWake();
        return;
    }""",
     "townership"),
    ("the PER_ACCESS FinishAccess path taken under PERSISTENT too", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        Bool reserved = priv->reserved, wake = FALSE;""",
     """    if (lorieIsAhb(priv)) {
        Bool reserved = priv->reserved, wake = FALSE;""",
     "townership"),
    ("a CPU access to an AHB under PER_ACCESS without the shared lock", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv))
        return TRUE;
""",
     "",
     "townership"),
    ("a failed PrepareAccess under PER_ACCESS keeps the shared lock", "InitOutput.c",
     """                } else {
                    pvfb->accessLocks--;
                    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
                }
            }
            return FALSE;""",
     """                } else {
                }
            }
            return FALSE;""",
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
     """        pvfb->rootCopy.abandoned = TRUE;
    if (priv->persistentWanted) {
        xorg_list_del(&priv->persistentLink);
        priv->persistentWanted = FALSE;
    }""",
     """        pvfb->rootCopy.abandoned = TRUE;""",
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
    rendererVkPacingReset();
    releaseWinAndSurface(&win, &sfc);""",
     """    vkActive = false;
    rendererVkPacingReset();
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
            rendererVkPacingReset();

            if (state) {""",
     """            }
            rendererVkPacingReset();

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
        if (res == VK_SUCCESS && state->backend.vkFrameDiag)
            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);    // P11-C3: these copies' own fence
    }
    if (res == VK_SUCCESS) {
        // Only now that the device finished (and gave the buffers back) may the X server reuse the source.
        uint64_t queued[LORIE_GPU_COPY_QUEUE_CAPACITY], publishedUs;
        uint32_t n = rendererVkQueuedTimes(readIndex, queued);""",
     """            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
        if (res == VK_SUCCESS && state->backend.vkFrameDiag)
            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);    // P11-C3: these copies' own fence
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

    // P11-C1: no copy in it (serial: the last entry it took, cancelled ones too), presented now; its result taken up
    // once the fence was waited for, as the present below.
    if ((early = submitOpt && res == VK_SUCCESS && !serial)) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();
        rendererStage(LORIE_R_PRESENT);
        presented = lorieVkPresent(&vk, &vkr, index, state);
        rendererStage(LORIE_R_FRAME);
        if (diag)
            ft.presentEndUs = lorieMonotonicUs();
        LORIE_DIAG_ADD(state, vkEarlyPresents, 1);
    }
    // P11-C3-B2: the root copy submitted ahead - over by now or soon: the device runs it before this frame
    if (vkAux.pending) {
        rendererVkAuxFinish();
        if (!vkActive)
            return;
    }
    if (res == VK_SUCCESS) {
        // The device runs the frame; the X server goes on, without the frame's buffers (rendererVkInFlight).
        bool over;
        rendererStage(LORIE_R_FENCE_WAIT);
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        rendererStage(LORIE_R_FRAME);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
        if (diag)
            ft.fenceUs = vkr.fenceWaitUs;   // this frame's own fence (P11-C3)
    }""",
     """    state->waitForNextFrame = true;

    // P11-C1: no copy in it (serial: the last entry it took, cancelled ones too), presented now; its result taken up
    // once the fence was waited for, as the present below.
    if ((early = submitOpt && res == VK_SUCCESS && !serial)) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();
        rendererStage(LORIE_R_PRESENT);
        presented = lorieVkPresent(&vk, &vkr, index, state);
        rendererStage(LORIE_R_FRAME);
        if (diag)
            ft.presentEndUs = lorieMonotonicUs();
        LORIE_DIAG_ADD(state, vkEarlyPresents, 1);
    }
    // P11-C3-B2: the root copy submitted ahead - over by now or soon: the device runs it before this frame
    if (vkAux.pending) {
        rendererVkAuxFinish();
        if (!vkActive)
            return;
    }
    if (res == VK_SUCCESS) {
        // The device runs the frame; the X server goes on, without the frame's buffers (rendererVkInFlight).
        bool over;
        rendererStage(LORIE_R_FENCE_WAIT);
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        rendererStage(LORIE_R_FRAME);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
        if (diag)
            ft.fenceUs = vkr.fenceWaitUs;   // this frame's own fence (P11-C3)
    }
    lorie_mutex_unlock(&state->lock, &state->lockingPid);""",
     "tpresentcopy"),
    ("a frame's buffers not listed in flight", "renderer.c",
     """    if (res == VK_SUCCESS) {
        vkAux.chained = false;
        rendererVkInFlight();
    }
    state->waitForNextFrame = true;""",
     """    if (res == VK_SUCCESS) {
        vkAux.chained = false;
    }
    state->waitForNextFrame = true;""",
     "tpresentcopy"),
    ("a frame's buffers never given back to the X server", "renderer.c",
     """state->gpuInFlight.pid == getpid() && (__atomic_fetch_and(&state->gpuInFlight.busy, ~list, __ATOMIC_RELEASE) & list))""",
     """state->gpuInFlight.pid == getpid() && 0 && (__atomic_fetch_and(&state->gpuInFlight.busy, ~list, __ATOMIC_RELEASE) & list))""",
     "tpresentcopy"),
    ("an access CPU-locking an AHB a submitted Vulkan frame uses", "InitOutput.c",
     """            lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
            gpuUs = GetTimeInMicros() - gpuStartUs;""",
     """            gpuUs = GetTimeInMicros() - gpuStartUs;""",
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
        rendererVkWaitOthers();""",
     """        inflightUs = lorieMonotonicUs();""",
     "tpresentcopy"),
    ("this process clearing another's list", "renderer.c",
     """    if (state && state->gpuInFlight.pid == getpid() && (__atomic_fetch_and""",
     """    if (state && (__atomic_fetch_and""",
     "tpresentcopy"),
    ("Vulkan's tick frame allowed only after the vblanks queued their copies", "InitOutput.c",
     """    if (lorieAhbPerAccess() && !twinned)
        pvfb->state->waitForNextFrame = false;
    loriePerformVblanks();""",
     """    loriePerformVblanks();""",
     "twiring"),
    ("FIFO copies held up by the frame's swapchain image", "renderer.c",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR || submitOpt) {""",
     """    if (submitOpt) {""",
     "tpresentcopy"),
    ("copies in a submission of their own whatever the present mode, the option off", "renderer.c",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR || submitOpt) {""",
     """    if (1) {""",
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
    ("P10-S1: a copy in flight with a live renderer taken as cancelled", "InitOutput.c",
     """            if ((lorieGpuInFlight(st, e->srcBufferId) || lorieGpuInFlight(st, e->dstBufferId)) && !lorieGpuSubmitterGone(st))""",
     """            if (0)""",
     "tpresentcopy"),
    ("P10-S1: Vulkan copying an entry the X server cancelled", "renderer.c",
     """        if (entry.cancelled) {
            // Its presentation let go before it was recorded (InitOutput.c lorieGpuCopySettle): not copied, completed.
            *serial = entry.serial;
            continue;
        }""",
     "",
     "tpresentcopy"),
    ("P10-S1: GLES copying an entry the X server cancelled", "renderer.c",
     """        if (entry.cancelled) {
            // Its presentation let go before it was recorded (InitOutput.c lorieGpuCopySettle): not copied, completed.
            lastSerial = entry.serial;""",
     """        if (0) {
            lastSerial = entry.serial;""",
     "tpresentcopy"),
    ("P11-A: the yield asked for with TERMUX_X11_X_YIELD_ON_WAKE off", "InitOutput.c",
     """    if (QueueWorkProc(proc, NULL, closure) && __atomic_load_n(&pvfb->yieldOnWake, __ATOMIC_RELAXED))""",
     """    if (QueueWorkProc(proc, NULL, closure))""",
     "tdispatchyield"),
    ("P11-A: the yield never asked for", "InitOutput.c",
     """        __atomic_exchange_n(&dispatchYieldRequested, 1, __ATOMIC_RELEASE);""",
     """        ;""",
     "tdispatchyield"),
    ("P11-A: the vsync tick queued as any work", "InitOutput.c",
     """        lorieQueueUrgentWorkProc(lorieRedraw, (void *) (uintptr_t) (uint32_t) now);""",
     """        QueueWorkProc(lorieRedraw, NULL, (void *) (uintptr_t) (uint32_t) now);
        lorieWakeServer();""",
     "twiring"),
    ("P11-B: the root's front locked for reading", "InitOutput.c",
     """(failed = LorieBuffer_lockRect(front, true, rect, (void **) &dst)))""",
     """(failed = LorieBuffer_lockRect(front, false, rect, (void **) &dst)))""",
     "townership"),
    ("P11-B: the locks' rect from the boxes unclipped", "InitOutput.c",
     """bx2 = min(box[i].x2, d->width), by2 = min(box[i].y2, d->height);""",
     """bx2 = box[i].x2, by2 = box[i].y2;""",
     "townership"),
    ("P11-B: the locks' rect holding only the first box", "InitOutput.c",
     """    int x1 = d->width, y1 = d->height, x2 = 0, y2 = 0, i;
    for (i = 0; i < n; i++) {""",
     """    int x1 = d->width, y1 = d->height, x2 = 0, y2 = 0, i;
    for (i = 0; i < n && i < 1; i++) {""",
     "townership"),
    ("P11-B: the back left locked when the front's lock is refused", "InitOutput.c",
     """(failed = LorieBuffer_lockRect(front, true, rect, (void **) &dst)))
            LorieBuffer_unlock(back);""",
     """(failed = LorieBuffer_lockRect(front, true, rect, (void **) &dst)))
            ;""",
     "townership"),
    ("P11-B: its own locks refused, not given up", "InitOutput.c",
     """        pvfb->rootCopyOptFailed = failed != 0;""",
     """        ;""",
     "townership"),
    ("P11-B: its own locks refused, the locks of before not taken", "InitOutput.c",
     """        pvfb->rootCopyOptFailed = failed != 0;
    }
    if (failed) {""",
     """        pvfb->rootCopyOptFailed = failed != 0;
    }
    if (failed && !pvfb->rootCopyOpt) {""",
     "townership"),
    ("P11-B: a refused rect lock marked locked", "buffer.c",
     """    if (ret)
        return ret;     // unlike LorieBuffer_lock, not marked locked: nothing to unlock""",
     """    (void) 0;""",
     "tbufferlock"),
    ("P11-B: a read lock asking to write too", "buffer.c",
     """write ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,""",
     """write ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,""",
     "tbufferlock"),
    ("P11-C1: the copies not ahead of the frame", "renderer.c",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR || submitOpt) {""",
     """    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR) {""",
     "tpresentcopy"),
    ("P11-C1: a frame carrying copies presented before they are published", "renderer.c",
     """    if ((early = submitOpt && res == VK_SUCCESS && !serial)) {""",
     """    if ((early = submitOpt && res == VK_SUCCESS)) {""",
     "tpresentcopy"),
    ("P11-C1: an early present's failure not taken up", "renderer.c",
     """    if (presented == LORIE_VK_FRAME_FATAL)
        return rendererVkFail();
    state->renderedFrames++;""",
     """    if (!early && presented == LORIE_VK_FRAME_FATAL)
        return rendererVkFail();
    state->renderedFrames++;""",
     "tpresentcopy"),
    ("P11-C1: a frame presented early presented again after its fence", "renderer.c",
     """    if (!early) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();""",
     """    if (1) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();""",
     "tpresentcopy"),
    ("P11-C1: frames presented early with the option off", "renderer.c",
     """    if ((early = submitOpt && res == VK_SUCCESS && !serial)) {""",
     """    if ((early = res == VK_SUCCESS && !serial)) {""",
     "tpresentcopy"),
    ("P11-C2: the root's buffer CPU-locked before the copy into it settled", "InitOutput.c",
     """    if (pvfb->rootCopy.seq && priv && priv->buffer == pvfb->rootCopy.to)
        lorieRootCopyWait(FALSE);""",
     """    ;""",
     "townership"),
    ("P11-C2: a root copy no renderer took waited for with the shared lock held", "InitOutput.c",
     """    Bool held = pvfb->accessLocks > 0, copy = FALSE, waited = FALSE;""",
     """    Bool held = FALSE, copy = FALSE, waited = FALSE;""",
     "townership"),
    ("P11-C2: an EXA access's shared lock not counted", "InitOutput.c",
     """        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
        pvfb->accessLocks++;""",
     """        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);""",
     "townership"),
    ("P11-C2: a root copy no renderer will take waited for anyway", "InitOutput.c",
     """            if (takeBack || held || !lorieConnectionAlive() || !lorieRendererAvailable() || lorieOwnershipAsked()) {""",
     """            if (takeBack || held) {""",
     "townership"),
    ("P11-C2: a root copy the renderer could not make left uncopied", "InitOutput.c",
     """    if (done == seq && __atomic_load_n(&st->rootCopy.failed, __ATOMIC_RELAXED) == seq)
        copy = TRUE;""",
     """    ;""",
     "townership"),
    ("P11-C2: a root copy taken by a renderer gone waited for", "InitOutput.c",
     """        } else if (st->rootCopy.pid > 0 && kill(st->rootCopy.pid, 0) == -1 && errno == ESRCH) {""",
     """        } else if (0) {""",
     "townership"),
    ("P11-C2: a swap's root copy asked for before the last one settled", "InitOutput.c",
     """    lorieRootCopyWait(FALSE);   // P11-C2: the last copy asked of the GPU settled first (and the swap undone, should it have failed)""",
     """    ;""",
     "townership"),
    ("P11-C2: the ownership answer given with a root copy left to the GPU", "InitOutput.c",
     """    lorieRootCopyWait(TRUE);""",
     """    ;""",
     "townership"),
    ("P11-C2: the ownership answer waiting for a renderer to take the root copy", "InitOutput.c",
     """    lorieRootCopyWait(TRUE);""",
     """    lorieRootCopyWait(FALSE);""",
     "townership"),
    ("P11-C2: a root copy settled as copied before its fence", "renderer.c",
     """    if (res == VK_SUCCESS) {
        if (ahead)
            rendererVkInFlightAux();
        else
            rendererVkInFlight();
        LORIE_DIAG_ADD(state, rootGpuCopySubmits, 1);
        if (w->diag)
            w->submittedUs = lorieMonotonicUs();
    }""",
     """    if (res == VK_SUCCESS) {
        if (ahead)
            rendererVkInFlightAux();
        else
            rendererVkInFlight();
        LORIE_DIAG_ADD(state, rootGpuCopySubmits, 1);
        if (w->diag)
            w->submittedUs = lorieMonotonicUs();
        rendererRootCopySettle(w->seq, false);
    }""",
     "tpresentcopy"),
    ("P11-C2: a root copy's buffers not listed in flight", "renderer.c",
     """    if (res == VK_SUCCESS) {
        if (ahead)
            rendererVkInFlightAux();
        else
            rendererVkInFlight();
        LORIE_DIAG_ADD(state, rootGpuCopySubmits, 1);
        if (w->diag)
            w->submittedUs = lorieMonotonicUs();
    }""",
     """    if (res == VK_SUCCESS) {
        LORIE_DIAG_ADD(state, rootGpuCopySubmits, 1);
        if (w->diag)
            w->submittedUs = lorieMonotonicUs();
    }""",
     "tpresentcopy"),
    ("P11-C2: a root copy drawn without the PER_ACCESS answer", "renderer.c",
     """    state->rootCopy.pid = getpid();
    if (w->diag)
        w->takenUs = lorieMonotonicUs();
    if (!rendererVkOwnedLocked()) {""",
     """    state->rootCopy.pid = getpid();
    if (w->diag)
        w->takenUs = lorieMonotonicUs();
    if (0) {""",
     "tpresentcopy"),
    ("P11-C2: a root copy into a buffer the renderer does not have settled as copied", "renderer.c",
     """    if (!src || !dst || lorieCpuReserved(state, w->srcId) || lorieCpuReserved(state, w->dstId)) {
        rendererRootCopySettle(w->seq, true);""",
     """    if (!src || !dst || lorieCpuReserved(state, w->srcId) || lorieCpuReserved(state, w->dstId)) {
        rendererRootCopySettle(w->seq, false);""",
     "tpresentcopy"),
    ("P11-C2: a root copy not made first in the renderer's loop", "renderer.c",
     """    if (state && rendererRootCopyPending()) {
        if (!vkActive)
            rendererRootCopyDecline();
        else if (state->backend.vkScheduleOpt && rendererVkFrameWanted())
            rendererVkRootCopyAhead();
        else
            rendererVkRootCopy();
    }""",
     """    if (state && rendererRootCopyPending() && !vkActive)
        rendererRootCopyDecline();""",
     "twiring"),
    ("P11-C2: the renderer sleeping through a root copy asked for", "renderer.c",
     """    if (stateChanged || windowChanged || buffersChanged || gpuCopyPending || backendSelect || rendererRootCopyPending())""",
     """    if (stateChanged || windowChanged || buffersChanged || gpuCopyPending || backendSelect)""",
     "twiring"),
    ("TERMUX_X11_RENDERER_VK_TRANSFER_COPY: a region not clipped to the destination", "renderer.c",
     """        if (w > (int32_t) d->width - dx)
            w = (int32_t) d->width - dx;""",
     """        if (0)
            w = (int32_t) d->width - dx;""",
     "tpresentcopy"),
]


# P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG: the timings' own controls
CONTROLS += [
    ("P11-C3: a root copy's fence counted as a frame's", "renderer.c",
     """            lorieHistAdd(&dg->vkFenceRoot, fenceUs);""",
     """            lorieHistAdd(&dg->vkFrameFence, fenceUs);""",
     "tpresentcopy"),
    ("P11-C3: the copies' own fence counted as the frame's", "renderer.c",
     """            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);    // P11-C3: these copies' own fence""",
     """            lorieHistAdd(&state->diag.vkFrameFence, vkr.fenceWaitUs);""",
     "tpresentcopy"),
    ("P11-C3: every fence's wait so far as the frame's", "renderer.c",
     """            ft.fenceUs = vkr.fenceWaitUs;   // this frame's own fence (P11-C3)""",
     """            ft.fenceUs = state->diag.vkFenceWaitUs;""",
     "tpresentcopy"),
    ("P11-C3: the copies' fence sampled with the option off", "renderer.c",
     """        if (res == VK_SUCCESS && state->backend.vkFrameDiag)
            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);""",
     """        if (res == VK_SUCCESS)
            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);""",
     "tpresentcopy"),
    ("P11-C3: a root copy's end stamped with the option off", "renderer.c",
     """    if (state->backend.vkFrameDiag)
        state->rootCopy.doneUs = lorieMonotonicUs();""",
     """    state->rootCopy.doneUs = lorieMonotonicUs();""",
     "tpresentcopy"),
    ("P11-C3: the first frame after a reset an interval (from 0)", "renderer.c",
     """    if (!prev)
        return LORIE_VK_INTERVAL_FIRST;""",
     """    if (!prev && 0)
        return LORIE_VK_INTERVAL_FIRST;""",
     "tpresentcopy"),
    ("P11-C3: an idle gap's bound taken as a gap", "renderer.c",
     """    if (now - prev > LORIE_VK_IDLE_GAP_US)""",
     """    if (now - prev >= LORIE_VK_IDLE_GAP_US)""",
     "tpresentcopy"),
    ("P11-C3: a stamp before the last sampled as a huge interval", "renderer.c",
     """    if (now < prev)
        return LORIE_VK_INTERVAL_INVALID;""",
     """    if (0)
        return LORIE_VK_INTERVAL_INVALID;""",
     "tpresentcopy"),
    ("P11-C3: a tick's request the last frame took up already taken as this one's wake", "renderer.c",
     """    if (t->wantedUs && t->wantedUs > lastStartUs && t->wantedUs <= t->startUs""",
     """    if (t->wantedUs && t->wantedUs <= t->startUs""",
     "tpresentcopy"),
    ("P11-C3: the root copy kept as ahead of every later frame", "renderer.c",
     """        ft.afterRootUs = vkRootCopyBusyUs;
        vkRootCopyBusyUs = 0;""",
     """        ft.afterRootUs = vkRootCopyBusyUs;""",
     "tpresentcopy"),
    ("P11-C3: a bucket's bound counted in the bucket above (16.667 ms over 16.667 ms)", "lorie.h",
     """    while (b < LORIE_HIST_BUCKETS - 1 && v > bounds[b])""",
     """    while (b < LORIE_HIST_BUCKETS - 1 && v >= bounds[b])""",
     "tdiaglog"),
    ("P11-C3: p99 reported as p95", "InitOutput.c",
     """    static const uint32_t percent[2] = { 95, 99 };""",
     """    static const uint32_t percent[2] = { 95, 95 };""",
     "tdiaglog"),
    ("P11-C3: a timing's maximum not taken back by its report", "InitOutput.c",
     """    uint32_t n = now->count - before->count, top = __atomic_exchange_n(max, 0, __ATOMIC_RELAXED), over16 = 0, over33 = 0;""",
     """    uint32_t n = now->count - before->count, top = __atomic_load_n(max, __ATOMIC_RELAXED), over16 = 0, over33 = 0;""",
     "tdiaglog"),
    ("P11-C3: the frame diagnostics' lines written with the option off", "InitOutput.c",
     """    if (pvfb->frameDiag) {
        char h[10][112];""",
     """    if (1) {
        char h[10][112];""",
     "tdiaglog"),
    ("P11-C3: a tick that asked for no frame leaves the last request standing", "InitOutput.c",
     """        __atomic_fetch_add(&pvfb->state->diag.xTicksFrameMissed, 1, __ATOMIC_RELAXED);
    pvfb->frameAsked = FALSE;""",
     """        __atomic_fetch_add(&pvfb->state->diag.xTicksFrameMissed, 1, __ATOMIC_RELAXED);""",
     "tdiaglog"),
    ("P11-C3: a frame presented by GLES not seen by the tick", "InitOutput.c",
     """    return __atomic_load_n(&pvfb->state->diag.vkFramesPresented, __ATOMIC_RELAXED) +
           __atomic_load_n(&pvfb->state->diag.eglSwapBuffers, __ATOMIC_RELAXED);""",
     """    return __atomic_load_n(&pvfb->state->diag.vkFramesPresented, __ATOMIC_RELAXED);""",
     "tdiaglog"),
    ("P11-C3: the X server's resume timed for a copy it never waited for", "InitOutput.c",
     """    if (waited && pvfb->frameDiag) {""",
     """    if (pvfb->frameDiag) {""",
     "townership"),
    ("P11-C3: an access's start left after its FinishAccess", "InitOutput.c",
     """            priv->accessStartUs = 0;
        }
        if (!reserved)""",
     """        }
        if (!reserved)""",
     "townership"),
    ("P11-C3: a request's opcodes taken after it ran (it may have closed its client)", "xserver.patch",
     """+                            ddxRequestTimed(timedIndex, timedMajor, timedMinor, timedUs);""",
     """+                            ddxRequestTimed(client->index, client->majorOp, client->minorOp, timedUs);""",
     "tdispatchyield"),
    ("P11-C3: the clock read for every request with the option off", "xserver.patch",
     """+                        CARD64 timedUs = dispatchRequestTimed ? ddxRequestStart(client->index, client->majorOp, client->minorOp) : 0;""",
     """+                        CARD64 timedUs = GetTimeInMicros();""",
     "tdispatchyield"),
    ("P11-C3: the option read into the renderer's flag only", "InitOutput.c",
     """    dispatchRequestTimed = lorieScreen.frameDiag || lorieScreen.freezeDiag;""",
     """""",
     "twiring"),
    ("P11-C3: the pacing not started over with another X server session", "renderer.c",
     """            vkOwned = false;
            rendererVkPacingReset();""",
     """            vkOwned = false;""",
     "twiring"),
]


# P11-C3-B1, TERMUX_X11_EXA_LOCK_OPT: the reservations' own controls
CONTROLS += [
    ("P11-C3-B1: the published root reserved", "InitOutput.c",
     """    if (!pvfb->exaLockOpt || id == st->rootWindowTextureID)""",
     """    if (!pvfb->exaLockOpt)""",
     "townership"),
    ("P11-C3-B1: an AHB reserved while a frame may use it (no wait first)", "InitOutput.c",
     """        if (perAccessAhb && !priv->locked && !priv->mem) {
            uint64_t gpuStartUs = GetTimeInMicros(), gpuUs;""",
     """        if (perAccessAhb && !priv->locked && !priv->mem && !pvfb->exaLockOpt) {
            uint64_t gpuStartUs = GetTimeInMicros(), gpuUs;""",
     "townership"),
    ("P11-C3-B1: the reservation let go before the CPU lock", "InitOutput.c",
     """        Bool reserved = priv->reserved, wake = FALSE;
        if (!priv->wasLocked) {""",
     """        Bool reserved = priv->reserved, wake = FALSE;
        if (reserved) {
            lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
            lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
            lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        }
        if (!priv->wasLocked) {""",
     "townership"),
    ("P11-C3-B1: a refused CPU lock keeps its reservation", "InitOutput.c",
     """                    wake = lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
                    priv->reserved = FALSE;
                    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);""",
     """                    wake = FALSE;
                    priv->reserved = FALSE;
                    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);""",
     "townership"),
    ("P11-C3-B1: a reserved access counted as holding the shared lock", "InitOutput.c",
     """            if (priv->accessStartUs)
                priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);
            pvfb->accessLocks--;""",
     """            if (priv->accessStartUs)
                priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);""",
     "townership"),
    ("P11-C3-B1: a renderer waiting for a release never woken", "InitOutput.c",
     """    return __atomic_exchange_n(&st->cpuAccess.waiter, 0, __ATOMIC_RELAXED) != 0;""",
     """    return FALSE;""",
     "townership"),
    ("P11-C3-B1: a copy using a reserved buffer recorded", "renderer.c",
     """    if (!state->cpuAccess.count || (!lorieCpuReserved(state, entry->srcBufferId) && !lorieCpuReserved(state, entry->dstBufferId)))""",
     """    if (1 || (!lorieCpuReserved(state, entry->srcBufferId) && !lorieCpuReserved(state, entry->dstBufferId)))""",
     "tpresentcopy"),
    ("P11-C3-B1: GLES copies a reserved buffer", "renderer.c",
     """        if (rendererCopyDeferred(&entry, state->gpuCopyQueue.readIndex))
            break;      // P11-C3-B1: left queued, with the copies after it""",
     """        if (0)
            break;""",
     "tpresentcopy"),
    ("P11-C3-B1: copies alone submitted with none taken", "renderer.c",
     """    if (readIndex == state->gpuCopyQueue.readIndex) {""",
     """    if (0) {""",
     "tpresentcopy"),
    ("P11-C3-B1: a root copy into a reserved buffer", "renderer.c",
     """    if (!src || !dst || lorieCpuReserved(state, w->srcId) || lorieCpuReserved(state, w->dstId)) {""",
     """    if (!src || !dst) {""",
     "tpresentcopy"),
    ("P11-C3-B1: copies left queued wake the renderer again and again", "renderer.c",
     """    // P11-C3-B1: copies left queued for a reservation are pending again only once one is let go
    gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex && !rendererCopiesStillDeferred();""",
     """    // P11-C3-B1: copies left queued for a reservation are pending again only once one is let go
    gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex;""",
     "twiring"),
]


# a frame's fence not known to be over is never reset
CONTROLS += [
    ("vk: a fence not known to be over reset", "vk.c",
     """        if (*over)
            vk->ResetFences(vk->device, 1, &r->frameFence);
        return res;""",
     """        vk->ResetFences(vk->device, 1, &r->frameFence);
        return res;""",
     "tpresentcopy"),
]


# P11-C3-B2, TERMUX_X11_RENDERER_VK_SCHEDULE_OPT: the root copy ahead of the frame - its own controls
CONTROLS += [
    ("P11-C3-B2: the frame not chained after the root copy ahead", "renderer.c",
     """    vkr.chainWait = vkAux.chained;      // P11-C3-B2: on the device after the root copy submitted ahead""",
     """    vkr.chainWait = false;""",
     "tpresentcopy"),
    ("P11-C3-B2: the chain semaphore never signalled", "vk.c",
     """    if (r->chainSignal)
        signal[signals++] = r->chain;""",
     """    if (0)
        signal[signals++] = r->chain;""",
     "tpresentcopy"),
    ("P11-C3-B2: the root copy ahead settled as copied before its fence", "renderer.c",
     """    vkAux.work = w;
    vkAux.pending = vkAux.chained = true;""",
     """    vkAux.work = w;
    rendererRootCopySettle(w.seq, false);
    vkAux.pending = vkAux.chained = true;""",
     "tpresentcopy"),
    ("P11-C3-B2: the root copy's list kept in flight after its fence", "renderer.c",
     """        if (ahead)
            rendererVkAuxDone();
        else
            rendererVkFrameDone();""",
     """        if (!ahead)
            rendererVkFrameDone();""",
     "tpresentcopy"),
    ("P11-C3-B2: a submission over clears every list in flight", "renderer.c",
     """    if (state && state->gpuInFlight.pid == getpid() && (__atomic_fetch_and(&state->gpuInFlight.busy, ~list, __ATOMIC_RELEASE) & list))""",
     """    if (state && state->gpuInFlight.pid == getpid() && __atomic_exchange_n(&state->gpuInFlight.busy, 0, __ATOMIC_RELEASE))""",
     "tpresentcopy"),
    ("P11-C3-B2: no empty submission consumes the chain semaphore", "renderer.c",
     """        if (lorieVkChainConsume(&vk, &vkr, state) != VK_SUCCESS)
            rendererVkFail();""",
     """        if (0)
            rendererVkFail();""",
     "tpresentcopy"),
    ("P11-C3-B2: the empty submission's fence not waited for", "vk.c",
     """    if ((res = vk->QueueSubmit(vk->queue, 1, &submit, r->frameFence)) != VK_SUCCESS ||
        (res = vk->WaitForFences(vk->device, 1, &r->frameFence, VK_TRUE, LORIE_VK_FENCE_TIMEOUT_NS)) != VK_SUCCESS ||
        (res = vk->ResetFences(vk->device, 1, &r->frameFence)) != VK_SUCCESS) {""",
     """    if ((res = vk->QueueSubmit(vk->queue, 1, &submit, r->frameFence)) != VK_SUCCESS) {""",
     "tpresentcopy"),
    ("P11-C3-B2: the X server waiting for the root copy ahead not seen", "renderer.c",
     """    if (vkAux.pending && __atomic_load_n(&state->rootCopy.waiting, __ATOMIC_ACQUIRE) == vkAux.work.seq) {""",
     """    if (0) {""",
     "tpresentcopy"),
    ("P11-C3-B2: a copy into the root copy's buffer recorded while it runs", "renderer.c",
     """        if (vkAux.pending && (entry.srcBufferId == vkAux.work.srcId || entry.srcBufferId == vkAux.work.dstId ||""",
     """        if (0 && (entry.srcBufferId == vkAux.work.srcId || entry.srcBufferId == vkAux.work.dstId ||""",
     "tpresentcopy"),
    ("P11-C3-B2: the X server's wait for the root copy not said", "InitOutput.c",
     """        __atomic_store_n(&st->rootCopy.waiting, seq, __ATOMIC_RELEASE);    // P11-C3-B2: a renderer may finish it first""",
     """""",
     "townership"),
    ("P11-C3-B2: a frame waits for this process's own lists (the root copy ahead: for good)", "renderer.c",
     """static void rendererVkWaitOthers(void) {
    if (state->gpuInFlight.pid != getpid())""",
     """static void rendererVkWaitOthers(void) {
    if (1)""",
     "tpresentcopy"),
    ("P11-C3-B2: the root copy ahead without a frame to follow", "renderer.c",
     """        else if (state->backend.vkScheduleOpt && rendererVkFrameWanted())""",
     """        else if (state->backend.vkScheduleOpt)""",
     "twiring"),
]


# P11-C3: the worst interval's frame before it - the one that began it - and the gap after it
CONTROLS += [
    ("P11-C3: the worst interval's earlier frame not kept", "renderer.c",
     """    vkPacingLast.cpuUs = t->endUs - t->startUs;""",
     """    vkPacingLast.cpuUs = 0;""",
     "tpresentcopy"),
    ("P11-C3: the gap measured from the earlier frame's start", "renderer.c",
     """    vkPacingLast.endUs = t->endUs;""",
     """    vkPacingLast.endUs = t->startUs;""",
     "tpresentcopy"),
]


# the renderer's pass: Vulkan or GLES, never both - and a root copy submitted ahead settled within it
CONTROLS += [
    ("pass: the GLES frame not chained to the Vulkan branch", "renderer.c",
     """    } else if (state && state->surfaceAvailable && !state->waitForNextFrame &&
        (state->drawRequested || state->cursor.moved || state->cursor.updated || gpuCopyPending)) {
        rendererRedrawLocked(waitingForBuffers);""",
     """    }
    if (state && state->surfaceAvailable && !state->waitForNextFrame &&
        (state->drawRequested || state->cursor.moved || state->cursor.updated || gpuCopyPending)) {
        rendererRedrawLocked(waitingForBuffers);""",
     "tpresentcopy"),
    ("pass: the GLES branch the root copy settling's else (b32e1a1)", "renderer.c",
     """        if (vkAux.pending || vkAux.chained)
            rendererVkAuxSettle();
    } else if (state && state->surfaceAvailable && !state->waitForNextFrame &&""",
     """    }
        if (vkAux.pending || vkAux.chained)
            rendererVkAuxSettle(); else if (state && state->surfaceAvailable && !state->waitForNextFrame &&""",
     "tpresentcopy"),
    ("pass: GLES's copies alone in a Vulkan pass", "renderer.c",
     """    } else if (gpuCopyPending) {
        // Also when no window is drawn (or Vulkan failed, even in a session that asked for Vulkan only): the X""",
     """    }
    if (gpuCopyPending) {
        // Also when no window is drawn (or Vulkan failed, even in a session that asked for Vulkan only): the X""",
     "tpresentcopy"),
    ("pass: a root copy submitted ahead left pending after the pass", "renderer.c",
     """        // P11-C3-B2: a root copy submitted ahead and no frame after it: waited for, its chain semaphore consumed
        if (vkAux.pending || vkAux.chained)
            rendererVkAuxSettle();""",
     """        // P11-C3-B2: a root copy submitted ahead and no frame after it: waited for, its chain semaphore consumed""",
     "tpresentcopy"),
]

# P11-C3-C: the renderer's wake (lorie.h struct lorie_renderer_wake): each way to lose a wake again
CONTROLS += [
    ("wake: the count read after the look (the lost wakeup put back)", "renderer.c",
     """        uint32_t seen = lorieWakeSeen(rendererWake);
        if (!rendererShouldWait(waitingForBuffers))
            break;""",
     """        uint32_t seen;
        if (!rendererShouldWait(waitingForBuffers))
            break;
        seen = lorieWakeSeen(rendererWake);""",
     "twake"),
    ("wake: the sleep compares the count as it is then, not as the look saw it", "renderer.c",
     """    syscall(SYS_futex, &rendererWake->seq, FUTEX_WAIT, seen, timeoutUs >= 0 ? &rel : NULL, NULL, 0);""",
     """    syscall(SYS_futex, &rendererWake->seq, FUTEX_WAIT, lorieWakeSeen(rendererWake) + 0 * seen, timeoutUs >= 0 ? &rel : NULL, NULL, 0);""",
     "twake"),
    ("wake: a wake that does not move the count", "lorie.h",
     """    __atomic_fetch_add(&w->seq, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &w->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);""",
     """    syscall(SYS_futex, &w->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);""",
     "twake"),
    ("wake: a private futex wake, which a sleep in another process never sees", "lorie.h",
     """    syscall(SYS_futex, &w->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);""",
     """    syscall(SYS_futex, &w->seq, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);""",
     "twake"),
    ("wake: copies left for a reservation polled for, not slept on until the X server's wake", "renderer.c",
     """        rendererSleep(seen, -1);
    }
    // P11-C3-C: the asks it woke to""",
     """        rendererSleep(seen + (rendererCopiesStillDeferred() ? 1 : 0), -1);
    }
    // P11-C3-C: the asks it woke to""",
     "twake"),
    ("wake: the ownership answer looked for before the count is read", "renderer.c",
     """        uint32_t seen = lorieWakeSeen(rendererWake);
        uint64_t now;
        if (LORIE_OWNERSHIP_SEQ(ack = __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE)) == seq ||
            (now = lorieMonotonicUs()) >= deadlineUs)
            break;
        rendererSleep(seen, (int64_t) (deadlineUs - now));""",
     """        uint32_t seen;
        uint64_t now;
        if (LORIE_OWNERSHIP_SEQ(ack = __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE)) == seq ||
            (now = lorieMonotonicUs()) >= deadlineUs)
            break;
        seen = lorieWakeSeen(rendererWake);
        rendererSleep(seen, (int64_t) (deadlineUs - now));""",
     "tvkrenderer"),
    ("wake: a buffer's wake before it is in the list", "renderer.c",
     """    LorieBuffer_addToList(buf, &addedBuffers);
    pthread_spin_unlock(&bufferLock);
    lorieWakeRenderer(rendererWake);     // after the buffer is in the list the renderer looks at""",
     """    lorieWakeRenderer(rendererWake);     // after the buffer is in the list the renderer looks at
    LorieBuffer_addToList(buf, &addedBuffers);
    pthread_spin_unlock(&bufferLock);""",
     "twiring"),
    ("wake: the X server's GPU copy wake before the copy is queued", "InitOutput.c",
     """    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above
    lorieWakeRenderer(rendererWake);""",
     """    lorieWakeRenderer(rendererWake);
    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above""",
     "twiring"),
    ("wake: the region swapped without the input lock", "InitOutput.c",
     """    input_lock();
    old = rendererWake;
    rendererWake = newWake;
    input_unlock();""",
     """    old = rendererWake;
    rendererWake = newWake;""",
     "twiring"),
]

# P11-C3-C: the shared lock's 33 ms wait on the clock pthread_mutex_timedlock uses
CONTROLS += [
    ("shared lock: its deadline on CLOCK_MONOTONIC again", "lorie.h",
     """        clock_gettime(CLOCK_REALTIME, &ts);""",
     """        clock_gettime(CLOCK_MONOTONIC, &ts);""",
     "tsharedlock"),
]

# P11-C3-C: the buffers a pass's copies use waited for before the shared lock, not under it
CONTROLS += [
    ("buffers: the pass does not wait for them before the lock", "renderer.c",
     """    // P11-C3-C: the buffers its copies use, waited for now if they have not come - not under the X server's lock
    rendererAwaitBuffers();
""",
     """    // P11-C3-C: the buffers its copies use, waited for now if they have not come - not under the X server's lock
""",
     "tpresentcopy"),
    ("buffers: waited for under the lock again (the old 5 ms retries)", "renderer.c",
     """    pthread_spin_lock(&bufferLock);
    buf = LorieBufferList_findById(&buffers, id);
    if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id)))
        LorieBuffer_addToList(buf, &buffers);     // its GLES texture, if GLES uses it: rendererGlBuffer
    pthread_spin_unlock(&bufferLock);
    if (!buf && later) {""",
     """    for (int attempt = 0; attempt < 20; attempt++) {
        pthread_spin_lock(&bufferLock);
        buf = LorieBufferList_findById(&buffers, id);
        if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id)))
            LorieBuffer_addToList(buf, &buffers);     // its GLES texture, if GLES uses it: rendererGlBuffer
        pthread_spin_unlock(&bufferLock);
        if (buf) break;
        usleep(5000);
    }
    if (!buf && later) {""",
     "tpresentcopy"),
    ("buffers: a Vulkan copy whose buffer may still come skipped at once", "renderer.c",
     """        src = rendererFindBuffer(entry.srcBufferId, &later);
        dst = rendererFindBuffer(entry.dstBufferId, &later);
        if ((!src || !dst) && later) {
            LORIE_DIAG_ADD(state, copiesLeftForBuffers, 1);
            break;      // P11-C3-C: a buffer that may still come - left queued, with the copies after it, for the next pass
        }
        if (!src || !dst)""",
     """        src = rendererFindBuffer(entry.srcBufferId, &later);
        dst = rendererFindBuffer(entry.dstBufferId, &later);
        if ((!src || !dst) && later && 0) {
            LORIE_DIAG_ADD(state, copiesLeftForBuffers, 1);
            break;      // P11-C3-C: a buffer that may still come - left queued, with the copies after it, for the next pass
        }
        if (!src || !dst)""",
     "tpresentcopy"),
    ("buffers: a GLES copy whose buffer may still come skipped at once", "renderer.c",
     """        src = rendererFindBuffer(entry.srcBufferId, &later);
        dst = rendererFindBuffer(entry.dstBufferId, &later);
        if ((!src || !dst) && later) {
            LORIE_DIAG_ADD(state, copiesLeftForBuffers, 1);
            break;      // P11-C3-C: a buffer that may still come - left queued, with the copies after it, for the next pass
        }
        if (!src)""",
     """        src = rendererFindBuffer(entry.srcBufferId, &later);
        dst = rendererFindBuffer(entry.dstBufferId, &later);
        if ((!src || !dst) && later && 0) {
            LORIE_DIAG_ADD(state, copiesLeftForBuffers, 1);
            break;      // P11-C3-C: a buffer that may still come - left queued, with the copies after it, for the next pass
        }
        if (!src)""",
     "tpresentcopy"),
    ("buffers: one given up on still waited for (never skipped)", "renderer.c",
     """        for (i = 0; i < buffersGivenUpCount; i++)
            if (buffersGivenUp[i] == id)
                *later = false;""",
     """        for (i = 0; i < buffersGivenUpCount; i++)
            if (buffersGivenUp[i] == id)
                *later = true;""",
     "tpresentcopy"),
]

# P11-C3-C: the freeze watchdog (TERMUX_X11_FRAME_FREEZE_DIAG=1)
CONTROLS += [
    ("freeze: time since the last frame taken for a freeze, whatever waits", "InitOutput.c",
     """    f->stallUs = x > r ? x : r;""",
     """    f->stallUs = x > r ? x : r;
    if (now - f->framesUs > f->stallUs)
        f->stallUs = now - f->framesUs;""",
     "tfreeze"),
    ("freeze: a frame asked for and not presented not watched (a stall taken for idle)", "InitOutput.c",
     """        if (s->askSeq != s->takenSeq && (u = lorieFreezeSince(now, s->askUs, f->framesUs)) >= LORIE_FREEZE_PERIOD_US)""",
     """        if (0 && s->askSeq != s->takenSeq && (u = lorieFreezeSince(now, s->askUs, f->framesUs)) >= LORIE_FREEZE_PERIOD_US)""",
     "tfreeze"),
    ("freeze: its end not tied to its begin", "InitOutput.c",
     """        f->moved = moved & f->beginPending;     // of what waited as it began, what moved (none: it is no longer waited for)""",
     """        f->moved = moved & f->beginPending;     // of what waited as it began, what moved (none: it is no longer waited for)
        f->id++;""",
     "tfreeze"),
    ("freeze: a level logged again and again", "InitOutput.c",
     """            if (!(f->levels & (1u << k)) && now - f->beginUs >= (uint64_t) (k + 1) * 1000000) {""",
     """            if (now - f->beginUs >= (uint64_t) (k + 1) * 1000000) {""",
     "tfreeze"),
    ("freeze: the X server's thread's stage not watched", "InitOutput.c",
     """    if (s->xStage != LORIE_X_LOOP && (u = lorieFreezeSince(now, s->xStageUs, 0)) >= LORIE_FREEZE_PERIOD_US)""",
     """    if (0 && s->xStage != LORIE_X_LOOP && (u = lorieFreezeSince(now, s->xStageUs, 0)) >= LORIE_FREEZE_PERIOD_US)""",
     "tfreeze"),
    ("freeze: the renderer's work watched with no window", "InitOutput.c",
     """    if (s->surface) {
        if (s->askSeq != s->takenSeq""",
     """    if (1) {
        if (s->askSeq != s->takenSeq""",
     "tfreeze"),
    ("freeze: the ask timed from the last one, not the first still waiting", "InitOutput.c",
     """    s->askUs = s->askSeq != s->takenSeq ? lorieAskUs(p, s->takenSeq + 1, s->askSeq) : 0;""",
     """    s->askUs = s->askSeq != s->takenSeq ? lorieAskUs(p, s->askSeq, s->askSeq) : 0;""",
     "tfreeze"),
    ("freeze: the renderer's stage kept with the option off", "renderer.c",
     """static void rendererStage(uint32_t stage) {
    if (state && state->backend.freezeDiag) {""",
     """static void rendererStage(uint32_t stage) {
    if (state) {""",
     "tpresentcopy"),
    ("freeze: Dispatch starts the request's timing while untimed", "xserver.patch",
     """+                        CARD64 timedUs = dispatchRequestTimed ? ddxRequestStart(client->index, client->majorOp, client->minorOp) : 0;""",
     """+                        CARD64 timedUs = ddxRequestStart(client->index, client->majorOp, client->minorOp) * (dispatchRequestTimed != 0);""",
     "tdispatchyield"),
]

# P11-C3-C: the frame gaps (TERMUX_X11_RENDERER_VK_FRAME_DIAG)
CONTROLS += [
    ("gaps: a long gap with a frame waiting taken as idle (the 250 ms rule back)", "renderer.c",
     """    if (wait >= LORIE_VK_GAP_US) {
        bool asleep""",
     """    if (0) {
        bool asleep""",
     "tpresentcopy"),
    ("gaps: a renderer asleep through the ask not told from a late one", "renderer.c",
     """        cause = asleep && wake * 2 >= wait ? LORIE_GAP_UNWOKEN : LORIE_GAP_LATE;""",
     """        cause = asleep && 0 ? LORIE_GAP_UNWOKEN : LORIE_GAP_LATE;""",
     "tpresentcopy"),
    ("gaps: the ask's wait from the last ask, not the first still waiting", "renderer.c",
     """        ft.firstAskUs = askSeq != ft.takenSeq ? lorieAskUs(&state->progress, ft.takenSeq + 1, askSeq) : 0;""",
     """        ft.firstAskUs = askSeq != ft.takenSeq ? lorieAskUs(&state->progress, askSeq, askSeq) : 0;""",
     "tpresentcopy"),
    ("gaps: the X server's ticks not looked at", "renderer.c",
     """        cause = (uint64_t) ticks * 50000 < gap ? LORIE_GAP_X_STALLED : LORIE_GAP_NONE;""",
     """        cause = (uint64_t) ticks * 0 > gap ? LORIE_GAP_X_STALLED : LORIE_GAP_NONE;""",
     "tpresentcopy"),
]

# P11-C3-C: the 5 second report outside the input lock
CONTROLS += [
    ("report: made in the timer again, under the input lock", "InitOutput.c",
     """    QueueWorkProc(lorieReportWorkProc, NULL, NULL);
    return 5000;""",
     """    lorieReport();
    return 5000;""",
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
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", OUT=gen)
    if file == "xserver.patch":
        path = os.path.join(work, file)
        shutil.copyfile(os.path.join(HERE, "..", "..", "app", "src", "main", "cpp", "patches", file), path)
        env["LORIE_XSERVER_PATCH"] = path
        if old.count("\n") != new.count("\n"):
            sys.exit("control %r: changes the patch's line count - its hunks would not apply" % what)
    else:
        path = os.path.join(src, file)
    text = open(path, encoding="utf-8").read()
    if text.count(old) != 1:
        sys.exit("control %r: its text is in %s %d times, not once - update the control" % (what, file, text.count(old)))
    open(path, "w", encoding="utf-8").write(text.replace(old, new))

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
