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

# patches/xserver.patch - or, for a negative control, a changed copy of it (tcontrols.py)
PATCH = os.environ.get("LORIE_XSERVER_PATCH") or os.path.join(HERE, "..", "..", "app", "src", "main", "cpp", "patches", "xserver.patch")
_present = []

def present(name):
    """present/<name> as the build compiles it: a copy of the xserver's present/ with patches/xserver.patch's
    present/ hunks applied (in OUT - the xserver checkout itself is never patched)."""
    if not _present:
        import re, shutil, subprocess
        d = os.path.join(OUT, "xs-present")
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "present"))
        for f in os.listdir(os.path.join(XS, "present")):
            if f.endswith((".c", ".h")):
                shutil.copyfile(os.path.join(XS, "present", f), os.path.join(d, "present", f))
        hunks = "".join(x for x in re.split(r"(?m)^(?=\+\+\+ \./)", open(PATCH, encoding="utf-8").read())
                        if x.startswith("+++ ./present/"))
        subprocess.run(["patch", "-p1", "-N", "-s", "-d", d], input=hunks, text=True, check=True)
        _present.append(d)
    return os.path.join(_present[0], "present", name)

_patched = {}

def patched(path):
    """xserver/<path> as the build compiles it: a copy of that one file with patches/xserver.patch's hunks for it
    applied (in OUT - the xserver checkout itself is never patched)."""
    if path not in _patched:
        import re, shutil, subprocess
        d = os.path.join(OUT, "xs-patched")
        os.makedirs(os.path.dirname(os.path.join(d, path)), exist_ok=True)
        shutil.copyfile(os.path.join(XS, path), os.path.join(d, path))
        hunks = "".join(x for x in re.split(r"(?m)^(?=\+\+\+ \./)", open(PATCH, encoding="utf-8").read())
                        if x.startswith("+++ ./%s\n" % path))
        if not hunks:
            sys.exit("patches/xserver.patch has no hunks for " + path)
        subprocess.run(["patch", "-p1", "-N", "-s", "-d", d], input=hunks, text=True, check=True)
        _patched[path] = os.path.join(d, path)
    return _patched[path]

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
        + func(I, "static void lorieLogRendererDiag(void) {")
        # P11-C3: the tick's frame bookkeeping
        + func(I, "static uint32_t lorieFramesShown(void) {")
        + func(I, "static void lorieTickTimed(uint64_t tickUs, uint32_t postedUs, Bool posted) {")
        + func(I, "static void lorieTickAsked(uint64_t tickUs) {"),
    # AHB CPU ownership - townership.c: xserver's list, EXA's access indices, then every pixmap path that takes
    # or drops a CPU lock, as 3c98ffa has them ("own_base") and as they are now, with the handshake ("own_new")
    "xorglist": lambda: open(os.path.join(XS, "include", "list.h"), encoding="utf-8").read(),
    "own_consts": lambda: macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_DEST")
        + macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_SRC")
        + macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_MASK")
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
        # P11-C3-B1: the reservations PER_ACCESS AHB accesses may hold instead of the shared lock
        "static Bool lorieCpuReserve(uint64_t id) {",
        "static Bool lorieCpuRelease(uint64_t id) {",
        "static void lorieCpuReleasedWake(void) {",
    ]) + FAILAT() + func(I, "static uint8_t lorieRequestedVkFailAt(const char *value, volatile uint8_t *invalid) {")
        + func(I, "static uint8_t lorieRequestedVkSelftest(const char *value, volatile uint8_t *invalid) {"),
    # root double buffering's swap, the bounds its own locks take (P11-B), and its copy by the renderer's GPU asked
    # for, waited for or taken back (P11-C2) - townership.c
    "rootswap": lambda: macro(I, "LORIE_ROOT_GPU_COPY_MIN_BYTES")
        + func(I, "static ARect *lorieRootCopyBounds(BoxPtr box, int n, const LorieBuffer_Desc *d, ARect *bounds) {")
        + func(I, "static Bool lorieRootCopyCpu(LorieBuffer *back, LorieBuffer *front, BoxPtr box, int n, uint64_t waitStartUs) {")
        + func(I, "static Bool lorieOwnershipAsked(void) {")
        + func(I, "static Bool lorieRootGpuCopyAsk(LorieBuffer *back, LorieBuffer *front, BoxPtr box, int n) {")
        + func(I, "static void lorieRootCopyWait(Bool takeBack) {")
        + func(I, "static Bool lorieRootSwap(LoriePixmapPriv *priv) {"),
    # P11-B: buffer.c's CPU locks as they are - tbufferlock.c
    "bufferlock": lambda: span(os.path.join(L, "buffer.h"), "enum {\n    LORIEBUFFER_UNKNOWN __unused,", "typedef struct LorieBuffer LorieBuffer;")
        + span(B, "struct LorieBuffer {", "    int32_t gpuCopyPending;\n};")
        + func(B, "__LIBC_HIDDEN__ int LorieBuffer_lock(LorieBuffer* buffer, void** out) {")
        + func(B, "__LIBC_HIDDEN__ int LorieBuffer_lockRect(LorieBuffer* buffer, bool write, const ARect* rect, void** out) {")
        + func(B, "__LIBC_HIDDEN__ int LorieBuffer_unlock(LorieBuffer* buffer) {"),
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
        + func(VC, "uint8_t lorieVkTransferCheck(struct lorie_vk *vk, volatile struct lorie_shared_server_state *st) {")
        + span(VC, "/* ---- presenting ---- */", "LORIE_FALLBACK_VK_PRESENT, res);\n            return LORIE_VK_FRAME_FATAL;\n    }\n}"),
    # the renderer's choice of backend and its side of the ownership handshake - tvkrenderer.c
    "vkrenderer": lambda: span(R, "enum { RENDERER_USE_GLES, RENDERER_USE_VULKAN, RENDERER_USE_NONE };", "RENDERER_USE_NONE };")
        + func(R, "static int rendererBackendFor(uint8_t requested, bool vulkanFailed) {")
        + func(R, "static bool rendererGlesEager(void) {")
        + func(R, "static void rendererSleep(uint32_t seen, int64_t timeoutUs) {")
        + func(R, "static bool rendererVkOwnership(uint8_t mode) {")
        + func(R, "static bool rendererVkOwnedLocked(void) {"),
    # P11-C3-C: the freeze watchdog's stages - the renderer's (rendererProgressOn, rendererStage) and the X server's
    # thread's (lorieXEnter, lorieXLeave) - tpresentcopy.c, twake.c, townership.c, tfreeze.c
    "rendererstage": lambda: func(R, "static bool rendererProgressOn(void) {") + func(R, "static void rendererStage(uint32_t stage) {"),
    "contentconsts": lambda: macro(I, "LORIE_CONTENT_CLIENTS") + macro(os.path.join(XS, "exa", "exa.h"), "EXA_NUM_PREPARE_INDICES"),
    # P11-C3-D0: client content from its presentation to the screen - the X server's hooks (counting, stamping,
    # carrying through CPU accesses, publishing) and the renderer's frames - tcontentframe.c
    "contentframe": lambda: "".join(macro(os.path.join(XS, "exa", "exa.h"), m) for m in (
            "EXA_PREPARE_DEST", "EXA_PREPARE_SRC", "EXA_PREPARE_MASK", "EXA_PREPARE_AUX_DEST", "EXA_PREPARE_AUX_SRC",
            "EXA_PREPARE_AUX_MASK"))
        + span(I, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + macro(I, "LORIE_PIXMAP_PRIV_FROM_PIXMAP")
        + func(I, "static void lorieDiagWord(char *out, size_t size, const char *in) {")
        + func(I, "Bool lorieGpuCopyIsDone(uint64_t serial) {")
        + "".join(func(I, f) for f in (
            "static void lorieContentClient(Bool skipped, uint64_t now) {",
            "uint32_t lorieContentRequest(PixmapPtr pixmap, uint64_t *request_us) {",
            "void lorieContentSkipped(uint32_t seq) {",
            "static void lorieContentStore(PixmapPtr dst, const struct lorie_content_stamp *c, uint64_t now, Bool carried) {",
            "uint64_t lorieContentCopied(uint32_t seq, uint64_t request_us, PixmapPtr dst, Bool gpu) {",
            "void lorieContentCopyDone(uint32_t seq, uint64_t request_us, uint64_t copy_us, PixmapPtr dst) {",
            "void lorieContentComplete(uint32_t seq, CARD8 mode) {"))
        + span(I, "enum { LORIE_CONTENT_OPEN,", "LORIE_CONTENT_FAILED };")
        + "".join(func(I, f) for f in (
            "static void lorieContentAccess(PixmapPtr pPix, LoriePixmapPriv *priv, int index, int how) {",
            "void lorieContentDamaged(uint64_t serial) {",
            "static void lorieContentStaleRead(LoriePixmapPriv *priv, int index) {",
            "static void lorieContentPublish(LoriePixmapPriv *priv) {"))
        + span(R, "// P11-C3-D0: the content generation the last frame presented showed", "static struct { uint32_t gen; uint64_t newUs; } vkContentLast;")
        + func(R, "static void rendererContentRead(struct lorie_content *out) {") + macro(R, "LORIE_CONTENT_GAP_US")
        + func(R, "static void rendererContentFrame(const struct lorie_content *c, uint64_t now) {"),
    # P11-C3-D1: the RENDER composites timed by class (TERMUX_X11_RENDERER_VK_FRAME_DIAG) - the X server's own fields
    # for them, their report (tdiaglog.c, texacomposite.c) and the timing itself (texacomposite.c)
    "compositeconsts": lambda: macro(I, "LORIE_COMPOSITE_CLASSES"),
    "compositefields": lambda: span(I, "    CompositeProcPtr composite;\n", "} worstComposite;"),
    "compositereport": lambda: func(I, "static void lorieCompositeName(char *out, size_t size, uint32_t key) {")
        + func(I, "static uint64_t lorieCompositeCpuUs(uint64_t us, uint64_t gpu, uint64_t root, uint64_t lock, uint64_t ahb) {")
        + macro(I, "LORIE_COMPOSITE_SHOWN")
        + func(I, "static void lorieCompositeReport(char *totals, size_t totalsSize, char *classes, size_t classesSize) {"),
    "compositetimed": lambda: span(I, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + func(I, "static Bool lorieIsAhb(LoriePixmapPriv *priv) {")
        + span(I, "enum { LORIE_PICT_SOLID,", "LORIE_PICT_ROOT };")
        + func(I, "static uint32_t lorieCompositePict(PicturePtr pict) {")
        + func(I, "static uint32_t lorieCompositeKey(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst) {")
        + func(I, "static void lorieCompositeTimed(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 xSrc, INT16 ySrc,\n"
                  "                                INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst, CARD16 width, CARD16 height) {"),
    "xstage_decl": lambda: span(I, "enum { LORIE_X_LOOP,", "struct lorieXMark { uint32_t stage; uint64_t us; };"),
    "xstage": lambda: span(I, "enum { LORIE_X_LOOP,", "struct lorieXMark { uint32_t stage; uint64_t us; };")
        + func(I, "static struct lorieXMark lorieXEnter(uint32_t stage) {") + func(I, "static void lorieXLeave(struct lorieXMark *prev) {"),
    # P11-C3-C: the freeze watchdog - its look, its judgement, its lines (all but its thread) - tfreeze.c
    "freeze": lambda: func(I, "static uint32_t lorieFramesShown(void) {")
        + span(I, "#define LORIE_FREEZE_PERIOD_US 100000", "#undef LORIE_D\n}"),
    # P11-C3-C: the shared lock's wait (lorie.h) - tsharedlock.c
    "sharedlock": lambda: func(H, "static inline __always_inline void lorie_mutex_lock(pthread_mutex_t* mutex, pid_t* lockingPid) {")
        + func(H, "static inline __always_inline void lorie_mutex_unlock(pthread_mutex_t* mutex, pid_t* lockingPid) {"),
    # P11-C3-C: the renderer's wake, both ends (lorie.h) - tvkrenderer.c, twake.c
    "wake": lambda: span(H, "struct lorie_renderer_wake {", "    return __atomic_load_n(&w->seq, __ATOMIC_ACQUIRE);\n}"),
    # ... and the renderer's wait for work: its look (rendererShouldWait, with the pending root copy and the copies left
    # queued for a reservation it asks about), its sleep, and the loop of the two - twake.c
    "rendererwait": lambda: func(R, "static bool rendererProgressOn(void) {") + func(R, "static void rendererStage(uint32_t stage) {")
        + func(R, "static void rendererSleep(uint32_t seen, int64_t timeoutUs) {")
        + span(R, "static bool deferredCopies;", "static uint32_t deferredAt, deferredReleases;")
        + func(R, "static bool rendererCopiesStillDeferred(void) {")
        + func(R, "static bool rendererRootCopyPending(void) {")
        + func(R, "static inline __always_inline bool rendererShouldWait(bool *waitingForBuffers) {")
        + func(R, "static void rendererWaitForWork(bool *waitingForBuffers) {")
        + func(R, "void rendererAddBuffer(LorieBuffer* buf) {"),
    # the GPU copy queue's types (fakevk.h)
    "gpucopytypes": lambda: span(H, "typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;", "} LorieGpuCopyEntry;")
        + macro(H, "LORIE_GPU_IN_FLIGHT_MAX"),
    # the AHBs a submitted Vulkan frame uses, as both sides look them up (fakevk.h, townership.c)
    "inflight": lambda: func(H, "static inline __always_inline bool lorieGpuInFlight(volatile struct lorie_shared_server_state *st, uint64_t id) {")
        + func(H, "static inline __always_inline bool lorieCpuReserved(volatile struct lorie_shared_server_state *st, uint64_t id) {")
        + func(H, "static inline __always_inline bool lorieGpuAnyInFlight(volatile struct lorie_shared_server_state *st) {")
        + func(H, "static inline __always_inline bool lorieGpuSubmitterGone(volatile struct lorie_shared_server_state *st) {")
        + func(H, "static inline __always_inline void lorieGpuWaitIdle(volatile struct lorie_shared_server_state *st, bool every, uint64_t id) {"),
    # a Present copy from the X server's queue through the renderer's frame and back to the X server - tpresentcopy.c:
    # LorieBuffer's description and GLES's swizzle rule, the X server's side (schedule, done, ack), the renderer's
    # GLES copies (what Vulkan must match) and its Vulkan copies, alone and in a frame
    "buffertypes": lambda: span(os.path.join(L, "buffer.h"), "enum {\n    LORIEBUFFER_UNKNOWN __unused,", "} LorieBuffer_Desc;"),
    "presentcopy": lambda: func(B, "__LIBC_HIDDEN__ bool LorieBuffer_isRgba(LorieBuffer *buffer) {")
        + func(R, "static bool rendererProgressOn(void) {") + func(R, "static void rendererStage(uint32_t stage) {")
        # P11-C3-B1: copies left queued for the X server's reservations
        + span(R, "static bool deferredCopies;", "static uint32_t deferredAt, deferredReleases;")
        + func(R, "static bool rendererCopyDeferred(const LorieGpuCopyEntry *entry, uint32_t index) {")
        + func(R, "static bool rendererCopiesStillDeferred(void) {")
        + func(I, "bool lorieRendererAvailable(void) {")
        + func(I, "Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,")
        + func(I, "void lorieCountCopyAck(void) {")
        + func(I, "Bool lorieGpuCopyIsDone(uint64_t serial) {")
        + func(I, "int lorieGpuCopySettle(uint64_t serial) {")
        + func(I, "void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer) {")
        + func(R, "static void rendererGlBuffer(LorieBuffer *buf) {")
        + func(R, "static void drawRegion(GLuint id, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t flip) {")
        # P11-C3-C: the buffers a pass's copies use, waited for before the shared lock; looked up under it without waiting
        + span(R, "#define LORIE_BUFFER_AWAIT_US 100000", "static uint32_t buffersGivenUpCount;")
        + func(R, "static bool rendererHasBuffer(uint64_t id) {") + "static bool rendererRootCopyPending(void);\n"
        + func(R, "static void rendererAwaitBuffers(void) {")
        + func(R, "static LorieBuffer *rendererFindBuffer(uint64_t id, bool *later) {")
        + func(R, "static uint64_t rendererApplyPendingGpuCopiesLocked(void) {")
        + func(R, "static bool rendererVkOwnedLocked(void) {")
        + func(R, "static void rendererVkInFlight(void) {")
        + func(R, "static void rendererVkInFlightAux(void) {")
        + func(R, "static void rendererVkListDone(uint32_t list) {")
        + func(R, "static void rendererVkFrameDone(void) {")
        + func(R, "static void rendererVkAuxDone(void) {")
        + func(R, "static void rendererVkIdle(void) {")
        + func(R, "static struct lorie_vk_quad rendererVkCopyQuad(LorieGpuCopyRect r, int16_t xOff, int16_t yOff, const LorieBuffer_Desc *src,")
        + func(R, "static void rendererVkCountTransfer(const LorieGpuCopyEntry *entry, uint32_t n, const struct lorie_vk_image *s,")
        + func(R, "static bool rendererVkCopyBgraIntoRgbx(const LorieGpuCopyEntry *entry, const struct lorie_vk_image *s, const struct lorie_vk_image *d) {")
        + func(R, "static uint32_t rendererVkCopyRegions(const LorieGpuCopyEntry *entry, uint32_t n, const struct lorie_vk_image *s,")
        # P11-C3-B2: the others' frames; a root copy ahead of the frame
        + func(R, "static void rendererVkWaitOthers(void) {")
        + span(R, "struct rendererVkRootWork {", "static struct { struct rendererVkRootWork work; bool pending, chained; } vkAux;\nstatic bool rendererVkLock(void);")
        + func(R, "static bool rendererVkCopiesLocked(uint64_t *serial, uint32_t *readIndex) {")
        + func(R, "static uint32_t rendererVkQueuedTimes(uint32_t readIndex, uint64_t *queued) {")
        + func(R, "static void rendererVkCountCopies(const uint64_t *queued, uint32_t n, bool frame, uint64_t startUs, uint64_t lockedUs,")
        # P11-C3: the Vulkan frames' timings
        + span(R, "// P11-C3: the last Vulkan frame's start and present call", "    memset(&vkContentLast, 0, sizeof(vkContentLast));\n}")
        + span(R, "// Gaps between frames longer than this", "static uint64_t vkRootCopyBusyUs;")
        + span(R, "enum { LORIE_VK_INTERVAL_SAMPLED,", "};") + func(R, "static int rendererVkInterval(")
        + span(R, "struct rendererVkFrameTimes {", "};")
        # P11-C3-C: the frame gaps by what waited in them
        + span(R, "#define LORIE_VK_GAP_US 100000", "enum { LORIE_GAP_NONE, LORIE_GAP_X_STALLED, LORIE_GAP_UNWOKEN, LORIE_GAP_LATE };")
        + func(R, "static void rendererVkFrameGap(const struct rendererVkFrameTimes *t, uint64_t lastStartUs, uint32_t lastTicks) {")
        + func(R, "static void rendererVkFrameTimed(const struct rendererVkFrameTimes *t) {")
        # P11-C3-D0: the client content the screen frames show
        + func(R, "static void rendererContentRead(struct lorie_content *out) {") + macro(R, "LORIE_CONTENT_GAP_US")
        + func(R, "static void rendererContentFrame(const struct lorie_content *c, uint64_t now) {")
        + func(R, "static void rendererVkApplyPendingGpuCopies(void) {")
        + func(R, "static bool rendererRootCopyPending(void) {")
        + func(R, "static void rendererRootCopySettle(uint32_t seq, bool failed) {")
        + func(R, "static void rendererRootCopyDecline(void) {")
        + func(R, "static bool rendererVkRootCopySubmit(struct rendererVkRootWork *w, bool ahead) {")
        + func(R, "static void rendererVkRootCopyEnd(struct rendererVkRootWork *w, bool ahead) {")
        + func(R, "static void rendererVkRootCopy(void) {")
        + func(R, "static void rendererVkRootCopyAhead(void) {")
        + func(R, "static void rendererVkAuxFinish(void) {")
        + span(R, "enum { LORIE_ROOT_EARLY_ACQUIRED, LORIE_ROOT_EARLY_LOCK };", "LORIE_ROOT_EARLY_LOCK };")
        + func(R, "static void rendererVkAuxEarly(int at) {") + macro(R, "LORIE_ROOT_EARLY_POLL_US")
        + func(R, "static bool rendererVkLock(void) {")
        + func(R, "static void rendererVkAuxSettle(void) {")
        + func(R, "static void rendererVkAuxForget(void) {")
        + func(R, "static void rendererVkRedraw(bool *waitingForBuffers) {")
        + func(R, "static bool rendererVkFrameWanted(void) {")
        + func(R, "static void rendererPass(bool *waitingForBuffers) {"),
    # P10-S0: the Present paths that end a presentation whose GPU copy may still run, from the patched xserver -
    # tpresentsafety.c: the vblank as present_priv.h has it, execute (and its copy and post), re-execute, scrap,
    # destroy, the window's vblanks freed, the renderer's completion recheck, and present_scmd_pixmap's loop that
    # scraps a queued presentation a new one overwrites in the same frame
    "presentsafety_struct": lambda: span(present("present_priv.h"), "struct present_vblank {",
                                         "    int16_t             lorie_copy_dx, lorie_copy_dy;\n};")
        + span(present("present_priv.h"), "#define LORIE_GPU_COPY_EARLY_SCRAP 0", "#define LORIE_GPU_COPY_CANCELLED 2")
        + span(present("present_priv.h"), "#define LORIE_COPY_DAMAGE_DONE 0", "#define LORIE_COPY_DAMAGE_DROPPED 2"),
    "presentsafety": lambda: "static void\n" + func(present("present_scmd.c"), "present_re_execute(present_vblank_ptr vblank)\n{")
        + "static void\n" + func(present("present_scmd.c"), "present_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)\n{")
        + "static void\n" + func(present("present_execute.c"),
                                 "present_damage_gpu_copy_region(WindowPtr window, PixmapPtr dst, PixmapPtr pixmap, RegionPtr update, int dx, int dy)\n{")
        + "void\n" + func(present("present_execute.c"), "present_damage_gpu_copy(present_vblank_ptr vblank, int how)\n{")
        + "void\n" + func(present("present_execute.c"), "present_execute_copy(present_vblank_ptr vblank, uint64_t crtc_msc)\n{")
        + "void\n" + func(present("present_execute.c"), "present_execute_post(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)\n{")
        + span(present("present_vblank.c"), "struct present_retired_copy {",
               "        retired->idle = FALSE;\n        present_retired_copy_release(retired);\n    }\n}")
        + "void\n" + func(present("present_vblank.c"), "present_vblank_scrap(present_vblank_ptr vblank)\n{")
        + "void\n" + func(present("present_vblank.c"), "present_vblank_destroy(present_vblank_ptr vblank)\n{")
        + "static void\n" + func(present("present_screen.c"), "present_free_window_vblank(WindowPtr window)\n{")
        + "void\n" + func(present("present_scmd.c"), "lorieRecheckGpuCopies(void)\n{")
        + "static void presentOverwrite(present_window_priv_ptr window_priv, RegionPtr update, PixmapPtr pixmap, RRCrtcPtr target_crtc,\n"
          "                             uint64_t target_msc) {\n    present_vblank_ptr vblank, tmp;\n"
        + span(present("present_scmd.c"), "    if (!update && pixmap) {\n        xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->vblank, window_list) {",
               "            if (vblank->flip_ready)\n                present_re_execute(vblank);\n        }\n    }") + "}\n",
    # P11-A: the X server's main loop as patched - tdispatchyield.c: the work queue (dixutils.c, with its lock), the
    # ready clients and the smart scheduler's choice of client (dispatch.c, unpatched), and Dispatch and
    # WaitForSomething with the yield asked for by lorieQueueUrgentWorkProc (InitOutput.c) ...
    "dispatchyield_common": lambda: span(os.path.join(XS, "include", "dixstruct.h"), "typedef struct _WorkQueue {", "} WorkQueueRec;")
        + span(patched("dix/dixutils.c"), "WorkQueuePtr workQueue;",
                                         "    pthread_spin_unlock(&workQueueLock);\n    return TRUE;\n}")
        + span(patched("dix/dispatch.c"), "static struct xorg_list ready_clients;",
               "mark_client_not_ready(ClientPtr client)\n{\n    xorg_list_del(&client->ready);\n}")
        + "static ClientPtr\n" + func(os.path.join(XS, "dix", "dispatch.c"), "SmartScheduleClient(void)\n{"),
    "dispatchyield": lambda: "Bool\n" + func(patched("os/WaitFor.c"), "WaitForSomething(Bool are_ready)\n{")
        + "void\n" + func(patched("dix/dispatch.c"), "Dispatch(void)\n{")
        + func(I, "void lorieQueueUrgentWorkProc(Bool (*proc)(ClientPtr, void *), void *closure) {"),
    # ... and the two as the xserver has them, which the patched ones must equal with the yield never asked for
    "dispatchyield_orig": lambda: "Bool\n" + func(os.path.join(XS, "os", "WaitFor.c"), "WaitForSomething(Bool are_ready)\n{")
        + "void\n" + func(os.path.join(XS, "dix", "dispatch.c"), "Dispatch(void)\n{"),
    # lorieGpuCopySettle's answers, as the patched present_priv.h defines them - tpresentcopy.c
    "settleconsts": lambda: span(present("present_priv.h"), "#define LORIE_GPU_COPY_RUNNING 0", "#define LORIE_GPU_COPY_CANCELLED 2"),
    # the shaders as committed (vk.c includes them)
    "vk_shaders": lambda: open(os.path.join(L, "vk_shaders.h"), encoding="utf-8").read(),
}

for name, make in recipes.items():
    with open(os.path.join(OUT, name + (".h" if name == "vk_shaders" else "_src.inc")), "w") as f:
        f.write(make())
