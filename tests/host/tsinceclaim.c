/* TSINCECLAIM: "published since the claim" (rootZcPublishedSinceClaim) against the real handover. The X
 * server's lorieRootHandover (rootharness.h, gen.py "root") and the renderer's rendererClaimRootBuffer,
 * rootZcPublishedSinceClaim and rendererReleaseRootSlot (gen.py "sinceclaim") work on the one
 * rootHandover word, as they do across the shared memory.
 *
 * 1. A claim held while the X server publishes k times, k from 0 to past two wraps of the publish count
 *    in the word. The answer has to be yes for every k > 0 and no for k = 0. Code that decides by the
 *    count says no at one wrap and at two - a publish lost each time.
 * 2. A long random run of the renderer's lifecycle: claim, then give the slot back or keep it (submitted,
 *    then retiring, up to LORIE_ZC_MAX_HELD), give back retiring slots, ask; the X server publishing
 *    whenever it has a slot to move on to. Every answer is compared with whether a publish was actually
 *    made since the claim; and the X server must never publish the claimed slot.
 *    And the slot accounting through it all: the renderer never holds more than LORIE_ZC_MAX_HELD plus the
 *    claim in flight; while it holds no more than LORIE_ZC_MAX_HELD the X server always has its two
 *    slots and no publish is refused; the slot the X server draws into is never held; and once the
 *    renderer gives everything back, no held bit is left.
 * 3. The pool replaced while a claim is out (the word as the X server leaves it): yes. */
#include "rootharness.h"
#include <sched.h>
#include <pthread.h>
static typeof(fakeState) *state = &fakeState;
static int64_t rendererNowNs(void) { return 0; }
#define log(...) ((void) 0)
#include "sinceclaim_src.inc"

static int newestSlot(void) {
    return (int) ((fakeState.rootHandover >> LORIE_ROOT_NEWEST_SHIFT) & LORIE_ROOT_NEWEST_MASK);
}

