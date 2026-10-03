/* T07: outcome vs GPU use-complete, and the failed list wrapping. Functions extracted verbatim. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define LORIE_GPU_COPY_FAILED_SLOTS_FOR_STATE 16
struct fakeShared {
    struct {
        volatile uint64_t completedSerial;
        volatile uint64_t failedSerials[LORIE_GPU_COPY_FAILED_SLOTS_FOR_STATE];
        volatile uint32_t failedCount;
        volatile uint64_t failedLostUpTo;
        volatile uint32_t readIndex, writeIndex;
        struct { uint64_t serial; } entries[8];
    } gpuCopyQueue;
    /* what the session bookkeeping reads; these tests stay within one live connection */
    volatile uint32_t sessionTag;
    struct { volatile uint32_t seq, session; volatile int32_t pid; volatile uint64_t serial; } retired[4];
    volatile uint32_t retiredClaim;
};
static struct fakeShared shared;
static struct fakeShared *state = &shared;                 /* renderer's view */
static struct { struct fakeShared *state; uint64_t gpuCopySerialCounter; } fakePvfb = { &shared, 0 };
#define pvfb (&fakePvfb)                                    /* X server's view */
static uint64_t lorieNowUs(void) { return 0; }
static Bool lorieCancelQueuedEntry(uint32_t slot) { (void) slot; return FALSE; }
static void lorieRootCopyCancelled(uint64_t serial) { (void) serial; }
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#define log(prio, ...) ((void) 0)
#define PROP_VALUE_MAX 92
#define __system_property_get(name, value) 0
#define kill(pid, sig) (-1)
#define close(fd) ((void) 0)
#include "t07_src.inc"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void reset(void) {
    __builtin_memset(&shared, 0, sizeof shared);
#ifdef HAVE_SESSIONS
    /* one live connection, which every serial here belongs to */
    lorieRendererSession = 1;
#ifdef HAVE_SESSION_LIST
    static LorieRendererSessionRec live;
    __builtin_memset(&live, 0, sizeof live);
    live.id = 1;
    live.firstSerial = 1;
    lorieSessions = &live;
    (void) lorieUnmadeSize;
#else
    __builtin_memset(lorieSessions, 0, sizeof lorieSessions);
    lorieSessions[1].id = 1;
    lorieSessions[1].firstSerial = 1;
#endif
#endif
}

int main(void) {
    /* a. serial 7 skipped while 5 and 6 of the same batch are still on the GPU: the outcome is known
     *    (not made) but the GPU is not finished with the batch, so nothing may be released yet */
    reset();
    shared.gpuCopyQueue.completedSerial = 4;
    rendererPublishFailedSerial(7);
    CHECK(!lorieGpuCopyMade(7), "skipped serial reported as made");
    CHECK(!lorieGpuCopyResolved(7), "skip treated as GPU use-complete while the batch is still running");
    CHECK(!lorieGpuCopyResolved(5), "earlier entry of the batch treated as finished");

    /* b. the batch fence signalled and the watermark moved past it */
    shared.gpuCopyQueue.completedSerial = 7;
    CHECK(lorieGpuCopyResolved(7) && !lorieGpuCopyMade(7), "after fence: resolved, not made");
    CHECK(lorieGpuCopyResolved(5) && lorieGpuCopyMade(5), "after fence: 5 resolved and made");

    /* c. the failed list wraps: 20 failures into 16 slots. The four pushed out must never read as made. */
    reset();
    for (uint64_t s = 1; s <= 20; s++) rendererPublishFailedSerial(s);
    shared.gpuCopyQueue.completedSerial = 20;
    for (uint64_t s = 1; s <= 20; s++)
        CHECK(!lorieGpuCopyMade(s), "failed serial %llu read as made after the list wrapped", (unsigned long long) s);
    CHECK(shared.gpuCopyQueue.failedLostUpTo == 4, "lost watermark %llu, want 4",
          (unsigned long long) shared.gpuCopyQueue.failedLostUpTo);

    /* d. a later success beyond the lost range still acks */
    shared.gpuCopyQueue.completedSerial = 21;
    CHECK(lorieGpuCopyMade(21), "successful serial 21 not acked");

    printf("T07 outcome vs use-complete, failed list wrap: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
