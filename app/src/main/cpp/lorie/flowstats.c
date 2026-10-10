/*
 * Input -> damage -> draw request -> renderer, counted per 5 second window (see flowstats.h). Pure
 * measurement: nothing here changes what any stage does or when.
 *
 * Threads: input notes come from the X server's input thread, injection from it or (touch) from the main
 * thread; lorieMoveCursor runs wherever the X server moves the sprite (from QueuePointerEvents on the
 * input thread for pointer motion, or the main thread); the rest is the main thread, which also prints
 * and resets. Counters are atomics; a
 * sample racing the reset at a window boundary may land in either window.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <android/log.h>
#include "flowstats.h"

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

#define FL_HIST_BUCKETS 6

static struct {
    /* input thread */
    uint32_t mouseMotion, mouseButton, mouseScroll, touch, stylus, key, dragMotion;
    uint32_t motionGapMaxUs, dragGapMaxUs;
    uint32_t motionHist[FL_HIST_BUCKETS];
    /* input thread or main thread */
    uint32_t inject, injectGapMaxUs;
    /* main thread */
    uint32_t cursor, cursorGapMaxUs, inToCursorMaxUs;
    uint32_t dmgChecks, dmgOn, dmgOff, dmgSkipped, dmgGapMaxUs;
    uint32_t dmgHist[FL_HIST_BUCKETS];
    uint32_t inToDmgMaxUs, dragNoDmgMaxUs;
    uint32_t req, reqAlready, reqGapMaxUs, sigDraw, sigCursor;
    uint32_t reqHist[FL_HIST_BUCKETS];
} fl;

/* Timestamps (CLOCK_MONOTONIC ns); 0 = none. */
static int64_t lastMotionNs, lastDragMotionNs;          /* input thread */
static int64_t lastInjectNs LORIE_ATOMIC64;             /* either thread */
static int64_t firstMotionUnseenNs LORIE_ATOMIC64;      /* input thread sets, main thread takes at lorieMoveCursor */
static int64_t firstDragUndrawnNs LORIE_ATOMIC64;       /* input thread sets, main thread takes at damage */
static int64_t lastCursorNs LORIE_ATOMIC64;             /* lorieMoveCursor: input thread (pointer motion) or main */
static int64_t lastDmgOnNs, lastReqNs;                  /* main thread */
static uint32_t buttonsDown, touchesDown;               /* input thread */

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

static void flDragEnded(int64_t now) {
    // A drag that ends with motion still undrawn: count how long it waited, then stop the clock.
    int64_t first = __atomic_exchange_n(&firstDragUndrawnNs, 0, __ATOMIC_ACQ_REL);
    if (first)
        flMax(&fl.dragNoDmgMaxUs, flUs(now - first));
    lastDragMotionNs = 0;
}

