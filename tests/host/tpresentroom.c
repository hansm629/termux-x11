/* A present the GPU path has no room for - the queue full, the root's owed-area records full, no copy
 * record free - held back for the renderer to make some (lorieMakeRoomForPresent, lorieWaitForRenderer,
 * extracted from InitOutput.c by gen.py) rather than drawn by the CPU at once. What has to hold:
 *   - with room, nothing waits and the record is taken;
 *   - without, the renderer is waited for, and the present goes to the GPU as soon as it makes room;
 *   - the wait is bounded (TERMUX_X11_PRESENT_ROOM_WAIT_US), and not entered at all where it cannot
 *     help: an access open (the shared lock held), no renderer, or the wait turned off;
 *   - refused, nothing is taken: no record held, and the reason is the one that was missing. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <linux/futex.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define min(a, b) (((a) < (b)) ? (a) : (b))
struct xorg_list { struct xorg_list *next, *prev; };
typedef struct _LorieBuffer LorieBuffer;
typedef struct _Pixmap *PixmapPtr;
typedef struct _Window *WindowPtr;
typedef uint32_t CARD32;
struct present_fence;
#include "proom_types.inc"
static struct {
    struct { volatile uint32_t writeIndex, readIndex, readIndexWaiters; volatile uint64_t completedSerial; } gpuCopyQueue;
    struct { uint32_t presentRoomWaits, presentRoomWaitUs, presentRoomMade, rootReplacingFull, copyRecordExhausted; } presentStats;
} fakeState;
static struct { typeof(fakeState) *state; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)

/* time, and the renderer: it moves only while the X server sleeps on it (the futex), one step a sleep */
static uint64_t nowUs = 1000;
static uint64_t lorieNowUs(void) { return nowUs += 10; }
static Bool connected = TRUE, surface = TRUE, stuck, giveRoomOnStep;
static int lorieSharedLockHeld, steps, replacingRoom = TRUE, rendererCond;
#define pthread_cond_signal(c) ((void) (c))
static Bool lorieConnectionAlive(void) { return connected; }
static Bool lorieRendererAvailable(void) { return surface; }
typedef struct { int unused; } LoriePixmapPriv;
static Bool lorieRootCanQueueCopy(LoriePixmapPriv *priv) { (void) priv; return replacingRoom; }
static void lorieReapAbandonedCopies(void);
static long harnessFutex(void) {
    nowUs += 300;
    if (stuck)
        return 0;
    steps++;
    if (fakeState.gpuCopyQueue.readIndex != fakeState.gpuCopyQueue.writeIndex)
        fakeState.gpuCopyQueue.readIndex++;
    fakeState.gpuCopyQueue.completedSerial++;
    if (giveRoomOnStep)
        replacingRoom = TRUE;
    return 0;
}
#define syscall(nr, addr, op, val, timeout, ...) ((void) (timeout), harnessFutex())
#include "proom_funcs.inc"
/* records the renderer has finished with, given back as lorieReapAbandonedCopies does: `abandoned` ones
 * once it has moved on */
