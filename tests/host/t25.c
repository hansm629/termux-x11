/* T25: a queued GPU copy must never land on top of CPU drawing that happened after it was queued - even
 * when the renderer cannot get to the queue in time. X's lorieCancelConflictingCopies and the renderer's
 * rendererClaimEntry are extracted from the sources by gen.py; regions are real pixman. The renderer is
 * simulated only as "run every entry it manages to claim, in order", which is what its drain does. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pixman.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef pixman_region16_t RegionRec, *RegionPtr;
typedef pixman_box16_t BoxRec, *BoxPtr;
#define rgnOUT PIXMAN_REGION_OUT
static inline int RegionContainsRect(RegionPtr r, BoxPtr b) { return pixman_region_contains_rectangle(r, b); }
void _pixman_log_error(const char *f, const char *m) { (void) f; (void) m; }
int pixman_image_get_width(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_height(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_stride(pixman_image_t *i) { (void) i; return 0; }
uint32_t *pixman_image_get_data(pixman_image_t *i) { (void) i; return NULL; }
#define LORIE_TRACE_CANCEL 0
#define lorieTrace(...) do {} while (0)
#include "t25_src_types.inc"
struct fakeShared {
    struct {
        volatile uint32_t writeIndex, readIndex;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
        volatile uint32_t entryState[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;
    struct { uint32_t copyCancelledForCpuWrite, copyClaimedBeforeCpuWrite; } presentStats;
};
static struct fakeShared shared;
static struct fakeShared *state = &shared;                  /* renderer's view */
static struct { struct fakeShared *state; } fakePvfb = { &shared };
#define pvfb (&fakePvfb)                                     /* X server's view */
#include "t25_src_funcs.inc"

#define W 64
#define H 16
enum { SRC = 3, ROOT = 7, OTHER = 9 };
static uint32_t root[W * H], src[W * H];
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void) { memset(&shared, 0, sizeof shared); memset(root, 0, sizeof root); memset(src, 0, sizeof src); }
static void fill(uint32_t *px, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) px[y * W + x] = v;
}
static int areaIs(uint32_t *px, BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) if (px[y * W + x] != v) return 0;
    return 1;
}
/* the X server queueing a copy (lorieTryScheduleGpuCopy): QUEUED, then published */
static uint32_t enqueue(uint64_t srcId, uint64_t dstId, BoxRec b, uint64_t serial) {
    uint32_t w = shared.gpuCopyQueue.writeIndex, slot = w % LORIE_GPU_COPY_QUEUE_CAPACITY;
    LorieGpuCopyEntry *e = &shared.gpuCopyQueue.entries[slot];
    memset(e, 0, sizeof *e);
    e->serial = serial; e->srcBufferId = srcId; e->dstBufferId = dstId; e->numRects = 1;
    e->rects[0] = (LorieGpuCopyRect) { b.x1, b.y1, b.x2, b.y2 };
    shared.gpuCopyQueue.entryState[slot] = LORIE_JOB_QUEUED;
    __atomic_store_n(&shared.gpuCopyQueue.writeIndex, w + 1, __ATOMIC_RELEASE);
    return slot;
}
/* the renderer's drain: every entry it manages to claim is run, in order; a cancelled one is skipped */
static void rendererRun(int max) {
    while (max-- && shared.gpuCopyQueue.readIndex != shared.gpuCopyQueue.writeIndex) {
        uint32_t slot = shared.gpuCopyQueue.readIndex % LORIE_GPU_COPY_QUEUE_CAPACITY;
        LorieGpuCopyEntry *e = &shared.gpuCopyQueue.entries[slot];
        if (shared.gpuCopyQueue.entryState[slot] != LORIE_JOB_CANCELLED && rendererClaimEntry(slot) && e->dstBufferId == ROOT)
            for (int i = 0; i < e->numRects; i++) {
                BoxRec b = { e->rects[i].x1, e->rects[i].y1, e->rects[i].x2, e->rects[i].y2 };
                for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) root[y * W + x] = src[y * W + x];
            }
        shared.gpuCopyQueue.readIndex++;
    }
}
/* the X server drawing into a buffer with the CPU: the queue is dealt with first, then the pixels */
static void cpuWrite(uint64_t bufferId, uint32_t *px, BoxRec b, uint32_t v) {
    RegionRec r; pixman_region_init_rect(&r, b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1);
    lorieCancelConflictingCopies(bufferId, &r);
    pixman_region_fini(&r);
    fill(px, b, v);
}

