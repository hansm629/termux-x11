/* T33: a handover's carry given to the GPU (lorieRootCarryOnGpu) instead of copied by the CPU.
 * Against the real handover, repair, replacement and take-back bookkeeping (see rootharness.h); the
 * renderer, its queue, and what it means for it to have claimed a copy are simulated.
 *
 * The carry is queued as copies from the slot going out into the next drawing slot, and that slot owes
 * the area until they are made. What has to hold:
 *   - the CPU never reads or draws the drawing slot over a carry that has not landed: one still queued
 *     is taken back and the area fetched by the CPU, one the renderer has claimed is waited out under
 *     the lock (PrepareAccess);
 *   - a carry made is not copied again by the CPU, and one not made is fetched from the slot it came
 *     from, which stays pinned for it;
 *   - every slot published matches what the X server drew and the clients presented, as in T26.
 */
#include "rootharness.h"

static int verbose;
#define V(...) do { if (verbose) printf(__VA_ARGS__); } while (0)

typedef struct {
    uint64_t serial;
    int carry, slot, from, n;           /* a carry copies boxes[] from slot `from` into `slot` */
    BoxRec b, boxes[LORIE_GPU_COPY_MAX_RECTS];
    uint32_t v;                         /* a present writes v over b into `slot` */
    int cancelled, claimed, gaveUp;
} Job;
#define MAXJ 4096
static Job jobs[MAXJ];
static int jobHead, jobTail;
static uint64_t serial;
static uint8_t outcome[FAKE_SERIALS];   /* 1 made, 2 not made */
static int failCarries, failPresents;   /* the renderer gives up on 1 in this many it has not claimed */
static uint32_t rng, seedNow;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static BoxRec rbox(BoxRec in) {
    int x1 = in.x1 + rnd(in.x2 - in.x1), y1 = in.y1 + rnd(in.y2 - in.y1);
    int x2 = x1 + 1 + rnd(in.x2 - x1), y2 = y1 + 1 + rnd(in.y2 - y1);
    return (BoxRec) { x1, y1, x2, y2 };
}
static int meets(BoxRec a, BoxRec b) { return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2; }
static int jobMeets(Job *j, BoxRec b) {
    if (!j->carry)
        return meets(j->b, b);
    for (int k = 0; k < j->n; k++)
        if (meets(j->boxes[k], b))
            return 1;
    return 0;
}

static uint64_t queueCarry(int from, int to, BoxPtr box, int n) {
    Job *j = &jobs[jobTail++];
    V("  carry %llu queued %d -> %d, %d rects, first [%d,%d %d,%d]\n", (unsigned long long) serial + 1, from, to, n,
      box[0].x1, box[0].y1, box[0].x2, box[0].y2);
    memset(j, 0, sizeof *j);
    j->serial = ++serial;
    j->carry = 1;
    j->from = from;
    j->slot = to;
    j->n = n;
    memcpy(j->boxes, box, (size_t) n * sizeof *box);
    return j->serial;
}
static void land(Job *j) {
    if (!j->carry) {
        fill(j->slot, j->b, j->v);
        return;
    }
    for (int k = 0; k < j->n; k++)
        for (int y = j->boxes[k].y1; y < j->boxes[k].y2; y++)
            for (int x = j->boxes[k].x1; x < j->boxes[k].x2; x++)
                pixels[j->slot][y * W + x] = pixels[j->from][y * W + x];
}
/* the renderer: the next job in the queue. One it has claimed runs; one cancelled is reported not made;
 * one it has not claimed it may give up on. */
