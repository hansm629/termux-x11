/* T05/T06: root slot handover. The handover, stale tracking and repair functions and LoriePixmapPriv are
 * extracted from InitOutput.c by gen.py; regions are real pixman (compiled from the tree's own copy). Only
 * the GPU's write and the X server's CPU drawing are simulated, each with the same bookkeeping the real
 * code does around them (see enqueueRootCopy / xDraw). The renderer holds the two most recently published
 * slots, as the zero-copy path does (one on screen, one retiring). */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <pixman.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))
typedef pixman_region16_t RegionRec, *RegionPtr;
typedef pixman_box16_t BoxRec, *BoxPtr;
static inline void RegionNull(RegionPtr r) { pixman_region_init(r); }
static inline void RegionInit(RegionPtr r, BoxPtr b, int n) { (void) n; if (b) pixman_region_init_rect(r, b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1); else pixman_region_init(r); }
static inline void RegionUninit(RegionPtr r) { pixman_region_fini(r); }
static inline Bool RegionCopy(RegionPtr d, RegionPtr s) { return pixman_region_copy(d, s); }
static inline Bool RegionUnion(RegionPtr d, RegionPtr a, RegionPtr b) { return pixman_region_union(d, a, b); }
static inline Bool RegionIntersect(RegionPtr d, RegionPtr a, RegionPtr b) { return pixman_region_intersect(d, a, b); }
static inline Bool RegionSubtract(RegionPtr d, RegionPtr a, RegionPtr b) { return pixman_region_subtract(d, a, b); }
static inline Bool RegionNotEmpty(RegionPtr r) { return pixman_region_not_empty(r); }
static inline void RegionEmpty(RegionPtr r) { pixman_region_clear(r); }
static inline int RegionNumRects(RegionPtr r) { return pixman_region_n_rects(r); }
static inline BoxPtr RegionRects(RegionPtr r) { return pixman_region_rectangles(r, NULL); }
/* pixman's region code links against these for region_init_from_image and logging */
void _pixman_log_error(const char *f, const char *m) { (void) f; (void) m; }
int pixman_image_get_width(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_height(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_stride(pixman_image_t *i) { (void) i; return 0; }
uint32_t *pixman_image_get_data(pixman_image_t *i) { (void) i; return NULL; }

typedef struct { int32_t width, stride, height; uint64_t id; } LorieBuffer_Desc;
typedef struct { LorieBuffer_Desc desc; } LorieBuffer;
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }
#define LORIE_TRACE_PUBLISH 0
#define lorieTrace(...) do {} while (0)
static uint64_t fakeNow = 1, fakeCompleted = 0;
static uint64_t lorieNowUs(void) { return fakeNow++; }
static Bool lorieGpuCopyResolved(uint64_t serial) { return fakeCompleted >= serial; }
/* Only the handover from before the region tracking asks this; it means "a copy into this slot has not
 * landed yet", which the harness knows from what it queued and what it let land. */
static uint64_t slotPendingSerial[8];
static LorieBuffer bufs[8];
static Bool LorieBuffer_hasGpuCopyPending(LorieBuffer *b) { return slotPendingSerial[b - bufs] > fakeCompleted; }
static struct {
    volatile uint32_t rootHandover;
    volatile uint64_t rootBufferIds[8];
    struct { uint64_t rootCopyBytes; uint32_t rootCopyUs, rootCopies, rootPublishAttempts, rootPublishHeldForRepair,
             rootPublishNoSlot, rootPublishes, rootStalePostponed, rootOwedRepairs, rootHandoverDeferrals,
             rootUnpublishedMaxUs; } presentStats;
} fakeState;
static struct { typeof(fakeState) *state; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
#include "t05_src.inc"

#define W 64
#define H 16
static uint32_t pixels[LORIE_ROOT_SLOTS][W * H];
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* as lorieEnsureRootDoubleBuffer leaves it: slot 0 published, drawing into 1 */
static void init(LoriePixmapPriv *priv) {
    memset(priv, 0, sizeof *priv); memset(pixels, 0, sizeof pixels);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) {
        bufs[i].desc = (LorieBuffer_Desc) { W, W, H, 100 + i };
        priv->rootBuf[i] = &bufs[i]; priv->rootLocked[i] = pixels[i];
        RegionNull(&priv->rootStale[i]);
#ifdef HAVE_GPU_PENDING
        RegionNull(&priv->rootGpuPending[i]);
#endif
        slotPendingSerial[i] = 0;
        fakeState.rootBufferIds[i] = 100 + i;
    }
#ifdef HAVE_OWED
    RegionNull(&priv->rootOwed); priv->rootOwedDonor = -1;
#endif
    fakeState.rootHandover = 0; priv->rootWrite = 1; priv->rootDouble = TRUE; fakeCompleted = 0;
}
static void fill(int slot, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) pixels[slot][y * W + x] = v;
}
static int areaIs(int slot, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) if (pixels[slot][y * W + x] != v) return 0;
    return 1;
}
/* the X server's bookkeeping when it queues a copy into the root (lorieTryScheduleGpuCopy) */
static void enqueueRootCopy(LoriePixmapPriv *priv, BoxRec b, uint64_t serial) {
    RegionRec r; RegionInit(&r, &b, 1);
    lorieMarkRootStale(priv, &r);
    slotPendingSerial[priv->rootWrite] = serial;
#ifdef HAVE_GPU_PENDING
    RegionUnion(&priv->rootGpuPending[priv->rootWrite], &priv->rootGpuPending[priv->rootWrite], &r);
    priv->rootGpuPendingSerial[priv->rootWrite] = serial;
#endif
    RegionUninit(&r);
}
/* the renderer's GPU write landing, and the watermark it then publishes */
static void gpuLands(int slot, BoxRec b, uint32_t v, uint64_t serial) { fill(slot, b, v); fakeCompleted = serial; }
/* the X server drawing with the CPU into the slot it draws into, and the damage it then records */
static void xDraw(LoriePixmapPriv *priv, BoxRec b, uint32_t v) {
    RegionRec r; RegionInit(&r, &b, 1);
    fill(priv->rootWrite, b, v); lorieMarkRootStale(priv, &r); RegionUninit(&r);
}
/* the renderer: holds what was published last and the one before it, gives back the rest */
static int published = -1, retiring = -1;
static Bool handover(LoriePixmapPriv *priv) {
    int drawn = priv->rootWrite;
    Bool ok = lorieRootHandover(priv);
    if (ok) {
        uint32_t held = 0;
        retiring = published; published = drawn;
        if (published >= 0) held |= 1u << published;
        if (retiring >= 0) held |= 1u << retiring;
        fakeState.rootHandover = (fakeState.rootHandover & ~LORIE_ROOT_HELD_MASK) | held;
    }
    return ok;
}

