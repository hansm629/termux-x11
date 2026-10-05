/* What reaches the compositor when a client presents once a vsync and the root goes to it directly
 * (ROOT_DIRECT). Production order, with the renderer free to run at any point of it:
 *
 *   X server (lorieRedraw, InitOutput.c, on a vsync tick):
 *     1. the vsync tick: presents due now run (loriePerformVblanks) - each queues its GPU copy into
 *        the drawing slot and signals the renderer (lorieTryScheduleGpuCopy)
 *     2. waitForNextFrame = false - the renderer's vsync gate opens
 *     3. the root's damage: remap, stale marking
 *     4. lorieRootHandover: the drawing slot published - newest slot and publish count in rootHandover
 *     5. drawRequested = TRUE, and the renderer signalled
 *   or, a present arriving between ticks (request dispatch), then the block handler's lorieRedraw with
 *   no tick: its copy queued and signalled, then 3-5.
 *
 *   renderer (rendererThread, renderer.c): woken, it loops while rendererShouldWait() says not to wait -
 *   which takes a queued copy as work before it looks at the gate - and each time round either runs a
 *   frame (gate open, and drawRequested, a retry or a queued copy) or, gate closed, drains the queue on
 *   its own (rendererApplyPendingGpuCopies). A ROOT_DIRECT frame (rendererRedrawLocked -> rootZcPresent):
 *     claim the newest published slot (rendererClaimRootBuffer); alreadyOnScreen if it is the one the
 *     compositor already has; the shared lock (X steps can land while it waits); drain the queue;
 *     rootZcFrameBegun; the copies' fence (X steps can land here too); rootZcFrameDrained; then either
 *     nothing newer (rootZcNothingNewDone) or apply it to the compositor (rootZcSubmitDone).
 *
 *   compositor: at each vsync it latches the buffer applied last, and gives back the ones before it.
 *
 * rendererShouldWait, rendererClaimRootBuffer, rendererSetOutputRetry, rendererReleaseRootSlot,
 * rootZcPublishedSinceClaim and the four gates are the real code, extracted from renderer.c by gen.py.
 * The gates are run as they are now (the positive control) and as 38712c5 had them, statement for
 * statement (the negative control): there, every frame took the vsync, and nothing asked again for a
 * slot published while a frame ran.
 *
 * What has to hold now, a client presenting once a vsync whenever in it, the renderer woken at any
 * points: every published slot is applied and latched, the latched content moving on by exactly one
 * each vsync - never shown twice, never skipped - and never two buffers applied in one vsync. And the
 * negative control has to show the regression: slots published and never applied, frames shown twice. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/cdefs.h>
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif
typedef int Bool;
#define TRUE 1
#define FALSE 0

/* what rendererShouldWait looks at besides the shared state, all quiet here */
struct xorg_list { volatile struct xorg_list *next, *prev; };
static volatile struct xorg_list addedBuffers = { &addedBuffers, &addedBuffers }, removedBuffers = { &removedBuffers, &removedBuffers };
static int xorg_list_is_empty(volatile struct xorg_list *l) { return l->next == l; }
static int bufferLock;
#define pthread_spin_lock(l) ((void) (l))
#define pthread_spin_unlock(l) ((void) (l))
static volatile bool stateChanged, windowChanged, expectedSizeChanged;
static int64_t rendererCopyBlockedUntilNs;
static uint64_t rendererCopyDeferredSerial;
static int rendererPreRedrawCoalesceFrames, rendererHighRefreshLimitFrames;
static int64_t rendererPreRedrawCoalesceWaitUs, rendererHighRefreshLimitWaitUs;
static int64_t rendererLastPreRedrawCoalesceWaitUs = -1, rendererLastHighRefreshLimitWaitUs = -1;
static uint64_t rendererPreRedrawCoalescedCount, rendererHighRefreshLimitedCount;
static pthread_mutex_t stateLock;
static pthread_cond_t *stateCond;
#define pthread_cond_timedwait(c, m, t) ((void) (c), (void) (m), (void) (t))
static int64_t rendererNowNs(void) { return 0; }
#define log(...) ((void) 0)

