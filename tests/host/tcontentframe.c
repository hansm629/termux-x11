/* TCONTENTFRAME: client content from its presentation to the screen (P11-C3-D0, TERMUX_X11_CONTENT_FRAME_DIAG=1), from
 * the real code (gen.py "contentframe"): the X server's hooks (InitOutput.c lorieContent*: numbering, counting per
 * client, stamping a pixmap, carrying the stamp through CPU accesses, stale reads, publishing with the screen pixmap)
 * and the renderer's frames (renderer.c rendererContentRead, rendererContentFrame), on a clock a case sets.
 *
 *   Off: nothing numbered, read or counted. A client's content through a compositor - its GPU copy queued, read by the
 *   compositor before it ran (stale; not once in flight or done), done, composited into the compositor's buffer and
 *   from there into the screen pixmap (a copy's order: source first), published, shown by a frame, then shown again
 *   (reused) - each stage's wait exact. A source read alone carries nothing into a later destination. Straight into
 *   the screen pixmap by the GPU (no compositor) and into a window by the CPU. Skipped and completed (copy, skip). An
 *   older content never over a newer one. Numbers past their wrap. A publish with nothing new changes nothing. Frames
 *   with new content 22 ms apart, then 101 ms apart (asked for in the first half: pending), then 99 ms apart (asked for
 *   late: client): their intervals, the longest, the slowest content with its stages. Clients: named once, counted
 *   apart, a ninth taking the least-used slot, a reused index with another client a new slot. An access whose
 *   PrepareAccess failed (EXA finishes none): as if never opened - its source carried nowhere, its index not left open,
 *   what the index carried before kept, the next composite clean; a failed destination carries nothing; nested
 *   accesses by their index (source and mask: the newer); two sources one after the other at one index: the newer. With
 *   TERMUX_X11_EXA_COMPOSITE_OPT=1 (P11-C3-D1-A: a copy's damage reported once it is done) no read is stale: the
 *   compositor is told of the copy only once it is there - a read while it is queued sees what it was told of.
 */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
typedef int Bool;
typedef uint8_t CARD8;
#define TRUE 1
#define FALSE 0
#define PresentCompleteModeCopy 0
#define PresentCompleteModeSkip 2

#include "diagtypes_src.inc"
#include "gpucopytypes_src.inc"
#include "xorglist_src.inc"
#include "contentconsts_src.inc"

/* the shared state, as far as the code under test uses it (lorie.h's fields) */
struct lorie_shared_server_state {
    struct { volatile uint64_t completedSerial; } gpuCopyQueue;
    struct {
        volatile uint32_t busy;
        volatile pid_t pid;
        uint32_t count;
        uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX];
        uint32_t auxCount;
        uint64_t auxIds[LORIE_GPU_IN_FLIGHT_MAX];
    } gpuInFlight;
    struct { uint32_t count; uint64_t ids[LORIE_CPU_ACCESS_MAX]; volatile uint32_t releases, waiter; } cpuAccess;
    struct lorie_renderer_diag diag;
    struct lorie_backend_control backend;
    struct lorie_content content;
};
#include "inflight_src.inc"

/* buffers, pixmaps, clients, the screen - as far as the code under test looks at them */
typedef struct LorieBuffer { struct { uint64_t id; } d; } LorieBuffer;
#define LorieBuffer_description(b) (&(b)->d)
typedef struct { void *driverPriv; } PixmapRec, *PixmapPtr;
#define exaGetPixmapDriverPrivate(p) ((p)->driverPriv)
typedef struct { int index; } ClientRec, *ClientPtr;
static ClientPtr curClient;
static ClientPtr GetCurrentClient(void) { return curClient; }
static const char *clientNames[16];
static const char *GetClientCmdName(ClientPtr c) { return clientNames[c->index]; }
static struct { void *devPrivate; } screenRec, *pScreenPtr = &screenRec;

