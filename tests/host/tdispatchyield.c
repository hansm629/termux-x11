/* TDISPATCHYIELD (P11-A): when the X server's main thread runs the work queued for it from other threads - a vsync
 * tick (lorieRedraw) or a GPU copy's completion (handleGpuCopyDoneEvent) - while clients keep sending requests. Run on
 * the patched xserver's own code (gen.py "dispatchyield"): Dispatch and WaitForSomething, the work queue with its lock
 * (dixutils.c), the ready clients and the smart scheduler's choice of client (dispatch.c), and
 * lorieQueueUrgentWorkProc, which queues the work and, with TERMUX_X11_X_YIELD_ON_WAKE=1, asks for the yield. The
 * rest is stubbed: clients are scripted request streams (each request costs SmartScheduleTime, as the smart scheduler's
 * timer would count it), and what the other threads do happens at scripted points - during a given request, when a
 * round is about to start, while the server sleeps, or while the work queue itself runs.
 *
 * Off, the patched loop must do exactly what the xserver's own does (gen.py "dispatchyield_orig", run side by side on
 * the same scripts): the same requests, rounds, work and priorities, in the same order, and no yield ever asked for.
 * On, every tick and completion runs right after the request in progress when it was queued - never behind the rest of
 * a round - each once, in the order queued; a round still serves at least one request; and the clients are served
 * exactly as off - the same requests in the same order, the same priorities: the round a yield broke off goes on with
 * the rest of its slice, so which client runs, and for how long, stays the smart scheduler's choice. */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the server, as far as these functions use it ---- */
typedef int Bool;
typedef uint8_t CARD8;
typedef uint16_t CARD16;
#define TRUE 1
#define FALSE 0
#define Success 0
#define BadLength 16
#define EXTENSION_BASE 128
#define DE_RESET 1
#define DE_TERMINATE 2
#define DE_PRIORITYCHANGE 4
#define SMART_MAX_PRIORITY (20)
#define SMART_MIN_PRIORITY (-20)
#define ETEST(err) ((err) == EAGAIN)
#include "xorglist_src.inc"

typedef struct _Client *ClientPtr;
typedef struct _WorkQueue *WorkQueuePtr;
typedef struct { CARD8 reqType; CARD8 data; CARD16 length; } xReq;
typedef struct { unsigned short (*MinorOpcode)(ClientPtr); } ExtensionEntry;
typedef struct _Client {
    int index, priority, smart_priority;
    long smart_start_tick, smart_stop_tick;
    struct xorg_list ready;
    void *requestBuffer;
    int (**requestVector)(ClientPtr);
    int sequence, majorOp, minorOp, noClientException;
    unsigned long errorValue;
    Bool clientGone;
    /* the script's */
    char name;
    int pending, cost, served;
} ClientRec;

long SmartScheduleTime, SmartScheduleSlice = 5, SmartScheduleInterval = 5, SmartScheduleMaxSlice = 15;
int SmartScheduleLatencyLimited;
static Bool SmartScheduleSignalEnable = TRUE;     /* HAVE_SETITIMER: time advances by the timer (here: by the requests) */
static ClientPtr SmartLastClient;
static int SmartLastIndex[SMART_MAX_PRIORITY - SMART_MIN_PRIORITY + 1];
volatile char dispatchException, isItTimeToYield;
int dispatchYieldRequested;                     /* patches/xserver.patch dix/dispatch.c */
static int nextFreeClientID, nClients;
static long maxBigRequestSize = 4194303;
static ClientPtr currentClient;
static Bool NewOutputPending;
static struct ospoll *server_poll;

#include "dispatchyield_common_src.inc"

