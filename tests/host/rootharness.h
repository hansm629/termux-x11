/* Shared by the root slot tests (t05.c, t26.c). The handover, stale tracking and repair functions and
 * LoriePixmapPriv are extracted from InitOutput.c by gen.py into root_src.inc; regions are real pixman
 * (compiled from the tree's own copy). Only the GPU's write, the renderer's report of it and the X
 * server's CPU drawing are simulated, each with the same bookkeeping the real code does around them (see
 * enqueueRootCopy / xDraw). The renderer holds the two most recently published slots, as the zero-copy
 * path does (one on screen, one retiring).
 *
 * Builds against code from before rootReplacing existed as well (root_src.inc then has no
 * HAVE_REPLACING), with the bookkeeping that code did - which is how a test is shown to fail there. */
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

typedef struct { int32_t width, stride, height; uint64_t id; int format, type; } LorieBuffer_Desc;
typedef struct { LorieBuffer_Desc desc; void *mem; int locked, refs, registered; } LorieBuffer;
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#define LORIEBUFFER_REGULAR 0
#define LORIEBUFFER_FD 1
#define LORIEBUFFER_AHARDWAREBUFFER 2
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }
#define LORIE_TRACE_PUBLISH 0
#define lorieTrace(...) do {} while (0)

/* The renderer's two answers about a serial (lorieGpuCopyResolved, lorieGpuCopyMade): it passes them
 * in order, so a watermark says which have been dealt with, and a set says which of those failed. */
static uint64_t fakeNow = 1, fakeCompleted = 0;
#define FAKE_SERIALS 4096
static uint8_t fakeFailed[FAKE_SERIALS];
static uint64_t lorieNowUs(void) { return fakeNow++; }
static Bool lorieGpuCopyResolved(uint64_t serial) { return fakeCompleted >= serial; }
static Bool lorieGpuCopyMade(uint64_t serial) { return fakeCompleted >= serial && !fakeFailed[serial % FAKE_SERIALS]; }
/* Only the handover from before the region tracking asks this; it means "a copy into this slot has not
 * landed yet", which the harness knows from what it queued and what it let land. */
static uint64_t slotPendingSerial[8];
static LorieBuffer bufs[8];
static Bool LorieBuffer_hasGpuCopyPending(LorieBuffer *b) { return slotPendingSerial[b - bufs] > fakeCompleted; }
static struct {
    volatile uint32_t rootHandover;
    volatile uint8_t rootDoubleBuffered;
    volatile uint64_t rootBufferIds[8];
    struct { uint64_t rootCopyBytes; uint32_t rootCopyUs, rootCopies, rootPublishAttempts, rootPublishHeldForRepair,
             rootPublishNoSlot, rootPublishes, rootStalePostponed, rootOwedRepairs, rootHandoverDeferrals,
             rootUnpublishedMaxUs, rootReplacementsNotMade, rootOwedFromOlder, rootOwedLost,
             rootReplacingFull, rootPublishHeldForDonor, rootOwedExposed;
             uint64_t cpuCarryBytes, cpuOwedFetchBytes;
             uint32_t gpuCarryJobs, gpuCarryTakenBack, gpuCarryNotMade, cpuCarryKept[8];
             uint64_t gpuCarryBytes, cpuSeedBytes; } presentStats;
} fakeState;
static struct { typeof(fakeState) *state; struct { Bool legacyDrawing; } root; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
/* what lorieRootCopyCancelled looks the root up through */
typedef void *PixmapPtr;
typedef struct FakeScreen *ScreenPtr;
struct FakeScreen { PixmapPtr (*GetScreenPixmap)(ScreenPtr);
                   Bool (*ModifyPixmapHeader)(PixmapPtr, int, int, int, int, int, void *); };
static void *fakeRootPriv;
static PixmapPtr fakeGetScreenPixmap(ScreenPtr s) { (void) s; return fakeRootPriv; }
static int fakeHeaderPitch;
static Bool fakeModifyPixmapHeader(PixmapPtr p, int w, int h, int d, int b, int pitch, void *data) {
    (void) p; (void) w; (void) h; (void) d; (void) b; (void) data;
    fakeHeaderPitch = pitch;
    return TRUE;
}
static struct FakeScreen fakeScreen = { fakeGetScreenPixmap, fakeModifyPixmapHeader };
static ScreenPtr pScreenPtr __attribute__((unused)) = &fakeScreen;
#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(p) ((LoriePixmapPriv *) (p))
/* What lorieMarkQueuedCopySuperseded does to a copy still waiting in the queue: the test's queue says
 * whether it was (and marks it cancelled), and then the X server's bookkeeping hears of it. */
static int (*harnessCancel)(uint64_t serial);
static void harnessCopyCancelled(uint64_t serial);
static void __attribute__((unused)) lorieMarkQueuedCopySuperseded(uint64_t serial) {
    if (harnessCancel && harnessCancel(serial))
        harnessCopyCancelled(serial);
}
/* Where the root's CPU copies are counted (lorieCountCpuCopy): just the site, here. */
static void __attribute__((unused)) lorieCountCpuCopy(volatile uint64_t *site, uint64_t bytes) { *site += bytes; }
#include "root_src.inc"
#ifdef HAVE_CARRY
/* Whether the GPU takes a handover's carry, and what queuing one of its copies does - a test that wants
 * carries on the GPU sets both. Left alone, every carry is the CPU's, as it was before there were any. */
static int harnessCarryOn;
static uint64_t (*harnessQueueCarry)(int from, int to, BoxPtr box, int n);
static Bool lorieRootCarryAllowed(int entries) { (void) entries; return harnessCarryOn && harnessQueueCarry; }
static uint64_t lorieQueueRootSlotCopy(LoriePixmapPriv *priv, int from, int to, BoxPtr box, int n) {
    (void) priv;
    return harnessQueueCarry(from, to, box, n);
}
#endif
/* What the shared lock waits out once a PrepareAccess takes it: copies the renderer had claimed. */
static void (*harnessLockWait)(void);
static void harnessCopyCancelled(uint64_t serial) {
#ifdef HAVE_REPLACING
    lorieRootCopyCancelled(serial);
#else
    (void) serial;
#endif
}

#define W 64
#define H 16
static uint32_t pixels[LORIE_ROOT_SLOTS][W * H];
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* as lorieEnsureRootDoubleBuffer leaves it: slot 0 published, drawing into 1 */
static int published = -1, retiring = -1;
static void init(LoriePixmapPriv *priv) {
    memset(priv, 0, sizeof *priv); memset(pixels, 0, sizeof pixels); memset(fakeFailed, 0, sizeof fakeFailed);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) {
        bufs[i].desc = (LorieBuffer_Desc) { W, W, H, 100 + i };
        priv->rootBuf[i] = &bufs[i]; priv->rootLocked[i] = pixels[i];
        RegionNull(&priv->rootStale[i]);
#ifdef HAVE_GPU_PENDING
        RegionNull(&priv->rootGpuPending[i]);
#endif
#ifdef HAVE_REPLACING
        priv->rootCondDonor[i] = -1;
#endif
        slotPendingSerial[i] = 0;
        fakeState.rootBufferIds[i] = 100 + i;
    }
#ifdef HAVE_OWED
    RegionNull(&priv->rootOwed); priv->rootOwedDonor = -1;
#endif
#ifdef HAVE_CARRY
    RegionNull(&priv->rootOwedNow);
#endif
#ifdef HAVE_PINS
    RegionNull(&priv->rootLostArea);
#endif
    fakeState.rootHandover = 0; priv->rootWrite = 1; priv->rootDouble = TRUE; fakeCompleted = 0;
    published = 0; retiring = -1;
    fakeRootPriv = priv;
}
static void fill(int slot, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) pixels[slot][y * W + x] = v;
}
static int areaIs(int slot, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) if (pixels[slot][y * W + x] != v) return 0;
    return 1;
}
/* the X server's bookkeeping when it queues a copy into the root (lorieTryScheduleGpuCopy); FALSE when
 * it refuses the copy, which the caller then draws with the CPU */
