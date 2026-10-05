/* Making the root rotate through its slots (lorieEnsureRootDoubleBuffer, extracted by gen.py with the
 * handover, carry and repair code in rootharness.h). It used to copy the root's buffer into a new slot 0
 * and slot 0 into the other four - five full-screen CPU copies, at start-up and after every resize.
 *
 * The root's own buffer is now slot 0 as it is; slot 1, drawn into next, is brought up to date from it
 * on the GPU and owed until that copy is made; slots 2-4 lack everything until a handover carries it to
 * them, on the GPU. Checked: no copy at all where the renderer is there and gets to it first; the CPU
 * fetching the area only when the X server touches slot 1 before that; one copy into slot 1, not five,
 * with no renderer; a root that cannot be a slot, and slots that cannot be allocated, leaving the root
 * as it was; and every slot published afterwards matching what was drawn. */
#include "rootharness.h"

static int nextBuf, failAt = -1;
static LorieBuffer *LorieBuffer_allocate(int32_t w, int32_t h, int format, int type) {
    if (nextBuf == failAt || nextBuf >= LORIE_ROOT_SLOTS)
        return NULL;
    LorieBuffer *b = &bufs[nextBuf];
    b->desc = (LorieBuffer_Desc) { w, w, h, 100 + (uint64_t) nextBuf, format, type };
    b->mem = pixels[nextBuf++];
    b->refs = 1;
    b->locked = b->registered = 0;
    return b;
}
static LorieBuffer *LorieBuffer_allocateForComposer(int32_t w, int32_t h, int format, bool *granted) {
    *granted = true;
    return LorieBuffer_allocate(w, h, format, LORIEBUFFER_AHARDWAREBUFFER);
}
static int LorieBuffer_lock(LorieBuffer *b, void **out) { *out = b->mem; b->locked = 1; return 0; }
static int LorieBuffer_unlock(LorieBuffer *b) { b->locked = 0; return 0; }
static void LorieBuffer_release(LorieBuffer *b) { b->refs--; }
static void lorieRegisterBuffer(LorieBuffer *b) { b->registered = 1; }
static void lorieUnregisterBuffer(LorieBuffer *b) { b->registered = 0; }
#define log(...) do {} while (0)
#include "rootseed_src.inc"

/* the renderer's queue: carry copies only here */
typedef struct { uint64_t serial; int from, to, n, cancelled, claimed; BoxRec boxes[LORIE_GPU_COPY_MAX_RECTS]; } Job;
static Job jobs[256];
static int jobHead, jobTail;
static uint64_t serial;
static uint64_t queueCarry(int from, int to, BoxPtr box, int n) {
    Job *j = &jobs[jobTail++];
    memset(j, 0, sizeof *j);
    j->serial = ++serial; j->from = from; j->to = to; j->n = n;
    memcpy(j->boxes, box, (size_t) n * sizeof *box);
    return j->serial;
}
static void renderAll(void) {
    while (jobHead < jobTail) {
        Job *j = &jobs[jobHead++];
        if (j->cancelled) { gpuFails(j->serial); continue; }
        for (int k = 0; k < j->n; k++)
            for (int y = j->boxes[k].y1; y < j->boxes[k].y2; y++)
                for (int x = j->boxes[k].x1; x < j->boxes[k].x2; x++)
                    pixels[j->to][y * W + x] = pixels[j->from][y * W + x];
        fakeCompleted = j->serial;
    }
}
static int cancelJob(uint64_t s) {
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].serial == s)
            return (jobs[j].cancelled || jobs[j].claimed) ? 0 : (jobs[j].cancelled = 1);
    return 0;
}

static uint32_t ref[W * H];
static LoriePixmapPriv priv;
static const BoxRec ALL = { 0, 0, W, H }, Q = { 40, 2, 56, 10 }, P = { 2, 2, 8, 6 };
static void refFill(BoxRec b, uint32_t v) { for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) ref[y * W + x] = v; }
static int matches(int slot) { return !memcmp(pixels[slot], ref, sizeof ref); }