static struct {
    struct { volatile uint32_t readIndex, writeIndex; } gpuCopyQueue;
    volatile uint32_t rootHandover;
    volatile uint64_t rootBufferIds[8];
    volatile uint8_t rootDoubleBuffered;
    volatile uint64_t rootWindowTextureID;
    volatile uint8_t surfaceAvailable, drawRequested, waitForNextFrame;
    volatile uint32_t outputRetryPending;
    struct { volatile uint8_t moved, updated; } cursor;
    struct { uint32_t rootClaimsAcrossPools, rootStaleSlotReleases, directReuseNoSubmit,
                      directPublishedDuringNothingNew, directPublishedDuringSubmit; } presentStats;
} fakeState, *state = &fakeState;
#include "pacing_src.inc"
#include "pacing_gates_src.inc"

/* 38712c5's rootZcPresent, statement for statement where the gates now stand: drawRequested cleared
 * after the drain, the vsync taken by every frame once its copies were done, and the output retry
 * cleared whatever the frame did. */
static void oldFrameBegun(void) { state->drawRequested = FALSE; }
static void oldFrameDrained(bool alreadyOnScreen) { (void) alreadyOnScreen; state->waitForNextFrame = true; }
static void oldNothingNewDone(void) {
    rendererSetOutputRetry(false);
    __atomic_fetch_add(&state->presentStats.directReuseNoSubmit, 1, __ATOMIC_RELAXED);
}
static void oldSubmitDone(void) { rendererSetOutputRetry(false); }
static struct { void (*begun)(void), (*drained)(bool), (*nothingNew)(void), (*submitted)(void); } gates;
static void useGates(bool old) {
    if (old) { gates.begun = oldFrameBegun; gates.drained = oldFrameDrained; gates.nothingNew = oldNothingNewDone; gates.submitted = oldSubmitDone; }
    else { gates.begun = rootZcFrameBegun; gates.drained = rootZcFrameDrained; gates.nothingNew = rootZcNothingNewDone; gates.submitted = rootZcSubmitDone; }
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the X server ---- */
static int xWrite, content[LORIE_ROOT_SLOTS], presentNo;
static bool damaged, dirty;
static int published[4096], publishedCount;      /* the content of each publish, in order */
static void xPresent(void) {                     /* present_execute_copy -> lorieTryScheduleGpuCopy */
    content[xWrite] = ++presentNo;
    state->gpuCopyQueue.writeIndex++;
    damaged = true;                              /* lorieDamageGpuCopy */
}
static void xOpenGate(void) { state->waitForNextFrame = false; }
/* lorieRootHandover's publish: the drawing slot becomes the newest and the publish count moves on; the
 * next drawing slot is one neither held nor the newest. Without one it is retried later (rootDirty). */
static bool xPublish(void) {
    uint32_t old = __atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE), next;
    int n;
    for (;;) {
        for (n = 0; n < LORIE_ROOT_SLOTS; n++)
            if (n != xWrite && !(old & LORIE_ROOT_HELD_MASK & (1u << n))) break;
        if (n == LORIE_ROOT_SLOTS)
            return false;
        next = (old & ~((uint32_t) LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT) & ~LORIE_ROOT_COUNT_MASK)
             | ((uint32_t) xWrite << LORIE_ROOT_NEWEST_SHIFT) | ((old + LORIE_ROOT_COUNT_STEP) & LORIE_ROOT_COUNT_MASK);
        if (__atomic_compare_exchange_n(&state->rootHandover, &old, next, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    published[publishedCount++ % 4096] = content[xWrite];
    xWrite = n;
    return true;
}
static void xDamage(void) { /* remap, stale marking: nothing the renderer sees */ }
static void xHandoverSignal(void) {             /* lorieRedraw, from "if (nonEmpty ...)" to the signal */
    if (damaged) {
        damaged = false;
        dirty = !xPublish();
        state->drawRequested = TRUE;
    }
    if (dirty && xPublish()) {
        dirty = false;
        state->drawRequested = TRUE;
    }
    /* "if (drawRequested || outputRetryPending ...) pthread_cond_signal": the renderer is woken after */
}
enum { PRESENT, OPEN, DAMAGE, HANDOVER, STEPS };

/* ---- the compositor ---- */
static int displayedSlot = -1, appliedThisVsync, latched, lastLatched;
static uint64_t displayedId;
static struct { int slot; uint64_t id; } retiring[8];
static int retiringCount;
static bool appliedContent[8192];
static int repeats, skipsLatched, doubles, maxLag;

/* ---- the renderer ---- */
static void (*inLock)(void), (*inFence)(void);  /* X work landing while a frame waits */
static bool hookRan;
static void runHook(void (**h)(void)) { if (*h) { void (*f)(void) = *h; *h = NULL; f(); hookRan = true; } }
static void rootZcPresentModel(void) {          /* rendererRedrawLocked -> rootZcPresent, ROOT_DIRECT */
    uint64_t id = rendererClaimRootBuffer();
    int slot = rendererRootSlot;
    bool alreadyOnScreen = slot == displayedSlot && id == displayedId;
    runHook(&inLock);                            /* lorie_mutex_lock(&state->lock) */
    state->gpuCopyQueue.readIndex = state->gpuCopyQueue.writeIndex;   /* rendererApplyPendingGpuCopiesLocked */
    gates.begun();
    runHook(&inFence);                           /* rendererFinishIssuedWork */
    gates.drained(alreadyOnScreen);
    if (alreadyOnScreen) {
        rendererRootSlot = -1;
        gates.nothingNew();
        return;
    }
    if (displayedSlot >= 0) { retiring[retiringCount].slot = displayedSlot; retiring[retiringCount++].id = displayedId; }
    displayedSlot = slot;
    displayedId = id;
    appliedThisVsync++;
    if (content[slot] < 8192) appliedContent[content[slot]] = true;
    rendererRootSlot = -1;
    gates.submitted();
}
static int spins;
static void rendererRun(void) {                 /* rendererThread: woken, round while it should not wait */
    static bool waitingForBuffers = false;
    int rounds = 0;
    while (!rendererShouldWait(&waitingForBuffers)) {
        bool gpuCopyPending = state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex;
        if (++rounds > 8) { spins++; return; }
        if (state->surfaceAvailable && !state->waitForNextFrame &&
            (state->drawRequested || rootZcRetryPending || state->cursor.moved || state->cursor.updated || gpuCopyPending))
            rootZcPresentModel();
        else if (gpuCopyPending)
            state->gpuCopyQueue.readIndex = state->gpuCopyQueue.writeIndex;   /* rendererApplyPendingGpuCopies */
    }
}
static void vsync(bool judged) {
    latched = displayedSlot >= 0 ? content[displayedSlot] : 0;
    if (judged) {
        if (appliedThisVsync > 1) doubles++;
        if (latched == lastLatched) repeats++;
        else if (latched > lastLatched + 1) skipsLatched++;
        if (presentNo - latched > maxLag) maxLag = presentNo - latched;
    }
    lastLatched = latched;
    appliedThisVsync = 0;
    for (int i = 0; i < retiringCount; i++)
        rendererReleaseRootSlot(retiring[i].slot, retiring[i].id, rendererRootSlotGen);
    retiringCount = 0;
}

static void reset(bool old) {
    memset(&fakeState, 0, sizeof fakeState);
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) { state->rootBufferIds[i] = 100 + i; content[i] = 0; }
    state->rootDoubleBuffered = 1;
    state->surfaceAvailable = 1;
    state->rootHandover = 0;                     /* slot 0 published, nothing held */
    xWrite = 1; presentNo = publishedCount = 0; damaged = dirty = false;
    displayedSlot = -1; displayedId = 0; appliedThisVsync = retiringCount = latched = lastLatched = 0;
    memset(appliedContent, 0, sizeof appliedContent);
    repeats = skipsLatched = doubles = maxLag = spins = 0;
    rootZcRetryPending = false; rendererRootSlot = -1; inLock = inFence = NULL;
    useGates(old);
}

/* One vsync period. `mid` false: the present runs at the tick (steps PRESENT, OPEN, DAMAGE, HANDOVER);
 * true: the tick finds nothing (OPEN, and a handover only if something is pending), and the present
 * arrives between ticks (PRESENT, DAMAGE, HANDOVER). wake[k] wakes the renderer just before step k,
 * wake[STEPS] once more after the last (it is always woken after a handover's signal regardless); `at`
 * puts that step inside the first frame woken before it, in its lock wait (window 0) or fence wait (1).
 * `present` false is a client late by a vsync. */
typedef struct { bool mid; int wake[STEPS + 1], at, window; } Pattern;
static int hookStep;
static void hookFn(void) {
    if (hookStep == PRESENT) xPresent();
    else if (hookStep == OPEN) xOpenGate();
    else if (hookStep == DAMAGE) xDamage();
    else xHandoverSignal();
}
static void doStep(const Pattern *p, int k, bool present) {
    bool landed = false;
    if (k == PRESENT && !present)
        return;
    for (int w = 0; w < p->wake[k]; w++) {
        if (p->at == k && w == 0 && !landed) {
            hookStep = k; hookRan = false;
            if (p->window == 0) inLock = hookFn; else inFence = hookFn;
            rendererRun();
            inLock = inFence = NULL;
            landed = hookRan;
        } else
            rendererRun();
    }
    if (!landed)
        switch (k) {
        case PRESENT: xPresent(); break;
        case OPEN: xOpenGate(); break;
        case DAMAGE: xDamage(); break;
        default: xHandoverSignal(); break;
        }
}
static void period(const Pattern *p, bool present) {
    if (!p->mid) {
        for (int k = PRESENT; k <= HANDOVER; k++) doStep(p, k, present);
    } else {
        Pattern tickPart = *p;
        tickPart.at = -1;
        doStep(&tickPart, OPEN, present);
        xHandoverSignal();                       /* the tick's lorieRedraw: nothing damaged yet */
        rendererRun();
        doStep(p, PRESENT, present);
        doStep(p, DAMAGE, present);
        doStep(p, HANDOVER, present);
    }
    for (int w = 0; w < p->wake[STEPS] + 1; w++)
        rendererRun();
}

static Pattern patterns[1024];
static int patternCount;
static void makePatterns(void) {
    for (int mid = 0; mid < 2; mid++)
        for (int code = 0; code < 32; code++)
            for (int at = -1; at < STEPS; at++)
                for (int window = 0; window < 2; window++) {
                    Pattern p = { mid, { 0 }, at, window };
                    for (int k = 0; k <= STEPS; k++) p.wake[k] = (code >> k) & 1;
                    /* the gate opening inside a frame that then takes the vsync is a frame running over
                     * its vsync - the same before the root went direct; not a case here */
                    if (at == OPEN || (at == -1 && window) || (at >= 0 && !p.wake[at])) continue;
                    if (mid && at == OPEN) continue;
                    patterns[patternCount++] = p;
                }
}
static const Pattern settled = { false, { 0 }, -1, 0 };

typedef struct { int pairsFailed, pairs, worstLag, published, applied, neverApplied, repeats, skips, doubles, spins; } Outcome;
static Outcome alternate(bool old) {
    Outcome o = { 0 };
    for (int i = 0; i < patternCount; i++)
        for (int j = 0; j < patternCount; j++) {
            int never = 0, applied = 0, base;
            reset(old);
            for (int t = 0; t < 4; t++) { period(&settled, true); vsync(false); }
            base = presentNo;
            for (int t = 0; t < 24; t++) { period(t & 1 ? &patterns[j] : &patterns[i], true); vsync(true); }
            period(&settled, true); vsync(false);   /* the last one's chance to go out */
            for (int c = base + 1; c < presentNo; c++) { if (appliedContent[c]) applied++; else never++; }
            o.pairs++;
            o.published += presentNo - base - 1;
            o.applied += applied;
            o.neverApplied += never;
            o.repeats += repeats;
            o.skips += skipsLatched;
            o.doubles += doubles;
            o.spins += spins;
            if (maxLag > o.worstLag) o.worstLag = maxLag;
            if (never || repeats || skipsLatched || doubles || spins)
                o.pairsFailed++;
        }
    return o;
}

int main(void) {
    makePatterns();

    /* every pair of wake orders and present timings, alternating vsync by vsync - what wakes the
     * renderer, and when a game's present lands against the X server's tick, changes from one vsync to
     * the next - one present a vsync */
    Outcome now = alternate(false), before = alternate(true);
    printf("pacing now:     %d pairs alternating; %d presents published, %d applied, %d never applied; %d frames shown "
           "twice, %d skipped at latch, %d vsyncs with two applied, %d spins; latency at most %d vsync\n", now.pairs,
           now.published, now.applied, now.neverApplied, now.repeats, now.skips, now.doubles, now.spins, now.worstLag);
    printf("pacing 38712c5: %d pairs alternating; %d presents published, %d applied, %d never applied; %d frames shown "
           "twice, %d skipped at latch, %d vsyncs with two applied, %d spins; latency at most %d vsync\n", before.pairs,
           before.published, before.applied, before.neverApplied, before.repeats, before.skips, before.doubles,
           before.spins, before.worstLag);
    CHECK(now.pairsFailed == 0, "positive control: %d pairs of wake orders lose or repeat frames", now.pairsFailed);
    CHECK(before.neverApplied > 0 && before.repeats > 0,
          "negative control: 38712c5's gates did not reproduce the regression (never applied %d, shown twice %d)",
          before.neverApplied, before.repeats);

    /* the regression's own interleaving, step by step: a present between ticks, the renderer woken by
     * its copy before the handover, the published slot then waiting */
    for (int old = 0; old < 2; old++) {
        Pattern woken = { true, { 0, 0, 1, 0, 0 }, -1, 0 };   /* woken by the copy: after PRESENT, before DAMAGE */
        reset(old);
        for (int t = 0; t < 4; t++) { period(&settled, true); vsync(false); }
        int base = presentNo, never = 0;
        for (int t = 0; t < 24; t++) { period(t & 1 ? &settled : &woken, true); vsync(true); }
        period(&settled, true); vsync(false);
        for (int c = base + 1; c < presentNo; c++) never += !appliedContent[c];
        printf("  %s, present between ticks woken by its copy every other vsync: %d published, %d never applied, "
               "%d shown twice\n", old ? "38712c5" : "now    ", presentNo - base - 1, never, repeats);
        if (old)
            CHECK(never > 0 && repeats > 0, "negative control: the regression's interleaving did not lose frames");
        else
            CHECK(never == 0 && repeats == 0 && skipsLatched == 0, "positive control: the regression's interleaving still "
                  "loses %d frames, shows %d twice", never, repeats);
    }

    /* two presents in one vsync - a client faster than the display - and then none: the last one has to
     * reach the screen at the next vsync, whatever the renderer was doing when it was published, even
     * with nothing more to come to ask for it */
    for (int old = 0; old < 2; old++) {
        int lost = 0, doubled = 0, cases = 0;
        for (int i = 0; i < patternCount; i++) {
            if (patterns[i].mid) continue;
            for (int j = 0; j < patternCount; j++) {
                if (!patterns[j].mid) continue;
                reset(old);
                for (int t = 0; t < 4; t++) { period(&settled, true); vsync(false); }
                period(&patterns[i], true);              /* a present at the tick ... */
                doStep(&patterns[j], PRESENT, true);      /* ... and another before the next one */
                doStep(&patterns[j], DAMAGE, true);
                doStep(&patterns[j], HANDOVER, true);
                for (int w = 0; w < patterns[j].wake[STEPS] + 1; w++) rendererRun();
                vsync(true);
                doubled += doubles;
                for (int t = 0; t < 3; t++) { period(&settled, false); vsync(false); }
                cases++;
                lost += latched != presentNo;
            }
        }
        printf("  %s, two presents in a vsync and then none: %d cases, %d leaving the last one off the screen, "
               "%d with two applied in that vsync\n", old ? "38712c5" : "now    ", cases, lost, doubled);
        if (!old)
            CHECK(lost == 0 && doubled == 0, "positive control: a burst's last present left off the screen in %d cases", lost);
    }

    /* the second of two presents in one vsync handed on while the frame taking the first one waits for
     * the shared lock - between its claim and its clearing drawRequested - and then no more presents */
    for (int old = 0; old < 2; old++) {
        reset(old);
        for (int t = 0; t < 4; t++) { period(&settled, true); vsync(false); }
        xPresent(); xOpenGate(); xDamage(); xHandoverSignal();        /* P1 handed on, the renderer not yet run */
        xPresent(); xDamage();                                        /* P2 queued */
        hookStep = HANDOVER; inLock = hookFn;                         /* P2 handed on during P1's frame */
        rendererRun();
        inLock = NULL;
        vsync(false);
        int second = presentNo;
        for (int t = 0; t < 3; t++) { period(&settled, false); vsync(false); }
        printf("  %s, a present handed on during the frame taking the one before it, then none: P%d %s\n",
               old ? "38712c5" : "now    ", second, latched == second ? "shown" : "never shown");
        if (old)
            CHECK(latched != second, "negative control: the lost request did not show");
        else
            CHECK(latched == second, "positive control: a present handed on during a frame never shown");
    }

    printf("display pacing (ROOT_DIRECT gates, negative control 38712c5): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
