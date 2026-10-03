/* T26: a copy queued over an area the drawing slot owes replaces it only once it is known to have been
 * made. Against the real handover, repair and replacement bookkeeping (see rootharness.h).
 *
 * Every case starts the same way: a copy of R lands in A after A went out, so B owes R with A as the
 * donor. Then a copy over R is queued into B, and what becomes of it varies. */
#include "rootharness.h"

static BoxRec R = { 8, 2, 24, 10 }, Q = { 40, 2, 56, 10 };
static BoxRec R1 = { 8, 2, 16, 10 }, R2 = { 16, 2, 24, 10 };   /* the two halves of R */
static int A, B, C;

/* A gets R = 1 by GPU after it went out; B is the drawing slot and owes R from A */
static void setup(LoriePixmapPriv *priv) {
    init(priv);
    A = priv->rootWrite;
    enqueueRootCopy(priv, R, 1);
    handover(priv);
    B = priv->rootWrite;
    gpuLands(A, R, 1, 1);
}

/*
 * Randomized: presents into three overlapping windows, CPU drawing anywhere, the renderer making or
 * failing copies at its own pace, and handovers, in random order - checked against a reference of what
 * each published slot should hold. The reference replays every present and every CPU draw in the order
 * the X server issued them, a present only if its copy was made, up to the slot's publish.
 *
 * One area per slot is exempt: copies into it still in flight when it went out, that then were not
 * made. The slot went out before that was known, which nothing can undo; what is checked is that no
 * slot after it inherits the old content.
 */
#define MAXEV 4096
typedef struct { int cpu; BoxRec b; uint32_t v; uint64_t serial; } Ev;
typedef struct { uint64_t serial; int slot; BoxRec b; uint32_t v; int cancelled; } Job;
typedef struct { int slot, nev; uint64_t maxSerial; int nIn; uint64_t in[64]; } Check;
static Ev ev[MAXEV];
static int nev;
static Job jobs[MAXEV];
static int jobHead, jobTail;
static uint8_t outcome[FAKE_SERIALS];          /* 1 made, 2 not made */
static Check checks[64];
static int nChecks, checked, unchecked, mismatched;
static uint32_t rng, seedNow;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static BoxRec rbox(BoxRec in) {
    int x1 = in.x1 + rnd(in.x2 - in.x1), y1 = in.y1 + rnd(in.y2 - in.y1);
    int x2 = x1 + 1 + rnd(in.x2 - x1), y2 = y1 + 1 + rnd(in.y2 - y1);
    return (BoxRec) { x1, y1, x2, y2 };
}
static int meets(BoxRec a, BoxRec b) { return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2; }
static void renderOne(void) {
    Job *j = &jobs[jobHead++];
    if (j->cancelled || rnd(4) == 0) { gpuFails(j->serial); outcome[j->serial % FAKE_SERIALS] = 2; }
    else { gpuLands(j->slot, j->b, j->v, j->serial); outcome[j->serial % FAKE_SERIALS] = 1; }
}
static void renderAll(void) { while (jobHead < jobTail) renderOne(); }
/* CPU drawing, as every CPU write reaches it: queued copies into the drawing slot it would draw over are
 * cancelled first (lorieExaAccess), then it draws */
