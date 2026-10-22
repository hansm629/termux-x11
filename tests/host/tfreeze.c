/* TFREEZE: the freeze watchdog (P11-C3-C, TERMUX_X11_FRAME_FREEZE_DIAG=1), from the real code (InitOutput.c, gen.py
 * "freeze"): its look at the shared state (lorieFreezeSample), its judgement (lorieFreezeCheck), its look at a thread
 * in /proc (lorieThreadInfo) and its lines (lorieFreezeLine, lorieXSchedLine) - on looks made 100 ms apart, as its
 * thread makes them, of a world a case sets up.
 *
 *   Idle for a minute (nothing asked for; ticks run, or none at all): nothing. A long gap with nothing asked for - a
 *   client drawing nothing, a scene change: nothing. Frames presented while asks keep coming: nothing. A frame asked
 *   for and not presented: a freeze from 500 ms on - not at 400 - on the renderer's side, where it was seen (asleep),
 *   with its 1, 2 and 3 second levels each once, and its end when the frame is presented, the same freeze (its id),
 *   about as long as it lasted. GPU copies not completed while frames still come: a freeze, of the copies. The renderer
 *   in one stage (a fence) 700 ms: a freeze. The X server's thread in one request 1.2 s: a freeze on its side; waiting
 *   for a root copy the renderer does not make, ticks piling up: a freeze of both. No window: what the renderer has not
 *   done is no freeze, the X server's thread still watched; no connection: its root copy no freeze. A second freeze:
 *   the next id. The sample reads the asks' first waiting one, the oldest copy, the stages. The lines: every key, the
 *   end's durations and thread times, the widest within logcat's 1023 bytes; /proc read for this very thread.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
typedef int Bool;
#define TRUE 1
#define FALSE 0

#include "diagtypes_src.inc"
#include "gpucopytypes_src.inc"
/* the shared state, as far as the watchdog reads it (lorie.h's fields) */
struct lorie_shared_server_state {
    int lock, lockingPid;
    struct {
        volatile uint32_t writeIndex, readIndex;
        volatile uint64_t completedSerial;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;
    struct { volatile uint32_t busy; } gpuInFlight;
    struct {
        volatile uint32_t seq, taken, cancelled, failed, done, waiting;
        uint64_t queuedUs;
    } rootCopy;
    volatile uint8_t surfaceAvailable;
    struct lorie_renderer_diag diag;
    struct lorie_backend_control backend;
    struct lorie_progress progress;
    struct { int lock; } cursor;
};
static struct lorie_shared_server_state st;
/* the X server's own, as far as the watchdog reads it (InitOutput.c lorieScreenInfo) */
#include "xstage_decl_src.inc"
static struct {
    struct lorie_shared_server_state *state;
    Bool freezeDiag;
    uint32_t xStage, xRequests;
    uint64_t xStageUs;
    int xRequestClient, xRequestMajor, xRequestMinor;
    uint64_t reportUs, reportMaxUs;
    uint64_t copyNotifiedSerial, copyNotifiedUs, copyAckedSerial, copyAckedUs;
    pid_t inputTid;
} fakePvfb = { &st, TRUE }, *pvfb = &fakePvfb;
/* the input thread: this build's (dix-config.h.in) and this server's (-dumbSched: off) */
#define INPUTTHREAD 1
static Bool InputThreadEnable = TRUE;
static bool connected = true;
static bool lorieConnectionAlive(void) { return connected; }
/* the clock the sample reads: the case's */
static uint64_t now = 1000000000;
#define lorieMonotonicUs() (now)
#include "freeze_src.inc"
#undef lorieMonotonicUs

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- a world, looked at every 100 ms ---- */
static struct lorie_freeze f;
static struct { int begins, levels, ends, levelSeen[4]; uint32_t beginId, endId; uint64_t beginAt, endAt, duration;
                char beginLine[1024], endLine[1024]; } ev;
static struct lorie_thread_info th[LORIE_THREADS], begin[LORIE_THREADS];

static void reset(void) {
    memset(&st, 0, sizeof(st));
    memset(&f, 0, sizeof(f));
    memset(&ev, 0, sizeof(ev));
    fakePvfb.xStage = LORIE_X_LOOP;
    fakePvfb.xStageUs = now;
    st.surfaceAvailable = 1;
    st.progress.rStage = LORIE_R_SLEEP;
    connected = true;
    lorieFreezeCheck(&f, &(struct lorie_freeze_sample) { .nowUs = now, .xStage = LORIE_X_LOOP, .xStageUs = now });  /* its first look */
}
/* the watchdog started on the world as a case set it up: its first look, now */
static void watchFrom(void) {
    struct lorie_freeze_sample s;
    memset(&f, 0, sizeof(f));
    lorieFreezeSample(&s);
    lorieFreezeCheck(&f, &s);
}

/* one look, 100 ms on: the X server ticks (`ticks`: posted and run, or posted only), as the case says */
enum { TICKS_NONE, TICKS_RUN, TICKS_STUCK };
static void look(int ticks) {
    struct lorie_freeze_sample s;
    char line[1024];
    int what;
    now += LORIE_FREEZE_PERIOD_US;
    if (ticks != TICKS_NONE)
        st.diag.xTicksPosted += 12;
    if (ticks == TICKS_RUN)
        st.diag.xTicksRun = st.diag.xTicksPosted;
    lorieFreezeSample(&s);
    what = lorieFreezeCheck(&f, &s);
    if (what == LORIE_FREEZE_NONE)
        return;
    lorieFreezeLine(line, sizeof(line), &f, &s, what, th, begin);
    if (what == LORIE_FREEZE_BEGIN) {
        ev.begins++, ev.beginId = f.id, ev.beginAt = now;
        snprintf(ev.beginLine, sizeof(ev.beginLine), "%s", line);
    } else if (what == LORIE_FREEZE_LEVEL) {
        ev.levels++;
        if (f.level <= 3)
            ev.levelSeen[f.level]++;
    } else {
        ev.ends++, ev.endId = f.id, ev.endAt = now, ev.duration = s.nowUs - f.beginUs;
        snprintf(ev.endLine, sizeof(ev.endLine), "%s", line);
    }
}
static void looks(int n, int ticks) {
    while (n-- > 0)
        look(ticks);
}

/* the X server's tick asking for a frame, as lorieTickAsked numbers it; the renderer presenting one that took them up */
static void ask(void) {
    uint32_t seq = st.progress.askSeq + 1;
    st.progress.askUs[seq % LORIE_ASK_RING] = now;
    st.progress.askSeq = seq;
}
static void present(void) {
    st.progress.takenSeq = st.progress.askSeq;
    st.diag.vkFramesPresented++;
}

int main(void) {
    int i;

    /* the threads as a case sees them: this one in /proc, the rest made up */
    lorieThreadInfo((pid_t) syscall(SYS_gettid), &th[LORIE_THREAD_MAIN]);
    CHECK(th[LORIE_THREAD_MAIN].state >= 'A' && th[LORIE_THREAD_MAIN].state <= 'Z', "/proc: this thread's state '%c'",
          th[LORIE_THREAD_MAIN].state);
    if (th[LORIE_THREAD_MAIN].runUs < 0)
        printf("  (no schedstat here: run and wait times -1)\n");
    lorieThreadInfo(0, &th[LORIE_THREAD_INPUT]);
    CHECK(th[LORIE_THREAD_INPUT].state == '?' && th[LORIE_THREAD_INPUT].runUs == -1 && th[LORIE_THREAD_INPUT].sys == -2,
          "/proc: no thread, not unknown");
    for (i = 0; i < LORIE_THREADS; i++)
        th[i] = begin[i] = (struct lorie_thread_info) { 'S', 1000, 500, SYS_futex, (unsigned long long) (uintptr_t) &st.lock, FALSE };
    th[LORIE_THREAD_MAIN].runUs = 1700, th[LORIE_THREAD_MAIN].waitUs = 2500;

    /* idle a minute, ticks run: nothing; no ticks at all (the screen off): nothing */
    reset();
    looks(600, TICKS_RUN);
    looks(600, TICKS_NONE);
    CHECK(ev.begins == 0 && ev.ends == 0, "idle: %d freezes", ev.begins);
    /* nothing asked for 5 s while ticks run (a client drawing nothing, a scene change): nothing */
    reset();
    ask(), present();
    looks(50, TICKS_RUN);
    CHECK(ev.begins == 0, "a gap with nothing asked for: %d freezes", ev.begins);
    /* frames presented every 100 ms while asks keep coming: nothing */
    reset();
    for (i = 0; i < 100; i++) {
        ask();
        ask();
        st.progress.takenSeq = st.progress.askSeq - 1;     /* the last ask not taken yet */
        st.diag.vkFramesPresented++;
        look(TICKS_RUN);
    }
    CHECK(ev.begins == 0, "frames coming, asks pending: %d freezes", ev.begins);

    /* a frame asked for, not presented: 400 ms - nothing; then a freeze from 500 ms, its levels, its end */
    reset();
    looks(3, TICKS_RUN);
    ask();
    looks(4, TICKS_RUN);
    CHECK(ev.begins == 0, "a frame waiting 400 ms: a freeze");
    present();
    looks(3, TICKS_RUN);
    CHECK(ev.begins == 0 && ev.ends == 0, "a frame presented after 400 ms: an event");
    ask();
    looks(5, TICKS_RUN);
    CHECK(ev.begins == 1 && ev.beginId == 1 && f.side == 'r' && (f.pending & LORIE_FREEZE_FRAME) && f.stallUs >= 500000 &&
          f.stallUs < 600000, "a frame waiting 500 ms: %d begins, side %c, stall %llu us", ev.begins, f.side,
          (unsigned long long) f.stallUs);
    CHECK(strstr(ev.beginLine, "XlorieFreeze: phase=begin id=1 side=renderer stall_us=500000 ") &&
          strstr(ev.beginLine, " pending=frame stage=renderer:sleep x_stage=loop ") && strstr(ev.beginLine, " asks=") &&
          strstr(ev.beginLine, " x_main=S,futex:lock "), "begin line: %s", ev.beginLine);
    looks(31, TICKS_RUN);
    CHECK(ev.levels == 3 && ev.levelSeen[1] == 1 && ev.levelSeen[2] == 1 && ev.levelSeen[3] == 1 && ev.ends == 0,
          "levels: %d (1s %d, 2s %d, 3s %d), %d ends", ev.levels, ev.levelSeen[1], ev.levelSeen[2], ev.levelSeen[3], ev.ends);
    looks(5, TICKS_RUN);
    CHECK(ev.levels == 3, "levels logged again: %d", ev.levels);
    present();
    look(TICKS_RUN);
    CHECK(ev.ends == 1 && ev.endId == ev.beginId && ev.duration >= 4000000 && ev.duration <= 4200000,
          "end: %d, id %u (begin %u), %llu us", ev.ends, ev.endId, ev.beginId, (unsigned long long) ev.duration);
    CHECK(strstr(ev.endLine, "XlorieFreeze: phase=end id=1 duration_us=") && strstr(ev.endLine, " side=renderer stalled_stage=renderer:sleep ") &&
          strstr(ev.endLine, " pending_at_begin=frame ended_by=frame levels=3 x_main_run_us=700 x_main_runq_us=2000 "),
          "end line: %s", ev.endLine);
    looks(20, TICKS_RUN);
    CHECK(ev.begins == 1 && ev.ends == 1, "after the end: %d begins, %d ends", ev.begins, ev.ends);
    /* a second one: the next id */
    ask();
    looks(6, TICKS_RUN);
    present();
    look(TICKS_RUN);
    CHECK(ev.begins == 2 && ev.beginId == 2 && ev.endId == 2, "a second freeze: ids %u, %u", ev.beginId, ev.endId);

    /* GPU copies not completed while frames still come: a freeze of the copies */
    reset();
    st.gpuCopyQueue.entries[0].queuedUs = now;
    st.gpuCopyQueue.writeIndex = 1;
    for (i = 0; i < 7; i++) {
        st.diag.vkFramesPresented++;
        look(TICKS_RUN);
    }
    CHECK(ev.begins == 1 && f.pending == LORIE_FREEZE_COPY && f.side == 'r', "copies stuck, frames coming: %d begins, pending %u",
          ev.begins, f.pending);
    st.gpuCopyQueue.completedSerial = 1, st.gpuCopyQueue.readIndex = 1;
    look(TICKS_RUN);
    CHECK(ev.ends == 1 && strstr(ev.endLine, " ended_by=copy "), "copies stuck: end %d: %s", ev.ends, ev.endLine);

    /* the renderer in its fence wait 700 ms, nothing asked: a freeze where it stands */
    reset();
    st.progress.rStage = LORIE_R_FENCE_WAIT;
    st.progress.rStageUs = now;
    looks(7, TICKS_RUN);
    CHECK(ev.begins == 1 && f.pending == LORIE_FREEZE_RSTAGE && strstr(ev.beginLine, " stage=renderer:fence_wait "),
          "renderer in its fence 700 ms: %d begins: %s", ev.begins, ev.beginLine);

    /* the X server's thread in one request 1.2 s: its side; the ticks it does not run with it */
    reset();
    fakePvfb.xStage = LORIE_X_REQUEST;
    fakePvfb.xStageUs = now;
    fakePvfb.xRequestMajor = 139, fakePvfb.xRequestMinor = 7, fakePvfb.xRequestClient = 3;
    looks(12, TICKS_STUCK);
    CHECK(ev.begins == 1 && f.side == 'x' && (f.pending & LORIE_FREEZE_XSTAGE) && (f.pending & LORIE_FREEZE_TICKS) &&
          ev.levelSeen[1] == 1 && strstr(ev.beginLine, " stage=x:request x_stage=request ") && strstr(ev.beginLine, " x_request=139.7@3 "),
          "X in a request 1.2 s: %d begins, side %c, pending %u, levels %d: %s", ev.begins, f.side, f.pending, ev.levels, ev.beginLine);
    fakePvfb.xStage = LORIE_X_LOOP;
    fakePvfb.xStageUs = now;
    look(TICKS_RUN);
    CHECK(ev.ends == 1 && strstr(ev.endLine, " side=x stalled_stage=x:request ") && strstr(ev.endLine, " ended_by=x,ticks "),
          "X in a request: end %d: %s", ev.ends, ev.endLine);

    /* the X server waiting for a root copy the renderer does not make, its ticks piling up: both sides */
    reset();
    st.rootCopy.seq = 5, st.rootCopy.taken = st.rootCopy.done = 4, st.rootCopy.waiting = 5, st.rootCopy.queuedUs = now;
    fakePvfb.xStage = LORIE_X_ROOT_COPY_WAIT;
    fakePvfb.xStageUs = now;
    th[LORIE_THREAD_MAIN].arg = (unsigned long long) (uintptr_t) &st.rootCopy.done;
    watchFrom();
    looks(6, TICKS_STUCK);
    CHECK(ev.begins == 1 && f.side == 'b' && strstr(ev.beginLine, " side=both ") &&
          strstr(ev.beginLine, " stage=x:root_copy_wait+renderer:sleep ") && strstr(ev.beginLine, " x_main=S,futex:root_done ") &&
          strstr(ev.beginLine, " root=5/4/4/0 ") && strstr(ev.beginLine, " x_waiting_root=5 "),
          "X waiting for a root copy: %d begins, side %c: %s", ev.begins, f.side, ev.beginLine);
    st.rootCopy.done = 5, st.rootCopy.waiting = 0;
    fakePvfb.xStage = LORIE_X_LOOP;
    look(TICKS_RUN);
    CHECK(ev.ends == 1 && strstr(ev.endLine, " ended_by=root,x,ticks "), "root copy made: end %d: %s", ev.ends, ev.endLine);
    th[LORIE_THREAD_MAIN].arg = (unsigned long long) (uintptr_t) &st.lock;

    /* no window: the renderer's frames are no freeze; the X server's thread still watched */
    reset();
    st.surfaceAvailable = 0;
    ask();
    looks(30, TICKS_RUN);
    CHECK(ev.begins == 0, "no window, a frame asked for: %d freezes", ev.begins);
    fakePvfb.xStage = LORIE_X_TICK;
    fakePvfb.xStageUs = now;
    looks(6, TICKS_STUCK);
    CHECK(ev.begins == 1 && f.side == 'x' && strstr(ev.beginLine, " surface=0 "), "no window, X in a tick 600 ms: %d begins, side %c",
          ev.begins, f.side);
    /* no connection: a root copy asked for is no freeze of the renderer */
    reset();
    connected = false;
    st.rootCopy.seq = 9, st.rootCopy.queuedUs = now;
    looks(10, TICKS_RUN);
    CHECK(ev.begins == 0, "no connection, a root copy asked for: %d freezes", ev.begins);

    /* the sample: the first ask still waiting (not the last), the oldest copy, out of the ring */
    reset();
    for (i = 0; i < 40; i++) {
        ask();
        now += 1000;
    }
    st.progress.takenSeq = 5;
    st.gpuCopyQueue.entries[2].queuedUs = 777, st.gpuCopyQueue.entries[3].queuedUs = 888;
    st.gpuCopyQueue.readIndex = 2, st.gpuCopyQueue.writeIndex = 4;
    {
        struct lorie_freeze_sample s;
        lorieFreezeSample(&s);
        CHECK(s.askSeq == 40 && s.takenSeq == 5 && s.askUs == st.progress.askUs[(40 - 31) % LORIE_ASK_RING] && s.copiesQueued == 2 &&
              s.copyQueuedUs == 777, "sample: asks %u/%u at %llu (want the oldest kept), copies %u from %llu", s.askSeq, s.takenSeq,
              (unsigned long long) s.askUs, s.copiesQueued, (unsigned long long) s.copyQueuedUs);
        st.progress.takenSeq = 38;
        lorieFreezeSample(&s);
        CHECK(s.askUs == st.progress.askUs[39 % LORIE_ASK_RING], "sample: ask 39 not the first waiting");
    }

    /* the widest lines within 1023 bytes */
    {
        struct lorie_freeze w;
        struct lorie_freeze_sample s;
        struct lorie_thread_info t[LORIE_THREADS], b[LORIE_THREADS];
        char line[2048];
        int n, max = 0;
        memset(&w, 0xff, sizeof(w));
        memset(&s, 0xff, sizeof(s));
        w.side = 'b', w.level = 3, w.beginUs = 0, s.nowUs = UINT64_MAX, s.xStage = LORIE_X_ROOT_COPY_WAIT, s.rStage = LORIE_R_DEVICE_IDLE;
        w.beginXStage = LORIE_X_ROOT_COPY_WAIT, w.beginRStage = LORIE_R_DEVICE_IDLE;
        for (i = 0; i < LORIE_THREADS; i++) {
            t[i] = (struct lorie_thread_info) { 'D', LLONG_MAX, LLONG_MAX, 999999999, ~0ULL, FALSE };
            b[i] = (struct lorie_thread_info) { 'D', LLONG_MIN / 2, LLONG_MIN / 2, 1, 0, FALSE };
        }
        for (i = LORIE_FREEZE_BEGIN; i <= LORIE_FREEZE_END; i++) {
            n = lorieFreezeLine(line, sizeof(line), &w, &s, i, t, b);
            max = n > max ? n : max;
        }
        fakePvfb.reportUs = fakePvfb.reportMaxUs = UINT64_MAX;
        n = lorieXSchedLine(line, sizeof(line), t, b, UINT32_MAX);
        max = n > max ? n : max;
        CHECK(max < 1024, "the widest freeze line is %d bytes", max);
        printf("  longest freeze line: %d bytes\n", max);
    }
    /* the threads' line */
    {
        char line[1024];
        struct lorie_thread_info at[LORIE_THREADS];
        for (i = 0; i < LORIE_THREADS; i++)
            at[i] = (struct lorie_thread_info) { 'S', 100, 50, -1, 0, FALSE };
        fakePvfb.reportUs = 1234, fakePvfb.reportMaxUs = 2345;
        th[LORIE_THREAD_MAIN].runUs = 400, th[LORIE_THREAD_MAIN].waitUs = 350;
        fakePvfb.inputTid = 4321;
        lorieXSchedLine(line, sizeof(line), th, at, 2);
        CHECK(strstr(line, "XlorieXSched: main=S,futex:lock,run_us=300,runq_us=300 input=S,run_us=900,runq_us=450 ") &&
              strstr(line, " report_us=1234 report_max_us=2345 freezes=2") && fakePvfb.reportMaxUs == 0,
              "threads line: %s", line);
        /* P11-C3-D: the input thread not run yet - dix main starts it as Dispatch begins - or none in this server */
        {
            const char *why = "?";
            fakePvfb.inputTid = 0;
            lorieXSchedLine(line, sizeof(line), th, at, 2);
            CHECK(strstr(line, " input=starting,run_us=") && !lorieInputTid(&why) && why && !strcmp(why, "starting"),
                  "threads line, the input thread not run yet: %s", line);
            InputThreadEnable = FALSE;
            lorieXSchedLine(line, sizeof(line), th, at, 2);
            CHECK(strstr(line, " input=none,run_us=") && !lorieInputTid(&why) && why && !strcmp(why, "none"),
                  "threads line, no input thread: %s", line);
            InputThreadEnable = TRUE;
            fakePvfb.inputTid = 4321;
            CHECK(lorieInputTid(&why) == 4321 && !why, "the input thread's tid once it ran: not given");
        }
    }

    printf("TFREEZE the freeze watchdog (TERMUX_X11_FRAME_FREEZE_DIAG): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
