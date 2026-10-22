/* TEXACOMPOSITE: the RENDER composites timed by class (P11-C3-D1, TERMUX_X11_RENDERER_VK_FRAME_DIAG=1), from the real
 * code (gen.py "compositetimed", "compositereport"): lorieCompositeTimed wrapping the screen's Composite, a composite's
 * class from its pictures (lorieCompositePict, lorieCompositeKey, lorieCompositeName) and the period's report
 * (lorieCompositeReport) - on a clock and counters the wrapped Composite moves as EXA's accesses would.
 *
 *   Classes: every operator's name and one past them, each kind of source and destination (a solid fill, this
 *   process's memory, an AHB, the screen pixmap - a window drawn into it too - and a redirected window's own AHB, a
 *   pixmap without a private), the source's alpha, each kind of mask, a transform on the source or on the mask. A
 *   composite: passed through to the wrapped Composite with its arguments, the wrap taken off meanwhile and put back
 *   after (a Composite changed below it kept), its time split - the GPU, the shared lock (whose counter holds the GPU's
 *   too), the root copy, the AHB CPU locks, the rest its own - its accesses and AHB locks, its size, its class's slowest
 *   and the period's. One slot per class, a class seen again in its own; past LORIE_COMPOSITE_CLASSES, the last slot.
 *   The report: the totals, the slowest, the 4 classes that took longest in order (the last slot's as "other") and how
 *   many more; the period then reset, and an empty one said so.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef uint8_t CARD8;
typedef int16_t INT16;
typedef uint16_t CARD16;

#include "diagtypes_src.inc"
#include "xorglist_src.inc"
#include "compositeconsts_src.inc"

/* buffers, pixmaps, windows, pictures, the screen - as far as the code under test looks at them */
enum { LORIEBUFFER_UNKNOWN, LORIEBUFFER_REGULAR, LORIEBUFFER_FD, LORIEBUFFER_AHARDWAREBUFFER };    /* buffer.h's */
typedef struct LorieBuffer { struct { uint64_t id; int type; } d; } LorieBuffer;
#define LorieBuffer_description(b) (&(b)->d)
typedef struct _Screen *ScreenPtr;
#define DRAWABLE_WINDOW 0
#define DRAWABLE_PIXMAP 1
typedef struct _Drawable { uint8_t type; ScreenPtr pScreen; } DrawableRec, *DrawablePtr;
typedef struct _Pixmap { DrawableRec drawable; void *driverPriv; } PixmapRec, *PixmapPtr;
typedef struct _Window { DrawableRec drawable; PixmapPtr pixmap; } WindowRec, *WindowPtr;
typedef struct _Screen { PixmapPtr (*GetWindowPixmap)(WindowPtr); void *devPrivate; } ScreenRec;
#define exaGetPixmapDriverPrivate(p) ((p)->driverPriv)
typedef struct { int unused; } PictTransform;
typedef struct _Picture { DrawablePtr pDrawable; uint32_t format; unsigned int componentAlpha : 1; PictTransform *transform; } PictureRec, *PicturePtr;
typedef void (*CompositeProcPtr)(CARD8, PicturePtr, PicturePtr, PicturePtr, INT16, INT16, INT16, INT16, INT16, INT16, CARD16, CARD16);
typedef struct { CompositeProcPtr Composite; } PictureScreenRec, *PictureScreenPtr;
static PictureScreenRec pictureScreen;
#define GetPictureScreen(s) ((void) (s), &pictureScreen)
/* pixman's format codes and accessors (pixman.h PIXMAN_FORMAT, PIXMAN_FORMAT_A, PIXMAN_FORMAT_RGB) */
#define FMT(bpp, type, a, r, g, b) (((bpp) << 24) | ((type) << 16) | ((a) << 12) | ((r) << 8) | ((g) << 4) | (b))
#define PICT_FORMAT_A(f) (((f) >> 12) & 0x0f)
#define PICT_FORMAT_RGB(f) ((f) & 0xfff)
#define PICT_a8r8g8b8 FMT(32, 2, 8, 8, 8, 8)
#define PICT_x8r8g8b8 FMT(32, 2, 0, 8, 8, 8)
#define PICT_a8 FMT(8, 1, 8, 0, 0, 0)

