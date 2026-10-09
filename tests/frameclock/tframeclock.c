/*
 * Host test of the frame clock (app/src/main/cpp/lorie/frameclock.c), built against test doubles of
 * AChoreographer/ALooper (the headers in fake/android), a fake clock and a fake X server side: frameclock.c is
 * included directly so its state can be checked. Each test drives "vsyncs" the way
 * Choreographer::dispatchVsync does - every callback posted so far runs once - and plays the X server
 * main thread by calling lorieFrameClockRedrawBegin() like lorieRedraw does.
 *
 * Run through tests/frameclock/run.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include "../../app/src/main/cpp/lorie/frameclock.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- clock ---- */
static int64_t nowNs = 1000LL * NS_PER_MS;
int fake_clock_gettime(__unused clockid_t clock, struct timespec *ts) {
    ts->tv_sec = nowNs / 1000000000LL;
    ts->tv_nsec = nowNs % 1000000000LL;
    return 0;
}

/* ---- watchdog timerfd and the owner thread's Looper ---- */
static int64_t timerDeadlineNs; /* 0: disarmed */
static int timerFdNo = -1, looperDummy, timerFires;
static struct { int fd; ALooper_callbackFunc callback; } looperFds[4];
static int nLooperFds;
int fake_timerfd_create(__unused int clockid, __unused int flags) {
    if (timerFdNo < 0)
        timerFdNo = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); // something read() can drain
    return timerFdNo;
}
int fake_timerfd_settime(__unused int fd, __unused int flags, const struct itimerspec *value, __unused struct itimerspec *old) {
    int64_t ns = (int64_t) value->it_value.tv_sec * 1000000000LL + value->it_value.tv_nsec;
    timerDeadlineNs = ns ? nowNs + ns : 0;
    return 0;
}
ALooper *ALooper_forThread(void) { return (ALooper *) &looperDummy; }
int ALooper_addFd(__unused ALooper *looper, int fd, __unused int ident, __unused int events, ALooper_callbackFunc callback, __unused void *data) {
    int i;
    for (i = 0; i < nLooperFds && looperFds[i].fd != fd; i++)
        ;
    looperFds[i].fd = fd;
    looperFds[i].callback = callback;
    if (i == nLooperFds)
        nLooperFds++;
    return 1;
}
static void looperDispatch(int fd) {
    int i;
    for (i = 0; i < nLooperFds; i++)
        if (looperFds[i].fd == fd)
            looperFds[i].callback(fd, ALOOPER_EVENT_INPUT, NULL);
}
/* Lets time pass; the watchdog fires when its deadline is reached, like the Looper would. */
static void advance(int64_t ns) {
    int64_t end = nowNs + ns;
    while (timerDeadlineNs && timerDeadlineNs <= end) {
        nowNs = timerDeadlineNs;
        timerDeadlineNs = 0;
        timerFires++;
        looperDispatch(timerFdNo);
    }
    nowNs = end;
}

/* What the activity coming back does: the resume check, then the owner thread's Looper running it. */
static void resumeKick(void) {
    lorieFrameClockResumeCheck();
    looperDispatch(fc.kickFd);
}

/* ---- choreographer ---- */
struct AChoreographer { int unused; };
static struct AChoreographer theChoreographer;
typedef struct { AChoreographer_frameCallback cb; FcFrameCallback64 cb64; void *data; } Posted;
#define MAX_POSTED 64
static Posted posted[MAX_POSTED];
static int nposted;
static int have64 = 1;

AChoreographer *AChoreographer_getInstance(void) { return &theChoreographer; }
void AChoreographer_postFrameCallback(__unused AChoreographer *c, AChoreographer_frameCallback cb, void *data) {
    posted[nposted++] = (Posted) { .cb = cb, .data = data };
}
static void fakePost64(__unused AChoreographer *c, FcFrameCallback64 cb, void *data) {
    posted[nposted++] = (Posted) { .cb64 = cb, .data = data };
}
void *fake_dlopen(__unused const char *name, __unused int flags) { return (void *) 1; }
void *fake_dlsym(__unused void *handle, const char *symbol) {
    if (!strcmp(symbol, "AChoreographer_postFrameCallback64"))
        return have64 ? (void *) fakePost64 : NULL;
    return NULL;
}