/* a single-buffered root, its buffer allocated as lorieCreatePixmap allocates it, holding a picture */
static void singleRoot(int format, int carryOn) {
    memset(&priv, 0, sizeof priv);
    memset(&fakeState, 0, sizeof fakeState);
    memset(bufs, 0, sizeof bufs);
    memset(fakeFailed, 0, sizeof fakeFailed);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
        for (int k = 0; k < W * H; k++)
            pixels[i][k] = 0xdead;                    /* what a new buffer holds: anything */
    nextBuf = 0; failAt = -1; jobHead = jobTail = 0; serial = 0; fakeCompleted = 0;
    priv.buffer = LorieBuffer_allocate(W, H, format, LORIEBUFFER_AHARDWAREBUFFER);
    LorieBuffer_lock(priv.buffer, &priv.locked);
    refFill(ALL, 7); refFill(Q, 9);
    memcpy(pixels[0], ref, sizeof ref);
    fakeRootPriv = &priv;
    lorieSingleRootBuffer = FALSE;
    harnessCarryOn = carryOn; harnessQueueCarry = queueCarry; harnessCancel = cancelJob; harnessLockWait = NULL;
    published = 0; retiring = -1;
}
/* the X server draws, and the renderer runs the carries behind every handover; each slot published must
 * hold the picture */
static int publishAll(const char *what) {
    int bad = 0;
    for (int k = 0; k < 2 * LORIE_ROOT_SLOTS; k++) {
        int drawn = priv.rootWrite;
        BoxRec b = { (short) (4 * k), 12, (short) (4 * k + 3), 15 };
        xDraw(&priv, b, 20 + k); refFill(b, 20 + k);
        if (!handover(&priv)) { CHECK(0, "%s: handover %d not published", what, k); return 1; }
        renderAll();
        if (!matches(drawn)) bad++;
    }
    CHECK(!bad, "%s: %d published slots differ from what was drawn", what, bad);
    return bad;
}

