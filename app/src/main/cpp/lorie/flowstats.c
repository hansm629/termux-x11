/*
 * Input -> damage -> draw request -> renderer, counted per 5 second window (see flowstats.h). Pure
 * measurement: nothing here changes what any stage does or when.
 *
 * Threads: input notes come from the X server's input thread, injection from it or (touch) from the main
 * thread; lorieMoveCursor runs wherever the X server moves the sprite (from QueuePointerEvents on the
 * input thread for pointer motion, or the main thread); the rest is the main thread, which also prints
 * and resets. Counters are atomics; a sample racing the reset at a window boundary may land in either
 * window.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <android/log.h>
#include "flowstats.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

#define FL_HIST_BUCKETS 6
/* How far past the last matched entry of the activity's send log a received event is looked for. */
#define FL_MATCH_SCAN 32

static struct {
    /* input thread */
    uint32_t mouseMotion, mouseButton, mouseScroll, touch, stylus, key, dragMotion;
    uint32_t motionGapMaxUs, dragGapMaxUs;
    uint32_t motionHist[FL_HIST_BUCKETS];
    uint32_t rxMatched, rxUnmatched, rxMaxUs;
    uint64_t rxSumUs LORIE_ATOMIC64;
    /* input thread or main thread */
    uint32_t inject, injectGapMaxUs;
    uint32_t cursor, cursorGapMaxUs, inToCursorMaxUs;
    /* main thread */
    uint32_t touchQueueMaxUs, processRuns, injectToProcessMaxUs;
    uint32_t dmgChecks, dmgOn, dmgOff, dmgSkipped, dmgGapMaxUs;
    uint32_t dmgHist[FL_HIST_BUCKETS];
    uint32_t inToDmgMaxUs, dragNoDmgMaxUs;
    uint32_t req, reqAlready, reqGapMaxUs, sigDraw, sigCursor;
    uint32_t reqHist[FL_HIST_BUCKETS];
} fl;

/* Timestamps (CLOCK_MONOTONIC ns); 0 = none. The ones shared between threads are 8 byte aligned on every
 * ABI, so their atomics stay lock-free (see LORIE_ATOMIC64). */
typedef int64_t flAtomicNs LORIE_ATOMIC64;
static int64_t lastMotionNs, lastDragMotionNs;           /* input thread */
static LorieDragTracker drag;                            /* input thread */
static uint64_t rxNextSeq;                               /* input thread: next send log entry expected */
static flAtomicNs lastInjectNs;              /* either thread */
static flAtomicNs lastCursorNs;              /* lorieMoveCursor: input thread (pointer motion) or main */
static flAtomicNs firstMotionUnseenNs;       /* input thread sets, lorieMoveCursor takes */
static flAtomicNs firstDragUndrawnNs;        /* input thread sets, main thread takes at damage */
static flAtomicNs firstTouchQueuedNs;        /* input thread sets, handleTouchEvent takes */
static flAtomicNs firstInjectUnprocessedNs;  /* injection sets, ProcessInputEvents takes */
static int64_t lastDmgOnNs, lastReqNs;                   /* main thread */

#define FL_ADD(field, n) __atomic_fetch_add(&fl.field, (n), __ATOMIC_RELAXED)
#define FL_TAKE(field) __atomic_exchange_n(&fl.field, 0, __ATOMIC_RELAXED)

static void flMax(volatile uint32_t *field, uint32_t v) {
    if (v > __atomic_load_n(field, __ATOMIC_RELAXED))
        __atomic_store_n(field, v, __ATOMIC_RELAXED);
}

static uint32_t flUs(int64_t ns) {
    int64_t us = ns / 1000;
    return us < 0 ? 0 : us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
}

/* Same buckets as the frame clock: <4 / <12 / <25 / <50 / <100 / >=100 ms. */
static int flHistBucket(uint32_t us) {
    return us < 4000 ? 0 : us < 12000 ? 1 : us < 25000 ? 2 : us < 50000 ? 3 : us < 100000 ? 4 : 5;
}

static void flGap(int64_t now, int64_t *last, uint32_t *maxUs, uint32_t *hist) {
    if (*last) {
        uint32_t us = flUs(now - *last);
        flMax(maxUs, us);
        if (hist)
            __atomic_fetch_add(&hist[flHistBucket(us)], 1, __ATOMIC_RELAXED);
    }
    *last = now;
}