/* One VSYNC at the current time, handed `frameTimeNs`: runs every callback posted before it. */
static int vsyncAt(int64_t frameTimeNs) {
    Posted cbs[MAX_POSTED];
    int i, n = nposted;
    memcpy(cbs, posted, sizeof(cbs[0]) * n);
    nposted = 0;
    for (i = 0; i < n; i++) {
        if (cbs[i].cb64)
            cbs[i].cb64(frameTimeNs, cbs[i].data);
        else
            cbs[i].cb((long) frameTimeNs, cbs[i].data);
    }
    return n;
}
static int vsync(void) { return vsyncAt(nowNs); }

/* ---- X server side ---- */
static int queued, screenReady = 1, surfaceShown = 1;
bool lorieScreenReady(void) { return screenReady; }
bool lorieSurfaceShown(void) { return surfaceShown; }
void lorieQueueRedraw(void) { queued++; }

/* ---- log ---- */
static char logBuf[16][2048];
static int logCount;
int fake_log(__unused int prio, __unused const char *tag, const char *fmt, ...) {
    va_list ap;
    char *line = logBuf[logCount++ % 16];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(logBuf[0]), fmt, ap);
    va_end(ap);
    if (getenv("VERBOSE"))
        printf("    log: %s\n", line);
    return 0;
}
static const char *lastLogWith(const char *prefix) {
    int i;
    for (i = logCount - 1; i >= 0 && i >= logCount - 16; i--)
        if (strstr(logBuf[i % 16], prefix) == logBuf[i % 16])
            return logBuf[i % 16];
    return "";
}
static unsigned fieldOf(const char *line, const char *key) {
    char k[64];
    const char *p;
    snprintf(k, sizeof(k), " %s=", key);
    p = strstr(line, k);
    return p ? (unsigned) strtoul(p + strlen(k), NULL, 10) : 0xdeadbeef;
}

static struct lorie_frame_clock_stats rs;
static const char *report(void) {
    lorieFrameClockReport(&rs, 0, true, true, 120);
    return lastLogWith("XlorieFrameClock: ");
}

static void reset(void) {
    memset(&fc, 0, sizeof(fc));
    memset(&st, 0, sizeof(st));
    st.vsyncGapMinUs = UINT32_MAX;
    memset(&rs, 0, sizeof(rs));
    nposted = queued = timerFires = nLooperFds = 0;
    timerDeadlineNs = 0;
    screenReady = surfaceShown = 1;
    have64 = 1;
    lorieFrameClockStart();
}

#define STEP (8333333LL) /* 120 Hz */

static void testChainReposts(void) {
    int i;
    printf("chain: every callback re-posts itself and queues one tick\n");
    reset();
    CHECK(nposted == 1);
    for (i = 0; i < 10; i++) {
        nowNs += STEP;
        CHECK(vsync() == 1);
        CHECK(nposted == 1);
        CHECK(lorieFrameClockRedrawBegin() == 1);
    }
    CHECK(queued == 10);
    report();
}

static void testBacklogReplay(void) {
    const char *line;
    printf("backlog: ticks that pile up while the X server is busy are taken by the first redraw\n");
    reset();
    nowNs += STEP; vsync();
    nowNs += STEP; vsync();
    nowNs += STEP; vsync();
    CHECK(queued == 3);
    nowNs += 2 * NS_PER_MS;
    CHECK(lorieFrameClockRedrawBegin() == 3);
    CHECK(lorieFrameClockRedrawBegin() == 0);
    CHECK(lorieFrameClockRedrawBegin() == 0);
    line = report();
    CHECK(fieldOf(line, "queue_try") == 3);
    CHECK(fieldOf(line, "redraw") == 3);
    CHECK(fieldOf(line, "replay") == 2);
    CHECK(fieldOf(line, "ticks_max") == 3);
    CHECK(fieldOf(line, "multi_tick") == 1);
    CHECK(fieldOf(line, "cb_to_redraw_max_us") == (unsigned) ((2 * STEP + 2 * NS_PER_MS) / 1000));
}

