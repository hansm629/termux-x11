/* TVKRENDERER: the renderer's choice of backend and its side of the AHB ownership handshake, from the real code
 * (renderer.c rendererBackendFor, rendererVkOwnership, rendererVkOwnedLocked: gen.py "vkrenderer"), with a thread
 * playing the X server (InitOutput.c lorieProcessOwnershipRequest's part: answer under no lock, then wake the
 * renderer - lorie.h lorieWakeRenderer, the real one; the renderer sleeps in the real rendererSleep).
 *
 *   Backend: gles asked - GLES; auto or vulkan - Vulkan until it failed in the session; after a failure GLES in
 *   auto, nobody in vulkan (an explicit failure, no GLES fallback). A buffer gets its GLES texture when it comes
 *   only in a session GLES draws: gles asked, or auto after a failure (otherwise when GLES first uses it).
 *   Ownership: the request is one past the last answer, with the mode asked for; answered PER_ACCESS - owned,
 *   under that sequence number, and no longer once the X server answers otherwise (a new connection); refused,
 *   or not answered within a second - not owned; answered late but in time - owned; PERSISTENT asked and
 *   answered - not owned. */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif

#include "diagtypes_src.inc"
struct lorie_shared_server_state { struct lorie_renderer_diag diag; struct lorie_backend_control backend; };
static volatile struct lorie_shared_server_state *state;
static pthread_mutex_t stateLock = PTHREAD_MUTEX_INITIALIZER;
#include "wake_src.inc"
static struct lorie_renderer_wake wakeRegion, *rendererWake = &wakeRegion;
static bool vkOwned, vkActive, vkFailed;
static uint32_t vkOwnershipSeq;
#define loge(...) ((void) 0)
/* P11-C3-C: points forced in the renderer's wait, each once - as its sleep starts (rendererSleep letting stateLock go),
 * and once its look found no answer (the clock read after it: the second call, the first is for the deadline) */
static void (*hookSleepStart)(void), (*hookLookDone)(void);
static int clockCalls;
static int hookedMutexUnlock(pthread_mutex_t *m) {
    void (*f)(void) = hookSleepStart;
    hookSleepStart = NULL;
    if (m == &stateLock && f)
        f();
    return pthread_mutex_unlock(m);
}
static uint64_t hookedMonotonicUs(void) {
    void (*f)(void) = hookLookDone;
    if (f && ++clockCalls == 2) {
        hookLookDone = NULL;
        f();
    }
    return lorieMonotonicUs();
}
#define pthread_mutex_unlock hookedMutexUnlock
#define lorieMonotonicUs() hookedMonotonicUs()
#include "vkrenderer_src.inc"
#undef pthread_mutex_unlock
#undef lorieMonotonicUs

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* the X server: answers the next request after `delayMs`, with `answer` (or the mode asked for when -1) - not the one
 * there before this case asked (`stale`: a case may have changed the answer since) */
static struct { int delayMs, answer; bool silent; uint32_t stale; } x;
static void *xServer(void *unused) {
    uint32_t req, last = x.stale;
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
            lorieWakeRenderer(rendererWake);
            return NULL;
        }
        usleep(1000);
    }
    return NULL;
}

/* the X server's answer as lorieProcessOwnershipRequest gives it - the mode asked for - then its wake */
static void answerNow(void) {
    uint32_t req = __atomic_load_n(&state->backend.ownershipRequest, __ATOMIC_ACQUIRE);
    __atomic_store_n(&state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(LORIE_OWNERSHIP_SEQ(req), LORIE_OWNERSHIP_MODE(req)), __ATOMIC_RELEASE);
    lorieWakeRenderer(rendererWake);
}

static bool ask(uint8_t mode, int delayMs, int answer, bool silent, long *tookMs) {
    pthread_t t;
    struct timespec t0, t1;
    bool ok;
    x.delayMs = delayMs;
    x.answer = answer;
    x.silent = silent;
    x.stale = __atomic_load_n(&state->backend.ownershipRequest, __ATOMIC_ACQUIRE);
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

    /* P11-C3-C: answered between the renderer's look and its sleep - owned at once, not "not answered" a second later */
    s.backend.ownershipAck = s.backend.ownershipRequest;
    hookSleepStart = answerNow;
    CHECK(ask(LORIE_OWNERSHIP_PER_ACCESS, 0, -1, true, &ms) && vkOwned && ms < 300,
          "answered as the sleep starts: slept through it (%ld ms, owned %d)", ms, vkOwned);
    s.backend.ownershipAck = s.backend.ownershipRequest;
    clockCalls = 0;
    hookLookDone = answerNow;
    CHECK(ask(LORIE_OWNERSHIP_PER_ACCESS, 0, -1, true, &ms) && vkOwned && ms < 300,
          "answered once the look found none, before the sleep: slept through it (%ld ms, owned %d)", ms, vkOwned);

    s.backend.ownershipAck = s.backend.ownershipRequest;    /* let the next one be answered */
    CHECK(ask(LORIE_OWNERSHIP_PERSISTENT, 0, -1, false, &ms) && !vkOwned &&
          LORIE_OWNERSHIP_MODE(s.backend.ownershipRequest) == LORIE_OWNERSHIP_PERSISTENT, "PERSISTENT asked and answered: owned");

    printf("TVKRENDERER backend choice and ownership handshake: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