static void cpuDraw(LoriePixmapPriv *priv, BoxRec b, uint32_t v) {
    for (int j = jobHead; j < jobTail; j++)
        if (!jobs[j].cancelled && jobs[j].slot == priv->rootWrite && meets(jobs[j].b, b)) {
            jobs[j].cancelled = 1;
            xCancel(jobs[j].serial);
        }
    xDraw(priv, b, v);
    ev[nev++] = (Ev) { 1, b, v, 0 };
}
/* compares a slot with the reference once every copy it could depend on has resolved */
static void runChecks(int force, int reusing) {
    static uint32_t ref[W * H];
    for (int i = 0; i < nChecks; i++) {
        Check *c = &checks[i];
        if (c->maxSerial > fakeCompleted) {
            if (c->slot == reusing || force) { unchecked++; checks[i--] = checks[--nChecks]; }
            continue;
        }
        memset(ref, 0, sizeof ref);
        for (int e = 0; e < c->nev; e++)
            if (ev[e].cpu || outcome[ev[e].serial % FAKE_SERIALS] == 1)
                for (int y = ev[e].b.y1; y < ev[e].b.y2; y++)
                    for (int x = ev[e].b.x1; x < ev[e].b.x2; x++)
                        ref[y * W + x] = ev[e].v;
        int bad = 0, fx = -1, fy = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int exempt = 0;
                for (int k = 0; k < c->nIn && !exempt; k++)
                    for (int e = 0; e < c->nev; e++)
                        if (!ev[e].cpu && ev[e].serial == c->in[k] && outcome[c->in[k] % FAKE_SERIALS] == 2 &&
                            x >= ev[e].b.x1 && x < ev[e].b.x2 && y >= ev[e].b.y1 && y < ev[e].b.y2)
                            exempt = 1;
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
static int cancelQueuedJob(uint64_t s) {
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].serial == s && !jobs[j].cancelled) return jobs[j].cancelled = 1;
    return 0;
}
static void randomized(LoriePixmapPriv *priv, uint32_t seed, int steps) {
    harnessCancel = cancelQueuedJob;
    static const BoxRec win[3] = { { 2, 1, 30, 12 }, { 20, 4, 50, 15 }, { 0, 0, 64, 16 } };
    uint64_t serial = 0;
    uint32_t value = 1000;
    init(priv);
    rng = seedNow = seed; nev = 0; jobHead = jobTail = 0; nChecks = 0;
    memset(outcome, 0, sizeof outcome);
    for (int s = 0; s < steps && nev < MAXEV - 4 && jobTail < MAXEV - 4; s++) {
        uint32_t a = rnd(100);
        if (a < 30) {                                   /* a present: queued, or drawn by the CPU if refused */
            BoxRec b = rbox(win[rnd(2)]);
            uint64_t sr = ++serial;
            if (enqueueRootCopy(priv, b, sr)) {
                jobs[jobTail++] = (Job) { sr, priv->rootWrite, b, ++value, 0 };
                ev[nev++] = (Ev) { 0, b, value, sr };
            } else
                cpuDraw(priv, b, ++value);
        } else if (a < 45) {                            /* CPU drawing anywhere */
            cpuDraw(priv, rbox(win[2]), ++value);
        } else if (a < 75) {                            /* the renderer gets through some of the queue */
            for (int n = rnd(4); n > 0 && jobHead < jobTail; n--)
                renderOne();
        } else {                                        /* a block handler */
            int drawn = priv->rootWrite;
            runChecks(0, -1);
            if (handover(priv) && nChecks < 64) {
                Check *c = &checks[nChecks++];
                c->slot = drawn; c->nev = nev; c->maxSerial = serial; c->nIn = 0;
                for (int j = jobHead; j < jobTail && c->nIn < 64; j++)
                    if (jobs[j].slot == drawn)
                        c->in[c->nIn++] = jobs[j].serial;
                runChecks(0, priv->rootWrite);
            }
        }
    }
    /* everything resolved: the next two slots out must match the reference exactly */
    renderAll();
    runChecks(0, -1);
    for (int k = 0; k < 2; k++) {
        int drawn = priv->rootWrite;
        CHECK(handover(priv), "randomized seed %u: no publish once everything had resolved", seed);
        checks[nChecks++] = (Check) { drawn, nev, serial, 0, { 0 } };
        runChecks(1, -1);
    }
}