static void renderOne(void) {
    Job *j = &jobs[jobHead++];
    int give = !j->claimed && ((j->carry && failCarries && !rnd(failCarries)) ||
                               (!j->carry && failPresents && !rnd(failPresents)));
    V("render %llu %s into %d%s%s%s\n", (unsigned long long) j->serial, j->carry ? "carry" : "present", j->slot,
      j->carry ? " from " : "", j->carry ? (const char *[]) { "0", "1", "2", "3", "4" }[j->from] : "",
      j->cancelled ? " (cancelled)" : give ? " (given up)" : j->claimed ? " (claimed)" : "");
    if (j->cancelled || give) {
        j->gaveUp = !j->cancelled;
        gpuFails(j->serial);
        outcome[j->serial % FAKE_SERIALS] = 2;
    } else {
        land(j);
        fakeCompleted = j->serial;
        outcome[j->serial % FAKE_SERIALS] = 1;
    }
}
static void renderAll(void) { while (jobHead < jobTail) renderOne(); }
/* the renderer takes the next job or two under the lock: from then on they cannot be cancelled */
static void claimSome(void) {
    for (int j = jobHead; j < jobTail && j < jobHead + 2; j++) {
        if (jobs[j].cancelled)
            break;
        jobs[j].claimed = 1;
        if (rnd(2))
            break;
    }
}
static void lockWait(void) { while (jobHead < jobTail && jobs[jobHead].claimed) renderOne(); }
/* An EXA fallback beginning (lorieExaFallbackBegin): what the drawing slot can have now goes to the GPU
 * (lorieRootRepairOnGpu), then the queue is waited for - drained in time, or not; drained, what became
 * fetchable as it drained goes to the GPU as well, and is waited for in turn. */
static int preflights, healthy;
static void preflight(LoriePixmapPriv *priv) {
    V("preflight\n");
#ifdef HAVE_GPU_REPAIR
    lorieRootRepairOnGpu(priv);
#else
    (void) priv;
#endif
    preflights++;
    for (int round = 0; round < 4; round++) {
        int before = jobTail;
#ifdef HAVE_GPU_REPAIR
        if (round)
            lorieRootRepairOnGpu(priv);
#endif
        if (round && jobTail == before)
            break;
        if (healthy || rnd(4))
            renderAll();
        else {
            for (int n = rnd(3); n > 0 && jobHead < jobTail; n--)
                renderOne();
            break;                                     /* the wait ran out */
        }
    }
}
static int cancelJob(uint64_t s) {
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].serial == s)
            return (jobs[j].cancelled || jobs[j].claimed) ? 0 : (jobs[j].cancelled = 1);
    return 0;
}

/* What the X server drew and the clients presented, in order: the reference. */
#define MAXEV 4096
typedef struct { int cpu; BoxRec b; uint32_t v; uint64_t serial; } Ev;
static Ev ev[MAXEV];
static int nev;
static uint32_t value;

static void reference(uint32_t *ref, int upTo, uint8_t *unknown) {
    memset(ref, 0, W * H * sizeof *ref);
    if (unknown)
        memset(unknown, 0, W * H);
    for (int e = 0; e < upTo; e++) {
        uint8_t o = ev[e].cpu ? 1 : outcome[ev[e].serial % FAKE_SERIALS];
        for (int y = ev[e].b.y1; y < ev[e].b.y2; y++)
            for (int x = ev[e].b.x1; x < ev[e].b.x2; x++) {
                if (o == 1) {
                    ref[y * W + x] = ev[e].v;
                    if (unknown)
                        unknown[y * W + x] = 0;
                } else if (o == 0 && unknown)
                    unknown[y * W + x] = 1;      /* still in flight: either is right for now */
            }
    }
}

/* CPU drawing, as it reaches the root: queued copies into the drawing slot it would draw over are
 * cancelled (lorieExaAccess), then PrepareAccess (xDraw: carries taken back, repair, lock), then it draws */
static void cpuDrawAt(LoriePixmapPriv *priv, BoxRec b, uint32_t v, int mayClaim) {
    if (mayClaim && (healthy || rnd(2) == 0))
        preflight(priv);
    if (mayClaim && rnd(3) == 0)
        claimSome();
    V("draw %u [%d,%d %d,%d] into %d\n", v, b.x1, b.y1, b.x2, b.y2, priv->rootWrite);
    for (int j = jobHead; j < jobTail; j++)
        if (!jobs[j].cancelled && !jobs[j].claimed && jobs[j].slot == priv->rootWrite && jobMeets(&jobs[j], b)) {
            jobs[j].cancelled = 1;
            xCancel(jobs[j].serial);
        }
    xDraw(priv, b, v);
    ev[nev++] = (Ev) { 1, b, v, 0 };
}
static void cpuDraw(LoriePixmapPriv *priv, BoxRec b, uint32_t v) { cpuDrawAt(priv, b, v, 1); }
static void draw(LoriePixmapPriv *priv, BoxRec b, uint32_t v) { cpuDrawAt(priv, b, v, 0); }

/* A CPU read of the drawing slot - moving a window reads the root - after its PrepareAccess: it must see
 * what is there now. Exempt only presents still in flight, and an area the slot is still owed from a
 * donor whose own copies have not landed (rootOwedSerial), which the access goes ahead without by
 * design; a carry never is. */