static Bool enqueueRootCopy(LoriePixmapPriv *priv, BoxRec b, uint64_t serial) {
    RegionRec r; RegionInit(&r, &b, 1);
#ifdef HAVE_REPLACING
    if (!lorieRootCanQueueCopy(priv)) { RegionUninit(&r); return FALSE; }
    lorieRootNoteGpuCopy(priv, &r, serial);
#else
    lorieMarkRootStale(priv, &r);
#ifdef HAVE_GPU_PENDING
    RegionUnion(&priv->rootGpuPending[priv->rootWrite], &priv->rootGpuPending[priv->rootWrite], &r);
    priv->rootGpuPendingSerial[priv->rootWrite] = serial;
#endif
#endif
    slotPendingSerial[priv->rootWrite] = serial;
    RegionUninit(&r);
    return TRUE;
}
/* the renderer's GPU write landing, and the watermark it then publishes */
static void gpuLands(int slot, BoxRec b, uint32_t v, uint64_t serial) { fill(slot, b, v); fakeCompleted = serial; }
/* the renderer reporting a copy it did not make: given up on, its source never arrived, or cancelled */
static void gpuFails(uint64_t serial) { fakeFailed[serial % FAKE_SERIALS] = 1; fakeCompleted = serial; }
/* the X server cancelling a copy it had queued, before the renderer claimed it (lorieCancelQueuedEntry
 * won); the renderer reports it as not made when it reaches it */
static void xCancel(uint64_t serial) {
#ifdef HAVE_REPLACING
    lorieRootCopyCancelled(serial);
#else
    (void) serial;
#endif
}
/* the X server drawing with the CPU into the slot it draws into: PrepareAccess brings the slot up to
 * date first (loriePrepareAccess), and the damage reported after the operation marks the other slots
 * stale and takes the drawn area out of what is owed */
/* A PrepareAccess on the drawing slot, as loriePrepareAccess does it: queued carries taken back, the
 * slot brought up to date where it can be, then the shared lock, which waits out claimed copies. */
static void xPrepare(LoriePixmapPriv *priv) {
#ifdef HAVE_CARRY
    lorieRootTakeBackCarries(priv);
#endif
#ifdef HAVE_OWED
    lorieRepairRootOwed(priv);
#endif
    if (harnessLockWait)
        harnessLockWait();
}
static void xDraw(LoriePixmapPriv *priv, BoxRec b, uint32_t v) {
    RegionRec r; RegionInit(&r, &b, 1);
#ifdef HAVE_REPLACING
    xPrepare(priv);
    fill(priv->rootWrite, b, v);
    lorieMarkRootStale(priv, &r);
    lorieRootCpuDrawn(priv, &r);
#else
#ifdef HAVE_OWED
    lorieRepairRootOwed(priv);
#endif
    fill(priv->rootWrite, b, v);
    lorieMarkRootStale(priv, &r);
#endif
    RegionUninit(&r);
}
/* the renderer: holds what was published last and the one before it, gives back the rest */
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
