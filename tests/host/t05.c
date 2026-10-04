/* T05/T06: root slot handover, against the real handover, stale tracking and repair functions (see
 * rootharness.h for what is real and what is simulated). */
#include "rootharness.h"

int main(void) {
    LoriePixmapPriv priv;
    BoxRec R = { 8, 2, 24, 10 }, Q = { 40, 2, 56, 10 };

    /* T05 - the review's counterexample: R reaches A by GPU, the handover cannot carry it to B yet, the
     * copy then lands, only Q is drawn into B. B must not be published with R old, and nothing published
     * after it may carry R back to its old content. */
    init(&priv);
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
    init(&priv);
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

#ifdef HAVE_CPU_COPY_STATS
    /* T05c - what the CPU copies is counted against what it was for, by the bytes it moved: Q carried
     * forward by each handover, R fetched into the drawing slot once its copy had landed in the donor. */
    init(&priv);
    fakeState.presentStats.cpuCarryBytes = fakeState.presentStats.cpuOwedFetchBytes = 0;
    xDraw(&priv, Q, 5);                                     /* Q is 16 x 8 */
    CHECK(handover(&priv), "accounting: first publish");
    CHECK(fakeState.presentStats.cpuCarryBytes == 16 * 8 * 4, "accounting: %llu bytes carried for Q, not %d",
          (unsigned long long) fakeState.presentStats.cpuCarryBytes, 16 * 8 * 4);
    A = priv.rootWrite;
    enqueueRootCopy(&priv, R, 1);                           /* R is 16 x 8, pending into A */
    CHECK(handover(&priv), "accounting: second publish");
    CHECK(fakeState.presentStats.cpuCarryBytes == 2 * 16 * 8 * 4,
          "accounting: %llu bytes carried after the second handover, not Q twice (%d) - R was still pending",
          (unsigned long long) fakeState.presentStats.cpuCarryBytes, 2 * 16 * 8 * 4);
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == 0, "accounting: R fetched before its copy had landed");
    gpuLands(A, R, 1, 1);
    xDraw(&priv, Q, 6);                                     /* its PrepareAccess fetches R */
    CHECK(fakeState.presentStats.cpuOwedFetchBytes == 16 * 8 * 4, "accounting: %llu bytes fetched for R, not %d",
          (unsigned long long) fakeState.presentStats.cpuOwedFetchBytes, 16 * 8 * 4);
#endif

    printf("T05/T06 root handover: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