static void testLatencyAndGaps(void) {
    const char *line;
    printf("timing: callback latency against its VSYNC timestamp, VSYNC and callback gaps\n");
    reset();
    nowNs += STEP; vsync();
    lorieFrameClockRedrawBegin();
    // The thread was blocked: the next VSYNC (on time) is only handled 300 ms later.
    nowNs += 300 * NS_PER_MS;
    vsyncAt(nowNs - 300 * NS_PER_MS + STEP);
    lorieFrameClockRedrawBegin();
    line = report();
    CHECK(fieldOf(line, "cb_gap_max_us") == 300000);
    CHECK(fieldOf(line, "vsync_gap_max_us") == (unsigned) (STEP / 1000));
    CHECK(fieldOf(line, "cb_lat_max_us") == (unsigned) ((300 * NS_PER_MS - STEP) / 1000));
    CHECK(fieldOf(line, "cb_late") == 1);
    // No VSYNC for 300 ms: the callback runs on time, the VSYNC gap is the gap.
    nowNs += STEP; vsync();
    nowNs += 300 * NS_PER_MS; vsync();
    line = report();
    CHECK(fieldOf(line, "vsync_gap_max_us") == 300000);
    CHECK(fieldOf(line, "cb_late") == 0);
}

static void testNo64(void) {
    printf("api: without postFrameCallback64 the long variant keeps the chain going\n");
    reset();
    have64 = 0;
    memset(&fc, 0, sizeof(fc));
    nposted = 0;
    lorieFrameClockStart();
    CHECK(fc.post64 == NULL);
    CHECK(nposted == 1 && posted[0].cb && !posted[0].cb64);
    nowNs += STEP;
    CHECK(vsync() == 1 && nposted == 1 && posted[0].cb);
}

static void testNoScreen(void) {
    printf("screen: no ticks are queued before the screen exists\n");
    reset();
    screenReady = 0;
    nowNs += STEP; vsync();
    CHECK(queued == 0);
    CHECK(nposted == 1);
    CHECK(lorieFrameClockRedrawBegin() == 0);
}

/* Plays the X server keeping up: every queued tick is taken right away. */
static void runFrames(int n, int64_t step) {
    int i;
    for (i = 0; i < n; i++) {
        advance(step);
        vsync();
        while (lorieFrameClockRedrawBegin())
            ;
    }
}

static void testHealthyChainNeverRearms(void) {
    const char *line;
    printf("watchdog: a healthy 120 Hz chain never wakes it\n");
    reset();
    runFrames(1200, STEP);
    CHECK(timerFires == 0);
    line = report();
    CHECK(fieldOf(line, "stall") == 0 && fieldOf(line, "rearm") == 0 && fieldOf(line, "stale_cb") == 0);
    CHECK(fieldOf(line, "gen") == 1);
}

static void testSlowPanelNeverRearms(void) {
    const char *line;
    printf("watchdog: a panel idling at 1 Hz is a stall to report, not to re-arm\n");
    reset();
    runFrames(10, 1000 * NS_PER_MS);
    line = report();
    CHECK(fieldOf(line, "rearm") == 0);
    CHECK(fieldOf(line, "stall") == 10);
    CHECK(fieldOf(line, "gen") == 1);
    CHECK(nposted == 1);
}

static void testStaleCallbackIsDropped(void) {
    const char *line;
    int q;
    printf("stale: a callback of a replaced generation neither ticks nor re-posts\n");
    reset();
    runFrames(5, STEP);
    fcRearm("test", 0);             // two callbacks registered now: generation 1 and 2
    CHECK(nposted == 2);
    q = queued;
    advance(STEP);
    CHECK(vsync() == 2);
    CHECK(queued == q + 1);         // one tick for one VSYNC
    CHECK(nposted == 1);            // only generation 2 re-posted
    CHECK((uint32_t) (uintptr_t) posted[0].data == 2);
    lorieFrameClockRedrawBegin();
    runFrames(5, STEP);
    CHECK(queued == q + 6);
    line = report();
    CHECK(fieldOf(line, "stale_cb") == 1);
    CHECK(fieldOf(line, "forced_rearm") == 1);
    CHECK(fieldOf(line, "gen") == 2);
}

static void testLostChainRecovers(void) {
    const char *line;
    printf("lost: a chain with nothing registered is re-armed after the stall threshold\n");
    reset();
    runFrames(5, STEP);
    // The last callback did not re-post (what TERMUX_X11_CHOREO_TEST_DROP does).
    nposted = 0;
    fc.outstanding = false;
    advance(FC_STALL_NS - NS_PER_MS);
    CHECK(fc.generation == 1 && nposted == 0);
    advance(2 * NS_PER_MS);
    CHECK(fc.generation == 2 && nposted == 1 && fc.outstanding);
    CHECK(strstr(lastLogWith("XlorieFrameClock: re-arming"), "(lost)") != NULL);
    runFrames(10, STEP);
    CHECK(nposted == 1);
    CHECK(strstr(lastLogWith("XlorieFrameClock: callbacks are back"), "generation 2") != NULL);
    line = report();
    CHECK(fieldOf(line, "rearm") == 1 && fieldOf(line, "forced_rearm") == 0 && fieldOf(line, "stale_cb") == 0);
    // Once recovered, a healthy chain does not wake the watchdog again.
    timerFires = 0;
    runFrames(600, STEP);
    CHECK(timerFires == 0);
}

