/* P11-C4: compile the *actual* XRender opt-in adapter from InitOutput.c, with mock pixmaps, regions,
 * queue, fence completion and damage. Tests accepted Src/Over rectangles and conservative fallback
 * conditions. The Vulkan blend factors and imported-image lifecycle are tested separately by tvkframe. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <limits.h>

typedef int Bool;
typedef uint8_t CARD8;
typedef uint16_t CARD16;
typedef int16_t INT16;
#define TRUE 1
#define FALSE 0
#define PictOpSrc 1
#define PictOpOver 3
#define PictOpClear 0
#define PictOpOverReverse 4
#define PictOpIn 5
#define PictOpInReverse 6
#define PictOpOut 7
#define PictOpOutReverse 8
#define PictOpAtop 9
#define PictOpAtopReverse 10
#define PictOpXor 11
#define PictOpAdd 12
#define PictOpSaturate 13
#define PictFilterNearest 0
#define PICT_a8r8g8b8 100
#define PICT_x8r8g8b8 101
#define LORIE_ACTIVE_VULKAN 1
#include "gpucopytypes_src.inc"
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#define DRAWABLE_WINDOW 1
#define DRAWABLE_PIXMAP 2

typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxRec rect; } RegionRec, *RegionPtr;
#define RegionNumRects(p) ((p)->n)
#define RegionRects(p) (&(p)->rect)
typedef struct { uint64_t id; uint32_t format; } LorieBuffer_Desc;
typedef struct { LorieBuffer_Desc desc; } LorieBuffer;
struct lorie_content_stamp { uint32_t seq; };
typedef struct { LorieBuffer *buffer; void *mem; void *locked; struct lorie_content_stamp content; } LoriePixmapPriv;
typedef struct _Drawable DrawableRec, *DrawablePtr;
typedef struct _Pixmap PixmapRec, *PixmapPtr;
typedef struct _Screen ScreenRec, *ScreenPtr;
typedef struct _Picture PictureRec, *PicturePtr;
typedef void *WindowPtr;
struct _Drawable { int type; int x, y; ScreenPtr pScreen; };
struct _Pixmap { DrawableRec drawable; int screen_x, screen_y; LoriePixmapPriv *priv; };
struct _Screen { PixmapPtr devPrivate; PixmapPtr (*GetWindowPixmap)(void *window); };
struct _Picture {
    DrawablePtr pDrawable;
    int format, filter;
    int repeat, transform, alphaMap;
};
#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(p) ((p)->priv)

struct fake_state { struct { int active; } diag; struct { uint32_t writeIndex; } gpuCopyQueue; };
struct lorie_xrender_job {
    uint64_t serial;
    LorieBuffer *src, *dst;
    PixmapPtr dstPixmap;
    struct lorie_content_stamp content;
    uint32_t queueIndex, srcFormat, dstFormat;
    uint8_t op;
    Bool root;
    int16_t xOff, yOff;
    uint16_t numRects;
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
};
static struct { struct fake_state *state;
                int xrenderGpu, rootPresentGpu, accessLocks, contentDiag;
                unsigned xrenderGpuSuccess, xrenderGpuFallback, xrenderGpuMinPixels, xrenderGpuSmallSkipped;
                Bool xrenderAsync;
                struct lorie_xrender_job xrenderJobs[LORIE_GPU_COPY_QUEUE_CAPACITY];
                uint32_t xrenderJobsCount;
                uint64_t xrenderAsyncQueued;
                } world, *pvfb = &world;
static struct fake_state st;
static ScreenRec screen, *pScreenPtr = &screen;
static int failures, queued, acked, damaged, uninit, flushed, gpuAccepted = 1, gpuQueueAllowed = 1, perAccess = 1, rootTwinned;
static int ownershipPending;
static uint8_t lastOp;
static int lastXOff, lastYOff;
static BoxRec lastSource, lastDamage;
static int lastDamageType;
static void check(bool valid, const char *what) {
    if (!valid) { failures++; printf("  FAIL %s\n", what); }
}
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *buffer) { return &buffer->desc; }
static int lorieAhbPerAccess(void) { return perAccess; }
static int lorieOwnershipAsked(void) { return ownershipPending; }
static int lorieRootTwinned(void) { return rootTwinned; }
static uint64_t lorieMonotonicUs(void) { static uint64_t us = 1000; return ++us; }
static void lorieContentStore(PixmapPtr d, const void *c, uint64_t now, Bool carried) {
    (void)d;(void)c;(void)now;(void)carried;
}
static void RegionTranslate(RegionPtr r, int x, int y) {
    r->rect.x1 += x; r->rect.x2 += x; r->rect.y1 += y; r->rect.y2 += y;
}
static void RegionUninit(RegionPtr r) { uninit++; r->n = 0; }
static Bool miComputeCompositeRegion(RegionPtr r, PicturePtr s, PicturePtr m, PicturePtr d, INT16 xs, INT16 ys,
                                      INT16 xm, INT16 ym, INT16 xd, INT16 yd, CARD16 w, CARD16 h) {
    (void)s;(void)m;(void)d;(void)xs;(void)ys;(void)xm;(void)ym;
    r->n = 1; r->rect = (BoxRec) { xd, yd, xd+w, yd+h }; return TRUE;
}
static void DamageDamageRegion(DrawablePtr d, RegionPtr r) {
    damaged++; lastDamage = r->rect; lastDamageType = d->type;
}
static Bool lorieQueueGpuCopy(PixmapPtr src, PixmapPtr dst, RegionPtr r, int16_t x, int16_t y,
                              uint8_t op, Bool isPresent, Bool asyncXrender,
                              uint64_t *serial, void **destBuffer) {
    (void)src;(void)dst;
    (void)isPresent;
    (void)asyncXrender;
    if (!gpuQueueAllowed) return FALSE;
    queued++; lastSource = r->rect; lastOp = op; lastXOff = x; lastYOff = y;
    st.gpuCopyQueue.writeIndex++;
    *serial = 41; *destBuffer = dst->priv->buffer; return TRUE;
}
static Bool lorieWaitGpuCopyRecorded(uint64_t serial) {
    check(serial == 41, "GPU operation serial");
    return gpuAccepted;
}
static void lorieGpuCopyAck(PixmapPtr src, void *dst) {
    (void)src;(void)dst; acked++;
}
static void lorieXRenderAsyncFlush(void) { flushed++; world.xrenderJobsCount = 0; }
#include "xrenderasyncconflict_src.inc"
/* GetWindowPixmap is not called in the pixmap-only tests, but a window case is included. */
static PixmapPtr windowImage;
static PixmapPtr getWindow(void *window) { (void)window; return windowImage; }

