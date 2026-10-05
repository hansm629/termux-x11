/* T34: the copies X core rendering makes - a window moved within the root, a client's pixmap copied into
 * it, the root copied out to a pixmap - made by the GPU (lorieCoreCopyOnGpu) instead of the CPU. Against
 * the real handover, carry, repair and staging code (rootharness.h, extracted by gen.py), with the renderer
 * and its queue simulated, including running out of time with a copy half done.
 *
 * What has to hold:
 *   - a move within the root, which one GPU copy cannot both read and write, staged through a slot nothing
 *     needs, comes out exactly as the CPU's overlapping copy would, in every direction;
 *   - the X server never goes on with the copy unmade: taken back, the CPU makes it - and the second step
 *     of a staged move, once running, is waited out rather than made twice over a half-written slot;
 *   - what it reads in the root is up to date first, or it stays with the CPU;
 *   - every slot published afterwards matches what was drawn and copied, and with a healthy renderer the
 *     CPU copies nothing the GPU could.
 */
#include "rootharness.h"

static int verbose;
#define V(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

/* bufs[0..4] are the root's slots; then a client's AHB pixmap, one in plain memory, and an AHB pixmap the
 * root is copied out into */
#define CLIENT 5
#define REGULAR 6
#define OUT 7
static uint32_t clientPixels[W * H], regularPixels[W * H], outPixels[W * H];
static uint32_t *mem(int b) {
    return b < LORIE_ROOT_SLOTS ? pixels[b] : b == CLIENT ? clientPixels : b == REGULAR ? regularPixels : outPixels;
}

typedef struct {
    uint64_t serial;
    int src, dst, n, xOff, yOff, carry, cancelled, claimed, gaveUp;
    BoxRec boxes[LORIE_GPU_COPY_MAX_RECTS];
    int present; BoxRec pb; uint32_t v;              /* a present: fills pb with v in dst */
} Job;
#define MAXJ 16384
static Job jobs[MAXJ];
static int jobHead, jobTail;
static uint64_t serial;
static int healthy, failCarries;
static uint32_t rng, seedNow;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static int meets(BoxRec a, BoxRec b) { return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2; }

static void stagedThrough(int slot);
static uint64_t queueJob(int src, int dst, BoxPtr box, int n, int xOff, int yOff, int carry) {
    if (!carry && dst < LORIE_ROOT_SLOTS)
        stagedThrough(dst);
    Job *j = &jobs[jobTail++];
    memset(j, 0, sizeof *j);
    j->serial = ++serial; j->src = src; j->dst = dst; j->n = n; j->xOff = xOff; j->yOff = yOff; j->carry = carry;
    memcpy(j->boxes, box, (size_t) n * sizeof *box);
    fakeState.gpuCopyQueue.writeIndex = (uint32_t) jobTail;
    V("  queued %llu %s %d -> %d, %d rects, at %+d,%+d\n", (unsigned long long) j->serial, carry ? "carry" : "copy",
      src, dst, n, xOff, yOff);
    return j->serial;
}
static uint64_t queueCarry(int from, int to, BoxPtr box, int n) { return queueJob(from, to, box, n, 0, 0, 1); }
static uint64_t lorieQueueBufferCopy(LorieBuffer *src, LorieBuffer *dst, BoxPtr box, int n, int xOff, int yOff,
                                     int traceKind) {
    (void) traceKind;
    return queueJob((int) (src - bufs), (int) (dst - bufs), box, n, xOff, yOff, 0);
}
/* lateReport: the renderer reports what it ran only a while after letting go of the lock (1), or not
 * within the X server's hard wait at all (2) */
static int lateReport;
static uint64_t unreported;
static void renderOne(void) {
    Job *j = &jobs[jobHead++];
    fakeState.gpuCopyQueue.readIndex = (uint32_t) jobHead;
    int give = j->carry && !j->claimed && failCarries && !rnd(failCarries);
    if (j->cancelled || give) {
        j->gaveUp = !j->cancelled;
        gpuFails(j->serial);
        return;
    }
    if (j->present)
        fill(j->dst, j->pb, j->v);
    else
        for (int k = 0; k < j->n; k++)
            for (int y = j->boxes[k].y1; y < j->boxes[k].y2; y++)
                for (int x = j->boxes[k].x1; x < j->boxes[k].x2; x++)
                    mem(j->dst)[(y + j->yOff) * W + x + j->xOff] = mem(j->src)[y * W + x];
    if (lateReport)
        unreported = j->serial;
    else
        fakeCompleted = j->serial;
}
static void renderAll(void) { while (jobHead < jobTail) renderOne(); }
static void claimSome(void) {
    for (int j = jobHead; j < jobTail && j < jobHead + 3; j++) {
        if (jobs[j].cancelled)
            break;
        jobs[j].claimed = 1;
        if (rnd(2))
            break;
    }
}
static void lockWait(void) { while (jobHead < jobTail && jobs[jobHead].claimed) renderOne(); }
static int cancelJob(uint64_t s) {
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].serial == s)
            return (jobs[j].cancelled || jobs[j].claimed) ? 0 : (jobs[j].cancelled = 1);
    return 0;
}
static Bool lorieCancelSerial(uint64_t s) { return cancelJob(s); }

