/* T29: the vsync record ring with a real producer and consumer thread. lorieRecordVsync,
 * lorieAdvanceVsyncClock and the ring are extracted verbatim from InitOutput.c, with the ring cut to 4
 * records so the producer laps it constantly (gen.py). The producer stamps tick n with a value both
 * halves of which change with n, so any stamp the consumer sees can be traced back to the tick it
 * came from - a torn read, or a later lap's stamp read for an earlier tick, does not decode to the
 * tick the clock moved to. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
static struct { struct { volatile uint32_t vsyncRecordsLost; } presentStats; } fakeState;
static struct { typeof(fakeState) *state; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
#include "t29_src.inc"

#define TICKS 3000000u
static uint64_t stampOf(uint32_t n) { return ((uint64_t) n << 32) | (uint32_t) (n * 2654435761u); }
static Bool tickOf(uint64_t us, uint32_t *n) {
    *n = (uint32_t) (us >> 32);
    return stampOf(*n) == us;
}
static volatile int producing = 1;
static void *producer(void *arg) {
    (void) arg;
    for (uint32_t n = 0; n < TICKS; n++)
        lorieRecordVsync(stampOf(n));
    producing = 0;
    return NULL;
}

int main(void) {
    pthread_t th;
    uint64_t steps = 0, calls = 0, lostCounted = 0;
    uint32_t torn = 0, mismatched = 0, backwards = 0, future = 0;
    uint64_t lastUs = 0;
    int fails = 0;

    pthread_create(&th, NULL, producer, NULL);
    for (;;) {
        int done = !producing;
        uint32_t st = lorieAdvanceVsyncClock();
        if (st) {
            uint32_t n;
            calls++;
            steps += st;
            lostCounted += st - 1;
            if (!tickOf(lorieVsyncUs, &n))
                torn++;                                     /* not a stamp the producer ever wrote */
            else if (n != steps - 1)
                mismatched++;                               /* a stamp, but not the tick the clock moved to */
            if (n >= __atomic_load_n(&lorieVsyncProduced, __ATOMIC_ACQUIRE))
                future++;                                   /* a tick that had not been produced yet */
            if (lorieVsyncUs < lastUs)
                backwards++;
            lastUs = lorieVsyncUs;
        } else if (done)
            break;
    }
    pthread_join(th, NULL);

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
    CHECK(torn == 0, "%u torn stamps", torn);
    CHECK(mismatched == 0, "%u stamps from a different tick than the one the clock moved to", mismatched);
    CHECK(future == 0, "%u stamps of ticks not produced yet", future);
    CHECK(backwards == 0, "%u stamps going backwards", backwards);
    CHECK(steps == TICKS, "msc moved %llu for %u ticks", (unsigned long long) steps, TICKS);
    CHECK(fakeState.presentStats.vsyncRecordsLost == lostCounted,
          "lost counter %u, ticks taken without their own stamp %llu",
          fakeState.presentStats.vsyncRecordsLost, (unsigned long long) lostCounted);
    printf("T29 vsync ring, concurrent producer/consumer: %s (%d failures; %llu ticks over %llu reads, %llu without their own stamp)\n",
           fails ? "FAIL" : "PASS", fails, (unsigned long long) steps, (unsigned long long) calls,
           (unsigned long long) lostCounted);
    return fails != 0;
}