int main(void) {
    LoriePixmapPriv priv;
    BoxRec R = { 8, 2, 24, 10 }, Q = { 40, 2, 56, 10 };

    /* T05 - the review's counterexample: R reaches A by GPU, the handover cannot carry it to B yet, the
     * copy then lands, only Q is drawn into B. B must not be published with R old, and nothing published
     * after it may carry R back to its old content. */
    init(&priv); published = 0; retiring = -1;
    int A = priv.rootWrite;
    enqueueRootCopy(&priv, R, 1);
    CHECK(handover(&priv), "publish A");
    int B = priv.rootWrite;
    gpuLands(A, R, 1, 1);
    xDraw(&priv, Q, 2);
    CHECK(handover(&priv), "publish B");
    CHECK(areaIs(B, R, 1), "B published with R still old - the demo area reverts");
    CHECK(areaIs(B, Q, 2), "B lost Q");
    int C = priv.rootWrite;
    CHECK(handover(&priv), "publish C");
    CHECK(areaIs(C, R, 1) && areaIs(C, Q, 2), "C was carried old content from B");

    /* T06 - a client presenting into the root every frame: each frame queues a copy into the drawing slot
     * and it only lands during the next one. Publishing must go on regardless, and every slot that goes
     * out must have the latest frame's pixels, never an older one. */
    init(&priv); published = 0; retiring = -1;
    int stuck = 0, stale = 0;
    for (uint32_t f = 1; f <= 60; f++) {
        enqueueRootCopy(&priv, R, f);         /* queued into the drawing slot; not landed yet */
        if (!handover(&priv)) { stuck++; continue; }
        /* the renderer applies the queued copy to the slot before it shows it (R4), then checks */
        gpuLands(published, R, f, f);
        if (!areaIs(published, R, f)) stale++;
    }
    CHECK(stuck == 0, "continuous writer: %d of 60 publishes held back", stuck);
    CHECK(stale == 0, "continuous writer: %d slots published with an older frame", stale);

#ifdef HAVE_OWED
    /* T14 - a publish that failed once and succeeded on the next tick waited that whole interval, and the
     * longest-wait figure must say so (it said 0 when the outstanding mark was cleared first). The redraw
     * sets rootDirtySinceUs on the failed attempt; lorieNoteRootPublished closes it out on success. */
    init(&priv);
    fakeState.presentStats.rootUnpublishedMaxUs = 0;
    fakeNow = 1000000;
    priv.rootDirtySinceUs = 1000000;            /* t0: the attempt that failed */
    fakeNow = 1016667;                          /* t1: the next vsync, where it succeeds */
    lorieNoteRootPublished(&priv);
    CHECK(fakeState.presentStats.rootUnpublishedMaxUs == 16667, "waited 16.667 ms, recorded %u us",
          fakeState.presentStats.rootUnpublishedMaxUs);
    CHECK(priv.rootDirtySinceUs == 0, "wait not closed out");
#endif

    printf("T05/T06 root handover: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