/* The X server waiting on the renderer (lorieWaitCompleted): it gets as far in time, or only some way -
 * past a copy or two, on the next ones, which it has claimed - or not at all (forceTimeout), having
 * claimed everything queued or not (forceRunning). What lorieAwaitCopies does when the wait runs out is
 * the real code's: the shared lock waits out what was claimed, and a late report is waited for. */
static int forceTimeout, forceRunning;
static uint64_t lorieCoreCopyWaitUs(void) { return 8000; }
static Bool lorieWaitCompleted(uint64_t serial, uint64_t startUs, uint64_t maxUs) {
    (void) startUs; (void) maxUs;
    if (forceTimeout) {
        if (forceRunning)
            for (int j = jobHead; j < jobTail; j++)
                jobs[j].claimed = 1;
    } else if (healthy || rnd(4)) {
        while (fakeCompleted < serial && jobHead < jobTail)
            renderOne();
    } else {
        for (int n = rnd(3); n > 0 && jobHead < jobTail; n--)
            renderOne();
        if (rnd(2))
            claimSome();
    }
    return fakeCompleted >= serial;
}
static Bool harnessLock(void) { lockWait(); return TRUE; }
#define lorie_mutex_lock(mutex, pid) harnessLock()
#define lorie_mutex_unlock(mutex, pid) ((void) 0)
static int harnessNap(void) {
    fakeNow += 1000;
    if (lateReport == 1 && unreported) {
        fakeCompleted = unreported;
        unreported = 0;
    }
    return 0;
}
#define nanosleep(t, rem) ((void) (t), harnessNap())
static void lorieRegisterBuffer(LorieBuffer *b) { (void) b; }
static Bool lorieConnectionAlive(void) { return TRUE; }
static Bool lorieRendererAvailable(void) { return TRUE; }
static int lorieSharedLockHeld;
#include "corecopy_src.inc"

/* pixmaps as lorieCoreCopyOnGpu sees them */
static LoriePixmapPriv priv, clientPriv, regularPriv, outPriv;
#define ROOT ((PixmapPtr) &priv)

/* what the X server drew and copied, in order: the reference */
enum { FILL, MOVE, COPYIN };
typedef struct { int kind; BoxRec b; uint32_t v; int sdx, sdy; } Ev;
#define MAXEV 8192
static Ev ev[MAXEV];
static int nev;
static uint32_t value;
static void replay(uint32_t *ref, int upTo) {
    static uint32_t snap[W * H];
    memset(ref, 0, W * H * sizeof *ref);
    for (int e = 0; e < upTo; e++) {
        BoxRec b = ev[e].b;
        if (ev[e].kind == MOVE)
            memcpy(snap, ref, sizeof snap);
        for (int y = b.y1; y < b.y2; y++)
            for (int x = b.x1; x < b.x2; x++)
                ref[y * W + x] = ev[e].kind == FILL ? ev[e].v
                               : ev[e].kind == MOVE ? snap[(y + ev[e].sdy) * W + x + ev[e].sdx]
                               : clientPixels[(y + ev[e].sdy) * W + x + ev[e].sdx];
    }
}
static int sameAsRef(int slot, int upTo, int *fx, int *fy) {
    static uint32_t ref[W * H];
    replay(ref, upTo);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (pixels[slot][y * W + x] != ref[y * W + x]) { *fx = x; *fy = y; return 0; }
    return 1;
}

/* An EXA fallback beginning (lorieExaFallbackBegin): repairs to the GPU, the queue waited for, in rounds */
static void preflight(void) {
    for (int round = 0; round < 4; round++) {
        int before = jobTail;
        lorieRootRepairOnGpu(&priv);
        if (round && jobTail == before)
            break;
        if (healthy || rnd(4))
            renderAll();
        else {
            for (int n = rnd(3); n > 0 && jobHead < jobTail; n--)
                renderOne();
            break;
        }
    }
}
/* lorieExaAccess for a write over b: queued carries into the drawing slot there are cancelled */
static void cancelOver(BoxRec b) {
    for (int j = jobHead; j < jobTail; j++) {
        Job *jb = &jobs[j];
        if (jb->cancelled || jb->claimed || !jb->carry || jb->dst != priv.rootWrite)
            continue;
        for (int k = 0; k < jb->n; k++)
            if (meets(jb->boxes[k], b)) { jb->cancelled = 1; xCancel(jb->serial); break; }
    }
}
/* The CPU reading an area the drawing slot still owes after its PrepareAccess - the donor's own copies
 * there not landed, so it could not be fetched: the X server reads the slot's older content then, once
 * lorieExaFallbackBegin has waited what it may. That is the CPU path's, from before the GPU made any core
 * copy; here the renderer is let catch up first, and it is counted, so it neither hides nor is blamed on
 * what the GPU path does. */
