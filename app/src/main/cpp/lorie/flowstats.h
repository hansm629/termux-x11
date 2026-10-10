#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "frameclock.h"

/*
 * The flow from what the user does to what the renderer draws, measured stage by stage in the same 5
 * second windows as the frame clock (flowstats.c prints them as XlorieFlow / XlorieFlowR):
 *
 *   Android input (activity) -> socket -> X server input thread: handleLorieEvents        (in_*)
 *     -> injected into the X server (QueuePointerEvents / QueueTouchEvents)               (inject)
 *     -> X server main thread processes it, the pointer moves: lorieMoveCursor            (cursor)
 *     -> X clients draw, root is damaged; each lorieRedraw tick looks at the damage       (dmg_*)
 *     -> drawRequested goes false -> true, rendererCond is signalled                      (req, sig_*)
 *     -> renderer wakes (why), stops waiting (or why not), draws a frame (for what)       (XlorieFlowR)
 *
 * A renderer frame gap alone cannot tell "nothing new was drawn" from "something was, and the renderer
 * was late": gap_long_idle / gap_long_pending (renderer) and drag_nodmg_max_us (X server) do.
 *
 * Renderer half of the counters, in the shared server state: fields marked X are written by the X
 * server, the rest by the renderer; the X server resets the per-window ones after printing them.
 */
struct lorie_render_flow_stats {
    volatile int64_t drawRequestNs LORIE_ATOMIC64;   /* X: when lorieRedraw last set drawRequested from false */
    volatile int64_t cursorMoveNs LORIE_ATOMIC64;    /* X: when lorieMoveCursor last set cursor.moved from false */

    volatile uint32_t waitEnter;            /* pthread_cond_wait calls of the render loop */
    volatile uint32_t wakeDraw, wakeCursor, wakeGpuCopy, wakeState, wakeGated, wakeNone; /* ... what was pending on return */
    volatile uint32_t swWaitFrame, swBuffers, swNoSurface, swIdle;  /* rendererShouldWait() true, first reason */
    volatile uint32_t limiterWaits, limiterWaitMaxUs;  /* deliberate timed waits (coalesce / high-refresh limit) */

    volatile uint32_t framesDraw, framesCursor, framesOther;  /* frames by what asked for them */
    volatile uint32_t reqToFrameMaxUs, reqToFrameSamples;     /* drawRequestNs -> frame start */
    volatile uint64_t reqToFrameSumUs LORIE_ATOMIC64;
    volatile uint32_t cursorToFrameMaxUs;                     /* cursorMoveNs -> frame start */
    volatile uint32_t frameHist[6];      /* frame-to-frame gaps: <4 / <12 / <25 / <50 / <100 / >=100 ms */
    volatile uint32_t reqToFrameInvalid, cursorToFrameInvalid; /* stamps newer than the frame start: not sampled */
    /* Frame-to-frame gaps >= LORIE_LONG_FRAME_US, split by whether a request was pending for that long. */
    volatile uint32_t gapLongIdle, gapIdleMaxUs;
    volatile uint32_t gapLongPending, gapPendingMaxUs;
};

/*
 * The activity's half of the input path, Android MotionEvent -> socket write (inputflow.c), and the send
 * log the X server matches received events against to time the socket. Written by the activity (under its
 * own lock, through its own mapping of the shared state), read and reset by the X server, except the log
 * itself, which is never reset. All times are CLOCK_MONOTONIC: the time base of MotionEvent event times
 * (SystemClock.uptimeMillis) and of the X server's clock (see inputflow.c).
 */
#define LORIE_INPUT_LOG 256
struct lorie_input_flow_stats {
    volatile uint32_t events, moves, dragMoves, historySamples;  /* MotionEvents handed to the activity */
    volatile uint32_t dragSampleGapMaxUs;   /* between the samples (historical included) of consecutive drag moves */
    volatile uint32_t dragCbGapMaxUs;       /* between the callbacks that delivered them */
    volatile uint32_t evToCbMaxUs, oldestToCbMaxUs; /* event time / oldest sample -> callback */
    volatile uint32_t clockBad;             /* event times in the future or > 10 s old: time bases differ */
    volatile uint32_t sends, sendsDrag, sendDragGapMaxUs; /* pointer events written to the socket */
    volatile uint32_t cbToSendMaxUs, sendMaxUs, sendFail; /* last callback -> motion write, write() time, short writes */

    volatile uint64_t logSeq LORIE_ATOMIC64; /* entries written to log[] so far, never reset */
    struct {
        volatile uint64_t seq LORIE_ATOMIC64;
        volatile int64_t sendNs LORIE_ATOMIC64;
        volatile uint32_t key;
    } log[LORIE_INPUT_LOG];
};

