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
#define PictFilterNearest 0
#define PICT_a8r8g8b8 100
#define PICT_x8r8g8b8 101
#define LORIE_ACTIVE_VULKAN 1
#define LORIE_GPU_COPY_OP_SRC 0u
#define LORIE_GPU_COPY_OP_OVER 1u
#define LORIE_GPU_COPY_MAX_RECTS 16
#define DRAWABLE_WINDOW 1
#define DRAWABLE_PIXMAP 2

typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxRec rect; } RegionRec, *RegionPtr;
#define RegionNumRects(p) ((p)->n)
typedef struct { uint64_t id; } LorieBuffer_Desc;
typedef struct { LorieBuffer_Desc desc; } LorieBuffer;
typedef struct { LorieBuffer *buffer; void *mem; void *locked; struct { uint32_t seq; } content; } LoriePixmapPriv;
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

struct fake_state { struct { int active; } diag; };
static struct { struct fake_state *state;
                int xrenderGpu, rootPresentGpu, accessLocks, contentDiag;
                unsigned xrenderGpuSuccess, xrenderGpuFallback; } world, *pvfb = &world;
static struct fake_state st;
static ScreenRec screen, *pScreenPtr = &screen;
static int failures, queued, acked, damaged, uninit, gpuAccepted = 1, gpuQueueAllowed = 1, perAccess = 1, rootTwinned;
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
                              uint8_t op, uint64_t *serial, void **destBuffer) {
    (void)src;(void)dst;
    if (!gpuQueueAllowed) return FALSE;
    queued++; lastSource = r->rect; lastOp = op; lastXOff = x; lastYOff = y;
    *serial = 41; *destBuffer = dst->priv->buffer; return TRUE;
}
static Bool lorieWaitGpuCopyRecorded(uint64_t serial) {
    check(serial == 41, "GPU operation serial");
    return gpuAccepted;
}
static void lorieGpuCopyAck(PixmapPtr src, void *dst) {
    (void)src;(void)dst; acked++;
}
/* GetWindowPixmap is not called in the pixmap-only tests, but a window case is included. */
static PixmapPtr windowImage;
static PixmapPtr getWindow(void *window) { (void)window; return windowImage; }

#include "xrendergpu_src.inc"

int main(void) {
    LorieBuffer sb = { { 11 } }, db = { { 22 } };
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

    gpuAccepted = 0;
    check(!lorieXRenderGpuComposite(PictOpOver, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
          acked == 3 && damaged == 2 && world.xrenderGpuFallback == 1,
          "device-skipped GPU Over must return CPU fallback exactly once and release refs");
    gpuAccepted = 1;
    gpuQueueAllowed = 0;
    check(!lorieXRenderGpuComposite(PictOpSrc, &src, NULL, &dst, 2, 3, 4, 5, 60, 35) &&
          acked == 3 && world.xrenderGpuFallback == 2,
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

    printf("TXRENDERGPU Src/Over, clipping, fallback and AHB ownership: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return !!failures;
}
