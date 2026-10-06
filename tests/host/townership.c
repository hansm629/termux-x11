/* TOWNERSHIP: the X server's CPU ownership of AHardwareBuffers (struct lorie_backend_control in lorie.h), from
 * the real code: the ownership helpers and every pixmap path that takes or drops a CPU lock, cut out of
 * InitOutput.c (gen.py "own_new"), run beside the same paths cut out of 3c98ffa's InitOutput.c ("own_base") on
 * the same operations, over fake buffers and a fake shared lock that record every call in order.
 *
 *   PERSISTENT is 3c98ffa: the same calls, in the same order, as 3c98ffa's code makes - when nothing is asked,
 *   all through a session asked for gles (PER_ACCESS refused), and once PER_ACCESS ends (the renderer gives it
 *   back on fallback, its connection drops, or a new one is made), from buffers locked as 3c98ffa's are.
 *   Buffers that are not AHBs are handled as 3c98ffa handles them throughout.
 *   PER_ACCESS: whenever the renderer could take the shared lock and finds PER_ACCESS answered, the X server
 *   has no AHB CPU-locked; every CPU lock of an AHB is taken, and dropped, inside the shared lock.
 *   The answer describes the locks every time the shared lock is let go: PER_ACCESS - no AHB CPU-locked;
 *   PERSISTENT - every pixmap 3c98ffa keeps CPU-locked is. */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xorglist_src.inc"
#include "diagtypes_src.inc"
#include "own_consts_src.inc"

#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __always_inline
#define __always_inline __attribute__((always_inline))
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the X server, EXA and LorieBuffer, as far as the paths under test use them ---- */
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#define min(a, b) ((a) < (b) ? (a) : (b))
enum { LORIEBUFFER_REGULAR, LORIEBUFFER_FD, LORIEBUFFER_AHARDWAREBUFFER };

typedef struct { int type; unsigned long long id; int width, height, stride; } LorieBuffer_Desc;
typedef struct LorieBuffer {
    LorieBuffer_Desc desc;
    int locked, released, gpuCopyPending, failLock, lockAttempts;
    int cpuHeld;        /* the AHB's own CPU lock: taken only when locking succeeded (LorieBuffer's `locked` is set
                         * either way, as buffer.c does) */
    void *lockedData;
    uint32_t *mem;
} LorieBuffer;

typedef struct FakePixmap { struct { void *ptr; } devPrivate; void *driverPriv; } PixmapRec, *PixmapPtr;
typedef struct FakeWindow { PixmapPtr pixmap; } WindowRec, *WindowPtr;
typedef struct FakeScreen ScreenRec, *ScreenPtr;
struct FakeScreen {
    WindowPtr root;
    void *devPrivate;
    PixmapPtr (*GetWindowPixmap)(WindowPtr);
    void (*SetWindowPixmap)(WindowPtr, PixmapPtr);
    PixmapPtr (*GetScreenPixmap)(ScreenPtr);
    Bool (*ModifyPixmapHeader)(PixmapPtr, int, int, int, int, int, void *);
};
typedef void *ClientPtr;
#define exaGetPixmapDriverPrivate(p) (((PixmapPtr) (p))->driverPriv)

struct fake_state { int lock, lockingPid; struct lorie_renderer_diag diag; struct lorie_backend_control backend; };
struct fake_pvfb {
    struct fake_state *state;
    struct { Bool legacyDrawing; } root;
    uint64_t rootGpuCopyPending;
    void (*SetWindowPixmap)(WindowPtr, PixmapPtr);
    uint8_t ownership;
    struct xorg_list ahbPersistent;
};

static ScreenRec screen;
static WindowRec rootWin;
static ScreenPtr pScreenPtr = &screen;

/* every call, in order: A<id> allocate, V<id> convert, R<id> release, L<id>/U<id> CPU lock/unlock (! when
 * refused), M+/M- the shared lock, H a pixmap header change */
static char trace[1 << 16];
static size_t traceLen;
static void ev(const char *fmt, ...) {
    va_list ap;
    if (traceLen > sizeof trace - 64) {
        printf("  FAIL trace overflow\n");
        exit(1);
    }
    va_start(ap, fmt);
    traceLen += vsnprintf(trace + traceLen, sizeof trace - traceLen, fmt, ap);
    va_end(ap);
    traceLen += snprintf(trace + traceLen, sizeof trace - traceLen, " ");
}