static int cpuEarlyReads;
static void cpuReads(BoxRec b, int sdx, int sdy) {
    RegionRec r, over;
    RegionInit(&r, &b, 1);
    RegionTranslate(&r, sdx, sdy);
    RegionNull(&over);
    RegionIntersect(&over, &priv.rootOwed, &r);
    if (RegionNotEmpty(&over)) {
        cpuEarlyReads++;
        renderAll();
        xPrepare(&priv);
    }
    RegionUninit(&over);
    RegionUninit(&r);
}
static uint64_t cpuCoreBytes;
static void cpuWrite(BoxRec b, int kind, int sdx, int sdy, uint32_t v) {
    static uint32_t snap[W * H];
    RegionRec r;
    cancelOver(b);
    xPrepare(&priv);
    if (kind == MOVE)
        cpuReads(b, sdx, sdy);
    memcpy(snap, pixels[priv.rootWrite], sizeof snap);
    for (int y = b.y1; y < b.y2; y++)
        for (int x = b.x1; x < b.x2; x++)
            pixels[priv.rootWrite][y * W + x] = kind == FILL ? v
                                              : kind == MOVE ? snap[(y + sdy) * W + x + sdx]
                                              : clientPixels[(y + sdy) * W + x + sdx];
    if (kind != FILL)
        cpuCoreBytes += (uint64_t) (b.x2 - b.x1) * (b.y2 - b.y1) * 4;
    RegionInit(&r, &b, 1);
    lorieMarkRootStale(&priv, &r);
    lorieRootCpuDrawn(&priv, &r);
    RegionUninit(&r);
}
static void draw(BoxRec b, uint32_t v) {
    if (healthy || rnd(2)) preflight();
    cpuWrite(b, FILL, 0, 0, v);
    ev[nev++] = (Ev) { FILL, b, v, 0, 0 };
}
/* The damage after a copy into the root can cover more than it wrote - the part of a CopyArea whose
 * source was obscured, which it leaves as it was - and takes all of it out of what the slot owes
 * (lorieRootDamaged): so that part had better be right. The CPU's prepare fetched what it could there;
 * what it could not is the CPU path's own early read (cpuReads). */
static void damagedBeyond(BoxRec *extra, int byCpu) {
    RegionRec r, over;
    if (!extra)
        return;
    RegionInit(&r, extra, 1);
    RegionNull(&over);
    RegionIntersect(&over, &priv.rootOwed, &r);
    if (byCpu && RegionNotEmpty(&over)) {
        cpuEarlyReads++;
        renderAll();
        xPrepare(&priv);
    }
    RegionUninit(&over);
    lorieMarkRootStale(&priv, &r);
    lorieRootCpuDrawn(&priv, &r);
    RegionUninit(&r);
}
/* a copy into the root - a window moved, or a client's pixmap - by the GPU if it takes it, else the CPU;
 * the damage after it takes the area out of what the slot owes, as lorieRootDamaged does */
static int noPreflight;
static int copyInto(int kind, BoxRec b, int sdx, int sdy, BoxRec *extra) {
    RegionRec r;
    if (!noPreflight && (healthy || rnd(2))) preflight();
    RegionInit(&r, &b, 1);
    int done = lorieCoreCopyOnGpu(kind == MOVE ? LORIE_CORE_COPY_WINDOW : LORIE_CORE_COPY_AREA,
                                  kind == MOVE ? ROOT : (PixmapPtr) &clientPriv, ROOT, &r, sdx, sdy, 32, TRUE);
    if (done)
        lorieRootCpuDrawn(&priv, &r);
    RegionUninit(&r);
    if (!done)
        cpuWrite(b, kind, sdx, sdy, 0);
    damagedBeyond(extra, !done);
    ev[nev++] = (Ev) { kind, b, 0, sdx, sdy };
    V("%s [%d,%d %d,%d] from %+d,%+d %s\n", kind == MOVE ? "move" : "copy in", b.x1, b.y1, b.x2, b.y2, sdx, sdy,
      done ? "by the GPU" : "by the CPU");
    return done;
}
/* the root copied out into a client's AHB pixmap: it has to receive what the root holds now */
static int readsBad, readsChecked;
static void copyOut(BoxRec b, int sdx, int sdy) {
    static uint32_t ref[W * H];
    RegionRec r;
    if (healthy || rnd(2)) preflight();
    RegionInit(&r, &b, 1);
    int done = lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA, ROOT, (PixmapPtr) &outPriv, &r, sdx, sdy, 32, TRUE);
    RegionUninit(&r);
    if (!done) {
        xPrepare(&priv);
        cpuReads(b, sdx, sdy);
        for (int y = b.y1; y < b.y2; y++)
            for (int x = b.x1; x < b.x2; x++)
                outPixels[y * W + x] = pixels[priv.rootWrite][(y + sdy) * W + x + sdx];
        cpuCoreBytes += (uint64_t) (b.x2 - b.x1) * (b.y2 - b.y1) * 4;
    }
    replay(ref, nev);
    readsChecked++;
    for (int y = b.y1; y < b.y2; y++)
        for (int x = b.x1; x < b.x2; x++)
            if (outPixels[y * W + x] != ref[(y + sdy) * W + x + sdx]) {
                if (readsBad++ < 3)
                    printf("  FAIL seed %u: copied out %u at %d,%d, the root holds %u there (%s)\n", seedNow,
                           outPixels[y * W + x], x + sdx, y + sdy, ref[(y + sdy) * W + x + sdx], done ? "GPU" : "CPU");
                fails++;
                return;
            }
}

