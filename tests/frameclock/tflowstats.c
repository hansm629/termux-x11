/*
 * Host test of the input -> damage -> draw request -> renderer counters: flowstats.c (X server side) and
 * inputflow.c (activity side) are included directly; rendererFlowNoteFrame and rendererFlowNoteShouldWait
 * are cut out of renderer.c by run.sh (OUT/rendererflow.inc) and compiled against a minimal stand-in for
 * the shared state.
 *
 * Run through tests/frameclock/run.sh.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include "../../app/src/main/cpp/lorie/flowstats.c"
#include "../../app/src/main/cpp/lorie/inputflow.c"

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
/* The input flow stats in the shared state, as the X server sees them (its mapping is never unmapped);
 * the activity gets a mapping of its own, here a page standing in for it. */
static struct lorie_input_flow_stats sharedInputFlow;
static struct lorie_input_flow_stats *in = &sharedInputFlow;

static const char *report(void) {
    lorieFlowReport(&sharedState.renderFlow, in, 0, true, true);
    return lastLogWith("XlorieFlow: ");
}

static void attachActivity(void) {
    void *own = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    lorieInputFlowAttach(own, 4096, &sharedInputFlow);
}

static void reset(void) {
    memset(&fl, 0, sizeof(fl));
    lastMotionNs = lastDragMotionNs = lastInjectNs = firstMotionUnseenNs = firstDragUndrawnNs = 0;
    lastCursorNs = lastDmgOnNs = lastReqNs = firstTouchQueuedNs = firstInjectUnprocessedNs = 0;
    memset(&drag, 0, sizeof(drag));
    rxNextSeq = 0;
    memset((void *) &sharedState, 0, sizeof(sharedState));
    sharedState.surfaceAvailable = 1;
    memset(&sharedInputFlow, 0, sizeof(sharedInputFlow));
    logCount = 0;
    attachActivity();
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
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, 0, true);
    for (i = 0; i < 100; i++) {
        nowNs += STEP / 2;
        lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, 0, false);
        lorieFlowNoteInject();
        nowNs += STEP / 2;
        tick(true);
        nowNs += 1 * MS;
        frame();
    }
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, 0, false);
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
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, 0, true);
    for (i = 0; i < 60; i++) {          // 60 ticks = 500 ms of drag motion, nothing drawn
        nowNs += STEP;
        lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, 0, false);
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
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, 0, true);
    nowNs += STEP;
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, 0, false);
    nowNs += 30 * MS;
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_BUTTON, 1, 0, false);
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
    lorieFlowNoteInput(LORIE_FLOW_MOUSE_MOTION, 0, 0, false);
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
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 0, 0, false);
    nowNs += STEP;
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 1, 0, false);
    nowNs += 40 * MS;
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 2, 0, false);
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
    lorieFlowNoteInput(LORIE_FLOW_KEY, 0, 0, true);
    nowNs += STEP; tick(true); frame();
    report();
    CHECK(fieldOf(report(), "in_key") == 0 && fieldOf(lastLogWith("XlorieFlow: "), "dmg_on") == 0);
    CHECK(rf->framesDraw == 0 && rf->reqToFrameSamples == 0);
}

/* ---- measurement errors found on the device (ad0301b) ---- */

static void testStampNewerThanFrameStart(void) {
    const char *line;
    printf("overflow: a request stamped after the frame took its start time is counted apart, not as 4294 s\n");
    reset();
    nowNs += STEP; tick(true); nowNs += MS; frame();
    nowNs += STEP;
    sharedState.cursor.moved = 1;   // the frame starts for the cursor...
    // ... and right after it took frameStartNs, the X server stamps a draw request (and a cursor move).
    sharedState.drawRequested = 1;
    __atomic_store_n(&sharedState.renderFlow.drawRequestNs, nowNs + 2 * MS, __ATOMIC_RELAXED);
    __atomic_store_n(&sharedState.renderFlow.cursorMoveNs, nowNs + 1 * MS, __ATOMIC_RELAXED);
    frame();
    report();
    line = lastLogWith("XlorieFlowR: ");
    CHECK(fieldOf(line, "req_to_frame_invalid") == 1 && fieldOf(line, "cursor_to_frame_invalid") == 1);
    // Only the first frame's valid sample counts (1 ms), not 4294967294 from the negative one.
    CHECK(fieldOf(line, "req_to_frame_max_us") == 1000 && fieldOf(line, "cursor_to_frame_max_us") == 0);
    CHECK(fieldOf(line, "req_to_frame_avg_us") == 1000); // only the valid sample of the first frame
    CHECK(fieldOf(line, "gap_long_pending") == 0);
}

struct stress { volatile uint32_t *field; int n; };
static void *stressAdder(void *arg) {
    struct stress *st = arg;
    int i;
    for (i = 0; i < st->n; i++)
        LORIE_STAT_ADD(*st->field, 1);
    return NULL;
}