/*
 * Counters in the shared state are written by one process and reset (exchanged to 0) by the other: a
 * plain ++ could write a whole window's count back after the reset, so they are only ever added to
 * atomically. A maximum is only ever raised by its one writer with a fresh sample, which at worst lands in
 * the window next to the one it belongs to.
 */
#define LORIE_STAT_ADD(field, n) __atomic_fetch_add(&(field), (n), __ATOMIC_RELAXED)
#define LORIE_STAT_TAKE(field) __atomic_exchange_n(&(field), 0, __ATOMIC_RELAXED)
#define LORIE_STAT_MAX(field, v) do { if ((v) > (field)) (field) = (v); } while (0)

/*
 * Whether pointer input is a drag, identically on both ends of the socket: motion while a mouse button or
 * a touch (by id: the activity sends TouchEnd for every id that is not down with each move) is down.
 */
typedef struct { uint32_t buttons, touches; } LorieDragTracker;
enum { LORIE_FLOW_MOUSE_MOTION, LORIE_FLOW_MOUSE_BUTTON, LORIE_FLOW_MOUSE_SCROLL, LORIE_FLOW_TOUCH,
       LORIE_FLOW_STYLUS, LORIE_FLOW_KEY };
enum { LORIE_DRAG_NONE = 0, LORIE_DRAG_MOTION = 1, LORIE_DRAG_DRAGGING = 2, LORIE_DRAG_STARTED = 4, LORIE_DRAG_ENDED = 8 };
/* detail: mouse button number; touch 0 begin / 1 update / 2 end. id: touch id. */
static inline int lorieDragTrack(LorieDragTracker *t, int kind, int detail, int id, bool down) {
    bool was = t->buttons || t->touches;
    uint32_t bit;
    int r = 0;
    switch (kind) {
        case LORIE_FLOW_MOUSE_MOTION:
            r = LORIE_DRAG_MOTION | (was ? LORIE_DRAG_DRAGGING : 0);
            break;
        case LORIE_FLOW_MOUSE_BUTTON:
            bit = detail > 0 && detail < 32 ? 1u << detail : 0;
            t->buttons = down ? t->buttons | bit : t->buttons & ~bit;
            break;
        case LORIE_FLOW_TOUCH:
            // An update without a begin counts as one, as handleTouchEvent turns it into one; an end for
            // an id that is not down changes nothing.
            bit = id >= 0 && id < 32 ? 1u << id : 0;
            if (detail == 2)
                t->touches &= ~bit;
            else {
                t->touches |= bit;
                r = LORIE_DRAG_MOTION | LORIE_DRAG_DRAGGING;
            }
            break;
        case LORIE_FLOW_STYLUS:
            r = LORIE_DRAG_MOTION | (down ? LORIE_DRAG_DRAGGING : 0);
            break;
    }
    if (!was && (t->buttons || t->touches))
        r |= LORIE_DRAG_STARTED;
    if (was && !t->buttons && !t->touches)
        r |= LORIE_DRAG_ENDED;
    return r;
}

/* X server input thread (handleLorieEvents). */
void lorieFlowNoteInput(int kind, int detail, int id, bool down);
/* ... for every pointer event read, before it is handled: times the socket against the activity's log. */
void lorieFlowNoteReceive(uint32_t key, volatile struct lorie_input_flow_stats *in);
/* ... a touch was queued for the main thread (handleTouchEvent). */
void lorieFlowNoteTouchQueued(void);
/* Input thread or X server main thread: an event was handed to the X server's input queue. */
void lorieFlowNoteInject(void);

/* Wherever the X server moves the sprite (lorieMoveCursor). */
void lorieFlowNoteCursorMove(bool wasMoved, volatile struct lorie_render_flow_stats *rf);

/* X server main thread. */
void lorieFlowNoteTouchHandled(void);
void lorieFlowNoteProcessInput(void);
void lorieFlowNoteDamageSkipped(void);
void lorieFlowNoteDamage(bool nonEmpty, bool wasRequested, volatile struct lorie_render_flow_stats *rf);
void lorieFlowNoteSignal(bool fromCursor);
void lorieFlowReport(volatile struct lorie_render_flow_stats *rf, volatile struct lorie_input_flow_stats *in,
                     int renderedFrames, bool surfaceAvailable, bool connected);

/* Activity (inputflow.c), any thread. Attach takes over `mapping` (munmapped on the next attach). */
void lorieInputFlowAttach(void *mapping, size_t size, volatile struct lorie_input_flow_stats *stats);
void lorieInputFlowNoteMotion(int action, bool drag, int64_t eventTimeNs, int64_t oldestSampleNs, int historySize,
                              int64_t sampleGapNs);
int64_t lorieInputFlowBeforeSend(int kind, int detail, int id, bool down, uint32_t key);
void lorieInputFlowAfterSend(int64_t startNs, bool ok);