int main(void) {
    static LoriePixmapPriv priv;
    const int wrap = 1 << __builtin_popcount(LORIE_ROOT_COUNT_MASK);
    int ks[] = { 0, 1, 2, wrap - 1, wrap, wrap + 1, 2 * wrap, 2 * wrap + 1 }, misses = 0;

    /* 1. one claim, k publishes */
    for (unsigned n = 0; n < sizeof ks / sizeof ks[0]; n++) {
        int k = ks[n], made = 0;
        init(&priv);
        fakeState.rootDoubleBuffered = 1;
        rendererRootSlot = -1;
        rendererClaimRootBuffer();
        int claimed = rendererRootSlot;
        for (int i = 0; i < k; i++) {
            if (!lorieRootHandover(&priv))
                break;
            made++;
            CHECK(newestSlot() != claimed, "published the claimed slot %d", claimed);
        }
        CHECK(made == k, "only %d of %d publishes made with one slot claimed", made, k);
        bool said = rootZcPublishedSinceClaim();
        if (said != (k > 0)) {
            misses++;
            printf("    after %d publishes (the count wraps at %d) the answer is %s\n", k, wrap, said ? "yes" : "no");
        }
        CHECK(said == (k > 0), "claim held through %d publishes: answered %s", k, said ? "yes" : "no");
        rendererReleaseRootBuffer();
    }

    /* 2. the lifecycle at random, as rootZcPresent keeps it: the slot submitted last (displayed) and the
     * ones before it waiting for their release (retiring) stay held; a claim of the displayed slot is
     * "nothing new"; a submit retires the displayed one; only retiring slots are given back. */
    init(&priv);
    fakeState.rootDoubleBuffered = 1;
    rendererRootSlot = -1;
    int retiring[LORIE_ROOT_SLOTS], nRetiring = 0, displayed = -1, claimed = -1, asked = 0, wrong = 0;
    int publishes = 0, refused = 0;
    uint64_t retiringId[LORIE_ROOT_SLOTS], displayedId = 0, claimedId = 0;
    bool truth = false;
    srand(29);
    for (int step = 0; step < 300000; step++) {
        switch (rand() % 6) {
        case 0:
        case 1: {                                   /* the X server, at a tick with new content */
            int held = __builtin_popcount(fakeState.rootHandover & LORIE_ROOT_HELD_MASK);
            CHECK(held <= LORIE_ZC_MAX_HELD + 1, "step %d: the renderer holds %d slots", step, held);
            Bool ok = lorieRootHandover(&priv);
            CHECK(ok || held > LORIE_ZC_MAX_HELD, "step %d: publish refused with the renderer holding %d of %d",
                  step, held, LORIE_ROOT_SLOTS);
            CHECK(!(fakeState.rootHandover & (1u << priv.rootWrite)), "step %d: the X server draws into held slot %d",
                  step, priv.rootWrite);
            if (ok) {
                publishes++;
                if (claimed >= 0) {
                    truth = true;
                    CHECK(newestSlot() != claimed, "step %d: published the claimed slot %d", step, claimed);
                }
            } else
                refused++;
            break;
        }
        case 2:                                     /* a frame claims the newest */
            if (claimed < 0) {
                claimedId = rendererClaimRootBuffer();
                claimed = rendererRootSlot;
                truth = false;
            }
            break;
        case 3:                                     /* it asks */
            if (claimed >= 0) {
                asked++;
                if (rootZcPublishedSinceClaim() != truth)
                    wrong++;
            }
            break;
        case 4:                                     /* the frame ends */
            if (claimed < 0)
                break;
            if (claimed == displayed)               /* nothing new: it stays held as the one shown */
                rendererRootSlot = -1;
            else if (nRetiring + 2 <= LORIE_ZC_MAX_HELD && rand() % 2) {
                if (displayed >= 0) {               /* submitted: the one shown before starts retiring */
                    retiring[nRetiring] = displayed;
                    retiringId[nRetiring++] = displayedId;
                }
                displayed = claimed;
                displayedId = claimedId;
                rendererRootSlot = -1;
            } else
                rendererReleaseRootBuffer();        /* dropped, or no room: the claim goes back */
            claimed = -1;
            break;
        case 5:                                     /* a retiring slot's release has come */
            if (nRetiring) {
                int i = rand() % nRetiring;
                rendererReleaseRootSlot(retiring[i], retiringId[i], LORIE_ROOT_GEN(fakeState.rootHandover));
                retiring[i] = retiring[--nRetiring];
                retiringId[i] = retiringId[nRetiring];
            }
            break;
        }
    }
    CHECK(wrong == 0, "random run: %d of %d answers wrong", wrong, asked);
    /* everything given back: nothing may stay held */
    if (claimed >= 0 && claimed != displayed)
        rendererReleaseRootBuffer();
    while (nRetiring) {
        nRetiring--;
        rendererReleaseRootSlot(retiring[nRetiring], retiringId[nRetiring], LORIE_ROOT_GEN(fakeState.rootHandover));
    }
    if (displayed >= 0)
        rendererReleaseRootSlot(displayed, displayedId, LORIE_ROOT_GEN(fakeState.rootHandover));
    CHECK((fakeState.rootHandover & LORIE_ROOT_HELD_MASK) == 0, "held bits left after everything was given back: %#x",
          fakeState.rootHandover & LORIE_ROOT_HELD_MASK);
    CHECK(asked > 10000 && publishes > 10000, "random run too thin: %d asked, %d published", asked, publishes);

    /* 3. the pool replaced while the claim is out */
    init(&priv);
    fakeState.rootDoubleBuffered = 1;
    rendererRootSlot = -1;
    rendererClaimRootBuffer();
    fakeState.rootHandover = (LORIE_ROOT_GEN(fakeState.rootHandover) + 2u) << LORIE_ROOT_GEN_SHIFT;
    CHECK(rootZcPublishedSinceClaim(), "pool replaced during the claim: answered no");

    printf("TSINCECLAIM published since the claim, against the real handover: %s (%d failures; count wraps at %d, "
           "%d single-claim misses; random run %d asked, %d published, %d refused)\n",
           fails ? "FAIL" : "PASS", fails, wrap, misses, asked, publishes, refused);
    return fails != 0;
}