/* published slots, checked once what was queued before they went out has resolved; exempt only a carry
 * into one that the renderer gave up on (T26, T33) */
typedef struct { int slot, nev; uint64_t maxSerial; int nIn; int in[64]; } Check;
static Check checks[64];
static int nChecks, checked, mismatched;
static void runChecks(int force) {
    static uint32_t ref[W * H];
    for (int i = 0; i < nChecks; i++) {
        Check *c = &checks[i];
        if (c->maxSerial > fakeCompleted) {
            if (force || c->slot == priv.rootWrite) checks[i--] = checks[--nChecks];
            continue;
        }
        replay(ref, c->nev);
        int bad = 0, fx = 0, fy = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                BoxRec px = { x, y, x + 1, y + 1 };
                int exempt = 0;
                for (int k = 0; k < c->nIn && !exempt; k++) {
                    Job *j = &jobs[c->in[k]];
                    if (j->gaveUp)
                        for (int m = 0; m < j->n && !exempt; m++)
                            exempt = meets(j->boxes[m], px);
                }
                if (!exempt && pixels[c->slot][y * W + x] != ref[y * W + x] && bad++ == 0) { fx = x; fy = y; }
            }
        if (bad) {
            if (mismatched++ < 3)
                printf("  FAIL seed %u: slot %d went out with %d pixels wrong, first at %d,%d: %u, should be %u\n",
                       seedNow, c->slot, bad, fx, fy, pixels[c->slot][fy * W + fx], ref[fy * W + fx]);
            fails++;
        }
        checked++;
        checks[i--] = checks[--nChecks];
    }
}
/* A copy within the root staged through a slot: one the renderer neither holds nor is about to take, and
 * nothing it went out with is shown any more - what it is checked for is moot from here. */
static void stagedThrough(int slot) {
    uint32_t word = fakeState.rootHandover;
    if (slot == priv.rootWrite)
        return;
    CHECK(!(word & LORIE_ROOT_HELD_MASK & (1u << slot)) && slot != published,
          "seed %u: staged through slot %d, which the renderer holds or is about to take", seedNow, slot);
    for (int i = 0; i < nChecks; i++)
        if (checks[i].slot == slot) checks[i--] = checks[--nChecks];
}
/* The renderer takes what was published last at its next frame, not at once: until then the slot is the
 * newest without being held (untaken), and the next render step takes it. */
static int untaken = -1, lagTake;
static void rendererTakes(void) {
    if (untaken >= 0)
        fakeState.rootHandover |= 1u << untaken;
    untaken = -1;
}
static int publish(void) {
    int drawn = priv.rootWrite;
    runChecks(0);
    rendererTakes();
    if (!handover(&priv))
        return 0;
    if (lagTake && rnd(2)) {
        fakeState.rootHandover &= ~(1u << drawn);
        untaken = drawn;
    }
    if (nChecks < 64) {
        Check *c = &checks[nChecks++];
        c->slot = drawn; c->nev = nev; c->maxSerial = serial; c->nIn = 0;
        for (int j = jobHead; j < jobTail && c->nIn < 64; j++)
            if (jobs[j].dst == drawn) c->in[c->nIn++] = j;
    }
    runChecks(0);
    return 1;
}

