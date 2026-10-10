/*
 * Host test of the input -> damage -> draw request -> renderer counters: flowstats.c (X server side) is
 * included directly; rendererFlowNoteFrame and rendererFlowNoteShouldWait are cut out of renderer.c by
 * run.sh (OUT/rendererflow.inc) and compiled against a minimal stand-in for the shared state.
 *
 * Run through tests/frameclock/run.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app/src/main/cpp/lorie/flowstats.c"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MS (1000000LL)
static int64_t nowNs = 1000 * MS;
int64_t lorieFrameClockNowNs(void) { return nowNs; }

static char logBuf[8][4096];
static int logCount;
int fake_log(__unused int prio, __unused const char *tag, const char *fmt, ...) {
    va_list ap;
    char *line = logBuf[logCount++ % 8];
    va_start(ap, fmt);
    vsnprintf(line, sizeof(logBuf[0]), fmt, ap);
    va_end(ap);
    if (getenv("VERBOSE"))
        printf("    log: %s\n", line);
    return 0;
}
static const char *lastLogWith(const char *prefix) {
    int i;
    for (i = logCount - 1; i >= 0 && i >= logCount - 8; i--)
        if (strstr(logBuf[i % 8], prefix) == logBuf[i % 8])
            return logBuf[i % 8];
    return "";
}
static unsigned fieldOf(const char *line, const char *key) {
    char k[64];
    const char *p;
    snprintf(k, sizeof(k), " %s=", key);
    p = strstr(line, k);
    return p ? (unsigned) strtoul(p + strlen(k), NULL, 10) : 0xdeadbeef;
}

/* ---- renderer side, cut out of renderer.c ---- */
#define LORIE_LONG_FRAME_US 33000
static struct {
    volatile uint8_t drawRequested, surfaceAvailable, waitForNextFrame;
    struct { volatile uint8_t moved, updated; } cursor;
    struct lorie_render_flow_stats renderFlow;
} sharedState;
static volatile __typeof__(sharedState) *state = &sharedState;
static inline int64_t rendererNsToUs(int64_t ns) { return ns / 1000LL; }
#include "rendererflow.inc"

static struct lorie_render_flow_stats *rf = (struct lorie_render_flow_stats *) &sharedState.renderFlow;

static const char *report(void) {
    lorieFlowReport(&sharedState.renderFlow, 0, true, true);
    return lastLogWith("XlorieFlow: ");
}

static void reset(void) {
    memset(&fl, 0, sizeof(fl));
    lastMotionNs = lastDragMotionNs = lastInjectNs = firstMotionUnseenNs = firstDragUndrawnNs = 0;
    lastCursorNs = lastDmgOnNs = lastReqNs = 0;
    buttonsDown = touchesDown = 0;
    memset((void *) &sharedState, 0, sizeof(sharedState));
    sharedState.surfaceAvailable = 1;
    logCount = 0;
}

/* One frame tick of the X server: damage or not, the way lorieRedraw reports it. */
static void tick(bool damaged) {
    lorieFlowNoteDamage(damaged, sharedState.drawRequested, &sharedState.renderFlow);
    if (damaged)
        sharedState.drawRequested = 1;
}
/* One renderer frame: what rendererRedrawLocked does with the flags. */
static void frame(void) {
    rendererFlowNoteFrame(nowNs);
    sharedState.drawRequested = 0;
    sharedState.cursor.moved = sharedState.cursor.updated = 0;
}

#define STEP (8333333LL)

static void testDragDrawnEveryTick(void) {
    const char *line;
    int i;
    printf("drag: motion drawn on every tick - short input -> damage, no idle-vs-pending confusion\n");
    reset();
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, true);
    for (i = 0; i < 100; i++) {
        nowNs += STEP / 2;
        lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, false);
        lorieFlowNoteInject();
        nowNs += STEP / 2;
        tick(true);
        nowNs += 1 * MS;
        frame();
    }
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, false);
    line = report();
    CHECK(fieldOf(line, "in_motion") == 100 && fieldOf(line, "in_drag") == 100 && fieldOf(line, "in_btn") == 2);
    CHECK(fieldOf(line, "inject") == 100);
    CHECK(fieldOf(line, "dmg_on") == 100 && fieldOf(line, "dmg_off") == 0);
    CHECK(fieldOf(line, "req") == 100 && fieldOf(line, "req_already") == 0);
    CHECK(fieldOf(line, "in_to_dmg_max_us") == (unsigned) (STEP / 2 / 1000));
    CHECK(fieldOf(line, "drag_nodmg_max_us") == 0);   // every motion was drawn on the next tick
    CHECK(strstr(line, " req_hist=0/99/0/0/0/0 ") != NULL);
    line = lastLogWith("XlorieFlowR: ");
    CHECK(fieldOf(line, "f_draw") == 100);
    CHECK(strstr(line, " frame_hist=0/99/0/0/0/0 ") != NULL);
    CHECK(fieldOf(line, "req_to_frame_max_us") == 1000);
    CHECK(fieldOf(line, "gap_long_idle") == 0 && fieldOf(line, "gap_long_pending") == 0);
}