static void testWindowBoundaryLosesNothing(void) {
    struct stress renderer = { &sharedState.renderFlow.framesDraw, 300000 }, activity;
    pthread_t a, b;
    unsigned long frames = 0, sends = 0;
    int reports = 0;
    printf("boundary: counters added while the report takes them are never lost nor counted twice\n");
    reset();
    activity = (struct stress) { &in->sends, 300000 };
    pthread_create(&a, NULL, stressAdder, &renderer);
    pthread_create(&b, NULL, stressAdder, &activity);
    while (frames + sends < 600000 && reports < 1000000) {
        report();
        frames += fieldOf(lastLogWith("XlorieFlowR: "), "f_draw");
        sends += fieldOf(lastLogWith("XlorieInput: "), "a_send");
        reports++;
    }
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    report();
    frames += fieldOf(lastLogWith("XlorieFlowR: "), "f_draw");
    sends += fieldOf(lastLogWith("XlorieInput: "), "a_send");
    CHECK(frames == 300000 && sends == 300000);
    if (frames != 300000 || sends != 300000)
        printf("    frames %lu sends %lu over %d reports\n", frames, sends, reports);
}

static void testDirectTouchEndsForOtherIds(void) {
    const char *line;
    int i, id;
    printf("touch: the TouchEnd sent with every move for ids that are not down does not end the drag\n");
    reset();
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 0, 0, false);              // finger 0 down
    for (i = 0; i < 10; i++) {
        nowNs += STEP;
        lorieFlowNoteInput(LORIE_FLOW_TOUCH, 1, 0, false);          // what InputEventSender.sendTouchEvent sends
        for (id = 1; id < 10; id++)
            lorieFlowNoteInput(LORIE_FLOW_TOUCH, 2, id, false);
    }
    nowNs += 120 * MS;                                               // a real gap in the middle of the drag
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 1, 0, false);
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 1, 0, false);
    lorieFlowNoteInput(LORIE_FLOW_TOUCH, 2, 0, false);              // finger 0 up
    line = report();
    CHECK(fieldOf(line, "in_drag") == 13);
    CHECK(fieldOf(line, "in_drag_gap_max_us") == 120000);
}

/* ---- activity -> socket -> X server ---- */

/* What one pointer event does on the activity's side: written to the socket after `writeNs`. */
static uint32_t activitySend(int kind, int detail, int id, bool down, uint32_t key, int64_t writeNs) {
    int64_t start = lorieInputFlowBeforeSend(kind, detail, id, down, key);
    nowNs += writeNs;
    lorieInputFlowAfterSend(start, true);
    return key;
}

static void testTransportMatched(void) {
    const char *line;
    int i;
    printf("socket: each event read by the X server is timed against the activity's write\n");
    reset();
    activitySend(LORIE_FLOW_MOUSE_BUTTON, 1, 0, true, 0x100, 0);
    lorieFlowNoteReceive(0x100, in);
    for (i = 0; i < 20; i++) {
        nowNs += STEP;
        lorieInputFlowNoteMotion(2, true, nowNs - 1 * MS, nowNs - 1 * MS, 0, STEP);
        activitySend(LORIE_FLOW_MOUSE_MOTION, 0, 0, false, 0x200 + i, 50000);
        nowNs += (i == 10 ? 30 : 1) * MS;                           // one event spends 30 ms in the socket
        lorieFlowNoteReceive(0x200 + i, in);
    }
    report();
    line = lastLogWith("XlorieInput: ");
    CHECK(fieldOf(line, "a_events") == 20 && fieldOf(line, "a_drag") == 20);
    CHECK(fieldOf(line, "a_send") == 21 && fieldOf(line, "a_send_drag") == 20);
    CHECK(fieldOf(line, "a_ev_to_cb_max_us") == 1000);
    CHECK(fieldOf(line, "a_drag_sample_gap_max_us") == (unsigned) (STEP / 1000));
    CHECK(fieldOf(line, "a_write_max_us") == 50);
    CHECK(fieldOf(line, "xmit") == 21 && fieldOf(line, "xmit_unmatched") == 0);
    CHECK(fieldOf(line, "xmit_max_us") == 30050);
}

