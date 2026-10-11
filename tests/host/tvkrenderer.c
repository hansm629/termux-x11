/* TVKRENDERER: the renderer's choice of backend and its side of the AHB ownership handshake, from the real code
 * (renderer.c rendererBackendFor, rendererVkOwnership, rendererVkOwnedLocked: gen.py "vkrenderer"), with a thread
 * playing the X server (InitOutput.c lorieProcessOwnershipRequest's part: answer under no lock, signal the
 * renderer's condition variable).
 *
 *   Backend: gles asked - GLES; auto or vulkan - Vulkan until it failed in the session; after a failure GLES in
 *   auto, nobody in vulkan (an explicit failure, no GLES fallback). A buffer gets its GLES texture when it comes
 *   only in a session GLES draws: gles asked, or auto after a failure (otherwise when GLES first uses it).
 *   Ownership: the request is one past the last answer, with the mode asked for; answered PER_ACCESS - owned,
 *   under that sequence number, and no longer once the X server answers otherwise (a new connection); refused,
 *   or not answered within a second - not owned; answered late but in time - owned; PERSISTENT asked and
 *   answered - not owned. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "diagtypes_src.inc"
struct lorie_shared_server_state { struct lorie_renderer_diag diag; struct lorie_backend_control backend; volatile uint8_t tickOpen; };
static volatile struct lorie_shared_server_state *state;
static pthread_mutex_t stateLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER, *stateCond = &cond;
static bool vkOwned, vkActive, vkFailed;
static uint32_t vkOwnershipSeq;
#define loge(...) ((void) 0)
#include "vkrenderer_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* the X server: answers the next request after `delayMs`, with `answer` (or the mode asked for when -1) */
static struct { int delayMs, answer; bool silent; } x;
static void *xServer(void *unused) {
    uint32_t req, last = 0;
    (void) unused;
    for (int spins = 0; spins < 3000; spins++) {
        req = __atomic_load_n(&state->backend.ownershipRequest, __ATOMIC_ACQUIRE);
        if (LORIE_OWNERSHIP_SEQ(req) != LORIE_OWNERSHIP_SEQ(__atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE)) && req != last) {
            last = req;
            if (x.silent)
                return NULL;
            usleep(x.delayMs * 1000);
            __atomic_store_n(&state->backend.ownershipAck,
                             LORIE_OWNERSHIP_WORD(LORIE_OWNERSHIP_SEQ(req), x.answer < 0 ? LORIE_OWNERSHIP_MODE(req) : x.answer), __ATOMIC_RELEASE);
            pthread_cond_signal(stateCond);
            return NULL;
        }
        usleep(1000);
    }
    return NULL;
}

static long msSince(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}
/* the X server ends its tick after 1 ms: tickOpen cleared, the renderer woken */
static void *tickEnds(void *unused) {
    (void) unused;
    usleep(1000);
    __atomic_store_n(&state->tickOpen, 0, __ATOMIC_RELEASE);
    pthread_cond_signal(stateCond);
    return NULL;
}

static bool ask(uint8_t mode, int delayMs, int answer, bool silent, long *tookMs) {
    pthread_t t;
    struct timespec t0, t1;
    bool ok;
    x.delayMs = delayMs;
    x.answer = answer;
    x.silent = silent;
    pthread_create(&t, NULL, xServer, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&stateLock);
    ok = rendererVkOwnership(mode);
    pthread_mutex_unlock(&stateLock);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_join(t, NULL);
    *tookMs = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    return ok;
}