static void testDragNotDrawn(void) {
    const char *line;
    int i;
    printf("drag: input keeps coming for 500 ms, no damage - drag_nodmg shows it, the renderer gap is idle\n");
    reset();
    nowNs += STEP; tick(true); frame();
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, true);
    for (i = 0; i < 60; i++) {          // 60 ticks = 500 ms of drag motion, nothing drawn
        nowNs += STEP;
        lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, false);
        tick(false);
    }
    nowNs += STEP;
    tick(true);                          // finally drawn
    nowNs += MS;
    frame();
    line = report();
    CHECK(fieldOf(line, "dmg_off") == 60 && fieldOf(line, "dmg_on") == 2);
    CHECK(fieldOf(line, "drag_nodmg_max_us") == (unsigned) (59 * STEP / 1000));
    CHECK(fieldOf(line, "in_to_dmg_max_us") == (unsigned) (60 * STEP / 1000));
    CHECK(fieldOf(line, "dmg_gap_max_us") == (unsigned) (61 * STEP / 1000));
    line = lastLogWith("XlorieFlowR: ");
    // The renderer was idle for that long: nothing was there to draw.
    CHECK(fieldOf(line, "gap_long_idle") == 1 && fieldOf(line, "gap_long_pending") == 0);
    CHECK(fieldOf(line, "gap_idle_max_us") == (unsigned) ((61 * STEP + MS) / 1000));
}

static void testRendererLateWithRequest(void) {
    const char *line;
    printf("renderer: a request that waits 200 ms for its frame is a pending gap\n");
    reset();
    nowNs += STEP; tick(true); frame();
    nowNs += STEP;
    tick(true);                          // request made...
    nowNs += 200 * MS;                   // ... the renderer only gets to it now
    frame();
    report();
    line = lastLogWith("XlorieFlowR: ");
    CHECK(fieldOf(line, "gap_long_pending") == 1 && fieldOf(line, "gap_long_idle") == 0);
    CHECK(fieldOf(line, "gap_pending_max_us") == 200000);
    CHECK(fieldOf(line, "req_to_frame_max_us") == 200000);
}

static void testRequestAlreadyPending(void) {
    const char *line;
    printf("request: damage while the renderer has not taken the last request rides on it\n");
    reset();
    nowNs += STEP; tick(true);
    nowNs += STEP; tick(true);
    nowNs += STEP; tick(true);
    frame();
    line = report();
    CHECK(fieldOf(line, "req") == 1 && fieldOf(line, "req_already") == 2);
    // The latency is from the first request, not the last damage.
    CHECK(fieldOf(lastLogWith("XlorieFlowR: "), "req_to_frame_max_us") == (unsigned) (2 * STEP / 1000));
}

static void testDragEndsUndrawn(void) {
    const char *line;
    printf("drag: a drag released before anything was drawn stops the no-damage clock\n");
    reset();
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, true);
    nowNs += STEP;
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, false);
    nowNs += 30 * MS;
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, false);
    nowNs += 2000 * MS;
    tick(false);                         // long after the drag: not counted as undrawn drag input
    line = report();
    CHECK(fieldOf(line, "drag_nodmg_max_us") == 30000);
}

static void testHoverIsNotDrag(void) {
    const char *line;
    printf("hover: motion without a button is input and cursor, never undrawn drag input\n");
    reset();
    nowNs += STEP;
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, false);
    lorieFlowNoteCursorMove(false, &sharedState.renderFlow);
    sharedState.cursor.moved = 1;
    nowNs += 500 * MS;
    tick(false);
    nowNs += MS;
    frame();
    line = report();
    CHECK(fieldOf(line, "in_motion") == 1 && fieldOf(line, "in_drag") == 0);
    CHECK(fieldOf(line, "drag_nodmg_max_us") == 0);
    CHECK(fieldOf(line, "cursor") == 1);
    CHECK(fieldOf(lastLogWith("XlorieFlowR: "), "f_cursor") == 1);
    CHECK(fieldOf(lastLogWith("XlorieFlowR: "), "cursor_to_frame_max_us") == 501000);
}

static void testTouchDrag(void) {
    const char *line;
    printf("touch: begin/update/end is a drag\n");
    reset();
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 0, false);
    nowNs += STEP;
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 1, false);
    nowNs += 40 * MS;
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 2, false);
    line = report();
    CHECK(fieldOf(line, "in_touch") == 3 && fieldOf(line, "in_drag") == 2);
    CHECK(fieldOf(line, "in_drag_gap_max_us") == (unsigned) (STEP / 1000));
    CHECK(fieldOf(line, "drag_nodmg_max_us") == (unsigned) ((STEP + 40 * MS) / 1000));
}

static void testShouldWaitReasons(void) {
    const char *line;
    printf("renderer: why the loop sleeps, first reason wins\n");
    reset();
    sharedState.surfaceAvailable = 0;
    sharedState.waitForNextFrame = 1;
    rendererFlowNoteShouldWait(true);
    sharedState.surfaceAvailable = 1;
    rendererFlowNoteShouldWait(true);
    sharedState.waitForNextFrame = 0;
    rendererFlowNoteShouldWait(true);
    rendererFlowNoteShouldWait(false);
    report();
    line = lastLogWith("XlorieFlowR: ");
    CHECK(fieldOf(line, "sw_nosurface") == 1 && fieldOf(line, "sw_waitframe") == 1);
    CHECK(fieldOf(line, "sw_buffers") == 1 && fieldOf(line, "sw_idle") == 1);
}

static void testReportResets(void) {
    printf("report: every window starts from zero\n");
    reset();
    lorieFlowNoteInput(LORIE_FLOW_KEY, 0, true);
    nowNs += STEP; tick(true); frame();
    report();
    CHECK(fieldOf(report(), "in_key") == 0 && fieldOf(lastLogWith("XlorieFlow: "), "dmg_on") == 0);
    CHECK(rf->framesDraw == 0 && rf->reqToFrameSamples == 0);
}

int main(void) {
    testDragDrawnEveryTick();
    testDragNotDrawn();
    testRendererLateWithRequest();
    testRequestAlreadyPending();
    testDragEndsUndrawn();
    testHoverIsNotDrag();
    testTouchDrag();
    testShouldWaitReasons();
    testReportResets();
    printf(failures ? "flowstats: %d FAILED\n" : "flowstats: PASS\n", failures);
    return failures != 0;
}