static void flSetIfUnset(flAtomicNs *field, int64_t now) {
    int64_t expected = 0;
    __atomic_compare_exchange_n(field, &expected, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/* How long ago `*field` was set, taking (clearing) it; 0 when it was not set. */
static uint32_t flTakeAge(flAtomicNs *field, int64_t now) {
    int64_t then = __atomic_exchange_n(field, 0, __ATOMIC_ACQ_REL);
    return then ? flUs(now - then) : 0;
}

void lorieFlowNoteInput(int kind, int detail, int id, bool down) {
    int64_t now = lorieFrameClockNowNs();
    int r = lorieDragTrack(&drag, kind, detail, id, down);

    switch (kind) {
        case LORIE_FLOW_MOUSE_MOTION: FL_ADD(mouseMotion, 1); break;
        case LORIE_FLOW_MOUSE_BUTTON: FL_ADD(mouseButton, 1); break;
        case LORIE_FLOW_MOUSE_SCROLL: FL_ADD(mouseScroll, 1); break;
        case LORIE_FLOW_TOUCH: FL_ADD(touch, 1); break;
        case LORIE_FLOW_STYLUS: FL_ADD(stylus, 1); break;
        case LORIE_FLOW_KEY: FL_ADD(key, 1); break;
    }

    if (r & LORIE_DRAG_STARTED)
        lastDragMotionNs = 0;
    if (r & LORIE_DRAG_ENDED) {
        // A drag that ends with motion still undrawn: count how long it waited, then stop the clock.
        uint32_t waited = flTakeAge(&firstDragUndrawnNs, now);
        if (waited)
            flMax(&fl.dragNoDmgMaxUs, waited);
        lastDragMotionNs = 0;
    }
    if (!(r & LORIE_DRAG_MOTION))
        return;

    flGap(now, &lastMotionNs, &fl.motionGapMaxUs, fl.motionHist);
    flSetIfUnset(&firstMotionUnseenNs, now);

    if (r & LORIE_DRAG_DRAGGING) {
        FL_ADD(dragMotion, 1);
        flGap(now, &lastDragMotionNs, &fl.dragGapMaxUs, NULL);
        flSetIfUnset(&firstDragUndrawnNs, now);
    }
}

void lorieFlowNoteReceive(uint32_t key, volatile struct lorie_input_flow_stats *in) {
    int64_t now = lorieFrameClockNowNs();
    uint64_t head, s;
    int scanned;

    if (!in)
        return;

    // The activity logs every pointer event right before writing it, in the order it writes them, so
    // this event is normally the next entry; entries the X server never received (or that were
    // overwritten) are skipped, events the activity did not log (sent before it had the shared state)
    // stay unmatched.
    head = __atomic_load_n(&in->logSeq, __ATOMIC_ACQUIRE);
    s = rxNextSeq;
    if (head > LORIE_INPUT_LOG && s < head - LORIE_INPUT_LOG)
        s = head - LORIE_INPUT_LOG;
    for (scanned = 0; s < head && scanned < FL_MATCH_SCAN; s++, scanned++) {
        volatile __typeof__(in->log[0]) *e = &in->log[s % LORIE_INPUT_LOG];
        uint32_t entryKey;
        int64_t sentNs;

        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != s)
            continue;
        entryKey = __atomic_load_n(&e->key, __ATOMIC_RELAXED);
        sentNs = __atomic_load_n(&e->sendNs, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&e->seq, __ATOMIC_RELAXED) != s || entryKey != key)
            continue; // rewritten meanwhile, or not this event

        rxNextSeq = s + 1;
        if (sentNs > now) {
            FL_ADD(rxUnmatched, 1); // can not have been sent after it arrived
            return;
        }
        FL_ADD(rxMatched, 1);
        flMax(&fl.rxMaxUs, flUs(now - sentNs));
        FL_ADD(rxSumUs, flUs(now - sentNs));
        return;
    }
    FL_ADD(rxUnmatched, 1);
}

void lorieFlowNoteTouchQueued(void) {
    flSetIfUnset(&firstTouchQueuedNs, lorieFrameClockNowNs());
}