static void testTransportBeforeAttachAndOverwrite(void) {
    const char *line;
    int i;
    printf("socket: events the activity did not log stay unmatched, an overwritten log is skipped\n");
    reset();
    lorieInputFlowAttach(NULL, 0, NULL);                             // not attached yet: nothing logged
    activitySend(LORIE_FLOW_MOUSE_MOTION, 0, 0, false, 0x1, 0);
    lorieFlowNoteReceive(0x1, in);
    attachActivity();
    for (i = 0; i < LORIE_INPUT_LOG + 40; i++)                      // the X server falls far behind
        activitySend(LORIE_FLOW_MOUSE_MOTION, 0, 0, false, 0x1000 + i, 0);
    nowNs += 5 * MS;
    for (i = 0; i < LORIE_INPUT_LOG + 40; i++)
        lorieFlowNoteReceive(0x1000 + i, in);
    report();
    line = lastLogWith("XlorieInput: ");
    // The one sent before the activity attached and the first 40, overwritten before they were read, are
    // unmatched; the rest still match, in order.
    CHECK(fieldOf(line, "xmit") == LORIE_INPUT_LOG && fieldOf(line, "xmit_unmatched") == 41);
    if (fieldOf(line, "xmit_unmatched") != 41)
        printf("    %s\n", line);
    CHECK(fieldOf(line, "xmit_max_us") == 5000);
}

static void testTransportSameKeyInOrder(void) {
    int i;
    printf("socket: identical events (same key) match one by one, in order\n");
    reset();
    for (i = 0; i < 5; i++) {
        activitySend(LORIE_FLOW_MOUSE_MOTION, 0, 0, false, 0x77, 0);
        nowNs += MS;
    }
    for (i = 0; i < 5; i++)
        lorieFlowNoteReceive(0x77, in);
    report();
    CHECK(fieldOf(lastLogWith("XlorieInput: "), "xmit") == 5);
    CHECK(fieldOf(lastLogWith("XlorieInput: "), "xmit_max_us") == 5000); // the first one, sent 5 ms before
}

static void testClockMismatchDetected(void) {
    const char *line;
    printf("clock: event times from another time base are counted, not used\n");
    reset();
    lorieInputFlowNoteMotion(2, true, nowNs + 3600LL * 1000 * MS, 0, 0, 0);  // e.g. elapsedRealtime after sleep
    lorieInputFlowNoteMotion(2, true, nowNs - 20000 * MS, 0, 0, 0);
    lorieInputFlowNoteMotion(2, true, nowNs - 2 * MS, nowNs - 6 * MS, 3, 0);
    report();
    line = lastLogWith("XlorieInput: ");
    CHECK(fieldOf(line, "a_clock_bad") == 2);
    CHECK(fieldOf(line, "a_ev_to_cb_max_us") == 2000 && fieldOf(line, "a_oldest_to_cb_max_us") == 6000);
    CHECK(fieldOf(line, "a_hist") == 3);
}

static void testActivityDragGaps(void) {
    const char *line;
    printf("activity: drag callback gaps restart at every gesture\n");
    reset();
    lorieInputFlowNoteMotion(0, false, nowNs, nowNs, 0, 0);          // DOWN
    nowNs += STEP; lorieInputFlowNoteMotion(2, true, nowNs, nowNs, 0, STEP);
    nowNs += 90 * MS; lorieInputFlowNoteMotion(2, true, nowNs, nowNs, 0, 90 * MS);
    lorieInputFlowNoteMotion(1, false, nowNs, nowNs, 0, 0);          // UP
    nowNs += 3000 * MS; lorieInputFlowNoteMotion(0, false, nowNs, nowNs, 0, 0);
    nowNs += STEP; lorieInputFlowNoteMotion(2, true, nowNs, nowNs, 0, STEP);
    report();
    line = lastLogWith("XlorieInput: ");
    CHECK(fieldOf(line, "a_drag") == 3 && fieldOf(line, "a_moves") == 3);
    CHECK(fieldOf(line, "a_drag_cb_gap_max_us") == 90000);         // not the 3 s between the gestures
    CHECK(fieldOf(line, "a_drag_sample_gap_max_us") == 90000);
}

static void testXSideQueues(void) {
    const char *line;
    printf("x server: queued touch -> handleTouchEvent, injected input -> ProcessInputEvents\n");
    reset();
    lorieFlowNoteTouchQueued();
    nowNs += 3 * MS;
    lorieFlowNoteTouchQueued();                                      // the oldest one is what is timed
    nowNs += 4 * MS;
    lorieFlowNoteTouchHandled();
    lorieFlowNoteInject();
    nowNs += 9 * MS;
    lorieFlowNoteProcessInput();
    lorieFlowNoteProcessInput();                                     // nothing new: not a run
    report();
    line = lastLogWith("XlorieInput: ");
    CHECK(fieldOf(line, "touch_queue_max_us") == 7000);
    CHECK(fieldOf(line, "x_process") == 1 && fieldOf(line, "inject_to_process_max_us") == 9000);
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
    testStampNewerThanFrameStart();
    testWindowBoundaryLosesNothing();
    testDirectTouchEndsForOtherIds();
    testTransportMatched();
    testTransportBeforeAttachAndOverwrite();
    testTransportSameKeyInOrder();
    testClockMismatchDetected();
    testActivityDragGaps();
    testXSideQueues();
    printf(failures ? "flowstats: %d FAILED\n" : "flowstats: PASS\n", failures);
    return failures != 0;
}