#include "xrendergpu_src.inc"

int main(void) {
    LorieBuffer sb = { { 11, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM } };
    LorieBuffer db = { { 22, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM } };
    LoriePixmapPriv sp = { .buffer = &sb }, dp = { .buffer = &db };
    PixmapRec s = { { DRAWABLE_PIXMAP, 150, 100, &screen }, 150, 100, &sp };
    PixmapRec d = { { DRAWABLE_PIXMAP, 60, 40, &screen }, 60, 40, &dp };
    PictureRec src = { &s.drawable, PICT_a8r8g8b8, PictFilterNearest, 0, 0, 0 };
    PictureRec dst = { &d.drawable, PICT_x8r8g8b8, PictFilterNearest, 0, 0, 0 };
    PictureRec mask = { &s.drawable, PICT_a8r8g8b8, PictFilterNearest, 0, 0, 0 };
    world.state = &st; st.diag.active = LORIE_ACTIVE_VULKAN; world.xrenderGpu = 1;
    screen.devPrivate = NULL; screen.GetWindowPixmap = getWindow;

    check(lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 10, 15, 20, 25, 80, 40),
          "XRender Src should be handled");
    check(queued == 1 && acked == 1 && damaged == 1 && uninit == 1 &&
          lastOp == LORIE_GPU_COPY_OP_SRC && lastXOff == 10 && lastYOff == 10 &&
          lastSource.x1 == 10 && lastSource.y1 == 15 && lastSource.x2 == 90 && lastSource.y2 == 55 &&
          lastDamage.x1 == 80 && lastDamage.y1 == 65 && lastDamage.x2 == 160 && lastDamage.y2 == 105,
          "XRender Src must offset/clamp region and damage in drawable/global coordinates");
    check(lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
          lastOp == LORIE_GPU_COPY_OP_OVER && world.xrenderGpuSuccess == 2,
          "XRender Over must preserve its premultiplied blending op in GPU queue");

    /* All 12 Porter-Duff operators that can be expressed by fixed blend factors. Dest-alpha
     * dependent operators require a destination that actually stores alpha, not an RGBX AHB. */
    {
        const struct { int pict; uint8_t gpu; } operators[] = {
            { PictOpSrc, LORIE_GPU_COPY_OP_SRC }, { PictOpOver, LORIE_GPU_COPY_OP_OVER },
            { PictOpClear, LORIE_GPU_COPY_OP_CLEAR }, { PictOpOverReverse, LORIE_GPU_COPY_OP_OVER_REVERSE },
            { PictOpIn, LORIE_GPU_COPY_OP_IN }, { PictOpInReverse, LORIE_GPU_COPY_OP_IN_REVERSE },
            { PictOpOut, LORIE_GPU_COPY_OP_OUT }, { PictOpOutReverse, LORIE_GPU_COPY_OP_OUT_REVERSE },
            { PictOpAtop, LORIE_GPU_COPY_OP_ATOP }, { PictOpAtopReverse, LORIE_GPU_COPY_OP_ATOP_REVERSE },
            { PictOpXor, LORIE_GPU_COPY_OP_XOR }, { PictOpAdd, LORIE_GPU_COPY_OP_ADD } };
        db.desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        dst.format = PICT_a8r8g8b8;
        for (size_t i = 0; i < sizeof operators / sizeof operators[0]; i++)
            check(lorieXRenderGpuComposite(operators[i].pict, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
                  lastOp == operators[i].gpu, "all Porter-Duff ops must reach Vulkan with their exact operator");
        db.desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
        dst.format = PICT_x8r8g8b8;
        check(!lorieXRenderGpuComposite(PictOpIn, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
              "DST_ALPHA GPU operator must reject an XRGB/RGBX destination");
        check(!lorieXRenderGpuComposite(PictOpSaturate, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
              "Saturate remains on pixel-correct software path");
        sb.desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
        check(!lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
              "ARGB32 source stored as RGBX AHB must not lose its alpha on GPU");
        sb.desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        world.xrenderGpuMinPixels = 4096;
        check(!lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
              world.xrenderGpuSmallSkipped == 1,
              "small XRender operations must avoid synchronous GPU-copy round trips");
        world.xrenderGpuMinPixels = 0;
    }

    gpuAccepted = 0;
    int expectedAck = acked, expectedDamage = damaged;
    check(!lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
          acked == expectedAck + 1 && damaged == expectedDamage && world.xrenderGpuFallback == 1,
          "device-skipped GPU Over must return CPU fallback exactly once and release refs");
    gpuAccepted = 1;
    gpuQueueAllowed = 0;
    expectedAck = acked;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
          acked == expectedAck && world.xrenderGpuFallback == 2,
          "queue-full GPU Src must return CPU fallback without GPU acknowledgement");
    gpuQueueAllowed = 1;

    check(!lorieXRenderGpuComposite(PictOpOver, &src, &mask, &dst, 2, 3, 4, 5, 60, 35),
          "mask/alpha/transform must stay on software path");
    src.transform = 1;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "transformed sampling must stay on software path");
    src.transform = 0; src.repeat = 1;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "repeating source must stay on software path");
    src.repeat = 0; perAccess = 0;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "GPU rendering must require per-access AHB ownership");
    perAccess = 1; db.desc.id = 11;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "same underlying AHB cannot be sampled and written in one draw");
    db.desc.id = 22;
    ownershipPending = 1;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "ownership transition pending must not block X server waiting for a renderer");
    ownershipPending = 0;
    screen.devPrivate = &d;
    rootTwinned = 1;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "twinned root must respect root Present opt-in");
    world.rootPresentGpu = 1;
    check(lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "twinned root destination may be used when opt-in enabled");
    rootTwinned = 0; world.rootPresentGpu = 0;

    windowImage = &s;
    s.drawable.type = DRAWABLE_WINDOW;
    check(lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35),
          "window drawable should resolve to its backing AHB pixmap");
    check(lastDamageType == DRAWABLE_PIXMAP, "damage must be reported on destination drawable");

    /* P11-C5-B2: independent GPU writes can be queued without waiting on a fence; a second
     * overlapping destination or RAW/WAR dependency must flush before it can be scheduled. */
    {
        int beforeFlushes = flushed, beforeQueued = queued, beforeAcks = acked;
        world.xrenderAsync = 1;
        world.rootPresentGpu = 0;
        s.drawable.type = DRAWABLE_PIXMAP;
        check(lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 0, 0, 0, 0, 32, 32) &&
              world.xrenderJobsCount == 1 && queued == beforeQueued + 1 && acked == beforeAcks &&
              world.xrenderAsyncQueued == 1 && flushed == beforeFlushes,
              "async GPU Composite must queue and pin without waiting for the renderer");
        check(lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 32, 0, 64, 0, 16, 20) &&
              world.xrenderJobsCount == 2 && flushed == beforeFlushes,
              "disjoint GPU writes into the same destination should batch without CPU fence");
        check(lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 0, 0, 0, 0, 32, 32) &&
              world.xrenderJobsCount == 1 && flushed == beforeFlushes + 1,
              "overlapping destination writes must flush the previous batch");
        world.xrenderAsync = 0;
        lorieXRenderAsyncFlush();
    }

    printf("TXRENDERGPU 12 Porter-Duff ops, clipping, alpha/cost gates, fallback and AHB ownership: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return !!failures;
}
