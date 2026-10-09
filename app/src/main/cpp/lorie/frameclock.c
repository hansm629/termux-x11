/*
 * The frame clock: what turns Android's VSYNC into X server frames.
 *
 *   SurfaceFlinger VSYNC
 *     -> AChoreographer frame callback on the owner thread - the thread CmdEntryPoint.start() runs on, i.e.
 *        the Java main Looper of the X server process. The callback is one-shot and re-posts itself.
 *     -> lorieQueueRedraw(): QueueWorkProc(lorieRedraw) + eventfd wake of the X server (InitOutput.c),
 *        unless a lorieRedraw is still queued: then the tick is only counted (coalesced)
 *     -> X server main thread, WaitForSomething -> ProcessWorkQueue -> lorieRedraw: current_msc += the
 *        ticks counted since the last one, Present vblanks, waitForNextFrame = false, signal the renderer
 *     -> renderer thread (activity process) draws once and sets waitForNextFrame again.
 *
 * A long visible frame gap can come from any of those hops, and the renderer's own numbers (frame_delta
 * against root_wait/swap/render) cannot say which. This file measures each hop and prints them every 5
 * seconds (lorieFrameClockReport, called from lorieFramecounter) as XlorieFrameClock* lines:
 *   - the callback: count, gaps, the VSYNC timestamps it was handed and how late it ran after them. A
 *     late callback with regular VSYNC timestamps means its thread did not run; an on-time callback with
 *     a large VSYNC gap means no VSYNC was sent;
 *   - callback -> lorieRedraw: how long a tick waited for the X server main thread and how many ticks
 *     piled up meanwhile;
 *   - lorieRedraw -> renderer: from struct lorie_frame_clock_stats in the shared state;
 *   - how the two X server threads involved were scheduled (run/runnable-wait time, CPU, nice, affinity).
 *
 * The chain is tracked by generation. Every callback is posted with the generation current at the time
 * as its data; re-arming the chain (posting a fresh callback) starts a new generation, so a callback of
 * an older one that still fires afterwards is stale: it neither ticks nor re-posts, and the chain can
 * never run twice per VSYNC. A watchdog on the owner thread's Looper re-arms the chain when it is lost -
 * no callback of the current generation registered - and once per stall when a registered one has been
 * silent for far longer than any display can legitimately be. It never draws or ticks by itself.
 * When the activity comes back (a new surface, a new connection) the owner thread is asked to look at
 * the chain right away, and re-arms it if it has been quiet for a stall's length.
 *
 * At most one lorieRedraw is queued at a time. Before, every callback queued its own, so after the X
 * server main thread was busy for N frames, ProcessWorkQueue ran N lorieRedraws back to back: N Present
 * vblank rounds in microseconds, each reporting a different MSC with the same UST, and the renderer
 * released again within its own frame - the catch-up burst. Now ticks that arrive while one is queued
 * are counted, and that one lorieRedraw advances MSC by all of them: the MSC still counts every VSYNC the
 * callback saw, exactly as before, and Present clients waiting for any MSC up to it are completed once,
 * at the MSC actually reached - what a real CRTC does when an interrupt is serviced late.
 *
 * Fault injection for testing all of the above on a device, off unless its variable is set (in the X
 * server's environment). Each fault is injected once per FC_TEST_PERIOD_NS, the first one a little
 * after start, and logged as "XlorieFrameClock: test: ...":
 *   TERMUX_X11_CHOREO_TEST_DROP=n         n times, a callback does not re-post: the chain is lost
 *                                         (expect: re-arm (lost) ~100 ms later, then a normal cadence)
 *   TERMUX_X11_CHOREO_TEST_REARM=n        n times, a healthy chain is re-armed (expect: stale_cb=1, one
 *                                         tick per VSYNC throughout)
 *   TERMUX_X11_CHOREO_TEST_BLOCK_MS=ms    the choreographer thread sleeps ms after a callback (expect: a
 *                                         late callback with a regular VSYNC gap, cb_lat_max_us ~ ms)
 *   TERMUX_X11_FRAMECLOCK_TEST_XSTALL_MS=ms  the X server main thread sleeps ms (expect: callbacks go on,
 *                                         queue_coalesced ~ ms / frame, one redraw takes them, replay=0)
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <android/choreographer.h>
#include <android/log.h>
#include <android/looper.h>
#include "frameclock.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

#define NS_PER_US ((int64_t) 1000)
#define NS_PER_MS ((int64_t) 1000000)

/* A callback gap this long is reported as an anomaly even while nothing is on screen. */
#define FC_ANOMALY_GAP_NS (100 * NS_PER_MS)