/* the X server's own (InitOutput.c lorieScreenInfo), as far as the code under test uses it */
static struct lorie_shared_server_state st;
static struct {
    struct lorie_shared_server_state *state;
    uint64_t gpuCopySerialCounter;
    Bool contentDiag;
    Bool exaCompositeOpt;           /* P11-C3-D1-A: the copies' damage once they are done */
    uint32_t contentSeq, contentGen, contentPublishedSeq;
    uint32_t contentOpen;
    struct lorie_content_stamp contentFrom[EXA_NUM_PREPARE_INDICES], contentBefore[EXA_NUM_PREPARE_INDICES];
    struct {
        const void *client;
        int index;
        char cmd[24];
        uint32_t requests, skipped, requestsAt, skippedAt, maxGapUs;
        uint64_t lastUs;
    } contentClients[LORIE_CONTENT_CLIENTS];
} fakePvfb = { &st }, *pvfb = &fakePvfb;
static volatile struct lorie_shared_server_state *state = &st;     /* the renderer's view of it */

static uint64_t fakeNow = 1000;
#define lorieMonotonicUs() (fakeNow)
#include "contentframe_src.inc"
#undef lorieMonotonicUs

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static LorieBuffer bufs[8];
static LoriePixmapPriv privs[8];
static PixmapRec pix[8];
enum { CLIENT_PIX, WINDOW, COMPOSITOR, ROOT, TERMINAL };
static void world(void) {
    int i;
    memset(&st, 0, sizeof(st));
    memset(privs, 0, sizeof(privs));
    for (i = 0; i < 8; i++) {
        bufs[i].d.id = 100 + i;
        privs[i].buffer = &bufs[i];
        pix[i].driverPriv = &privs[i];
    }
    screenRec.devPrivate = &pix[ROOT];
    fakePvfb.contentDiag = TRUE;
    st.backend.contentDiag = 1;
    fakePvfb.contentOpen = 0;
    memset(fakePvfb.contentFrom, 0, sizeof fakePvfb.contentFrom);
    memset(fakePvfb.contentBefore, 0, sizeof fakePvfb.contentBefore);
    memset(&vkContentLast, 0, sizeof(vkContentLast));
}
/* EXA's order: a composite opens its destination, then its source; a copy its source, then its destination; both
 * finish the source first (loriePrepareAccess, lorieFinishAccess call these, with the option on) */
