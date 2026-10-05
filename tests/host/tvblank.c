/* TVBLANK: the vsync clock as a Present client sees it. Cut verbatim out of InitOutput.c by gen.py: the
 * record ring, lorieAdvanceVsyncClock, lorieUstForMsc, the Present hooks that report a time
 * (loriePresentGetUstMsc, loriePerformVblanks) and queue a vblank (loriePresentQueueVblank), and the tick
 * lorieRedraw takes. Modelled here: the display, ticking every 16.667 ms give or take 0.2 ms; the
 * Choreographer callback, which records each tick and queues one redraw - except before the screen
 * exists, or when that redraw is lost on the way; and a client presenting every frame against the msc
 * it is told.
 *
 * At every redraw, and for every vblank Present is told about: the time reported must be the time of
 * the tick the reported msc names; that tick must be the newest one that has happened, since whatever
 * Present does then is done after it; msc must count every tick that has happened; and no vblank may be
 * told about before the msc it asked for, nor twice. A redraw that took no tick must not open the
 * renderer's gate again. */
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../../app/src/main/cpp/xserver/include/list.h"
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define Success 0
#define BadAlloc 11
#ifndef __unused
#define __unused __attribute__((unused))
#endif
typedef void *RRCrtcPtr;
enum { LORIE_TRACE_TICK = 5, LORIE_TRACE_VSYNC = 19 };
#define lorieTrace(st, kind, a, b) ((void) 0)
static void lorieTraceFlush(Bool force) { (void) force; }

#define PERIOD 16667u
#define BASE 5000000u
#define MAXTICKS 2048
#define MAXEVENTS 4096
static uint64_t tickUs[MAXTICKS];      /* when display tick k happened; msc m names tick m - 1 */
static uint32_t happened;              /* display ticks so far */
static uint64_t nowUs;
static uint64_t GetTimeInMicros(void) { return nowUs; }
static uint64_t tickTime(uint32_t k) { return (uint64_t) ((int64_t) BASE + (int64_t) k * PERIOD + (int64_t) ((k * 7919u) % 401u) - 200); }

static struct { struct { uint32_t vsyncRecordsLost; } presentStats; bool waitForNextFrame; } fakeState;
static struct { typeof(fakeState) *state; uint64_t current_msc; struct xorg_list vblank_queue; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)

static void present_event_notify(uint64_t id, uint64_t ust, uint64_t msc);
#include "tvblank_src.inc"

static int fails;
enum { STALE_MSC, WRONG_TICK, OLD_TICK, BAD_PREDICTION, EARLY, TWICE, GATE_REOPENED, NEVER_TOLD, KINDS };
static const char *kindName[KINDS] = {
    "msc short of the ticks that happened", "time not that of the tick the msc names",
    "time not that of the newest tick", "next tick predicted off by over 1 ms",
    "vblank told before its msc", "vblank told twice", "gate opened by a redraw that took no tick",
    "vblank never told",
};
static uint32_t bad[KINDS];
static uint64_t worstAgeUs;
static void fault(int kind, const char *fmt, uint64_t a, uint64_t b) {
    if (!bad[kind]++) {
        printf("    first: %s: ", kindName[kind]);
        printf(fmt, (unsigned long long) a, (unsigned long long) b);
        printf("\n");
    }
}

static uint64_t targetOf[MAXEVENTS];
static bool told[MAXEVENTS], queued[MAXEVENTS];
static uint64_t nextId;
static uint64_t tellMsc[MAXEVENTS];

static void present_event_notify(uint64_t id, uint64_t ust, uint64_t msc) {
    if (told[id])
        fault(TWICE, "event %llu, msc %llu", id, msc);
    told[id] = true;
    tellMsc[id] = msc;
    if (msc < targetOf[id])
        fault(EARLY, "asked for %llu, told at %llu", targetOf[id], msc);
    if (msc != happened)
        fault(STALE_MSC, "msc %llu with %llu ticks happened", msc, happened);
    if (!msc || ust != tickUs[msc - 1])
        fault(WRONG_TICK, "msc %llu told with time %llu", msc, ust);
}

static uint64_t queue(uint64_t target) {
    uint64_t id = ++nextId;
    targetOf[id] = target;
    queued[id] = true;
    loriePresentQueueVblank(NULL, id, target);
    return id;
}

/* the vsync, and the Choreographer callback recording it */
static void displayTick(void) {
    tickUs[happened] = tickTime(happened);
    nowUs = tickUs[happened] + 100;
    lorieRecordVsync(tickUs[happened]);
    happened++;
}

/* one work proc the callback queued, run by the X server after the newest tick */
static void redraw(void) {
    uint64_t ust, msc;

    nowUs = tickUs[happened - 1] + 300;
    lorieRedrawTick();
    loriePresentGetUstMsc(NULL, &ust, &msc);
    if (msc != happened)
        fault(STALE_MSC, "msc %llu with %llu ticks happened", msc, happened);
    if (!msc || ust != tickUs[msc - 1])
        fault(WRONG_TICK, "msc %llu reported with time %llu", msc, ust);
    if (ust != tickUs[happened - 1]) {
        fault(OLD_TICK, "reported time %llu, the newest tick at %llu", ust, tickUs[happened - 1]);
        if (ust < tickUs[happened - 1] && nowUs - ust > worstAgeUs)
            worstAgeUs = nowUs - ust;
    }
    // what a flip for the next msc is told (loriePresentAfterFlip): the next tick's time
    uint64_t next = lorieUstForMsc(msc + 1), want = tickTime(happened);
    if ((next > want ? next - want : want - next) > 1000)
        fault(BAD_PREDICTION, "msc + 1 at %llu, the next tick at %llu", next, want);
}

