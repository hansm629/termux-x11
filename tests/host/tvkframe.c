/* TVKFRAME: presenting (vk.c: renderer objects, swapchain, acquire, a frame, submit, present; gen.py "vk") on the
 * fake device of fakevk.h, which checks at every render pass and draw that an X server buffer is owned by the
 * renderer and in the layout of that use, and counts every object made and destroyed.
 *
 *   Renderer objects: made, or - when any one of them fails - none left behind and one fallback (FATAL_BACKEND,
 *   VK_DEVICE). Swapchain: R8G8B8A8 preferred, else B8G8R8A8, else VK_SURFACE_FORMAT; the window's size within
 *   the surface's limits; one image more than the minimum within the maximum; IDENTITY transform when allowed;
 *   opaque composite alpha when allowed; FIFO; made again without leaking, destroyed completely (swapchain before
 *   surface); injection at surface or swapchain makes no call. Acquire: each result handled - the fence waited
 *   for and reset on success, nothing waited for on a timeout. A frame: each X server buffer acquired once from
 *   FOREIGN, moved between uses, released once; draws counted only after the fence; a failed or injected
 *   submission moves no counter and acquires nothing; a fence that does not signal is waited out (device idle)
 *   before returning. Present: each result handled; the frame's fence waited for before it. */
#include "fakevk.h"

static struct lorie_shared_server_state st;
static struct lorie_vk vk;
static struct lorie_vk_images images;
static struct lorie_vk_renderer r;
static ANativeWindow window = { 1080, 2400 };

#define X_USAGE (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)

static void device(void) {
    lorieVkImagesClear(&vk, &images);
    lorieVkRendererDestroy(&vk, &r);
    if (vk.device)
        lorieVkDestroyDevice(&vk);
    memset(&st, 0, sizeof st);
    good();
    CHECK(lorieVkCreateDevice(&vk, fGetInstanceProcAddr, &st), "no device");
    vk.verifiedFormats = 7;
    memset(&imp, 0, sizeof imp);
}

static bool fallbackOnce(uint8_t cls, uint8_t reason) {
    return st.diag.fallbackTotal == 1 && st.diag.fallbackByClass[cls] == 1 && st.diag.fallbackByReason[reason] == 1;
}

static AHardwareBuffer *buffer(uint32_t format, uint64_t usage) {
    AHardwareBuffer_Desc d = { .width = 640, .height = 480, .layers = 1, .format = format, .usage = usage };
    AHardwareBuffer *b = NULL;
    CHECK(AHardwareBuffer_allocate(&d, &b) == 0, "no buffer");
    return b;
}

