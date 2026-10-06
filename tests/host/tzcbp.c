/* TZCBP: the root-slot lifecycle against a compositor with buffer backpressure (and, built with
 * NO_BP_SYMBOL, on a system without ASurfaceTransaction_setEnableBackPressure; and, against a build that
 * does not ask for it - LORIE_ZC_REQUEST_BACKPRESSURE 0, the comparison build - with the call there but
 * never made).
 *
 * Real code: the X server's lorieRootHandover (rootharness.h), and the renderer's rendererClaimRootBuffer,
 * rootZcPublishedSinceClaim, rendererReleaseRootSlot, rootZcDrainRetiring, rootZcOnComplete,
 * rootZcFenceState, rootZcHandOver, rootZcStopPresenting, rootZcParkingBuffer, rootZcSetBackpressure and
 * teardownRootOverlay (gen.py "zcbp"). The frame around them is rendererRedrawLocked/rootZcPresent's
 * order: claim, give back what has been released and check for room, nothing-new, the copies, hand over,
 * apply. The compositor follows AOSP (as tools/model/zcslots.py does): a transaction is ready for a latch
 * if applied 0.3 ms before it; with backpressure one buffer per layer per flush, the rest stay queued in
 * order; without it every ready one is applied and all but the last buffer are dropped; the frame's first
 * buffer callback gets the release fence of the buffer on screen, signalled at the next vsync; a hidden
 * layer keeps its buffer. Timing from the 829b4ec trace, at 60 and 120 Hz.
 *
 * Every event: a slot whose held bit is clear is not one the compositor has (queued, latched, shown,
 * being released, kept by a hidden layer), nor the one a frame has claimed and is still using; the X
 * server never draws into such a slot or a held one; the renderer holds at most LORIE_ZC_MAX_HELD plus a
 * claim; the X server has two slots at every publish. Leaving the root layer for the GL path and coming
 * back is rendererRedrawLocked's order too: the GL frame claims the newest slot, leaves the layer
 * (rootZcStopPresenting), samples the slot and gives its claim back - and with nothing new published,
 * the newest slot is the one the layer still shows, so the claim, the slot on screen and the slot being
 * released are one buffer.
 * Every scenario: with backpressure, no buffer dropped by the compositor and no published frame lost
 * beyond the unavoidable (latches the compositor skipped, frames beyond the display rate, a frame longer
 * than a period, a pool replaced); presented in order; at the end nothing held that is not accounted for.
 * Latency is reported, not judged: a frame deeper after a late frame is the trade-off. */
#include "rootharness.h"
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>

typedef struct ASurfaceControl { int epoch; } ASurfaceControl;
static ASurfaceControl layers_ensure[4] = { { 4 }, { 5 }, { 6 }, { 7 } };
typedef struct AHardwareBuffer { int pool, slot; } AHardwareBuffer;
typedef struct ANativeWindow ANativeWindow;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
typedef struct ASurfaceTransactionStats { int prevFence; ASurfaceControl *control; } ASurfaceTransactionStats;
typedef struct ASurfaceTransaction {
    int visSet, vis, bufSet, bp, pub;
    AHardwareBuffer *buf;
    ASurfaceControl *layer;
    void *ctx;
    void (*cb)(void *, ASurfaceTransactionStats *);
    int64_t applied;
    int done;
} ASurfaceTransaction;
typedef struct { uint32_t width, height, layers, format; uint64_t usage; uint32_t stride, rfu0; uint64_t rfu1; } AHardwareBuffer_Desc;
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ull << 8)
#define ASURFACE_TRANSACTION_VISIBILITY_HIDE 0
#define ASURFACE_TRANSACTION_VISIBILITY_SHOW 1
#define LORIE_TRACE_RELEASE 11
static typeof(fakeState) *state = &fakeState;
static bool rootZcRetryPending;
static int64_t rendererNowNs(void) { return 0; }
#define log(...) ((void) 0)
static AHardwareBuffer parkingAhb = { -1, -1 };
static int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *d, AHardwareBuffer **out) { (void) d; *out = &parkingAhb; return 0; }
static void rootZcNoteCompletion(uint32_t seq, ASurfaceTransactionStats *stats) { (void) seq; (void) stats; }
#include "zcbp_src.inc"
/* a build that does not ask for backpressure even with the call there (the comparison build) */
#if defined(LORIE_ZC_REQUEST_BACKPRESSURE) && !LORIE_ZC_REQUEST_BACKPRESSURE
#define BP_NOT_REQUESTED 1
#endif
static bool cursorOverlayResolveApi(void) { return true; }
static int created;
static ASurfaceControl *createFromWindow(ANativeWindow *w, const char *name) { (void) w; (void) name; return &layers_ensure[created++ % 4]; }

