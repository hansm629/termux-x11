/* T28: cursor buffers come back only on the compositor's word. The pool transitions are extracted
 * verbatim from renderer.c (gen.py); the compositor's release fences are real pipes - no data is a
 * fence still pending, data written is one that has signalled, a closed descriptor is one that cannot
 * be waited on. What is simulated is the order things happen in: the renderer rendering, the overlay
 * thread applying, the completion arriving. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
typedef struct LorieBuffer LorieBuffer;
static struct { struct { uint32_t zeroCopyFenceErrors; } presentStats; } fakeState, *state = &fakeState;
#include "cursor_src.inc"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void) {
    memset(cursorPool, 0, sizeof cursorPool);
    cursorPoolHandedSlot = -1;
}
/* the renderer renders a new image into a buffer nobody holds and hands it over; -1 if there is none */
static int render(void) {
    cursorPoolDrain();
    int slot = cursorPoolTake();
    if (slot >= 0)
        cursorPoolHand(slot);
    return slot;
}
/* the overlay thread applies the handed buffer; returns the seq its completion will carry */
static uint32_t apply(void) { return cursorPoolSetOnLayer(cursorPoolHandedSlot); }
/* a release fence: rd pending until wr is written */
static int fenceRd, fenceWr;
static int newFence(void) { int p[2]; if (pipe(p)) return -1; fenceRd = p[0]; fenceWr = p[1]; return fenceRd; }
static void signalFence(int wr) { if (write(wr, "x", 1) != 1) {} }

int main(void) {
    int A, B, C, D, slot, fd;
    (void) cursorOverlayRenderPending;          /* the renderer's flag, not a pool transition */
    uint32_t seqA, seqB, seqC;

    /* 1. The review's model: A submitted, B submitted, A not released yet, the cursor changes again.
     *    A must not be rendered into, however many times that happens before its release. */
    reset();
    A = render(); apply();
    B = render(); seqA = apply();                 /* B replaces A; A is retiring */
    CHECK(seqA != 0 && cursorPool[A].state == LORIE_CURSOR_RETIRING, "A not retiring once B replaced it");
    C = render();
    CHECK(C >= 0 && C != A, "A rendered into before the compositor released it (got %d)", C);
    seqB = apply();
    D = render();
    CHECK(D >= 0 && D != A && D != B, "a buffer still with the compositor rendered into (got %d)", D);
    seqC = apply();
    slot = render();
    CHECK(slot < 0, "every buffer is with the compositor, yet slot %d was handed out", slot);

    /* 2. A's completion arrives with a fence that has not signalled: still A is not free. Signalled, it is. */
    fd = newFence();
    cursorPoolReleaseReported(seqA, fd, true);
    slot = render();
    CHECK(slot < 0, "A taken back before its release fence signalled");
    signalFence(fenceWr);
    slot = render();
    CHECK(slot == A, "A not taken back after its fence signalled (got %d)", slot);
    close(fenceWr);

    /* 3. A fence that cannot be waited on: the buffer is never taken back, no matter how long. */
    reset();
    A = render(); apply();
    B = render(); seqA = apply();
    fd = newFence(); close(fenceRd); close(fenceWr);   /* a descriptor that is no longer open */
    cursorPoolReleaseReported(seqA, fd, true);
    for (int i = 0; i < 4; i++) { if (render() >= 0) apply(); }
    CHECK(cursorPool[A].state == LORIE_CURSOR_RETIRING && cursorPool[A].fenceUnusable,
          "A with an unusable fence was taken back");

    /* 4. A completion that did not carry the layer says nothing: A stays held. -1 from one that did
     *    means the compositor had let go already. */
    reset();
    A = render(); apply();
    B = render(); seqA = apply();
    cursorPoolReleaseReported(seqA, -1, false);
    for (int i = 0; i < 4; i++) { if (render() >= 0) apply(); }
    CHECK(cursorPool[A].state == LORIE_CURSOR_RETIRING, "A taken back on a report that never mentioned its layer");
    reset();
    A = render(); apply();
    B = render(); seqA = apply();
    cursorPoolReleaseReported(seqA, -1, true);
    slot = render();
    CHECK(slot == A || cursorPool[A].state == LORIE_CURSOR_FREE, "A not taken back on an already-released report");

    /* 5. A handed buffer replaced before the overlay thread applied it goes back at once - the
     *    compositor never saw it. */
    reset();
    A = render();
    B = render();
    CHECK(cursorPool[A].state == LORIE_CURSOR_FREE && cursorPool[B].state == LORIE_CURSOR_HANDED,
          "a handed buffer that was never applied is still held");

    /* 6. The layer is torn down. What it had is never rendered into again, and a completion from the
     *    old layer arriving afterwards changes nothing in the new pool. */
    reset();
    A = render(); apply();
    B = render(); seqA = apply();                 /* A retiring, B on the old layer */
    cursorPoolOrphan();
    CHECK(cursorPool[A].state == LORIE_CURSOR_ORPHANED && cursorPool[B].state == LORIE_CURSOR_ORPHANED,
          "buffers of a torn-down layer not orphaned");
    C = render();
    CHECK(C != A && C != B, "a buffer of the torn-down layer rendered into");
    apply();                                      /* the new layer's first buffer */
    D = render(); seqB = apply();                 /* C retiring on the new layer */
    fd = newFence(); signalFence(fenceWr);
    cursorPoolReleaseReported(seqA, fd, true);    /* the old layer's report for A, late */
    CHECK(cursorPool[A].state == LORIE_CURSOR_ORPHANED && cursorPool[C].state == LORIE_CURSOR_RETIRING,
          "an old layer's report changed the new pool");
    close(fenceWr);
    (void) seqB; (void) seqC;

    /* Counterexample, as a model of the rule this replaced: the next image went into "the one not
     *    handed over last" (index ^ 1). In case 1 that picks A while A is still the compositor's. */
    {
        unsigned index = 0;                       /* A was handed first */
        index ^= 1u;                              /* B handed */
        unsigned next = index ^ 1u;               /* the next update */
        CHECK(next == 0, "model");
        printf("T28 model of the old alternation: next update renders into buffer %u = A, unreleased\n", next);
    }

    printf("T28 cursor buffer release lifetime: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