void lorieFlowNoteTouchHandled(void) {
    uint32_t waited = flTakeAge(&firstTouchQueuedNs, lorieFrameClockNowNs());
    if (waited)
        flMax(&fl.touchQueueMaxUs, waited);
}

void lorieFlowNoteInject(void) {
    int64_t now = lorieFrameClockNowNs();
    int64_t prev = __atomic_exchange_n(&lastInjectNs, now, __ATOMIC_ACQ_REL);
    FL_ADD(inject, 1);
    if (prev)
        flMax(&fl.injectGapMaxUs, flUs(now - prev));
    flSetIfUnset(&firstInjectUnprocessedNs, now);
}

void lorieFlowNoteProcessInput(void) {
    // The main thread delivering queued input to clients (ProcessInputEvents -> mieqProcessInputEvents).
    uint32_t waited = flTakeAge(&firstInjectUnprocessedNs, lorieFrameClockNowNs());
    if (waited) {
        FL_ADD(processRuns, 1);
        flMax(&fl.injectToProcessMaxUs, waited);
    }
}

void lorieFlowNoteCursorMove(bool wasMoved, volatile struct lorie_render_flow_stats *rf) {
    int64_t now = lorieFrameClockNowNs();
    uint32_t unseen = flTakeAge(&firstMotionUnseenNs, now);
    int64_t prev = __atomic_exchange_n(&lastCursorNs, now, __ATOMIC_ACQ_REL);

    FL_ADD(cursor, 1);
    if (prev)
        flMax(&fl.cursorGapMaxUs, flUs(now - prev));
    if (unseen)
        flMax(&fl.inToCursorMaxUs, unseen);
    if (!wasMoved && rf)
        __atomic_store_n(&rf->cursorMoveNs, now, __ATOMIC_RELEASE);
}

void lorieFlowNoteDamageSkipped(void) {
    FL_ADD(dmgSkipped, 1);
}

void lorieFlowNoteDamage(bool nonEmpty, bool wasRequested, volatile struct lorie_render_flow_stats *rf) {
    int64_t now = lorieFrameClockNowNs();
    int64_t firstDrag = __atomic_load_n(&firstDragUndrawnNs, __ATOMIC_ACQUIRE);
    uint32_t waited;

    FL_ADD(dmgChecks, 1);
    if (!nonEmpty) {
        FL_ADD(dmgOff, 1);
        if (firstDrag)
            flMax(&fl.dragNoDmgMaxUs, flUs(now - firstDrag));
        return;
    }

    FL_ADD(dmgOn, 1);
    flGap(now, &lastDmgOnNs, &fl.dmgGapMaxUs, fl.dmgHist);
    if ((waited = flTakeAge(&firstDragUndrawnNs, now)))
        flMax(&fl.inToDmgMaxUs, waited);

    if (wasRequested) {
        // The renderer has not taken the previous request yet; this damage rides on it.
        FL_ADD(reqAlready, 1);
        return;
    }
    FL_ADD(req, 1);
    flGap(now, &lastReqNs, &fl.reqGapMaxUs, fl.reqHist);
    if (rf)
        __atomic_store_n(&rf->drawRequestNs, now, __ATOMIC_RELEASE);
}

void lorieFlowNoteSignal(bool fromCursor) {
    if (fromCursor)
        FL_ADD(sigCursor, 1);
    else
        FL_ADD(sigDraw, 1);
}