/* ---- timing (tools/model/zcslots.py) ---- */
static int64_t P, TICK = 300, WAKE = 200, LATCH, CALLBACK = 2240, MARGIN = 300, WORK = 2500, SLOW, SLOWER;
static void timing(int hz) {
    P = 1000000 / hz; LATCH = P * 6516 / 10000; SLOW = LATCH + (P - LATCH) / 2; SLOWER = P + 1500;
}

/* ---- scenarios ---- */
typedef struct {
    const char *name;
    int64_t (*work)(int k);
    int64_t (*cbLate)(int k), (*relLate)(int k);
    int (*content)(int k), (*extra)(int k), (*sfSkip)(int k);
    int resizeAt, teardownAt, resumeAt;
    int glAt, glBackAt;     /* the GL path from this tick (the layer kept, only left), back from that; 0 never */
} Scenario;
static int64_t workPlain(int k) { (void) k; return WORK; }
static int64_t workEvery10(int k) { return k % 10 == 5 ? SLOW : WORK; }
static int64_t workFive(int k) { return k >= 100 && k < 105 ? SLOW : WORK; }
static int64_t workDeep(int k) { return k == 90 ? SLOW : WORK; }
static int64_t workStop(int k) { return k == 119 ? SLOW : WORK; }
static int64_t lateNone(int k) { (void) k; return 0; }
static int64_t lateFrame(int k) { return k >= 100 && k < 160 ? P : 0; }
static int64_t lateSpread(int k) { return (k * 7919) % 2001; }
static int64_t lateAround(int k) { return k >= 95 && k < 105 ? P : 0; }
static int64_t lateTwo(int k) { return k >= 100 && k < 160 ? 2 * P : 0; }
static int on(int k) { (void) k; return 1; }
static int off(int k) { (void) k; return 0; }
static int contentStops(int k) { return !(k >= 120 && k < 180); }
static int64_t lateAt130(int k) { return k >= 128 && k < 136 ? P : 0; }
static int extraBurst(int k) { return k >= 100 && k < 130; }
static int skipEvery20(int k) { return k % 20 == 7; }
static const Scenario scenarios[] = {
    { "steady", workPlain, lateNone, lateNone, on, off, off, -1, -1, -1 },
    { "a slow frame every 10 ticks", workEvery10, lateNone, lateNone, on, off, off, -1, -1, -1 },
    { "five slow frames in a row", workFive, lateNone, lateNone, on, off, off, -1, -1, -1 },
    { "release fences a frame late", workDeep, lateNone, lateFrame, on, off, off, -1, -1, -1 },
    { "OnComplete a frame late", workDeep, lateFrame, lateNone, on, off, off, -1, -1, -1 },
    { "slow every 10, release fences 0-2 ms late", workEvery10, lateNone, lateSpread, on, off, off, -1, -1, -1 },
    { "client faster than the display", workPlain, lateNone, lateNone, on, extraBurst, off, -1, -1, -1 },
    { "compositor skips a latch every 20", workPlain, lateNone, lateNone, on, off, skipEvery20, -1, -1, -1 },
    { "client stops, then resumes", workStop, lateNone, lateNone, contentStops, off, off, -1, -1, -1 },
    { "pool replaced, completions a frame late", workDeep, lateAround, lateNone, on, off, off, 100, -1, -1 },
    { "surface torn down and made again, old completions late", workDeep, lateAround, lateNone, on, off, off, -1, 100, 104 },
    /* beyond what four renderer slots hold: judged for safety only, and what it costs is reported */
    { "OnComplete two frames late (beyond the design)", workDeep, lateTwo, lateNone, on, off, off, -1, -1, -1 },
    /* leaving the root layer for GL and coming back: with nothing new, the claim is the slot on screen */
    { "to GL and back, nothing new, back after the release", workPlain, lateNone, lateNone, contentStops, off, off,
      -1, -1, -1, 130, 140 },
    { "to GL and back, nothing new, back before the release", workPlain, lateNone, lateAt130, contentStops, off, off,
      -1, -1, -1, 130, 131 },
    { "to GL and back, content going on", workEvery10, lateNone, lateNone, on, off, off, -1, -1, -1, 130, 140 },
};
static int beyondDesign(const Scenario *s) { return strstr(s->name, "beyond the design") != NULL; }