static void SmartScheduleStartTimer(void) {}
static void SmartScheduleStopTimer(void) {}
static int check_timers(void) { return -1; }
static void BlockHandler(void *timeout) { (void) timeout; }
static void WakeupHandler(int result) { (void) result; }
static int GetErrno(void) { return errno; }
static void ErrorF(const char *f, ...) { (void) f; }
static Bool InputCheckPending(void) { return FALSE; }
static void ProcessInputEvents(void) {}
static void FlushIfCriticalOutputPending(void) {}
static ExtensionEntry *GetExtensionEntry(int major) { (void) major; return NULL; }
static int XaceHookDispatch(ClientPtr c, int major) { (void) c; (void) major; return Success; }
static long GetTimeInMillis(void) { return SmartScheduleTime; }
static void CloseDownClient(ClientPtr c) { mark_client_not_ready(c); }
static void SendErrorToClient(ClientPtr c, int major, int minor, unsigned long value, int error) {
    (void) c; (void) major; (void) minor; (void) value; (void) error;
}
static void KillAllClients(void) {}
static void ResetOsBuffers(void) {}

/* ---- the script ---- */
#define CLIENTS 2
#define WORKS 1024
static ClientRec clients[CLIENTS];
static int nclients;
static char *trail;                 /* requests ('a', 'b'), work run ("T<n>", "C<n>"), round ends ('|') */
static size_t trailLen;
static void log1(const char *s) {
    size_t n = strlen(s);
    trail = realloc(trail, trailLen + n + 2);
    if (trailLen)
        trail[trailLen++] = ' ';
    memcpy(trail + trailLen, s, n + 1);
    trailLen += n;
}

/* at: where it was queued - during that request (> 0), as a round was about to start (0), while the server slept
 * (-1), or while the work queue ran (-2) */
struct work { char kind; int id, at, queuedAt, ranAt, runs; long queuedTime, ranTime; uint64_t msc; bool nested, drop; };
static struct work works[WORKS];
static int nworks;
static uint64_t msc;                /* as lorieRedraw's pvfb->current_msc: one per tick run */
static int requests;                /* requests served so far */
static ClientPtr serving;           /* the client of the last request served */
static int roundRequests, rounds, emptyRounds;
static bool lastReadEmpty;          /* the round's last read found no request: a round may end with none served then */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static struct { int yieldOnWake; } screenRec, *pvfb = &screenRec;
static int wakePending;
static void lorieWakeServer(void) { wakePending = 1; }
static bool yieldAskedOff;          /* a yield asked for while the option is off */
/* the script's clients, rounds and polls (below) */
static int ReadRequestFromClient(ClientPtr c);
static void FlushAllOutput(void);
static int ospoll_wait(struct ospoll *p, int timeout);

#include "dispatchyield_src.inc"
#define Dispatch origDispatch
#define WaitForSomething origWaitForSomething
#include "dispatchyield_orig_src.inc"
#undef Dispatch
#undef WaitForSomething

static void post(char kind, bool nested, int at);
/* the work, as lorieRedraw / handleGpuCopyDoneEvent run (here only logged) */
static Bool runWork(ClientPtr c, void *closure) {
    struct work *w = closure;
    char s[16];
    (void) c;
    w->runs++;
    w->ranAt = requests;
    w->ranTime = SmartScheduleTime;
    if (w->kind == 'T')
        w->msc = ++msc;
    snprintf(s, sizeof s, "%c%d", w->kind, w->id);
    log1(s);
    if (w->nested)                  /* another thread queues more while the work queue runs */
        post('C', false, -2);
    if (w->drop)                    /* the client in the round stops being ready meanwhile (ignored, or gone) */
        mark_client_not_ready(serving);
    return TRUE;
}
/* another thread (the Choreographer's, the input thread) queuing a tick or a completion */
static void post(char kind, bool nested, int at) {
    struct work *w = &works[nworks];
    *w = (struct work) { .kind = kind, .id = nworks + 1, .at = at, .queuedAt = requests, .queuedTime = SmartScheduleTime,
                         .nested = nested };
    nworks++;
    lorieQueueUrgentWorkProc(runWork, w);
    if (!pvfb->yieldOnWake && __atomic_load_n(&dispatchYieldRequested, __ATOMIC_RELAXED))
        yieldAskedOff = true;
}

/* when the other threads act: during request `at` (1-based), or - at 0 - when a round is about to start, or - at -1 -
 * while the server sleeps */
struct event { int at; char kind; bool nested, drop; };
struct script { const char *name; int pending[CLIENTS], cost[CLIENTS]; int every[2]; struct event ev[16]; };
static const struct script *script;
static bool fired[16];

