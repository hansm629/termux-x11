/* P11-C5-B2: run the REAL XRender GPU batch/conflict/replay implementation from InitOutput.c
 * against CPU-mappable fake AHBs, real pixman and a fake GPU completion queue.
 * Tests disjoint writes, skipped-job CPU replay, cancellation, AHB reference lifetimes and
 * CPU-access barriers, without relying on the phone's Android drivers. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include <pixman.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define ERROR 6
#define log(level, ...) ((void) 0)
#define LORIE_GPU_COPY_MAX_RECTS 16
#define LORIE_GPU_COPY_QUEUE_CAPACITY 8
#define LORIE_GPU_COPY_NOT_RECORDED 0u
#define LORIE_GPU_COPY_RECORDED 1u
#define LORIE_GPU_COPY_SKIPPED 2u
#define LORIE_GPU_COPY_OP_SRC 0u
#define LORIE_GPU_COPY_OP_OVER 1u
#define LORIE_GPU_COPY_OP_CLEAR 2u
#define LORIE_GPU_COPY_OP_OVER_REVERSE 3u
#define LORIE_GPU_COPY_OP_IN 4u
#define LORIE_GPU_COPY_OP_IN_REVERSE 5u
#define LORIE_GPU_COPY_OP_OUT 6u
#define LORIE_GPU_COPY_OP_OUT_REVERSE 7u
#define LORIE_GPU_COPY_OP_ATOP 8u
#define LORIE_GPU_COPY_OP_ATOP_REVERSE 9u
#define LORIE_GPU_COPY_OP_XOR 10u
#define LORIE_GPU_COPY_OP_ADD 11u
#define LORIE_GPU_COPY_OP_COUNT 12u
#define LORIE_GPU_COPY_CANCELLED 1
#define LORIE_GPU_COPY_DONE 2
#define LORIE_GPU_COPY_RUNNING 3

typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxPtr rects; } RegionRec, *RegionPtr;
#define RegionNumRects(r) ((r)->n)
#define RegionRects(r) ((r)->rects)
typedef struct { int width, height, stride; uint64_t id; } LorieBuffer_Desc;
typedef struct LorieBuffer {
    LorieBuffer_Desc desc;
    uint32_t *pixels;
    int ref, pending, cpuLocks, releases;
} LorieBuffer;
typedef struct fake_pixmap *PixmapPtr;
struct fake_pixmap { uint64_t id; };
struct lorie_content_stamp { uint32_t seq; };
typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;
typedef struct {
    uint64_t serial;
    uint8_t recorded, cancelled;
} LorieGpuCopyEntry;
struct lorie_shared_server_state {
    int lock, lockingPid;
    struct {
        uint64_t completedSerial;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;
};
struct lorie_xrender_job {
    uint64_t serial;
    LorieBuffer *src, *dst;
    PixmapPtr dstPixmap;
    struct lorie_content_stamp content;
    uint32_t queueIndex, srcFormat, dstFormat;
    uint8_t op;
    Bool root;
    int16_t xOff, yOff;
    uint16_t numRects;
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
};
static struct {
    struct lorie_shared_server_state *state;
    struct lorie_xrender_job xrenderJobs[LORIE_GPU_COPY_QUEUE_CAPACITY];
    uint32_t xrenderJobsCount;
    uint64_t xrenderGpuSuccess, xrenderGpuFallback, xrenderAsyncFlushed,
             xrenderAsyncReplayed, xrenderAsyncFlushUs;
    int contentDiag;
    int rootGpuCopyPending;
} world, *pvfb = &world;
static struct lorie_shared_server_state st;
static int failures, cpuReplays, gpuWaits, contentStores, gone;
static uint64_t nowUs = 100000;
static void check(int cond, const char *message) {
    if (!cond) { failures++; printf("  FAIL %s\n", message); }
}
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }
static uint64_t lorieMonotonicUs(void) { return ++nowUs; }
static int lorieConnectionAlive(void) { return !gone; }
static int lorieRendererAvailable(void) { return !gone; }
static int lorieGpuCopyIsDone(uint64_t s) { return st.gpuCopyQueue.completedSerial >= s; }
static int lorieGpuCopySettle(uint64_t s) {
    if (lorieGpuCopyIsDone(s)) return LORIE_GPU_COPY_DONE;
    for (int i = 0; i < LORIE_GPU_COPY_QUEUE_CAPACITY; i++)
        if (st.gpuCopyQueue.entries[i].serial == s) {
            st.gpuCopyQueue.entries[i].cancelled = 1;
            return LORIE_GPU_COPY_CANCELLED;
        }
    return LORIE_GPU_COPY_RUNNING;
}
static void lorie_mutex_lock(int *m, int *pid) { ++*m; *pid = 1; }
static void lorie_mutex_unlock(int *m, int *pid) { --*m; *pid = 0; }
static void lorieGpuWaitIdle(struct lorie_shared_server_state *s, bool all, uint64_t id) {
    (void) s; (void) all; (void) id; gpuWaits++;
}
static int LorieBuffer_lock(LorieBuffer *b, void **p) {
    check(!b->cpuLocks, "CPU double-lock of same AHB");
    b->cpuLocks++;
    *p = b->pixels;
    return 0;
}
static int LorieBuffer_unlock(LorieBuffer *b) {
    check(b->cpuLocks == 1, "CPU released an AHB that was not locked");
    b->cpuLocks--;
    return 0;
}
static void LorieBuffer_gpuCopyPendingDec(LorieBuffer *b) {
    check(b->pending > 0, "GPU queue pending count underflow");
    b->pending--;
}
static void LorieBuffer_release(LorieBuffer *b) {
    check(b->ref > 0, "GPU queue reference count underflow");
    b->ref--;
    b->releases++;
}
static void lorieContentStore(PixmapPtr pix, const struct lorie_content_stamp *c, uint64_t now, Bool carried) {
    (void) pix; (void) now; (void) carried;
    if (c->seq) contentStores++;
}

#include "xrenderasync_src.inc"

static void expectedOver(uint32_t *src, uint32_t *dst, int x, int y, int w, int h) {
    pixman_image_t *s = pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 4, src, 8 * 4);
    pixman_image_t *d = pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 4, dst, 8 * 4);
    pixman_image_composite32(PIXMAN_OP_OVER, s, NULL, d, x, y, 0, 0, x, y, w, h);
    pixman_image_unref(s);
    pixman_image_unref(d);
}

int main(void) {
    uint32_t sourcePixels[32], destinationPixels[32], expectedPixels[32];
    LorieBuffer src = { .desc = { 8, 4, 8, 111 }, .pixels = sourcePixels };
    LorieBuffer dst = { .desc = { 8, 4, 8, 222 }, .pixels = destinationPixels };
    LorieBuffer other = { .desc = { 8, 4, 8, 333 } };
    struct fake_pixmap pix = { 44 };
    BoxRec candidate = { 6, 0, 7, 2 };
    RegionRec rect = { 1, &candidate };

    for (int i = 0; i < 32; i++) {
        sourcePixels[i] = 0x80800000;
        destinationPixels[i] = expectedPixels[i] = 0xff000040;
    }
    world.state = &st;
    st.gpuCopyQueue.completedSerial = 2;
    st.gpuCopyQueue.entries[0] = (LorieGpuCopyEntry) { .serial = 1, .recorded = LORIE_GPU_COPY_RECORDED };
    st.gpuCopyQueue.entries[1] = (LorieGpuCopyEntry) { .serial = 2, .recorded = LORIE_GPU_COPY_SKIPPED };
    world.xrenderJobsCount = 2;
    world.contentDiag = 1;
    for (int i = 0; i < 2; i++) {
        struct lorie_xrender_job *j = &world.xrenderJobs[i];
        *j = (struct lorie_xrender_job) {
            .serial = (uint64_t) i + 1, .queueIndex = i, .src = &src, .dst = &dst,
            .dstPixmap = &pix, .content = { (uint32_t) i + 1 },
            .srcFormat = PIXMAN_a8r8g8b8, .dstFormat = PIXMAN_a8r8g8b8,
            .op = LORIE_GPU_COPY_OP_OVER, .numRects = 1,
            .rects = { { i ? 4 : 0, 0, i ? 6 : 2, 2 } }
        };
        src.ref++; src.pending++; dst.ref++; dst.pending++;
    }
    // The first operation succeeded on the GPU, the second was skipped: its CPU replay must
    // not blend twice on the already-GPU-written rectangle of the first operation.
    expectedOver(sourcePixels, expectedPixels, 0, 0, 2, 2);
    expectedOver(sourcePixels, expectedPixels, 4, 0, 2, 2);
    expectedOver(sourcePixels, destinationPixels, 0, 0, 2, 2);

    check(!lorieXRenderAsyncConflict(&src, &dst, &rect, 0, 0),
          "disjoint destination writes must stay in one GPU batch");
    candidate = (BoxRec) { 1, 0, 2, 2 };
    check(lorieXRenderAsyncConflict(&src, &dst, &rect, 0, 0),
          "overlapping destinations must be a GPU-to-CPU ordering boundary");
    check(lorieXRenderAsyncConflict(&dst, &other, &rect, 0, 0),
          "read-after-write of a pending GPU destination must flush");
    check(lorieXRenderAsyncConflict(&other, &src, &rect, 0, 0),
          "write-after-read of a pending GPU source must flush");
    check(!lorieXRenderAsyncConflict(&src, &other, &rect, 0, 0),
          "same read-only source and disjoint write destinations can batch");

    lorieXRenderAsyncFlushOnBuffer(&other);
    check(world.xrenderJobsCount == 2, "unrelated CPU buffer access unexpectedly flushed GPU batch");
    lorieXRenderAsyncFlushOnBuffer(&dst);
    check(world.xrenderJobsCount == 0 && world.xrenderAsyncFlushed == 2 &&
          world.xrenderGpuSuccess == 1 && world.xrenderAsyncReplayed == 1 &&
          !memcmp(destinationPixels, expectedPixels, sizeof expectedPixels) &&
          !src.ref && !src.pending && !dst.ref && !dst.pending && !st.lock &&
          cpuReplays == 0 && contentStores == 2,
          "GPU completion, failed-job pixman replay, damage visibility or AHB lifetime");
    check(src.releases == 2 && dst.releases == 2 && gpuWaits == 2,
          "each queued GPU job must relinquish its own two buffer refs exactly once");

    // A renderer that disconnects before taking the job is cancelled under the GPU queue lock.
    // The X server replays the operation itself, never freeing the buffers before its CPU write.
    st.gpuCopyQueue.entries[2] = (LorieGpuCopyEntry) { .serial = 3 };
    world.xrenderJobs[0] = (struct lorie_xrender_job) {
        .serial = 3, .queueIndex = 2, .src = &src, .dst = &dst, .dstPixmap = &pix,
        .srcFormat = PIXMAN_a8r8g8b8, .dstFormat = PIXMAN_a8r8g8b8,
        .op = LORIE_GPU_COPY_OP_SRC, .numRects = 1, .rects = { { 6, 0, 8, 2 } }
    };
    world.xrenderJobsCount = 1;
    src.ref = dst.ref = src.pending = dst.pending = 1;
    gone = 1;
    lorieXRenderAsyncFlush();
    check(st.gpuCopyQueue.entries[2].cancelled && world.xrenderJobsCount == 0 &&
          world.xrenderAsyncReplayed == 2 && !src.ref && !dst.ref && !src.pending && !dst.pending &&
          destinationPixels[6] == sourcePixels[6] && !st.lock,
          "disconnected renderer must cancel its queued copy and CPU-replay before buffer release");

    printf("TASYNCXRENDER real batch, disjoint hazards, skipped/cancelled pixman replay: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
