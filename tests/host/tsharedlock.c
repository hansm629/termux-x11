/* TSHAREDLOCK: the shared lock's wait (lorie.h lorie_mutex_lock, the real one: gen.py "sharedlock") - a recursive,
 * process-shared mutex, as the X server makes it.
 *
 *   Held by another thread for 100 ms: taken once it is let go (not before, not much after), the thread asleep
 *   meanwhile - a few timed waits of 33 ms, not a loop of waits that end at once, and little CPU. Free: taken at once.
 *   Held by this thread: taken again (recursive). Held by a process no longer connected: reinitialised and taken, but
 *   only after one 33 ms wait - not at once. While connected, never reinitialised: still waited for after 150 ms.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif

static bool connected = true;
static bool lorieConnectionAlive(void) { return connected; }
static long timedCalls;
static int countedTimedlock(pthread_mutex_t *m, const struct timespec *ts) {
    timedCalls++;
    return pthread_mutex_timedlock(m, ts);
}
#define pthread_mutex_timedlock countedTimedlock
#include "sharedlock_src.inc"
#undef pthread_mutex_timedlock

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static pthread_mutex_t lock;
static pid_t lockingPid;
static long holdMs;
static volatile int holding;

static long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static long cpuMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static void init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lock, &attr);
    lockingPid = 0;
}
/* another thread - as the other process would - holds the lock for holdMs (forever if negative) */
static void *holder(void *arg) {
    (void) arg;
    pthread_mutex_lock(&lock);
    lockingPid = getpid() + 1;      /* not this process's */
    __atomic_store_n(&holding, 1, __ATOMIC_RELEASE);
    if (holdMs >= 0) {
        usleep(holdMs * 1000);
        lockingPid = 0;
        pthread_mutex_unlock(&lock);
    } else
        pause();
    return NULL;
}
static void startHolder(pthread_t *t, long ms) {
    holding = 0;
    holdMs = ms;
    pthread_create(t, NULL, holder, NULL);
    while (!__atomic_load_n(&holding, __ATOMIC_ACQUIRE))
        usleep(500);
}

static volatile int waiterGot;
static void *waiter(void *arg) {
    (void) arg;
    lorie_mutex_lock(&lock, &lockingPid);
    waiterGot = 1;
    return NULL;
}

int main(void) {
    pthread_t t;
    long t0, c0, ms, cpu;

    /* held by another thread for 100 ms */
    init();
    connected = true;
    startHolder(&t, 100);
    timedCalls = 0;
    t0 = nowMs();
    c0 = cpuMs();
    lorie_mutex_lock(&lock, &lockingPid);
    ms = nowMs() - t0;
    cpu = cpuMs() - c0;
    CHECK(ms >= 85 && ms < 160, "held 100 ms: taken after %ld ms", ms);
    CHECK(timedCalls <= 6 && cpu <= 15, "held 100 ms: %ld timed waits, %ld ms of CPU - a busy loop, not a sleep until the unlock",
          timedCalls, cpu);
    CHECK(lockingPid == getpid(), "held 100 ms: taken, but not marked this process's");
    lorie_mutex_unlock(&lock, &lockingPid);
    pthread_join(t, NULL);

    /* free; held by this thread */
    timedCalls = 0;
    t0 = nowMs();
    lorie_mutex_lock(&lock, &lockingPid);
    lorie_mutex_lock(&lock, &lockingPid);
    CHECK(nowMs() - t0 < 20 && timedCalls == 2, "free, then held by this thread: %ld ms, %ld timed waits", nowMs() - t0, timedCalls);
    pthread_mutex_unlock(&lock);
    lorie_mutex_unlock(&lock, &lockingPid);

    /* held for good by a process no longer connected: reinitialised and taken after one 33 ms wait */
    init();
    startHolder(&t, -1);
    connected = false;
    timedCalls = 0;
    t0 = nowMs();
    lorie_mutex_lock(&lock, &lockingPid);
    ms = nowMs() - t0;
    CHECK(ms >= 28 && ms < 120 && timedCalls == 2, "held by a process gone: taken after %ld ms, %ld timed waits (want one 33 ms wait, then the lock)",
          ms, timedCalls);
    lorie_mutex_unlock(&lock, &lockingPid);
    connected = true;
    pthread_cancel(t);
    pthread_join(t, NULL);

    /* held for good while connected: never taken from it */
    {
        pthread_t w;
        init();
        startHolder(&t, -1);
        waiterGot = 0;
        pthread_create(&w, NULL, waiter, NULL);
        usleep(150000);
        CHECK(!waiterGot, "held while connected: taken from its holder");
        connected = false;      /* let it end: the holder's process gone after all */
        pthread_join(w, NULL);
        connected = true;
        pthread_cancel(t);
        pthread_join(t, NULL);
    }

    printf("TSHAREDLOCK the shared lock's wait (lorie_mutex_lock): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