static LorieBuffer *buffers[64];
static int nbuffers;
static LorieBuffer *newBuffer(int type, int width, int height) {
    LorieBuffer *b = calloc(1, sizeof *b);
    b->desc = (LorieBuffer_Desc) { type, (unsigned long long) nbuffers, width, height, width };
    b->mem = calloc((size_t) width * height, 4);
    buffers[nbuffers++] = b;
    return b;
}

static struct fake_pvfb *checked;   /* this branch's world, on which the ownership invariants are checked */
static const char *runName;
static int opIndex;
static uint8_t answered(void) {
    return LORIE_OWNERSHIP_MODE(checked->state->backend.ownershipAck);
}
static void rendererView(const char *where);

static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) {
    static const LorieBuffer_Desc none = {0};
    return b ? &b->desc : &none;
}
static int LorieBuffer_hasGpuCopyPending(LorieBuffer *b) { return b && b->gpuCopyPending; }
static LorieBuffer *LorieBuffer_allocate(int width, int height, __unused int format, int type) {
    LorieBuffer *b = newBuffer(type, width, height);
    ev("A%llu", b->desc.id);
    return b;
}
static void LorieBuffer_release(LorieBuffer *b) {
    b->released = 1;
    ev("R%llu", b->desc.id);
}
static void lorieUnregisterBuffer(__unused LorieBuffer *b) {}
static void LorieBuffer_convert(LorieBuffer *b, int8_t type, __unused int8_t format) {
    if (!b || b->desc.type != LORIEBUFFER_REGULAR)
        return;
    b->desc.type = type;            /* as buffer.c: the old memory, and any lock of it, are gone */
    b->locked = 0;
    b->cpuHeld = 0;
    b->lockedData = NULL;
    ev("V%llu", b->desc.id);
}
static int LorieBuffer_lock(LorieBuffer *b, void **out) {
    int ret = 0;
    if (!b)
        return ENODEV;
    CHECK(!b->released, "%s op %d: CPU lock of released buffer %llu", runName, opIndex, b->desc.id);
    if (b->locked) {                /* as buffer.c */
        ev("L%llu!", b->desc.id);
        if (out)
            *out = b->lockedData;
        return EEXIST;
    }
    if (checked && b->desc.type == LORIEBUFFER_AHARDWAREBUFFER && answered() == LORIE_OWNERSHIP_PER_ACCESS)
        CHECK(checked->state->lock > 0, "%s op %d: AHB %llu CPU-locked outside the shared lock while PER_ACCESS is answered",
              runName, opIndex, b->desc.id);
    b->lockAttempts++;
    if (b->failLock)
        ret = EIO, b->lockedData = NULL;
    else
        b->lockedData = b->mem, b->cpuHeld = 1;
    if (out)
        *out = b->lockedData;
    b->locked = 1;                  /* as buffer.c, also when AHardwareBuffer_lock failed */
    ev("L%llu", b->desc.id);
    return ret;
}
static int LorieBuffer_unlock(LorieBuffer *b) {
    if (!b)
        return ENODEV;
    if (!b->locked) {
        ev("U%llu!", b->desc.id);
        return ENOENT;
    }
    b->locked = 0;
    b->cpuHeld = 0;
    b->lockedData = NULL;
    ev("U%llu", b->desc.id);
    return 0;
}
static void lorie_mutex_lock(int *m, __unused int *pid) {
    ++*m;
    ev("M+");
}
static void lorie_mutex_unlock(int *m, __unused int *pid) {
    CHECK(*m > 0, "%s op %d: shared lock let go while not held", runName, opIndex);
    ev("M-");
    if (*m > 0 && --*m == 0 && checked && m == &checked->state->lock)
        rendererView("shared lock let go");
}
static uint64_t fakeNow;
static uint64_t GetTimeInMicros(void) { return fakeNow += 3; }
static Bool (*queuedProc)(ClientPtr, void *);
static int wakes, condSignals;
static Bool QueueWorkProc(Bool (*proc)(ClientPtr, void *), __unused ClientPtr client, __unused void *closure) {
    queuedProc = proc;
    return TRUE;
}
static void lorieWakeServer(void) { wakes++; }
static pthread_cond_t *rendererCond;
#define pthread_cond_signal(c) ((void) (c), condSignals++)
/* a freed pixmap private is kept, never reused, so one still on the list can be seen without touching freed memory */
static void *freed[256];
static int nfreed;
static void fakeFree(void *p) {
    if (p && nfreed < 256)
        freed[nfreed++] = p;
}
static bool wasFreed(void *p) {
    for (int i = 0; i < nfreed; i++)
        if (freed[i] == p)
            return true;
    return false;
}
#define free(p) fakeFree(p)