/*
 * No callback for this long counts as a stall, and a chain found lost is re-armed then: 12 frames at
 * 120 Hz, 6 at 60 Hz - beyond any frame a busy system merely delays, still short of a pause a user reads
 * as the end of an animation.
 */
#define FC_STALL_NS (100 * NS_PER_MS)
/*
 * A callback that is registered but silent this long is re-armed once per stall. SurfaceFlinger answers
 * a pending VSYNC request within about a second even when the display stalls (EventThread fakes a VSYNC
 * after 1000 ms, and sends synthetic ones while the screen is off), and an LTPO panel at rest may run at
 * 1 Hz: twice that is no longer a slow display.
 */
#define FC_SILENT_NS (2000 * NS_PER_MS)
/* While nothing is on screen a silent chain is left alone; it is looked at again this often. */
#define FC_IDLE_CHECK_NS (5000 * NS_PER_MS)
/*
 * A queued lorieRedraw that has not run for this long is presumed dropped (a server reset clears the
 * work queue) and one more is queued: a stuck flag must never stop the clock. Should the X server only
 * have been that busy, the extra one runs right after the first and finds no ticks to take.
 */
#define FC_REQUEUE_NS (1000 * NS_PER_MS)

#define FC_TEST_PERIOD_NS (10000 * NS_PER_MS)
#define FC_TEST_MAX_SLEEP_NS (5000 * NS_PER_MS)

typedef void (*FcFrameCallback64)(int64_t frameTimeNanos, void *data);
typedef void (*FcPostFrameCallback64)(AChoreographer *choreographer, FcFrameCallback64 callback, void *data);
typedef void (*FcRefreshRateCallback)(int64_t vsyncPeriodNanos, void *data);
typedef void (*FcRegisterRefreshRateCallback)(AChoreographer *choreographer, FcRefreshRateCallback callback, void *data);

static struct {
    AChoreographer *choreographer;
    FcPostFrameCallback64 post64;   /* API 29+, NULL before: then the (long) variant is used */
    pid_t ownerTid, xTid;

    /* owner thread */
    uint32_t generation;            /* of the chain; callbacks carrying another one are stale */
    bool outstanding;               /* a callback of the current generation is registered */
    bool stallSeen;                 /* the watchdog found the chain quiet since the last valid callback */
    bool rearmedThisStall;          /* ... and re-armed it */
    int64_t startNs, lastCallbackNs, lastFrameTimeNs;
    int64_t periodNs LORIE_ATOMIC64; /* from the refresh rate callback, 0 if it never ran */
    int timerFd;                    /* the watchdog, on the owner thread's Looper; -1 without one */
    int kickFd;                     /* eventfd, same Looper: lorieFrameClockResumeCheck() from any thread */

    /* owner thread -> X server main thread */
    uint32_t pendingTicks;          /* callbacks since the last lorieRedraw took them */
    int64_t oldestTickNs LORIE_ATOMIC64; /* when the first of them arrived */
    bool redrawQueued;              /* a lorieRedraw is queued and has not started yet */
    int64_t queuedNs;               /* owner thread only: when it was queued */

    /* X server main thread */
    int64_t lastRedrawNs, xWakeNs;
} fc;

/* Per 5 second window; the X server main thread reads and resets them. */
#define FC_HIST_BUCKETS 6
static struct {
    uint32_t callbacks, posts, staleCallbacks, stalls, rearms, forcedRearms, resumeChecks;
    uint32_t cbGapMaxUs, vsyncGapMaxUs, vsyncGapMinUs, cbLatencyMaxUs, cbLate;
    uint32_t cbHist[FC_HIST_BUCKETS];
    uint32_t queueTry, queueActual, queueCoalesced, requeues;
    uint32_t redraws, replays, ticksMax, multiTick, redrawGapMaxUs;
    uint32_t redrawHist[FC_HIST_BUCKETS];
    uint32_t cbToRedrawMaxUs, cbToRedrawSamples;
    uint64_t cbToRedrawSumUs LORIE_ATOMIC64;
    uint32_t waitClears;
    uint32_t xBusyMaxUs, lockWaitMaxUs;
    uint64_t lockWaitSumUs LORIE_ATOMIC64;
} st = { .vsyncGapMinUs = UINT32_MAX };

/* Fault injection (see the top of this file). Set before the X server thread starts, never after. */
static struct {
    long drops, rearms;
    int64_t ownerBlockNs, xStallNs;
    int64_t nextDropNs, nextRearmNs, nextOwnerBlockNs; /* owner thread */
    int64_t nextXStallNs;                              /* X server main thread */
} fcTest;

static void fcTestSleep(int64_t ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    while (nanosleep(&ts, &ts) == -1)
        ;
}