static void reset(void) {
    init(&priv);
    memset(&fakeState.presentStats, 0, sizeof fakeState.presentStats);
    cpuEarlyReads = 0;
    jobHead = jobTail = 0; fakeState.gpuCopyQueue.writeIndex = fakeState.gpuCopyQueue.readIndex = 0; serial = 0; nev = 0; nChecks = 0; value = 1000; cpuCoreBytes = 0;
    forceTimeout = forceRunning = lateReport = 0; unreported = 0; untaken = -1;
    harnessCarryOn = 1; harnessQueueCarry = queueCarry; harnessCancel = cancelJob; harnessLockWait = lockWait;
    bufs[CLIENT].desc = (LorieBuffer_Desc) { W, W, H, 200, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, LORIEBUFFER_AHARDWAREBUFFER };
    bufs[REGULAR].desc = (LorieBuffer_Desc) { W, W, H, 201, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, LORIEBUFFER_REGULAR };
    bufs[OUT].desc = (LorieBuffer_Desc) { W, W, H, 202, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, LORIEBUFFER_AHARDWAREBUFFER };
    memset(&clientPriv, 0, sizeof clientPriv); clientPriv.buffer = &bufs[CLIENT];
    memset(&regularPriv, 0, sizeof regularPriv); regularPriv.buffer = &bufs[REGULAR];
    memset(&outPriv, 0, sizeof outPriv); outPriv.buffer = &bufs[OUT];
    for (int k = 0; k < W * H; k++) { clientPixels[k] = 500000 + k; regularPixels[k] = 600000 + k; outPixels[k] = 0; }
}
/* a picture in the root, published and carried into every slot */
static void picture(void) {
    for (int y = 0; y < H; y += 2)
        draw((BoxRec) { 0, (short) y, W, (short) (y + 2) }, 10 + y);
    for (int x = 0; x < W; x += 8)
        draw((BoxRec) { (short) x, 0, (short) (x + 3), H }, 100 + x);
    for (int k = 0; k < 2 * LORIE_ROOT_SLOTS; k++) { publish(); renderAll(); }
}
static BoxRec rbox(BoxRec in) {
    int x1 = in.x1 + rnd(in.x2 - in.x1), y1 = in.y1 + rnd(in.y2 - in.y1);
    int x2 = x1 + 1 + rnd(in.x2 - x1), y2 = y1 + 1 + rnd(in.y2 - y1);
    return (BoxRec) { x1, y1, x2, y2 };
}
/* a box and an offset with both inside the root */
static void randomMove(BoxRec *b, int *sdx, int *sdy) {
    do {
        *b = rbox((BoxRec) { 0, 0, W, H });
        *sdx = (int) rnd(13) - 6;
        *sdy = (int) rnd(7) - 3;
    } while (b->x1 + *sdx < 0 || b->x2 + *sdx > W || b->y1 + *sdy < 0 || b->y2 + *sdy > H || (!*sdx && !*sdy));
}

/* T34_WATCH=x,y with a seed: that pixel in every slot, what the root should hold there, and whether the
 * drawing slot owes it, after every step */
static int watchX = -1, watchY = -1;
static void watch(const char *what) {
    static uint32_t ref[W * H];
    if (watchX < 0)
        return;
    replay(ref, nev);
    printf("    [%s] w=%d ref %u slots", what, priv.rootWrite, ref[watchY * W + watchX]);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
        printf(" %u", pixels[i][watchY * W + watchX]);
    printf(" owed %d (donor %d) jobs %d..%d\n", pixman_region_contains_point(&priv.rootOwed, watchX, watchY, NULL),
           priv.rootOwedDonor, jobHead, jobTail);
}
static void randomized(uint32_t seed, int steps) {
    reset();
    rng = seedNow = seed;
    failCarries = healthy ? 0 : 8;
    lagTake = seed % 2;
    picture();
    for (int s = 0; s < steps && nev < MAXEV - 8 && jobTail < MAXJ - 64 && serial < FAKE_SERIALS - 64; s++) {
        uint32_t a = rnd(100);
        BoxRec b;
        int sdx, sdy;
        if (a < 20)
            draw(rbox((BoxRec) { 0, 0, W, H }), ++value);
        else if (a < 45) {
            randomMove(&b, &sdx, &sdy);
            copyInto(MOVE, b, sdx, sdy, NULL);
        } else if (a < 55) {
            randomMove(&b, &sdx, &sdy);
            BoxRec eb = rbox((BoxRec) { 0, 0, W, H });
            copyInto(COPYIN, b, sdx, sdy, rnd(3) ? NULL : &eb);
        } else if (a < 62) {
            randomMove(&b, &sdx, &sdy);
            copyOut(b, sdx, sdy);
        } else if (a < 80) {
            rendererTakes();
            for (int n = rnd(4); n > 0 && jobHead < jobTail; n--)
                renderOne();
        } else {
            harnessCarryOn = healthy || rnd(5) != 0;
            publish();
        }
        watch(a < 20 ? "draw" : a < 45 ? "move" : a < 55 ? "copy in" : a < 62 ? "copy out" : a < 80 ? "render" : "publish");
    }
    failCarries = 0;
    lagTake = 0;
    rendererTakes();
    renderAll();
    runChecks(0);
    for (int k = 0; k < 2; k++) {
        int drawn = priv.rootWrite;
        CHECK(handover(&priv), "seed %u: no publish once everything had resolved", seed);
        renderAll();
        checks[nChecks++] = (Check) { drawn, nev, serial, 0, { 0 } };
        runChecks(1);
    }
}

/* a client's present into the drawing slot, queued and booked as lorieTryScheduleGpuCopy does */
static void presentPending(BoxRec pb, uint32_t v) {
    RegionRec r;
    Job *j = &jobs[jobTail++];
    memset(j, 0, sizeof *j);
    j->serial = ++serial; j->dst = priv.rootWrite; j->present = 1; j->pb = pb; j->v = v;
    fakeState.gpuCopyQueue.writeIndex = (uint32_t) jobTail;
    RegionInit(&r, &pb, 1);
    lorieRootNoteGpuCopy(&priv, &r, j->serial);
    RegionUninit(&r);
    ev[nev++] = (Ev) { FILL, pb, v, 0, 0 };
}