/* ---- 3c98ffa ---- */
#define pvfb basePvfb
#define LoriePixmapPriv BasePixmapPriv
#define lorieEnsureGpuSampleable base_lorieEnsureGpuSampleable
#define lorieRootWindowPixmapPriv base_lorieRootWindowPixmapPriv
#define drawSquare base_drawSquare
#define drawSquares base_drawSquares
#define lorieSetWindowPixmap base_lorieSetWindowPixmap
#define lorieCreatePixmap base_lorieCreatePixmap
#define lorieExaDestroyPixmap base_lorieExaDestroyPixmap
#define lorieNeedsGpuLock base_lorieNeedsGpuLock
#define loriePrepareAccess base_loriePrepareAccess
#define lorieFinishAccess base_lorieFinishAccess
static struct fake_pvfb baseWorld, *basePvfb = &baseWorld;
#include "own_base_src.inc"
static LorieBuffer **base_bufferOf(void *priv) { return &((LoriePixmapPriv *) priv)->buffer; }
#undef pvfb
#undef LoriePixmapPriv
#undef lorieEnsureGpuSampleable
#undef lorieRootWindowPixmapPriv
#undef drawSquare
#undef drawSquares
#undef lorieSetWindowPixmap
#undef lorieCreatePixmap
#undef lorieExaDestroyPixmap
#undef lorieNeedsGpuLock
#undef loriePrepareAccess
#undef lorieFinishAccess
#undef LORIE_PIXMAP_PRIV_FROM_PIXMAP

/* ---- this branch ---- */
static struct fake_pvfb newWorld, *pvfb = &newWorld;
#include "own_new_src.inc"
static LorieBuffer **new_bufferOf(void *priv) { return &((LoriePixmapPriv *) priv)->buffer; }

/* What a renderer frame would find, taking the shared lock now. */
static void rendererView(const char *where) {
    LoriePixmapPriv *priv;
    int i;
    if (!checked)
        return;
    xorg_list_for_each_entry(priv, &checked->ahbPersistent, persistentLink)
        CHECK(!wasFreed(priv), "%s op %d, %s: a destroyed pixmap is still remembered", runName, opIndex, where);
    if (answered() == LORIE_OWNERSHIP_PER_ACCESS) {
        for (i = 0; i < nbuffers; i++)
            if (!buffers[i]->released && buffers[i]->desc.type == LORIEBUFFER_AHARDWAREBUFFER)
                CHECK(!buffers[i]->cpuHeld, "%s op %d, %s: AHB %llu CPU-locked though PER_ACCESS is answered",
                      runName, opIndex, where, buffers[i]->desc.id);
    } else
        xorg_list_for_each_entry(priv, &checked->ahbPersistent, persistentLink)
            CHECK(priv->locked && priv->buffer->cpuHeld,
                  "%s op %d, %s: AHB %llu, kept CPU-locked by 3c98ffa, is not though PERSISTENT is answered",
                  runName, opIndex, where, priv->buffer->desc.id);
}

static PixmapPtr fakeGetWindowPixmap(WindowPtr w) { return w->pixmap; }
static void fakeSetWindowPixmap(WindowPtr w, PixmapPtr p) { w->pixmap = p; }
static PixmapPtr fakeGetScreenPixmap(ScreenPtr s) { return s->devPrivate; }
static Bool fakeModifyPixmapHeader(__unused PixmapPtr p, __unused int w, __unused int h, __unused int d,
                                   __unused int bpp, __unused int devKind, __unused void *data) {
    ev("H");
    return TRUE;
}

struct impl {
    const char *name;
    struct fake_pvfb *w;
    bool ownership;
    void *(*create)(ScreenPtr, int, int, int, int, int, int *);
    void (*destroy)(ScreenPtr, void *);
    Bool (*prepare)(PixmapPtr, int);
    void (*finish)(PixmapPtr, int);
    void (*setWindowPixmap)(WindowPtr, PixmapPtr);
    LorieBuffer *(*ensure)(PixmapPtr, int8_t);
    Bool (*draw)();
    LorieBuffer **(*bufferOf)(void *);
};
static const struct impl base = { "3c98ffa", &baseWorld, false, base_lorieCreatePixmap, base_lorieExaDestroyPixmap,
    base_loriePrepareAccess, base_lorieFinishAccess, base_lorieSetWindowPixmap, base_lorieEnsureGpuSampleable,
    base_drawSquares, base_bufferOf };
