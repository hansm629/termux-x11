/* TVKFRAME: presenting (vk.c: renderer objects, swapchain, acquire, a frame, submit, present; gen.py "vk") on the
 * fake device of fakevk.h, which checks at every render pass and draw that an X server buffer is owned by the
 * renderer and in the layout of that use, and counts every object made and destroyed.
 *
 *   Renderer objects: made, or - when any one of them fails - none left behind and one fallback (FATAL_BACKEND,
 *   VK_DEVICE). Swapchain: R8G8B8A8 preferred, else B8G8R8A8, else VK_SURFACE_FORMAT; the window's size within
 *   the surface's limits; one image more than the minimum within the maximum; IDENTITY transform when allowed;
 *   opaque composite alpha when allowed; FIFO for smooth, else IMMEDIATE, MAILBOX or FIFO, the first the surface
 *   lists; the surface's currentExtent when it gives one, else the window's
 *   size; made again without leaking, destroyed completely (swapchain before surface); injection at surface or
 *   swapchain makes no call. Acquire: each result handled - the fence waited for and reset on success, nothing
 *   waited for on a timeout. Made again only when out of date: SUBOPTIMAL on every acquire and present keeps
 *   the one swapchain; out of date more than LORIE_VK_MAX_OUT_OF_DATE times without a frame presented in
 *   between is a failure, and a presented frame starts the count over. A frame: each X server buffer acquired once from
 *   FOREIGN, moved between uses, released once; draws counted only after the fence; a failed or injected
 *   submission moves no counter and acquires nothing; a fence that does not signal is waited out (device idle)
 *   before returning. Present: each result handled; the frame's fence waited for before it (lorieVkFrameSubmit), and
 *   what it waits for a semaphore a submission signalled - never one signalled twice. */
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
    /* P11-C4: the normal Vulkan startup makes neither Over pipeline; opt-in makes two format-specific
     * premultiplied Over pipelines that use the existing imported AHB render-pass and shader path. */
    CHECK(!r.overPipeline[0] && !r.overPipeline[1], "default renderer created optional XRender Over pipelines");
    device();
    st.backend.xrenderGpu = 1;
    {
        int overBefore = fakeOverPipelineCreates;
        AHardwareBuffer *overSrc = NULL, *overDst = NULL;
        struct lorie_vk_image *overSrcImage, *overDstImage;
        CHECK(lorieVkRendererCreate(&vk, &r, &st) && r.overPipeline[0] && r.overPipeline[1] &&
              fakeOverPipelineCreates == overBefore + 2,
              "opt-in Vulkan must make two premultiplied Over pipelines (made %d, wanted %d)",
              fakeOverPipelineCreates, overBefore + 2);
        for (int i = 0; i < 2; i++)
            for (int op = LORIE_GPU_COPY_OP_CLEAR; op < LORIE_GPU_COPY_OP_COUNT; op++)
                CHECK(r.pdPipeline[i][op] != VK_NULL_HANDLE, "missing opt-in Porter-Duff pipeline format %d op %d", i, op);
        overSrc = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, X_USAGE);
        overDst = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, X_USAGE);
        overSrcImage = lorieVkImport(&vk, &images, 81, overSrc, 0, &st);
        overDstImage = lorieVkImport(&vk, &images, 82, overDst, 0, &st);
        CHECK(overSrcImage && overDstImage, "opt-in Vulkan failed to import source/destination AHBs for Over");
        if (overSrcImage && overDstImage) {
            int initialDraws = emu.draws;
            CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS &&
                  lorieVkFrameOver(&vk, &r, overSrcImage, overDstImage, quads, 1, 0) &&
                  lorieVkFrameSubmit(&vk, &r, 0, false, &st) == VK_SUCCESS &&
                  emu.draws == initialDraws + 1 && emu.violations == 0,
                  "XRender Over did not issue a GPU draw into the imported AHB with correct ownership");
            /* Multiple blend operations into the same imported destination form a single command buffer:
             * LOAD of the subsequent pass requires the explicit COLOR_ATTACHMENT memory dependency. */
            initialDraws = emu.draws;
            CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS, "Porter-Duff batch begin failed");
            for (uint8_t op = 0; op < LORIE_GPU_COPY_OP_COUNT; op++)
                CHECK(lorieVkFramePorterDuff(&vk, &r, overSrcImage, overDstImage, quads, 1, 0, op),
                      "Porter-Duff op=%u failed to record a GPU draw", op);
            CHECK(lorieVkFrameSubmit(&vk, &r, 0, false, &st) == VK_SUCCESS &&
                  emu.draws == initialDraws + LORIE_GPU_COPY_OP_COUNT && emu.violations == 0,
                  "Porter-Duff 12-mode batch: draws=%d expected=%d, ownership violations=%d",
                  emu.draws, initialDraws + LORIE_GPU_COPY_OP_COUNT, emu.violations);
        }
        lorieVkImagesClear(&vk, &images);
        lorieVkRendererDestroy(&vk, &r);
        AHardwareBuffer_release(overSrc);
        AHardwareBuffer_release(overDst);
        CHECK(emu.live == 0, "opt-in Vulkan destroyed with live GPU objects");
        /* The later base-frame test checks exact push-constant order, independent of this opt-in draw. */
        pushes = 0;
    }
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
    CHECK(r.imageCount == 3 && r.extent.width == 1080 && r.extent.height == 2400, "swapchain: %u images", r.imageCount);
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
          emu.swapchainInfo.compositeAlpha == VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          "within the surface's limits: %ux%u, %u images, alpha %#x", emu.swapchainInfo.imageExtent.width,
          emu.swapchainInfo.imageExtent.height, emu.swapchainInfo.minImageCount, emu.swapchainInfo.compositeAlpha);
    lorieVkSwapchainDestroy(&vk, &r);
    /* the surface's size first; the window's only where the surface leaves it to the swapchain */
    {
        ANativeWindow other = { 1000, 2000 };
        device();
        CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
        CHECK(lorieVkSwapchainCreate(&vk, &r, &other, &st) && emu.swapchainInfo.imageExtent.width == 1080 &&
              emu.swapchainInfo.imageExtent.height == 2400, "window of another size: %ux%u, not currentExtent",
              emu.swapchainInfo.imageExtent.width, emu.swapchainInfo.imageExtent.height);
        lorieVkSwapchainDestroy(&vk, &r);
        emu.caps.currentExtent = (VkExtent2D) { UINT32_MAX, UINT32_MAX };
        CHECK(lorieVkSwapchainCreate(&vk, &r, &other, &st) && emu.swapchainInfo.imageExtent.width == 1000 &&
              emu.swapchainInfo.imageExtent.height == 2000, "undefined currentExtent: %ux%u, not the window's size",
              emu.swapchainInfo.imageExtent.width, emu.swapchainInfo.imageExtent.height);
        lorieVkSwapchainDestroy(&vk, &r);
    }
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
            CHECK(r.recreate == (t[k].result == VK_ERROR_OUT_OF_DATE_KHR), "acquire %d: recreate %d", t[k].result, r.recreate);
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

    /* WSI preference: default MAILBOX where available, explicit legacy
     * IMMEDIATE rollback, then FIFO. Smooth presentation always chooses FIFO. */
    {
        static const struct { bool smooth, legacy; VkPresentModeKHR modes[3]; uint32_t n; VkPresentModeKHR want; const char *name; } t[] = {
            { false, false, { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR }, 3, VK_PRESENT_MODE_MAILBOX_KHR, "MAILBOX" },
            { false, true, { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR }, 3, VK_PRESENT_MODE_IMMEDIATE_KHR, "IMMEDIATE" },
            { false, false, { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR }, 2, VK_PRESENT_MODE_IMMEDIATE_KHR, "IMMEDIATE" },
            { false, false, { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR }, 2, VK_PRESENT_MODE_MAILBOX_KHR, "MAILBOX" },
            { false, false, { VK_PRESENT_MODE_FIFO_KHR }, 1, VK_PRESENT_MODE_FIFO_KHR, "FIFO" },
            { true, true, { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR }, 3, VK_PRESENT_MODE_FIFO_KHR, "FIFO" },
        };
        for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
            device();
            st.backend.vkPreferImmediate = t[k].legacy;
            CHECK(lorieVkRendererCreate(&vk, &r, &st), "renderer objects not made");
            memcpy(emu.presentModes, t[k].modes, sizeof t[k].modes);
            emu.presentModeCount = t[k].n;
            r.smooth = t[k].smooth;
            CHECK(r.preferImmediate == t[k].legacy, "renderer did not honor IMMEDIATE rollback");
            CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st) && emu.swapchainInfo.presentMode == t[k].want && r.presentMode == t[k].want &&
                  !strcmp((const char *) st.diag.vkPresentMode, t[k].name) && lorieVkPresentModeFor(&vk, &r, t[k].smooth) == t[k].want,
                  "present mode %u (%s, %u listed): made with %d, logged %s", k, t[k].smooth ? "smooth" : "immediate", t[k].n,
                  emu.swapchainInfo.presentMode, (const char *) st.diag.vkPresentMode);
        }
        r.smooth = false;
        st.backend.vkPreferImmediate = 0;
    }

    /* fifty frames with every acquire and present SUBOPTIMAL (a swapchain not pre-rotated): one swapchain */
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st) && lorieVkSwapchainCreate(&vk, &r, &window, &st), "no swapchain");
    emu.acquireResult = emu.presentResult = VK_SUBOPTIMAL_KHR;
    for (int k = 0; k < 50; k++) {
        if (r.recreate)
            lorieVkSwapchainCreate(&vk, &r, &window, &st);
        CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK && lorieVkFrameBegin(&vk, &r) == VK_SUCCESS, "suboptimal frame %d", k);
        lorieVkFrameScreen(&vk, &r, index, NULL, &viewport, NULL, NULL, 0);
        CHECK(lorieVkFrameSubmit(&vk, &r, index, true, &st) == VK_SUCCESS && lorieVkPresent(&vk, &r, index, &st) == LORIE_VK_FRAME_OK,
              "suboptimal frame %d not presented", k);
    }
    CHECK(emu.swapchainsMade == 1 && !r.recreate && st.diag.vkSwapchainCreated == 1 && st.diag.vkAcquireSuboptimal == 50 &&
          st.diag.vkFramesPresented == 50 && st.diag.fallbackTotal == 0,
          "suboptimal: %d swapchains made for 50 frames (want 1), %u presented", emu.swapchainsMade, st.diag.vkFramesPresented);

    /* out of date: made again, until that stops helping */
    device();
    CHECK(lorieVkRendererCreate(&vk, &r, &st) && lorieVkSwapchainCreate(&vk, &r, &window, &st), "no swapchain");
    emu.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
    CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_RECREATE && r.recreate, "out of date: not made again");
    CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st) && !r.recreate && emu.swapchainsMade == 2, "out of date: not made again");
    emu.acquireResult = VK_SUCCESS;
    CHECK(lorieVkAcquire(&vk, &r, &index, &st) == LORIE_VK_FRAME_OK && lorieVkFrameBegin(&vk, &r) == VK_SUCCESS, "after remaking");
    lorieVkFrameScreen(&vk, &r, index, NULL, &viewport, NULL, NULL, 0);
    CHECK(lorieVkFrameSubmit(&vk, &r, index, true, &st) == VK_SUCCESS && lorieVkPresent(&vk, &r, index, &st) == LORIE_VK_FRAME_OK &&
          r.outOfDateStreak == 0, "presented after remaking: count not started over");
    emu.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
    {
        int got = LORIE_VK_FRAME_RECREATE, made = emu.swapchainsMade, tries = 0;
        while (got == LORIE_VK_FRAME_RECREATE && tries++ < 10) {
            got = lorieVkAcquire(&vk, &r, &index, &st);
            if (got == LORIE_VK_FRAME_RECREATE)
                CHECK(lorieVkSwapchainCreate(&vk, &r, &window, &st), "out of date: not made again");
        }
        CHECK(got == LORIE_VK_FRAME_FATAL && emu.swapchainsMade - made == LORIE_VK_MAX_OUT_OF_DATE &&
              fallbackOnce(LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SWAPCHAIN) && st.diag.lastVkResult == VK_ERROR_OUT_OF_DATE_KHR,
              "always out of date: %d more swapchains, result %d (want %d swapchains, then a VK_SWAPCHAIN failure)",
              emu.swapchainsMade - made, got, LORIE_VK_MAX_OUT_OF_DATE);
    }
    emu.acquireResult = VK_SUCCESS;

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
                /* a frame into image 0 first: its semaphore is what the present waits for */
                CHECK(lorieVkFrameBegin(&vk, &r) == VK_SUCCESS && lorieVkFrameSubmit(&vk, &r, 0, true, &st) == VK_SUCCESS,
                      "present %d: no frame submitted before it", t[k].result);
                emu.presentResult = t[k].result;
                CHECK(lorieVkPresent(&vk, &r, 0, &st) == t[k].want && (st.diag.vkFramesPresented == presented + 1) == t[k].counted &&
                      r.recreate == (t[k].result == VK_ERROR_OUT_OF_DATE_KHR), "present %d: handled wrongly", t[k].result);
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

    CHECK(emu.earlyPresents == 0, "%d presents before their frame's fence was waited for", emu.earlyPresents);
    printf("TVKFRAME swapchain and frames on a small device: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