#define FC_ADD(field, n) __atomic_fetch_add(&st.field, (n), __ATOMIC_RELAXED)
#define FC_TAKE(field) __atomic_exchange_n(&st.field, 0, __ATOMIC_RELAXED)

static void fcMax(uint32_t *field, uint32_t v) {
    // Only ever raised by one thread; a reset racing it loses at most this one sample.
    if (v > __atomic_load_n(field, __ATOMIC_RELAXED))
        __atomic_store_n(field, v, __ATOMIC_RELAXED);
}

static uint32_t fcUs(int64_t ns) {
    int64_t us = ns / NS_PER_US;
    return us < 0 ? 0 : us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
}

/* <4 ms (faster than 240 Hz: a burst), <12 (120 Hz), <25 (60 Hz), <50, <100, >=100 ms */
static int fcHistBucket(uint32_t us) {
    return us < 4000 ? 0 : us < 12000 ? 1 : us < 25000 ? 2 : us < 50000 ? 3 : us < 100000 ? 4 : 5;
}

int64_t lorieFrameClockNowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void fcFrameCallback64(int64_t frameTimeNanos, void *data);
static void fcFrameCallbackLong(long frameTimeNanos, void *data);

static void fcPost(void) {
    void *data = (void *) (uintptr_t) fc.generation;
    if (fc.post64)
        fc.post64(fc.choreographer, fcFrameCallback64, data);
    else
        AChoreographer_postFrameCallback(fc.choreographer, fcFrameCallbackLong, data);
    fc.outstanding = true;
    FC_ADD(posts, 1);
}

static void fcArmWatchdog(int64_t delayNs) {
    struct itimerspec its = { .it_value = { .tv_sec = delayNs / 1000000000LL, .tv_nsec = delayNs % 1000000000LL } };
    if (fc.timerFd >= 0)
        timerfd_settime(fc.timerFd, 0, &its, NULL);
}

// Starts a new generation and posts its first callback. Whatever callback of the old one is still
// registered turns stale.
static void fcRearm(const char *why, int64_t sinceNs) {
    bool forced = fc.outstanding;

    fc.generation++;
    fc.rearmedThisStall = true;
    FC_ADD(rearms, 1);
    if (forced)
        FC_ADD(forcedRearms, 1);
    log(WARN, "XlorieFrameClock: re-arming the frame callback (%s): none for %" PRId64 " ms, %s, generation %u",
        why, sinceNs / NS_PER_MS, forced ? "replacing the registered one" : "none was registered", fc.generation);
    fcPost();
}

/*
 * Watchdog, on the owner thread. Pushed back by every valid callback, so it only fires once the chain
 * has been quiet for FC_STALL_NS.
 */
static void fcCheck(void) {
    int64_t now = lorieFrameClockNowNs();
    int64_t since = now - (fc.lastCallbackNs ? fc.lastCallbackNs : fc.startNs);

    if (since < FC_STALL_NS) {
        fcArmWatchdog(FC_STALL_NS - since);
        return;
    }

    if (!fc.stallSeen) {
        fc.stallSeen = true;
        FC_ADD(stalls, 1);
    }
    if (!fc.outstanding) {
        // Nothing registered will ever call back: the chain is lost.
        fcRearm("lost", since);
    } else if (!fc.rearmedThisStall) {
        if (since < FC_SILENT_NS)
            fcArmWatchdog(FC_SILENT_NS - since);
        else if (lorieSurfaceShown())
            fcRearm("silent", since);
        else
            fcArmWatchdog(FC_IDLE_CHECK_NS);
    }
    // Once re-armed for this stall, only the next valid callback arms the watchdog again.
}

/*
 * The activity got a surface or connected again. If the chain has gone quiet meanwhile, re-arm it now
 * instead of waiting for the watchdog's silence limit: this is exactly when a stale registration would
 * otherwise keep the screen frozen.
 */
static void fcResumeCheck(void) {
    int64_t since = lorieFrameClockNowNs() - (fc.lastCallbackNs ? fc.lastCallbackNs : fc.startNs);

    FC_ADD(resumeChecks, 1);
    if (since >= FC_STALL_NS && !fc.rearmedThisStall)
        fcRearm("resume", since);
}

static int fcLooperCallback(int fd, __unused int events, __unused void *data) {
    uint64_t value;
    while (read(fd, &value, sizeof(value)) > 0)
        ;
    if (fd == fc.kickFd)
        fcResumeCheck();
    else
        fcCheck();
    return 1;
}