static const struct impl cur = { "this branch", &newWorld, true, lorieCreatePixmap, lorieExaDestroyPixmap,
    loriePrepareAccess, lorieFinishAccess, lorieSetWindowPixmap, lorieEnsureGpuSampleable, drawSquares, new_bufferOf };

enum {
    CREATE, CREATE_REGULAR, IMPORT, SCREEN, DEST, SRC, ENSURE, DRAW, ROOT, DESTROY, PENDING, NOT_PENDING,
    ROOT_PENDING, ROOT_DONE, FAIL_SRC,
    /* the renderer's side of the handshake, and its connection: nothing to 3c98ffa */
    ASK_PER_ACCESS, ASK_PERSISTENT, ASK_INVALID, ASK_NOTHING_NEW, DISCONNECT, CONNECT,
};
#define OWNERSHIP_OP(what) ((what) >= ASK_PER_ACCESS)
struct op { int what, pix; };

#define NPIX 16
#define MAXOPS 160
static PixmapRec pix[NPIX];
static char slices[2][MAXOPS][256], lockSets[2][MAXOPS][65];
static int perAccessAcquires;

static void ask(struct fake_pvfb *w, uint8_t mode) {
    uint32_t seq = LORIE_OWNERSHIP_SEQ(w->state->backend.ownershipAck) + 1;
    w->state->backend.ownershipRequest = LORIE_OWNERSHIP_WORD(seq, mode);
    lorieProcessOwnershipRequest();                         /* as lorieRedraw does, every frame */
    CHECK(LORIE_OWNERSHIP_SEQ(w->state->backend.ownershipAck) == seq, "%s op %d: request %u not answered",
          runName, opIndex, seq);
    CHECK(LORIE_OWNERSHIP_MODE(w->state->backend.ownershipAck) ==
          (mode == LORIE_OWNERSHIP_PER_ACCESS && w->state->backend.requested != LORIE_BACKEND_GLES
           ? LORIE_OWNERSHIP_PER_ACCESS : LORIE_OWNERSHIP_PERSISTENT),
          "%s op %d: request for mode %u answered with %u", runName, opIndex, mode,
          LORIE_OWNERSHIP_MODE(w->state->backend.ownershipAck));
}

