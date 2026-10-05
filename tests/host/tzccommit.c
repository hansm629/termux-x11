/* TZCCOMMIT: OnCommit, measured only (renderer.c rootZcOnCommit, rootZcCommitNewLayer, rootZcNoteSubmitted,
 * rootZcFlushCommits, extracted by gen.py "zccommit"). What has to hold:
 *   - apply -> OnCommit falls in the right bucket (under 0.5, 1, 2, 4, 8, 16, 33 ms, longer), the longest kept;
 *   - each submit counts how many on the layer were not committed yet (none, 1, 2, 3 or more), the most kept,
 *     whatever order the callbacks come in;
 *   - a late OnCommit for a transaction of the layer before is not taken for one of this layer's;
 *   - callbacks beyond what is held between frames are counted as not recorded, and one whose apply time is
 *     no longer known as such - never as a latency;
 *   - the measurement's state is touched nowhere but in these functions: no frame decision reads it. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
typedef struct ASurfaceTransactionStats ASurfaceTransactionStats;
#define LORIE_TRACE_ZCCOMMIT 27
#define LORIE_STAT_MAX(ptr, value) do { if ((value) > *(ptr)) *(ptr) = (value); } while (0)
static struct {
    volatile uint8_t traceEnabled;
    struct { uint32_t zcCommits, zcCommitLatencyBuckets[8], zcCommitLatencyMaxUs, zcCommitUnmatched, zcCommitLost,
                      zcUncommittedAtSubmit[4], zcUncommittedMax; } presentStats;
} fakeState, *state = &fakeState;
static int traced, tracedLast[2];
static void lorieTraceAt(void *st, uint32_t kind, uint32_t a, uint64_t b, uint64_t tUs) {
    (void) st; (void) kind; (void) tUs; traced++; tracedLast[0] = (int) a; tracedLast[1] = (int) b;
}
static int64_t fakeNs;
static int64_t rendererNowNs(void) { return fakeNs; }
static uint32_t rootZcRetireSeq;
#include "zccommit_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void submit(uint32_t seq, int64_t atUs) { fakeNs = atUs * 1000; rootZcRetireSeq = seq; rootZcNoteSubmitted(seq); }
static void commit(uint32_t seq, int64_t atUs) { fakeNs = atUs * 1000; rootZcOnCommit((void *) (uintptr_t) seq, NULL); }
static void reset(void) { memset(&fakeState.presentStats, 0, sizeof fakeState.presentStats); rootZcCommitNewLayer(); }

int main(void) {
    /* 1. latencies into buckets; the depth at each submit */
    reset();
    fakeState.traceEnabled = 1;
    submit(1, 0);                                   /* none waiting */
    submit(2, 100);                                 /* 1 waiting */
    commit(1, 300);                                 /* 300 us */
    commit(2, 1100);                                /* 1000 us: the 2 ms bucket - under 1 ms means under */
    rootZcFlushCommits();
    submit(3, 2000);                                /* none waiting again */
    submit(4, 2100); submit(5, 2200); submit(6, 2300);   /* 1, 2, 3 waiting */
    commit(6, 40300); commit(4, 4200); commit(3, 2000 + 16000); commit(5, 2200 + 9000);   /* out of order */
    rootZcFlushCommits();
    uint32_t *b = (uint32_t *) fakeState.presentStats.zcCommitLatencyBuckets;
    /* 300 us, 1000 us, 2100 us, 9000 us, 16000 us, 38000 us: one each under 0.5, 2, 4, 16, 33 ms and longer */
    CHECK(b[0] == 1 && b[1] == 0 && b[2] == 1 && b[3] == 1 && b[4] == 0 && b[5] == 1 && b[6] == 1 && b[7] == 1,
          "buckets %u %u %u %u %u %u %u %u", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    CHECK(fakeState.presentStats.zcCommitLatencyMaxUs == 38000, "longest %u", fakeState.presentStats.zcCommitLatencyMaxUs);
    uint32_t *d = (uint32_t *) fakeState.presentStats.zcUncommittedAtSubmit;
    CHECK(d[0] == 2 && d[1] == 2 && d[2] == 1 && d[3] == 1 && fakeState.presentStats.zcUncommittedMax == 3,
          "depth at submit %u %u %u %u, most %u", d[0], d[1], d[2], d[3], fakeState.presentStats.zcUncommittedMax);
    CHECK(fakeState.presentStats.zcCommits == 6 && fakeState.presentStats.zcCommitUnmatched == 0, "%u commits, %u unmatched",
          fakeState.presentStats.zcCommits, fakeState.presentStats.zcCommitUnmatched);
    CHECK(traced == 6 && tracedLast[0] == 5 && tracedLast[1] == 9000, "traced %d, last %d %d", traced, tracedLast[0], tracedLast[1]);

    /* 2. a new layer; the old one's last transaction committed late - not one of the new layer's */
    reset();
    submit(10, 0);                                  /* old layer, never committed before the change */
    rootZcRetireSeq = 10; rootZcCommitNewLayer();
    submit(11, 1000);                               /* new layer: none of its own waiting */
    commit(10, 1500);                               /* the old one's, late */
    rootZcFlushCommits();
    submit(12, 2000);                               /* 11 still waiting: 1 */
    CHECK(fakeState.presentStats.zcUncommittedAtSubmit[0] == 2 && fakeState.presentStats.zcUncommittedAtSubmit[1] == 1,
          "after a layer change: depth %u %u", fakeState.presentStats.zcUncommittedAtSubmit[0],
          fakeState.presentStats.zcUncommittedAtSubmit[1]);

    /* 3. more callbacks than are held between frames; and one whose apply time was written over */
    reset();
    for (uint32_t s = 100; s < 100 + LORIE_ZC_COMMITS + 5; s++) { submit(s, s); commit(s, s + 10); }
    rootZcFlushCommits();
    CHECK(fakeState.presentStats.zcCommits == LORIE_ZC_COMMITS && fakeState.presentStats.zcCommitLost == 5,
          "%u recorded, %u lost", fakeState.presentStats.zcCommits, fakeState.presentStats.zcCommitLost);
    reset();
    submit(200, 0);
    submit(200 + LORIE_ZC_COMMITS, 10);             /* the same place in the apply times */
    commit(200, 500);
    rootZcFlushCommits();
    CHECK(fakeState.presentStats.zcCommitUnmatched == 1 && fakeState.presentStats.zcCommitLatencyMaxUs == 0,
          "a commit without its apply time: %u unmatched, longest %u", fakeState.presentStats.zcCommitUnmatched,
          fakeState.presentStats.zcCommitLatencyMaxUs);

    /* 4. nothing but these functions touches what they keep */
#define SAME(n) CHECK(USES_##n##_IN_FILE == USES_##n##_MEASURED, #n " used %d times in renderer.c, %d of them in the measurement", USES_##n##_IN_FILE, USES_##n##_MEASURED)
    SAME(rootZcCommitSeen); SAME(rootZcApplyTimes); SAME(rootZcCommitFirstSeq); SAME(rootZcSubmittedOnLayer);
    SAME(rootZcCommittedOnLayer);

    printf("TZCCOMMIT OnCommit measured, nothing decided on it: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
