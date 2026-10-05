/* What the renderer makes of the compositor's answers and of the publishes it applies (renderer.c,
 * extracted by gen.py: rootZcNoteCompletion, rootZcFlushDisplayStats, rootZcFenceSignalledNs,
 * rootZcNoteApplied). What has to hold:
 *   - a present fence call that is there but gives nothing (-1) is counted as that, so "nothing
 *     presented" is never a measurement; a missing call is reported as missing, a fence that cannot be
 *     read as lost;
 *   - equal latch times are counted as equal, gaps over 1.5 refresh periods as such, the period read
 *     from what the renderer published, not from the callback's thread;
 *   - completions beyond what the record holds are counted as lost, not dropped silently;
 *   - publishes are counted by their full sequence: skipped ones, the same one applied again, across a
 *     wrap of the count - and not the way 8 bits of the handover word gave 254 skipped for a re-apply. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/sync_file.h>
typedef struct ASurfaceControl ASurfaceControl;
typedef struct ASurfaceTransaction ASurfaceTransaction;
typedef struct ASurfaceTransactionStats { int64_t latch; int fence; } ASurfaceTransactionStats;
typedef struct ANativeWindow ANativeWindow;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct ARect ARect;
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define LORIE_TRACE_SFDONE 25
#define LORIE_TRACE_SFPRESENT 26
static pthread_mutex_t rootOverlayLock = PTHREAD_MUTEX_INITIALIZER;
static int64_t fakeNs = 1000000;
static int64_t rendererNowNs(void) { return fakeNs; }
static volatile int64_t rendererDisplayPeriodNs = 16666667;
static struct {
    volatile uint8_t traceEnabled;
    struct { uint32_t directPublishesSkipped, directReapplied, sfCompletions, sfLatches, sfSameLatch, sfLatchLateGaps,
                      sfLatchGapMaxUs, sfPresents, sfPresentLateGaps, sfPresentGapMaxUs, sfStatsLost, sfStatsMissing,
                      sfNoPresentFence; } presentStats;
} fakeState, *state = &fakeState;
static int traced[64][2], tracedCount;
static void lorieTraceAt(void *st, uint32_t kind, uint32_t a, uint64_t b, uint64_t tUs) {
    (void) st; (void) b; (void) tUs;
    if (tracedCount < 64) { traced[tracedCount][0] = (int) kind; traced[tracedCount++][1] = (int) a; }
}
#define LORIE_STAT_MAX(ptr, value) do { if ((value) > *(ptr)) *(ptr) = (value); } while (0)
#include "sfstats_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int64_t fakeLatch(ASurfaceTransactionStats *s) { return s->latch; }
static int fakePresent(ASurfaceTransactionStats *s) { return s->fence; }
static void reset(void) { memset(&fakeState.presentStats, 0, sizeof fakeState.presentStats); tracedCount = 0; }

int main(void) {
    ASurfaceTransactionStats st;
    int p[2];

    /* the calls there; no present fence given, latches 16.7 ms apart, one equal, one gap of two periods */
    scApi.statsLatchTime = fakeLatch;
    scApi.statsPresentFenceFd = fakePresent;
    fakeState.traceEnabled = 1;
    reset();
    int64_t latches[] = { 100000000, 116666667, 116666667, 133333333, 166666667 };
    for (int i = 0; i < 5; i++) {
        st = (ASurfaceTransactionStats) { latches[i], -1 };
        rootZcNoteCompletion(10 + (uint32_t) i, &st);
    }
    rootZcFlushDisplayStats();
    CHECK(fakeState.presentStats.sfCompletions == 5 && fakeState.presentStats.sfNoPresentFence == 5 &&
          fakeState.presentStats.sfPresents == 0 && !(fakeState.presentStats.sfStatsMissing & 2),
          "present fence given as -1: not counted as no fence (%u), or reported as a missing call",
          fakeState.presentStats.sfNoPresentFence);
    CHECK(fakeState.presentStats.sfLatches == 4 && fakeState.presentStats.sfSameLatch == 1 &&
          fakeState.presentStats.sfLatchLateGaps == 1 && fakeState.presentStats.sfLatchGapMaxUs == 33333,
          "latches %u, equal %u, late gaps %u, longest %u us", fakeState.presentStats.sfLatches,
          fakeState.presentStats.sfSameLatch, fakeState.presentStats.sfLatchLateGaps, fakeState.presentStats.sfLatchGapMaxUs);
    CHECK(tracedCount == 5 && traced[0][0] == LORIE_TRACE_SFDONE && traced[4][1] == 14, "completions not traced by their number");

    /* at 120 Hz (the renderer keeps the last latch: these go on from 166.7 ms) a 10 ms gap is on time, a
     * two-period one late */
    reset();
    rendererDisplayPeriodNs = 8333333;
    int64_t fast[] = { 175000000, 185000000, 201666667 };
    for (int i = 0; i < 3; i++) { st = (ASurfaceTransactionStats) { fast[i], -1 }; rootZcNoteCompletion(20, &st); }
    rootZcFlushDisplayStats();
    CHECK(fakeState.presentStats.sfLatchLateGaps == 1, "120 Hz: %u late gaps, the period not the renderer's",
          fakeState.presentStats.sfLatchLateGaps);
    rendererDisplayPeriodNs = 16666667;

    /* a fence that cannot be read: counted lost, and closed */
    reset();
    CHECK(pipe(p) == 0 && write(p[1], "x", 1) == 1, "pipe");
    st = (ASurfaceTransactionStats) { 300000000, p[0] };
    rootZcNoteCompletion(30, &st);
    rootZcFlushDisplayStats();
    CHECK(fakeState.presentStats.sfStatsLost == 1 && fakeState.presentStats.sfPresents == 0 &&
          fcntl(p[0], F_GETFD) == -1, "an unreadable fence not counted as lost, or left open");
    close(p[1]);

    /* more completions than the record holds before the renderer gets to them */
    reset();
    for (int i = 0; i < LORIE_ZC_SEEN + 4; i++) { st = (ASurfaceTransactionStats) { 400000000 + i, -1 }; rootZcNoteCompletion(40, &st); }
    rootZcFlushDisplayStats();
    CHECK(fakeState.presentStats.sfCompletions == LORIE_ZC_SEEN + 4 && fakeState.presentStats.sfStatsLost == 4,
          "overflow: %u lost", fakeState.presentStats.sfStatsLost);

    /* the calls missing: reported missing, not counted as fences that were not given */
    reset();
    scApi.statsLatchTime = NULL;
    scApi.statsPresentFenceFd = NULL;
    st = (ASurfaceTransactionStats) { 500000000, -1 };
    rootZcNoteCompletion(50, &st);
    rootZcFlushDisplayStats();
    CHECK(fakeState.presentStats.sfStatsMissing == 3 && fakeState.presentStats.sfNoPresentFence == 0 &&
          fakeState.presentStats.sfLatches == 0, "missing calls: %u", fakeState.presentStats.sfStatsMissing);

    /* publishes by their full sequence (the renderer keeps the last one applied, as in a session) */
    reset();
    uint32_t seqs[] = { 7, 8, 9, 12, 12, 13 };
    for (unsigned i = 0; i < sizeof seqs / sizeof seqs[0]; i++)
        rootZcNoteApplied(seqs[i]);
    /* 9 -> 12 skips 2; 12 again is an apply again */
    CHECK(fakeState.presentStats.directReapplied == 1, "applied again: %u", fakeState.presentStats.directReapplied);
    CHECK(fakeState.presentStats.directPublishesSkipped == 2, "skipped: %u", fakeState.presentStats.directPublishesSkipped);
    rootZcNoteApplied(0xffffffefu);      /* where the count stood before the wrap below */
    reset();
    for (uint32_t s = 0xfffffff0u; s != 0x10u; s++) rootZcNoteApplied(s);
    CHECK(fakeState.presentStats.directPublishesSkipped == 0 && fakeState.presentStats.directReapplied == 0,
          "across the wrap: %u skipped, %u again", fakeState.presentStats.directPublishesSkipped,
          fakeState.presentStats.directReapplied);

    /* the 8-bit field of the handover word, as ec47f31 counted it: the same publish applied again, with
     * the held and newest bits lower than the last time, borrowed from the count and read as 255 */
    {
        uint32_t last = (5u << 8) | (3u << 5) | 0x3u, now = (5u << 8) | (3u << 5) | 0x1u;
        uint32_t old = ((now - last) & 0xff00u) / 0x100u;
        CHECK(old == 255, "the old count was %u, the borrow not reproduced", old);
        rootZcNoteApplied(41);
        reset();
        rootZcNoteApplied(42);
        rootZcNoteApplied(42);
        CHECK(fakeState.presentStats.directPublishesSkipped == 0 && fakeState.presentStats.directReapplied == 1,
              "the same publish again: %u skipped", fakeState.presentStats.directPublishesSkipped);
    }

    printf("compositor answers and applied publishes counted: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