int main(int argc, char **argv) {
    int fx, fy;

    if (argc > 1) {
        verbose = 1;
        healthy = getenv("T34_HEALTHY") != NULL;
        if (getenv("T34_WATCH"))
            sscanf(getenv("T34_WATCH"), "%d,%d", &watchX, &watchY);
        randomized((uint32_t) atoi(argv[1]), 400);
        return fails != 0;
    }

    /* a window moved in each direction, overlapping where it was: exactly the CPU's copy, by the GPU */
    static const int dirs[][2] = { { 3, 2 }, { -3, -2 }, { 5, 0 }, { 0, -4 }, { -6, 3 } };
    for (unsigned d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
        reset();
        rng = 1;
        picture();
        BoxRec b = { 10, 4, 40, 12 };
        int temp = lorieRootTempSlot(&priv);
        uint32_t epoch = temp >= 0 ? priv.rootEpoch[temp] : 0;
        CHECK(copyInto(MOVE, b, dirs[d][0], dirs[d][1], NULL), "move %+d,%+d: not by the GPU", dirs[d][0], dirs[d][1]);
        CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy), "move %+d,%+d: the drawing slot differs at %d,%d", dirs[d][0],
              dirs[d][1], fx, fy);
        CHECK(temp >= 0 && priv.rootEpoch[temp] == epoch + 1 && pixman_region_not_empty(&priv.rootStale[temp]),
              "move %+d,%+d: the slot it went through not marked as holding something else", dirs[d][0], dirs[d][1]);
        CHECK(cpuCoreBytes == 0 && fakeState.presentStats.coreGpuCopies[LORIE_CORE_COPY_WINDOW] == 1,
              "move %+d,%+d: %llu bytes copied by the CPU", dirs[d][0], dirs[d][1], (unsigned long long) cpuCoreBytes);
        for (int k = 0; k < 2 * LORIE_ROOT_SLOTS; k++) {
            int drawn = priv.rootWrite;
            CHECK(publish(), "move: no publish");
            renderAll();
            CHECK(sameAsRef(drawn, nev, &fx, &fy), "move %+d,%+d: slot %d went out wrong at %d,%d after it", dirs[d][0],
                  dirs[d][1], drawn, fx, fy);
        }
    }

    /* out of time with the staged move's second step not claimed: taken back whole, the CPU makes it */
    reset(); rng = 2; picture();
    forceTimeout = 1;
    CHECK(!copyInto(MOVE, (BoxRec) { 10, 4, 40, 12 }, 3, 2, NULL), "timeout: said made");
    forceTimeout = 0;
    CHECK(fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_TIMEOUT] == 1, "timeout: not counted");
    renderAll();
    CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy), "timeout: the drawing slot differs at %d,%d", fx, fy);
    for (int k = 0; k < LORIE_ROOT_SLOTS; k++) { int drawn = priv.rootWrite; publish(); renderAll();
        CHECK(sameAsRef(drawn, nev, &fx, &fy), "timeout: slot %d went out wrong at %d,%d", drawn, fx, fy); }
    /* ... and with it already running: waited out, made once */
    reset(); rng = 3; picture();
    forceTimeout = forceRunning = 1;
    CHECK(copyInto(MOVE, (BoxRec) { 10, 4, 40, 12 }, -3, -2, NULL), "running: not made");
    forceTimeout = forceRunning = 0;
    CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy) && cpuCoreBytes == 0, "running: the drawing slot differs at %d,%d, "
          "or the CPU copied too", fx, fy);
    /* ... reported only after the renderer let go of the lock, and later than the X server waits: made all
     * the same, not made a second time by the CPU over what it moved */
    for (int late = 1; late <= 2; late++) {
        reset(); rng = 3; picture();
        forceTimeout = forceRunning = 1;
        lateReport = late;
        CHECK(copyInto(MOVE, (BoxRec) { 10, 4, 40, 12 }, -3, -2, NULL), "reported late (%d): not made", late);
        forceTimeout = forceRunning = 0;
        lateReport = 0;
        if (unreported)
            fakeCompleted = unreported;
        CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy) && cpuCoreBytes == 0,
              "reported late (%d): the drawing slot differs at %d,%d, or the CPU copied too", late, fx, fy);
    }

    /* the slot a move is staged through: never one the renderer holds or is about to take, one a carry
     * still reads, the donor of what the drawing slot owes or of what another went out conditional on,
     * one a GPU write is still pending into, or one that went out with copies still unresolved */
    for (int rule = 0; rule <= 7; rule++) {
        static const char *rules[] = { "nothing", "held", "about to be taken", "pinned by a carry", "the owed donor",
                                       "a conditional donor", "written by the GPU", "conditional itself" };
        reset(); rng = 7; picture();
        int x = -1;
        for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
            if (i != priv.rootWrite && !(fakeState.rootHandover & LORIE_ROOT_HELD_MASK & (1u << i)) && i != published) {
                if (x < 0) x = i;
                else priv.rootCarrySrcSerial[i] = fakeCompleted + 1000;
            }
        int other = (x + 1) % LORIE_ROOT_SLOTS == priv.rootWrite ? (x + 2) % LORIE_ROOT_SLOTS : (x + 1) % LORIE_ROOT_SLOTS;
        BoxRec bx = { 0, 0, 4, 4 };
        uint32_t word = fakeState.rootHandover;
        switch (rule) {
        case 1: fakeState.rootHandover |= 1u << x; break;
        case 2: fakeState.rootHandover = (word & ~((uint32_t) LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT))
                                         | ((uint32_t) x << LORIE_ROOT_NEWEST_SHIFT); break;
        case 3: priv.rootCarrySrcSerial[x] = fakeCompleted + 1000; break;
        case 4: RegionUninit(&priv.rootOwed); RegionInit(&priv.rootOwed, &bx, 1); priv.rootOwedDonor = x;
                priv.rootOwedDonorEpoch = priv.rootEpoch[x]; break;
        case 5: priv.rootCondCount[other] = 1; priv.rootCondDonor[other] = x;
                RegionInit(&priv.rootCond[other][0].region, &bx, 1); priv.rootCond[other][0].serial = fakeCompleted; break;
        case 6: RegionUnion(&priv.rootGpuPending[x], &priv.rootGpuPending[x], &(RegionRec) { bx, NULL });
                priv.rootGpuPendingSerial[x] = fakeCompleted + 1000; break;
        case 7: priv.rootCondCount[x] = 1; priv.rootCondDonor[x] = -1;
                RegionInit(&priv.rootCond[x][0].region, &bx, 1); priv.rootCond[x][0].serial = fakeCompleted + 1000; break;
        }
        int got = lorieRootTempSlot(&priv);
        CHECK(x >= 0 && (rule ? got == -1 : got == x), "staging slot: with the only candidate %s, got %d", rules[rule], got);
    }

    /* no slot to stage through: every other one held, pinned, or about to be shown */
    reset(); rng = 4; picture();
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
        if (i != priv.rootWrite) priv.rootCarrySrcSerial[i] = fakeCompleted + 1000;   /* a carry still to read it */
    CHECK(!copyInto(MOVE, (BoxRec) { 10, 4, 40, 12 }, 3, 2, NULL), "no slot: said made");
    CHECK(fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_NO_SLOT] == 1, "no slot: not counted as that");
    CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy), "no slot: the CPU's copy is wrong at %d,%d", fx, fy);

    /* reading an area the drawing slot owes from the slot that went out, whose present there has not
     * landed: waited for, then repaired and read by the GPU - or, with the renderer stuck, the CPU's */
    for (int stuck = 0; stuck < 2; stuck++) {
        reset(); rng = 5; picture();
        presentPending((BoxRec) { 12, 6, 30, 10 }, 77);
        CHECK(handover(&priv), "waiting source: no publish");
        forceTimeout = stuck;
        noPreflight = 1;
        int done = copyInto(MOVE, (BoxRec) { 40, 6, 58, 10 }, -28, 0, NULL);
        forceTimeout = noPreflight = 0;
        if (stuck)
            CHECK(!done && fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_OWED] == 1,
                  "waiting source, renderer stuck: given to the GPU, reading the area before the present had landed");
        else
            CHECK(done && fakeState.presentStats.gpuOwedRepairs > 0,
                  "waiting source: not waited for and repaired by the GPU");
        renderAll();
        CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy), "waiting source%s: the drawing slot differs at %d,%d",
              stuck ? ", renderer stuck" : "", fx, fy);
    }
    /* the same area owed, a client's pixmap copied in elsewhere, and the damage after it covering that area
     * too (a CopyArea whose source was partly obscured): the area is settled first, not left owed-no-more
     * with the old content */
    reset(); rng = 9; picture();
    presentPending((BoxRec) { 12, 6, 30, 10 }, 78);
    CHECK(handover(&priv), "damage beyond: no publish");
    noPreflight = 1;
    CHECK(copyInto(COPYIN, (BoxRec) { 0, 12, 10, 16 }, 3, -5, &(BoxRec) { 0, 6, 40, 16 }),
          "damage beyond: not by the GPU");
    noPreflight = 0;
    {
        int drawn = priv.rootWrite;
        CHECK(publish(), "damage beyond: no second publish");
        renderAll();
        CHECK(sameAsRef(drawn, nev, &fx, &fy), "damage beyond: went out with the old content at %d,%d", fx, fy);
    }

    /* between pixmaps: a client's AHB pixmap into the root, the root out into one; not one in plain
     * memory, not within one other than the root, not more rects than an entry within the root, not a
     * raster op, not 16 bpp */
    reset(); rng = 6; picture();
    CHECK(copyInto(COPYIN, (BoxRec) { 5, 5, 50, 15 }, 7, -3, NULL), "copy in: not by the GPU");
    CHECK(sameAsRef(priv.rootWrite, nev, &fx, &fy), "copy in: the drawing slot differs at %d,%d", fx, fy);
    readsBad = 0;
    copyOut((BoxRec) { 0, 0, 30, 10 }, 20, 4);
    CHECK(readsBad == 0 && fakeState.presentStats.coreGpuCopies[LORIE_CORE_COPY_AREA] == 2, "copy out: wrong or not by the GPU");
    {
        RegionRec r;
        BoxRec b = { 0, 0, 8, 8 };
        RegionInit(&r, &b, 1);
        CHECK(!lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA, (PixmapPtr) &regularPriv, ROOT, &r, 1, 1, 32, TRUE) &&
              fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_NOT_GPU] == 1, "plain memory: given to the GPU");
        CHECK(!lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA, (PixmapPtr) &clientPriv, (PixmapPtr) &clientPriv, &r, 1, 1, 32, TRUE) &&
              fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_SAME_PIXMAP] == 1, "within a client pixmap: given to the GPU");
        CHECK(!lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA, (PixmapPtr) &clientPriv, ROOT, &r, 1, 1, 32, FALSE) &&
              !lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA, (PixmapPtr) &clientPriv, ROOT, &r, 1, 1, 16, TRUE) &&
              fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_OP] == 2, "not a plain 32 bpp copy: given to the GPU");
        RegionUninit(&r);
        RegionNull(&r);
        for (int k = 0; k < 70; k++) {
            RegionRec one;
            BoxRec px = { (short) (k % 32 * 2), (short) (k / 32 * 3), (short) (k % 32 * 2 + 1), (short) (k / 32 * 3 + 1) };
            RegionInit(&one, &px, 1); RegionUnion(&r, &r, &one); RegionUninit(&one);
        }
        CHECK(!lorieCoreCopyOnGpu(LORIE_CORE_COPY_WINDOW, ROOT, ROOT, &r, 1, 0, 32, TRUE) &&
              fakeState.presentStats.coreGpuKept[LORIE_CORE_KEPT_RECTS] == 1, "70 rects within the root: given to the GPU");
        RegionUninit(&r);
    }

    /* randomized: moves, copies in and out, drawing, handovers and the renderer at its own pace, running out
     * of time and giving carries up; then a healthy renderer, with which the CPU copies nothing */
    uint32_t seeds = getenv("T34_SEEDS") ? (uint32_t) atoi(getenv("T34_SEEDS")) : 200;
    readsBad = readsChecked = checked = mismatched = 0;
    uint64_t gpu = 0, cpu = 0;
    uint32_t kept[LORIE_CORE_KEPT_REASONS] = { 0 }, early = 0;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        randomized(seed, 400);
        gpu += fakeState.presentStats.coreGpuBytes[0] + fakeState.presentStats.coreGpuBytes[1];
        cpu += cpuCoreBytes;
        early += cpuEarlyReads;
        for (int w = 0; w < LORIE_CORE_KEPT_REASONS; w++) kept[w] += fakeState.presentStats.coreGpuKept[w];
    }
    printf("T34 randomized: %d published slots checked, %d wrong; %d copies out checked, %d wrong; core copies %.1f KB "
           "by the GPU, %.1f KB by the CPU (no slot %u, root owing %u, timeout %u, busy %u); CPU reads of an area "
           "not fetched yet %u\n", checked, mismatched, readsChecked, readsBad, gpu / 1024.0, cpu / 1024.0,
           kept[LORIE_CORE_KEPT_NO_SLOT], kept[LORIE_CORE_KEPT_OWED], kept[LORIE_CORE_KEPT_TIMEOUT],
           kept[LORIE_CORE_KEPT_BUSY], early);
    CHECK(checked > 1000 && readsChecked > 1000, "randomized: too little checked");
    CHECK(kept[LORIE_CORE_KEPT_TIMEOUT] > 0, "randomized: never ran out of time");

    healthy = 1;
    readsBad = readsChecked = checked = mismatched = 0;
    gpu = cpu = 0;
    memset(kept, 0, sizeof kept);
    uint64_t rootCpu = 0;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        randomized(seed, 400);
        gpu += fakeState.presentStats.coreGpuBytes[0] + fakeState.presentStats.coreGpuBytes[1];
        cpu += cpuCoreBytes;
        rootCpu += fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes;
        for (int w = 0; w < LORIE_CORE_KEPT_REASONS; w++) kept[w] += fakeState.presentStats.coreGpuKept[w];
    }
    healthy = 0;
    printf("T34 healthy renderer: %d published slots checked, %d wrong; %d copies out checked, %d wrong; core copies "
           "%.1f KB by the GPU, %.1f KB by the CPU (no slot %u); root carries and owed areas %.1f KB by the CPU\n", checked,
           mismatched, readsChecked, readsBad, gpu / 1024.0, cpu / 1024.0, kept[LORIE_CORE_KEPT_NO_SLOT], rootCpu / 1024.0);
    CHECK(cpu == 0 && rootCpu == 0, "healthy renderer: the CPU copied %llu bytes of core copies, %llu of the root's",
          (unsigned long long) cpu, (unsigned long long) rootCpu);

    printf("T34 core copies on the GPU: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