static void exec(const struct impl *im, struct op o) {
    struct fake_pvfb *w = im->w;
    PixmapPtr p = &pix[o.pix];
    LorieBuffer *b = p->driverPriv ? *im->bufferOf(p->driverPriv) : NULL;
    bool perAccess = im->ownership && answered() == LORIE_OWNERSHIP_PER_ACCESS && b &&
                     b->desc.type == LORIEBUFFER_AHARDWAREBUFFER;
    int pitch, transitions = w->state->diag.xOwnershipTransitions, signals = condSignals;
    Bool ok;

    switch (o.what) {
    case CREATE:
    case CREATE_REGULAR:
        p->driverPriv = im->create(&screen, 64, 64, 24, o.what == CREATE ? CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED : 0,
                                   32, &pitch);
        break;
    case IMPORT:    /* DRI3: a pixmap without memory, given a client's AHB */
        p->driverPriv = im->create(&screen, 0, 0, 24, 0, 32, &pitch);
        *im->bufferOf(p->driverPriv) = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 64, 64);
        break;
    case SCREEN:
        screen.devPrivate = p;
        rootWin.pixmap = p;
        break;
    case DEST:
    case SRC:
    case FAIL_SRC: {
        int index = o.what == DEST ? EXA_PREPARE_DEST : EXA_PREPARE_SRC;
        int attempts = b->lockAttempts;
        if (o.what == FAIL_SRC) {
            if (!perAccess)
                break;
            b->failLock = 1;
        }
        ok = im->prepare(p, index);
        if (o.what == FAIL_SRC) {
            CHECK(!ok && b->lockAttempts == attempts + 1, "%s op %d: PrepareAccess did not fail on a buffer that cannot be locked",
                  runName, opIndex);
            CHECK(w->state->lock == 0, "%s op %d: failed PrepareAccess left the shared lock held", runName, opIndex);
            b->failLock = 0;
            break;
        }
        CHECK(ok && p->devPrivate.ptr, "%s op %d: PrepareAccess gave no pixels", runName, opIndex);
        if (!ok)
            break;
        if (perAccess) {
            CHECK(w->state->lock == 1, "%s op %d: CPU access to an AHB under PER_ACCESS without the shared lock",
                  runName, opIndex);
            perAccessAcquires++;
        }
        if (p->devPrivate.ptr)
            ((uint32_t *) p->devPrivate.ptr)[0] ^= 1;      /* the access */
        im->finish(p, index);
        p->devPrivate.ptr = NULL;                           /* EXA hides it again */
        CHECK(w->state->lock == 0, "%s op %d: shared lock still held after FinishAccess", runName, opIndex);
        break;
    }
    case ENSURE:
        CHECK(im->ensure(p, LORIEBUFFER_AHARDWAREBUFFER) == b, "%s op %d: pixmap not made GPU-sampleable", runName, opIndex);
        break;
    case DRAW: {
        LorieBuffer *sb = *im->bufferOf(((PixmapPtr) screen.devPrivate)->driverPriv);
        uint32_t *at = sb->mem + 29 * sb->desc.stride + 29;   /* where the last square starts, on 64x64 */
        bool drawn = rootWin.pixmap == screen.devPrivate;      /* 3c98ffa: only while it keeps it CPU-locked */
        *at = 0;
        if (drawn && im->ownership && answered() == LORIE_OWNERSHIP_PER_ACCESS)
            perAccessAcquires++;
        im->draw();
        CHECK((*at == 0x000000FF) == drawn, "%s op %d: drawSquares %s", runName, opIndex,
              drawn ? "did not draw" : "drew into a pixmap 3c98ffa does not draw into");
        CHECK(w->state->lock == 0, "%s op %d: shared lock still held after drawSquares", runName, opIndex);
        break;
    }
    case ROOT:
        im->setWindowPixmap(&rootWin, p);
        break;
    case DESTROY:
        im->destroy(&screen, p->driverPriv);
        p->driverPriv = NULL;
        break;
    case PENDING:
    case NOT_PENDING:
        b->gpuCopyPending = o.what == PENDING;
        break;
    case ROOT_PENDING:
    case ROOT_DONE:
        w->rootGpuCopyPending = o.what == ROOT_PENDING;
        break;
    }

    if (!im->ownership)
        return;
    switch (o.what) {
    case ASK_PER_ACCESS:
        ask(w, LORIE_OWNERSHIP_PER_ACCESS);
        break;
    case ASK_PERSISTENT:
        ask(w, LORIE_OWNERSHIP_PERSISTENT);
        break;
    case ASK_INVALID: {
        unsigned refused = w->state->diag.xOwnershipRefused;
        ask(w, 7);
        CHECK(w->state->diag.xOwnershipRefused == refused + 1, "%s op %d: unknown mode not refused", runName, opIndex);
        break;
    }
    case ASK_NOTHING_NEW:
        lorieProcessOwnershipRequest();
        CHECK(w->state->diag.xOwnershipTransitions == transitions && condSignals == signals,
              "%s op %d: an answered request taken up again", runName, opIndex);
        break;
    case DISCONNECT: {
        uint32_t req = w->state->backend.ownershipRequest;
        int woken = wakes;
        queuedProc = NULL;
        lorieRendererDisconnected();                        /* input thread */
        CHECK(queuedProc && wakes == woken + 1, "%s op %d: disconnect not handed to the main thread", runName, opIndex);
        if (queuedProc)
            queuedProc(NULL, NULL);                         /* main thread */
        CHECK(w->state->backend.ownershipAck == LORIE_OWNERSHIP_WORD(LORIE_OWNERSHIP_SEQ(req), LORIE_OWNERSHIP_PERSISTENT),
              "%s op %d: disconnect did not answer PERSISTENT to request %u", runName, opIndex, LORIE_OWNERSHIP_SEQ(req));
        break;
    }
    case CONNECT: {
        uint32_t req = w->state->backend.ownershipRequest;
        lorieResetOwnership();                              /* lorieActivityConnected */
        CHECK(w->state->backend.ownershipAck == LORIE_OWNERSHIP_WORD(LORIE_OWNERSHIP_SEQ(req), LORIE_OWNERSHIP_PERSISTENT),
              "%s op %d: new connection did not answer PERSISTENT to request %u", runName, opIndex, LORIE_OWNERSHIP_SEQ(req));
        break;
    }
    }
    CHECK(w->ownership == answered() && w->state->diag.xOwnership == answered(),
          "%s op %d: mode in effect %u, logged %u, answered %u", runName, opIndex, w->ownership,
          w->state->diag.xOwnership, answered());
}