static void composite(int src, int dst) {
    lorieContentAccess(&pix[dst], &privs[dst], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[src], &privs[src], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[src], &privs[src], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
    lorieContentAccess(&pix[dst], &privs[dst], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
}
static void copyArea(int src, int dst) {
    lorieContentAccess(&pix[src], &privs[src], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[dst], &privs[dst], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[src], &privs[src], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
    lorieContentAccess(&pix[dst], &privs[dst], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
}
/* a frame presented at `fakeNow`: as rendererVkRedraw takes the content under the lock and counts it once presented */
static void frame(void) {
    struct lorie_content c;
    rendererContentRead(&c);
    rendererContentFrame(&c, fakeNow);
}
/* a client's presentation through the compositor into the screen pixmap, published: its number */
static uint32_t throughCompositor(uint64_t askAt, uint64_t doneAt, uint64_t compositedAt, uint64_t rootAt, uint64_t publishAt) {
    uint64_t requestUs, copyUs;
    uint32_t seq;
    fakeNow = askAt;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    copyUs = lorieContentCopied(seq, requestUs, &pix[WINDOW], TRUE);
    fakeNow = doneAt;
    st.gpuCopyQueue.completedSerial = fakePvfb.gpuCopySerialCounter;
    lorieContentCopyDone(seq, requestUs, copyUs, &pix[WINDOW]);
    fakeNow = compositedAt;
    composite(WINDOW, COMPOSITOR);
    fakeNow = rootAt;
    copyArea(COMPOSITOR, ROOT);
    fakeNow = publishAt;
    lorieContentPublish(&privs[ROOT]);
    return seq;
}

int main(void) {
    static ClientRec glmark = { 3 }, xfwm = { 5 }, other[10];
    volatile struct lorie_renderer_diag *d = &st.diag;
    uint64_t requestUs, copyUs;
    uint32_t seq, n;
    int i;

    clientNames[3] = "/usr/bin/glmark2-es2", clientNames[5] = "xfwm4";

    /* off: nothing numbered, nothing read */
    world();
    fakePvfb.contentDiag = FALSE;
    st.backend.contentDiag = 0;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    st.content.gen = 4;
    frame();
    CHECK(seq == 0 && requestUs == 0 && d->contentRequests == 0 && d->contentScreenNew == 0 && d->contentScreenReused == 0,
          "off: numbered %u, %u requests, %u frames counted", seq, d->contentRequests, d->contentScreenNew + d->contentScreenReused);

    /* through a compositor, step by step */
    world();
    curClient = &glmark;
    fakeNow = 1000;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    CHECK(seq && requestUs == 1000 && d->contentRequests == 1, "request: %u at %llu", seq, (unsigned long long) requestUs);
    fakeNow = 1100;
    fakePvfb.gpuCopySerialCounter = 40;     /* lorieTryScheduleGpuCopy queued serial 40 */
    copyUs = lorieContentCopied(seq, requestUs, &pix[WINDOW], TRUE);
    CHECK(copyUs == 1100 && privs[WINDOW].contentCopySerial == 40 && !privs[WINDOW].content.seq && d->contentGpuCopies == 1 &&
          d->contentPresentToCopy.count == 1 && d->contentPresentToCopy.sumUs == 100,
          "GPU copy queued: at %llu, serial %llu kept, stamp %u, %u copies, wait %llu", (unsigned long long) copyUs,
          (unsigned long long) privs[WINDOW].contentCopySerial, privs[WINDOW].content.seq, d->contentGpuCopies,
          (unsigned long long) d->contentPresentToCopy.sumUs);
    /* the compositor reads the window before the copy ran: stale; with it in flight or done: not */
    st.gpuCopyQueue.completedSerial = 39;
    lorieContentStaleRead(&privs[WINDOW], EXA_PREPARE_SRC);
    lorieContentStaleRead(&privs[WINDOW], EXA_PREPARE_DEST);
    st.gpuInFlight.ids[0] = bufs[WINDOW].d.id, st.gpuInFlight.count = 1, st.gpuInFlight.busy = LORIE_GPU_IN_FLIGHT_MAIN;
    lorieContentStaleRead(&privs[WINDOW], EXA_PREPARE_SRC);
    st.gpuInFlight.busy = 0;
    st.gpuCopyQueue.completedSerial = 40;
    lorieContentStaleRead(&privs[WINDOW], EXA_PREPARE_SRC);
    CHECK(d->contentStaleReads == 1, "stale reads: %u (want 1: queued, read as a source)", d->contentStaleReads);
    fakeNow = 3000;
    lorieContentCopyDone(seq, requestUs, copyUs, &pix[WINDOW]);
    CHECK(privs[WINDOW].content.seq == seq && privs[WINDOW].content.copyUs == 3000 && d->contentCopiesDone == 1 &&
          d->contentCopyToComplete.sumUs == 1900 && d->contentRootUpdates == 0 && d->contentCompositorUpdates == 0,
          "copy done: stamp %u at %llu, wait %llu", privs[WINDOW].content.seq, (unsigned long long) privs[WINDOW].content.copyUs,
          (unsigned long long) d->contentCopyToComplete.sumUs);
    /* a read of the window alone, then a terminal drawn: nothing carried into it */
    lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
    lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
    lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
    CHECK(!privs[TERMINAL].content.seq && fakePvfb.contentOpen == 0, "a source read alone carried into a later destination");
    curClient = &xfwm;
    fakeNow = 4000;
    composite(WINDOW, COMPOSITOR);
    CHECK(privs[COMPOSITOR].content.seq == seq && privs[COMPOSITOR].content.readyUs == 4000 && d->contentCompositorUpdates == 1 &&
          d->contentRootUpdates == 0, "composited into the compositor's buffer: stamp %u, %u compositor updates",
          privs[COMPOSITOR].content.seq, d->contentCompositorUpdates);
    fakeNow = 5000;
    copyArea(COMPOSITOR, ROOT);
    CHECK(privs[ROOT].content.seq == seq && d->contentRootUpdates == 1 && d->contentCompleteToRoot.sumUs == 2000 &&
          privs[ROOT].content.requestUs == 1000 && privs[ROOT].content.copyUs == 3000,
          "copied into the screen pixmap: stamp %u, %u root updates, wait %llu", privs[ROOT].content.seq, d->contentRootUpdates,
          (unsigned long long) d->contentCompleteToRoot.sumUs);
    fakeNow = 6000;
    lorieContentPublish(&privs[ROOT]);
    CHECK(st.content.gen == 1 && st.content.seq == seq && st.content.rootUs == 5000 && st.content.publishUs == 6000 &&
          d->contentRootPublishes == 1 && d->contentRootToPublish.sumUs == 1000, "published: gen %u seq %u, root %llu, at %llu",
          st.content.gen, st.content.seq, (unsigned long long) st.content.rootUs, (unsigned long long) st.content.publishUs);
    fakeNow = 7000;
    lorieContentPublish(&privs[ROOT]);
    CHECK(st.content.gen == 1 && d->contentRootPublishes == 1, "published again with nothing new: gen %u", st.content.gen);
    fakeNow = 8000;
    frame();
    fakeNow = 16000;
    frame();
    CHECK(d->contentScreenNew == 1 && d->contentScreenReused == 1 && d->contentPublishToScreen.sumUs == 2000 &&
          d->contentTotal.sumUs == 7000 && d->contentWorstUs == 7000 && d->contentWorstCopyUs == 2000 &&
          d->contentWorstCompositeUs == 2000 && d->contentWorstPublishUs == 1000 && d->contentWorstScreenUs == 2000,
          "frames: %u new, %u reused, to screen %llu, total %llu, worst %u (%u/%u/%u/%u)", d->contentScreenNew, d->contentScreenReused,
          (unsigned long long) d->contentPublishToScreen.sumUs, (unsigned long long) d->contentTotal.sumUs, d->contentWorstUs,
          d->contentWorstCopyUs, d->contentWorstCompositeUs, d->contentWorstPublishUs, d->contentWorstScreenUs);

    /* frames with new content 22 ms, 101 ms (asked for early: pending), 99 ms (asked for late: client) apart */
    curClient = &glmark;
    throughCompositor(20000, 21000, 22000, 23000, 24000);
    fakeNow = 30000;
    frame();
    throughCompositor(31000, 32000, 33000, 34000, 35000);
    fakeNow = 131000;
    frame();
    throughCompositor(220000, 222000, 224000, 226000, 228000);
    fakeNow = 230000;
    frame();
    CHECK(d->contentScreenNew == 4 && d->contentInterval.count == 3 && d->contentInterval.sumUs == 22000 + 101000 + 99000 &&
          d->contentGapsPending == 1 && d->contentGapsClient == 1 && d->contentMaxGapUs == 101000 && d->contentWorstUs == 100000 &&
          d->contentWorstCopyUs == 1000 && d->contentWorstScreenUs == 96000,
          "intervals: %u new, %u sampled (sum %llu), pending %u client %u, longest %u, worst %u (copy %u, screen %u)",
          d->contentScreenNew, d->contentInterval.count, (unsigned long long) d->contentInterval.sumUs, d->contentGapsPending,
          d->contentGapsClient, d->contentMaxGapUs, d->contentWorstUs, d->contentWorstCopyUs, d->contentWorstScreenUs);

    /* straight into the screen pixmap by the GPU; into a window by the CPU; skipped; completed */
    world();
    fakeNow = 1000;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    copyUs = lorieContentCopied(seq, requestUs, &pix[ROOT], TRUE);
    fakeNow = 2500;
    lorieContentCopyDone(seq, requestUs, copyUs, &pix[ROOT]);
    CHECK(privs[ROOT].content.seq == seq && d->contentRootUpdates == 1 && d->contentCompositorUpdates == 0 &&
          d->contentCompleteToRoot.count == 1 && d->contentCompleteToRoot.sumUs == 0,
          "GPU copy straight into the screen pixmap: %u root updates, %u compositor, wait %llu", d->contentRootUpdates,
          d->contentCompositorUpdates, (unsigned long long) d->contentCompleteToRoot.sumUs);
    fakeNow = 3000;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    fakeNow = 3200;
    lorieContentCopied(seq, requestUs, &pix[WINDOW], FALSE);
    CHECK(privs[WINDOW].content.seq == seq && privs[WINDOW].content.copyUs == 3200 && d->contentCpuCopies == 1 &&
          d->contentPresentToCopy.sumUs == 200, "CPU copy into a window: stamp %u, %u CPU copies", privs[WINDOW].content.seq,
          d->contentCpuCopies);
    lorieContentSkipped(seq);
    lorieContentComplete(seq, PresentCompleteModeCopy);
    lorieContentComplete(seq, PresentCompleteModeSkip);
    lorieContentSkipped(0);
    lorieContentComplete(0, PresentCompleteModeCopy);
    CHECK(d->contentSkipped == 1 && d->contentCompletes == 1 && d->contentCompletesSkip == 1, "skipped %u, completes %u, skip %u",
          d->contentSkipped, d->contentCompletes, d->contentCompletesSkip);
    /* an older content never over a newer one */
    privs[WINDOW].content.seq = 50;
    lorieContentStore(&pix[WINDOW], &(struct lorie_content_stamp) { 49, 1, 1, 1 }, 9000, TRUE);
    CHECK(privs[WINDOW].content.seq == 50, "an older content over a newer one: %u", privs[WINDOW].content.seq);

    /* numbers past their wrap: 0 is none, newer stays newer */
    world();
    fakePvfb.contentSeq = UINT32_MAX - 1;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    lorieContentCopied(seq, requestUs, &pix[WINDOW], FALSE);
    n = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    lorieContentCopied(n, requestUs, &pix[WINDOW], FALSE);
    CHECK(seq == UINT32_MAX && n == 1 && privs[WINDOW].content.seq == 1, "wrap: %u then %u, stamp %u", seq, n, privs[WINDOW].content.seq);
    /* without a pixmap (NotifyMSC): not numbered */
    CHECK(lorieContentRequest(NULL, &requestUs) == 0, "a presentation without a pixmap numbered");

    /* clients: named once, counted apart; a ninth takes the least-used slot; a reused index, another client: a new slot */
    world();
    memset(fakePvfb.contentClients, 0, sizeof(fakePvfb.contentClients));
    curClient = &glmark;
    for (i = 0; i < 5; i++) {
        fakeNow = 100000 + (uint64_t) i * 1000;
        lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    }
    fakeNow = 200000;
    lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    lorieContentSkipped(1);
    CHECK(fakePvfb.contentClients[0].client == &glmark && !strcmp(fakePvfb.contentClients[0].cmd, "glmark2-es2") &&
          fakePvfb.contentClients[0].requests == 6 && fakePvfb.contentClients[0].skipped == 1 && fakePvfb.contentClients[0].maxGapUs == 96000,
          "client: %s, %u requests, %u skipped, max gap %u", fakePvfb.contentClients[0].cmd, fakePvfb.contentClients[0].requests,
          fakePvfb.contentClients[0].skipped, fakePvfb.contentClients[0].maxGapUs);
    for (i = 0; i < 8; i++) {
        other[i].index = 6 + i;
        clientNames[6 + (i % 10)] = "client";
        curClient = &other[i];
        lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    }
    CHECK(fakePvfb.contentClients[0].client == &glmark, "the busiest client's slot taken by a ninth");
    other[9].index = 3;     /* glmark2's index, another client */
    curClient = &other[9];
    lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    for (i = 0, n = 0; i < LORIE_CONTENT_CLIENTS; i++)
        n += fakePvfb.contentClients[i].client == &other[9];
    CHECK(n == 1 && fakePvfb.contentClients[0].client == &glmark, "a reused index with another client: %u slots of its own", n);
    curClient = NULL;
    n = d->contentRequests;
    lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    CHECK(d->contentRequests == n + 1, "a presentation with no current client not counted");

    /* a failed opening (loriePrepareAccess returning FALSE: EXA finishes no such access): as if never opened - its source
     * carried nowhere, its index not left open, what that index carried before kept; nested accesses each by their
     * index, the newest of a composite's source and mask carried; a failed destination carries nothing */
    {
        struct lorie_content_stamp older = { 30, 100, 200, 300 }, newer = { 31, 110, 210, 310 }, newest = { 32, 120, 220, 320 };
        world();
        privs[WINDOW].content = newer;
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_FAILED);
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
        CHECK(!privs[COMPOSITOR].content.seq && !fakePvfb.contentOpen, "a failed source carried (%u) or left open (%#x)",
              privs[COMPOSITOR].content.seq, fakePvfb.contentOpen);
        composite(TERMINAL, ROOT);
        CHECK(!privs[ROOT].content.seq && !fakePvfb.contentOpen, "after a failed source, the next composite carried %u",
              privs[ROOT].content.seq);

        privs[TERMINAL].content = older;
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_FAILED);
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
        CHECK(privs[COMPOSITOR].content.seq == older.seq && !fakePvfb.contentOpen, "a failed source after another at its index: "
              "carried %u (want %u, the one before)", privs[COMPOSITOR].content.seq, older.seq);

        world();
        privs[WINDOW].content = newer, privs[TERMINAL].content = older;
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
        CHECK(privs[COMPOSITOR].content.seq == newer.seq, "two sources one after the other at one index: carried %u (want the "
              "newer %u)", privs[COMPOSITOR].content.seq, newer.seq);

        world();
        privs[TERMINAL].content = newer, privs[WINDOW].content = newest;
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_MASK, LORIE_CONTENT_OPEN);
        CHECK(fakePvfb.contentOpen == (1u << EXA_PREPARE_DEST | 1u << EXA_PREPARE_SRC | 1u << EXA_PREPARE_MASK),
              "nested: open %#x", fakePvfb.contentOpen);
        lorieContentAccess(&pix[TERMINAL], &privs[TERMINAL], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_MASK, LORIE_CONTENT_DONE);
        lorieContentAccess(&pix[COMPOSITOR], &privs[COMPOSITOR], EXA_PREPARE_DEST, LORIE_CONTENT_DONE);
        CHECK(privs[COMPOSITOR].content.seq == newest.seq && !fakePvfb.contentOpen, "nested: carried %u (want the mask's %u)",
              privs[COMPOSITOR].content.seq, newest.seq);

        world();
        privs[WINDOW].content = newer;
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[ROOT], &privs[ROOT], EXA_PREPARE_DEST, LORIE_CONTENT_OPEN);
        lorieContentAccess(&pix[ROOT], &privs[ROOT], EXA_PREPARE_DEST, LORIE_CONTENT_FAILED);
        lorieContentAccess(&pix[WINDOW], &privs[WINDOW], EXA_PREPARE_SRC, LORIE_CONTENT_DONE);
        CHECK(!privs[ROOT].content.seq && !fakePvfb.contentOpen && !fakePvfb.contentFrom[EXA_PREPARE_SRC].seq,
              "a failed destination: carried %u, open %#x, kept %u", privs[ROOT].content.seq, fakePvfb.contentOpen,
              fakePvfb.contentFrom[EXA_PREPARE_SRC].seq);
    }

    /* P11-C3-D1-A: the copy's damage reported once it is done - a read while it is queued sees what the compositor was
     * told of, not stale */
    world();
    fakePvfb.exaCompositeOpt = TRUE;
    seq = lorieContentRequest(&pix[CLIENT_PIX], &requestUs);
    fakePvfb.gpuCopySerialCounter = 90;
    st.gpuCopyQueue.completedSerial = 89;
    lorieContentCopied(seq, requestUs, &pix[WINDOW], TRUE);
    lorieContentStaleRead(&privs[WINDOW], EXA_PREPARE_SRC);
    CHECK(!privs[WINDOW].contentCopySerial && d->contentStaleReads == 0 && d->contentGpuCopies == 1, "with the damage put off: "
          "serial %llu kept, %u stale reads (want none)", (unsigned long long) privs[WINDOW].contentCopySerial, d->contentStaleReads);
    fakePvfb.exaCompositeOpt = FALSE;

    printf("TCONTENTFRAME client content from its presentation to the screen (TERMUX_X11_CONTENT_FRAME_DIAG): %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