static void fire(int at) {
    for (int i = 0; i < 16 && script->ev[i].kind; i++)
        if (script->ev[i].at == at && !fired[i]) {
            fired[i] = true;
            post(script->ev[i].kind, script->ev[i].nested, at);
            works[nworks - 1].drop = script->ev[i].drop;
        }
    if (at > 0 && script->every[0] && at % script->every[0] == 0)
        post('T', false, at);
    if (at > 0 && script->every[1] && at % script->every[1] == 0)
        post('C', false, at);
}

static int request(ClientPtr c) {
    char s[2] = { c->name, 0 };
    requests++;
    roundRequests++;
    serving = c;
    c->served++;
    c->pending--;
    SmartScheduleTime += c->cost;
    log1(s);
    fire(requests);
    return Success;
}
static int (*requestVector[256])(ClientPtr);
static xReq req = { .reqType = 1 };
static int ReadRequestFromClient(ClientPtr c) {
    if (c->pending <= 0) {
        mark_client_not_ready(c);
        lastReadEmpty = true;
        return 0;
    }
    lastReadEmpty = false;
    c->requestBuffer = &req;
    return 4;
}
/* Dispatch's end of a round */
static void FlushAllOutput(void) {
    rounds++;
    if (!roundRequests && !lastReadEmpty)
        emptyRounds++;
    roundRequests = 0;
    lastReadEmpty = false;
    log1("|");
}
static bool connected;
static int ospoll_wait(struct ospoll *p, int timeout) {
    (void) p;
    if (!connected) {
        connected = true;
        for (int i = 0; i < nclients; i++)
            if (clients[i].pending > 0)
                mark_client_ready(&clients[i]);
        return 1;
    }
    if (timeout == 0)               /* a round about to start */
        fire(0);
    if (wakePending) {
        wakePending = 0;
        return 1;
    }
    if (timeout == 0)
        return 0;
    for (int i = 0; i < 16 && script->ev[i].kind; i++)
        if (script->ev[i].at == -1 && !fired[i]) {
            fire(-1);
            wakePending = 0;
            return 1;
        }
    dispatchException = DE_TERMINATE;
    return 0;
}

struct result {
    char *trail;
    struct work works[WORKS];
    int nworks, rounds, emptyRounds, served[CLIENTS], priority[CLIENTS], midShare;
    uint64_t msc;
    bool yieldAskedOff, flagLeft;
};
static void run(void (*dispatch)(void), int yield, const struct script *s, struct result *r) {
    script = s;
    memset(fired, 0, sizeof fired);
    free(trail);
    trail = NULL;
    trailLen = 0;
    nworks = requests = roundRequests = rounds = emptyRounds = 0;
    lastReadEmpty = connected = yieldAskedOff = false;
    msc = 0;
    wakePending = 0;
    SmartScheduleTime = 0;
    SmartLastClient = NULL;
    memset(SmartLastIndex, 0, sizeof SmartLastIndex);
    dispatchException = isItTimeToYield = 0;
    dispatchYieldRequested = 0;
    pvfb->yieldOnWake = yield;
    for (int i = 0; i < 256; i++)
        requestVector[i] = request;
    nclients = s->pending[1] ? 2 : 1;
    for (int i = 0; i < nclients; i++) {
        clients[i] = (ClientRec) { .index = i + 1, .name = (char) ('a' + i), .pending = s->pending[i], .cost = s->cost[i],
                                   .requestVector = requestVector };
        xorg_list_init(&clients[i].ready);
    }
    dispatch();
    r->trail = strdup(trail ? trail : "");
    memcpy(r->works, works, sizeof works);
    r->nworks = nworks;
    r->rounds = rounds;
    r->emptyRounds = emptyRounds;
    r->msc = msc;
    r->yieldAskedOff = yieldAskedOff;
    r->flagLeft = dispatchYieldRequested != 0;
    for (int i = 0; i < nclients; i++) {
        r->served[i] = clients[i].served;
        r->priority[i] = clients[i].smart_priority;
    }
    CHECK(!workQueue, "%s: work left queued", s->name);
}