/* ---- events ---- */
enum { EV_TICK, EV_EXTRA, EV_FRAME, EV_DRAINED, EV_LATCH, EV_COMPLETE, EV_SWITCH, EV_RELEASE };
typedef struct { int64_t t; long order; int kind, a, b; } Ev;
static Ev evq[65536];
static int nEv;
static long evOrder;
static void at(int64_t t, int kind, int a, int b) { evq[nEv++] = (Ev) { t, evOrder++, kind, a, b }; }
static int popEv(Ev *e) {
    if (!nEv) return 0;
    int m = 0;
    for (int i = 1; i < nEv; i++)
        if (evq[i].t < evq[m].t || (evq[i].t == evq[m].t && evq[i].order < evq[m].order)) m = i;
    *e = evq[m]; evq[m] = evq[--nEv];
    return 1;
}

/* ---- the compositor ---- */
#define POOLS 4
enum { SF_FREE, SF_QUEUED, SF_LATCHED, SF_SHOWN, SF_RELEASING, SF_HIDDEN };
static const char *sfName[] = { "free", "queued", "latched", "shown", "being released", "kept by a hidden layer" };
static int sfState[POOLS][LORIE_ROOT_SLOTS];
static AHardwareBuffer ahbs[POOLS][LORIE_ROOT_SLOTS];
static ASurfaceControl layers[8];
static struct { AHardwareBuffer *shown, *next; int visible, bp, nextPub, nextTx, lastTx; } Ls[8];
static ASurfaceTransaction txs[20000];
static int nTxs, sfQ[4096], nQ;
static int fenceRd[20000], fenceWr[20000], fenceDelivered[20000], fenceSignalled[20000], nFences;
static AHardwareBuffer *fenceBuf[20000];
static int pubAt[512], submittedPub[512], presentedSeqLast, inOrder, dropped, presented, sfSkips, extraPubs, overruns;
static int latency[8];
static int violations;

static void sfSet(AHardwareBuffer *b, int st) { if (b && b->slot >= 0) sfState[b->pool][b->slot] = st; }

static ASurfaceTransaction *txCreate(void) { memset(&txs[nTxs], 0, sizeof txs[0]); txs[nTxs].pub = -1; return &txs[nTxs++]; }
static void txDelete(ASurfaceTransaction *t) { (void) t; }
static int64_t nowT;
static void txApply(ASurfaceTransaction *t) {
    t->applied = nowT;
    sfQ[nQ++] = (int) (t - txs);
    if (t->bufSet) sfSet(t->buf, SF_QUEUED);
}
static void txSetVisibility(ASurfaceTransaction *t, ASurfaceControl *c, int8_t v) { t->layer = c; t->visSet = 1; t->vis = v; }
static void txSetZOrder(ASurfaceTransaction *t, ASurfaceControl *c, int32_t z) { (void) t; (void) c; (void) z; }
static void txSetBuffer(ASurfaceTransaction *t, ASurfaceControl *c, AHardwareBuffer *b, int f) { (void) f; t->layer = c; t->bufSet = 1; t->buf = b; }
static void txSetGeometry(ASurfaceTransaction *t, ASurfaceControl *c, const ARect *s, const ARect *d, int32_t r) { (void) t; (void) c; (void) s; (void) d; (void) r; }
static void txReparent(ASurfaceTransaction *t, ASurfaceControl *c, ASurfaceControl *p) { (void) t; (void) c; (void) p; }
static void txSetOnComplete(ASurfaceTransaction *t, void *ctx, void (*cb)(void *, ASurfaceTransactionStats *)) { t->ctx = ctx; t->cb = cb; }
static int bpTransactions;
static void txSetEnableBackPressure(ASurfaceTransaction *t, ASurfaceControl *c, bool on) { t->layer = c; t->bp = on; bpTransactions++; }
static void scRelease(ASurfaceControl *c) { (void) c; }
static int statsPrevReleaseFenceFd(ASurfaceTransactionStats *s, ASurfaceControl *c) {
    (void) c;
    if (s->prevFence < 0) return -1;
    fenceDelivered[s->prevFence] = 1;
    return dup(fenceRd[s->prevFence]);
}
static void statsGetControls(ASurfaceTransactionStats *s, ASurfaceControl ***out, size_t *n) { *out = malloc(sizeof **out); (*out)[0] = s->control; *n = 1; }
static void statsReleaseControls(ASurfaceControl **c) { free(c); }
static int newFence(AHardwareBuffer *b) {
    int p[2];
    if (pipe(p)) abort();
    fenceRd[nFences] = p[0]; fenceWr[nFences] = p[1]; fenceBuf[nFences] = b;
    fenceDelivered[nFences] = fenceSignalled[nFences] = 0;
    return nFences++;
}
static void closeFence(int f) {
    if (fenceRd[f] >= 0 && fenceDelivered[f] && fenceSignalled[f]) { close(fenceRd[f]); fenceRd[f] = -1; }
}