void lorieFlowNoteInput(int kind, int detail, bool down) {
    int64_t now = lorieFrameClockNowNs();
    bool motion = false, drag = false;

    switch (kind) {
        case LORIE_FLOW_MOUSE_MOTION:
            FL_ADD(mouseMotion, 1);
            motion = true;
            drag = buttonsDown != 0;
            break;
        case LORIE_FLOW_MOUSE_BUTTON: {
            uint32_t bit = detail > 0 && detail < 32 ? 1u << detail : 0;
            bool wasDragging = buttonsDown || touchesDown;
            FL_ADD(mouseButton, 1);
            buttonsDown = down ? buttonsDown | bit : buttonsDown & ~bit;
            if (wasDragging && !buttonsDown && !touchesDown)
                flDragEnded(now);
            else if (!wasDragging)
                lastDragMotionNs = 0;
            break;
        }
        case LORIE_FLOW_MOUSE_SCROLL:
            FL_ADD(mouseScroll, 1);
            break;
        case LORIE_FLOW_TOUCH: /* detail: 0 begin, 1 update, 2 end */
            FL_ADD(touch, 1);
            if (detail == 0) {
                if (!buttonsDown && !touchesDown)
                    lastDragMotionNs = 0;
                touchesDown++;
            }
            motion = drag = detail != 2;
            if (detail == 2 && touchesDown && !--touchesDown && !buttonsDown)
                flDragEnded(now);
            break;
        case LORIE_FLOW_STYLUS:
            FL_ADD(stylus, 1);
            motion = true;
            drag = down;
            break;
        case LORIE_FLOW_KEY:
            FL_ADD(key, 1);
            break;
    }

    if (!motion)
        return;

    flGap(now, &lastMotionNs, &fl.motionGapMaxUs, fl.motionHist);
    {
        int64_t expected = 0;
        __atomic_compare_exchange_n(&firstMotionUnseenNs, &expected, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }

    if (drag) {
        int64_t expected = 0;
        FL_ADD(dragMotion, 1);
        flGap(now, &lastDragMotionNs, &fl.dragGapMaxUs, NULL);
        __atomic_compare_exchange_n(&firstDragUndrawnNs, &expected, now, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
}

void lorieFlowNoteInject(void) {
    int64_t now = lorieFrameClockNowNs();
    int64_t prev = __atomic_exchange_n(&lastInjectNs, now, __ATOMIC_ACQ_REL);
    FL_ADD(inject, 1);
    if (prev)
        flMax(&fl.injectGapMaxUs, flUs(now - prev));
}

void lorieFlowNoteCursorMove(bool wasMoved, volatile struct lorie_render_flow_stats *rf) {
    int64_t now = lorieFrameClockNowNs();
    int64_t firstUnseen = __atomic_exchange_n(&firstMotionUnseenNs, 0, __ATOMIC_ACQ_REL);
    int64_t prev = __atomic_exchange_n(&lastCursorNs, now, __ATOMIC_ACQ_REL);

    FL_ADD(cursor, 1);
    if (prev)
        flMax(&fl.cursorGapMaxUs, flUs(now - prev));
    if (firstUnseen)
        flMax(&fl.inToCursorMaxUs, flUs(now - firstUnseen));
    if (!wasMoved && rf)
        __atomic_store_n(&rf->cursorMoveNs, now, __ATOMIC_RELEASE);
}

void lorieFlowNoteDamageSkipped(void) {
    FL_ADD(dmgSkipped, 1);
}

void lorieFlowNoteDamage(bool nonEmpty, bool wasRequested, volatile struct lorie_render_flow_stats *rf) {
    int64_t now = lorieFrameClockNowNs();
    int64_t firstDrag = __atomic_load_n(&firstDragUndrawnNs, __ATOMIC_ACQUIRE);

    FL_ADD(dmgChecks, 1);
    if (!nonEmpty) {
        FL_ADD(dmgOff, 1);
        if (firstDrag)
            flMax(&fl.dragNoDmgMaxUs, flUs(now - firstDrag));
        return;
    }

    FL_ADD(dmgOn, 1);
    flGap(now, &lastDmgOnNs, &fl.dmgGapMaxUs, fl.dmgHist);
    if ((firstDrag = __atomic_exchange_n(&firstDragUndrawnNs, 0, __ATOMIC_ACQ_REL)))
        flMax(&fl.inToDmgMaxUs, flUs(now - firstDrag));

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

#define RF_TAKE(field) __atomic_exchange_n(&rf->field, 0, __ATOMIC_RELAXED)

void lorieFlowReport(volatile struct lorie_render_flow_stats *rf, int renderedFrames, bool surfaceAvailable,
                     bool connected) {
    static bool debug = false, debugChecked = false;
    uint32_t mouseMotion = FL_TAKE(mouseMotion), mouseButton = FL_TAKE(mouseButton), mouseScroll = FL_TAKE(mouseScroll);
    uint32_t touch = FL_TAKE(touch), stylus = FL_TAKE(stylus), key = FL_TAKE(key), dragMotion = FL_TAKE(dragMotion);
    uint32_t motionGapMaxUs = FL_TAKE(motionGapMaxUs), dragGapMaxUs = FL_TAKE(dragGapMaxUs);
    uint32_t inject = FL_TAKE(inject), injectGapMaxUs = FL_TAKE(injectGapMaxUs);
    uint32_t cursor = FL_TAKE(cursor), cursorGapMaxUs = FL_TAKE(cursorGapMaxUs), inToCursorMaxUs = FL_TAKE(inToCursorMaxUs);
    uint32_t dmgChecks = FL_TAKE(dmgChecks), dmgOn = FL_TAKE(dmgOn), dmgOff = FL_TAKE(dmgOff);
    uint32_t dmgSkipped = FL_TAKE(dmgSkipped), dmgGapMaxUs = FL_TAKE(dmgGapMaxUs);
    uint32_t inToDmgMaxUs = FL_TAKE(inToDmgMaxUs), dragNoDmgMaxUs = FL_TAKE(dragNoDmgMaxUs);
    uint32_t req = FL_TAKE(req), reqAlready = FL_TAKE(reqAlready), reqGapMaxUs = FL_TAKE(reqGapMaxUs);
    uint32_t sigDraw = FL_TAKE(sigDraw), sigCursor = FL_TAKE(sigCursor);
    uint32_t motionHist[FL_HIST_BUCKETS], dmgHist[FL_HIST_BUCKETS], reqHist[FL_HIST_BUCKETS], frameHist[FL_HIST_BUCKETS];
    uint32_t reqToFrameSamples;
    uint64_t reqToFrameSumUs;
    int i;

    for (i = 0; i < FL_HIST_BUCKETS; i++) {
        motionHist[i] = FL_TAKE(motionHist[i]);
        dmgHist[i] = FL_TAKE(dmgHist[i]);
        reqHist[i] = FL_TAKE(reqHist[i]);
        frameHist[i] = __atomic_exchange_n(&rf->frameHist[i], 0, __ATOMIC_RELAXED);
    }

    if (!debugChecked) {
        debug = getenv("TERMUX_X11_DEBUG") != NULL;
        debugChecked = true;
    }

    if ((surfaceAvailable && connected) || debug || mouseMotion || touch || stylus) {
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

        reqToFrameSamples = rf->reqToFrameSamples;
        reqToFrameSumUs = __atomic_load_n(&rf->reqToFrameSumUs, __ATOMIC_RELAXED);
        log(INFO, "XlorieFlowR: frames=%d frame_hist=%u/%u/%u/%u/%u/%u f_draw=%u f_cursor=%u f_other=%u req_to_frame_max_us=%u "
                  "req_to_frame_avg_us=%u cursor_to_frame_max_us=%u gap_long_idle=%u gap_idle_max_us=%u "
                  "gap_long_pending=%u gap_pending_max_us=%u | wait=%u wake_draw=%u wake_cursor=%u wake_gpucopy=%u "
                  "wake_state=%u wake_gated=%u wake_none=%u | sw_waitframe=%u sw_buffers=%u sw_nosurface=%u "
                  "sw_idle=%u limiter=%u limiter_max_us=%u",
            renderedFrames, frameHist[0], frameHist[1], frameHist[2], frameHist[3], frameHist[4], frameHist[5],
            rf->framesDraw, rf->framesCursor, rf->framesOther, rf->reqToFrameMaxUs,
            reqToFrameSamples ? (uint32_t) (reqToFrameSumUs / reqToFrameSamples) : 0, rf->cursorToFrameMaxUs,
            rf->gapLongIdle, rf->gapIdleMaxUs, rf->gapLongPending, rf->gapPendingMaxUs, rf->waitEnter,
            rf->wakeDraw, rf->wakeCursor, rf->wakeGpuCopy, rf->wakeState, rf->wakeGated, rf->wakeNone,
            rf->swWaitFrame, rf->swBuffers, rf->swNoSurface, rf->swIdle, rf->limiterWaits,
            rf->limiterWaitMaxUs);
    }

    RF_TAKE(waitEnter);
    RF_TAKE(wakeDraw); RF_TAKE(wakeCursor); RF_TAKE(wakeGpuCopy); RF_TAKE(wakeState); RF_TAKE(wakeGated); RF_TAKE(wakeNone);
    RF_TAKE(swWaitFrame); RF_TAKE(swBuffers); RF_TAKE(swNoSurface); RF_TAKE(swIdle);
    RF_TAKE(limiterWaits); RF_TAKE(limiterWaitMaxUs);
    RF_TAKE(framesDraw); RF_TAKE(framesCursor); RF_TAKE(framesOther);
    RF_TAKE(reqToFrameMaxUs); RF_TAKE(reqToFrameSamples); RF_TAKE(reqToFrameSumUs); RF_TAKE(cursorToFrameMaxUs);
    RF_TAKE(gapLongIdle); RF_TAKE(gapIdleMaxUs); RF_TAKE(gapLongPending); RF_TAKE(gapPendingMaxUs);
}