/* how long work waited behind requests: the most requests served, and SmartScheduleTime gone by, after it was queued */
static void waited(const struct result *r, int *reqs, long *ms) {
    *reqs = 0;
    *ms = 0;
    for (int i = 0; i < r->nworks; i++) {
        if (r->works[i].ranAt - r->works[i].queuedAt > *reqs)
            *reqs = r->works[i].ranAt - r->works[i].queuedAt;
        if (r->works[i].ranTime - r->works[i].queuedTime > *ms)
            *ms = r->works[i].ranTime - r->works[i].queuedTime;
    }
}

/* the requests alone, in the order served */
static char *requestsOnly(const char *t) {
    char *r = malloc(strlen(t) + 1), *q = r;
    for (; *t; t++)
        if (*t == 'a' || *t == 'b')
            *q++ = *t;
    *q = 0;
    return r;
}

/* every work ran once, in the order queued; the ticks' msc one by one */
static void ranOnceInOrder(const char *what, const struct result *r) {
    uint64_t m = 0;
    for (int i = 0; i < r->nworks; i++) {
        const struct work *w = &r->works[i];
        CHECK(w->runs == 1, "%s: %c%d ran %d times", what, w->kind, w->id, w->runs);
        if (i)
            CHECK(w->ranAt >= r->works[i - 1].ranAt, "%s: %c%d ran before %c%d, queued first", what, w->kind, w->id,
                  r->works[i - 1].kind, r->works[i - 1].id);
        if (w->kind == 'T')
            CHECK(w->msc == ++m, "%s: tick %d at msc %llu (want %llu)", what, w->id, (unsigned long long) w->msc, (unsigned long long) m);
    }
    {
        /* the order in the trail itself */
        int last = 0;
        for (const char *p = r->trail; (p = strpbrk(p, "TC")); p++) {
            int id = atoi(p + 1);
            CHECK(id > last, "%s: work %d ran after work %d", what, id, last);
            last = id;
        }
    }
}

static const struct script scripts[] = {
    { "one client streaming, a few ticks and completions", { 60 }, { 1 }, { 0, 0 },
      { { 3, 'T' }, { 7, 'C' }, { 20, 'T' }, { 21, 'C' }, { 40, 'T' } } },
    { "one client streaming 1600 requests (its slice grows to 15), ticks every 16, completions every 5", { 1600 }, { 1 },
      { 16, 5 }, { { 0 } } },
    { "two clients streaming, ticks every 16, completions every 7", { 400, 400 }, { 1, 1 }, { 16, 7 }, { { 0 } } },
    { "queued while a round is about to start", { 30 }, { 1 }, { 0, 0 }, { { 0, 'T' } } },
    { "queued while the work queue runs", { 40 }, { 1 }, { 0, 0 }, { { 5, 'T', true }, { 25, 'T' } } },
    { "queued while the server sleeps", { 10 }, { 1 }, { 0, 0 }, { { -1, 'T' }, { 4, 'C' } } },
    { "ticks every 3 requests, the slice never reached", { 90 }, { 1 }, { 3, 0 }, { { 0 } } },
    { "the client in the round no longer ready once its work ran", { 20, 20 }, { 1, 1 }, { 0, 0 }, { { 2, 'T', false, true } } },
};