static const Scenario *sc;
static void latch(int64_t t, int k) {
    if (sc->sfSkip(k)) { sfSkips++; return; }
    int flushed[64], nFlush = 0, bufsIn[8] = { 0 };
    while (nQ && nFlush < 64) {
        ASurfaceTransaction *x = &txs[sfQ[0]];
        if (x->applied > t - MARGIN) break;
        int e = x->layer ? x->layer->epoch : 0;
        if (x->bufSet && Ls[e].bp && bufsIn[e]) break;      /* backpressure: the next one waits */
        if (x->bufSet) bufsIn[e]++;
        flushed[nFlush++] = sfQ[0];
        memmove(sfQ, sfQ + 1, --nQ * sizeof sfQ[0]);
    }
    int firstBuf[8]; for (int e = 0; e < 8; e++) firstBuf[e] = 1;
    for (int i = 0; i < nFlush; i++) {
        ASurfaceTransaction *x = &txs[flushed[i]];
        int e = x->layer ? x->layer->epoch : 0, fence = -1;
        if (x->bp) Ls[e].bp = 1;
        if (x->bufSet) {
            AHardwareBuffer *prev = Ls[e].next ? Ls[e].next : Ls[e].shown;
            if (Ls[e].next) {                           /* replaced before it was shown: dropped at once */
                sfSet(Ls[e].next, SF_FREE);
                if (Ls[e].next->slot >= 0) dropped++;
            } else if (firstBuf[e] && prev && Ls[e].visible) {
                fence = newFence(prev);                 /* released when the frame without it is shown */
                at((k + 1) * P + sc->relLate(k + 1) + 1, EV_RELEASE, fence, 0);
            }
            if (firstBuf[e] && prev && !Ls[e].visible && !Ls[e].next)
                sfSet(prev, SF_FREE);                   /* never shown again: released at once */
            firstBuf[e] = 0;
            Ls[e].next = x->buf;
            Ls[e].nextPub = x->pub;
            Ls[e].nextTx = flushed[i];
            sfSet(x->buf, SF_LATCHED);
        }
        if (x->visSet) {
            if (!x->vis && Ls[e].visible && !x->bufSet && Ls[e].shown)
                sfSet(Ls[e].shown, SF_HIDDEN);           /* a hide keeps the buffer */
            Ls[e].visible = x->vis;
        }
        x->done = 1;
        if (x->cb) {
            int64_t ct = t + CALLBACK + sc->cbLate(k);
            at(ct, EV_COMPLETE, flushed[i], fence);
        }
    }
    for (int e = 0; e < 8; e++)
        if (Ls[e].next) at((k + 1) * P, EV_SWITCH, e, 0);
}
/* the vsync: what was latched goes on screen; what it replaced is read until its fence */
static void sswitch(int e) {
    AHardwareBuffer *b = Ls[e].next, *old = Ls[e].shown;
    if (!b) return;
    if (old && old->slot >= 0 && (sfState[old->pool][old->slot] == SF_SHOWN || sfState[old->pool][old->slot] == SF_HIDDEN))
        sfSet(old, SF_RELEASING);
    Ls[e].shown = b; Ls[e].next = NULL;
    if (b->slot >= 0) {
        presented++;
        int lat = (int) (nowT / P) - Ls[e].nextPub;
        latency[lat < 0 ? 0 : lat > 7 ? 7 : lat]++;
        if (Ls[e].nextTx < Ls[e].lastTx) inOrder = 0;
        Ls[e].lastTx = Ls[e].nextTx;
    }
    sfSet(b, Ls[e].visible ? SF_SHOWN : SF_HIDDEN);
}
static void release(int f) {
    char c = 1;
    if (write(fenceWr[f], &c, 1) != 1) abort();
    close(fenceWr[f]); fenceWr[f] = -1;
    fenceSignalled[f] = 1;
    AHardwareBuffer *b = fenceBuf[f];
    if (b && b->slot >= 0 && sfState[b->pool][b->slot] == SF_RELEASING)
        sfState[b->pool][b->slot] = SF_FREE;
    closeFence(f);
}