static void fcNoteCallback(int64_t now, int64_t frameTimeNs, bool frameTimeValid) {
    FC_ADD(callbacks, 1);

    if (fc.lastCallbackNs) {
        uint32_t gapUs = fcUs(now - fc.lastCallbackNs);
        fcMax(&st.cbGapMaxUs, gapUs);
        FC_ADD(cbHist[fcHistBucket(gapUs)], 1);
    } else
        // The callback must run on the thread that owns the AChoreographer (and the watchdog's Looper).
        log(INFO, "XlorieFrameClock: first callback on tid %d, owner tid %d%s", gettid(), fc.ownerTid,
            gettid() == fc.ownerTid ? "" : " - NOT the owner thread");
    fc.lastCallbackNs = now;

    if (!frameTimeValid)
        return;

    // The VSYNC timestamp the callback was handed, against when it actually ran.
    {
        uint32_t latencyUs = fcUs(now - frameTimeNs);
        fcMax(&st.cbLatencyMaxUs, latencyUs);
        if (latencyUs >= 4000)
            FC_ADD(cbLate, 1);
    }

    if (fc.lastFrameTimeNs && frameTimeNs > fc.lastFrameTimeNs) {
        uint32_t vsyncGapUs = fcUs(frameTimeNs - fc.lastFrameTimeNs);
        fcMax(&st.vsyncGapMaxUs, vsyncGapUs);
        if (vsyncGapUs < __atomic_load_n(&st.vsyncGapMinUs, __ATOMIC_RELAXED))
            __atomic_store_n(&st.vsyncGapMinUs, vsyncGapUs, __ATOMIC_RELAXED);
    }
    fc.lastFrameTimeNs = frameTimeNs;
}

static void fcTick(int64_t now) {
    if (!lorieScreenReady())
        return;

    // Count the tick before looking at the flag: a lorieRedraw that cleared the flag takes the count
    // only after that, so it either takes this tick or this callback finds the flag clear and queues a
    // lorieRedraw that will.
    if (__atomic_fetch_add(&fc.pendingTicks, 1, __ATOMIC_ACQ_REL) == 0)
        __atomic_store_n(&fc.oldestTickNs, now, __ATOMIC_RELEASE);

    FC_ADD(queueTry, 1);
    if (!__atomic_exchange_n(&fc.redrawQueued, true, __ATOMIC_SEQ_CST)) {
        fc.queuedNs = now;
        FC_ADD(queueActual, 1);
        lorieQueueRedraw();
    } else if (now - fc.queuedNs >= FC_REQUEUE_NS) {
        log(WARN, "XlorieFrameClock: the queued redraw has not run for %" PRId64 " ms, queueing another",
            (now - fc.queuedNs) / NS_PER_MS);
        fc.queuedNs = now;
        FC_ADD(requeues, 1);
        FC_ADD(queueActual, 1);
        lorieQueueRedraw();
    } else
        FC_ADD(queueCoalesced, 1);
}

static void fcOnFrame(int64_t frameTimeNs, bool frameTimeValid, void *data) {
    int64_t now = lorieFrameClockNowNs();

    if ((uint32_t) (uintptr_t) data != fc.generation) {
        // Posted before a re-arm replaced it. Ticking or re-posting would run the clock twice.
        FC_ADD(staleCallbacks, 1);
        return;
    }

    fc.outstanding = false;
    if (fc.rearmedThisStall)
        log(INFO, "XlorieFrameClock: callbacks are back, %" PRId64 " ms after the last one, generation %u",
            (now - fc.lastCallbackNs) / NS_PER_MS, fc.generation);
    fc.stallSeen = fc.rearmedThisStall = false;
    fcNoteCallback(now, frameTimeNs, frameTimeValid);

    if (fcTest.drops > 0 && now >= fcTest.nextDropNs) {
        fcTest.drops--;
        fcTest.nextDropNs = now + FC_TEST_PERIOD_NS;
        log(WARN, "XlorieFrameClock: test: not re-posting the frame callback (generation %u)", fc.generation);
    } else
        fcPost();

    if (fcTest.rearms > 0 && now >= fcTest.nextRearmNs && fc.outstanding) {
        fcTest.rearms--;
        fcTest.nextRearmNs = now + FC_TEST_PERIOD_NS;
        log(WARN, "XlorieFrameClock: test: re-arming a healthy chain");
        fcRearm("test", 0);
    }

    fcArmWatchdog(FC_STALL_NS);
    fcTick(now);

    if (fcTest.ownerBlockNs && now >= fcTest.nextOwnerBlockNs) {
        fcTest.nextOwnerBlockNs = now + FC_TEST_PERIOD_NS;
        log(WARN, "XlorieFrameClock: test: blocking the choreographer thread for %" PRId64 " ms",
            fcTest.ownerBlockNs / NS_PER_MS);
        fcTestSleep(fcTest.ownerBlockNs);
    }
}