static int readsBad, readsChecked;
static void xRead(LoriePixmapPriv *priv, BoxRec b, const char *what) {
    static uint32_t ref[W * H];
    static uint8_t unknown[W * H];
    RegionRec waiting;

    if (healthy || rnd(2) == 0)
        preflight(priv);
    if (rnd(3) == 0)
        claimSome();
    V("read [%d,%d %d,%d] of %d\n", b.x1, b.y1, b.x2, b.y2, priv->rootWrite);
    /* xPrepare, a step at a time: what is still owed from a donor whose copies have not landed is what
     * the repair left, before the lock waited out whatever the renderer had claimed */
    lorieRootTakeBackCarries(priv);
#ifdef HAVE_FETCH_THROUGH
    lorieRootFetchThroughCarries(priv);
#endif
    lorieRepairRootOwed(priv);
    RegionNull(&waiting);
    if (!lorieGpuCopyResolved(priv->rootOwedSerial))
        RegionSubtract(&waiting, &priv->rootOwed, &priv->rootOwedNow);
    lockWait();
    reference(ref, nev, unknown);
    readsChecked++;
    for (int y = b.y1; y < b.y2; y++)
        for (int x = b.x1; x < b.x2; x++) {
            if (unknown[y * W + x] || pixman_region_contains_point(&waiting, x, y, NULL))
                continue;
            if (pixels[priv->rootWrite][y * W + x] != ref[y * W + x]) {
                if (readsBad++ < 3)
                    printf("  FAIL %s seed %u: read %u at %d,%d of the drawing slot %d, it holds %u\n", what,
                           seedNow, pixels[priv->rootWrite][y * W + x], x, y, priv->rootWrite, ref[y * W + x]);
                fails++;
                RegionUninit(&waiting);
                return;
            }
        }
    RegionUninit(&waiting);
}

/* Published slots, checked as in T26 once every copy queued before they went out has resolved. Exempt:
 * copies into the slot still in flight when it went out that then were not made - a present, or a
 * carry the renderer gave up on. Nothing else, and nothing in any slot after it. */
typedef struct { int slot, nev; uint64_t maxSerial; int nIn; int in[64]; } Check;
static Check checks[64];
static int nChecks, checked, unchecked, mismatched;
static void runChecks(int force, int reusing) {
    static uint32_t ref[W * H];
    for (int i = 0; i < nChecks; i++) {
        Check *c = &checks[i];
        if (c->maxSerial > fakeCompleted) {
            if (c->slot == reusing || force) { unchecked++; checks[i--] = checks[--nChecks]; }
            continue;
        }
        reference(ref, c->nev, NULL);
        int bad = 0, fx = -1, fy = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                BoxRec px = { x, y, x + 1, y + 1 };
                int exempt = 0;
                for (int k = 0; k < c->nIn && !exempt; k++) {
                    Job *j = &jobs[c->in[k]];
                    if (outcome[j->serial % FAKE_SERIALS] == 2 && (!j->carry || j->gaveUp) && jobMeets(j, px))
                        exempt = 1;
                }
                if (!exempt && pixels[c->slot][y * W + x] != ref[y * W + x] && bad++ == 0) { fx = x; fy = y; }
            }
        if (bad) {
            if (mismatched++ < 3)
                printf("  FAIL randomized seed %u: slot %d went out with %d pixels other than the reference, "
                       "first at %d,%d: %u, should be %u\n", seedNow, c->slot, bad, fx, fy,
                       pixels[c->slot][fy * W + fx], ref[fy * W + fx]);
            fails++;
        }
        checked++;
        checks[i--] = checks[--nChecks];
    }
}
static void notePublished(int slot) {
    if (nChecks >= 64)
        return;
    Check *c = &checks[nChecks++];
    c->slot = slot; c->nev = nev; c->maxSerial = serial; c->nIn = 0;
    for (int j = jobHead; j < jobTail && c->nIn < 64; j++)
        if (jobs[j].slot == slot)
            c->in[c->nIn++] = j;
}