/* ---- X server and renderer ---- */
static LoriePixmapPriv priv;
static int pool, epoch, gen, zc, pending, gateOpen, drawRequested, retry, busy, frameSlot, frameTick;
static uint64_t frameId;
static uint32_t frameGen;
static int pubTick[LORIE_ROOT_SLOTS], publishStalls, frameStalls, xAvailMin, heldMax, stopped[512];

static int heldCount(void) { return __builtin_popcount(fakeState.rootHandover & LORIE_ROOT_HELD_MASK); }
static void check(const char *when) {
    if (rendererRootSlot >= 0 && LORIE_ROOT_GEN(fakeState.rootHandover) == rendererRootSlotGen &&
        !(fakeState.rootHandover & (1u << rendererRootSlot)) && violations++ < 3)
        printf("    %s: slot %d given back while the frame that claimed it still uses it\n", when, rendererRootSlot);
    for (int s = 0; s < LORIE_ROOT_SLOTS; s++)
        if (!(fakeState.rootHandover & (1u << s)) && sfState[pool][s] != SF_FREE) {
            if (violations++ < 3) printf("    %s: slot %d not held while the compositor %s it\n", when, s, sfName[sfState[pool][s]]);
        }
    if (sfState[pool][priv.rootWrite] != SF_FREE || (fakeState.rootHandover & (1u << priv.rootWrite)))
        if (violations++ < 3) printf("    %s: the X server draws into slot %d (%s)\n", when, priv.rootWrite, sfName[sfState[pool][priv.rootWrite]]);
    int h = heldCount() - (rendererRootSlot >= 0 ? 1 : 0);
    if (h > heldMax) heldMax = h;
    if (h > LORIE_ZC_MAX_HELD && violations++ < 3) printf("    %s: the renderer holds %d\n", when, h);
}
static void publish(int k) {
    int avail = LORIE_ROOT_SLOTS - heldCount();
    if (avail < xAvailMin) xAvailMin = avail;
    int drawn = priv.rootWrite;
    if (lorieRootHandover(&priv)) {
        pubTick[drawn] = k; pubAt[k] = 1; pending = 0; drawRequested = 1;
    } else
        publishStalls++;
}
static void wake(int64_t t) {
    if (busy || !gateOpen || !(drawRequested || retry)) return;
    busy = 1;
    at(t + WAKE, EV_FRAME, 0, 0);
}
static void frame(int64_t t) {
    drawRequested = 0;
    frameId = rendererClaimRootBuffer();
    frameSlot = rendererRootSlot; frameGen = rendererRootSlotGen;
    if (frameSlot < 0) { busy = 0; return; }
    if (!zc) {                                          /* the GL path: leaves the layer, samples, gives back */
        if (rootZcDisplayedSlot >= 0 || rootZcRetiringCount > 0)
            rootZcStopPresenting();
        check("GL frame, sampling");
        rendererReleaseRootBuffer();
        busy = 0; gateOpen = 0;
        return;
    }
    int drainedRoom = rootZcDrainRetiring();
    check("frame, after the drain");
    if (!drainedRoom) {
        frameStalls++;
        int mine = frameSlot == rootZcDisplayedSlot && frameId == rootZcDisplayedId;
        for (int i = 0; i < rootZcRetiringCount; i++)
            mine |= rootZcRetiring[i].slot == frameSlot && rootZcRetiring[i].bufferId == frameId;
        if (!mine) rendererReleaseRootBuffer(); else rendererRootSlot = -1;
        retry = 1; gateOpen = 0; busy = 0;
        return;
    }
    if (frameSlot == rootZcDisplayedSlot && frameId == rootZcDisplayedId) {
        rendererRootSlot = -1; retry = 0; busy = 0;
        if (rootZcPublishedSinceClaim()) drawRequested = 1;
        wake(t);
        return;
    }
    /* the copies go into the claimed slot; one already submitted complete before has none queued (the frame
     * that submitted it waited for them), which is what a frame back from the GL path finds */
    int submittedBefore = 0;
    for (int i = 0; i < rootZcRetiringCount; i++)
        submittedBefore |= rootZcRetiring[i].slot == frameSlot && rootZcRetiring[i].bufferId == frameId;
    if (!submittedBefore && sfState[pool][frameSlot] != SF_FREE && violations++ < 3)
        printf("    the frame copies into slot %d, which the compositor %s\n", frameSlot, sfName[sfState[pool][frameSlot]]);
    frameTick = pubTick[frameSlot];
    int64_t w = sc->work(frameTick);
    if (w > P - WAKE) overruns++;
    at(t + w, EV_DRAINED, 0, 0);
}
static void drained(int64_t t) {
    gateOpen = 0;
    if (!zc || LORIE_ROOT_GEN(fakeState.rootHandover) != frameGen) {
        rendererReleaseRootBuffer();                    /* off the layer, or the pool went: nothing submitted */
        busy = 0; retry = 1;
        return;
    }
    uint32_t seq = rootZcHandOver(frameSlot, frameId, frameGen);
    ASurfaceTransaction *x = scApi.txCreate();
    scApi.txSetVisibility(x, rootSurfaceControl, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    scApi.txSetBuffer(x, rootSurfaceControl, &ahbs[pool][frameSlot], -1);
    scApi.txSetOnComplete(x, (void *) (uintptr_t) seq, rootZcOnComplete);
    x->pub = frameTick;
    nowT = t;
    scApi.txApply(x);
    submittedPub[frameTick] = 1;
    rendererRootSlot = -1;
    retry = rootZcPublishedSinceClaim();
    busy = 0;
    wake(t);
}

static void replacePool(void) {
    pool++;
    init(&priv);                                        /* new buffers, slot 0 published, drawing into 1 */
    gen += 2;
    fakeState.rootHandover = (uint32_t) gen << LORIE_ROOT_GEN_SHIFT;
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = 1000u * (unsigned) pool + i;
    fakeState.rootDoubleBuffered = 1;
}

static int runScenario(const Scenario *s, int hz, int bpSymbol, int strict) {
    sc = s; timing(hz);
    memset(sfState, 0, sizeof sfState); memset(Ls, 0, sizeof Ls); memset(pubAt, 0, sizeof pubAt);
    memset(submittedPub, 0, sizeof submittedPub); memset(stopped, 0, sizeof stopped); memset(latency, 0, sizeof latency);
    nTxs = nQ = nEv = 0; evOrder = 0; dropped = presented = sfSkips = extraPubs = overruns = 0; inOrder = 1; presentedSeqLast = 0;
    publishStalls = frameStalls = heldMax = 0; xAvailMin = LORIE_ROOT_SLOTS; violations = 0; bpTransactions = 0;
    for (int f = 0; f < nFences; f++) { if (fenceRd[f] >= 0) close(fenceRd[f]); if (fenceWr[f] >= 0) close(fenceWr[f]); }
    nFences = 0;
    memset(&fakeState, 0, sizeof fakeState);
    pool = 0; gen = 0; epoch = 0;
    init(&priv);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) for (int p = 0; p < POOLS; p++) ahbs[p][i] = (AHardwareBuffer) { p, i };
    fakeState.rootDoubleBuffered = 1;
    rendererRootSlot = -1; rootZcDisplayedSlot = -1; rootZcRetiringCount = 0; rootZcUnusableCount = 0; rootZcRetireSeq = 0;
    scApi.txSetEnableBackPressure = bpSymbol ? txSetEnableBackPressure : NULL;
    for (int e = 0; e < 8; e++) { layers[e].epoch = e; Ls[e].lastTx = -1; }
    nowT = 0;                                           /* before anything is applied */
    rootSurfaceControl = &layers[0];
    rootZcSetBackpressure(rootSurfaceControl);
    zc = 1; pending = gateOpen = drawRequested = retry = busy = 0;
    int end = 270;
    for (int k = 0; k < end + 10; k++) {
        if (k < end) at(k * P + TICK, EV_TICK, k, 0);
        if (k < end && s->extra(k)) at(k * P + P / 2, EV_EXTRA, k, 0);
        at(k * P + LATCH, EV_LATCH, k, 0);
    }
    Ev e;
    while (popEv(&e)) {
        nowT = e.t;
        switch (e.kind) {
        case EV_TICK: {
            int k = e.a;
            gateOpen = 1;
            if (k == s->resizeAt) replacePool();
            if (k == s->teardownAt) { teardownRootOverlay(); zc = 0; }
            if (k == s->resumeAt) { rootSurfaceControl = &layers[++epoch]; rootZcSetBackpressure(rootSurfaceControl); zc = 1; }
            if (s->glAt && k == s->glAt) { zc = 0; drawRequested = 1; }      /* e.g. the filtering changed: redrawn */
            if (s->glBackAt && k == s->glBackAt) { zc = 1; drawRequested = 1; }
            if (!zc) stopped[k] = 1;
            if (k >= 240 || s->content(k) || k == s->resizeAt) pending = 1;
            if (pending) publish(k);
            wake(e.t);
            break;
        }
        case EV_EXTRA: extraPubs++; pending = 1; publish(e.a); gateOpen = 1; wake(e.t); break;
        case EV_FRAME: frame(e.t); break;
        case EV_DRAINED: drained(e.t); break;
        case EV_LATCH: latch(e.t, e.a); break;
        case EV_COMPLETE: {
            ASurfaceTransaction *x = &txs[e.a];
            ASurfaceTransactionStats st = { e.b, x->layer };
            x->cb(x->ctx, &st);
            if (e.b >= 0) closeFence(e.b);
            break;
        }
        case EV_SWITCH: sswitch(e.a); break;
        case EV_RELEASE: release(e.a); break;
        }
        check(e.kind == EV_TICK ? "tick" : e.kind == EV_FRAME ? "frame" : e.kind == EV_COMPLETE ? "completion" : "compositor");
    }
    /* nothing held that is not the slot submitted last or one still retiring */
    uint32_t want = 0;
    if (rootZcDisplayedSlot >= 0 && rootZcDisplayedGen == (uint32_t) gen) want |= 1u << rootZcDisplayedSlot;
    for (int i = 0; i < rootZcRetiringCount; i++)
        if (rootZcRetiring[i].gen == (uint32_t) gen) want |= 1u << rootZcRetiring[i].slot;
    int leak = (fakeState.rootHandover & LORIE_ROOT_HELD_MASK) != want;
    int lost = 0, unavoidable = sfSkips + extraPubs + overruns;
    for (int k = 0; k < end; k++)
        if (pubAt[k] && !submittedPub[k] && !stopped[k] && !(k > 0 && stopped[k - 1])) lost++;
    int bad = violations || leak || xAvailMin < 2 || heldMax > LORIE_ZC_MAX_HELD
              || (strict && (dropped || lost > unavoidable || bpTransactions != 1 + (s->resumeAt >= 0)));
    int p50 = 0, acc = 0, maxLat = 0;
    for (int l = 0; l < 8; l++) if (latency[l]) maxLat = l;
    for (int l = 0; l < 8 && acc * 2 < presented; l++) { acc += latency[l]; p50 = l; }
    bad |= !inOrder;
    printf("  %3d Hz %-56s %s presented %d dropped %d lost %d (unavoidable %d) publish-stalls %d frame-stalls %d "
           "held max %d X-min %d latency %d/%d%s%s%s\n", hz, s->name, bad ? "FAIL" : "PASS", presented, dropped, lost,
           unavoidable, publishStalls, frameStalls, heldMax, xAvailMin, p50, maxLat, leak ? " LEAK" : "",
           violations ? " VIOLATIONS" : "", inOrder ? "" : " OUT-OF-ORDER");
    return bad;
}

