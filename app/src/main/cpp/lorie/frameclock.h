#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 64 bit fields used with __atomic builtins: 32 bit x86 aligns int64_t to 4 only, which turns them into
 * locked library calls - not atomic across the two processes sharing the struct. */
#define LORIE_ATOMIC64 __attribute__((aligned(8)))

/*
 * The frame clock (frameclock.c): Android VSYNC -> AChoreographer callback -> QueueWorkProc(lorieRedraw)
 * -> X server main thread -> lorieRedraw (MSC, Present vblanks, waitForNextFrame) -> renderer.
 *
 * Renderer-side half of its counters. They live in the shared server state because the renderer runs in
 * the activity's process, whose logcat cannot be read without root or adb, while the 5 second report runs
 * in the X server's. Fields marked X are written by the X server, the rest by the renderer; the X server
 * resets the per-window ones after printing them (a lost update at the boundary is not worth a lock).
 */
struct lorie_frame_clock_stats {
    volatile uint64_t tickSerial LORIE_ATOMIC64;     /* X: lorieRedraw runs, i.e. frame ticks handed to the renderer */
    volatile int64_t lastTickNs LORIE_ATOMIC64;      /* X: CLOCK_MONOTONIC time of the last one */
    volatile uint64_t drawTickSerial LORIE_ATOMIC64; /* X: tickSerial of the last tick that asked the renderer to draw */

    volatile uint32_t waitSet;           /* frames that set waitForNextFrame */
    volatile uint32_t waitSetOverTick;   /* ... although a tick had cleared it again while the frame ran */
    volatile uint32_t handoverFrames;    /* frames that started for a tick's draw request */
    volatile uint32_t handoverLate;      /* ... one or more ticks after that tick */
    volatile uint32_t handoverLateMaxTicks;
    volatile uint32_t handoverMaxUs;     /* tick -> frame start, for frames that started on their tick */
    volatile uint32_t lockHeldMaxUs;     /* longest hold of state->lock by a frame */
    volatile uint32_t bufferWaitMaxUs;   /* longest wait for a GPU-copy buffer to arrive over the socket */
    volatile uint32_t surfaceGeneration; /* window surfaces applied or dropped, never reset */
};

int64_t lorieFrameClockNowNs(void);

/* Owner thread (the one CmdEntryPoint.start() runs on, which has a Looper): starts the callback chain. */
void lorieFrameClockStart(void);
/* Any thread: the activity is back (new surface, new connection); have the owner thread check the chain. */
void lorieFrameClockResumeCheck(void);

/* X server main thread. RedrawBegin returns the frame ticks to advance MSC by, 0 when there are none. */
void lorieFrameClockSetXThread(void);
void lorieFrameClockResetQueue(void);
uint32_t lorieFrameClockRedrawBegin(void);
void lorieFrameClockNoteClear(bool wasWaiting);
void lorieFrameClockXWakeup(void);
void lorieFrameClockXBlock(void);
void lorieFrameClockNoteLockWait(int64_t ns);
void lorieFrameClockReport(volatile struct lorie_frame_clock_stats *rs, int renderedFrames,
                           bool surfaceAvailable, bool connected, uint32_t appHz);

/* Provided by InitOutput.c: whether the screen exists, whether the renderer shows it on a surface, and
 * queueing one lorieRedraw on the X server. */
bool lorieScreenReady(void);
bool lorieSurfaceShown(void);
void lorieQueueRedraw(void);