static void reset(LoriePixmapPriv *priv) {
    init(priv);
    harnessCarryOn = 1;
    harnessQueueCarry = queueCarry;
    harnessCancel = cancelJob;
    harnessLockWait = lockWait;
    jobHead = jobTail = 0; serial = 0; nev = 0; nChecks = 0; value = 1000;
    failCarries = failPresents = 0;
    memset(outcome, 0, sizeof outcome);
    memset(&fakeState.presentStats, 0, sizeof fakeState.presentStats);
}
static Bool present(LoriePixmapPriv *priv, BoxRec b) {
#ifdef HAVE_GPU_REPAIR
    lorieRootRepairOnGpu(priv);                        /* lorieTryScheduleGpuCopy, before the queue */
#endif
    uint64_t s = ++serial;
    if (!enqueueRootCopy(priv, b, s)) {
        serial--;
        cpuDraw(priv, b, ++value);
        return FALSE;
    }
    jobs[jobTail++] = (Job) { .serial = s, .slot = priv->rootWrite, .b = b, .v = ++value };
    V("present %llu value %u [%d,%d %d,%d] into %d\n", (unsigned long long) s, value, b.x1, b.y1, b.x2, b.y2, priv->rootWrite);
    ev[nev++] = (Ev) { 0, b, value, s };
    return TRUE;
}

static uint64_t lastCpu;
static void watchCpu(const char *where) {
    uint64_t now = fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes;
    if (now != lastCpu)
        V("  CPU copied %llu bytes in %s (kept on the CPU so far: rects %u)\n", (unsigned long long) (now - lastCpu),
          where, fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS]);
    lastCpu = now;
}
static void randomized(LoriePixmapPriv *priv, uint32_t seed, int steps) {
    static const BoxRec win[3] = { { 2, 1, 30, 12 }, { 20, 4, 50, 15 }, { 0, 0, 64, 16 } };
    reset(priv);
    rng = seedNow = seed;
    failCarries = healthy ? 0 : 8;
    failPresents = healthy ? 0 : 4;
    for (int s = 0; s < steps && nev < MAXEV - 8 && jobTail < MAXJ - 8 && serial < FAKE_SERIALS - 8; s++) {
        uint32_t a = rnd(100);
        lastCpu = fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes;
        if (a < 25) {
            present(priv, rbox(win[rnd(2)]));
            watchCpu("present");
        } else if (a < 40) {
            cpuDraw(priv, rbox(win[2]), ++value);
            watchCpu("draw");
        } else if (a < 50) {
            xRead(priv, win[2], "randomized");
            watchCpu("read");
        }
        else if (a < 75) {
            for (int n = rnd(4); n > 0 && jobHead < jobTail; n--)
                renderOne();
        } else {
            int drawn = priv->rootWrite;
            /* now and then the GPU cannot take the carry - never, with a healthy renderer */
            harnessCarryOn = (healthy || rnd(5) != 0) && !getenv("T33_NO_CARRY");
            runChecks(0, -1);
            V("handover from %d%s\n", drawn, harnessCarryOn ? "" : " (carry on the CPU)");
            int ok = handover(priv);
            watchCpu("handover");
            if (ok) {
                V("  published %d, drawing into %d\n", drawn, priv->rootWrite);
                notePublished(drawn);
                runChecks(0, priv->rootWrite);
            }
        }
    }
    /* a renderer that gives nothing up from here, and everything resolved: the next two slots out must
     * match exactly - including what the handover itself gives the GPU on the way (lorieRootRepairOnGpu) */
    failCarries = failPresents = 0;
    renderAll();
    runChecks(0, -1);
    xRead(priv, win[2], "randomized, all resolved");
    for (int k = 0; k < 2; k++) {
        int drawn = priv->rootWrite;
        CHECK(handover(priv), "randomized seed %u: no publish once everything had resolved", seed);
        renderAll();
        checks[nChecks++] = (Check) { drawn, nev, serial, 0, { 0 } };
        runChecks(1, -1);
    }
}