int main(void) {
    struct result *orig = calloc(1, sizeof *orig), *off = calloc(1, sizeof *off), *on = calloc(1, sizeof *on);

    for (size_t k = 0; k + 1 < sizeof scripts / sizeof *scripts; k++) {
        const struct script *s = &scripts[k];
        int offReqs, onReqs;
        long offMs, onMs;

        run(origDispatch, 0, s, orig);
        run(Dispatch, 0, s, off);
        run(Dispatch, 1, s, on);

        /* off: the xserver's own loop, step for step */
        CHECK(!strcmp(off->trail, orig->trail) && off->rounds == orig->rounds && off->msc == orig->msc &&
              !memcmp(off->served, orig->served, sizeof off->served) && !memcmp(off->priority, orig->priority, sizeof off->priority),
              "%s: off is not the xserver's own loop", s->name);
        for (int i = 0; i < off->nworks; i++)
            CHECK(off->works[i].ranAt == orig->works[i].ranAt && off->works[i].runs == orig->works[i].runs,
                  "%s: off, work %d ran at request %d (the xserver's own: %d)", s->name, i + 1, off->works[i].ranAt,
                  orig->works[i].ranAt);
        CHECK(!off->yieldAskedOff && !off->flagLeft, "%s: off asked for a yield", s->name);

        /* on: right after the request in progress, once, in order; every round serves; the clients served in full */
        ranOnceInOrder(s->name, on);
        ranOnceInOrder(s->name, off);
        waited(off, &offReqs, &offMs);
        waited(on, &onReqs, &onMs);
        for (int i = 0; i < on->nworks; i++) {
            /* queued as a round is about to start, the round serves its one request first */
            const struct work *w = &on->works[i];
            CHECK(w->ranAt == w->queuedAt + (w->at == 0), "%s: on, %c%d queued after request %d (at %d) ran after request %d",
                  s->name, w->kind, w->id, w->queuedAt, w->at, w->ranAt);
        }
        CHECK(on->emptyRounds == 0 && off->emptyRounds == 0, "%s: a round ended serving no request (on %d, off %d)", s->name,
              on->emptyRounds, off->emptyRounds);
        CHECK(!memcmp(on->served, off->served, sizeof on->served) && on->msc == off->msc && on->nworks == off->nworks,
              "%s: on, served or ran something else", s->name);
        {
            char *a = requestsOnly(on->trail), *b = requestsOnly(off->trail);
            CHECK(!strcmp(a, b) && !memcmp(on->priority, off->priority, sizeof on->priority),
                  "%s: on, the clients not served as off (same order %s, priorities %d %d, off %d %d)", s->name,
                  strcmp(a, b) ? "no" : "yes", on->priority[0], on->priority[1], off->priority[0], off->priority[1]);
            free(a);
            free(b);
        }
        CHECK(!on->flagLeft, "%s: on, a yield still asked for at the end", s->name);
        printf("  %-72s waited off %3d requests %3ld ms, on %d requests %ld ms\n", s->name, offReqs, offMs, onReqs, onMs);
    }

    /* the client whose round a yield broke off no longer ready when the work ran: not resumed - the scheduler picks
     * another; off, as the xserver's own */
    {
        const struct script *s = &scripts[sizeof scripts / sizeof *scripts - 1];
        const char *t;
        run(origDispatch, 0, s, orig);
        run(Dispatch, 0, s, off);
        CHECK(!strcmp(off->trail, orig->trail), "no longer ready: off is not the xserver's own loop");
        run(Dispatch, 1, s, on);
        t = strstr(on->trail, "T1");
        CHECK(t && !strchr(t, 'b') && on->served[1] == 2 && on->served[0] == 20 && on->works[0].ranAt == 2 &&
              on->emptyRounds == 0, "no longer ready: on, %s", on->trail);
        printf("  %-72s on %s\n", s->name, on->trail);
    }

    /* two clients streaming: each one's share of the first 400 requests, the same either way */
    {
        const struct script *s = &scripts[2];
        int half[2][2] = { { 0 } };
        for (int v = 0; v < 2; v++) {
            struct result *r = v ? on : off;
            run(Dispatch, v, s, r);
            for (const char *p = r->trail; *p && half[v][0] + half[v][1] < 400; p++)
                if (*p == 'a' || *p == 'b')
                    half[v][*p - 'a']++;
        }
        CHECK(half[1][0] == half[0][0] && half[1][1] == half[0][1], "two clients: on, %d and %d of the first 400 requests (off %d and %d)",
              half[1][0], half[1][1], half[0][0], half[0][1]);
        printf("  %-72s first 400: off a %d b %d, on a %d b %d\n", "two clients streaming: their share", half[0][0], half[0][1],
               half[1][0], half[1][1]);
    }

    printf("TDISPATCHYIELD the X server's main loop with TERMUX_X11_X_YIELD_ON_WAKE off and on: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
