/* Stats snapshot: one writer adds and raises maxima while another takes atomic snapshots. Nothing may be
 * lost or double counted, and a reset may never be undone by a stale maximum. LORIE_STAT_MAX is extracted
 * verbatim from lorie.h; the writer and reader use the same atomics as renderer.c and lorieFramecounter. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "tstat_src.inc"
static volatile uint32_t counter, maxv;
static volatile int done;
#define N 2000000u
static void *writer(void *arg) {
    (void) arg;
    for (uint32_t i = 1; i <= N; i++) {
        __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED);
        LORIE_STAT_MAX(&maxv, i);
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}
int main(void) {
    pthread_t t; uint64_t total = 0; int fails = 0, snaps = 0; uint32_t lastMax = 0;
    pthread_create(&t, NULL, writer, NULL);
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
        uint32_t c = __atomic_exchange_n(&counter, 0, __ATOMIC_RELAXED);
        uint32_t m = __atomic_exchange_n(&maxv, 0, __ATOMIC_RELAXED);
        total += c; snaps++;
        /* writer raises max with strictly increasing values, so a snapshot's max can never go backwards
         * unless a reset was overwritten by a value from before it */
        if (m && m < lastMax) { fails++; printf("  FAIL stale max %u after %u\n", m, lastMax); }
        if (m) lastMax = m;
    }
    pthread_join(t, NULL);
    total += __atomic_exchange_n(&counter, 0, __ATOMIC_RELAXED);
    if (total != N) { fails++; printf("  FAIL counted %llu of %u\n", (unsigned long long) total, N); }
    printf("stats snapshot (%d snapshots during %u updates): %s (%d failures)\n", snaps, N, fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