int main(int argc, char **argv) {
    LoriePixmapPriv priv;
    if (argc > 1) {                         /* t33 SEED: that run, step by step */
        verbose = 1;
        healthy = getenv("T33_HEALTHY") != NULL;
        randomized(&priv, (uint32_t) atoi(argv[1]), 600);
        return fails != 0;
    }
    BoxRec Q = { 40, 2, 56, 10 }, P = { 2, 2, 8, 6 }, R = { 8, 2, 24, 10 };
    const uint64_t qBytes = 16 * 8 * 4;

    /* Made before the CPU gets back to the slot: nothing for the CPU to copy, then or later. */
    reset(&priv);
    draw(&priv, Q, 5);
    int A = priv.rootWrite;
    CHECK(handover(&priv), "made: no publish");
    int B = priv.rootWrite;
    CHECK(fakeState.presentStats.gpuCarryJobs == 1 && fakeState.presentStats.gpuCarryBytes == qBytes,
          "made: %u carry copies, %llu bytes - Q is %llu", fakeState.presentStats.gpuCarryJobs,
          (unsigned long long) fakeState.presentStats.gpuCarryBytes, (unsigned long long) qBytes);
    CHECK(fakeState.presentStats.cpuCarryBytes == 0, "made: the CPU carried %llu bytes as well",
          (unsigned long long) fakeState.presentStats.cpuCarryBytes);
    CHECK(!areaIs(B, Q, 5), "made: Q in the new slot before the GPU had run - the test is not testing");
    renderAll();
    draw(&priv, P, 6);
    CHECK(areaIs(B, Q, 5), "made: the drawing slot lacks Q");
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == 0, "made: the CPU fetched %llu bytes the GPU had carried",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);
    CHECK(handover(&priv) && areaIs(B, Q, 5) && areaIs(B, P, 6), "made: B went out without Q or P");
    (void) A;

    /* Still queued when the CPU draws elsewhere in the slot: taken back, and the CPU fetches Q first. */
    reset(&priv);
    draw(&priv, Q, 5);
    CHECK(handover(&priv), "taken back: no publish");
    B = priv.rootWrite;
    draw(&priv, P, 6);
    CHECK(fakeState.presentStats.gpuCarryTakenBack == 1, "taken back: %u carries taken back",
          fakeState.presentStats.gpuCarryTakenBack);
    CHECK(areaIs(B, Q, 5) && areaIs(B, P, 6), "taken back: the drawing slot lacks Q or P");
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == qBytes, "taken back: the CPU fetched %llu bytes, not Q's %llu",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes, (unsigned long long) qBytes);
    renderAll();                                       /* the renderer reaches the cancelled entry */
    CHECK(handover(&priv) && areaIs(B, Q, 5) && areaIs(B, P, 6), "taken back: B went out without Q or P");

    /* Claimed by the renderer when the CPU gets there: waited out under the lock, not copied twice. */
    reset(&priv);
    draw(&priv, Q, 5);
    CHECK(handover(&priv), "claimed: no publish");
    B = priv.rootWrite;
    jobs[jobHead].claimed = 1;
    draw(&priv, P, 6);
    CHECK(areaIs(B, Q, 5) && areaIs(B, P, 6), "claimed: the drawing slot lacks Q or P");
    CHECK(fakeState.presentStats.gpuCarryTakenBack == 0 && fakeState.presentStats.cpuOwedFetchBytes == 0,
          "claimed: taken back %u, fetched %llu bytes", fakeState.presentStats.gpuCarryTakenBack,
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);

    /* Drawn over by the CPU while queued: the cancelled copy must not land on top of the new pixels. */
    reset(&priv);
    draw(&priv, Q, 5);
    CHECK(handover(&priv), "drawn over: no publish");
    B = priv.rootWrite;
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].slot == B && jobMeets(&jobs[j], Q)) { jobs[j].cancelled = 1; xCancel(jobs[j].serial); }
    draw(&priv, Q, 7);
    renderAll();
    CHECK(areaIs(B, Q, 7), "drawn over: the older Q landed over the newer drawing");

    /* A read of the drawing slot - a window being moved - before the carry has landed. */
    reset(&priv);
    draw(&priv, Q, 5);
    CHECK(handover(&priv), "read: no publish");
    readsBad = 0;
    xRead(&priv, Q, "read before the carry landed");
    CHECK(readsBad == 0, "read: the drawing slot was read with Q old");

    /* Not made, after the slot already went out with it: that slot shows Q old (nothing could know yet),
     * every slot after it has Q, fetched from where the carry came from. */
    reset(&priv);
    draw(&priv, Q, 5);
    A = priv.rootWrite;
    CHECK(handover(&priv), "not made: first publish");
    B = priv.rootWrite;
    CHECK(handover(&priv), "not made: B not published with its carry in flight");
    int C = priv.rootWrite;
    jobs[jobHead].gaveUp = 1;
    gpuFails(jobs[jobHead].serial);
    outcome[jobs[jobHead].serial % FAKE_SERIALS] = 2;
    jobHead++;
    renderAll();
    draw(&priv, P, 6);
    CHECK(areaIs(C, Q, 5), "not made: C does not have Q");
    CHECK(handover(&priv) && areaIs(C, Q, 5) && areaIs(C, P, 6), "not made: C went out without Q or P");
    CHECK(fakeState.presentStats.gpuCarryNotMade + fakeState.presentStats.rootOwedFromOlder >= 1 ||
          fakeState.presentStats.cpuOwedFetchBytes > 0, "not made: Q never fetched");
    CHECK(fakeState.presentStats.rootOwedLost == 0, "not made: Q lost track of");
    (void) A; (void) B;

    /* A read of the next slot through a carry still in flight into the slot that went out: what it lacks
     * there is not in that slot yet, but it is in the slot the carry reads, and has to be read from there
     * - not left as the old content, as if it were a present nobody can have yet. */
    reset(&priv);
    draw(&priv, Q, 5);
    A = priv.rootWrite;
    CHECK(handover(&priv), "through a carry: first publish");      /* Q carried A -> B, not landed */
    B = priv.rootWrite;
    CHECK(handover(&priv), "through a carry: B not published with its carry in flight");
    C = priv.rootWrite;
    xPrepare(&priv);                                              /* a PrepareAccess on C, nothing drained */
    CHECK(areaIs(C, Q, 5), "through a carry: C read with Q old while B's carry from A was in flight");
    CHECK(handover(&priv), "through a carry: C not published");
    renderAll();
    CHECK(areaIs(C, Q, 5), "through a carry: C went out without Q");
    (void) A;

