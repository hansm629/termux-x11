/*
 * The frame clock: what turns Android's VSYNC into X server frames.
 *
 *   SurfaceFlinger VSYNC
 *     -> AChoreographer frame callback on the owner thread - the thread CmdEntryPoint.start() runs on, i.e.
 *        the Java main Looper of the X server process. The callback is one-shot and re-posts itself.
 *     -> lorieQueueRedraw(): QueueWorkProc(lorieRedraw) + eventfd wake of the X server (InitOutput.c)
 *     -> X server main thread, WaitForSomething -> ProcessWorkQueue -> lorieRedraw: current_msc++,
 *        Present vblanks, waitForNextFrame = false, signal the renderer
 *     -> renderer thread (activity process) draws once and sets waitForNextFrame again.
 *
 * A long visible frame gap can come from any of those hops, and the renderer's own numbers (frame_delta
 * against root_wait/swap/render) cannot say which. This file measures each hop and prints them every 5
 * seconds (lorieFrameClockReport, called from lorieFramecounter) as XlorieFrameClock* lines:
 *   - the callback: count, gaps, the VSYNC timestamps it was handed and how late it ran after them. A
 *     late callback with regular VSYNC timestamps means its thread did not run; an on-time callback with
 *     a large VSYNC gap means no VSYNC was sent;
 *   - callback -> lorieRedraw: how long a tick waited for the X server main thread, how many ticks piled
 *     up meanwhile and how many queued lorieRedraws then ran with nothing new (the backlog replay);
 *   - lorieRedraw -> renderer: from struct lorie_frame_clock_stats in the shared state;
 *   - how the two X server threads involved were scheduled (run/runnable-wait time, CPU, nice, affinity).
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
#include <android/choreographer.h>
#include <android/log.h>
#include "frameclock.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

#define NS_PER_US 1000LL
#define NS_PER_MS 1000000LL

/* A callback gap this long is reported as an anomaly even while nothing is on screen. */
#define FC_ANOMALY_GAP_NS (100 * NS_PER_MS)

typedef void (*FcFrameCallback64)(int64_t frameTimeNanos, void *data);
typedef void (*FcPostFrameCallback64)(AChoreographer *choreographer, FcFrameCallback64 callback, void *data);
typedef void (*FcRefreshRateCallback)(int64_t vsyncPeriodNanos, void *data);
typedef void (*FcRegisterRefreshRateCallback)(AChoreographer *choreographer, FcRefreshRateCallback callback, void *data);

static struct {
    AChoreographer *choreographer;
    FcPostFrameCallback64 post64;   /* API 29+, NULL before: then the (long) variant is used */
    pid_t ownerTid, xTid;

    /* owner thread */
    int64_t lastCallbackNs, lastFrameTimeNs;
    int64_t periodNs;               /* from the refresh rate callback, 0 if it never ran */

    /* owner thread -> X server main thread */
    uint32_t pendingTicks;          /* callbacks since the last lorieRedraw took them */
    int64_t oldestTickNs;           /* when the first of them arrived */

    /* X server main thread */
    int64_t lastRedrawNs, xWakeNs;
} fc;

/* Per 5 second window; the X server main thread reads and resets them. */
#define FC_HIST_BUCKETS 6
static struct {
    uint32_t callbacks, posts;
    uint32_t cbGapMaxUs, vsyncGapMaxUs, vsyncGapMinUs, cbLatencyMaxUs, cbLate;
    uint32_t cbHist[FC_HIST_BUCKETS];
    uint32_t queueTry, queueActual;
    uint32_t redraws, replays, ticksMax, multiTick, redrawGapMaxUs;
    uint32_t redrawHist[FC_HIST_BUCKETS];
    uint32_t cbToRedrawMaxUs, cbToRedrawSamples;
    uint64_t cbToRedrawSumUs;
    uint32_t waitClears;
    uint32_t xBusyMaxUs, lockWaitMaxUs;
    uint64_t lockWaitSumUs;
} st = { .vsyncGapMinUs = UINT32_MAX };

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

static void fcPost(void *data) {
    if (fc.post64)
        fc.post64(fc.choreographer, fcFrameCallback64, data);
    else
        AChoreographer_postFrameCallback(fc.choreographer, fcFrameCallbackLong, data);
    FC_ADD(posts, 1);
}

