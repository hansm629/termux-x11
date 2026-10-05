/* T21: vsync clock. Functions are extracted verbatim from InitOutput.c into t21_src.inc. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
struct { struct { struct { uint32_t vsyncRecordsLost; } presentStats; } *state; } fakePvfb;
static struct { struct { uint32_t vsyncRecordsLost; } presentStats; } fakeState;
#define pvfb (&fakePvfb)
#include "t21_src.inc"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void reset(void) {
#ifdef HAVE_VSYNC_SEQ
    memset((void *) lorieVsyncRecords, 0, sizeof lorieVsyncRecords);
#else
    for (int i = 0; i < LORIE_VSYNC_RECORDS; i++) lorieVsyncRecordUs[i] = 0;
#endif
    lorieVsyncProduced = 0; lorieVsyncConsumed = 0; lorieVsyncUs = 0; lorieVsyncPeriodUs = 16667;
    fakeState.presentStats.vsyncRecordsLost = 0;
}
int main(void) {
    fakePvfb.state = (void *) &fakeState;

    /* 1. backlog of work procs on a single callback: one tick, not ten */
    reset();
    lorieRecordVsync(1000000);
    uint32_t total = 0;
    for (int i = 0; i < 10; i++) total += lorieAdvanceVsyncClock();
    CHECK(total == 1, "one callback, ten redraws: msc moved %u, want 1", total);
    CHECK(lorieVsyncUs == 1000000, "ust %llu, want 1000000", (unsigned long long) lorieVsyncUs);

    /* 2. three ticks recorded while the X server was busy: the next redraw takes all three, dated by the
     * newest - whatever it does is done after that one. The redraws queued meanwhile take nothing. (This
     * used to want one tick per call, each with its own time: a redraw that never comes then leaves the
     * clock behind for good - tvblank.c.) */
    reset();
    lorieRecordVsync(1000000); lorieRecordVsync(1016667); lorieRecordVsync(1033334);
    uint32_t st2 = lorieAdvanceVsyncClock();
    CHECK(st2 == 3, "busy: steps %u, want 3", st2);
    CHECK(lorieVsyncUs == 1033334, "busy: ust %llu, want 1033334", (unsigned long long) lorieVsyncUs);
    CHECK(fakeState.presentStats.vsyncRecordsLost == 2, "busy: taken up late %u, want 2",
          fakeState.presentStats.vsyncRecordsLost);
    CHECK(lorieAdvanceVsyncClock() == 0 && lorieAdvanceVsyncClock() == 0, "the other two redraws take nothing");

    /* 3. ring overrun: 20 ticks unread with 16 records - all 20 counted, 19 times lost, newest time kept */
    reset();
    for (int i = 0; i < 20; i++) lorieRecordVsync(2000000 + (uint64_t) i * 16667);
    uint32_t st = lorieAdvanceVsyncClock();
    CHECK(st == 20, "overrun: steps %u, want 20", st);
    CHECK(fakeState.presentStats.vsyncRecordsLost == 19, "lost %u, want 19", fakeState.presentStats.vsyncRecordsLost);
    CHECK(lorieVsyncUs == 2000000 + 19ull * 16667, "overrun ust %llu", (unsigned long long) lorieVsyncUs);
    CHECK(lorieAdvanceVsyncClock() == 0, "nothing left after overrun");

    /* 4. period estimate follows a 120 Hz stream and ignores an implausible gap */
    reset();
    for (int i = 0; i < 64; i++) { lorieRecordVsync(3000000 + (uint64_t) i * 8333); lorieAdvanceVsyncClock(); }
    CHECK(lorieVsyncPeriodUs > 8200 && lorieVsyncPeriodUs < 8500, "120Hz period %llu", (unsigned long long) lorieVsyncPeriodUs);
    uint64_t before = lorieVsyncPeriodUs;
    lorieRecordVsync(lorieVsyncUs + 500000); lorieAdvanceVsyncClock();
    CHECK(lorieVsyncPeriodUs == before, "500 ms gap moved period to %llu", (unsigned long long) lorieVsyncPeriodUs);

    printf("T21 vsync clock: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