int main(void) {
    /* the renderer there, and running the carry before the X server draws again */
    singleRoot(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 1);
    lorieEnsureRootDoubleBuffer((PixmapPtr) &priv);
    CHECK(priv.rootDouble && priv.rootWrite == 1 && priv.buffer == priv.rootBuf[1] && fakeState.rootDoubleBuffered,
          "not double buffered");
    CHECK(priv.rootBuf[0] == &bufs[0], "slot 0 is not the root's own buffer - a new one, to be copied into");
    CHECK(fakeState.presentStats.cpuSeedBytes == 0, "the CPU copied %llu bytes to fill the slots",
          (unsigned long long) fakeState.presentStats.cpuSeedBytes);
    CHECK(jobTail == 1 && jobs[0].from == 0 && jobs[0].to == 1 && jobs[0].n == 1 && jobs[0].boxes[0].x2 == W &&
          jobs[0].boxes[0].y2 == H, "slot 1 not given to the GPU from slot 0, the whole of it");
    CHECK(fakeState.presentStats.gpuCarryBytes == W * H * 4, "%llu bytes given to the GPU",
          (unsigned long long) fakeState.presentStats.gpuCarryBytes);
    for (int i = 2; i < LORIE_ROOT_SLOTS; i++)
        CHECK(pixman_region_contains_rectangle(&priv.rootStale[i], (BoxRec *) &ALL) == PIXMAN_REGION_IN,
              "slot %d not marked as lacking everything", i);
    CHECK(!pixman_region_not_empty(&priv.rootStale[0]) && !pixman_region_not_empty(&priv.rootStale[1]),
          "slot 0 or 1 marked stale");
    CHECK(priv.rootOwedDonor == 0 && pixman_region_contains_rectangle(&priv.rootOwed, (BoxRec *) &ALL) == PIXMAN_REGION_IN,
          "slot 1 not owed everything from slot 0 while its copy is in flight");
    CHECK(((fakeState.rootHandover >> LORIE_ROOT_NEWEST_SHIFT) & LORIE_ROOT_NEWEST_MASK) == 0 &&
          !(fakeState.rootHandover & LORIE_ROOT_HELD_MASK), "slot 0 not the one published, or a slot held");
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
        CHECK(bufs[i].registered && fakeState.rootBufferIds[i] == 100 + (uint64_t) i, "slot %d not registered", i);
    CHECK(bufs[0].refs == 1, "the root's buffer released (%d) - it is slot 0 now", bufs[0].refs);
    CHECK(fakeHeaderPitch == W * 4, "pixmap header not moved to slot 1's pitch");
    renderAll();
    CHECK(matches(1), "slot 1 does not have the picture once its copy has run");
    xDraw(&priv, P, 6); refFill(P, 6);
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == 0, "the CPU fetched %llu bytes the GPU had already copied",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);
    publishAll("GPU");
    CHECK(fakeState.presentStats.cpuSeedBytes + fakeState.presentStats.cpuCarryBytes +
          fakeState.presentStats.cpuOwedFetchBytes == 0, "the CPU copied %llu bytes with the renderer keeping up",
          (unsigned long long) (fakeState.presentStats.cpuSeedBytes + fakeState.presentStats.cpuCarryBytes +
                                fakeState.presentStats.cpuOwedFetchBytes));

    /* the X server draws into slot 1 before the renderer has got to it: taken back, and the whole of it
     * fetched by the CPU - PrepareAccess does not know what the drawing will cover */
    singleRoot(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 1);
    lorieEnsureRootDoubleBuffer((PixmapPtr) &priv);
    xDraw(&priv, P, 6); refFill(P, 6);
    CHECK(matches(1), "taken back: slot 1 lacks the picture where it was drawn into");
    CHECK(fakeState.presentStats.gpuCarryTakenBack == 1 && fakeState.presentStats.cpuOwedFetchBytes == W * H * 4,
          "taken back: %u taken back, %llu bytes fetched", fakeState.presentStats.gpuCarryTakenBack,
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);
    renderAll();
    CHECK(matches(1), "taken back: the cancelled copy landed after the drawing");
    publishAll("taken back");

    /* no renderer: nothing copied there and then; slot 1 owes it, and the CPU fetches it once, when slot 1
     * is first touched - not one copy per slot */
    singleRoot(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 0);
    lorieEnsureRootDoubleBuffer((PixmapPtr) &priv);
    CHECK(priv.rootDouble && fakeState.presentStats.cpuSeedBytes == 0 && jobTail == 0,
          "no renderer: %llu bytes copied into the slots, %d copies queued",
          (unsigned long long) fakeState.presentStats.cpuSeedBytes, jobTail);
    xDraw(&priv, P, 6); refFill(P, 6);
    CHECK(matches(1) && fakeState.presentStats.cpuOwedFetchBytes == W * H * 4,
          "no renderer: slot 1 lacks the picture, or was fetched other than once (%llu bytes)",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);
    publishAll("no renderer");

    /* a root buffer that cannot be a slot: left single buffered, nothing copied */
    singleRoot(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 1);
    lorieEnsureRootDoubleBuffer((PixmapPtr) &priv);
    CHECK(!priv.rootDouble && priv.buffer == &bufs[0] && !priv.rootBuf[0] && nextBuf == 1 &&
          fakeState.presentStats.cpuSeedBytes == 0 && lorieSingleRootBuffer, "an RGBX root: double buffered "
          "anyway, or a slot allocated, or something copied");
    lorieSingleRootBuffer = FALSE;

    /* slot 3 cannot be allocated: the others go again, the root is as it was */
    singleRoot(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 1);
    failAt = 3;
    lorieEnsureRootDoubleBuffer((PixmapPtr) &priv);
    CHECK(!priv.rootDouble && priv.buffer == &bufs[0] && priv.locked == pixels[0] && bufs[0].refs == 1,
          "allocation failure: the root's own buffer touched");
    CHECK(bufs[1].refs == 0 && bufs[2].refs == 0 && !priv.rootBuf[0] && !priv.rootBuf[1] && !priv.rootBuf[2] &&
          jobTail == 0, "allocation failure: the slots allocated before it kept, or a copy queued");

    printf("root slots from the root's own buffer: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