int main(void) {
    BoxRec R = { 8, 2, 24, 10 }, Q = { 40, 2, 56, 10 }, P = { 16, 4, 32, 8 };

    /* 1. the review's counterexample: copy A queued into R, the renderer does not get to it before the X
     *    server draws newer B into R, then the renderer runs. R must end up B. */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(ROOT, root, R, 0xB);
    rendererRun(8);
    CHECK(areaIs(root, R, 0xB), "older queued copy landed on top of newer CPU drawing");
    CHECK(shared.presentStats.copyCancelledForCpuWrite == 1, "conflicting copy not cancelled");

    /* 2. disjoint: drawing into Q says nothing about a copy into R - it must still run */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(ROOT, root, Q, 0xB);
    rendererRun(8);
    CHECK(areaIs(root, R, 0xA), "copy into an untouched area was dropped");
    CHECK(areaIs(root, Q, 0xB), "Q lost");
    CHECK(shared.presentStats.copyCancelledForCpuWrite == 0, "unneeded cancellation");

    /* 3. partial overlap cancels; the copy is all or nothing */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(ROOT, root, P, 0xB);
    rendererRun(8);
    CHECK(areaIs(root, P, 0xB), "partial overlap: older copy over newer drawing");

    /* 4. renderer claim wins: the renderer has already claimed A (it is running under the shared lock),
     *    so it cannot be cancelled - and the X server's lock then waits until A has finished before the
     *    CPU draws. Modelled as: claim, finish A, then draw. */
    reset(); fill(src, R, 0xA);
    uint32_t slot = enqueue(SRC, ROOT, R, 1);
    CHECK(rendererClaimEntry(slot), "renderer could not claim a queued entry");
    {   /* the X server's check while the renderer has it */
        RegionRec r; pixman_region_init_rect(&r, R.x1, R.y1, R.x2 - R.x1, R.y2 - R.y1);
        lorieCancelConflictingCopies(ROOT, &r); pixman_region_fini(&r);
    }
    CHECK(shared.gpuCopyQueue.entryState[slot] == LORIE_JOB_CLAIMED, "a claimed entry was cancelled");
    CHECK(shared.presentStats.copyClaimedBeforeCpuWrite == 1, "claimed entry not reported");
    rendererRun(8);                 /* finishes A (already claimed: the run skips the claim) */
    fill(root, R, 0xA);             /* what the claimed run wrote */
    fill(root, R, 0xB);             /* then the X server, after the lock */
    CHECK(areaIs(root, R, 0xB), "claimed case: order not kept");

    /* 5. renderer unavailable for a long time, then back: nothing it does later can undo the drawing */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(ROOT, root, R, 0xB);
    /* ... no renderer for a while ... */
    rendererRun(8);
    CHECK(areaIs(root, R, 0xB), "late renderer undid newer drawing");

    /* 6. head of the queue stuck on an unrelated buffer, conflicting copy behind it */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, OTHER, R, 1);      /* stuck: the renderer cannot run it yet */
    enqueue(SRC, ROOT, R, 2);
    cpuWrite(ROOT, root, R, 0xB);
    rendererRun(8);
    CHECK(areaIs(root, R, 0xB), "copy behind a stuck head landed over newer drawing");
    CHECK(shared.presentStats.copyCancelledForCpuWrite == 1, "only the conflicting one should be cancelled");

    /* 7. the X server writing the copy's source: the copy would read the newer pixels, so it goes too */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(SRC, src, R, 0xC);
    rendererRun(8);
    CHECK(!areaIs(root, R, 0xC), "copy read its source after the CPU had overwritten it");

    /* 8. nested: a read access first (no cancellation), then the write - same result, no waiting involved */
    reset(); fill(src, R, 0xA);
    enqueue(SRC, ROOT, R, 1);
    cpuWrite(ROOT, root, R, 0xB);
    rendererRun(8);
    CHECK(areaIs(root, R, 0xB), "nested: order not kept");

    printf("T25 queued copy vs later CPU write: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
