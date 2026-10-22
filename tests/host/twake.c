/* TWAKE: the renderer's wait for work and its wake (P11-C3-C), from the real code - lorie.h lorieWakeRenderer and
 * lorieWakeSeen (gen.py "wake"); renderer.c rendererSleep, rendererShouldWait with the root copy and the copies left
 * queued for a reservation it asks about, rendererWaitForWork and rendererAddBuffer (gen.py "rendererwait"). The X
 * server's part is played as InitOutput.c plays it: its work published into the shared state, then lorieWakeRenderer.
 *
 * Each wake comes at a point forced in the renderer's own steps: as its look starts (the first thing rendererShouldWait
 * does is take bufferLock), once the look has read the buffer lists (bufferLock let go: a buffer added then is one the
 * look has missed - after the look, before the sleep, the window a condition variable signalled without its mutex
 * loses), as its sleep starts (rendererSleep letting stateLock go) - or from another thread, or another process through
 * a shared mapping. A renderer still asleep 500 ms after its work was published slept through it: the test's rescuer
 * moves the count with a wake of its own, and the case fails.
 *
 *   Published before the look, as it starts, after it, as the sleep starts, during the sleep: each taken up, none slept
 *   through - a draw, a GPU copy, a root copy, a cursor move, a buffer. 20000 from another thread at random times: all
 *   taken up. During a pass: the next wait returns at once. No window: asleep (not polling) until a window comes. A
 *   root copy asked for: no sleep; taken: asleep. Copies left queued for a reservation: asleep, not polling, until the
 *   reservation is let go - also let go between the look and the sleep. From another process, the renderer asleep, and
 *   between its look and its sleep. The delay a coalesced redraw asks for: waited, cut short by a wake. And the previous
 *   protocol, modelled - its look, then pthread_cond_wait, the signal sent between them - shown to sleep through it.
 */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif

#include "xorglist_src.inc"
#include "diagtypes_src.inc"
#include "gpucopytypes_src.inc"
#include "wake_src.inc"

/* the shared state, as far as the code under test reads it (lorie.h's fields) */
struct lorie_shared_server_state {
    struct {
        volatile uint32_t writeIndex;
        volatile uint32_t readIndex;
    } gpuCopyQueue;
    struct {
        uint32_t count;
        uint64_t ids[8];
        volatile uint32_t releases, waiter;
    } cpuAccess;
    struct {
        volatile uint32_t seq, taken, cancelled, failed, done;
    } rootCopy;
    uint64_t rootWindowTextureID;
    volatile uint8_t drawRequested, surfaceAvailable, waitForNextFrame;
    struct {
        volatile uint8_t updated, moved;
    } cursor;
    struct lorie_backend_control backend;     /* P11-C3-C: the options the stage and the asks are kept for */
    struct lorie_progress progress;
};
static volatile struct lorie_shared_server_state *state;
static struct lorie_renderer_wake *rendererWake;

/* the renderer's own state the look reads */
typedef struct LorieBuffer { struct xorg_list link; } LorieBuffer;
#define LorieBuffer_addToList(buf, list) xorg_list_add(&(buf)->link, (struct xorg_list *) (list))
static struct xorg_list addedBuffers, removedBuffers;
static pthread_spinlock_t bufferLock;
static pthread_mutex_t stateLock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool stateChanged, windowChanged, expectedSizeChanged;
static bool backendSelect;
static int rendererPreRedrawCoalesceFrames, rendererHighRefreshLimitFrames;
static int64_t rendererPreRedrawCoalesceWaitUs, rendererHighRefreshLimitWaitUs;
static int64_t rendererLastPreRedrawCoalesceWaitUs = -1, rendererLastHighRefreshLimitWaitUs = -1;
static uint64_t rendererPreRedrawCoalescedCount, rendererHighRefreshLimitedCount;