/* a client presenting every frame: the next one as soon as the last is done, for the msc after */
static uint64_t clientEvent;
static void client(void) {
    uint64_t ust, msc;
    if (clientEvent && !told[clientEvent])
        return;
    loriePresentGetUstMsc(NULL, &ust, &msc);
    clientEvent = queue(msc + 1);
}

static void reset(void) {
    memset((void *) lorieVsyncRecords, 0, sizeof lorieVsyncRecords);
    lorieVsyncProduced = 0; lorieVsyncConsumed = 0; lorieVsyncUs = 0; lorieVsyncPeriodUs = 16667;
    fakeState.presentStats.vsyncRecordsLost = 0;
    fakeState.waitForNextFrame = true;
    fakePvfb.current_msc = 0;
    xorg_list_init(&fakePvfb.vblank_queue);
    happened = 0; nextId = 0; clientEvent = 0; worstAgeUs = 0;
    memset(told, 0, sizeof told); memset(queued, 0, sizeof queued); memset(bad, 0, sizeof bad);
}

static void finish(const char *name) {
    struct vblank *v, *tmp;
    uint32_t backlog = lorieVsyncProduced - lorieVsyncConsumed, n = 0;

    // whatever was asked for an msc that has passed must have been told
    for (uint64_t id = 1; id <= nextId; id++)
        if (queued[id] && !told[id] && targetOf[id] <= happened)
            fault(NEVER_TOLD, "event %llu for msc %llu", id, targetOf[id]);
    if (lorieVsyncPeriodUs < PERIOD - 300 || lorieVsyncPeriodUs > PERIOD + 300)
        printf("    period estimate %llu us\n", (unsigned long long) lorieVsyncPeriodUs);
    for (int k = 0; k < KINDS; k++)
        n += bad[k];
    printf("  %s: backlog after the last redraw %u, msc %llu for %u ticks, reported time up to %.1f ms old; %u faults\n",
           name, backlog, (unsigned long long) fakePvfb.current_msc, happened, worstAgeUs / 1000.0, n);
    for (int k = 0; k < KINDS; k++)
        if (bad[k])
            printf("    %u x %s\n", bad[k], kindName[k]);
    if (n || backlog || lorieVsyncPeriodUs < PERIOD - 300 || lorieVsyncPeriodUs > PERIOD + 300)
        fails++;
    xorg_list_for_each_entry_safe(v, tmp, &fakePvfb.vblank_queue, link) {
        xorg_list_del(&v->link);
        free(v);
    }
}

int main(void) {
    /* 1. four ticks before the screen existed queued no redraw; then one redraw per tick, for 600 ticks */
    reset();
    for (int i = 0; i < 4; i++)
        displayTick();
    for (int i = 0; i < 600; i++) {
        displayTick();
        redraw();
        client();
    }
    finish("four ticks with no redraw at start, then one each");

    /* 2. steady, then one redraw lost on its way (tick 100), then steady again */
    reset();
    for (int i = 0; i < 300; i++) {
        displayTick();
        if (i != 100)
            redraw();
        client();
    }
    finish("one redraw lost in 300 ticks");

    /* 3. vblanks for the next four mscs and the sixth; the X server busy for four ticks, then the four
     * redraws queued meanwhile run back to back. The renderer takes the gate the first one opens. */
    reset();
    for (int i = 0; i < 50; i++) {
        displayTick();
        redraw();
    }
    uint64_t m = fakePvfb.current_msc, ev[5];
    for (int k = 0; k < 4; k++)
        ev[k] = queue(m + 1 + k);
    ev[4] = queue(m + 6);
    for (int k = 0; k < 4; k++)
        displayTick();
    for (int k = 0; k < 4; k++) {
        fakeState.waitForNextFrame = true;      /* the renderer drew a frame and shut the gate */
        redraw();
        if (k && !fakeState.waitForNextFrame)
            fault(GATE_REOPENED, "back-to-back redraw %llu reopened it at msc %llu", (uint64_t) k, fakePvfb.current_msc);
    }
    for (int k = 0; k < 4; k++)
        if (!told[ev[k]])
            fault(NEVER_TOLD, "event for msc %llu after the busy spell (msc %llu)", m + 1 + k, fakePvfb.current_msc);
    if (told[ev[4]])
        fault(EARLY, "asked for %llu, told at %llu", m + 6, tellMsc[ev[4]]);
    displayTick(); redraw();
    if (told[ev[4]] && tellMsc[ev[4]] < m + 6)
        fault(EARLY, "asked for %llu, told at %llu", m + 6, tellMsc[ev[4]]);
    displayTick(); redraw();
    if (!told[ev[4]] || tellMsc[ev[4]] != m + 6)
        fault(NEVER_TOLD, "event for msc %llu, told at %llu", m + 6, told[ev[4]] ? tellMsc[ev[4]] : 0);
    finish("busy for four ticks, four redraws back to back");

    printf("TVBLANK vsync clock against Present: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