int main(void) {
    long objects;
    int live;
    uint32_t index = 0;
    struct lorie_vk_quad quads[2] = { { { -1, -1, 0, 0 }, { 0, 0, .5f, .5f }, 0 }, { { 0, 0, 1, 1 }, { .5f, .5f, 1, 1 }, 1 } };
    struct lorie_vk_quad rootQuad = { { -1, -1, 1, 1 }, { 0, 0, 1, 1 }, LORIE_VK_QUAD_OPAQUE | LORIE_VK_QUAD_SWIZZLE };
    struct lorie_vk_quad cursorQuad = { { 0, 0, .1f, .1f }, { 0, 0, 1, 1 }, 0 };
    VkRect2D viewport = { { 0, 0 }, { 1080, 2400 } };
    static uint32_t bits[32 * 32];

    /* renderer objects: all made, or none */
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st) && st.diag.fallbackTotal == 0, "renderer objects not made");
    objects = emu.objects;
    CHECK(objects > 5 && emu.live == objects && emu.pools == 1 && emu.fences == 2 && emu.buffers == 1 && emu.mapped == 1,
          "renderer objects: %ld made, %d alive", objects, emu.live);
    lorieVkRendererDestroy(&vk, &r);
    CHECK(emu.live == 0 && emu.pools == 0 && emu.fences == 0 && emu.buffers == 0 && emu.mapped == 0 && imp.memories == 0,
          "renderer objects left: %d objects, %d pools, %d fences, %d buffers", emu.live, emu.pools, emu.fences, emu.buffers);
    for (long k = 1; k <= objects; k++) {
        device();
        emu.failObject = k;
        CHECK(!lorieVkRendererCreate(&vk, &r, &st) && fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_DEVICE),
              "renderer object %ld failing: not a failure recorded once as VK_DEVICE", k);
        CHECK(emu.live == 0 && emu.pools == 0 && emu.fences == 0 && emu.buffers == 0 && imp.memories == 0,
              "renderer object %ld failing: %d objects left", k, emu.live);
    }

    /* the swapchain */
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
    live = emu.live;
    CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st), "swapchain not made");
    CHECK(emu.surfaces == 1 && emu.swapchains == 1 && st.diag.vkSurfaceCreated == 1 && st.diag.vkSwapchainCreated == 1 &&
          !strcmp((const char *) st.diag.vkPresentMode, "FIFO"), "swapchain: surfaces %d swapchains %d", emu.surfaces, emu.swapchains);
    CHECK(emu.swapchainInfo.imageFormat == VK_FORMAT_R8G8B8A8_UNORM && emu.swapchainInfo.imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
          emu.swapchainInfo.imageExtent.width == 1080 && emu.swapchainInfo.imageExtent.height == 2400 && emu.swapchainInfo.minImageCount == 3 &&
          emu.swapchainInfo.preTransform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
          emu.swapchainInfo.compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR && emu.swapchainInfo.presentMode == VK_PRESENT_MODE_FIFO_KHR &&
          emu.swapchainInfo.clipped && emu.swapchainInfo.imageUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT && !emu.swapchainInfo.oldSwapchain,
          "swapchain made with format %d, %ux%u, %u images, transform %#x, alpha %#x, mode %d", emu.swapchainInfo.imageFormat,
          emu.swapchainInfo.imageExtent.width, emu.swapchainInfo.imageExtent.height, emu.swapchainInfo.minImageCount,
          emu.swapchainInfo.preTransform, emu.swapchainInfo.compositeAlpha, emu.swapchainInfo.presentMode);
    CHECK(r.imageCount == 3 && r.windowWidth == 1080 && r.windowHeight == 2400, "swapchain: %u images", r.imageCount);
    {
        VkSwapchainKHR first = r.swapchain;
        int afterFirst = emu.live;
        CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st) && emu.swapchainInfo.oldSwapchain == first && emu.swapchains == 1 &&
              emu.surfaces == 1 && emu.live == afterFirst, "made again: old %p, %d swapchains, %d objects (was %d)",
              (void *) emu.swapchainInfo.oldSwapchain, emu.swapchains, emu.live, afterFirst);
    }
    lorieVkSwapchainDestroy(&vk, &r);
    CHECK(emu.swapchains == 0 && emu.surfaces == 0 && emu.live == live, "swapchain destroyed: %d swapchains %d surfaces %d objects (want %d)",
          emu.swapchains, emu.surfaces, emu.live, live);
    {
        static const struct { const char *what; VkFormat formats[2]; uint32_t n; VkFormat want; } t[] = {
            { "B8G8R8A8 only", { VK_FORMAT_B8G8R8A8_UNORM }, 1, VK_FORMAT_B8G8R8A8_UNORM },
            { "R8G8B8A8 listed second", { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM }, 2, VK_FORMAT_R8G8B8A8_UNORM },
            { "no usable format", { VK_FORMAT_R5G6B5_UNORM_PACK16 }, 1, VK_FORMAT_UNDEFINED },
        };
        for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
            device();
            CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
            emu.surfaceFormatCount = t[k].n;
            for (uint32_t j = 0; j < t[k].n; j++)
                emu.surfaceFormats[j] = (VkSurfaceFormatKHR) { t[k].formats[j], VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
            bool ok = lorieVkSwapchainCreate(&vk, &r, &window, &st);
            if (t[k].want)
                CHECK(ok && emu.swapchainInfo.imageFormat == t[k].want, "%s: format %d", t[k].what, emu.swapchainInfo.imageFormat);
            else
                CHECK(!ok && fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SURFACE_FORMAT), "%s: not VK_SURFACE_FORMAT", t[k].what);
            lorieVkSwapchainDestroy(&vk, &r);
            CHECK(emu.swapchains == 0 && emu.surfaces == 0, "%s: left behind", t[k].what);
        }
    }
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
    emu.caps.maxImageExtent = (VkExtent2D) { 1000, 2000 };
    emu.caps.maxImageCount = 2;
    emu.caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st) && emu.swapchainInfo.imageExtent.width == 1000 &&
          emu.swapchainInfo.imageExtent.height == 2000 && emu.swapchainInfo.minImageCount == 2 &&
          emu.swapchainInfo.compositeAlpha == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR && r.windowWidth == 1080,
          "within the surface's limits: %ux%u, %u images, alpha %#x", emu.swapchainInfo.imageExtent.width,
          emu.swapchainInfo.imageExtent.height, emu.swapchainInfo.minImageCount, emu.swapchainInfo.compositeAlpha);
    lorieVkSwapchainDestroy(&vk, &r);
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
    emu.surfaceUnsupported = true;
    CHECK(!lorieVkSwapchainCreate(&vk, &r, &window, &st) && fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SURFACE),
          "unsupported surface: not VK_SURFACE");
    lorieVkSwapchainDestroy(&vk, &r);
    CHECK(emu.surfaces == 0, "unsupported surface: left behind");
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
    st.backend.vkFailAt = LORIE_VK_FAIL_AT_SURFACE;
    CHECK(!lorieVkSwapchainCreate(&vk, &r, &window, &st) && emu.surfaces == 0 &&
          fallbackOnce(LORIE_FALLBACK_CLASS_INJECTED, LORIE_FALLBACK_VK_SURFACE), "injected at surface");
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
    st.backend.vkFailAt = LORIE_VK_FAIL_AT_SWAPCHAIN;
    CHECK(!lorieVkSwapchainCreate(&vk, &r, &window, &st) && emu.swapchains == 0 && emu.objects == objects + 3 /* screen pass, 2 pipelines */ &&
          fallbackOnce(LORIE_FALLBACK_CLASS_INJECTED, LORIE_FALLBACK_VK_SWAPCHAIN), "injected at swapchain: a swapchain made");
    lorieVkSwapchainDestroy(&vk, &r);
    CHECK(emu.surfaces == 0, "injected at swapchain: surface left behind");

    /* acquire: each result */
    {
        static const struct { VkResult result; int want; uint8_t reason; } t[] = {
            { VK_SUCCESS, LORIE_VK_FRAME_OK, 0 }, { VK_SUBOPTIMAL_KHR, LORIE_VK_FRAME_OK, 0 },
            { VK_TIMEOUT, LORIE_VK_FRAME_SKIP, 0 }, { VK_NOT_READY, LORIE_VK_FRAME_SKIP, 0 },
            { VK_ERROR_OUT_OF_DATE_KHR, LORIE_VK_FRAME_RECREATE, 0 },
            { VK_ERROR_DEVICE_LOST, LORIE_VK_FRAME_FATAL, LORIE_FALLBACK_VK_DEVICE_LOST },
            { VK_ERROR_SURFACE_LOST_KHR, LORIE_VK_FRAME_FATAL, LORIE_FALLBACK_VK_SWAPCHAIN },
        };
        for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
            device();
            CHECK(lorieVkRendererCreate(&vk, &r, &st) && lorieVkSwapchainCreate(&vk, &r, &window, &st), "no swapchain");
            emu.acquireResult = t[k].result;
            int got = lorieVkAcquire(&vk, &r, &index, &st);
            CHECK(got == t[k].want, "acquire %d: %d, not %d", t[k].result, got, t[k].want);
            CHECK(!*signaled(r.acquireFence), "acquire %d: the fence left signalled", t[k].result);
            CHECK(r.recreate == (t[k].result == VK_SUBOPTIMAL_KHR || t[k].result == VK_ERROR_OUT_OF_DATE_KHR),
                  "acquire %d: recreate %d", t[k].result, r.recreate);
            CHECK(t[k].reason ? fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, t[k].reason) : st.diag.fallbackTotal == 0,
                  "acquire %d: fallback total %u reason %u", t[k].result, st.diag.fallbackTotal, st.diag.lastFallbackReason);
            CHECK(st.diag.vkAcquireSuccess == (t[k].result == VK_SUCCESS) && st.diag.vkAcquireSuboptimal == (t[k].result == VK_SUBOPTIMAL_KHR) &&
                  st.diag.vkAcquireTimeout == (t[k].result == VK_TIMEOUT) && st.diag.vkAcquireNotReady == (t[k].result == VK_NOT_READY) &&
                  st.diag.vkAcquireOutOfDate == (t[k].result == VK_ERROR_OUT_OF_DATE_KHR), "acquire %d: counters", t[k].result);
            emu.acquireResult = VK_SUCCESS;
            if (t[k].want == LORIE_VK_FRAME_OK)
                CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK && emu.violations == 0,
                      "acquire %d: the next acquire got a signalled fence", t[k].result);
        }
        device();
        CHECK(lorieVkRendererCreate(&vk, &r, &st) && lorieVkSwapchainCreate(&vk, &r, &window, &st), "no swapchain");
        emu.fenceResult = VK_TIMEOUT;
        CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_FATAL && fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SWAPCHAIN),
              "acquire whose fence never signals: not a failure");
    }

    /* a frame: a copy from an RGBA buffer into the root (RGBX), the cursor, the root on screen */
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st) && lorieVkSwapchainCreate(&vk, &r, &window, &st), "no swapchain");
    {
        AHardwareBuffer *src = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, X_USAGE), *rootB = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, X_USAGE);
        AHardwareBuffer *sampledOnly = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE);
        struct lorie_vk_image *s = lorieVkImport(&vk, &images, 1, src, 0, &st), *root = lorieVkImport(&vk, &images, 2, rootB, 0, &st);
        struct lorie_vk_image *ro = lorieVkImport(&vk, &images, 3, sampledOnly, 0, &st);
        int liveBefore;

        CHECK(s && root && ro, "frame: buffers not imported");
        CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK, "frame: no image");
        CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS, "frame: not begun");
        CHECK(!lorieVkFrameCopy(&vk, &r, root, root, quads, 2, 0) && !lorieVkFrameCopy(&vk, &r, s, ro, quads, 2, 0),
              "frame: a copy into itself or into a buffer that cannot be rendered to");
        CHECK(lorieVkFrameCopy(&vk, &r, s, root, quads, 2, 1), "frame: copy not recorded");
        CHECK(lorieVkFrameCursor(&vk, &r, bits, 32, 32), "frame: cursor not refilled");
        lorieVkFrameScreen(&vk, &r, index, root, &viewport, &rootQuad, &cursorQuad, 0);
        CHECK(st.diag.vkQueueSubmits == 0 && st.diag.vkImportedSourceDraws == 0, "frame: counted before its submission");
        CHECK(lorieVkFrameSubmit(&vk, &r, index, true, &st) == VK_SUCCESS, "frame: not submitted");
        CHECK(emu.violations == 0, "frame: %d violations", emu.violations);
        CHECK(emu.acquires == 2 && emu.releases == 2 && emu.acquiresFromUndefined == 0 && !src->owned && !rootB->owned,
              "frame: %d acquires, %d releases (want each buffer once)", emu.acquires, emu.releases);
        CHECK(emu.passesIntoImported == 1 && emu.drawsIntoImported == 2 && emu.drawsFromImported == 3 && emu.draws == 4,
              "frame: %d passes into, %d draws into, %d draws from imported, %d draws", emu.passesIntoImported,
              emu.drawsIntoImported, emu.drawsFromImported, emu.draws);
        CHECK(st.diag.vkQueueSubmits == 1 && st.diag.vkImportedSourceDraws == 3 && st.diag.vkImportedDestinationDraws == 2 &&
              st.diag.vkRenderCommands == 4, "frame: submits %u, source draws %u, destination draws %u, render commands %u",
              st.diag.vkQueueSubmits, st.diag.vkImportedSourceDraws, st.diag.vkImportedDestinationDraws, st.diag.vkRenderCommands);
        CHECK(pushes == 4 && !memcmp(&pushed[0], &quads[0], sizeof *quads) && !memcmp(&pushed[1], &quads[1], sizeof *quads) &&
              !memcmp(&pushed[2], &rootQuad, sizeof rootQuad) && !memcmp(&pushed[3], &cursorQuad, sizeof cursorQuad),
              "frame: %d quads pushed, not the ones given", pushes);
        CHECK(lorieVkPresent(&vk, &r, index, &st) == LORIE_VK_FRAME_OK && emu.presents == 1 && st.diag.vkQueuePresentSuccess == 1 &&
              st.diag.vkFramesPresented == 1, "frame: not presented");

        /* forty frames more: descriptor sets and command buffer reused, nothing piles up */
        liveBefore = emu.live;
        for (int k = 0; k < 40; k++) {
            CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK && lorieVkFrameBegin(&vk, &r) == VK_SUCCESS &&
                  lorieVkFrameCopy(&vk, &r, s, root, quads, 2, 0), "frame %d not recorded", k);
            lorieVkFrameScreen(&vk, &r, index, root, &viewport, &rootQuad, NULL, 0);
            CHECK(lorieVkFrameSubmit(&vk, &r, index, true, &st) == VK_SUCCESS && lorieVkPresent(&vk, &r, index, &st) == LORIE_VK_FRAME_OK,
                  "frame %d not done", k);
        }
        CHECK(emu.live == liveBefore && emu.violations == 0 && emu.acquires == 2 * 41, "forty frames: %d objects (was %d), %d violations",
              emu.live, liveBefore, emu.violations);

        /* the copies alone: nothing for the screen, no semaphore */
        CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS && lorieVkFrameCopy(&vk, &r, s, root, quads, 1, 0) &&
              lorieVkFrameSubmit(&vk, &r, 0, false, &st) == VK_SUCCESS && emu.violations == 0, "copies alone not done");

        /* a black frame: no X server buffer */
        {
            int acquires = emu.acquires;
            CHECK(lorieVkAcquire(&vk, &r, &index, NULL) == LORIE_VK_FRAME_OK && lorieVkFrameBegin(&vk, &r) == VK_SUCCESS, "black frame");
            lorieVkFrameScreen(&vk, &r, index, NULL, &viewport, NULL, NULL, 0);
            CHECK(lorieVkFrameSubmit(&vk, &r, index, true, NULL) == VK_SUCCESS && emu.acquires == acquires, "black frame used a buffer");
            lorieVkPresent(&vk, &r, index, NULL);
        }

        /* a failed submission: nothing changed hands, nothing counted */
        {
            uint32_t submits = st.diag.vkQueueSubmits;
            int acquires = emu.acquires;
            CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK && lorieVkFrameBegin(&vk, &r) == VK_SUCCESS &&
                  lorieVkFrameCopy(&vk, &r, s, root, quads, 2, 0), "failed submission: not recorded");
            emu.submitResult = VK_ERROR_DEVICE_LOST;
            CHECK(lorieVkFrameSubmit(&vk, &r, index, true, &st) == VK_ERROR_DEVICE_LOST && st.diag.vkQueueSubmits == submits &&
                  emu.acquires == acquires && st.diag.lastFallbackReason == LORIE_FALLBACK_VK_DEVICE_LOST,
                  "failed submission: counted, or buffers acquired, or not VK_DEVICE_LOST");
            emu.submitResult = VK_SUCCESS;
        }
        /* a fence that does not signal in time: the device waited out before returning */
        {
            uint32_t submits = st.diag.vkQueueSubmits;
            int waits = emu.deviceWaits;
            CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS && lorieVkFrameCopy(&vk, &r, s, root, quads, 2, 0), "late fence: not recorded");
            emu.fenceResult = VK_TIMEOUT;
            CHECK(lorieVkFrameSubmit(&vk, &r, 0, false, &st) == VK_TIMEOUT && emu.deviceWaits == waits + 1 &&
                  st.diag.vkQueueSubmits == submits && st.diag.lastFallbackReason == LORIE_FALLBACK_VK_SUBMIT && !emu.fencePending,
                  "late fence: returned before the device was idle, or counted");
            emu.fenceResult = VK_SUCCESS;
            CHECK(!*signaled(r.frameFence), "late fence: left signalled after the device was waited out");
        }
        /* injected at submit: no submission */
        {
            int acquires = emu.acquires;
            st.backend.vkFailAt = LORIE_VK_FAIL_AT_SUBMIT;
            CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS && lorieVkFrameCopy(&vk, &r, s, root, quads, 2, 0) &&
                  lorieVkFrameSubmit(&vk, &r, 0, false, &st) != VK_SUCCESS && emu.acquires == acquires &&
                  st.diag.lastFallbackClass == LORIE_FALLBACK_CLASS_INJECTED && st.diag.lastFallbackReason == LORIE_FALLBACK_VK_SUBMIT,
                  "injected at submit: submitted, or not INJECTED VK_SUBMIT");
            st.backend.vkFailAt = LORIE_VK_FAIL_AT_NONE;
        }
        /* present: each result */
        {
            static const struct { VkResult result; int want; bool counted; } t[] = {
                { VK_SUBOPTIMAL_KHR, LORIE_VK_FRAME_OK, true }, { VK_ERROR_OUT_OF_DATE_KHR, LORIE_VK_FRAME_RECREATE, false },
                { VK_ERROR_SURFACE_LOST_KHR, LORIE_VK_FRAME_FATAL, false },
            };
            for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
                uint32_t presented = st.diag.vkFramesPresented;
                r.recreate = false;
                emu.presentResult = t[k].result;
                CHECK(lorieVkPresent(&vk, &r, 0, &st) == t[k].want && (st.diag.vkFramesPresented == presented + 1) == t[k].counted &&
                      r.recreate == (t[k].result != VK_ERROR_SURFACE_LOST_KHR), "present %d: handled wrongly", t[k].result);
            }
            emu.presentResult = VK_SUCCESS;
            st.backend.vkFailAt = LORIE_VK_FAIL_AT_PRESENT;
            int presents = emu.presents;
            CHECK(lorieVkPresent(&vk, &r, 0, &st) == LORIE_VK_FRAME_FATAL && emu.presents == presents &&
                  st.diag.lastFallbackClass == LORIE_FALLBACK_CLASS_INJECTED && st.diag.lastFallbackReason == LORIE_FALLBACK_VK_PRESENT,
                  "injected at present: presented, or not INJECTED VK_PRESENT");
            st.backend.vkFailAt = LORIE_VK_FAIL_AT_NONE;
        }

        /* an image removed: its framebuffer too */
        {
            int before = emu.live;
            lorieVkImageRemove(&vk, &images, 2);
            CHECK(emu.live == before - 1, "removed root: its framebuffer left (%d -> %d objects)", before, emu.live);
        }
        lorieVkImagesClear(&vk, &images);
        AHardwareBuffer_release(src);
        AHardwareBuffer_release(rootB);
        AHardwareBuffer_release(sampledOnly);
    }
    lorieVkRendererDestroy(&vk, &r);
    CHECK(emu.live == 0 && emu.swapchains == 0 && emu.surfaces == 0 && emu.fences == 0 && emu.pools == 0 && emu.buffers == 0 &&
          imp.images == 0 && imp.memories == 0 && imp.views == 0 && emu.ahbLive == 0,
          "after all: %d objects, %d swapchains, %d surfaces, %d fences, %d images, %d memories, %d views, %d buffers left",
          emu.live, emu.swapchains, emu.surfaces, emu.fences, imp.images, imp.memories, imp.views, emu.ahbLive);
    lorieVkDestroyDevice(&vk);

    printf("TVKFRAME swapchain and frames on a small device: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
