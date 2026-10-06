/* T19: root slot claim/release across a pool replacement. Functions extracted verbatim from renderer.c. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sched.h>
#include <pthread.h>
#include "rootslots_src.inc"
static struct {
    volatile uint32_t rootHandover;
    volatile uint64_t rootBufferIds[LORIE_ROOT_SLOTS];
    volatile uint8_t rootDoubleBuffered;
    uint64_t rootWindowTextureID;
    struct { volatile uint32_t rootStaleSlotReleases, rootClaimsAcrossPools; } presentStats;
} fakeState;
static typeof(fakeState) *state = &fakeState;
static int64_t rendererNowNs(void) { return 0; }
#define log(...) printf(__VA_ARGS__)
#include "slots_src.inc"
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What the X server does when it replaces the pool (InitOutput.c lorieEnsureRootDoubleBuffer): the
 * generation marked odd, new ids, then the whole word reset with slot 0 published and the generation
 * even again. (Code from before the generation existed only wrote the ids and reset the mask.) */
static void xReplacePool(uint64_t firstId) {
#ifdef LORIE_ROOT_GEN_SHIFT
    uint32_t gen = LORIE_ROOT_GEN(fakeState.rootHandover) | 1u;
    fakeState.rootHandover = (fakeState.rootHandover & 0xffffu) | (gen << LORIE_ROOT_GEN_SHIFT);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = firstId + i;
    __atomic_store_n(&fakeState.rootHandover, ((gen + 1u) & 0xffffu) << LORIE_ROOT_GEN_SHIFT, __ATOMIC_RELEASE);
#else
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) fakeState.rootBufferIds[i] = firstId + i;
    __atomic_store_n(&fakeState.rootHandover, 0u, __ATOMIC_RELEASE);
#endif
}
static void xPublish(int slot) {
    uint32_t old = __atomic_load_n(&fakeState.rootHandover, __ATOMIC_ACQUIRE);
    fakeState.rootHandover = (old & ~(LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT)) | ((uint32_t) slot << LORIE_ROOT_NEWEST_SHIFT);
}
#define HELD(i) ((fakeState.rootHandover >> (i)) & 1u)

int main(void) {
    fakeState.rootDoubleBuffered = 1;

    /* 1. ordinary claim and release clears the bit */
    xReplacePool(5); xPublish(2);
    uint64_t id = rendererClaimRootBuffer();
    CHECK(id == 7 && rendererRootSlot == 2 && HELD(2), "claim slot 2 id %llu", (unsigned long long) id);
    rendererReleaseRootBuffer();
    CHECK(!HELD(2) && rendererRootSlot == -1, "release clears slot 2");

    /* 2. the case from the device log: slot 0 of pool A is with the compositor, the pool is replaced,
     *    the renderer claims slot 0 of pool B (now on screen), then pool A's slot 0 release arrives */
    xReplacePool(5); xPublish(0);
    id = rendererClaimRootBuffer();                 /* pool A slot 0, id 5 */
    int oldSlot = rendererRootSlot; uint64_t oldId = rendererRootSlotId;
    uint32_t oldGen = rendererRootSlotGen; (void) oldGen;
    rendererRootSlot = -1;                          /* handed to the compositor, as rootZcPresent does */
    xReplacePool(319);                              /* X replaces the pool; mask cleared */
    id = rendererClaimRootBuffer();                 /* pool B slot 0, id 319, the new frame on screen */
    CHECK(id == 319 && HELD(0), "claimed new pool slot 0 id %llu", (unsigned long long) id);
    uint32_t newGen = rendererRootSlotGen; (void) newGen;
    rendererRootSlot = -1;
    uint32_t staleBefore = fakeState.presentStats.rootStaleSlotReleases;
#ifdef LORIE_ROOT_GEN_SHIFT
    rendererReleaseRootSlot(oldSlot, oldId, oldGen); /* the old pool's release finally arrives */
#else
    rendererReleaseRootSlot(oldSlot, oldId);
#endif
    CHECK(HELD(0), "old pool release cleared the new pool's slot 0 - X could now draw into what is on screen");
    CHECK(fakeState.presentStats.rootStaleSlotReleases == staleBefore + 1, "stale release counted");

    /* 3. the same release for the current pool still works */
#ifdef LORIE_ROOT_GEN_SHIFT
    rendererReleaseRootSlot(0, 319, newGen);
#else
    rendererReleaseRootSlot(0, 319);
#endif
    CHECK(!HELD(0), "current pool release clears its bit");

    printf("T19 slot release across pool replacement: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