int main(void) {
    LoriePixmapPriv priv;

    /* The review's counterexample: the replacement is not made, only Q is drawn by the CPU, B goes out.
     * B must have the donor's R, and so must C after it. */
    setup(&priv);
    enqueueRootCopy(&priv, R, 2);
    gpuFails(2);
    xDraw(&priv, Q, 3);
    CHECK(handover(&priv), "not made: B not published");
    CHECK(areaIs(B, R, 1), "not made: B went out with R old - the failed replacement dropped what B owed");
    CHECK(areaIs(B, Q, 3), "not made: B lost Q");
    C = priv.rootWrite;
    CHECK(handover(&priv), "not made: C not published");
    CHECK(areaIs(C, R, 1) && areaIs(C, Q, 3), "not made: C was carried old content from B");

    /* Cancelled by the X server for a CPU write over part of R: the CPU drawing stands where it drew, the
     * donor's content everywhere else in R. */
    setup(&priv);
    enqueueRootCopy(&priv, R, 2);
    xCancel(2);
    xDraw(&priv, R1, 3);
    gpuFails(2);                               /* the renderer reaching it later */
    CHECK(handover(&priv), "cancelled: B not published");
    CHECK(areaIs(B, R1, 3), "cancelled: the CPU drawing over R1 was overwritten");
    CHECK(areaIs(B, R2, 1), "cancelled: B went out with the rest of R old");

    /* Its source never arrived and the renderer gave up on it - after B had already gone out with it
     * pending. B itself shows R old (it went out before anything was known); nothing after it may. */
    setup(&priv);
    enqueueRootCopy(&priv, R, 2);
    CHECK(handover(&priv), "import failure: B held back by a copy in flight");
    C = priv.rootWrite;
    gpuFails(2);
    CHECK(handover(&priv), "import failure: C not published");
    CHECK(areaIs(C, R, 1), "import failure: C took R from B, which never got it");
    int D = priv.rootWrite;
    CHECK(handover(&priv), "import failure: D not published");
    CHECK(areaIs(D, R, 1), "import failure: D has R old");
    /* and B, drawn into again later, must not bring its old R back */
    for (int i = 0; i < 6 && priv.rootWrite != B; i++)
        handover(&priv);
    CHECK(priv.rootWrite == B, "import failure: B never came round again");
    handover(&priv);
    CHECK(areaIs(B, R, 1), "import failure: B went out again with its old R");

    /* Made: B has the new content and the donor's older content is never copied over it, whichever of
     * the two lands first. */
    setup(&priv);
    enqueueRootCopy(&priv, R, 2);
    gpuLands(B, R, 2, 2);
    CHECK(handover(&priv), "made: B not published");
    CHECK(areaIs(B, R, 2), "made: the donor's older R was copied over the replacement");
    C = priv.rootWrite;
    CHECK(handover(&priv), "made: C not published");
    CHECK(areaIs(C, R, 2), "made: C has an older R");

    /* Partial overlap: the replacement covers only R1. Not made, all of R comes from the donor; made, R1
     * is the replacement and R2 the donor's. */
    setup(&priv);
    enqueueRootCopy(&priv, R1, 2);
    gpuFails(2);
    CHECK(handover(&priv), "partial, not made: B not published");
    CHECK(areaIs(B, R, 1), "partial, not made: B went out with part of R old");
    setup(&priv);
    enqueueRootCopy(&priv, R1, 2);
    gpuLands(B, R1, 2, 2);
    CHECK(handover(&priv), "partial, made: B not published");
    CHECK(areaIs(B, R1, 2) && areaIs(B, R2, 1), "partial, made: R1 or R2 wrong");

    /* Order: the donor's content arrives while the replacement is still in flight. A repair then (any
     * PrepareAccess runs one) must leave R alone - it would race the GPU write, or land the older
     * content on top of it once that has happened. */
    init(&priv);
    A = priv.rootWrite;
    enqueueRootCopy(&priv, R, 1);
    handover(&priv);
    B = priv.rootWrite;
    enqueueRootCopy(&priv, R, 2);              /* queued before the donor's copy has even landed */
    gpuLands(A, R, 1, 1);                      /* the donor's copy lands; the replacement has not */
    xDraw(&priv, Q, 3);                        /* a PrepareAccess repairs what it can now */
    fill(B, R, 2); fakeCompleted = 2;          /* then the replacement lands */
    xDraw(&priv, Q, 4);                        /* and another repair runs */
    CHECK(handover(&priv), "order: B not published");
    CHECK(areaIs(B, R, 2), "order: the donor's older R landed over the replacement");
    /* the other way round: the replacement is reported not made with the donor's copy still pending -
     * B cannot go out until the donor's R can be fetched, and must not go out without it */
    init(&priv);
    A = priv.rootWrite;
    enqueueRootCopy(&priv, R, 1);
    handover(&priv);
    B = priv.rootWrite;
    enqueueRootCopy(&priv, R, 2);
    xCancel(2);                                /* known not made at once, before serial 1 resolves */
    Bool early = handover(&priv);
    CHECK(!early || areaIs(B, R, 1), "order: B went out with R old before the donor had it");
    gpuLands(A, R, 1, 1);
    if (!early)
        CHECK(handover(&priv), "order: B not published once the donor had R");
    CHECK(areaIs(B, R, 1), "order: B has R old");

    int before = fails;
    checked = unchecked = mismatched = 0;
#ifdef HAVE_REPLACING
    fakeState.presentStats.rootOwedLost = fakeState.presentStats.rootOwedFromOlder = 0;
#endif
    for (uint32_t seed = 1; seed <= 300; seed++)
        randomized(&priv, seed, 600);
    printf("T26 randomized: %d published slots checked (%d skipped, reused before their copies resolved), "
           "%d differed from the reference", checked, unchecked, mismatched);
#ifdef HAVE_REPLACING
    printf("; %u areas fetched from an older slot, %u areas no slot still had",
           fakeState.presentStats.rootOwedFromOlder, fakeState.presentStats.rootOwedLost);
#endif
    printf("\n");
    CHECK(checked > 1000, "randomized: too few slots checked (%d) to mean anything", checked);
    (void) before;

    printf("T26 owed area vs replacement outcome: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