static void fcFrameCallback64(int64_t frameTimeNanos, void *data) {
    fcOnFrame(frameTimeNanos, true, data);
}

static void fcFrameCallbackLong(long frameTimeNanos, void *data) {
    // A 32 bit long truncates the nanosecond timestamp; it is only usable for timing on LP64.
    fcOnFrame((int64_t) frameTimeNanos, sizeof(long) >= sizeof(int64_t), data);
}

static void fcRefreshRateCallback(int64_t vsyncPeriodNanos, __unused void *data) {
    if (vsyncPeriodNanos != fc.periodNs)
        log(INFO, "XlorieFrameClock: choreographer vsync period %" PRId64 " ns (%.1f Hz)", vsyncPeriodNanos,
            vsyncPeriodNanos > 0 ? 1e9 / (double) vsyncPeriodNanos : 0.0);
    __atomic_store_n(&fc.periodNs, vsyncPeriodNanos, __ATOMIC_RELAXED);
}

static int64_t fcTestEnv(const char *name, int64_t max) {
    const char *value = getenv(name);
    long long n = value ? strtoll(value, NULL, 10) : 0;
    return n <= 0 ? 0 : n > max ? max : n;
}

void lorieFrameClockStart(void) {
    FcRegisterRefreshRateCallback registerRefreshRate = NULL;
    ALooper *looper;
    void *android;

    fc.ownerTid = gettid();
    fc.startNs = lorieFrameClockNowNs();
    fc.timerFd = fc.kickFd = -1;
    fc.choreographer = AChoreographer_getInstance();
    if (!fc.choreographer) {
        log(ERROR, "XlorieFrameClock: no AChoreographer on tid %d, the frame clock is not running", fc.ownerTid);
        return;
    }

    // Both only exist on newer Android versions, so resolve them at runtime instead of linking them.
    if ((android = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD))) {
        fc.post64 = (FcPostFrameCallback64) dlsym(android, "AChoreographer_postFrameCallback64");
        registerRefreshRate = (FcRegisterRefreshRateCallback) dlsym(android, "AChoreographer_registerRefreshRateCallback");
    }
    if (registerRefreshRate)
        registerRefreshRate(fc.choreographer, fcRefreshRateCallback, NULL);

    // The watchdog and the resume check run on the same Looper as the callbacks, so they are serialized
    // with them.
    if ((looper = ALooper_forThread())) {
        if ((fc.timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK)) >= 0 &&
            ALooper_addFd(looper, fc.timerFd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, fcLooperCallback, NULL) != 1) {
            close(fc.timerFd);
            fc.timerFd = -1;
        }
        if ((fc.kickFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) >= 0 &&
            ALooper_addFd(looper, fc.kickFd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, fcLooperCallback, NULL) != 1) {
            close(fc.kickFd);
            fc.kickFd = -1;
        }
    }

    log(INFO, "XlorieFrameClock: owner tid %d, postFrameCallback64 %s, refresh rate callback %s, watchdog %s, "
              "resume check %s", fc.ownerTid, fc.post64 ? "yes" : "no", registerRefreshRate ? "yes" : "no",
        fc.timerFd >= 0 ? "yes" : "no", fc.kickFd >= 0 ? "yes" : "no");

    fcTest.drops = (long) fcTestEnv("TERMUX_X11_CHOREO_TEST_DROP", 1000);
    fcTest.rearms = (long) fcTestEnv("TERMUX_X11_CHOREO_TEST_REARM", 1000);
    fcTest.ownerBlockNs = fcTestEnv("TERMUX_X11_CHOREO_TEST_BLOCK_MS", FC_TEST_MAX_SLEEP_NS / NS_PER_MS) * NS_PER_MS;
    fcTest.xStallNs = fcTestEnv("TERMUX_X11_FRAMECLOCK_TEST_XSTALL_MS", FC_TEST_MAX_SLEEP_NS / NS_PER_MS) * NS_PER_MS;
    // Staggered, so each fault's effect can be told apart from the others' in the 5 second lines.
    fcTest.nextDropNs = fc.startNs + 10000 * NS_PER_MS;
    fcTest.nextRearmNs = fc.startNs + 15000 * NS_PER_MS;
    fcTest.nextXStallNs = fc.startNs + 20000 * NS_PER_MS;
    fcTest.nextOwnerBlockNs = fc.startNs + 25000 * NS_PER_MS;
    if (fcTest.drops || fcTest.rearms || fcTest.ownerBlockNs || fcTest.xStallNs)
        log(WARN, "XlorieFrameClock: test: fault injection on: drop=%ld rearm=%ld block_ms=%" PRId64 " xstall_ms=%" PRId64,
            fcTest.drops, fcTest.rearms, fcTest.ownerBlockNs / NS_PER_MS, fcTest.xStallNs / NS_PER_MS);

    fc.generation = 1;
    fcPost();
    fcArmWatchdog(FC_STALL_NS);
}