static void fcNoteCallback(int64_t now, int64_t frameTimeNs, bool frameTimeValid) {
    FC_ADD(callbacks, 1);

    if (fc.lastCallbackNs) {
        uint32_t gapUs = fcUs(now - fc.lastCallbackNs);
        fcMax(&st.cbGapMaxUs, gapUs);
        FC_ADD(cbHist[fcHistBucket(gapUs)], 1);
    } else
        log(INFO, "XlorieFrameClock: first callback");
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

    if (__atomic_fetch_add(&fc.pendingTicks, 1, __ATOMIC_ACQ_REL) == 0)
        __atomic_store_n(&fc.oldestTickNs, now, __ATOMIC_RELEASE);

    FC_ADD(queueTry, 1);
    FC_ADD(queueActual, 1);
    lorieQueueRedraw();
}

static void fcOnFrame(int64_t frameTimeNs, bool frameTimeValid, void *data) {
    int64_t now = lorieFrameClockNowNs();

    fcNoteCallback(now, frameTimeNs, frameTimeValid);
    fcPost(data);
    fcTick(now);
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

void lorieFrameClockStart(void) {
    FcRegisterRefreshRateCallback registerRefreshRate = NULL;
    void *android;

    fc.ownerTid = gettid();
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

    log(INFO, "XlorieFrameClock: owner tid %d, postFrameCallback64 %s, refresh rate callback %s",
        fc.ownerTid, fc.post64 ? "yes" : "no", registerRefreshRate ? "yes" : "no");

    fcPost(NULL);
}

void lorieFrameClockSetXThread(void) {
    fc.xTid = gettid();
}

uint32_t lorieFrameClockRedrawBegin(void) {
    int64_t now = lorieFrameClockNowNs();
    uint32_t ticks = __atomic_exchange_n(&fc.pendingTicks, 0, __ATOMIC_ACQ_REL);
    int64_t oldest = __atomic_exchange_n(&fc.oldestTickNs, 0, __ATOMIC_ACQ_REL);

    FC_ADD(redraws, 1);
    if (fc.lastRedrawNs) {
        uint32_t gapUs = fcUs(now - fc.lastRedrawNs);
        fcMax(&st.redrawGapMaxUs, gapUs);
        FC_ADD(redrawHist[fcHistBucket(gapUs)], 1);
    }
    fc.lastRedrawNs = now;

    if (!ticks) {
        // An earlier lorieRedraw already took every tick that arrived: this one only replays.
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
    uint32_t callbacks = FC_TAKE(callbacks), posts = FC_TAKE(posts);
    uint32_t cbGapMaxUs = FC_TAKE(cbGapMaxUs), vsyncGapMaxUs = FC_TAKE(vsyncGapMaxUs);
    uint32_t vsyncGapMinUs = __atomic_exchange_n(&st.vsyncGapMinUs, UINT32_MAX, __ATOMIC_RELAXED);
    uint32_t cbLatencyMaxUs = FC_TAKE(cbLatencyMaxUs), cbLate = FC_TAKE(cbLate);
    uint32_t queueTry = FC_TAKE(queueTry), queueActual = FC_TAKE(queueActual);
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
    anomaly = cbGapMaxUs >= fcUs(FC_ANOMALY_GAP_NS) || replays || xBusyMaxUs >= fcUs(FC_ANOMALY_GAP_NS);
    if (!(surfaceAvailable && connected) && !anomaly && !debug)
        goto out;

    log(INFO, "XlorieFrameClock: app_hz=%u period_us=%u cb=%u post=%u cb_gap_max_us=%u "
              "cb_hist=%u/%u/%u/%u/%u/%u vsync_gap_min_us=%u vsync_gap_max_us=%u cb_lat_max_us=%u cb_late=%u "
              "queue_try=%u queue_actual=%u redraw=%u replay=%u ticks_max=%u multi_tick=%u "
              "cb_to_redraw_max_us=%u cb_to_redraw_avg_us=%u redraw_gap_max_us=%u redraw_hist=%u/%u/%u/%u/%u/%u",
        appHz, fcUs(__atomic_load_n(&fc.periodNs, __ATOMIC_RELAXED)), callbacks, posts, cbGapMaxUs,
        cbHist[0], cbHist[1], cbHist[2], cbHist[3], cbHist[4], cbHist[5],
        vsyncGapMinUs == UINT32_MAX ? 0 : vsyncGapMinUs, vsyncGapMaxUs, cbLatencyMaxUs, cbLate,
        queueTry, queueActual, redraws, replays, ticksMax, multiTick,
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