static int abandoned[LORIE_COPY_RECORDS];
static void lorieReapAbandonedCopies(void) {
    for (int i = 0; i < LORIE_COPY_RECORDS; i++)
        if (abandoned[i] && steps > 0) { lorieCopyRecords[i].inUse = FALSE; abandoned[i] = 0; }
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int inUse(void) { int n = 0; for (int i = 0; i < LORIE_COPY_RECORDS; i++) n += lorieCopyRecords[i].inUse; return n; }
static void reset(void) {
    memset(&fakeState, 0, sizeof fakeState);
    memset(lorieCopyRecords, 0, sizeof lorieCopyRecords);
    memset(abandoned, 0, sizeof abandoned);
    connected = surface = replacingRoom = TRUE;
    stuck = giveRoomOnStep = FALSE;
    lorieSharedLockHeld = steps = 0;
}
static void fillQueue(void) { fakeState.gpuCopyQueue.writeIndex = fakeState.gpuCopyQueue.readIndex + LORIE_GPU_COPY_QUEUE_CAPACITY; }

int main(void) {
    LoriePixmapPriv root;
    LorieAbandonedCopy *record;
    uint64_t start;

    /* room: no wait, the record taken */
    reset();
    record = NULL;
    CHECK(lorieMakeRoomForPresent(&root, &record) == -1 && record && record->inUse && inUse() == 1 &&
          fakeState.presentStats.presentRoomWaits == 0, "with room: waited, or no record");

    /* the queue full, the renderer taking an entry: to the GPU after one wait */
    reset();
    fillQueue();
    CHECK(lorieMakeRoomForPresent(&root, &record) == -1 && inUse() == 1 && fakeState.presentStats.presentRoomWaits >= 1 &&
          fakeState.presentStats.presentRoomMade == 1, "queue full, renderer moving: not to the GPU");

    /* ... the renderer stuck: the CPU's, after the wait, with nothing taken */
    reset();
    fillQueue();
    stuck = TRUE;
    start = nowUs;
    CHECK(lorieMakeRoomForPresent(&root, &record) == LORIE_CPU_PRESENT_QUEUE_FULL && inUse() == 0,
          "queue full, renderer stuck: not the CPU's, or a record kept");
    CHECK(nowUs - start >= LORIE_PRESENT_ROOM_WAIT_DEFAULT_US && nowUs - start < LORIE_PRESENT_ROOM_WAIT_DEFAULT_US + 1000 &&
          fakeState.presentStats.presentRoomMade == 0, "queue full, renderer stuck: waited %llu us",
          (unsigned long long) (nowUs - start));

    /* where waiting cannot help, not waited for: an access open, no renderer, no surface */
    for (int k = 0; k < 3; k++) {
        reset();
        fillQueue();
        if (k == 0) lorieSharedLockHeld = 1;
        if (k == 1) connected = FALSE;
        if (k == 2) surface = FALSE;
        start = nowUs;
        CHECK(lorieMakeRoomForPresent(&root, &record) == LORIE_CPU_PRESENT_QUEUE_FULL &&
              fakeState.presentStats.presentRoomWaits == 0 && steps == 0 && nowUs - start < 100,
              "%s: waited", k == 0 ? "lock held" : k == 1 ? "no renderer" : "no surface");
    }

    /* the root's owed-area records full: waited for the renderer to resolve one; or, stuck, the CPU's */
    reset();
    replacingRoom = FALSE;
    giveRoomOnStep = TRUE;
    CHECK(lorieMakeRoomForPresent(&root, &record) == -1 && inUse() == 1, "owed-area records full: not to the GPU once resolved");
    reset();
    replacingRoom = FALSE;
    stuck = TRUE;
    CHECK(lorieMakeRoomForPresent(&root, &record) == LORIE_CPU_PRESENT_REPLACING_FULL && inUse() == 0 &&
          fakeState.presentStats.rootReplacingFull == 1, "owed-area records full, renderer stuck: not the CPU's");
    /* not into the root: the root's records do not matter */
    CHECK(lorieMakeRoomForPresent(NULL, &record) == -1, "not into the root: held back for the root's records");

    /* every copy record taken: given back as the renderer finishes the copies holding them */
    reset();
    for (int i = 0; i < LORIE_COPY_RECORDS; i++) { lorieCopyRecords[i].inUse = TRUE; abandoned[i] = i % 2; }
    CHECK(lorieMakeRoomForPresent(&root, &record) == -1 && record, "records all taken, some being finished: no record");
    /* ... all of them held by presents, which only the X server gives back: the CPU's */
    reset();
    for (int i = 0; i < LORIE_COPY_RECORDS; i++) lorieCopyRecords[i].inUse = TRUE;
    CHECK(lorieMakeRoomForPresent(&root, &record) == LORIE_CPU_PRESENT_NO_RECORD &&
          fakeState.presentStats.copyRecordExhausted == 1, "records all held: not the CPU's");

    printf("present held back for room: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