void lorieFrameClockResumeCheck(void) {
    if (fc.kickFd >= 0)
        eventfd_write(fc.kickFd, 1);
}

void lorieFrameClockSetXThread(void) {
    fc.xTid = gettid();
}

void lorieFrameClockResetQueue(void) {
    __atomic_store_n(&fc.redrawQueued, false, __ATOMIC_SEQ_CST);
    __atomic_store_n(&fc.pendingTicks, 0, __ATOMIC_SEQ_CST);
}

uint32_t lorieFrameClockRedrawBegin(void) {
    int64_t now = lorieFrameClockNowNs();
    uint32_t ticks;
    int64_t oldest;

    // Clear first, take second (see fcTick): from here on a new tick queues a new lorieRedraw.
    __atomic_store_n(&fc.redrawQueued, false, __ATOMIC_SEQ_CST);
    ticks = __atomic_exchange_n(&fc.pendingTicks, 0, __ATOMIC_ACQ_REL);
    oldest = __atomic_exchange_n(&fc.oldestTickNs, 0, __ATOMIC_ACQ_REL);

    FC_ADD(redraws, 1);
    if (fc.lastRedrawNs) {
        uint32_t gapUs = fcUs(now - fc.lastRedrawNs);
        fcMax(&st.redrawGapMaxUs, gapUs);
        FC_ADD(redrawHist[fcHistBucket(gapUs)], 1);
    }
    fc.lastRedrawNs = now;

    if (!ticks) {
        // Every tick was taken by an earlier lorieRedraw (a re-queue, or a tick that raced the flag).
        FC_ADD(replays, 1);
        return 0;
    }

    fcMax(&st.ticksMax, ticks);
    if (ticks > 1)
        FC_ADD(multiTick, 1);
    if (oldest) {
        uint32_t delayUs = fcUs(now - oldest);
        fcMax(&st.cbToRedrawMaxUs, delayUs);
        FC_ADD(cbToRedrawSumUs, delayUs);
        FC_ADD(cbToRedrawSamples, 1);
    }
    return ticks;
}

void lorieFrameClockNoteClear(bool wasWaiting) {
    if (wasWaiting)
        FC_ADD(waitClears, 1);
}

void lorieFrameClockXWakeup(void) {
    fc.xWakeNs = lorieFrameClockNowNs();

    if (fcTest.xStallNs && fc.xWakeNs >= fcTest.nextXStallNs) {
        fcTest.nextXStallNs = fc.xWakeNs + FC_TEST_PERIOD_NS;
        log(WARN, "XlorieFrameClock: test: blocking the X server main thread for %" PRId64 " ms",
            fcTest.xStallNs / NS_PER_MS);
        fcTestSleep(fcTest.xStallNs);
    }
}

void lorieFrameClockXBlock(void) {
    // From the X server main thread waking up to it going back to sleep: everything it did in between,
    // lorieRedraw included, delayed whatever frame tick arrived meanwhile.
    if (fc.xWakeNs)
        fcMax(&st.xBusyMaxUs, fcUs(lorieFrameClockNowNs() - fc.xWakeNs));
    fc.xWakeNs = 0;
}

void lorieFrameClockNoteLockWait(int64_t ns) {
    uint32_t us = fcUs(ns);
    fcMax(&st.lockWaitMaxUs, us);
    FC_ADD(lockWaitSumUs, us);
}

struct fcThreadSample {
    bool ok;
    uint64_t runNs, waitNs;
    unsigned long majflt;
    long prio, nice;
    int cpu;
    char allowed[64];
};

