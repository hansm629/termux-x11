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
static int queued, screenReady = 1;
bool lorieScreenReady(void) { return screenReady; }
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
    nposted = queued = 0;
    screenReady = 1;
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

int main(void) {
    testChainReposts();
    testBacklogReplay();
    testLatencyAndGaps();
    testNo64();
    testNoScreen();
    printf(failures ? "frameclock: %d FAILED\n" : "frameclock: PASS\n", failures);
    return failures != 0;
}