static void run(int slot, const struct impl *im, const struct op *ops, int n, uint8_t requested) {
    struct fake_state *st = calloc(1, sizeof *st);
    int i, j;

    memset(pix, 0, sizeof pix);
    nbuffers = 0;
    nfreed = 0;
    traceLen = 0;
    perAccessAcquires = 0;
    screen = (ScreenRec) { &rootWin, NULL, fakeGetWindowPixmap, NULL, fakeGetScreenPixmap, fakeModifyPixmapHeader };
    rootWin.pixmap = NULL;
    *im->w = (struct fake_pvfb) { .state = st, .SetWindowPixmap = fakeSetWindowPixmap };
    xorg_list_init(&im->w->ahbPersistent);
    st->backend.requested = requested;
    checked = im->ownership ? im->w : NULL;
    runName = im->name;
    for (i = 0; i < n; i++) {
        size_t start = traceLen;
        opIndex = i;
        exec(im, ops[i]);
        snprintf(slices[slot][i], sizeof slices[slot][i], "%s", trace + start);
        for (j = 0; j < nbuffers; j++)
            lockSets[slot][i][j] = buffers[j]->released ? 'x' : buffers[j]->cpuHeld ? 'L' : '-';
        lockSets[slot][i][j] = 0;
        rendererView("between requests");
    }
    checked = NULL;
}

/* Where 3c98ffa's semantics must hold on this branch: every operation outside [from, to), and in it every
 * buffer that is not an AHB. */
static void compare(const char *scenario, const struct op *ops, int n, int from, int to) {
    int i, j;
    for (i = 0; i < n; i++) {
        if (OWNERSHIP_OP(ops[i].what))
            continue;
        if (i < from || i >= to) {
            CHECK(!strcmp(slices[0][i], slices[1][i]), "%s op %d: 3c98ffa made \"%s\", this branch \"%s\"",
                  scenario, i, slices[0][i], slices[1][i]);
            CHECK(!strcmp(lockSets[0][i], lockSets[1][i]), "%s op %d: CPU locks 3c98ffa %s, this branch %s",
                  scenario, i, lockSets[0][i], lockSets[1][i]);
        } else
            for (j = 0; lockSets[0][i][j]; j++)
                if (buffers[j]->desc.type != LORIEBUFFER_AHARDWAREBUFFER)
                    CHECK(lockSets[0][i][j] == lockSets[1][i][j], "%s op %d: non-AHB buffer %d: 3c98ffa %c, this branch %c",
                          scenario, i, j, lockSets[0][i][j], lockSets[1][i][j]);
    }
}

static const struct op A[] = {      /* 3c98ffa's own: the screen, plain and backed pixmaps, an import */
    {CREATE, 0}, {SCREEN, 0}, {CREATE_REGULAR, 1}, {CREATE, 2}, {CREATE_REGULAR, 3}, {IMPORT, 4},
    {DEST, 0}, {SRC, 0}, {ROOT_PENDING}, {SRC, 0}, {ROOT_DONE},
    {SRC, 2}, {PENDING, 2}, {SRC, 2}, {DEST, 2}, {NOT_PENDING, 2},
    {ENSURE, 1}, {DEST, 1}, {SRC, 3}, {DEST, 3}, {SRC, 4}, {PENDING, 4}, {DEST, 4}, {NOT_PENDING, 4},
    {DRAW}, {ROOT, 2}, {DEST, 0}, {SRC, 2}, {ROOT, 0},
};
static const struct op B[] = {      /* the same kinds of operation, and pixmaps made, converted, freed */
    {DEST, 0}, {SRC, 0}, {ROOT_PENDING}, {SRC, 0}, {ROOT_DONE}, {SRC, 2}, {DEST, 1}, {SRC, 3}, {DEST, 4},
    {CREATE, 5}, {DEST, 5}, {CREATE_REGULAR, 6}, {ENSURE, 6}, {SRC, 6}, {CREATE_REGULAR, 7},
    {DRAW}, {ROOT, 2}, {DEST, 2}, {SRC, 0}, {DRAW}, {ROOT, 0}, {DESTROY, 5}, {IMPORT, 8}, {SRC, 8},
};
static const struct op C[] = {      /* afterwards: everything again, pixmaps from both phases */
    {DEST, 0}, {SRC, 0}, {ROOT_PENDING}, {SRC, 0}, {ROOT_DONE}, {SRC, 2}, {PENDING, 2}, {SRC, 2}, {NOT_PENDING, 2},
    {DEST, 1}, {SRC, 3}, {DEST, 4}, {SRC, 6}, {PENDING, 6}, {DEST, 6}, {NOT_PENDING, 6}, {SRC, 7}, {SRC, 8},
    {DRAW}, {ROOT, 6}, {DEST, 6}, {SRC, 0}, {DRAW}, {ROOT, 0}, {ENSURE, 7}, {DEST, 7},
    {CREATE, 9}, {SRC, 9}, {DESTROY, 2}, {DESTROY, 6}, {DESTROY, 9},
};