static ssize_t fcReadFile(const char *path, char *buf, size_t size) {
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static void fcSampleThread(pid_t tid, struct fcThreadSample *s) {
    char path[64], buf[1024], *p;
    unsigned long long run = 0, wait = 0;
    int field;

    memset(s, 0, sizeof(*s));
    s->cpu = -1;
    strcpy(s->allowed, "?");
    if (tid <= 0)
        return;

    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    if (fcReadFile(path, buf, sizeof(buf)) > 0 && sscanf(buf, "%llu %llu", &run, &wait) == 2) {
        s->runNs = run;
        s->waitNs = wait;
        s->ok = true;
    }

    // Fields after the parenthesised comm, starting with field 3 (state).
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    if (fcReadFile(path, buf, sizeof(buf)) > 0 && (p = strrchr(buf, ')'))) {
        char *save = NULL, *tok = strtok_r(p + 1, " ", &save);
        for (field = 3; tok; tok = strtok_r(NULL, " ", &save), field++) {
            if (field == 12)
                s->majflt = strtoul(tok, NULL, 10);
            else if (field == 18)
                s->prio = strtol(tok, NULL, 10);
            else if (field == 19)
                s->nice = strtol(tok, NULL, 10);
            else if (field == 39) {
                s->cpu = (int) strtol(tok, NULL, 10);
                break;
            }
        }
    }

    snprintf(path, sizeof(path), "/proc/self/task/%d/status", tid);
    if (fcReadFile(path, buf, sizeof(buf)) > 0 && (p = strstr(buf, "Cpus_allowed_list:"))) {
        p += strlen("Cpus_allowed_list:");
        p += strspn(p, " \t");
        snprintf(s->allowed, sizeof(s->allowed), "%.*s", (int) strcspn(p, "\n"), p);
    }
}

static void fcNoteCgroup(const char *who, pid_t tid, char *last, size_t lastSize) {
    char path[64], buf[512];
    if (tid <= 0)
        return;
    snprintf(path, sizeof(path), "/proc/self/task/%d/cgroup", tid);
    if (fcReadFile(path, buf, sizeof(buf)) <= 0)
        return;
    for (char *c = buf; *c; c++)
        if (*c == '\n')
            *c = ' ';
    if (strcmp(buf, last) != 0) {
        log(INFO, "XlorieFrameClockSched: %s tid %d cgroup %s", who, tid, buf);
        snprintf(last, lastSize, "%s", buf);
    }
}

static void fcFormatThread(char *out, size_t size, const char *who, pid_t tid, struct fcThreadSample *now,
                           struct fcThreadSample *prev) {
    if (!now->ok) {
        snprintf(out, size, "%s tid=%d n/a", who, tid);
        return;
    }
    snprintf(out, size, "%s tid=%d cpu=%d allowed=%s prio=%ld nice=%ld run_ms=%.1f wait_ms=%.1f majflt=%lu",
             who, tid, now->cpu, now->allowed, now->prio, now->nice,
             prev->ok ? (double) (now->runNs - prev->runNs) / NS_PER_MS : 0.0,
             prev->ok ? (double) (now->waitNs - prev->waitNs) / NS_PER_MS : 0.0,
             prev->ok ? now->majflt - prev->majflt : 0);
}

void lorieFrameClockReport(volatile struct lorie_frame_clock_stats *rs, int renderedFrames,
                           bool surfaceAvailable, bool connected, uint32_t appHz) {
    static struct fcThreadSample prevOwner, prevX;
    static char ownerCgroup[512], xCgroup[512];
    static bool debug = false, debugChecked = false;
    struct fcThreadSample owner, x;
    char ownerStr[192], xStr[192];
    uint32_t cbHist[FC_HIST_BUCKETS], redrawHist[FC_HIST_BUCKETS];
    uint32_t callbacks = FC_TAKE(callbacks), posts = FC_TAKE(posts), staleCallbacks = FC_TAKE(staleCallbacks);
    uint32_t stalls = FC_TAKE(stalls), rearms = FC_TAKE(rearms), forcedRearms = FC_TAKE(forcedRearms);
    uint32_t resumeChecks = FC_TAKE(resumeChecks);
    uint32_t cbGapMaxUs = FC_TAKE(cbGapMaxUs), vsyncGapMaxUs = FC_TAKE(vsyncGapMaxUs);
    uint32_t vsyncGapMinUs = __atomic_exchange_n(&st.vsyncGapMinUs, UINT32_MAX, __ATOMIC_RELAXED);
    uint32_t cbLatencyMaxUs = FC_TAKE(cbLatencyMaxUs), cbLate = FC_TAKE(cbLate);
    uint32_t queueTry = FC_TAKE(queueTry), queueActual = FC_TAKE(queueActual);
    uint32_t queueCoalesced = FC_TAKE(queueCoalesced), requeues = FC_TAKE(requeues);
    uint32_t redraws = FC_TAKE(redraws), replays = FC_TAKE(replays), ticksMax = FC_TAKE(ticksMax);
    uint32_t multiTick = FC_TAKE(multiTick), redrawGapMaxUs = FC_TAKE(redrawGapMaxUs);
    uint32_t cbToRedrawMaxUs = FC_TAKE(cbToRedrawMaxUs), cbToRedrawSamples = FC_TAKE(cbToRedrawSamples);
    uint64_t cbToRedrawSumUs = FC_TAKE(cbToRedrawSumUs);
    uint32_t waitClears = FC_TAKE(waitClears), xBusyMaxUs = FC_TAKE(xBusyMaxUs);
    uint32_t lockWaitMaxUs = FC_TAKE(lockWaitMaxUs);
    uint64_t lockWaitSumUs = FC_TAKE(lockWaitSumUs);
    bool anomaly;
    int i;

    for (i = 0; i < FC_HIST_BUCKETS; i++) {
        cbHist[i] = FC_TAKE(cbHist[i]);
        redrawHist[i] = FC_TAKE(redrawHist[i]);
    }

    if (!debugChecked) {
        debug = getenv("TERMUX_X11_DEBUG") != NULL;
        debugChecked = true;
    }

    fcNoteCgroup("choreographer", fc.ownerTid, ownerCgroup, sizeof(ownerCgroup));
    fcNoteCgroup("x", fc.xTid, xCgroup, sizeof(xCgroup));
    fcSampleThread(fc.ownerTid, &owner);
    fcSampleThread(fc.xTid, &x);

    // Steady lines only while something is on screen; anything that looks like a stall regardless.
    anomaly = cbGapMaxUs >= fcUs(FC_ANOMALY_GAP_NS) || replays || xBusyMaxUs >= fcUs(FC_ANOMALY_GAP_NS) ||
              staleCallbacks || stalls || rearms || requeues;
    if (!(surfaceAvailable && connected) && !anomaly && !debug)
        goto out;

    log(INFO, "XlorieFrameClock: app_hz=%u period_us=%u gen=%u cb=%u post=%u stale_cb=%u stall=%u rearm=%u "
              "forced_rearm=%u resume_check=%u cb_gap_max_us=%u "
              "cb_hist=%u/%u/%u/%u/%u/%u vsync_gap_min_us=%u vsync_gap_max_us=%u cb_lat_max_us=%u cb_late=%u "
              "queue_try=%u queue_actual=%u queue_coalesced=%u requeue=%u redraw=%u replay=%u ticks_max=%u multi_tick=%u "
              "cb_to_redraw_max_us=%u cb_to_redraw_avg_us=%u redraw_gap_max_us=%u redraw_hist=%u/%u/%u/%u/%u/%u",
        appHz, fcUs(__atomic_load_n(&fc.periodNs, __ATOMIC_RELAXED)), __atomic_load_n(&fc.generation, __ATOMIC_RELAXED),
        callbacks, posts, staleCallbacks, stalls, rearms, forcedRearms, resumeChecks, cbGapMaxUs,
        cbHist[0], cbHist[1], cbHist[2], cbHist[3], cbHist[4], cbHist[5],
        vsyncGapMinUs == UINT32_MAX ? 0 : vsyncGapMinUs, vsyncGapMaxUs, cbLatencyMaxUs, cbLate,
        queueTry, queueActual, queueCoalesced, requeues, redraws, replays, ticksMax, multiTick,
        cbToRedrawMaxUs, cbToRedrawSamples ? (uint32_t) (cbToRedrawSumUs / cbToRedrawSamples) : 0, redrawGapMaxUs,
        redrawHist[0], redrawHist[1], redrawHist[2], redrawHist[3], redrawHist[4], redrawHist[5]);

    log(INFO, "XlorieFrameClockR: render=%d surface=%d connected=%d sgen=%u wait_set=%u wait_over_tick=%u "
              "wait_clear=%u handover=%u handover_late=%u handover_late_max_ticks=%u handover_max_us=%u "
              "lock_hold_max_us=%u buf_wait_max_us=%u",
        renderedFrames, surfaceAvailable, connected, rs->surfaceGeneration, rs->waitSet, rs->waitSetOverTick,
        waitClears, rs->handoverFrames, rs->handoverLate, rs->handoverLateMaxTicks, rs->handoverMaxUs,
        rs->lockHeldMaxUs, rs->bufferWaitMaxUs);

    fcFormatThread(ownerStr, sizeof(ownerStr), "choreographer", fc.ownerTid, &owner, &prevOwner);
    fcFormatThread(xStr, sizeof(xStr), "x", fc.xTid, &x, &prevX);
    log(INFO, "XlorieFrameClockSched: %s | %s | x_busy_max_us=%u lock_wait_max_us=%u lock_wait_ms=%.1f",
        ownerStr, xStr, xBusyMaxUs, lockWaitMaxUs, (double) lockWaitSumUs / 1000.0);

    out:
    rs->waitSet = rs->waitSetOverTick = 0;
    rs->handoverFrames = rs->handoverLate = rs->handoverLateMaxTicks = rs->handoverMaxUs = 0;
    rs->lockHeldMaxUs = rs->bufferWaitMaxUs = 0;
    prevOwner = owner;
    prevX = x;
}
