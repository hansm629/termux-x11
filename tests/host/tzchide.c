/* TZCHIDE: leaving the root layer (rootZcStopPresenting, teardownRootOverlay) and giving the slot it showed
 * back to the X server. rootZcStopPresenting, rootZcParkingBuffer, teardownRootOverlay, rootZcOnComplete,
 * rootZcDrainRetiring, rootZcFenceState and rendererReleaseRootSlot are the real code (gen.py "zclife");
 * the compositor is simulated as AOSP has it:
 *   - a transaction that sets a buffer releases the one it replaces: if that one was being displayed, its
 *     release fence is the present fence of the frame that no longer shows it (signalled at the next
 *     vsync; Display::setReleasedLayers / Output::presentFrameAndReleaseLayers for a layer leaving the
 *     display with a new buffer queued), else it is released at once (-1);
 *   - a transaction that changes no buffer gets no previous release fence (-1), and a hidden layer's
 *     buffer is still scanned out until the next vsync.
 * What has to hold: a slot goes back to the X server only once the compositor has stopped reading it -
 * never between the hide's completion and the vsync; without the buffer a hide is given (allocation
 * failing) the slot stays held until its pool is replaced, and then nothing of it is left; a completion
 * arriving after the pool was replaced never gives back a slot of the new pool. The code from before the
 * parking buffer (a hide only, its -1 taken as released) has to fail the first of these. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

typedef struct ASurfaceControl { int id; } ASurfaceControl;
typedef struct AHardwareBuffer { int id; } AHardwareBuffer;
typedef struct ANativeWindow ANativeWindow;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
typedef struct ASurfaceTransactionStats { int prevFence; ASurfaceControl *control; } ASurfaceTransactionStats;
typedef struct ASurfaceTransaction {
    int setVisibility, visible, setBuffer, reparentNull;
    AHardwareBuffer *buffer;
    void *ctx;
    void (*cb)(void *, ASurfaceTransactionStats *);
} ASurfaceTransaction;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ull << 8)
#define ASURFACE_TRANSACTION_VISIBILITY_HIDE 0
#define ASURFACE_TRANSACTION_VISIBILITY_SHOW 1
#include "rootslots_src.inc"
#define LORIE_TRACE_RELEASE 11
#define lorieTrace(...) ((void) 0)
#define log(...) ((void) 0)

static struct {
    volatile uint32_t rootHandover;
    volatile uint64_t rootBufferIds[LORIE_ROOT_SLOTS];
    volatile uint32_t outputRetryPending;
    struct { uint32_t zeroCopyFenceErrors, rootStaleSlotReleases; } presentStats;
} fakeState, *state = &fakeState;
static bool rootZcRetryPending;
static int64_t rendererNowNs(void) { return 0; }
static int parkingAllocFails, parkingAllocs;
static AHardwareBuffer parkingAhb = { 99 };
static int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) {
    (void) d;
    parkingAllocs++;
    if (parkingAllocFails) { *out = NULL; return -1; }
    *out = &parkingAhb;
    return 0;
}
static void rootZcNoteCompletion(uint32_t seq, ASurfaceTransactionStats *stats) { (void) seq; (void) stats; }
#include "zclife_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the compositor ---- */
static ASurfaceControl layer = { 1 };
static AHardwareBuffer slotAhb[LORIE_ROOT_SLOTS];
static struct { AHardwareBuffer *buffer; int visible, reparented; } L;    /* the root layer */
static AHardwareBuffer *scannedOut;          /* what the display reads until the next vsync */
static int pendingFence[16][2], nPendingFence;  /* release fences to signal at the next vsync */
static ASurfaceTransaction *queue[16];
static int nQueue;