void lorieFlowReport(volatile struct lorie_render_flow_stats *rf, volatile struct lorie_input_flow_stats *in,
                     int renderedFrames, bool surfaceAvailable, bool connected) {
    static bool debug = false, debugChecked = false;
    /* Everything is taken (read and reset in one step) before printing, so nothing that arrives between
     * the two is lost. */
    uint32_t mouseMotion = FL_TAKE(mouseMotion), mouseButton = FL_TAKE(mouseButton), mouseScroll = FL_TAKE(mouseScroll);
    uint32_t touch = FL_TAKE(touch), stylus = FL_TAKE(stylus), key = FL_TAKE(key), dragMotion = FL_TAKE(dragMotion);
    uint32_t motionGapMaxUs = FL_TAKE(motionGapMaxUs), dragGapMaxUs = FL_TAKE(dragGapMaxUs);
    uint32_t rxMatched = FL_TAKE(rxMatched), rxUnmatched = FL_TAKE(rxUnmatched), rxMaxUs = FL_TAKE(rxMaxUs);
    uint64_t rxSumUs = FL_TAKE(rxSumUs);
    uint32_t inject = FL_TAKE(inject), injectGapMaxUs = FL_TAKE(injectGapMaxUs);
    uint32_t cursor = FL_TAKE(cursor), cursorGapMaxUs = FL_TAKE(cursorGapMaxUs), inToCursorMaxUs = FL_TAKE(inToCursorMaxUs);
    uint32_t touchQueueMaxUs = FL_TAKE(touchQueueMaxUs), processRuns = FL_TAKE(processRuns);
    uint32_t injectToProcessMaxUs = FL_TAKE(injectToProcessMaxUs);
    uint32_t dmgChecks = FL_TAKE(dmgChecks), dmgOn = FL_TAKE(dmgOn), dmgOff = FL_TAKE(dmgOff);
    uint32_t dmgSkipped = FL_TAKE(dmgSkipped), dmgGapMaxUs = FL_TAKE(dmgGapMaxUs);
    uint32_t inToDmgMaxUs = FL_TAKE(inToDmgMaxUs), dragNoDmgMaxUs = FL_TAKE(dragNoDmgMaxUs);
    uint32_t req = FL_TAKE(req), reqAlready = FL_TAKE(reqAlready), reqGapMaxUs = FL_TAKE(reqGapMaxUs);
    uint32_t sigDraw = FL_TAKE(sigDraw), sigCursor = FL_TAKE(sigCursor);
    uint32_t motionHist[FL_HIST_BUCKETS], dmgHist[FL_HIST_BUCKETS], reqHist[FL_HIST_BUCKETS], frameHist[FL_HIST_BUCKETS];
    struct lorie_render_flow_stats r = {0};
    struct lorie_input_flow_stats a = {0};
    int i;

    for (i = 0; i < FL_HIST_BUCKETS; i++) {
        motionHist[i] = FL_TAKE(motionHist[i]);
        dmgHist[i] = FL_TAKE(dmgHist[i]);
        reqHist[i] = FL_TAKE(reqHist[i]);
        frameHist[i] = LORIE_STAT_TAKE(rf->frameHist[i]);
    }

#define RF(f) r.f = LORIE_STAT_TAKE(rf->f)
    RF(waitEnter); RF(wakeDraw); RF(wakeCursor); RF(wakeGpuCopy); RF(wakeState); RF(wakeGated); RF(wakeNone);
    RF(swWaitFrame); RF(swBuffers); RF(swNoSurface); RF(swIdle); RF(limiterWaits); RF(limiterWaitMaxUs);
    RF(framesDraw); RF(framesCursor); RF(framesOther); RF(reqToFrameMaxUs); RF(reqToFrameSamples); RF(reqToFrameSumUs);
    RF(cursorToFrameMaxUs); RF(gapLongIdle); RF(gapIdleMaxUs); RF(gapLongPending); RF(gapPendingMaxUs);
    RF(reqToFrameInvalid); RF(cursorToFrameInvalid);
#undef RF
    if (in) {
#define IN(f) a.f = LORIE_STAT_TAKE(in->f)
        IN(events); IN(moves); IN(dragMoves); IN(historySamples); IN(dragSampleGapMaxUs); IN(dragCbGapMaxUs);
        IN(evToCbMaxUs); IN(oldestToCbMaxUs); IN(clockBad); IN(sends); IN(sendsDrag); IN(sendDragGapMaxUs);
        IN(cbToSendMaxUs); IN(sendMaxUs); IN(sendFail);
#undef IN
    }

    if (!debugChecked) {
        debug = getenv("TERMUX_X11_DEBUG") != NULL;
        debugChecked = true;
    }

    if (!((surfaceAvailable && connected) || debug || mouseMotion || touch || stylus || a.events))
        return;

    log(INFO, "XlorieFlow: in_motion=%u in_drag=%u in_btn=%u in_scroll=%u in_touch=%u in_stylus=%u in_key=%u "
              "in_gap_max_us=%u in_drag_gap_max_us=%u in_hist=%u/%u/%u/%u/%u/%u inject=%u inject_gap_max_us=%u "
              "cursor=%u cursor_gap_max_us=%u in_to_cursor_max_us=%u | dmg_check=%u dmg_on=%u dmg_off=%u "
              "dmg_skip=%u dmg_gap_max_us=%u dmg_hist=%u/%u/%u/%u/%u/%u in_to_dmg_max_us=%u drag_nodmg_max_us=%u | "
              "req=%u req_already=%u req_gap_max_us=%u req_hist=%u/%u/%u/%u/%u/%u sig_draw=%u sig_cursor=%u",
        mouseMotion, dragMotion, mouseButton, mouseScroll, touch, stylus, key, motionGapMaxUs, dragGapMaxUs,
        motionHist[0], motionHist[1], motionHist[2], motionHist[3], motionHist[4], motionHist[5],
        inject, injectGapMaxUs, cursor, cursorGapMaxUs, inToCursorMaxUs, dmgChecks, dmgOn, dmgOff, dmgSkipped,
        dmgGapMaxUs, dmgHist[0], dmgHist[1], dmgHist[2], dmgHist[3], dmgHist[4], dmgHist[5], inToDmgMaxUs,
        dragNoDmgMaxUs, req, reqAlready, reqGapMaxUs, reqHist[0], reqHist[1], reqHist[2], reqHist[3], reqHist[4],
        reqHist[5], sigDraw, sigCursor);

    log(INFO, "XlorieFlowR: frames=%d frame_hist=%u/%u/%u/%u/%u/%u f_draw=%u f_cursor=%u f_other=%u req_to_frame_max_us=%u "
              "req_to_frame_avg_us=%u req_to_frame_invalid=%u cursor_to_frame_max_us=%u cursor_to_frame_invalid=%u "
              "gap_long_idle=%u gap_idle_max_us=%u gap_long_pending=%u gap_pending_max_us=%u | wait=%u wake_draw=%u "
              "wake_cursor=%u wake_gpucopy=%u wake_state=%u wake_gated=%u wake_none=%u | sw_waitframe=%u sw_buffers=%u "
              "sw_nosurface=%u sw_idle=%u limiter=%u limiter_max_us=%u",
        renderedFrames, frameHist[0], frameHist[1], frameHist[2], frameHist[3], frameHist[4], frameHist[5],
        r.framesDraw, r.framesCursor, r.framesOther, r.reqToFrameMaxUs,
        r.reqToFrameSamples ? (uint32_t) (r.reqToFrameSumUs / r.reqToFrameSamples) : 0, r.reqToFrameInvalid,
        r.cursorToFrameMaxUs, r.cursorToFrameInvalid, r.gapLongIdle, r.gapIdleMaxUs, r.gapLongPending,
        r.gapPendingMaxUs, r.waitEnter, r.wakeDraw, r.wakeCursor, r.wakeGpuCopy, r.wakeState, r.wakeGated,
        r.wakeNone, r.swWaitFrame, r.swBuffers, r.swNoSurface, r.swIdle, r.limiterWaits, r.limiterWaitMaxUs);

    log(INFO, "XlorieInput: a_events=%u a_moves=%u a_drag=%u a_hist=%u a_drag_sample_gap_max_us=%u "
              "a_drag_cb_gap_max_us=%u a_ev_to_cb_max_us=%u a_oldest_to_cb_max_us=%u a_clock_bad=%u | a_send=%u "
              "a_send_drag=%u a_send_drag_gap_max_us=%u a_cb_to_send_max_us=%u a_write_max_us=%u a_write_fail=%u | "
              "xmit=%u xmit_unmatched=%u xmit_max_us=%u xmit_avg_us=%u | x_rx_drag=%u x_rx_drag_gap_max_us=%u "
              "touch_queue_max_us=%u x_process=%u inject_to_process_max_us=%u",
        a.events, a.moves, a.dragMoves, a.historySamples, a.dragSampleGapMaxUs, a.dragCbGapMaxUs, a.evToCbMaxUs,
        a.oldestToCbMaxUs, a.clockBad, a.sends, a.sendsDrag, a.sendDragGapMaxUs, a.cbToSendMaxUs, a.sendMaxUs,
        a.sendFail, rxMatched, rxUnmatched, rxMaxUs, rxMatched ? (uint32_t) (rxSumUs / rxMatched) : 0,
        dragMotion, dragGapMaxUs, touchQueueMaxUs, processRuns, injectToProcessMaxUs);
}
