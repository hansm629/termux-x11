#pragma once

#include <stdbool.h>
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
    /* Frame-to-frame gaps >= LORIE_LONG_FRAME_US, split by whether a request was pending for that long. */
    volatile uint32_t gapLongIdle, gapIdleMaxUs;
    volatile uint32_t gapLongPending, gapPendingMaxUs;
};

/* X server input thread (handleLorieEvents). */
enum { LORIE_FLOW_MOUSE_MOTION, LORIE_FLOW_MOUSE_BUTTON, LORIE_FLOW_MOUSE_SCROLL, LORIE_FLOW_TOUCH,
       LORIE_FLOW_STYLUS, LORIE_FLOW_KEY };
void lorieFlowNoteInput(int kind, int detail, bool down);
/* Input thread or X server main thread: an event was handed to the X server's input queue. */
void lorieFlowNoteInject(void);

/* Wherever the X server moves the sprite (lorieMoveCursor). */
void lorieFlowNoteCursorMove(bool wasMoved, volatile struct lorie_render_flow_stats *rf);

/* X server main thread. */
void lorieFlowNoteDamageSkipped(void);
void lorieFlowNoteDamage(bool nonEmpty, bool wasRequested, volatile struct lorie_render_flow_stats *rf);
void lorieFlowNoteSignal(bool fromCursor);
void lorieFlowReport(volatile struct lorie_render_flow_stats *rf, int renderedFrames, bool surfaceAvailable,
                     bool connected);