#ifdef HAVE_GPU_REPAIR
    /* The GPU cannot take the carry at the handover - the queue busy - and can by the next EXA fallback:
     * the carry is not copied by the CPU there and then, but owed, and given to the GPU then. */
    reset(&priv);
    draw(&priv, Q, 5);
    harnessCarryOn = 0;
    CHECK(handover(&priv), "busy: no publish");
    B = priv.rootWrite;
    CHECK(fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes == 0,
          "busy: the CPU copied the carry at the handover");
    harnessCarryOn = 1;
    lorieRootRepairOnGpu(&priv);                       /* an EXA fallback begins ... */
    renderAll();                                       /* ... and the renderer drains in time */
    draw(&priv, P, 6);
    CHECK(fakeState.presentStats.gpuOwedRepairs == 1 && fakeState.presentStats.cpuCarryBytes +
          fakeState.presentStats.cpuOwedFetchBytes == 0, "busy: not given to the GPU by the next fallback (%u, %llu "
          "bytes by the CPU)", fakeState.presentStats.gpuOwedRepairs,
          (unsigned long long) (fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes));
    CHECK(areaIs(B, Q, 5) && areaIs(B, P, 6), "busy: the drawing slot lacks Q or P");

    /* An area a handover had to leave behind - a present still in flight into the slot going out - given
     * to the GPU once that present has landed, rather than fetched by the CPU when the slot is next
     * touched. */
    reset(&priv);
    A = priv.rootWrite;
    present(&priv, R);
    CHECK(handover(&priv), "owed by GPU: no publish");
    B = priv.rootWrite;
    renderAll();                                       /* the present lands in A; R is owed to B from A */
    lorieRootRepairOnGpu(&priv);                       /* an EXA fallback begins ... */
    renderAll();                                       /* ... and the renderer drains in time */
    draw(&priv, P, 6);
    CHECK(fakeState.presentStats.gpuOwedRepairs >= 1, "owed by GPU: the area was not given to the GPU");
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == 0, "owed by GPU: the CPU fetched %llu bytes",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes);
    CHECK(areaIs(B, R, jobs[0].v), "owed by GPU: B lacks the present");
    CHECK(handover(&priv) && areaIs(B, R, jobs[0].v), "owed by GPU: B went out without the present");
