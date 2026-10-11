#!/usr/bin/env python3
"""Writes each test's <name>_src.inc from the sources as they are now, so the tests exercise the real code.

usage: gen.py <lorie source dir> <output dir> <3c98ffa lorie source dir>

The third directory holds InitOutput.c as it is at 3c98ffa (run.sh takes it from git): the code that tests
comparing against 3c98ffa's behaviour run side by side with the current code.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract import func, macro, span

L, OUT, BASE = sys.argv[1], sys.argv[2], sys.argv[3]
I, R, H, B = (os.path.join(L, f) for f in ("InitOutput.c", "renderer.c", "lorie.h", "buffer.c"))
VC, VH = os.path.join(L, "vk.c"), os.path.join(L, "vk.h")
FAILAT = lambda: span(I, "static const char *const lorieVkFailAtNames[] = {", '"lorieVkFailAtNames is LORIE_VK_FAIL_POINTS");')
BI = os.path.join(BASE, "InitOutput.c")
XS = os.path.join(HERE, "..", "..", "app", "src", "main", "cpp", "xserver")

def opt(make, marker, path):
    """What make() extracts, if `path` has `marker` - so a recipe also builds against code from before the
    function existed, which is how a test is shown to fail there."""
    return make() if marker in open(path, encoding="utf-8").read() else ""

def own(path, ownership):
    """The pixmap private and the pixmap paths that take or drop a CPU lock, in the order InitOutput.c has them,
    with the ownership helpers (`ownership`, absent at 3c98ffa) ahead of them."""
    return (span(path, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + macro(path, "LORIE_PIXMAP_PRIV_FROM_PIXMAP")
        + "".join(func(path, f) for f in ownership)
        + "".join(func(path, f) for f in (
            "static LorieBuffer *lorieEnsureGpuSampleable(PixmapPtr pixmap, int8_t type) {",
            "static LoriePixmapPriv* lorieRootWindowPixmapPriv(void) {",
            "void drawSquare(int x, int y, int l, uint32_t color, uint32_t stride, uint32_t* pixels) {",
            "Bool drawSquares() {",
            "void lorieSetWindowPixmap(WindowPtr pWindow, PixmapPtr newPixmap) {",
            "void *lorieCreatePixmap(",
            "void lorieExaDestroyPixmap(",
            "static inline __always_inline Bool lorieNeedsGpuLock(",
            "Bool loriePrepareAccess(PixmapPtr pPix, int index) {",
            "void lorieFinishAccess(PixmapPtr pPix, int index) {")))

recipes = {
    # the renderer diagnostics, as lorie.h declares them and the X server logs them - tdiaglog.c
    "diagtypes": lambda: span(H, "enum { LORIE_ACTIVE_NONE = 0", "__ATOMIC_RELAXED); } while (0)"),
    "diaglog": lambda: span(I, "static const char *lorieActiveName(uint8_t a) {", "#undef LORIE_NAME") + FAILAT()
        + func(I, "static void lorieLogRendererDiag(void) {"),
    # AHB CPU ownership - townership.c: xserver's list, EXA's access indices, then every pixmap path that takes
    # or drops a CPU lock, as 3c98ffa has them ("own_base") and as they are now, with the handshake ("own_new")
    "xorglist": lambda: open(os.path.join(XS, "include", "list.h"), encoding="utf-8").read(),
    "own_consts": lambda: macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_DEST")
        + macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_SRC")
        + macro(I, "CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED"),
    "own_base": lambda: own(BI, []),
    "own_new": lambda: own(I, [
        "static Bool lorieAhbPerAccess(void) {",
        "static Bool lorieIsAhb(LoriePixmapPriv *priv) {",
        "static void lorieHoldPersistent(LoriePixmapPriv *priv) {",
        "static void lorieDropPersistent(LoriePixmapPriv *priv) {",
        "static void lorieApplyOwnership(uint8_t mode, uint32_t seq) {",
        "static void lorieProcessOwnershipRequest(void) {",
        "static void lorieResetOwnership(void) {",
        "static Bool lorieRendererGoneWorkProc(",
        "void lorieRendererDisconnected(void) {",
        "static uint8_t lorieRequestedBackend(const char *value, volatile uint8_t *invalid) {",
    ]) + FAILAT() + func(I, "static uint8_t lorieRequestedVkFailAt(const char *value, volatile uint8_t *invalid) {")
        + func(I, "static uint8_t lorieRequestedVkSelftest(const char *value, volatile uint8_t *invalid) {"),
    # root double buffering's swap - townership.c
    "rootswap": lambda: func(I, "static Bool lorieRootSwap(LoriePixmapPriv *priv) {"),
    # the Vulkan backend: instance and device, AHardwareBuffer import - tvkinit.c, tvkimport.c (fakevk.h)
    "vk": lambda: span(VH, "#define LORIE_VK_GLOBAL_FNS(X)", "    uint8_t verifiedFormats;    /* AHB formats the ownership self-test passed: 1 RGBA, 2 RGBX, 4 BGRA */\n};")
        + span(VH, "struct lorie_vk_image {", "    VkResult lastResult;\n};")
        + span(VH, "enum lorie_vk_use {", "    VkPipelineStageFlags srcStage, dstStage;\n};")
        + span(VH, "#define LORIE_VK_MAX_SWAPCHAIN_IMAGES 8", "enum { LORIE_VK_FRAME_OK, LORIE_VK_FRAME_SKIP, LORIE_VK_FRAME_RECREATE, LORIE_VK_FRAME_FATAL };")
        + span(VC, "static const char *const lorieVkInstanceExtensions[] = {", "#define LORIE_VK_MAX_DEVICES 8")
        + "".join(func(VC, f) for f in (
            "void lorieVkFallback(volatile struct lorie_shared_server_state *st, uint8_t cls, uint8_t reason, VkResult result) {",
            "bool lorieVkInjected(volatile struct lorie_shared_server_state *st, uint8_t point, uint8_t reason) {",
            "static bool lorieVkHasExtension(const VkExtensionProperties *ext, uint32_t count, const char *name) {",
            "static uint8_t lorieVkDeviceUnsuitable(struct lorie_vk *vk, VkPhysicalDevice pd, uint32_t *queueFamily) {",
            "void lorieVkDestroyDevice(struct lorie_vk *vk) {",
            "bool lorieVkCreateDevice(struct lorie_vk *vk, PFN_vkGetInstanceProcAddr gipa, volatile struct lorie_shared_server_state *st) {",
            "VkImageUsageFlags lorieVkAhbImageUsage(uint64_t ahbUsage) {",
            "struct lorie_vk_image *lorieVkImageFind(struct lorie_vk_images *images, uint64_t id) {",
            "static void lorieVkImageDestroy(struct lorie_vk *vk, struct lorie_vk_image *img) {",
            "static void lorieVkImportFailed(struct lorie_vk_images *images, volatile struct lorie_shared_server_state *st,",
            "struct lorie_vk_image *lorieVkImport(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id,",
            "void lorieVkImageRemove(struct lorie_vk *vk, struct lorie_vk_images *images, uint64_t id) {",
            "void lorieVkImagesClear(struct lorie_vk *vk, struct lorie_vk_images *images) {",
            "static void lorieVkUseScope(enum lorie_vk_use use, VkImageLayout *layout, VkPipelineStageFlags *stage,",
            "static struct lorie_vk_barrier lorieVkBarrier(VkImage image) {",
            "struct lorie_vk_barrier lorieVkAcquireBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use) {",
            "struct lorie_vk_barrier lorieVkReleaseBarrier(const struct lorie_vk *vk, VkImage image, enum lorie_vk_use use) {",
            "struct lorie_vk_barrier lorieVkUseBarrier(VkImage image, enum lorie_vk_use from, enum lorie_vk_use to) {"))
        + macro(VH, "LORIE_VK_SELFTEST_ITERATIONS") + macro(VH, "LORIE_VK_SELFTEST_QUICK_ITERATIONS")
        + span(VC, "#define LORIE_VK_SELFTEST_WIDTH 67", "    lorieVkFallback(st, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SELFTEST, r);\n    return false;\n}")
        + span(VC, "/* ---- presenting ---- */", "LORIE_FALLBACK_VK_PRESENT, res);\n            return LORIE_VK_FRAME_FATAL;\n    }\n}"),
    # the renderer's choice of backend and its side of the ownership handshake - tvkrenderer.c
    "vkrenderer": lambda: span(R, "enum { RENDERER_USE_GLES, RENDERER_USE_VULKAN, RENDERER_USE_NONE };", "RENDERER_USE_NONE };")
        + func(R, "static int rendererBackendFor(uint8_t requested, bool vulkanFailed) {")
        + func(R, "static bool rendererGlesEager(void) {")
        + func(R, "static void rendererTimespecAddUs(struct timespec *ts, int64_t us) {")
        + func(R, "static bool rendererVkOwnership(uint8_t mode) {")
        + func(R, "static bool rendererVkOwnedLocked(void) {")
        + func(R, "static bool rendererVkTickCopies(void) {"),
    # the GPU copy queue's types (fakevk.h)
    "gpucopytypes": lambda: span(H, "typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;", "} LorieGpuCopyEntry;")
        + macro(H, "LORIE_GPU_IN_FLIGHT_MAX"),
    # the AHBs a submitted Vulkan frame uses, as both sides look them up (fakevk.h, townership.c)
    "inflight": lambda: func(H, "static inline __always_inline bool lorieGpuInFlight(volatile struct lorie_shared_server_state *st, uint64_t id) {")
        + func(H, "static inline __always_inline bool lorieGpuAnyInFlight(volatile struct lorie_shared_server_state *st) {")
        + func(H, "static inline __always_inline bool lorieGpuSubmitterGone(volatile struct lorie_shared_server_state *st) {")
        + func(H, "static inline __always_inline void lorieGpuWaitIdle(volatile struct lorie_shared_server_state *st, bool every, uint64_t id) {"),
    # a Present copy from the X server's queue through the renderer's frame and back to the X server - tpresentcopy.c:
    # LorieBuffer's description and GLES's swizzle rule, the X server's side (schedule, done, ack), the renderer's
    # GLES copies (what Vulkan must match) and its Vulkan copies, alone and in a frame
    "buffertypes": lambda: span(os.path.join(L, "buffer.h"), "enum {\n    LORIEBUFFER_UNKNOWN __unused,", "} LorieBuffer_Desc;"),
    "presentcopy": lambda: func(B, "__LIBC_HIDDEN__ bool LorieBuffer_isRgba(LorieBuffer *buffer) {")
        + func(I, "bool lorieRendererAvailable(void) {")
        + func(I, "Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,")
        + func(I, "Bool lorieGpuCopyIsDone(uint64_t serial) {")
        + func(I, "void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer) {")
        + func(R, "static void rendererGlBuffer(LorieBuffer *buf) {")
        + func(R, "static void drawRegion(GLuint id, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t flip) {")
        + func(R, "static uint64_t rendererApplyPendingGpuCopiesLocked(void) {")
        + func(R, "static bool rendererVkOwnedLocked(void) {")
        + func(R, "static void rendererVkInFlight(void) {")
        + func(R, "static void rendererVkFrameDone(void) {")
        + func(R, "static void rendererVkIdle(void) {")
        + func(R, "static struct lorie_vk_quad rendererVkCopyQuad(LorieGpuCopyRect r, int16_t xOff, int16_t yOff, const LorieBuffer_Desc *src,")
        + func(R, "static bool rendererVkCopiesLocked(uint64_t *serial, uint32_t *readIndex) {")
        + func(R, "static void rendererVkApplyPendingGpuCopies(void) {")
        + func(R, "static void rendererVkRedraw(bool *waitingForBuffers) {"),
    # the shaders as committed (vk.c includes them)
    "vk_shaders": lambda: open(os.path.join(L, "vk_shaders.h"), encoding="utf-8").read(),
}

for name, make in recipes.items():
    with open(os.path.join(OUT, name + (".h" if name == "vk_shaders" else "_src.inc")), "w") as f:
        f.write(make())
