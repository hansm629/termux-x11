/*
 * The activity's half of the input path (flowstats.h): Android MotionEvent -> TouchInputHandler ->
 * InputEventSender -> LorieView native send* (activity.c) -> write() to the X server's socket. Pure
 * measurement: the socket, its protocol and the events written are untouched; the X server times the
 * socket by matching what it reads against the log written here right before each write.
 *
 * Time base. Everything here, and on the X server side, is clock_gettime(CLOCK_MONOTONIC), one clock for
 * the whole device. MotionEvent.getEventTime()/getEventTimeNanos() are in the SystemClock.uptimeMillis()
 * time base, which Android implements as systemTime(SYSTEM_TIME_MONOTONIC), i.e. CLOCK_MONOTONIC as well
 * (not elapsedRealtime/CLOCK_BOOTTIME, which counts deep sleep). That is checked as it runs: an event time
 * in the future or more than 10 s old counts as clockBad instead of a latency sample.
 *
 * The stats live in the shared server state, through a mapping of their own (activity.c maps it and
 * hands it over with lorieInputFlowAttach), so the renderer swapping and unmapping its mapping on a
 * reconnect can never pull it from under a writer.
 */

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include "flowstats.h"

#define NS_PER_MS 1000000LL
#define IF_CLOCK_FUTURE_NS (5 * NS_PER_MS)
#define IF_CLOCK_PAST_NS (10000 * NS_PER_MS)

static pthread_mutex_t ifLock = PTHREAD_MUTEX_INITIALIZER;
static void *ifMapped;
static size_t ifMappedSize;
static volatile struct lorie_input_flow_stats *ifStats;
static LorieDragTracker ifDrag;
static int64_t ifLastDragCbNs, ifLastMotionCbNs, ifLastSendDragNs;

static uint32_t ifUs(int64_t ns) {
    int64_t us = ns / 1000;
    return us < 0 ? 0 : us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
}

void lorieInputFlowAttach(void *mapping, size_t size, volatile struct lorie_input_flow_stats *stats) {
    void *old;
    size_t oldSize;

    pthread_mutex_lock(&ifLock);
    old = ifMapped;
    oldSize = ifMappedSize;
    ifMapped = mapping;
    ifMappedSize = size;
    ifStats = mapping ? stats : NULL;
    memset(&ifDrag, 0, sizeof(ifDrag));
    ifLastDragCbNs = ifLastMotionCbNs = ifLastSendDragNs = 0;
    pthread_mutex_unlock(&ifLock);

    if (old)
        munmap(old, oldSize); // no writer can still hold it: they all take ifLock
}

/* MotionEvent actions, from android.view.MotionEvent. */
enum { IF_ACTION_MOVE = 2, IF_ACTION_HOVER_MOVE = 7 };

void lorieInputFlowNoteMotion(int action, bool drag, int64_t eventTimeNs, int64_t oldestSampleNs, int historySize,
                              int64_t sampleGapNs) {
    int64_t now = lorieFrameClockNowNs();
    volatile struct lorie_input_flow_stats *in;

    pthread_mutex_lock(&ifLock);
    if (!(in = ifStats))
        goto out;

    LORIE_STAT_ADD(in->events, 1);
    ifLastMotionCbNs = now;

    if (eventTimeNs > now + IF_CLOCK_FUTURE_NS || eventTimeNs < now - IF_CLOCK_PAST_NS)
        LORIE_STAT_ADD(in->clockBad, 1);
    else {
        uint32_t evToCb = ifUs(now - eventTimeNs);
        LORIE_STAT_MAX(in->evToCbMaxUs, evToCb);
        if (historySize > 0 && oldestSampleNs <= eventTimeNs && oldestSampleNs >= now - IF_CLOCK_PAST_NS) {
            uint32_t oldestToCb = ifUs(now - oldestSampleNs);
            LORIE_STAT_MAX(in->oldestToCbMaxUs, oldestToCb);
        }
    }
    if (historySize > 0)
        LORIE_STAT_ADD(in->historySamples, (uint32_t) historySize);

    if (action == IF_ACTION_MOVE || action == IF_ACTION_HOVER_MOVE)
        LORIE_STAT_ADD(in->moves, 1);

    if (drag) {
        LORIE_STAT_ADD(in->dragMoves, 1);
        if (ifLastDragCbNs) {
            uint32_t gap = ifUs(now - ifLastDragCbNs);
            LORIE_STAT_MAX(in->dragCbGapMaxUs, gap);
        }
        ifLastDragCbNs = now;
        if (sampleGapNs > 0) {
            uint32_t gap = ifUs(sampleGapNs);
            LORIE_STAT_MAX(in->dragSampleGapMaxUs, gap);
        }
    } else
        ifLastDragCbNs = 0; // a gesture boundary: the next drag starts a new series of gaps

    out:
    pthread_mutex_unlock(&ifLock);
}

int64_t lorieInputFlowBeforeSend(int kind, int detail, int id, bool down, uint32_t key) {
    int64_t now = lorieFrameClockNowNs();
    volatile struct lorie_input_flow_stats *in;
    int r;

    pthread_mutex_lock(&ifLock);
    if (!(in = ifStats))
        goto out;

    r = lorieDragTrack(&ifDrag, kind, detail, id, down);
    LORIE_STAT_ADD(in->sends, 1);
    if (r & (LORIE_DRAG_STARTED | LORIE_DRAG_ENDED))
        ifLastSendDragNs = 0;
    if (r & LORIE_DRAG_DRAGGING) {
        LORIE_STAT_ADD(in->sendsDrag, 1);
        if (ifLastSendDragNs) {
            uint32_t gap = ifUs(now - ifLastSendDragNs);
            LORIE_STAT_MAX(in->sendDragGapMaxUs, gap);
        }
        ifLastSendDragNs = now;
    }
    if ((r & LORIE_DRAG_MOTION) && ifLastMotionCbNs) {
        uint32_t cbToSend = ifUs(now - ifLastMotionCbNs);
        LORIE_STAT_MAX(in->cbToSendMaxUs, cbToSend);
    }

    // Logged before the write, so the X server can never read the event before its entry exists. The
    // entry is invalidated first and published last (seqlock), as the X server may read it meanwhile.
    {
        uint64_t seq = __atomic_load_n(&in->logSeq, __ATOMIC_RELAXED);
        volatile __typeof__(in->log[0]) *e = &in->log[seq % LORIE_INPUT_LOG];
        __atomic_store_n(&e->seq, UINT64_MAX, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&e->sendNs, now, __ATOMIC_RELAXED);
        __atomic_store_n(&e->key, key, __ATOMIC_RELAXED);
        __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
        __atomic_store_n(&in->logSeq, seq + 1, __ATOMIC_RELEASE);
    }

    out:
    pthread_mutex_unlock(&ifLock);
    return now;
}

void lorieInputFlowAfterSend(int64_t startNs, bool ok) {
    int64_t now = lorieFrameClockNowNs();
    volatile struct lorie_input_flow_stats *in;

    pthread_mutex_lock(&ifLock);
    if ((in = ifStats)) {
        uint32_t writeUs = ifUs(now - startNs);
        LORIE_STAT_MAX(in->sendMaxUs, writeUs);
        if (!ok)
            LORIE_STAT_ADD(in->sendFail, 1);
    }
    pthread_mutex_unlock(&ifLock);
}