static ScreenRec screenRec;
static ScreenPtr pScreenPtr = &screenRec;
static PixmapPtr windowPixmap(WindowPtr w) { return w->pixmap; }

/* the X server's own (InitOutput.c lorieScreenInfo), as far as the code under test uses it */
static struct { struct lorie_renderer_diag diag; } st;
static struct {
    typeof(&st) state;
#include "compositefields_src.inc"
} fakePvfb = { &st }, *pvfb = &fakePvfb;

static uint64_t fakeNow = 1000;
#define GetTimeInMicros() (fakeNow)
#include "compositetimed_src.inc"
#include "compositereport_src.inc"
#undef GetTimeInMicros

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* the Composite wrapped (EXA's): what it was called with, and what it moves - the clock, the counters its accesses
 * move (the lock's holds the GPU's too), the accesses and AHB locks - and, asked, a Composite changed below it */
static struct {
    int calls;
    bool wrapOff;
    CARD8 op;
    PicturePtr src, mask, dst;
    INT16 args[6];
    CARD16 width, height;
    uint64_t us, gpu, lock, root, ahb;
    uint32_t accesses, locks;
    bool rewrap;
} below;
static void otherComposite(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 xSrc, INT16 ySrc, INT16 xMask,
                           INT16 yMask, INT16 xDst, INT16 yDst, CARD16 width, CARD16 height) {
    (void) op, (void) src, (void) mask, (void) dst, (void) xSrc, (void) ySrc, (void) xMask, (void) yMask, (void) xDst, (void) yDst;
    (void) width, (void) height;
}
static void exaComposite(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 xSrc, INT16 ySrc, INT16 xMask,
                         INT16 yMask, INT16 xDst, INT16 yDst, CARD16 width, CARD16 height) {
    below.calls++;
    below.wrapOff = pictureScreen.Composite == exaComposite;
    below.op = op, below.src = src, below.mask = mask, below.dst = dst;
    below.args[0] = xSrc, below.args[1] = ySrc, below.args[2] = xMask, below.args[3] = yMask, below.args[4] = xDst, below.args[5] = yDst;
    below.width = width, below.height = height;
    fakeNow += below.us;
    st.diag.xGpuWaitUs += below.gpu;
    st.diag.xStateLockWaitUs += below.gpu + below.lock;
    st.diag.rootCopyCpuBlockUs += below.root;
    st.diag.xAhbCpuLockUs += below.ahb;
    st.diag.xAhbCpuAcquires += below.locks;
    fakePvfb.xAccessesOpened += below.accesses;
    if (below.rewrap)
        pictureScreen.Composite = otherComposite;
}

/* the world: buffers (an AHB and this process's memory), pixmaps (the screen's, an AHB's, memory's, one without a
 * private), windows (one drawn into the screen pixmap, one redirected into an AHB, one into memory) */
static LorieBuffer ahbBuf = { { 7, LORIEBUFFER_AHARDWAREBUFFER } }, memBuf = { { 8, LORIEBUFFER_REGULAR } };
static LoriePixmapPriv rootPriv, ahbPriv, memPriv;
static PixmapRec rootPix, ahbPix, memPix, barePix;
static WindowRec rootWin, ahbWin, memWin;
static PictTransform xf;
static PictureRec pict(DrawablePtr d, uint32_t format) { return (PictureRec) { d, format, 0, NULL }; }

static const char *nameOf(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst) {
    static char name[64];
    lorieCompositeName(name, sizeof name, lorieCompositeKey(op, src, mask, dst));
    return name;
}

/* one composite through the wrap, its wrapped one moving the clock and the counters so */
static void composite(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, CARD16 w, CARD16 h, uint64_t us, uint64_t gpu,
                      uint64_t lock, uint64_t root, uint64_t ahb, uint32_t accesses, uint32_t locks) {
    below.us = us, below.gpu = gpu, below.lock = lock, below.root = root, below.ahb = ahb;
    below.accesses = accesses, below.locks = locks;
    pictureScreen.Composite(op, src, mask, dst, 1, 2, 3, 4, 5, 6, w, h);
}

int main(void) {
    char totals[512], classes[1024], want[1024];
    PictureRec solid, ahbSrc, ahbSrcX, memSrc, rootDst, rootWinDst, ahbWinSrc, memWinSrc, memDst, bareDst, maskA, maskRgb, maskCa;
    struct lorie_composite_class *c = fakePvfb.compositeClasses;
    int i;

    screenRec = (ScreenRec) { windowPixmap, &rootPix };
    rootPriv.buffer = &ahbBuf, ahbPriv.buffer = &ahbBuf, memPriv.buffer = &memBuf;
    rootPix = (PixmapRec) { { DRAWABLE_PIXMAP, &screenRec }, &rootPriv };
    ahbPix = (PixmapRec) { { DRAWABLE_PIXMAP, &screenRec }, &ahbPriv };
    memPix = (PixmapRec) { { DRAWABLE_PIXMAP, &screenRec }, &memPriv };
    barePix = (PixmapRec) { { DRAWABLE_PIXMAP, &screenRec }, NULL };
    rootWin = (WindowRec) { { DRAWABLE_WINDOW, &screenRec }, &rootPix };
    ahbWin = (WindowRec) { { DRAWABLE_WINDOW, &screenRec }, &ahbPix };
    memWin = (WindowRec) { { DRAWABLE_WINDOW, &screenRec }, &memPix };
    solid = pict(NULL, PICT_a8r8g8b8);
    ahbSrc = pict(&ahbPix.drawable, PICT_a8r8g8b8), ahbSrcX = pict(&ahbPix.drawable, PICT_x8r8g8b8);
    memSrc = pict(&memPix.drawable, PICT_x8r8g8b8), memDst = pict(&memPix.drawable, PICT_a8r8g8b8);
    rootDst = pict(&rootPix.drawable, PICT_x8r8g8b8), rootWinDst = pict(&rootWin.drawable, PICT_x8r8g8b8);
    ahbWinSrc = pict(&ahbWin.drawable, PICT_a8r8g8b8), memWinSrc = pict(&memWin.drawable, PICT_a8r8g8b8);
    bareDst = pict(&barePix.drawable, PICT_a8r8g8b8);
    maskA = pict(&memPix.drawable, PICT_a8), maskRgb = pict(&memPix.drawable, PICT_a8r8g8b8);
    maskCa = maskRgb, maskCa.componentAlpha = 1;

    /* classes */
    {
        static const char *const ops[] = { "clear", "src", "dst", "over", "overrev", "in", "inrev", "out", "outrev", "atop",
                                           "atoprev", "xor", "add", "saturate", "op14" };
        PictureRec xfSrc = ahbSrc, xfMask = maskA;
        xfSrc.transform = &xf, xfMask.transform = &xf;
        for (i = 0; i < 15; i++) {
            snprintf(want, sizeof want, "%s:solid_a>mem", ops[i]);
            CHECK(!strcmp(nameOf((CARD8) i, &solid, NULL, &memDst), want), "operator %d: %s (want %s)", i,
                  nameOf((CARD8) i, &solid, NULL, &memDst), want);
        }
        CHECK(!strcmp(nameOf(200, &solid, NULL, &memDst), "op200:solid_a>mem"), "operator 200: %s", nameOf(200, &solid, NULL, &memDst));
        CHECK(!strcmp(nameOf(1, &ahbSrcX, NULL, &rootDst), "src:ahb_x>root"), "AHB into the screen pixmap: %s",
              nameOf(1, &ahbSrcX, NULL, &rootDst));
        CHECK(!strcmp(nameOf(1, &memSrc, NULL, &rootWinDst), "src:mem_x>root"), "into a window drawn into the screen pixmap: %s",
              nameOf(1, &memSrc, NULL, &rootWinDst));
        CHECK(!strcmp(nameOf(3, &ahbWinSrc, NULL, &memDst), "over:ahb_a>mem"), "from a window redirected into an AHB: %s",
              nameOf(3, &ahbWinSrc, NULL, &memDst));
        CHECK(!strcmp(nameOf(3, &memWinSrc, NULL, &bareDst), "over:mem_a>mem"), "a window in memory into a pixmap without a "
              "private: %s", nameOf(3, &memWinSrc, NULL, &bareDst));
        CHECK(!strcmp(nameOf(3, &ahbSrc, &maskA, &memDst), "over:ahb_a>mem+mask_a"), "alpha mask: %s", nameOf(3, &ahbSrc, &maskA, &memDst));
        CHECK(!strcmp(nameOf(3, &ahbSrc, &maskRgb, &memDst), "over:ahb_a>mem+mask_rgb"), "colour mask: %s",
              nameOf(3, &ahbSrc, &maskRgb, &memDst));
        CHECK(!strcmp(nameOf(3, &ahbSrc, &maskCa, &memDst), "over:ahb_a>mem+mask_ca"), "component alpha mask: %s",
              nameOf(3, &ahbSrc, &maskCa, &memDst));
        CHECK(!strcmp(nameOf(1, &xfSrc, NULL, &memDst), "src:ahb_a>mem+xf"), "source transformed: %s", nameOf(1, &xfSrc, NULL, &memDst));
        CHECK(!strcmp(nameOf(3, &ahbSrc, &xfMask, &memDst), "over:ahb_a>mem+mask_a+xf"), "mask transformed: %s",
              nameOf(3, &ahbSrc, &xfMask, &memDst));
    }

    /* a composite through the wrap: passed through, the wrap put back, its time split */
    pictureScreen.Composite = exaComposite;
    fakePvfb.composite = pictureScreen.Composite;
    pictureScreen.Composite = lorieCompositeTimed;
    lorieCompositeReport(totals, sizeof totals, classes, sizeof classes);     /* the period begins */
    composite(3, &ahbSrc, &maskA, &memDst, 800, 600, 5000, 1200, 300, 700, 100, 3, 2);
    CHECK(below.calls == 1 && below.wrapOff && below.op == 3 && below.src == &ahbSrc && below.mask == &maskA && below.dst == &memDst &&
          below.args[0] == 1 && below.args[1] == 2 && below.args[2] == 3 && below.args[3] == 4 && below.args[4] == 5 &&
          below.args[5] == 6 && below.width == 800 && below.height == 600, "the wrapped Composite not called (%d) once with its "
          "arguments, the wrap off (%d)", below.calls, below.wrapOff);
    CHECK(pictureScreen.Composite == lorieCompositeTimed && fakePvfb.composite == exaComposite, "the wrap not put back");
    CHECK(c[0].n == 1 && c[0].key == lorieCompositeKey(3, &ahbSrc, &maskA, &memDst) && c[0].us == 5000 && c[0].gpuUs == 1200 &&
          c[0].lockUs == 300 && c[0].rootUs == 700 && c[0].ahbUs == 100 && c[0].accesses == 3 && c[0].locks == 2 &&
          c[0].px == 480000 && c[0].maxUs == 5000 && !c[1].n, "a composite: %u in its slot, %llu us (gpu %llu lock %llu root %llu "
          "ahb %llu), %u accesses %u locks, %llu px, max %u", c[0].n, (unsigned long long) c[0].us, (unsigned long long) c[0].gpuUs,
          (unsigned long long) c[0].lockUs, (unsigned long long) c[0].rootUs, (unsigned long long) c[0].ahbUs, c[0].accesses,
          c[0].locks, (unsigned long long) c[0].px, c[0].maxUs);
    CHECK(fakePvfb.worstComposite.us == 5000 && fakePvfb.worstComposite.gpuUs == 1200 && fakePvfb.worstComposite.lockUs == 300 &&
          fakePvfb.worstComposite.rootUs == 700 && fakePvfb.worstComposite.ahbUs == 100 && fakePvfb.worstComposite.width == 800 &&
          fakePvfb.worstComposite.height == 600 && fakePvfb.worstComposite.key == c[0].key, "the slowest not that one");
    composite(3, &ahbSrc, &maskA, &memDst, 10, 10, 1000, 0, 0, 0, 0, 3, 1);
    CHECK(c[0].n == 2 && c[0].us == 6000 && c[0].maxUs == 5000 && c[0].px == 480100 && c[0].accesses == 6 && c[0].locks == 3 &&
          !c[1].n && fakePvfb.worstComposite.us == 5000, "the same class again: %u in its slot, %llu us, max %u, next slot %u",
          c[0].n, (unsigned long long) c[0].us, c[0].maxUs, c[1].n);
    below.rewrap = true;
    composite(1, &memSrc, NULL, &rootDst, 1, 1, 10, 0, 0, 0, 0, 1, 0);
    below.rewrap = false;
    CHECK(pictureScreen.Composite == lorieCompositeTimed && fakePvfb.composite == otherComposite && c[1].n == 1,
          "a Composite changed below the wrap not kept, or the wrap not put back over it");
    fakePvfb.composite = exaComposite;

    /* the report: totals, the slowest, the 4 classes that took longest (the last slot's as "other") and how many more */
    lorieCompositeReport(totals, sizeof totals, classes, sizeof classes);
    {
        uint64_t before = fakePvfb.composites;
        PictureRec memDstX = pict(&memPix.drawable, PICT_x8r8g8b8);
        static const CARD8 small[] = { 12, 5, 7 };      /* add, in, out: one class each */
        for (i = 0; i < 3; i++)
            composite(3, &ahbSrc, &maskA, &memDst, 100, 100, 3000, 1000, 0, 0, 0, 3, 1);
        for (i = 0; i < 2; i++)
            composite(1, &memSrc, NULL, &rootWinDst, 1920, 1080, 4000, 0, 0, 1500, 0, 2, 0);
        for (i = 0; i < 10; i++)
            composite(3, &solid, NULL, &memDst, 10, 10, 100, 0, 0, 0, 0, 1, 0);
        composite(1, &ahbSrcX, NULL, &memDst, 50, 50, 500, 0, 0, 0, 0, 1, 0);
        composite(0, &solid, NULL, &memDst, 1, 1, 50, 0, 0, 0, 0, 1, 0);
        for (i = 0; i < 3; i++)
            composite(small[i], &solid, NULL, &memDst, 1, 1, 10, 0, 0, 0, 0, 1, 0);
        CHECK(c[LORIE_COMPOSITE_CLASSES - 1].n == 1 && !c[LORIE_COMPOSITE_CLASSES].n, "eight classes not in eight slots");
        composite(11, &solid, NULL, &memDstX, 20, 20, 700, 0, 0, 0, 0, 1, 0);       /* a ninth class, and a tenth */
        composite(9, &solid, NULL, &memDstX, 1, 1, 20, 0, 0, 0, 0, 1, 0);
        CHECK(c[LORIE_COMPOSITE_CLASSES].n == 2 && c[LORIE_COMPOSITE_CLASSES].us == 720, "the classes past %d not in the last "
              "slot: %u, %llu us", LORIE_COMPOSITE_CLASSES, c[LORIE_COMPOSITE_CLASSES].n,
              (unsigned long long) c[LORIE_COMPOSITE_CLASSES].us);
        lorieCompositeReport(totals, sizeof totals, classes, sizeof classes);
        snprintf(want, sizeof want, "composites=%llu(+22) px=4181105 us=19300 cpu_us=13300 gpu_us=3000 root_us=3000 lock_us=0 "
                 "ahb_us=0 accesses=30 ahb_locks=3 worst_us=4000 worst=src:mem_x>root:1920x1080:cpu=2500,gpu=0,root=1500,lock=0,ahb=0",
                 (unsigned long long) before + 22);
        CHECK(!strcmp(totals, want), "report totals:\n    %s\n  want\n    %s", totals, want);
        CHECK(!strcmp(classes, "over:ahb_a>mem+mask_a=3/30/9000/6000/3000/0/9/3000,src:mem_x>root=2/4147/8000/5000/0/3000/4/4000,"
                               "over:solid_a>mem=10/1/1000/1000/0/0/10/100,other=2/0/720/720/0/0/2/700 classes_more=5"),
              "report classes: %s", classes);
        for (i = 0; i <= LORIE_COMPOSITE_CLASSES; i++)
            CHECK(!c[i].n && !c[i].us && !c[i].px && !c[i].maxUs, "slot %d not reset by the report", i);
        CHECK(!fakePvfb.worstComposite.us && fakePvfb.composites == before + 22, "the slowest not reset, or the count not kept");
        lorieCompositeReport(totals, sizeof totals, classes, sizeof classes);
        snprintf(want, sizeof want, "composites=%llu(+0) px=0 us=0 cpu_us=0 gpu_us=0 root_us=0 lock_us=0 ahb_us=0 accesses=0 "
                 "ahb_locks=0 worst_us=0 worst=none:0x0:cpu=0,gpu=0,root=0,lock=0,ahb=0", (unsigned long long) before + 22);
        CHECK(!strcmp(totals, want) && !strcmp(classes, "none classes_more=0"), "an empty period: %s / %s", totals, classes);
    }

    printf("TEXACOMPOSITE the RENDER composites timed by class (P11-C3-D1): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