static int build(struct op *ops, int mid, int end) {
    int n = 0;
    memcpy(ops + n, A, sizeof A), n += sizeof A / sizeof *A;
    if (mid >= 0)
        ops[n++] = (struct op) { mid };
    memcpy(ops + n, B, sizeof B), n += sizeof B / sizeof *B;
    if (end >= 0)
        ops[n++] = (struct op) { end };
    memcpy(ops + n, C, sizeof C), n += sizeof C / sizeof *C;
    return n;
}

/* A, then `mid`, B, `end`, C - against 3c98ffa doing A, B, C. Returns this branch's PER_ACCESS CPU accesses. */
static int scenario(const char *name, uint8_t requested, int mid, int end, bool window) {
    struct op ops[MAXOPS];
    int n = build(ops, mid, end), nA = sizeof A / sizeof *A, nB = sizeof B / sizeof *B;
    run(0, &base, ops, n, requested);
    run(1, &cur, ops, n, requested);
    compare(name, ops, n, window ? nA : n, window ? nA + 1 + nB + 1 : n);
    return perAccessAcquires;
}

int main(void) {
    struct lorie_renderer_diag *d;      /* each run has its own state: rebound after it */
    uint8_t invalid;
    int acquires, signals;
    struct op ops[MAXOPS];
    int n;

    /* nothing asked: 3c98ffa from start to end */
    scenario("unasked", LORIE_BACKEND_AUTO, -1, -1, false);
    d = &newWorld.state->diag;
    CHECK(d->xOwnershipTransitions == 0 && d->xAhbCpuAcquires == 0, "unasked: ownership moved");

    /* Vulkan takes PER_ACCESS, then gives it back (fallback): 3c98ffa before and after */
    signals = condSignals;
    acquires = scenario("vulkan, then fallback", LORIE_BACKEND_AUTO, ASK_PER_ACCESS, ASK_PERSISTENT, true);
    d = &newWorld.state->diag;
    CHECK(acquires > 0 && d->xAhbCpuAcquires == (unsigned long long) acquires,
          "vulkan, then fallback: %llu AHB CPU accesses counted, %d made", (unsigned long long) d->xAhbCpuAcquires, acquires);
    CHECK(d->xStateLockWaitUs > 0, "vulkan, then fallback: no shared-lock wait recorded");
    CHECK(d->xOwnershipTransitions == 2 && d->xOwnershipRefused == 0 && condSignals == signals + 2,
          "vulkan, then fallback: %u transitions, %u refused, %d answers signalled", d->xOwnershipTransitions,
          d->xOwnershipRefused, condSignals - signals);
    CHECK(newWorld.state->backend.ownershipAck == LORIE_OWNERSHIP_WORD(2, LORIE_OWNERSHIP_PERSISTENT),
          "vulkan, then fallback: last answer %#x", newWorld.state->backend.ownershipAck);

    /* the same asked for by name */
    scenario("vulkan requested, then fallback", LORIE_BACKEND_VULKAN, ASK_PER_ACCESS, ASK_PERSISTENT, true);
    d = &newWorld.state->diag;
    CHECK(d->xOwnershipTransitions == 2, "vulkan requested: %u transitions", d->xOwnershipTransitions);

    /* a session asked for gles: PER_ACCESS refused, 3c98ffa throughout */
    scenario("gles requested", LORIE_BACKEND_GLES, ASK_PER_ACCESS, ASK_PERSISTENT, false);
    d = &newWorld.state->diag;
    CHECK(d->xOwnershipTransitions == 0 && d->xOwnershipRefused == 1 && d->xAhbCpuAcquires == 0,
          "gles requested: %u transitions, %u refused", d->xOwnershipTransitions, d->xOwnershipRefused);

    /* the renderer's connection drops, or a new one is made, while PER_ACCESS is in effect */
    scenario("disconnect", LORIE_BACKEND_AUTO, ASK_PER_ACCESS, DISCONNECT, true);
    CHECK(newWorld.state->diag.xOwnershipTransitions == 2, "disconnect: %u transitions", newWorld.state->diag.xOwnershipTransitions);
    scenario("new connection", LORIE_BACKEND_AUTO, ASK_PER_ACCESS, CONNECT, true);
    CHECK(newWorld.state->diag.xOwnershipTransitions == 2, "new connection: %u transitions", newWorld.state->diag.xOwnershipTransitions);

    /* this branch alone: a failed CPU lock under PER_ACCESS, a request taken up twice, an unknown mode */
    n = sizeof A / sizeof *A;
    memcpy(ops, A, sizeof A);
    ops[n++] = (struct op) { ASK_PER_ACCESS };
    ops[n++] = (struct op) { CREATE, 10 };
    ops[n++] = (struct op) { FAIL_SRC, 10 };
    ops[n++] = (struct op) { DESTROY, 10 };
    ops[n++] = (struct op) { ASK_NOTHING_NEW };
    ops[n++] = (struct op) { SRC, 2 };
    ops[n++] = (struct op) { ASK_INVALID };
    ops[n++] = (struct op) { SRC, 2 };
    ops[n++] = (struct op) { DRAW };
    run(1, &cur, ops, n, LORIE_BACKEND_AUTO);
    CHECK(newWorld.state->diag.xOwnershipTransitions == 2, "unknown mode: %u transitions", newWorld.state->diag.xOwnershipTransitions);

    /* TERMUX_X11_RENDERER_BACKEND */
    invalid = 0;
    CHECK(lorieRequestedBackend(NULL, &invalid) == LORIE_BACKEND_AUTO && !invalid, "unset");
    CHECK(lorieRequestedBackend("", &invalid) == LORIE_BACKEND_AUTO && !invalid, "empty");
    CHECK(lorieRequestedBackend("auto", &invalid) == LORIE_BACKEND_AUTO && !invalid, "auto");
    CHECK(lorieRequestedBackend("vulkan", &invalid) == LORIE_BACKEND_VULKAN && !invalid, "vulkan");
    CHECK(lorieRequestedBackend("gles", &invalid) == LORIE_BACKEND_GLES && !invalid, "gles");
    CHECK(lorieRequestedBackend("Vulkan", &invalid) == LORIE_BACKEND_AUTO && invalid, "Vulkan: not flagged");
    invalid = 0;
    CHECK(lorieRequestedBackend("vk", &invalid) == LORIE_BACKEND_AUTO && invalid, "vk: not flagged");

    /* TERMUX_X11_RENDERER_VK_FAIL_AT: the seven points, lowercase; nothing else injects */
    {
        static const struct { const char *value; uint8_t point; bool invalid; } t[] = {
            { NULL, LORIE_VK_FAIL_AT_NONE, false }, { "", LORIE_VK_FAIL_AT_NONE, false },
            { "instance", LORIE_VK_FAIL_AT_INSTANCE, false }, { "device", LORIE_VK_FAIL_AT_DEVICE, false },
            { "surface", LORIE_VK_FAIL_AT_SURFACE, false }, { "swapchain", LORIE_VK_FAIL_AT_SWAPCHAIN, false },
            { "import", LORIE_VK_FAIL_AT_IMPORT, false }, { "submit", LORIE_VK_FAIL_AT_SUBMIT, false },
            { "present", LORIE_VK_FAIL_AT_PRESENT, false }, { "none", LORIE_VK_FAIL_AT_NONE, true },
            { "Instance", LORIE_VK_FAIL_AT_NONE, true }, { "acquire", LORIE_VK_FAIL_AT_NONE, true },
        };
        for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
            uint8_t point;
            invalid = 0;
            point = lorieRequestedVkFailAt(t[k].value, &invalid);
            CHECK(point == t[k].point && invalid == t[k].invalid, "TERMUX_X11_RENDERER_VK_FAIL_AT=%s: point %u, flagged %u",
                  t[k].value ? t[k].value : "(unset)", point, invalid);
        }
    }

    printf("TOWNERSHIP AHB CPU ownership against 3c98ffa's own code: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