int main(int argc, char **argv) {
    int only = argc > 1 ? atoi(argv[1]) : -1;
    (void) only;
    scApi.txCreate = txCreate; scApi.txDelete = txDelete; scApi.txApply = txApply;
    scApi.txSetVisibility = txSetVisibility; scApi.txSetZOrder = txSetZOrder; scApi.txSetBuffer = txSetBuffer;
    scApi.txSetGeometry = txSetGeometry; scApi.txReparent = txReparent; scApi.txSetOnComplete = txSetOnComplete;
    scApi.release = scRelease; scApi.statsPrevReleaseFenceFd = statsPrevReleaseFenceFd;
    scApi.statsGetControls = statsGetControls; scApi.statsReleaseControls = statsReleaseControls;
    for (int f = 0; f < 20000; f++) fenceRd[f] = fenceWr[f] = -1;
    int failed = 0;
    scApi.createFromWindow = createFromWindow;
    scApi.statsPrevReleaseFenceFd = statsPrevReleaseFenceFd;

    /* ensureRootOverlay: a new layer gets backpressure once, in a transaction of its own; asking again for a
     * layer that is there sends nothing; a layer made again after a teardown gets it again; the renderer has
     * exactly one place that asks for it - not its buffer transactions */
    {
        static ANativeWindow *someWindow = (ANativeWindow *) &layers_ensure[0];
        win = someWindow; defaultWin = NULL; rootSurfaceControl = NULL; nTxs = 0; nQ = 0; bpTransactions = 0;
        scApi.txSetEnableBackPressure = txSetEnableBackPressure;
        ensureRootOverlay(); ensureRootOverlay();
#ifndef BP_NOT_REQUESTED
        CHECK(bpTransactions == 1 && nTxs == 1 && txs[0].bp && !txs[0].bufSet && !txs[0].cb,
              "ensureRootOverlay: %d backpressure calls in %d transactions", bpTransactions, nTxs);
#else
        CHECK(bpTransactions == 0 && nTxs == 0 && !rootZcBackpressureOn,
              "comparison build, the call there: %d backpressure calls in %d transactions", bpTransactions, nTxs);
#endif
        rootZcDisplayedSlot = -1; rootZcRetiringCount = 0;
        teardownRootOverlay();
        ensureRootOverlay();
#ifndef BP_NOT_REQUESTED
        CHECK(bpTransactions == 2 && rootZcBackpressureOn, "a layer made again: %d backpressure calls", bpTransactions);
#else
        CHECK(bpTransactions == 0 && !rootZcBackpressureOn, "comparison build, a layer made again: %d backpressure calls",
              bpTransactions);
#endif
        teardownRootOverlay();
        scApi.txSetEnableBackPressure = NULL;   /* below API 31 */
        nTxs = 0; bpTransactions = 0;
        ensureRootOverlay();
        CHECK(bpTransactions == 0 && nTxs == 0 && !rootZcBackpressureOn, "without the call: %d transactions sent", nTxs);
        CHECK(BACKPRESSURE_CALL_SITES == 1, "backpressure asked for in %d places", BACKPRESSURE_CALL_SITES);
        teardownRootOverlay();
        failed += fails;
        fails = 0;
    }
#if !defined(NO_BP_SYMBOL) && !defined(BP_NOT_REQUESTED)
    for (int hz = getenv("HZ") ? atoi(getenv("HZ")) : 60; hz <= (getenv("HZ") ? atoi(getenv("HZ")) : 120); hz += 60)
        for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++)
            if (only < 0 || (int) i == only)
                failed += runScenario(&scenarios[i], hz, 1, !beyondDesign(&scenarios[i]));
    printf("TZCBP root slots against compositor backpressure: %s (%d scenarios failing)\n", failed ? "FAIL" : "PASS", failed);
