/* Trace ring: several threads write at once (as the X server, input and renderer threads do) while the
 * X server's reader drains to a file. Every record must come out exactly once, intact, or be counted as
 * dropped - never torn, never twice. lorieTraceAt (lorie.h) and lorieTraceFlush (InitOutput.c) are the
 * real ones, extracted by gen.py. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#ifndef __always_inline
#define __always_inline inline
#endif
#include "ttrace_src_types.inc"
static struct lorie_shared_server_state shared;
static struct { struct lorie_shared_server_state *state; } fakePvfb = { &shared };
#define pvfb (&fakePvfb)
#include "ttrace_src_funcs.inc"

#define WRITERS 3
#define PER_WRITER 200000u
static volatile int running = 1;
static void *writer(void *arg) {
    uint32_t w = (uint32_t) (uintptr_t) arg;
    for (uint32_t i = 0; i < PER_WRITER; i++)
        lorieTraceAt(&shared, 100 + w, i, ((uint64_t) w << 32) | i, lorieTraceNowUs());
    return NULL;
}
int main(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/ttrace.bin", getenv("OUT") ? getenv("OUT") : ".");
    lorieTraceFile = fopen(path, "wb");
    shared.traceEnabled = 1;
    pthread_t t[WRITERS];
    for (int w = 0; w < WRITERS; w++) pthread_create(&t[w], NULL, writer, (void *) (uintptr_t) w);
    for (int i = 0; i < 4000; i++) { lorieTraceFlush(TRUE); struct timespec d = {0, 50000}; nanosleep(&d, NULL); }
    for (int w = 0; w < WRITERS; w++) pthread_join(t[w], NULL);
    lorieTraceFlush(TRUE);
    fclose(lorieTraceFile);

    FILE *f = fopen(path, "rb");
    struct { uint64_t tUs; uint32_t kind; uint32_t a; uint64_t b; } r;
    uint64_t got = 0; int fails = 0;
    int64_t last[WRITERS] = {-1, -1, -1};
    while (fread(&r, sizeof r, 1, f) == 1) {
        uint32_t w = r.kind - 100;
        got++;
        if (w >= WRITERS || (r.b >> 32) != w || (uint32_t) r.b != r.a) { fails++; if (fails < 5) printf("  FAIL torn record kind %u a %u b %llx\n", r.kind, r.a, (unsigned long long) r.b); continue; }
        if ((int64_t) r.a <= last[w]) { fails++; if (fails < 5) printf("  FAIL writer %u record %u out of order or twice\n", w, r.a); }
        last[w] = r.a;
    }
    fclose(f);
    uint64_t total = (uint64_t) WRITERS * PER_WRITER;
    if (got + shared.traceDropped != total) { fails++; printf("  FAIL written %llu + dropped %u != produced %llu\n", (unsigned long long) got, shared.traceDropped, (unsigned long long) total); }
    printf("trace ring (%d writers, %llu written, %u dropped): %s (%d failures)\n", WRITERS,
           (unsigned long long) got, shared.traceDropped, fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
