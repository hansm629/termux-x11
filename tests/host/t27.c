/* T27: claiming and releasing a root slot while the X server replaces the pool - with the replacement
 * forced into every point of the renderer's code. rendererClaimRootBuffer, rendererReleaseRootSlot and
 * rendererReleaseRootBuffer are extracted verbatim from renderer.c (gen.py); every atomic operation in
 * them, and every read of a buffer id, first runs a hook, and the hook runs the X server's steps at
 * whichever of those points a schedule names. Deterministic: one thread, every interleaving of the
 * replacement's steps with the renderer's operations tried in turn.
 *
 * The X server's replacement, as lorieEnsureRootDoubleBuffer does it: mark the generation odd, write
 * the new ids, reset the word (slot 0 published, nothing held, generation even). Code from before the
 * generation existed only wrote the ids and reset the word, and is driven that way - which is how the
 * test is shown to fail there. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sched.h>

static volatile uint64_t ids[5];
static void hook(void);
static volatile uint64_t *idsAt(void) { hook(); return ids; }
static struct {
    volatile uint32_t rootHandover;
    volatile uint64_t *(*rootBufferIdsFn)(void);
    volatile uint8_t rootDoubleBuffered;
    uint64_t rootWindowTextureID;
    struct { volatile uint32_t rootStaleSlotReleases, rootClaimsAcrossPools; } presentStats;
} fakeState = { .rootBufferIdsFn = idsAt };
static typeof(fakeState) *state = &fakeState;
static int64_t rendererNowNs(void) { return 0; }
#define log(...) printf(__VA_ARGS__)
#define rootBufferIds rootBufferIdsFn()
#define __atomic_load_n(p, o) (hook(), __atomic_load_n(p, o))
#define __atomic_compare_exchange_n(p, e, d, w, s, f) (hook(), __atomic_compare_exchange_n(p, e, d, w, s, f))
#define __atomic_thread_fence(o) (hook(), __atomic_thread_fence(o))
#include "slots_src.inc"
#undef __atomic_load_n
#undef __atomic_compare_exchange_n
#undef __atomic_thread_fence
#undef rootBufferIds

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 4) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define OLD_FIRST 10
#define NEW_FIRST 20
#define OLD_GEN 2u
static int hooksOn, hookStep;

/* the X server's three steps */
static uint32_t xGen;
static void xStep(int k) {
#ifdef LORIE_ROOT_GEN_SHIFT
    if (k == 0) {
        xGen = LORIE_ROOT_GEN(fakeState.rootHandover) | 1u;
        fakeState.rootHandover = (fakeState.rootHandover & 0xffffu) | (xGen << LORIE_ROOT_GEN_SHIFT);
    } else if (k == 1) {
        for (int i = 0; i < LORIE_ROOT_SLOTS; i++) ids[i] = NEW_FIRST + i;
    } else
        fakeState.rootHandover = ((xGen + 1u) & 0xffffu) << LORIE_ROOT_GEN_SHIFT;
#else
    if (k == 1)
        for (int i = 0; i < LORIE_ROOT_SLOTS; i++) ids[i] = NEW_FIRST + i;
    else if (k == 2)
        fakeState.rootHandover = 0;
#endif
}
static void xPublish(int slot) {
    fakeState.rootHandover = (fakeState.rootHandover & ~(LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT))
                             | ((uint32_t) slot << LORIE_ROOT_NEWEST_SHIFT);
}
static void oldPool(void) {
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) ids[i] = OLD_FIRST + i;
    fakeState.rootHandover = (OLD_GEN << 16) | (2u << LORIE_ROOT_NEWEST_SHIFT);   /* slot 2 published */
}

/* claim schedules: X step k runs just before the renderer's xAt[k]-th hooked operation */
static int xAt[3], xDone;
/* release schedule: the whole replacement, then a new claim of the same index, at one point */
static int eventAt = -1, eventDone;
static void replaceAndReclaim(int slot);
static void hook(void) {
    if (!hooksOn)
        return;
    while (xDone < 3 && xAt[xDone] <= hookStep)
        xStep(xDone++);
    if (!eventDone && eventAt == hookStep) {
        eventDone = 1;
        replaceAndReclaim(2);
    }
    hookStep++;
}
static void replaceAndReclaim(int slot) {
    int saved = hooksOn;
    hooksOn = 0;
    for (int k = 0; k < 3; k++) xStep(k);
    xPublish(slot);
    rendererClaimRootBuffer();               /* the renderer's next frame takes the same index */
    hooksOn = saved;
}
static void release(int slot, uint64_t id, uint32_t gen) {
#ifdef LORIE_ROOT_GEN_SHIFT
    rendererReleaseRootSlot(slot, id, gen);
#else
    (void) gen;
    rendererReleaseRootSlot(slot, id);
#endif
}

int main(void) {
    const int K = 16;
    int schedules = 0, straddled = 0;
    fakeState.rootDoubleBuffered = 1;

    /* A claim with the replacement's three steps at every combination of points in it. Whatever it
     * returns must be safe: a slot of the new pool only with its held bit set in the new word (or the X
     * server can draw into what is being sampled), a slot of the old pool only with a release that then
     * leaves the new word alone. */
    for (int p1 = 0; p1 <= K; p1++)
        for (int p2 = p1; p2 <= K; p2++)
            for (int p3 = p2; p3 <= K; p3++) {
                oldPool();
                xAt[0] = p1; xAt[1] = p2; xAt[2] = p3; xDone = 0;
                hookStep = 0; hooksOn = 1;
                uint64_t id = rendererClaimRootBuffer();
                hooksOn = 0;
                while (xDone < 3)
                    xStep(xDone++);
                schedules++;
                int s = rendererRootSlot;
                if (id >= NEW_FIRST) {
                    CHECK(s >= 0 && (fakeState.rootHandover & (1u << s)),
                          "x at %d,%d,%d: claimed slot %d of the new pool (id %llu) with its held bit clear - "
                          "the X server can draw into it", p1, p2, p3, s, (unsigned long long) id);
                } else {
                    straddled++;
                    uint32_t before = fakeState.rootHandover;
                    rendererReleaseRootBuffer();
                    CHECK(fakeState.rootHandover == before,
                          "x at %d,%d,%d: releasing old-pool slot %d changed the new pool's word", p1, p2, p3, s);
                }
            }

    /* A release of an old-pool slot - one that went to the compositor before the replacement - with the
     * replacement and the renderer's claim of the same index in the new pool forced into every point of
     * it. The new claim's bit must survive. */
    int releases = 0;
    for (int p = 0; p <= K; p++) {
        oldPool();
        hooksOn = 0;
        rendererClaimRootBuffer();                    /* slot 2 of the old pool */
        int slot = rendererRootSlot;
        uint64_t id = rendererRootSlotId;
        uint32_t gen = rendererRootSlotGen;
        rendererRootSlot = -1;                         /* handed to the compositor */
        eventAt = p; eventDone = 0; hookStep = 0; hooksOn = 1;
        release(slot, id, gen);                        /* its release arrives */
        hooksOn = 0;
        if (!eventDone)
            replaceAndReclaim(2);                      /* the release came first: nothing to race */
        releases++;
        CHECK(fakeState.rootHandover & (1u << 2),
              "replacement at %d: the old pool's release cleared the new claim of slot 2 - the X server "
              "can draw into it", p);
    }

    printf("T27 pool replacement during claim/release: %s (%d failures; %d claim schedules, %d of them "
           "ending on the old pool; %d release schedules)\n",
           fails ? "FAIL" : "PASS", fails, schedules, straddled, releases);
    return fails != 0;
}