/* the forced points: each hook runs once, on the renderer thread, then is cleared */
static pthread_t rendererThread;
static void (*hookLookStart)(void), (*hookLookRead)(void), (*hookSleepStart)(void);
static int looks, sleeps, inHook;
static void fire(void (**h)(void)) {
    void (*f)(void) = *h;
    *h = NULL;
    if (f) {
        inHook++;
        f();
        inHook--;
    }
}
static int hookedSpinLock(pthread_spinlock_t *l) {
    if (l == &bufferLock && pthread_equal(pthread_self(), rendererThread) && !inHook) {
        looks++;
        fire(&hookLookStart);
    }
    return pthread_spin_lock(l);
}
static int hookedSpinUnlock(pthread_spinlock_t *l) {
    int r = pthread_spin_unlock(l);
    if (l == &bufferLock && pthread_equal(pthread_self(), rendererThread) && !inHook)
        fire(&hookLookRead);
    return r;
}
static int hookedMutexUnlock(pthread_mutex_t *m) {
    if (m == &stateLock && pthread_equal(pthread_self(), rendererThread) && !inHook) {
        sleeps++;
        fire(&hookSleepStart);
    }
    return pthread_mutex_unlock(m);
}
#define pthread_spin_lock hookedSpinLock
#define pthread_spin_unlock hookedSpinUnlock
#define pthread_mutex_unlock hookedMutexUnlock
#include "rendererwait_src.inc"
#undef pthread_spin_lock
#undef pthread_spin_unlock
#undef pthread_mutex_unlock

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* the rescuer's own wake - its count moved and every sleeper on it woken - whatever lorieWakeRenderer does */
static void rescueWake(void) {
    __atomic_fetch_add(&rendererWake->seq, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &rendererWake->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static volatile int caseDone, rescued;
static long rescueAfterMs = 500;
static void *rescuer(void *arg) {
    long start = nowMs();
    (void) arg;
    while (!__atomic_load_n(&caseDone, __ATOMIC_ACQUIRE)) {
        usleep(2000);
        if (nowMs() - start >= rescueAfterMs) {
            rescued = 1;
            rescueWake();
        }
    }
    return NULL;
}

static struct lorie_shared_server_state st;
static struct lorie_renderer_wake wakeRegion;
static LorieBuffer bufs[4];
static bool waitingForBuffers;
static void reset(void) {
    memset(&st, 0, sizeof(st));
    state = &st;
    rendererWake = &wakeRegion;
    st.surfaceAvailable = 1;
    stateChanged = windowChanged = expectedSizeChanged = backendSelect = false;
    deferredCopies = false;
    xorg_list_init(&addedBuffers);
    xorg_list_init(&removedBuffers);
    hookLookStart = hookLookRead = hookSleepStart = NULL;
    looks = sleeps = 0;
    waitingForBuffers = false;
    rendererPreRedrawCoalesceFrames = rendererHighRefreshLimitFrames = 0;
    rendererPreRedrawCoalesceWaitUs = rendererHighRefreshLimitWaitUs = 0;
}

/* the renderer thread's wait for work, from now on: how long it took */
static long waitForWork(void) {
    pthread_t r;
    long t0;
    caseDone = rescued = 0;
    rendererThread = pthread_self();
    pthread_create(&r, NULL, rescuer, NULL);
    t0 = nowMs();
    pthread_mutex_lock(&stateLock);
    rendererWaitForWork(&waitingForBuffers);
    pthread_mutex_unlock(&stateLock);
    t0 = nowMs() - t0;
    __atomic_store_n(&caseDone, 1, __ATOMIC_RELEASE);
    pthread_join(r, NULL);
    return t0;
}

/* the X server's work, published as InitOutput.c publishes it, then the wake */
static void xDraw(void) { state->drawRequested = 1; lorieWakeRenderer(rendererWake); }                       /* lorieRedraw */
static void xCopy(void) {                                                                                  /* lorieTryScheduleGpuCopy */
    __atomic_store_n(&state->gpuCopyQueue.writeIndex, state->gpuCopyQueue.writeIndex + 1, __ATOMIC_RELEASE);
    lorieWakeRenderer(rendererWake);
}
static void xRootCopy(void) {                                                                              /* lorieRootGpuCopyAsk */
    __atomic_store_n(&state->rootCopy.seq, state->rootCopy.seq + 1, __ATOMIC_RELEASE);
    lorieWakeRenderer(rendererWake);
}
static void xCursor(void) { state->cursor.moved = 1; lorieWakeRenderer(rendererWake); }                      /* lorieMoveCursor */
static void xRelease(void) {                                                                               /* lorieCpuRelease, lorieCpuReleasedWake */
    __atomic_fetch_add(&state->cpuAccess.releases, 1, __ATOMIC_RELEASE);
    lorieWakeRenderer(rendererWake);
}
static void appBuffer(void) { rendererAddBuffer(&bufs[0]); }                                                 /* activity.c, the real one */

/* the same from another thread, after `ms` */
struct later { void (*what)(void); long ms; };
static void *laterThread(void *arg) {
    struct later *l = arg;
    usleep(l->ms * 1000);
    l->what();
    return NULL;
}

/* the stage the renderer gave itself as its sleep starts (P11-C3-C) */
static uint32_t stageAtSleep = LORIE_R_STAGES;
static void noteStage(void) { stageAtSleep = state->progress.rStage; }

/* 20000 copies queued from another thread at random times */
static volatile int xDone;
static void *xBurst(void *arg) {
    (void) arg;
    for (int k = 0; k < 20000; k++) {
        xCopy();
        if (rand() % 8 == 0)
            usleep(rand() % 40);
    }
    __atomic_store_n(&xDone, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* rendererSetWindow's part: the change, then the wake, under stateLock */
static void windowComes(void) {
    pthread_mutex_lock(&stateLock);
    windowChanged = true;
    lorieWakeRenderer(rendererWake);
    pthread_mutex_unlock(&stateLock);
}

/* one forced point, one kind of work: taken up, not slept through */
static void forced(const char *point, void (**hook)(void), const char *kind, void (*what)(void)) {
    long ms;
    reset();
    *hook = what;
    ms = waitForWork();
    CHECK(!rescued && ms < 250 && !*hook, "%s published %s: slept through it (%ld ms, rescued %d)", kind, point, ms, rescued);
}

/* ---- the previous protocol, modelled: its look, then pthread_cond_wait - a signal sent between them, as the X server
 * sent it, without the mutex the look ran under ---- */
static pthread_cond_t oldCond = PTHREAD_COND_INITIALIZER;
static void oldSignal(void) { st.drawRequested = 1; pthread_cond_signal(&oldCond); }
static void *oldRescuer(void *arg) {
    long start = nowMs();
    (void) arg;
    while (!__atomic_load_n(&caseDone, __ATOMIC_ACQUIRE)) {
        usleep(2000);
        if (nowMs() - start >= rescueAfterMs) {
            rescued = 1;
            pthread_mutex_lock(&stateLock);
            pthread_cond_signal(&oldCond);
            pthread_mutex_unlock(&stateLock);
        }
    }
    return NULL;
}

/* ---- another process: the shared mapping holds the state and the count, as memfd regions do ---- */
struct shared { struct lorie_shared_server_state st; struct lorie_renderer_wake wake; volatile int asleep, published; };
static struct shared *shm;
static void announceSleep(void) {
    __atomic_store_n(&shm->asleep, 1, __ATOMIC_RELEASE);
}
static void awaitPublished(void) {
    __atomic_store_n(&shm->asleep, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&shm->published, __ATOMIC_ACQUIRE))
        usleep(500);
}

int main(void) {
    struct later l;
    pthread_t t;
    long ms;
    int i;

    alarm(120);
    pthread_spin_init(&bufferLock, 0);

    /* published before the look */
    reset();
    xDraw();
    ms = waitForWork();
    CHECK(!rescued && sleeps == 0, "a draw published before the look: slept (%d sleeps, %ld ms)", sleeps, ms);

    /* at each forced point, each kind of work */
    {
        static struct { const char *kind; void (*what)(void); } kinds[] = {
            { "a draw", xDraw }, { "a GPU copy", xCopy }, { "a root copy", xRootCopy }, { "a cursor move", xCursor },
            { "a buffer", appBuffer },
        };
        for (i = 0; i < (int) (sizeof(kinds) / sizeof(kinds[0])); i++) {
            forced("as the look starts", &hookLookStart, kinds[i].kind, kinds[i].what);
            forced("as the sleep starts", &hookSleepStart, kinds[i].kind, kinds[i].what);
        }
        /* after the look read the buffer lists, before it decided and slept: the lost-wakeup window */
        forced("after the look read it, before the sleep", &hookLookRead, "a buffer", appBuffer);
        CHECK(sleeps == 1 && looks == 2, "a buffer after the look: %d sleeps, %d looks (want 1, 2)", sleeps, looks);
    }

    /* during the sleep, from another thread */
    reset();
    l = (struct later) { xDraw, 60 };
    pthread_create(&t, NULL, laterThread, &l);
    ms = waitForWork();
    pthread_join(t, NULL);
    CHECK(!rescued && ms >= 40 && ms < 400 && sleeps >= 1, "a draw during the sleep: %ld ms, rescued %d, %d sleeps", ms, rescued, sleeps);

    /* 20000 from another thread at random times, each taken up: the renderer takes the queue up to where it is */
    {
        uint32_t taken = 0;
        long start;
        reset();
        rescueAfterMs = 2000;
        xDone = 0;
        srand(7);
        pthread_create(&t, NULL, xBurst, NULL);
        start = nowMs();
        while (taken < 20000 && nowMs() - start < 20000) {
            ms = waitForWork();
            if (rescued)
                break;
            taken = __atomic_load_n(&st.gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);
            st.gpuCopyQueue.readIndex = taken;      /* the pass: the copies taken up */
            if (rand() % 4 == 0)
                usleep(rand() % 60);
        }
        pthread_join(t, NULL);
        CHECK(taken == 20000 && !rescued, "20000 from another thread: %u taken up, rescued %d", taken, rescued);
        rescueAfterMs = 500;
    }

    /* during a pass: the next wait returns at once */
    reset();
    xDraw();
    waitForWork();
    st.drawRequested = 0;           /* the pass took it up ... */
    xCopy();                        /* ... and meanwhile the X server queued a copy */
    sleeps = 0;
    ms = waitForWork();
    CHECK(!rescued && sleeps == 0, "a copy queued during the pass: the wait after it slept (%d sleeps, %ld ms)", sleeps, ms);

    /* no window to draw in: asleep, not polling, until one comes (rendererSetWindow: windowChanged, then the wake) */
    {
        reset();
        st.surfaceAvailable = 0;
        st.drawRequested = 1;
        l = (struct later) { windowComes, 120 };
        pthread_create(&t, NULL, laterThread, &l);
        ms = waitForWork();
        pthread_join(t, NULL);
        CHECK(!rescued && ms >= 100 && looks <= 3, "no window, then one: %ld ms, %d looks (polling?), rescued %d", ms, looks, rescued);
    }

    /* a root copy asked for: no sleep; taken already: asleep until something else */
    reset();
    st.rootCopy.seq = 3;
    ms = waitForWork();
    CHECK(!rescued && sleeps == 0, "a root copy asked for: slept (%d)", sleeps);
    reset();
    st.rootCopy.seq = st.rootCopy.taken = 3;
    l = (struct later) { xCursor, 80 };
    pthread_create(&t, NULL, laterThread, &l);
    ms = waitForWork();
    pthread_join(t, NULL);
    CHECK(!rescued && ms >= 60 && looks <= 3, "a root copy taken: did not sleep until the next work (%ld ms, %d looks)", ms, looks);

    /* copies left queued for a reservation: asleep, not polling, until it is let go */
    reset();
    st.gpuCopyQueue.writeIndex = 1;
    deferredCopies = true;
    deferredAt = 0;
    deferredReleases = st.cpuAccess.releases;
    l = (struct later) { xRelease, 120 };
    pthread_create(&t, NULL, laterThread, &l);
    ms = waitForWork();
    pthread_join(t, NULL);
    CHECK(!rescued && ms >= 100 && looks <= 3, "copies left for a reservation: %ld ms, %d looks (polling?), rescued %d", ms, looks, rescued);
    reset();
    st.gpuCopyQueue.writeIndex = 1;
    deferredCopies = true;
    deferredAt = 0;
    deferredReleases = st.cpuAccess.releases;
    hookSleepStart = xRelease;
    ms = waitForWork();
    CHECK(!rescued && ms < 250, "a reservation let go between the look and the sleep: slept through it (%ld ms)", ms);

    /* the delay a coalesced redraw asks for: waited for, cut short by a wake */
    reset();
    st.drawRequested = 1;
    rendererPreRedrawCoalesceFrames = 1;
    rendererPreRedrawCoalesceWaitUs = 200000;
    ms = waitForWork();
    CHECK(!rescued && ms >= 180 && ms < 400, "a coalesced redraw's 200 ms: waited %ld ms", ms);
    reset();
    st.drawRequested = 1;
    rendererPreRedrawCoalesceFrames = 1;
    rendererPreRedrawCoalesceWaitUs = 200000;
    l = (struct later) { xCursor, 30 };
    pthread_create(&t, NULL, laterThread, &l);
    ms = waitForWork();
    pthread_join(t, NULL);
    CHECK(!rescued && ms < 150, "a coalesced redraw's delay not cut short by a wake: %ld ms", ms);

    /* from another process: the renderer asleep, then between its look and its sleep */
    shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shm != MAP_FAILED, "no shared mapping");
    for (i = 0; i < 2 && shm != MAP_FAILED; i++) {
        pid_t pid;
        int status;
        reset();
        memset(shm, 0, sizeof(*shm));
        shm->st.surfaceAvailable = 1;
        state = &shm->st;
        rendererWake = &shm->wake;
        hookSleepStart = i == 0 ? announceSleep : awaitPublished;
        if ((pid = fork()) == 0) {
            while (!__atomic_load_n(&shm->asleep, __ATOMIC_ACQUIRE))
                usleep(500);
            if (i == 0)
                usleep(60000);      /* the renderer in its futex wait by now */
            shm->st.drawRequested = 1;
            lorieWakeRenderer(&shm->wake);
            __atomic_store_n(&shm->published, 1, __ATOMIC_RELEASE);
            _exit(0);
        }
        ms = waitForWork();
        waitpid(pid, &status, 0);
        CHECK(!rescued && ms < 250, "another process's draw %s: slept through it (%ld ms)",
              i == 0 ? "while the renderer sleeps" : "between the renderer's look and its sleep", ms);
    }

    /* TERMUX_X11_FRAME_FREEZE_DIAG: asleep, the stage says so; woken to work, the stage says a pass, the asks it saw kept */
    {
        reset();
        st.backend.freezeDiag = 1;
        st.progress.askSeq = 4;
        l = (struct later) { xDraw, 60 };
        pthread_create(&t, NULL, laterThread, &l);
        ms = waitForWork();
        pthread_join(t, NULL);
        CHECK(!rescued && st.progress.rStage == LORIE_R_PASS && st.progress.seenSeq == 4 && st.progress.seenUs && st.progress.rStageUs,
              "freeze diag: woken, stage %u (want a pass), seen %u", st.progress.rStage, st.progress.seenSeq);
        reset();
        st.backend.freezeDiag = 1;
        hookSleepStart = noteStage;
        l = (struct later) { xDraw, 40 };
        pthread_create(&t, NULL, laterThread, &l);
        ms = waitForWork();
        pthread_join(t, NULL);
        CHECK(stageAtSleep == LORIE_R_SLEEP, "freeze diag: asleep, stage %u (want asleep)", stageAtSleep);
        reset();
        xDraw();
        waitForWork();
        CHECK(st.progress.rStage == 0 && st.progress.rStageUs == 0 && st.progress.seenUs == 0, "freeze diag off: a stage kept");
    }

    /* the previous protocol: the X server's signal between the look and pthread_cond_wait, slept through */
    {
        reset();
        caseDone = rescued = 0;
        rendererThread = pthread_self();
        pthread_create(&t, NULL, oldRescuer, NULL);
        ms = nowMs();
        pthread_mutex_lock(&stateLock);
        while (rendererShouldWait(&waitingForBuffers)) {
            if (!st.drawRequested)
                oldSignal();        /* the X server, after the look, before the wait */
            pthread_cond_wait(&oldCond, &stateLock);
        }
        pthread_mutex_unlock(&stateLock);
        ms = nowMs() - ms;
        __atomic_store_n(&caseDone, 1, __ATOMIC_RELEASE);
        pthread_join(t, NULL);
        CHECK(rescued && ms >= 400, "the previous protocol did not sleep through the signal (%ld ms): the forced point is wrong", ms);
        printf("  the previous protocol (look, then pthread_cond_wait; the signal between them): slept %ld ms until rescued\n", ms);
    }

    printf("renderer wake (forced before, during and after the look and the sleep; threads; another process): %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