static ASurfaceTransaction *txCreate(void) { return calloc(1, sizeof(ASurfaceTransaction)); }
static void txDelete(ASurfaceTransaction *t) { (void) t; }   /* the queue keeps it until the commit */
static void txApply(ASurfaceTransaction *t) { queue[nQueue++] = t; }
static void txSetVisibility(ASurfaceTransaction *t, ASurfaceControl *c, int8_t v) { (void) c; t->setVisibility = 1; t->visible = v; }
static void txSetBuffer(ASurfaceTransaction *t, ASurfaceControl *c, AHardwareBuffer *b, int fence) { (void) c; (void) fence; t->setBuffer = 1; t->buffer = b; }
static void txSetGeometry(ASurfaceTransaction *t, ASurfaceControl *c, const ARect *s, const ARect *d, int32_t tr) { (void) t; (void) c; (void) s; (void) d; (void) tr; }
static void txReparent(ASurfaceTransaction *t, ASurfaceControl *c, ASurfaceControl *p) { (void) c; t->reparentNull = p == NULL; }
static void txSetOnComplete(ASurfaceTransaction *t, void *ctx, void (*cb)(void *, ASurfaceTransactionStats *)) { t->ctx = ctx; t->cb = cb; }
static void scRelease(ASurfaceControl *c) { (void) c; }
static int statsPrevReleaseFenceFd(ASurfaceTransactionStats *s, ASurfaceControl *c) { (void) c; return s->prevFence >= 0 ? dup(s->prevFence) : -1; }
static void statsGetControls(ASurfaceTransactionStats *s, ASurfaceControl ***out, size_t *n) {
    *out = malloc(sizeof(ASurfaceControl *)); (*out)[0] = s->control; *n = 1;
}
static void statsReleaseControls(ASurfaceControl **c) { free(c); }

/* one frame: every queued transaction applied, then the completions; `late` holds them back */
static ASurfaceTransaction *heldBack[16];
static int nHeldBack;
static void complete(ASurfaceTransaction *t, int fence) {
    if (t->cb) {
        ASurfaceTransactionStats st = { fence, &layer };
        t->cb(t->ctx, &st);
    }
}
static int fenceOf[16];
static void commit(bool late) {
    for (int i = 0; i < nQueue; i++) {
        ASurfaceTransaction *t = queue[i];
        int fence = -1;
        if (t->setBuffer) {
            if (L.buffer && L.visible) {             /* replaced while displayed: released at the next vsync */
                pipe(pendingFence[nPendingFence]);
                fence = pendingFence[nPendingFence++][0];
            }
            L.buffer = t->buffer;
        }
        if (t->setVisibility) L.visible = t->visible;
        if (t->reparentNull) L.reparented = 1;
        if (late) { fenceOf[nHeldBack] = fence; heldBack[nHeldBack++] = t; }
        else { complete(t, fence); free(t); }
    }
    nQueue = 0;
}
static void deliverHeldBack(void) {
    for (int i = 0; i < nHeldBack; i++) { complete(heldBack[i], fenceOf[i]); free(heldBack[i]); }
    nHeldBack = 0;
}
static void vsync(void) {
    for (int i = 0; i < nPendingFence; i++) { char c = 1; write(pendingFence[i][1], &c, 1); }
    nPendingFence = 0;
    scannedOut = L.visible && !L.reparented ? L.buffer : NULL;
}

/* a slot given back must not be one the display still reads, nor one on a visible layer */
static void giveBackCheck(const char *when) {
    for (int s = 0; s < LORIE_ROOT_SLOTS; s++) {
        if (fakeState.rootHandover & (1u << s)) continue;
        CHECK(scannedOut != &slotAhb[s], "%s: slot %d given back while the display still scans it out", when, s);
        CHECK(!(L.visible && !L.reparented && L.buffer == &slotAhb[s]), "%s: slot %d given back while shown", when, s);
    }
}
static void poll_(const char *when) { rootZcDrainRetiring(); giveBackCheck(when); }

/* the root layer showing slot `s`, nothing retiring */
static void setup(int s) {
    memset(&fakeState, 0, sizeof fakeState);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = 100 + i;
    fakeState.rootHandover = 1u << s;
    rootZcDisplayedSlot = s; rootZcDisplayedId = 100 + s; rootZcDisplayedGen = 0;
    rootZcRetiringCount = 0; rootZcUnusableCount = 0;
    rootSurfaceControl = &layer;
    L.buffer = &slotAhb[s]; L.visible = 1; L.reparented = 0;
    scannedOut = &slotAhb[s];
    nQueue = nPendingFence = nHeldBack = 0;
}