static void testSilentChainRearmsOnce(void) {
    const char *line;
    printf("silent: a registered callback that never fires is replaced once per stall\n");
    reset();
    runFrames(5, STEP);
    nposted = 0;                    // Choreographer dropped it; we still think it is registered
    advance(FC_SILENT_NS - NS_PER_MS);
    CHECK(fc.generation == 1);
    advance(2 * NS_PER_MS);
    CHECK(fc.generation == 2 && nposted == 1);
    advance(60000 * NS_PER_MS);     // still nothing: no second re-arm, and no more wake-ups
    CHECK(fc.generation == 2 && nposted == 1);
    CHECK(timerDeadlineNs == 0);
    runFrames(3, STEP);
    line = report();
    CHECK(fieldOf(line, "rearm") == 1 && fieldOf(line, "forced_rearm") == 1);
}

static void testSilentWithoutSurfaceWaits(void) {
    printf("silent: with nothing on screen the chain is only looked at every few seconds\n");
    reset();
    runFrames(5, STEP);
    nposted = 0;
    surfaceShown = 0;
    timerFires = 0;
    advance(60000 * NS_PER_MS);
    CHECK(fc.generation == 1);
    CHECK(timerFires <= 2 + 60000 / 5000);
    surfaceShown = 1;
    advance(FC_IDLE_CHECK_NS);
    CHECK(fc.generation == 2 && nposted == 1);
    CHECK(fieldOf(report(), "stall") == 1);   // one stall, however often it was looked at
}

static void testResumeRearmsAQuietChain(void) {
    const char *line;
    printf("resume: coming back to a quiet chain re-arms it at once, a running one is left alone\n");
    reset();
    runFrames(5, STEP);
    resumeKick();                   // callbacks are flowing: nothing to do
    CHECK(fc.generation == 1);
    nposted = 0;                    // the screen goes off and the registered callback is never delivered
    surfaceShown = 0;
    advance(30000 * NS_PER_MS);
    CHECK(fc.generation == 1);
    surfaceShown = 1;
    resumeKick();                   // new surface: re-armed without waiting for the silence limit
    CHECK(fc.generation == 2 && nposted == 1);
    resumeKick();                   // a second event of the same resume does not re-arm again
    CHECK(fc.generation == 2 && nposted == 1);
    runFrames(5, STEP);
    line = report();
    CHECK(fieldOf(line, "rearm") == 1 && fieldOf(line, "forced_rearm") == 1 && fieldOf(line, "resume_check") == 3);
    CHECK(strstr(lastLogWith("XlorieFrameClock: re-arming"), "(resume)") != NULL);
}

static void testResumeOldCallbackLateIsStale(void) {
    const char *line;
    int q;
    printf("resume: if the replaced callback turns up after all, it is stale and the cadence stays single\n");
    reset();
    runFrames(5, STEP);
    advance(500 * NS_PER_MS);       // quiet, but the generation 1 callback is still registered
    resumeKick();
    CHECK(fc.generation == 2 && nposted == 2);
    q = queued;
    advance(STEP);
    CHECK(vsync() == 2);
    CHECK(queued == q + 1 && nposted == 1);
    line = report();
    CHECK(fieldOf(line, "stale_cb") == 1);
}

int main(void) {
    testChainReposts();
    testBacklogReplay();
    testLatencyAndGaps();
    testNo64();
    testNoScreen();
    testHealthyChainNeverRearms();
    testSlowPanelNeverRearms();
    testStaleCallbackIsDropped();
    testLostChainRecovers();
    testSilentChainRearmsOnce();
    testSilentWithoutSurfaceWaits();
    testResumeRearmsAQuietChain();
    testResumeOldCallbackLateIsStale();
    printf(failures ? "frameclock: %d FAILED\n" : "frameclock: PASS\n", failures);
    return failures != 0;
}