int main(void) {
    static struct lorie_shared_server_state s;
    long ms;
    uint32_t seq;

    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(30);          /* a wait that never ends fails the test instead of hanging it */

    /* who draws */
    CHECK(rendererBackendFor(LORIE_BACKEND_GLES, false) == RENDERER_USE_GLES && rendererBackendFor(LORIE_BACKEND_GLES, true) == RENDERER_USE_GLES,
          "gles asked: not GLES");
    CHECK(rendererBackendFor(LORIE_BACKEND_AUTO, false) == RENDERER_USE_VULKAN && rendererBackendFor(LORIE_BACKEND_VULKAN, false) == RENDERER_USE_VULKAN,
          "auto or vulkan, nothing failed: not Vulkan");
    CHECK(rendererBackendFor(LORIE_BACKEND_AUTO, true) == RENDERER_USE_GLES, "auto after a failure: not GLES");
    CHECK(rendererBackendFor(LORIE_BACKEND_VULKAN, true) == RENDERER_USE_NONE, "vulkan after a failure: not an explicit failure");

    /* GLES textures when a buffer comes */
    CHECK(rendererGlesEager(), "no session: no GLES texture when a buffer comes");
    state = &s;
    s.backend.requested = LORIE_BACKEND_GLES;
    CHECK(rendererGlesEager(), "gles: no GLES texture when a buffer comes");
    s.backend.requested = LORIE_BACKEND_AUTO;
    CHECK(!rendererGlesEager(), "auto, Vulkan not failed: a GLES texture made though Vulkan may draw");
    vkActive = true;
    CHECK(!rendererGlesEager(), "Vulkan drawing: a GLES texture made");
    vkActive = false;
    vkFailed = true;
    CHECK(rendererGlesEager(), "auto after a failure: no GLES texture when a buffer comes");
    s.backend.requested = LORIE_BACKEND_VULKAN;
    CHECK(!rendererGlesEager(), "vulkan after a failure (nothing draws): a GLES texture made");
    vkFailed = false;
    s.backend.requested = LORIE_BACKEND_AUTO;

    /* the handshake */
    state = &s;
    s.backend.ownershipAck = LORIE_OWNERSHIP_WORD(41, LORIE_OWNERSHIP_PERSISTENT);
    CHECK(ask(LORIE_OWNERSHIP_PER_ACCESS, 0, -1, false, &ms) && vkOwned && vkOwnershipSeq == 42 &&
          s.backend.ownershipRequest == LORIE_OWNERSHIP_WORD(42, LORIE_OWNERSHIP_PER_ACCESS), "answered PER_ACCESS: not owned as 42");
    pthread_mutex_lock(&stateLock);
    CHECK(rendererVkOwnedLocked(), "answered PER_ACCESS: not owned under the lock");
    s.backend.ownershipAck = LORIE_OWNERSHIP_WORD(42, LORIE_OWNERSHIP_PERSISTENT);     /* a new connection: the X server's own again */
    CHECK(!rendererVkOwnedLocked(), "X server back to PERSISTENT: still owned");
    s.backend.ownershipAck = LORIE_OWNERSHIP_WORD(43, LORIE_OWNERSHIP_PER_ACCESS);     /* someone else's request */
    CHECK(!rendererVkOwnedLocked(), "another request's answer taken as ours");
    pthread_mutex_unlock(&stateLock);

    seq = LORIE_OWNERSHIP_SEQ(s.backend.ownershipAck);
    CHECK(!ask(LORIE_OWNERSHIP_PER_ACCESS, 0, LORIE_OWNERSHIP_PERSISTENT, false, &ms) && !vkOwned, "refused: owned");
    CHECK(LORIE_OWNERSHIP_SEQ(s.backend.ownershipRequest) == seq + 1, "refused: request not one past the last answer");

    CHECK(ask(LORIE_OWNERSHIP_PER_ACCESS, 300, -1, false, &ms) && vkOwned && ms >= 250, "answered after 300 ms: not owned (%ld ms)", ms);

    CHECK(!ask(LORIE_OWNERSHIP_PER_ACCESS, 0, -1, true, &ms) && !vkOwned && ms >= 900 && ms < 2500,
          "never answered: owned, or waited %ld ms instead of about a second", ms);

    s.backend.ownershipAck = s.backend.ownershipRequest;    /* let the next one be answered */
    CHECK(ask(LORIE_OWNERSHIP_PERSISTENT, 0, -1, false, &ms) && !vkOwned &&
          LORIE_OWNERSHIP_MODE(s.backend.ownershipRequest) == LORIE_OWNERSHIP_PERSISTENT, "PERSISTENT asked and answered: owned");

    /* copies queued in an X server tick: held until it ends (bounded) */
    {
        pthread_t t;
        struct timespec t0;
        long took;
        bool ended;
        s.tickOpen = 0;
        CHECK(!rendererVkTickCopies(), "no tick: copies held");
        s.tickOpen = 1;
        pthread_create(&t, NULL, tickEnds, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ended = rendererVkTickCopies();
        took = msSince(&t0);
        pthread_join(t, NULL);
        CHECK(ended && took < 4, "tick ending after 1 ms: %s after %ld ms", ended ? "ended" : "not ended", took);
        s.tickOpen = 1;     /* never ends */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ended = rendererVkTickCopies();
        took = msSince(&t0);
        CHECK(!ended && took >= 3 && took < 50, "tick never ending: %s after %ld ms (want given up after about 4)",
              ended ? "ended" : "not ended", took);
        s.tickOpen = 0;
    }

    printf("TVKRENDERER backend choice and ownership handshake: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