#endif

    /* Many rects: widened to their extents only past what the queue entries for a carry hold; up to that,
     * as many entries as they take. */
    reset(&priv);
    for (int k = 0; k < 40; k++)
        draw(&priv, (BoxRec) { (short) (k % 20 * 3), (short) (k / 20 * 4), (short) (k % 20 * 3 + 1), (short) (k / 20 * 4 + 1) }, 50 + k);
    B = priv.rootWrite;
    CHECK(handover(&priv), "rects: no publish");
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS] == 0 && fakeState.presentStats.gpuCarryJobs == 1,
          "rects: 40 rects - kept %u, given to the GPU %u", fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS],
          fakeState.presentStats.gpuCarryJobs);
    renderAll();
    xRead(&priv, (BoxRec) { 0, 0, 64, 16 }, "rects widened");
    CHECK(fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes == 0,
          "rects: the CPU copied %llu bytes", (unsigned long long) (fakeState.presentStats.cpuCarryBytes +
                                                                    fakeState.presentStats.cpuOwedFetchBytes));
    (void) B;
    reset(&priv);
    for (int k = 0; k < 70; k++)                       /* more rects than one queue entry holds */
        draw(&priv, (BoxRec) { (short) (k % 32 * 2), (short) (k / 32 * 3), (short) (k % 32 * 2 + 1), (short) (k / 32 * 3 + 1) }, 50 + k);
    CHECK(handover(&priv), "two entries: no publish");
    CHECK(fakeState.presentStats.gpuCarryJobs == 2, "two entries: %u carry copies", fakeState.presentStats.gpuCarryJobs);
    renderAll();
    xRead(&priv, (BoxRec) { 0, 0, 64, 16 }, "two entries");

    /* Randomized, against the reference: presents into overlapping windows, CPU drawing and reads, the
     * renderer claiming, running, giving up on copies at its own pace, and carries the GPU sometimes
     * cannot take. */
    int before = fails;
    checked = unchecked = mismatched = readsBad = readsChecked = 0;
    uint64_t gpuBytes = 0, takenBack = 0, notMade = 0, lost = 0, repairs = 0, cpuBytes = 0;
    uint32_t seeds = getenv("T33_SEEDS") ? (uint32_t) atoi(getenv("T33_SEEDS")) : 300;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        randomized(&priv, seed, 600);
        gpuBytes += fakeState.presentStats.gpuCarryBytes;
        takenBack += fakeState.presentStats.gpuCarryTakenBack;
        notMade += fakeState.presentStats.gpuCarryNotMade;
        lost += fakeState.presentStats.rootOwedLost;
        repairs += fakeState.presentStats.gpuOwedRepairs;
        cpuBytes += fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes;
    }
    printf("T33 randomized: %d published slots checked (%d skipped), %d differed; %d reads checked, %d wrong; "
           "carries: %.1f KB on the GPU (%llu owed areas), %llu taken back, %llu not made; %.1f KB copied by the CPU; "
           "%llu areas lost\n", checked, unchecked, mismatched, readsChecked, readsBad, gpuBytes / 1024.0,
           (unsigned long long) repairs, (unsigned long long) takenBack, (unsigned long long) notMade,
           cpuBytes / 1024.0, (unsigned long long) lost);
    CHECK(checked > 1000 && readsChecked > 1000, "randomized: too little checked (%d slots, %d reads)", checked, readsChecked);
    CHECK(gpuBytes > 0 && takenBack > 0 && notMade > 0, "randomized: some carry outcome never happened");
    CHECK(lost == 0, "randomized: %llu areas lost track of", (unsigned long long) lost);
    (void) before;

    /* A healthy renderer: it gives nothing up, takes every carry, and has the queue drained by the time
     * each EXA fallback's wait ends. Then the CPU copies nothing at all - not a carry, not an owed area. */
    healthy = 1;
    checked = unchecked = mismatched = readsBad = readsChecked = 0;
    uint64_t healthyCpu = 0, healthyGpu = 0;
    for (uint32_t seed = 1; seed <= seeds; seed++) {
        randomized(&priv, seed, 600);
        healthyCpu += fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes;
        healthyGpu += fakeState.presentStats.gpuCarryBytes;
    }
    healthy = 0;
    printf("T33 healthy renderer: %d published slots checked, %d differed; %d reads checked, %d wrong; %.1f KB on "
           "the GPU, %.1f KB copied by the CPU\n", checked, mismatched, readsChecked, readsBad, healthyGpu / 1024.0,
           healthyCpu / 1024.0);
    CHECK(healthyCpu == 0, "healthy renderer: the CPU copied %llu bytes", (unsigned long long) healthyCpu);
    CHECK(checked > 1000 && readsChecked > 1000, "healthy renderer: too little checked");

    printf("T33 root carry on the GPU: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