#else
    /* no setEnableBackPressure, or a build that does not ask for it: nothing asked of the compositor, which
     * may then drop a buffer for a newer one. Drops are not failures here; the slot lifecycle has to stay
     * just as safe (no slot given back that the compositor or a frame still has, the X server two slots,
     * the renderer at most LORIE_ZC_MAX_HELD, nothing leaked), and the drops have to show */
    int drops = 0, symbol = 0, hzTop = 60;
#ifndef NO_BP_SYMBOL
    symbol = 1; hzTop = 120;                            /* the call is there; this build does not make it */
#endif
    for (int hz = 60; hz <= hzTop; hz += 60)
        for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
            failed += runScenario(&scenarios[i], hz, symbol, 0);
            drops += dropped;
            CHECK(bpTransactions == 0 && !rootZcBackpressureOn, "%s: backpressure asked for (%d calls)", scenarios[i].name,
                  bpTransactions);
        }
    CHECK(drops > 0, "without backpressure no buffer was ever dropped: the check for it is blind");
    printf("TZCBP %s, the slot lifecycle as safe and the drops seen: %s (%d scenarios failing, %d drops)\n",
           symbol ? "comparison build, backpressure not requested with the call there" : "without the backpressure call",
           failed || fails ? "FAIL" : "PASS", failed, drops);
    failed += fails;
#endif
    return failed != 0;
}
