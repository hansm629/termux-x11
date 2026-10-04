/* The queue end of a handover's carry on the GPU (InitOutput.c, extracted by gen.py):
 *   lorieRootCarryAllowed - only with a renderer, and only into the first half of the queue and of the
 *     copy records, so a client's present is never turned down for a carry;
 *   lorieQueueRootSlotCopy - one copy from slot to slot: the entry as the renderer reads it, the buffers
 *     held and marked busy, the record handed to the reaper, the session owing its answer;
 *   lorieQueueHoldsOnlyCarries - what lets an EXA fallback skip waiting on the renderer. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef uint32_t CARD32;
#define min(a, b) (((a) < (b)) ? (a) : (b))
struct xorg_list { struct xorg_list *next, *prev; };
static void xorg_list_add(struct xorg_list *e, struct xorg_list *head) {
    e->next = head->next; e->prev = head; head->next->prev = e; head->next = e;
}
typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { uint64_t id; } LorieBuffer_Desc;
typedef struct _LorieBuffer { LorieBuffer_Desc desc; int refs, pending; } LorieBuffer;
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }
static void LorieBuffer_acquire(LorieBuffer *b) { b->refs++; }
static void LorieBuffer_gpuCopyPendingInc(LorieBuffer *b) { b->pending++; }
typedef struct _Pixmap *PixmapPtr;
typedef struct _Window *WindowPtr;
struct present_fence;
#include "tcarryq_types.inc"
typedef struct { LorieBuffer *rootBuf[LORIE_ROOT_SLOTS]; } LoriePixmapPriv;
static struct {
    struct { uint32_t writeIndex, readIndex; LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
             uint32_t entryState[LORIE_GPU_COPY_QUEUE_CAPACITY]; } gpuCopyQueue;
    struct { uint32_t cpuCarryKept[LORIE_CARRY_KEPT_REASONS]; } presentStats;
    uint8_t rootDoubleBuffered;
    uint64_t rootBufferIds[LORIE_ROOT_SLOTS];
} fakeState;
static struct { typeof(fakeState) *state; Bool gpuPresentDisabled; struct { Bool legacyDrawing; } root;
                uint64_t gpuCopySerialCounter; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
static Bool connected = TRUE, surface = TRUE;
static Bool lorieConnectionAlive(void) { return connected; }
static Bool lorieRendererAvailable(void) { return surface; }
static int signals;
static int rendererCond;
#define pthread_cond_signal(c) ((void) (c), signals++)
typedef struct { uint32_t outstanding; } LorieRendererSessionRec;
static LorieRendererSessionRec session7;
static uint32_t lorieRendererSession = 7;
static LorieRendererSessionRec *lorieSessionById(uint32_t id) { return id == 7 ? &session7 : NULL; }
static uint64_t lorieNowUs(void) { return 1234; }
#define lorieTrace(...) do {} while (0)
static struct xorg_list lorieAbandonedCopies = { &lorieAbandonedCopies, &lorieAbandonedCopies };
#include "tcarryq_funcs.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int listed(void) { int n = 0; for (struct xorg_list *e = lorieAbandonedCopies.next; e != &lorieAbandonedCopies; e = e->next) n++; return n; }

int main(void) {
    LorieBuffer bufs[LORIE_ROOT_SLOTS];
    LoriePixmapPriv priv;
    BoxRec boxes[2] = { { 1, 2, 30, 40 }, { 50, 60, 70, 80 } };
    uint32_t kept[LORIE_CARRY_KEPT_REASONS];

    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) {
        bufs[i] = (LorieBuffer) { { 100 + (uint64_t) i }, 1, 0 };
        priv.rootBuf[i] = &bufs[i];
        fakeState.rootBufferIds[i] = 100 + (uint64_t) i;
    }
    fakeState.rootDoubleBuffered = 1;

    /* allowed: only with a renderer, the GPU path on, and room left for presents */
    CHECK(lorieRootCarryAllowed(2), "refused with an empty queue and every record free");
    connected = FALSE;
    memcpy(kept, fakeState.presentStats.cpuCarryKept, sizeof kept);
    CHECK(!lorieRootCarryAllowed(1), "allowed with no renderer connected");
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_NO_RENDERER] == kept[LORIE_CARRY_KEPT_NO_RENDERER] + 1,
          "no renderer not counted as why");
    connected = TRUE; surface = FALSE;
    CHECK(!lorieRootCarryAllowed(1), "allowed with the renderer's surface gone");
    surface = TRUE; fakePvfb.gpuPresentDisabled = TRUE;
    CHECK(!lorieRootCarryAllowed(1), "allowed with GPU presents turned off");
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_OFF] == 1, "off not counted as why");
    fakePvfb.gpuPresentDisabled = FALSE;
    fakeState.gpuCopyQueue.writeIndex = 3;                        /* three presents queued */
    CHECK(lorieRootCarryAllowed(1), "one more into half the queue refused");
    CHECK(!lorieRootCarryAllowed(2), "allowed past half the queue - a present could be turned down for it");
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_BUSY] == 1, "busy not counted as why");
    fakeState.gpuCopyQueue.writeIndex = fakeState.gpuCopyQueue.readIndex = 0;
    for (int i = 0; i < LORIE_COPY_RECORDS / 2; i++)
        lorieCopyRecords[i].inUse = TRUE;
    CHECK(!lorieRootCarryAllowed(1), "allowed into the half of the records kept for presents");
    for (int i = 0; i < LORIE_COPY_RECORDS / 2; i++)
        lorieCopyRecords[i].inUse = FALSE;

    /* one copy from slot 1 into slot 3 */
    fakePvfb.gpuCopySerialCounter = 41;
    uint64_t s = lorieQueueRootSlotCopy(&priv, 1, 3, boxes, 2);
    LorieGpuCopyEntry *e = &fakeState.gpuCopyQueue.entries[0];
    CHECK(s == 42 && fakePvfb.gpuCopySerialCounter == 42, "serial %llu", (unsigned long long) s);
    CHECK(fakeState.gpuCopyQueue.writeIndex == 1 && signals == 1, "not published to the renderer");
    CHECK(e->serial == 42 && e->srcBufferId == 101 && e->dstBufferId == 103 && e->xOff == 0 && e->yOff == 0,
          "entry names %llu -> %llu at %d,%d", (unsigned long long) e->srcBufferId,
          (unsigned long long) e->dstBufferId, e->xOff, e->yOff);
    CHECK(e->numRects == 2 && e->rects[0].x1 == 1 && e->rects[0].y2 == 40 && e->rects[1].x1 == 50 && e->rects[1].y2 == 80,
          "rects not as given");
    CHECK(fakeState.gpuCopyQueue.entryState[0] == LORIE_JOB_QUEUED, "entry not queued");
    CHECK(bufs[1].refs == 2 && bufs[1].pending == 1 && bufs[3].refs == 2 && bufs[3].pending == 1,
          "slots not held for the copy: src %d/%d dst %d/%d", bufs[1].refs, bufs[1].pending, bufs[3].refs, bufs[3].pending);
    CHECK(listed() == 1, "record not handed to the reaper");
    LorieAbandonedCopy *r = (LorieAbandonedCopy *) lorieAbandonedCopies.next;
    CHECK(r->serial == 42 && r->session == 7 && r->src == &bufs[1] && r->dst == &bufs[3] && r->inUse,
          "record does not say what it holds");
    CHECK(session7.outstanding == 1, "the session does not owe the answer");

    /* no room: nothing taken, nothing held */
    fakeState.gpuCopyQueue.writeIndex = fakeState.gpuCopyQueue.readIndex + LORIE_GPU_COPY_QUEUE_CAPACITY;
    CHECK(lorieQueueRootSlotCopy(&priv, 1, 3, boxes, 2) == 0, "queued into a full queue");
    CHECK(bufs[1].refs == 2 && bufs[3].pending == 1 && listed() == 1, "a refused copy still took references");
    fakeState.gpuCopyQueue.writeIndex = fakeState.gpuCopyQueue.readIndex = 1;
    CHECK(lorieQueueRootSlotCopy(&priv, 1, 3, boxes, LORIE_GPU_COPY_MAX_RECTS + 1) == 0, "more rects than an entry holds");

    /* the fallback skips waiting only when everything queued is a carry */
    fakeState.gpuCopyQueue.readIndex = 0; fakeState.gpuCopyQueue.writeIndex = 2;
    fakeState.gpuCopyQueue.entries[0].srcBufferId = 102;
    fakeState.gpuCopyQueue.entries[1].srcBufferId = 104;
    fakeState.gpuCopyQueue.entryState[0] = fakeState.gpuCopyQueue.entryState[1] = LORIE_JOB_QUEUED;
    CHECK(lorieQueueHoldsOnlyCarries(), "two carries queued, not seen as only carries");
    fakeState.gpuCopyQueue.writeIndex = 3;
    fakeState.gpuCopyQueue.entries[2].srcBufferId = 777;          /* a client's pixmap: a present */
    fakeState.gpuCopyQueue.entryState[2] = LORIE_JOB_QUEUED;
    CHECK(!lorieQueueHoldsOnlyCarries(), "a present queued, and the fallback would not wait for it");
    fakeState.gpuCopyQueue.entryState[2] = LORIE_JOB_CANCELLED;
    CHECK(lorieQueueHoldsOnlyCarries(), "a cancelled present still counted");
    fakeState.gpuCopyQueue.entryState[2] = LORIE_JOB_CLAIMED;
    CHECK(!lorieQueueHoldsOnlyCarries(), "a claimed present not waited for");
    fakeState.rootDoubleBuffered = 0;
    CHECK(!lorieQueueHoldsOnlyCarries(), "single-buffered root: there are no carries");

    printf("carry queue (allowed, queued, only carries): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