int main(void) {
    scApi.txCreate = txCreate; scApi.txDelete = txDelete; scApi.txApply = txApply;
    scApi.txSetVisibility = txSetVisibility; scApi.txSetBuffer = txSetBuffer; scApi.txSetGeometry = txSetGeometry;
    scApi.txReparent = txReparent; scApi.txSetOnComplete = txSetOnComplete; scApi.release = scRelease;
    scApi.statsPrevReleaseFenceFd = statsPrevReleaseFenceFd; scApi.statsGetControls = statsGetControls;
    scApi.statsReleaseControls = statsReleaseControls;

#ifndef PARKING_FAILS
    /* 1. stop presenting: the hide, its completion, a renderer frame before the vsync, the vsync */
    setup(2);
    rootZcStopPresenting();
    commit(false);
    poll_("stop, before the vsync");
    CHECK(fakeState.rootHandover & (1u << 2), "stop: slot 2 given back before the vsync");
    vsync();
    poll_("stop, after the vsync");
    CHECK(!(fakeState.rootHandover & (1u << 2)), "stop: slot 2 never given back once released");
    CHECK(rootZcRetiringCount == 0, "stop: %d retiring entries left", rootZcRetiringCount);

    /* 2. the surface torn down: hide, then hidden and reparented in a second transaction */
    setup(3);
    teardownRootOverlay();
    commit(false);
    poll_("teardown, before the vsync");
    CHECK(fakeState.rootHandover & (1u << 3), "teardown: slot 3 given back before the vsync");
    vsync();
    poll_("teardown, after the vsync");
    CHECK(!(fakeState.rootHandover & (1u << 3)) && rootZcRetiringCount == 0, "teardown: slot 3 not given back");

    /* 3. the completion coming after the X server replaced the pool, whose renderer claimed the same index */
    setup(1);
    rootZcStopPresenting();
    commit(true);                                   /* the compositor has it; the completion is late */
    fakeState.rootHandover = (2u << LORIE_ROOT_GEN_SHIFT) | (1u << 1);   /* new pool, slot 1 claimed in it */
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = 200 + i;
    vsync();
    deliverHeldBack();
    rootZcDrainRetiring();
    CHECK(fakeState.rootHandover & (1u << 1), "a late completion gave back slot 1 of the new pool");
    CHECK(rootZcRetiringCount == 0, "late completion: %d entries of the old pool left", rootZcRetiringCount);
#else
    /* 4. no buffer to hide it with (allocation fails): nothing will release the slot, so it stays held -
     * through the hide, its vsync and any number of frames - until its pool is replaced; then nothing of
     * it is left, and the count of stuck slots is back where it was */
    parkingAllocFails = 1;
    setup(4);
    rootZcStopPresenting();
    commit(false);
    for (int i = 0; i < 10; i++) {
        poll_("no parking buffer");
        vsync();
    }
    CHECK(fakeState.rootHandover & (1u << 4), "no parking buffer: slot 4 given back with no release");
    CHECK(rootZcRetiringCount == 1 && rootZcUnusableCount == 1, "no parking buffer: %d entries, %d stuck",
          rootZcRetiringCount, rootZcUnusableCount);
    CHECK(!L.visible, "no parking buffer: the layer not hidden");
    fakeState.rootHandover = 2u << LORIE_ROOT_GEN_SHIFT;                  /* the pool replaced */
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = 200 + i;
    rootZcDrainRetiring();
    CHECK(rootZcRetiringCount == 0 && rootZcUnusableCount == 0, "pool replaced: %d entries, %d stuck left",
          rootZcRetiringCount, rootZcUnusableCount);
#endif
#ifndef PARKING_FAILS
    printf("TZCHIDE leaving the root layer, slots given back on the compositor's release: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
#else
    printf("TZCHIDE without a parking buffer, the slot held until its pool goes: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
#endif
    return fails != 0;
}
