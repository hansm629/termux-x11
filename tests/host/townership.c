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
 *   PERSISTENT - every pixmap 3c98ffa keeps CPU-locked is.
 *   A submitted Vulkan frame (lorie.h gpuInFlight): no AHB it uses is CPU-locked until it is over - an access,
 *   drawSquares, PERSISTENT taken again all wait for it, holding the shared lock; an AHB the frame does not use is
 *   not waited for, the root's (id 0, as buffer.c's first) included. The wait ends when the submitter says the
 *   frame is over, however long that takes, connection or not; or when the submitter no longer exists, and the
 *   list is cleared then.
 *   Root double buffering's swap (lorieRootSwap): the front written only once the frame sampling it is over, only
 *   the damage copied (all of it when out of date), the two buffers swapped and the new front published, both
 *   unlocked again; afterwards an access to the back waits for no frame while the front is being sampled.
 *   P11-B, its own locks (TERMUX_X11_RENDERER_ROOT_COPY_OPT=1): the back locked for reading only, the front for
 *   writing only, each within a rect inside the buffer that holds every box copied - against a lock that, as the
 *   mapper's contract allows, gives poison outside its rect to a read and drops what is written there (and checks
 *   nothing was), or that ignores the rect: pixel for pixel what the swap does with its locks off, for one box, several
 *   (some partly or wholly outside the buffer), the whole buffer, out of date, rows padded, a swap after a swap; with
 *   the frame sampling the front over first; either lock refused, what was locked unlocked, the locks of before taken
 *   instead and kept for the session; every lock unlocked once, none taken twice. */
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

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
#define max(a, b) ((a) > (b) ? (a) : (b))
typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxRec *rects; } RegionRec, *RegionPtr;
typedef struct { int32_t left, top, right, bottom; } ARect;
#define RegionRects(r) ((r)->rects)
#define RegionNumRects(r) ((r)->n)
#define DamageRegion(d) ((RegionPtr) (d))
enum { LORIEBUFFER_REGULAR, LORIEBUFFER_FD, LORIEBUFFER_AHARDWAREBUFFER };

typedef struct { int type; unsigned long long id; int width, height, stride; } LorieBuffer_Desc;
typedef struct LorieBuffer {
    LorieBuffer_Desc desc;
    int locked, released, gpuCopyPending, failLock, lockAttempts;
    int cpuHeld;        /* the AHB's own CPU lock: taken only when locking succeeded (LorieBuffer's `locked` is set
                         * either way, as buffer.c does) */
    void *lockedData;
    uint32_t *mem;
    /* P11-B, LorieBuffer_lockRect: refused, ignoring the rect; how many, the last one's access and rect, and what its
     * address shows when the rect is kept */
    int failLockRect, ignoreRect, rectLocks, lockWrite;
    ARect lockRect;
    uint32_t *shadow;
} LorieBuffer;

typedef struct FakePixmap { struct { void *ptr; } devPrivate; void *driverPriv; struct { int width, height; } drawable; } PixmapRec, *PixmapPtr;
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

#include "gpucopytypes_src.inc"
struct fake_state {
    int lock, lockingPid;
    struct lorie_renderer_diag diag;
    struct lorie_backend_control backend;
    struct { volatile uint32_t busy; volatile pid_t pid; uint32_t count; uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX];
             uint32_t auxCount; uint64_t auxIds[LORIE_GPU_IN_FLIGHT_MAX]; } gpuInFlight;     /* aux: P11-C3-B2 */
    struct { uint32_t count; uint64_t ids[LORIE_CPU_ACCESS_MAX]; volatile uint32_t releases, waiter; } cpuAccess;   /* P11-C3-B1 */
    uint64_t rootWindowTextureID;
    struct {                    /* lorie.h rootCopy (P11-C2) */
        volatile uint32_t seq, taken, cancelled, failed, done;
        volatile pid_t pid;
        uint64_t srcId, dstId, queuedUs;
        uint32_t numRects;
        LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
        volatile uint64_t doneUs;   /* P11-C3 */
        volatile uint32_t waiting;  /* P11-C3-B2 */
    } rootCopy;
};
struct fake_pvfb {
    struct fake_state *state;
    struct { Bool legacyDrawing; } root;
    uint64_t rootGpuCopyPending;
    Bool rootDouble, rootTwinStale;
    Bool rootCopyOpt, rootCopyOptFailed;
    void *damage;               /* a RegionRec here: DamageRegion */
    void (*SetWindowPixmap)(WindowPtr, PixmapPtr);
    uint8_t ownership;
    struct xorg_list ahbPersistent;
    Bool rootGpuCopy;           /* P11-C2: InitOutput.c's own */
    uint32_t rootCopySeq;
    struct {
        uint32_t seq;
        struct LorieBuffer *from, *to;
        int n;
        BoxRec boxes[LORIE_GPU_COPY_MAX_RECTS];
        Bool abandoned;
    } rootCopy;
    int accessLocks;
    Bool frameDiag;             /* P11-C3: TERMUX_X11_RENDERER_VK_FRAME_DIAG */
    struct { uint32_t us; int width, height, index; Bool reserved; } worstAccess;
    Bool exaLockOpt;            /* P11-C3-B1: TERMUX_X11_EXA_LOCK_OPT */
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
static LorieBuffer *newBufferStride(int type, int width, int height, int stride) {
    LorieBuffer *b = calloc(1, sizeof *b);
    b->desc = (LorieBuffer_Desc) { type, (unsigned long long) nbuffers, width, height, stride };
    b->mem = calloc((size_t) stride * height, 4);
    buffers[nbuffers++] = b;
    return b;
}
static LorieBuffer *newBuffer(int type, int width, int height) {
    return newBufferStride(type, width, height, width);
}

static struct fake_pvfb *checked;   /* this branch's world, on which the ownership invariants are checked */
static const char *runName;
static int opIndex;
static uint8_t answered(void) {
    return LORIE_OWNERSHIP_MODE(checked->state->backend.ownershipAck);
}
static void rendererView(const char *where);

/* the submitter of a listed frame: the X server's futex wait on gpuInFlight.busy is where the frame becomes over
 * (the submitter clears the list) once `gpuSlices` 33 ms slices timed out before it */
static int gpuWaits, gpuSlices;
static void rootRenderer(void);
static int rootWaits;
static int releaseWakes;
static long fakeFutexWait(volatile uint32_t *word, int op, uint32_t value) {
    if (op == FUTEX_WAKE) {
        /* P11-C3-B1: a reservation let go, a renderer woken */
        CHECK(checked && word == &checked->state->cpuAccess.releases, "%s op %d: an unexpected futex wake", runName, opIndex);
        releaseWakes++;
        return 0;
    }
    if (checked && word == &checked->state->rootCopy.done) {
        /* P11-C2: the X server waiting for a root copy - the renderer's moment; P11-C3-B2: saying so */
        CHECK(op == FUTEX_WAIT && *word == value, "%s: not a futex wait on the root copy's word", runName);
        CHECK(checked->state->rootCopy.waiting == checked->state->rootCopy.seq, "%s: waits for root copy %u, says %u", runName,
              checked->state->rootCopy.seq, checked->state->rootCopy.waiting);
        if (++rootWaits > 200) {
            CHECK(0, "%s: a wait for a root copy that does not end", runName);
            checked->state->rootCopy.failed = checked->state->rootCopy.done = checked->state->rootCopy.seq;
        }
        rootRenderer();
        return 0;
    }
    gpuWaits++;
    CHECK(op == FUTEX_WAIT && *word == value, "%s op %d: not a futex wait on the list's word", runName, opIndex);
    if (checked)
        CHECK(checked->state->lock > 0, "%s op %d: waited for a Vulkan frame without the shared lock", runName, opIndex);
    if (gpuWaits > 2000) {
        CHECK(0, "%s op %d: a wait for a Vulkan frame that does not end", runName, opIndex);
        *word = 0;
        return 0;
    }
    if (gpuSlices > 0) {
        gpuSlices--;
        errno = ETIMEDOUT;
        return -1;
    }
    *word = 0;
    return 0;
}
#define syscall(nr, word, op, value, timeout, a, b) ((void) (nr), (void) (timeout), fakeFutexWait(word, op, value))
#define lorie_shared_server_state fake_state
#include "inflight_src.inc"
#undef lorie_shared_server_state

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
/* P11-C2: a renderer process that took a root copy and is gone (a child reaped), and whether a root copy that is not
 * settled - not done, not taken back, its renderer not gone - uses buffer `b`: the X server CPU-locks no such one */
static pid_t rootDeadPid;
static bool rootCopyUnsettled(LorieBuffer *b) {
    struct fake_state *st;
    uint32_t seq;
    if (!checked || !b)
        return false;
    st = checked->state;
    seq = st->rootCopy.seq;
    return seq && seq != st->rootCopy.done && seq != st->rootCopy.cancelled &&
           !(st->rootCopy.taken == seq && rootDeadPid && st->rootCopy.pid == rootDeadPid) &&
           (b->desc.id == st->rootCopy.srcId || b->desc.id == st->rootCopy.dstId);
}
static int LorieBuffer_lock(LorieBuffer *b, void **out) {
    int ret = 0;
    if (!b)
        return ENODEV;
    CHECK(!rootCopyUnsettled(b), "%s op %d: AHB %llu CPU-locked while a root copy using it is not settled", runName, opIndex,
          b->desc.id);
    CHECK(!b->released, "%s op %d: CPU lock of released buffer %llu", runName, opIndex, b->desc.id);
    if (b->locked) {                /* as buffer.c */
        ev("L%llu!", b->desc.id);
        if (out)
            *out = b->lockedData;
        return EEXIST;
    }
    /* P11-C3-B1: or reserved (cpuAccess) - never while a submitted frame uses it */
    if (checked && b->desc.type == LORIEBUFFER_AHARDWAREBUFFER && answered() == LORIE_OWNERSHIP_PER_ACCESS)
        CHECK((checked->state->lock > 0 || lorieCpuReserved(checked->state, b->desc.id)) &&
              !(lorieCpuReserved(checked->state, b->desc.id) && lorieGpuInFlight(checked->state, b->desc.id)),
              "%s op %d: AHB %llu CPU-locked outside the shared lock while PER_ACCESS is answered",
              runName, opIndex, b->desc.id);
    if (checked && b->desc.type == LORIEBUFFER_AHARDWAREBUFFER)
        CHECK(!lorieGpuInFlight(checked->state, b->desc.id), "%s op %d: AHB %llu CPU-locked while a submitted Vulkan frame uses it",
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
/* P11-B: as buffer.c - the address the buffer's own, marked locked only once locked; outside the rect, what the
 * mapper's contract leaves undefined made visible: a read gives poison, a write is lost (and caught at the unlock) */
#define POISON 0xdeadbeefu
static bool inRect(const ARect *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static int LorieBuffer_lockRect(LorieBuffer *b, bool write, const ARect *rect, void **out) {
    int w, h, s;
    if (!b)
        return ENODEV;
    CHECK(!rootCopyUnsettled(b), "%s op %d: AHB %llu CPU-locked while a root copy using it is not settled", runName, opIndex,
          b->desc.id);
    CHECK(!b->released, "%s op %d: CPU lock of released buffer %llu", runName, opIndex, b->desc.id);
    if (b->locked) {
        ev("l%llu!", b->desc.id);
        return EEXIST;
    }
    /* P11-C3-B1: or reserved (cpuAccess) - never while a submitted frame uses it */
    if (checked && b->desc.type == LORIEBUFFER_AHARDWAREBUFFER && answered() == LORIE_OWNERSHIP_PER_ACCESS)
        CHECK((checked->state->lock > 0 || lorieCpuReserved(checked->state, b->desc.id)) &&
              !(lorieCpuReserved(checked->state, b->desc.id) && lorieGpuInFlight(checked->state, b->desc.id)),
              "%s op %d: AHB %llu CPU-locked outside the shared lock while PER_ACCESS is answered",
              runName, opIndex, b->desc.id);
    if (checked && b->desc.type == LORIEBUFFER_AHARDWAREBUFFER)
        CHECK(!lorieGpuInFlight(checked->state, b->desc.id), "%s op %d: AHB %llu CPU-locked while a submitted Vulkan frame uses it",
              runName, opIndex, b->desc.id);
    w = b->desc.width, h = b->desc.height, s = b->desc.stride;
    CHECK(!rect || (rect->left >= 0 && rect->top >= 0 && rect->right <= w && rect->bottom <= h && rect->left < rect->right &&
                    rect->top < rect->bottom), "%s: AHB %llu locked with a rect outside it, or empty", runName, b->desc.id);
    b->rectLocks++;
    if (b->failLockRect) {
        ev("l%llux", b->desc.id);
        return EIO;
    }
    b->lockWrite = write;
    b->lockRect = rect ? *rect : (ARect) { 0, 0, w, h };
    b->lockedData = b->mem;
    if (rect && !b->ignoreRect) {
        b->shadow = malloc((size_t) s * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < s; x++)
                b->shadow[y * s + x] = x < w && inRect(rect, x, y) ? b->mem[y * s + x] : POISON;
        b->lockedData = b->shadow;
    }
    b->locked = b->cpuHeld = 1;
    *out = b->lockedData;
    ev("l%llu%c", b->desc.id, write ? 'w' : 'r');
    return 0;
}
static int LorieBuffer_unlock(LorieBuffer *b) {
    if (!b)
        return ENODEV;
    if (!b->locked) {
        ev("U%llu!", b->desc.id);
        return ENOENT;
    }
    if (b->shadow) {
        int w = b->desc.width, h = b->desc.height, s = b->desc.stride, outside = 0, readWritten = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < s; x++) {
                uint32_t v = b->shadow[y * s + x];
                if (!(x < w && inRect(&b->lockRect, x, y)))
                    outside += v != POISON;
                else if (b->lockWrite)
                    b->mem[y * s + x] = v;
                else
                    readWritten += v != b->mem[y * s + x];
            }
        CHECK(!outside && !readWritten, "%s: AHB %llu written outside its lock's rect (%d pixels), or while locked for reading (%d)",
              runName, b->desc.id, outside, readWritten);
        free(b->shadow);
        b->shadow = NULL;
    }
    b->locked = 0;
    b->cpuHeld = 0;
    b->lockedData = NULL;
    ev("U%llu", b->desc.id);
    return 0;
}
/* P11-C2: the renderer's side of a root copy the X server asked for (rootCopy), as a case sets it - what it does when
 * the X server waits for it: after `rootRendererWaits` waits (having taken it at the first, with `rootRendererTakes`),
 * copy it - each rectangle's pixels from the request's source into its destination, as its GPU would - fail it, or
 * take it and be gone. A request no renderer took must not be waited for with the shared lock held: the renderer
 * takes it under that lock. */
enum { ROOT_COPIES, ROOT_FAILS, ROOT_TAKES_AND_DIES };
static int rootRendererDoes, rootRendererWaits, rootRendererCalls;
static bool rootRendererTakes;
static LorieBuffer *bufferWithId(uint64_t id) {
    for (int i = 0; i < nbuffers; i++)
        if (buffers[i]->desc.id == id)
            return buffers[i];
    return NULL;
}
static void rootRenderer(void) {
    struct fake_state *st = checked->state;
    uint32_t seq = st->rootCopy.seq;
    LorieBuffer *src = bufferWithId(st->rootCopy.srcId), *dst = bufferWithId(st->rootCopy.dstId);
    rootRendererCalls++;
    if (st->rootCopy.taken != seq)
        CHECK(st->lock == 0, "%s: a root copy no renderer took waited for with the shared lock held", runName);
    CHECK(!src || !dst || (!src->cpuHeld && !dst->cpuHeld), "%s: a root copy's buffer CPU-locked while it is waited for", runName);
    if (seq == st->rootCopy.done || seq == st->rootCopy.cancelled)
        return;
    if (rootRendererWaits > 0) {
        rootRendererWaits--;
        if (rootRendererTakes)
            st->rootCopy.taken = seq, st->rootCopy.pid = getpid();
        return;
    }
    if (st->rootCopy.taken == seq && st->rootCopy.pid == rootDeadPid)
        return;
    st->rootCopy.taken = seq;
    st->rootCopy.pid = getpid();
    if (rootRendererDoes == ROOT_TAKES_AND_DIES) {
        st->rootCopy.pid = rootDeadPid;
        return;
    }
    if (rootRendererDoes == ROOT_COPIES && src && dst)
        for (uint32_t k = 0; k < st->rootCopy.numRects; k++) {
            LorieGpuCopyRect r = st->rootCopy.rects[k];
            for (int y = r.y1; y < r.y2; y++)
                memcpy(dst->mem + (size_t) y * dst->desc.stride + r.x1, src->mem + (size_t) y * src->desc.stride + r.x1,
                       (size_t) (r.x2 - r.x1) * 4);
        }
    else
        st->rootCopy.failed = seq;
    if (st->backend.vkFrameDiag)
        st->rootCopy.doneUs = lorieMonotonicUs();   /* as rendererRootCopySettle (P11-C3) */
    st->rootCopy.done = seq;
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
/* P11-C2: a renderer connected, with a surface - as a case sets them */
static bool rootConnection = true, rootSurface = true;
static bool lorieConnectionAlive(void) { return rootConnection; }
static bool lorieRendererAvailable(void) { return rootSurface; }
static Bool (*queuedProc)(ClientPtr, void *);
static int wakes, rendererWakes;
static Bool QueueWorkProc(Bool (*proc)(ClientPtr, void *), __unused ClientPtr client, __unused void *closure) {
    queuedProc = proc;
    return TRUE;
}
static void lorieWakeServer(void) { wakes++; }
/* P10-S1: the X server's GPU copies rechecked (patches/xserver.patch) - the renderer gone, they settle at once */
static int rechecks;
static void lorieRecheckGpuCopies(void) { rechecks++; }
static void *rendererWake;
#define lorieWakeRenderer(w) ((void) (w), rendererWakes++)
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
static void lorieRootCopyWait(Bool takeBack);
#define lorie_shared_server_state fake_state
#include "own_new_src.inc"
#undef lorie_shared_server_state
static int pixman_blt(uint32_t *src, uint32_t *dst, int srcStride, int dstStride, int srcBpp, int dstBpp, int sx, int sy,
                      int dx, int dy, int w, int h) {
    (void) srcBpp; (void) dstBpp;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t) (dy + y) * dstStride + dx, src + (size_t) (sy + y) * srcStride + sx, (size_t) w * 4);
    return 1;
}
#define lorie_shared_server_state fake_state
#include "rootswap_src.inc"
#undef lorie_shared_server_state
static LorieBuffer **new_bufferOf(void *priv) { return &((LoriePixmapPriv *) priv)->buffer; }

/* P11-C2: a root double buffering world for the GPU copy - both buffers alike (base pixels) but for what the X server
 * drew into the back since the last swap (the damage's boxes, drawn pixels); the screen pixmap's private its */
static struct fake_state rootGpuState;
static LoriePixmapPriv rootGpuPriv;
static PixmapRec rootGpuPix;
static RegionRec rootGpuDamage;
static void rootGpuWorld(const char *name, BoxRec *boxes, int n) {
    const int w = 512, h = 520;
    LorieBuffer *back, *front;
    memset(&rootGpuState, 0, sizeof rootGpuState);
    nbuffers = 0;
    back = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, w, h);
    front = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, w, h);
    for (int i = 0; i < w * h; i++)
        back->mem[i] = front->mem[i] = 0x10000u + (uint32_t) i;
    for (int k = 0; k < n; k++)
        for (int y = max(boxes[k].y1, 0); y < min(boxes[k].y2, h); y++)
            for (int x = max(boxes[k].x1, 0); x < min(boxes[k].x2, w); x++)
                back->mem[y * w + x] = 0x900000u + (uint32_t) (y * w + x);
    rootGpuDamage = (RegionRec) { n, boxes };
    newWorld = (struct fake_pvfb) { .state = &rootGpuState, .ownership = LORIE_OWNERSHIP_PER_ACCESS, .damage = &rootGpuDamage,
                                    .rootGpuCopy = TRUE };
    xorg_list_init(&newWorld.ahbPersistent);
    rootGpuState.backend.ownershipAck = rootGpuState.backend.ownershipRequest = LORIE_OWNERSHIP_WORD(1, LORIE_OWNERSHIP_PER_ACCESS);
    rootGpuPriv = (LoriePixmapPriv) { .buffer = back, .twin = front };
    rootGpuPix = (PixmapRec) { { NULL }, &rootGpuPriv };
    screen.devPrivate = &rootGpuPix;
    checked = &newWorld;
    runName = name;
    traceLen = 0;
    trace[0] = 0;
    rootRendererDoes = ROOT_COPIES;
    rootRendererWaits = rootRendererCalls = rootWaits = 0;
    rootRendererTakes = false;
    rootConnection = rootSurface = true;
}
// whether the world's two buffers hold the same pixels
static bool rootGpuAlike(void) {
    return !memcmp(buffers[0]->mem, buffers[1]->mem, (size_t) 512 * 520 * 4);
}

/* P11-B: one swap, its own locks off or on, from the same buffers, damage and staleness; what the front holds then */
struct swapcase {
    const char *name;
    int w, h, stride, n;
    BoxRec box[4];
    Bool stale;
    int ignoreRect, failBack, failFront, failOld, inFlightFront;
};
static struct fake_state swapCaseState;
static LoriePixmapPriv swapCasePriv;
static uint32_t *swapOnce(const struct swapcase *c, Bool opt, Bool *ok) {
    static RegionRec damage;
    LorieBuffer *back, *front;
    uint32_t *out;
    size_t size = (size_t) c->stride * c->h;

    memset(&swapCaseState, 0, sizeof swapCaseState);
    damage = (RegionRec) { c->n, (BoxRec *) c->box };
    nbuffers = 0;
    back = newBufferStride(LORIEBUFFER_AHARDWAREBUFFER, c->w, c->h, c->stride);
    front = newBufferStride(LORIEBUFFER_AHARDWAREBUFFER, c->w, c->h, c->stride);
    for (size_t i = 0; i < size; i++)
        back->mem[i] = 0x10000u + (uint32_t) i, front->mem[i] = 0x50000u + (uint32_t) i;
    newWorld = (struct fake_pvfb) { .state = &swapCaseState, .ownership = LORIE_OWNERSHIP_PER_ACCESS, .damage = &damage,
                                    .rootTwinStale = c->stale, .rootCopyOpt = opt };
    xorg_list_init(&newWorld.ahbPersistent);
    swapCaseState.backend.ownershipAck = LORIE_OWNERSHIP_WORD(1, LORIE_OWNERSHIP_PER_ACCESS);
    swapCasePriv = (LoriePixmapPriv) { .buffer = back, .twin = front };
    back->failLockRect = c->failBack;
    front->failLockRect = c->failFront;
    front->failLock = c->failOld;
    back->ignoreRect = front->ignoreRect = c->ignoreRect;
    if (c->inFlightFront) {
        swapCaseState.gpuInFlight.ids[0] = front->desc.id;
        swapCaseState.gpuInFlight.count = 1;
        swapCaseState.gpuInFlight.pid = getpid();
        swapCaseState.gpuInFlight.busy = 1;
    }
    checked = &newWorld;
    runName = c->name;
    traceLen = 0;
    trace[0] = 0;
    gpuWaits = gpuSlices = 0;
    *ok = lorieRootSwap(&swapCasePriv);
    CHECK(!back->locked && !front->locked && !back->cpuHeld && !front->cpuHeld && swapCaseState.lock == 0,
          "%s (%s): a buffer, or the shared lock, still held", c->name, opt ? "on" : "off");
    CHECK(!strchr(trace, '!'), "%s (%s): a lock taken twice, or nothing unlocked: %s", c->name, opt ? "on" : "off", trace);
    CHECK(*ok ? swapCasePriv.buffer == front && swapCasePriv.twin == back && swapCaseState.rootWindowTextureID == back->desc.id
              : swapCasePriv.buffer == back && swapCasePriv.twin == front,
          "%s (%s): swapped %d, but the buffers say otherwise", c->name, opt ? "on" : "off", *ok);
    CHECK(!c->inFlightFront || gpuWaits == 1, "%s (%s): %d waits for the frame sampling the front (want 1)", c->name,
          opt ? "on" : "off", gpuWaits);
    out = malloc(size * 4);
    memcpy(out, front->mem, size * 4);
    checked = NULL;
    return out;
}

/* What a renderer frame would find, taking the shared lock now. */
static void rendererView(const char *where) {
    LoriePixmapPriv *priv;
    int i;
    if (!checked)
        return;
    xorg_list_for_each_entry(priv, &checked->ahbPersistent, persistentLink)
        CHECK(!wasFreed(priv), "%s op %d, %s: a destroyed pixmap is still remembered", runName, opIndex, where);
    if (answered() == LORIE_OWNERSHIP_PER_ACCESS) {
        /* P11-C3-B1: but for one the X server holds reserved (cpuAccess), which no renderer touches - and which no
         * submitted frame uses, ever */
        for (i = 0; i < nbuffers; i++)
            if (!buffers[i]->released && buffers[i]->desc.type == LORIEBUFFER_AHARDWAREBUFFER)
                CHECK(!buffers[i]->cpuHeld || lorieCpuReserved(checked->state, buffers[i]->desc.id),
                      "%s op %d, %s: AHB %llu CPU-locked though PER_ACCESS is answered", runName, opIndex, where,
                      buffers[i]->desc.id);
        for (i = 0; i < (int) checked->state->cpuAccess.count; i++) {
            CHECK(!lorieGpuInFlight(checked->state, checked->state->cpuAccess.ids[i]),
                  "%s op %d, %s: AHB %llu reserved while a submitted frame uses it", runName, opIndex, where,
                  (unsigned long long) checked->state->cpuAccess.ids[i]);
            CHECK(checked->state->cpuAccess.ids[i] != checked->state->rootWindowTextureID,
                  "%s op %d, %s: the published root reserved", runName, opIndex, where);
        }
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
    IN_FLIGHT,          /* the renderer submits a Vulkan frame using the pixmap's AHB */
};
#define OWNERSHIP_OP(what) ((what) >= ASK_PER_ACCESS)
struct op { int what, pix; };

#define NPIX 16
#define MAXOPS 160
static PixmapRec pix[NPIX];
static char slices[2][MAXOPS][256], lockSets[2][MAXOPS][65];
static int opWaits[MAXOPS];         /* waits for a Vulkan frame each operation made */
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
    int pitch, transitions = w->state->diag.xOwnershipTransitions, signals = rendererWakes;
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
            CHECK(!im->ownership || (w->state->cpuAccess.count == 0 && w->accessLocks == 0),
                  "%s op %d: failed PrepareAccess left a reservation (%u) or an access lock", runName, opIndex,
                  w->state->cpuAccess.count);
            b->failLock = 0;
            break;
        }
        CHECK(ok && p->devPrivate.ptr, "%s op %d: PrepareAccess gave no pixels", runName, opIndex);
        if (!ok)
            break;
        if (perAccess) {
            /* P11-C3-B1: or under a reservation, the shared lock let go - any AHB but the root it publishes */
            if (im->ownership && w->exaLockOpt && b->desc.id != w->state->rootWindowTextureID)
                CHECK(w->state->lock == 0 && w->accessLocks == 0 && lorieCpuReserved(w->state, b->desc.id) && b->cpuHeld,
                      "%s op %d: a reserved access: shared lock %d, reserved %d, CPU-locked %d", runName, opIndex,
                      w->state->lock, lorieCpuReserved(w->state, b->desc.id), b->cpuHeld);
            else
                CHECK(w->state->lock == 1 && (!im->ownership || !lorieCpuReserved(w->state, b->desc.id)),
                      "%s op %d: CPU access to an AHB under PER_ACCESS without the shared lock", runName, opIndex);
            perAccessAcquires++;
        }
        if (p->devPrivate.ptr)
            ((uint32_t *) p->devPrivate.ptr)[0] ^= 1;      /* the access */
        im->finish(p, index);
        p->devPrivate.ptr = NULL;                           /* EXA hides it again */
        CHECK(w->state->lock == 0, "%s op %d: shared lock still held after FinishAccess", runName, opIndex);
        if (im->ownership)
            CHECK(w->state->cpuAccess.count == 0,
                  "%s op %d: a reservation left after FinishAccess (%u)", runName, opIndex, w->state->cpuAccess.count);
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
        CHECK(w->state->diag.xOwnershipTransitions == transitions && rendererWakes == signals,
              "%s op %d: an answered request taken up again", runName, opIndex);
        break;
    case DISCONNECT: {
        uint32_t req = w->state->backend.ownershipRequest;
        int woken = wakes;
        queuedProc = NULL;
        lorieRendererDisconnected();                        /* input thread */
        CHECK(queuedProc && wakes == woken + 1, "%s op %d: disconnect not handed to the main thread", runName, opIndex);
        if (queuedProc) {
            int checked = rechecks;
            queuedProc(NULL, NULL);                         /* main thread */
            CHECK(rechecks == checked + 1, "%s op %d: the renderer gone, its GPU copies not rechecked", runName, opIndex);
        }
        CHECK(w->state->backend.ownershipAck == LORIE_OWNERSHIP_WORD(LORIE_OWNERSHIP_SEQ(req), LORIE_OWNERSHIP_PERSISTENT),
              "%s op %d: disconnect did not answer PERSISTENT to request %u", runName, opIndex, LORIE_OWNERSHIP_SEQ(req));
        break;
    }
    case IN_FLIGHT:
        w->state->gpuInFlight.ids[0] = b->desc.id;
        w->state->gpuInFlight.count = 1;
        w->state->gpuInFlight.pid = getpid();
        w->state->gpuInFlight.busy = 1;
        break;
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

static bool exaLockOpt;     /* P11-C3-B1: this branch's runs with TERMUX_X11_EXA_LOCK_OPT=1 */
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
    if (im->ownership)
        im->w->exaLockOpt = exaLockOpt;
    xorg_list_init(&im->w->ahbPersistent);
    st->backend.requested = requested;
    checked = im->ownership ? im->w : NULL;
    runName = im->name;
    for (i = 0; i < n; i++) {
        size_t start = traceLen;
        opIndex = i;
        opWaits[i] = gpuWaits;
        exec(im, ops[i]);
        opWaits[i] = gpuWaits - opWaits[i];
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

/* the ownership scenarios: this branch against 3c98ffa - with TERMUX_X11_EXA_LOCK_OPT off, then on (P11-C3-B1) */
static void scenarios(void) {
    struct lorie_renderer_diag *d;      /* each run has its own state: rebound after it */
    int acquires, signals;
    struct op ops[MAXOPS];
    int n;

    /* nothing asked: 3c98ffa from start to end */
    scenario("unasked", LORIE_BACKEND_AUTO, -1, -1, false);
    d = &newWorld.state->diag;
    CHECK(d->xOwnershipTransitions == 0 && d->xAhbCpuAcquires == 0, "unasked: ownership moved");

    /* Vulkan takes PER_ACCESS, then gives it back (fallback): 3c98ffa before and after */
    signals = rendererWakes;
    acquires = scenario("vulkan, then fallback", LORIE_BACKEND_AUTO, ASK_PER_ACCESS, ASK_PERSISTENT, true);
    d = &newWorld.state->diag;
    CHECK(acquires > 0 && d->xAhbCpuAcquires == (unsigned long long) acquires,
          "vulkan, then fallback: %llu AHB CPU accesses counted, %d made", (unsigned long long) d->xAhbCpuAcquires, acquires);
    CHECK(d->xStateLockWaitUs > 0, "vulkan, then fallback: no shared-lock wait recorded");
    CHECK(d->xOwnershipTransitions == 2 && d->xOwnershipRefused == 0 && rendererWakes == signals + 2,
          "vulkan, then fallback: %u transitions, %u refused, %d answers signalled", d->xOwnershipTransitions,
          d->xOwnershipRefused, rendererWakes - signals);
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
    /* P11-C3-B1: reserved accesses counted with the option, none without */
    CHECK(exaLockOpt ? newWorld.state->diag.xAccessReserved > 0 : newWorld.state->diag.xAccessReserved == 0,
          "%s: %u reserved accesses", exaLockOpt ? "TERMUX_X11_EXA_LOCK_OPT on" : "off", newWorld.state->diag.xAccessReserved);
}

int main(void) {
    uint8_t invalid;
    struct op ops[MAXOPS];
    int n;

    exaLockOpt = false;
    scenarios();
    exaLockOpt = true;
    scenarios();
    exaLockOpt = false;

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

    /* TERMUX_X11_RENDERER_VK_SELFTEST: full, or the quick one */
    invalid = 0;
    CHECK(lorieRequestedVkSelftest(NULL, &invalid) == 0 && !invalid, "self-test unset");
    CHECK(lorieRequestedVkSelftest("", &invalid) == 0 && !invalid, "self-test empty");
    CHECK(lorieRequestedVkSelftest("full", &invalid) == 1 && !invalid, "self-test full");
    CHECK(lorieRequestedVkSelftest("Full", &invalid) == 0 && invalid, "self-test Full: not flagged");
    invalid = 0;
    CHECK(lorieRequestedVkSelftest("quick", &invalid) == 0 && invalid, "self-test quick: not flagged (unset is quick)");

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

    /* PER_ACCESS with Vulkan frames in flight: the AHB a frame uses waited for, no other */
    {
        static const struct op F[] = {
            {ASK_PER_ACCESS},
            {IN_FLIGHT, 2}, {SRC, 0}, {DEST, 4},    /* the root (id 0) and pixmap 4 not in the frame */
            {DEST, 2},                              /* pixmap 2 in it */
            {IN_FLIGHT, 0}, {DEST, 2},              /* the root in the frame, pixmap 2 not */
            {SRC, 0},                               /* the root, id 0, in it */
            {IN_FLIGHT, 0}, {DRAW},                 /* drawSquares into it */
            {IN_FLIGHT, 4}, {CONNECT},              /* PERSISTENT again locks every AHB */
        };
        static const int waits[] = { 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1 };
        int nA = sizeof A / sizeof *A;
        memcpy(ops, A, sizeof A);
        memcpy(ops + nA, F, sizeof F);
        n = nA + (int) (sizeof F / sizeof *F);
        gpuWaits = gpuSlices = 0;
        run(1, &cur, ops, n, LORIE_BACKEND_AUTO);
        for (int i = 0; i < (int) (sizeof F / sizeof *F); i++)
            CHECK(opWaits[nA + i] == waits[i], "frames in flight, op %d: %d waits for a Vulkan frame, want %d", nA + i,
                  opWaits[nA + i], waits[i]);
        CHECK(!newWorld.state->gpuInFlight.busy, "frames in flight: the list still set at the end");

        /* the submitter there (the connection gone or not: nothing reads it): waited for until it says so */
        newWorld.state->gpuInFlight = (__typeof__(newWorld.state->gpuInFlight)) { 1, getpid(), 1, { 77 } };
        gpuWaits = 0;
        gpuSlices = 5;
        lorieGpuWaitIdle(newWorld.state, false, 77);
        CHECK(gpuWaits == 6 && !newWorld.state->gpuInFlight.busy, "submitter there: %d waits (want 6, through 5 slices)", gpuWaits);

        /* the submitter gone: a slice, then the list cleared */
        {
            pid_t dead = fork();
            if (!dead)
                _exit(0);
            waitpid(dead, NULL, 0);
            newWorld.state->gpuInFlight = (__typeof__(newWorld.state->gpuInFlight)) { 1, dead, 1, { 77 } };
        }
        gpuWaits = 0;
        gpuSlices = 1000;
        lorieGpuWaitIdle(newWorld.state, true, 0);
        CHECK(gpuWaits == 1 && !newWorld.state->gpuInFlight.busy, "submitter gone: %d waits, the list %s", gpuWaits,
              newWorld.state->gpuInFlight.busy ? "still set" : "cleared");
        gpuSlices = 0;
    }

    /* root double buffering: the swap */
    {
        static struct fake_state swapState;
        BoxRec box = { 2, 3, 10, 7 };
        RegionRec damage = { 1, &box };
        LoriePixmapPriv sp = {0};
        PixmapRec screenPix = { { NULL }, &sp };
        LorieBuffer *back, *front;
        int outside = 0, inside = 0;
        Bool ok;

        nbuffers = 0;           /* only these two are the world's now */
        back = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 16, 16);
        front = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 16, 16);
        for (int i = 0; i < 16 * 16; i++)
            back->mem[i] = 0x10000u + (uint32_t) i, front->mem[i] = 0;
        newWorld = (struct fake_pvfb) { .state = &swapState, .ownership = LORIE_OWNERSHIP_PER_ACCESS, .damage = &damage };
        xorg_list_init(&newWorld.ahbPersistent);
        swapState.backend.ownershipAck = LORIE_OWNERSHIP_WORD(1, LORIE_OWNERSHIP_PER_ACCESS);
        sp.buffer = back;
        sp.twin = front;
        checked = &newWorld;
        runName = "root swap";
        /* the front still sampled by a frame: that frame is over first */
        swapState.gpuInFlight.ids[0] = front->desc.id;
        swapState.gpuInFlight.count = 1;
        swapState.gpuInFlight.pid = getpid();
        swapState.gpuInFlight.busy = 1;
        gpuWaits = gpuSlices = 0;
        ok = lorieRootSwap(&sp);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                bool in = x >= 2 && x < 10 && y >= 3 && y < 7;
                if (in)
                    inside += front->mem[y * 16 + x] == back->mem[y * 16 + x];
                else
                    outside += front->mem[y * 16 + x] == 0;
            }
        CHECK(ok && gpuWaits == 1, "root swap: swapped %d, %d waits for the frame sampling the front (want 1)", ok, gpuWaits);
        CHECK(inside == 8 * 4 && outside == 16 * 16 - 8 * 4, "root swap: %d of the damage copied, %d outside it left alone", inside, outside);
        CHECK(sp.buffer == front && sp.twin == back && swapState.rootWindowTextureID == back->desc.id && !newWorld.rootTwinStale,
              "root swap: not swapped, or not published");
        CHECK(!back->locked && !front->locked && swapState.lock == 0, "root swap: a buffer, or the shared lock, still held");

        /* the X server draws into the new back while the new front is sampled: no wait */
        swapState.gpuInFlight.ids[0] = back->desc.id;
        swapState.gpuInFlight.busy = 1;
        gpuWaits = 0;
        ok = loriePrepareAccess(&screenPix, EXA_PREPARE_DEST);
        CHECK(ok && gpuWaits == 0 && screenPix.devPrivate.ptr == front->mem, "root swap: the back's access waited for the front's frame");
        if (ok)
            lorieFinishAccess(&screenPix, EXA_PREPARE_DEST);
        swapState.gpuInFlight.busy = 0;

        /* out of date: all of it */
        for (int i = 0; i < 16 * 16; i++)
            front->mem[i] = 0x20000u + (uint32_t) i, back->mem[i] = 0;
        newWorld.rootTwinStale = TRUE;
        ok = lorieRootSwap(&sp);
        CHECK(ok && !memcmp(back->mem, front->mem, 16 * 16 * 4) && sp.buffer == back, "root swap, out of date: not all copied");
        checked = NULL;
    }

    /* P11-B: the swap's own locks (TERMUX_X11_RENDERER_ROOT_COPY_OPT=1), pixel for pixel the swap without them */
    {
        static const struct swapcase cases[] = {
            { "one box", 16, 16, 16, 1, { { 2, 3, 10, 7 } } },
            { "several boxes, one partly and one wholly outside", 16, 16, 16, 4,
              { { 1, 1, 3, 2 }, { 12, 9, 15, 14 }, { 14, 0, 20, 3 }, { 30, 30, 40, 40 } } },
            { "the whole buffer", 16, 16, 16, 1, { { 0, 0, 16, 16 } } },
            { "out of date", 16, 16, 16, 1, { { 5, 5, 6, 6 } }, TRUE },
            { "rows padded, a box to the right edge", 13, 9, 16, 2, { { 9, 2, 13, 8 }, { 0, 0, 2, 1 } } },
            { "nothing inside the buffer to copy", 16, 16, 16, 1, { { 20, 20, 30, 30 } } },
            { "a mapper ignoring the rect", 16, 16, 16, 2, { { 2, 3, 10, 7 }, { 11, 11, 12, 15 } }, FALSE, 1 },
            { "the front still sampled", 16, 16, 16, 1, { { 2, 3, 10, 7 } }, FALSE, 0, 0, 0, 0, 1 },
            { "the back's lock refused", 16, 16, 16, 1, { { 2, 3, 10, 7 } }, FALSE, 0, 1 },
            { "the front's lock refused, the back's taken", 16, 16, 16, 1, { { 2, 3, 10, 7 } }, FALSE, 0, 0, 1 },
            { "the front's lock refused, and the old one too", 16, 16, 16, 1, { { 2, 3, 10, 7 } }, FALSE, 0, 0, 1, 1 },
        };
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            const struct swapcase *c = &cases[k];
            Bool okOff, okOn, fellBack;
            uint32_t *off = swapOnce(c, FALSE, &okOff), *on;
            int backLocks, frontLocks, backWrite, frontWrite;
            ARect backRect, frontRect;

            on = swapOnce(c, TRUE, &okOn);
            /* this run's two buffers, as swapOnce made them */
            backLocks = buffers[0]->rectLocks, frontLocks = buffers[1]->rectLocks;
            backWrite = buffers[0]->lockWrite, frontWrite = buffers[1]->lockWrite;
            backRect = buffers[0]->lockRect, frontRect = buffers[1]->lockRect;
            fellBack = newWorld.rootCopyOptFailed;
            CHECK(okOn == okOff && !memcmp(on, off, (size_t) c->stride * c->h * 4), "root swap, %s: on, %s (off %s)", c->name,
                  okOn ? "swapped" : "not swapped", okOff ? "swapped" : "not swapped");
            /* off as before: what the boxes hold copied, the rest as it was */
            for (int y = 0; y < c->h; y++)
                for (int x = 0; x < c->stride; x++) {
                    bool in = c->stale && x < c->w;
                    for (int i = 0; i < c->n && !c->stale; i++)
                        in |= x >= c->box[i].x1 && x < c->box[i].x2 && y >= c->box[i].y1 && y < c->box[i].y2 && x < c->w;
                    uint32_t want = okOff && in ? 0x10000u + (uint32_t) (y * c->stride + x) : 0x50000u + (uint32_t) (y * c->stride + x);
                    CHECK(off[y * c->stride + x] == want, "root swap, %s: off, (%d,%d) %08x (want %08x)", c->name, x, y,
                          off[y * c->stride + x], want);
                }
            if (!c->failBack && !c->failFront) {
                /* the back read, the front written, each within the same rect holding every box copied */
                CHECK(backLocks == 1 && frontLocks == 1 && !backWrite && frontWrite && !fellBack &&
                      !memcmp(&backRect, &frontRect, sizeof backRect),
                      "root swap, %s: on, %d and %d own locks, the back %s, the front %s, fell back %d", c->name, backLocks,
                      frontLocks, backWrite ? "written" : "read", frontWrite ? "written" : "read", fellBack);
                for (int i = 0; i < c->n; i++) {
                    int x1 = max(c->box[i].x1, 0), y1 = max(c->box[i].y1, 0), x2 = min(c->box[i].x2, c->w), y2 = min(c->box[i].y2, c->h);
                    CHECK(c->stale || x2 <= x1 || y2 <= y1 ||
                          (x1 >= backRect.left && y1 >= backRect.top && x2 <= backRect.right && y2 <= backRect.bottom),
                          "root swap, %s: on, box %d outside the locks' rect", c->name, i);
                }
            } else
                CHECK(fellBack && backLocks == 1 && frontLocks == (c->failBack ? 0 : 1),
                      "root swap, %s: on, fell back %d after %d and %d own locks", c->name, fellBack, backLocks, frontLocks);
            free(off);
            free(on);
        }

        /* fell back once: the session's swaps take the locks of before (none of their own), and still swap */
        {
            Bool ok;
            int before;
            free(swapOnce(&cases[8], TRUE, &ok));
            CHECK(ok && newWorld.rootCopyOptFailed, "fell back: not swapped, or not given up");
            buffers[0]->failLockRect = 0;
            before = buffers[0]->rectLocks + buffers[1]->rectLocks;
            checked = &newWorld;
            ok = lorieRootSwap(&swapCasePriv);
            checked = NULL;
            CHECK(ok && buffers[0]->rectLocks + buffers[1]->rectLocks == before, "fell back: its own locks taken again");
        }

        /* swaps after a swap: brought up to date whole, then twice the X server drawing into the new back and its
         * damage copied - the two buffers the same after each, and as off */
        {
            static BoxRec draws[2] = { { 6, 1, 15, 12 }, { 0, 10, 4, 16 } };
            uint32_t *res[2][2];
            for (int v = 0; v < 2; v++) {
                static const struct swapcase c = { "swaps after a swap", 16, 16, 16, 1, { { 0, 0, 1, 1 } }, TRUE };
                Bool ok;
                free(swapOnce(&c, v, &ok));
                for (int d = 0; d < 2; d++) {
                    static RegionRec damage2;
                    BoxRec *b = &draws[d];
                    for (int y = b->y1; y < b->y2; y++)     /* drawing into the back (the old front) */
                        for (int x = b->x1; x < b->x2; x++)
                            swapCasePriv.buffer->mem[y * 16 + x] = 0x90000u * (uint32_t) (d + 1) + (uint32_t) (y * 16 + x);
                    damage2 = (RegionRec) { 1, b };
                    newWorld.damage = &damage2;
                    checked = &newWorld;
                    ok = lorieRootSwap(&swapCasePriv) && ok;
                    checked = NULL;
                    CHECK(ok && !memcmp(swapCasePriv.buffer->mem, swapCasePriv.twin->mem, 16 * 16 * 4),
                          "swaps after a swap (%s), swap %d: not swapped, or the buffers differ after it", v ? "on" : "off", d + 2);
                }
                for (int i = 0; i < 2; i++) {
                    res[v][i] = malloc(16 * 16 * 4);
                    memcpy(res[v][i], (i ? swapCasePriv.twin : swapCasePriv.buffer)->mem, 16 * 16 * 4);
                }
            }
            CHECK(!memcmp(res[0][0], res[1][0], 16 * 16 * 4) && !memcmp(res[0][1], res[1][1], 16 * 16 * 4),
                  "swaps after a swap: on, not as off");
            for (int v = 0; v < 2; v++)
                free(res[v][0]), free(res[v][1]);
        }
    }

    /* P11-C2, TERMUX_X11_RENDERER_ROOT_GPU_COPY=1: the swap asks the renderer's GPU for its copy and swaps at once -
     * no CPU copy, no CPU lock, no wait for the frame sampling the front; the X server's first access to its new back
     * waits, without the shared lock, until the copy settles; the copy it may have to make itself (the renderer could
     * not, or the request was taken back) is the CPU copy of before. Through all of it the X server CPU-locks neither
     * buffer of a copy not settled (rootCopyUnsettled) and never waits for a request no renderer took holding the
     * shared lock (rootRenderer). */
    {
        static BoxRec big[1] = { { 0, 0, 512, 512 } };     /* 1 MiB of pixels: worth the GPU */
        static const struct {
            const char *name;
            int does, waits;
            bool takes;
            int stall;          /* 1 no connection, 2 no surface, 3 the ownership answer asked for, 4 an EXA access open */
            bool waited, fallback, cancelled;
        } cases[] = {
            { "the renderer copies it", ROOT_COPIES, 0, false, 0, true, false, false },
            { "the renderer copies it, slowly (taken at once)", ROOT_COPIES, 3, true, 0, true, false, false },
            { "the renderer copies it, slowly (taken late)", ROOT_COPIES, 3, false, 0, true, false, false },
            { "the renderer cannot", ROOT_FAILS, 0, false, 0, true, true, false },
            { "the renderer takes it and is gone", ROOT_TAKES_AND_DIES, 0, false, 0, true, true, false },
            { "no connection: taken back", ROOT_COPIES, 0, false, 1, false, true, true },
            { "no surface: taken back", ROOT_COPIES, 0, false, 2, false, true, true },
            { "the ownership answer asked for: taken back", ROOT_COPIES, 0, false, 3, false, true, true },
            { "an EXA access open (the shared lock held): taken back", ROOT_COPIES, 0, false, 4, false, true, true },
            { "no connection, taken already: waited for", ROOT_COPIES, 2, true, 1, true, false, false },
        };
        {
            pid_t dead = fork();
            if (!dead)
                _exit(0);
            waitpid(dead, NULL, 0);
            rootDeadPid = dead;
        }
        for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
            LorieBuffer *back, *front, *other = NULL;
            LoriePixmapPriv otherP = {0};
            PixmapRec otherPix = { { NULL }, &otherP };
            uint32_t fallbacks;
            Bool ok;

            rootGpuWorld(cases[k].name, big, 1);
            back = buffers[0], front = buffers[1];
            rootRendererDoes = cases[k].does;
            rootRendererWaits = cases[k].waits;
            rootRendererTakes = cases[k].takes;
            if (cases[k].stall == 4) {
                other = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 8, 8);
                otherP = (LoriePixmapPriv) { .buffer = other };
            }
            ok = lorieRootSwap(&rootGpuPriv);
            CHECK(ok && rootGpuPriv.buffer == front && rootGpuPriv.twin == back && rootGpuState.rootWindowTextureID == back->desc.id &&
                  !newWorld.rootTwinStale && rootGpuState.rootCopy.seq == 1 && rootGpuState.rootCopy.srcId == back->desc.id &&
                  rootGpuState.rootCopy.dstId == front->desc.id && rootGpuState.rootCopy.numRects == 1 &&
                  rootGpuState.diag.rootGpuCopyRequests == 1 && rootGpuState.diag.xRootCopyUs == 0,
                  "root GPU copy, %s: the swap: swapped %d, request %u from %llu into %llu, %u rectangles", cases[k].name, ok,
                  rootGpuState.rootCopy.seq, (unsigned long long) rootGpuState.rootCopy.srcId,
                  (unsigned long long) rootGpuState.rootCopy.dstId, rootGpuState.rootCopy.numRects);
            CHECK(!strstr(trace, "L") && !strstr(trace, "l") && front->mem[0] == 0x10000u,
                  "root GPU copy, %s: the swap CPU-locked a buffer, or copied: %s", cases[k].name, trace);
            if (cases[k].stall == 1)
                rootConnection = false;
            else if (cases[k].stall == 2)
                rootSurface = false;
            else if (cases[k].stall == 3)
                rootGpuState.backend.ownershipRequest = LORIE_OWNERSHIP_WORD(2, LORIE_OWNERSHIP_PERSISTENT);
            else if (cases[k].stall == 4)
                CHECK(loriePrepareAccess(&otherPix, EXA_PREPARE_DEST) && newWorld.accessLocks == 1, "root GPU copy, %s: no access open",
                      cases[k].name);
            if (cases[k].takes && cases[k].stall == 1) {
                /* taken before the connection went: the renderer exists, the copy is waited for */
                rootGpuState.rootCopy.taken = 1;
                rootGpuState.rootCopy.pid = getpid();
            }
            fallbacks = rootGpuState.diag.rootCpuFallbacks;
            /* the X server's first access to its new back */
            ok = loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(ok && rootGpuPix.devPrivate.ptr == front->mem && newWorld.rootCopy.seq == 0,
                  "root GPU copy, %s: the access: %d, its pixels %s, the request %s", cases[k].name, ok,
                  rootGpuPix.devPrivate.ptr == front->mem ? "the new back's" : "elsewhere", newWorld.rootCopy.seq ? "unsettled" : "settled");
            if (ok)
                lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            if (cases[k].stall == 4)
                lorieFinishAccess(&otherPix, EXA_PREPARE_DEST);
            CHECK(rootGpuAlike(), "root GPU copy, %s: the new back not what the old one held", cases[k].name);
            CHECK((rootRendererCalls > 0) == cases[k].waited && (rootGpuState.diag.rootCpuFallbacks == fallbacks + 1) == cases[k].fallback &&
                  (rootGpuState.rootCopy.cancelled == 1) == cases[k].cancelled && newWorld.accessLocks == 0 && rootGpuState.lock == 0,
                  "root GPU copy, %s: waited %d times (want %s), %u CPU copies instead, taken back %d", cases[k].name, rootRendererCalls,
                  cases[k].waited ? "some" : "none", rootGpuState.diag.rootCpuFallbacks - fallbacks, rootGpuState.rootCopy.cancelled == 1);
            CHECK(!cases[k].waited || rootGpuState.diag.rootCopyCpuBlockUs > 0, "root GPU copy, %s: no time waiting counted", cases[k].name);
            CHECK(rootGpuState.rootCopy.waiting == 0, "root GPU copy, %s: still says it waits", cases[k].name);
        }
        rootConnection = rootSurface = true;

        /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG=1: a copy the X server had to wait for - its wait, and from the
         * renderer settling it to the X server going on; the access after it - its wait for the shared lock, for a
         * frame using the buffer, its hold of the lock; a copy settled before the X server needed it: no wait, so
         * neither; taken back: the wait, no resume; off: none of them, nothing stamped */
        {
            static const struct { const char *name; bool diag, early; int stall; uint32_t block, resume; } diagCases[] = {
                { "frame diag: waited for", true, false, 0, 1, 1 },
                { "frame diag: settled before it was needed", true, true, 0, 0, 0 },
                { "frame diag: taken back", true, false, 1, 1, 0 },
                { "frame diag off: waited for", false, false, 0, 0, 0 },
            };
            for (size_t k = 0; k < sizeof diagCases / sizeof *diagCases; k++) {
                volatile struct lorie_renderer_diag *dg = &rootGpuState.diag;
                Bool ok;
                rootGpuWorld(diagCases[k].name, big, 1);
                newWorld.frameDiag = diagCases[k].diag;
                rootGpuState.backend.vkFrameDiag = diagCases[k].diag;
                rootRendererWaits = 2;
                rootRendererTakes = true;
                ok = lorieRootSwap(&rootGpuPriv);
                if (diagCases[k].early) {
                    rootRendererWaits = 0;
                    rootRenderer();             /* the renderer done before the X server comes to the buffer */
                }
                if (diagCases[k].stall == 1)
                    rootConnection = false, rootRendererTakes = false;
                ok = ok && loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST);
                if (ok)
                    lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
                rootConnection = true;
                CHECK(ok && rootGpuAlike() && dg->rootBlock.count == diagCases[k].block && dg->rootResume.count == diagCases[k].resume,
                      "%s: %u waits, %u resumes timed (want %u, %u)", diagCases[k].name, dg->rootBlock.count, dg->rootResume.count,
                      diagCases[k].block, diagCases[k].resume);
                CHECK(dg->rootResume.maxUs < 1000000 && (diagCases[k].diag || !rootGpuState.rootCopy.doneUs),
                      "%s: resumed %u us after it was settled, stamped %d", diagCases[k].name, dg->rootResume.maxUs,
                      rootGpuState.rootCopy.doneUs != 0);
                CHECK(dg->xAccessLock.count == diagCases[k].diag && dg->xAccessGpu.count == diagCases[k].diag &&
                      dg->xAccess.count == diagCases[k].diag && !rootGpuPriv.accessStartUs,
                      "%s: the access's lock wait %u, frame wait %u, hold %u timed (want %d each), its start left %llu",
                      diagCases[k].name, dg->xAccessLock.count, dg->xAccessGpu.count, dg->xAccess.count, diagCases[k].diag,
                      (unsigned long long) rootGpuPriv.accessStartUs);
            }
        }

        /* P11-C3-B1, TERMUX_X11_EXA_LOCK_OPT=1: a PER_ACCESS AHB access - not to the root the X server publishes -
         * holds a reservation instead of the shared lock: taken under it once no frame uses the AHB, the shared lock let
         * go before the CPU lock, the CPU lock gone before the reservation, let go under the shared lock again. Nested
         * (EXA's dest, src, mask), each its own; an access to the published root holds the shared lock as before, and a
         * reserved one inside it too; a frame using the AHB waited for first, holding the lock; no free slot: the
         * shared lock (counted); the CPU lock refused: the reservation let go, nothing held; a renderer waiting for a
         * release woken (its condition, the futex) - none woken otherwise; a reserved access open, the root copy into
         * the new back waited for (no shared lock held), not taken back. */
        {
            LorieBuffer *xb[10];
            LoriePixmapPriv xp[10];
            PixmapRec xr[10];
            struct fake_state *st = &rootGpuState;
            uint32_t releases;
            int signals0, wakes0, waits0;
            Bool ok;

            rootGpuWorld("B1", big, 1);
            newWorld.exaLockOpt = TRUE;
            newWorld.frameDiag = TRUE;
            st->rootWindowTextureID = buffers[1]->desc.id;          /* the front: published, sampled */
            for (int k = 0; k < 10; k++) {
                xb[k] = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 8, 8);
                xp[k] = (LoriePixmapPriv) { .buffer = xb[k] };
                xr[k] = (PixmapRec) { { NULL }, &xp[k], { 8, 8 } };
            }
            releases = st->cpuAccess.releases;
            signals0 = rendererWakes;
            ok = loriePrepareAccess(&xr[0], EXA_PREPARE_DEST) && loriePrepareAccess(&xr[1], EXA_PREPARE_SRC) &&
                 loriePrepareAccess(&xr[2], EXA_PREPARE_MASK);
            CHECK(ok && st->lock == 0 && newWorld.accessLocks == 0 && st->cpuAccess.count == 3 && xb[0]->cpuHeld && xb[1]->cpuHeld &&
                  xb[2]->cpuHeld && lorieCpuReserved(st, xb[0]->desc.id) && lorieCpuReserved(st, xb[2]->desc.id),
                  "B1 nested: prepared %d, shared lock %d, access locks %d, %u reserved", ok, st->lock, newWorld.accessLocks,
                  st->cpuAccess.count);
            lorieFinishAccess(&xr[2], EXA_PREPARE_MASK);
            lorieFinishAccess(&xr[1], EXA_PREPARE_SRC);
            CHECK(st->cpuAccess.count == 1 && lorieCpuReserved(st, xb[0]->desc.id) && !xb[1]->cpuHeld && st->lock == 0,
                  "B1 nested: after two finished, %u reserved", st->cpuAccess.count);
            lorieFinishAccess(&xr[0], EXA_PREPARE_DEST);
            CHECK(st->cpuAccess.count == 0 && st->lock == 0 && st->cpuAccess.releases == releases + 3 && !xb[0]->cpuHeld &&
                  st->diag.xAccessReserved == 3 && rendererWakes == signals0 && st->diag.xAccessHold.count == 3 &&
                  st->diag.xAccess.count == 3, "B1 nested: %u left, %u releases, %u counted, %d signals, %u holds timed",
                  st->cpuAccess.count, st->cpuAccess.releases - releases, st->diag.xAccessReserved, rendererWakes - signals0,
                  st->diag.xAccessHold.count);
            {
                /* each: the shared lock let go before the CPU lock; the CPU lock let go before the shared lock is taken again */
                char want[2][64];
                snprintf(want[0], sizeof want[0], "M+ M- L%llu ", xb[0]->desc.id);
                snprintf(want[1], sizeof want[1], "U%llu M+ M- ", xb[0]->desc.id);
                CHECK(strstr(trace, want[0]) && strstr(trace, want[1]), "B1 nested: the order of the locks: %s", trace);
            }

            /* the published root: the shared lock, as before - a reserved access inside it too */
            st->rootWindowTextureID = buffers[0]->desc.id;          /* as when root double buffering is not in effect */
            ok = loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(ok && st->lock == 1 && newWorld.accessLocks == 1 && st->cpuAccess.count == 0, "B1 published root: lock %d, %u reserved",
                  st->lock, st->cpuAccess.count);
            ok = loriePrepareAccess(&xr[3], EXA_PREPARE_SRC);
            CHECK(ok && st->lock == 1 && newWorld.accessLocks == 1 && lorieCpuReserved(st, xb[3]->desc.id),
                  "B1 inside the root's access: lock %d, access locks %d", st->lock, newWorld.accessLocks);
            lorieFinishAccess(&xr[3], EXA_PREPARE_SRC);
            lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(st->lock == 0 && newWorld.accessLocks == 0 && st->cpuAccess.count == 0, "B1 published root: lock %d left", st->lock);
            st->rootWindowTextureID = buffers[1]->desc.id;

            /* a frame using the AHB: waited for first, under the shared lock (fakeFutexWait checks it) */
            waits0 = gpuWaits;
            st->gpuInFlight.ids[0] = xb[4]->desc.id, st->gpuInFlight.count = 1, st->gpuInFlight.pid = getpid();
            st->gpuInFlight.busy = 1;
            ok = loriePrepareAccess(&xr[4], EXA_PREPARE_DEST);
            CHECK(ok && gpuWaits == waits0 + 1 && !st->gpuInFlight.busy && lorieCpuReserved(st, xb[4]->desc.id) && st->lock == 0,
                  "B1 frame in flight: %d waits, reserved %d", gpuWaits - waits0, lorieCpuReserved(st, xb[4]->desc.id));
            lorieFinishAccess(&xr[4], EXA_PREPARE_DEST);

            /* no free slot: the shared lock, counted */
            for (int k = 0; k < LORIE_CPU_ACCESS_MAX; k++)
                loriePrepareAccess(&xr[k], EXA_PREPARE_SRC);
            ok = loriePrepareAccess(&xr[8], EXA_PREPARE_DEST);
            CHECK(ok && st->cpuAccess.count == LORIE_CPU_ACCESS_MAX && st->lock == 1 && newWorld.accessLocks == 1 &&
                  !lorieCpuReserved(st, xb[8]->desc.id) && st->diag.xAccessReserveFull == 1,
                  "B1 full: lock %d, %u reserved, %u counted full", st->lock, st->cpuAccess.count, st->diag.xAccessReserveFull);
            lorieFinishAccess(&xr[8], EXA_PREPARE_DEST);
            for (int k = LORIE_CPU_ACCESS_MAX - 1; k >= 0; k--)
                lorieFinishAccess(&xr[k], EXA_PREPARE_SRC);
            CHECK(st->lock == 0 && st->cpuAccess.count == 0, "B1 full: lock %d, %u reserved after", st->lock, st->cpuAccess.count);

            /* the CPU lock refused: the reservation let go, nothing held */
            xb[5]->failLock = 1;
            releases = st->cpuAccess.releases;
            ok = loriePrepareAccess(&xr[5], EXA_PREPARE_DEST);
            xb[5]->failLock = 0;
            CHECK(!ok && st->lock == 0 && st->cpuAccess.count == 0 && st->cpuAccess.releases == releases + 1 && newWorld.accessLocks == 0,
                  "B1 refused: prepared %d, lock %d, %u reserved", ok, st->lock, st->cpuAccess.count);

            /* a renderer waiting for a release: woken once, the waiter cleared */
            signals0 = rendererWakes, wakes0 = releaseWakes;
            loriePrepareAccess(&xr[6], EXA_PREPARE_SRC);
            st->cpuAccess.waiter = 1;
            lorieFinishAccess(&xr[6], EXA_PREPARE_SRC);
            CHECK(rendererWakes == signals0 + 1 && releaseWakes == wakes0 + 1 && !st->cpuAccess.waiter && st->diag.copiesDeferredWakes == 1,
                  "B1 waiter: %d signals, %d futex wakes, waiter %u", rendererWakes - signals0, releaseWakes - wakes0, st->cpuAccess.waiter);
            loriePrepareAccess(&xr[6], EXA_PREPARE_SRC);
            lorieFinishAccess(&xr[6], EXA_PREPARE_SRC);
            CHECK(rendererWakes == signals0 + 1 && releaseWakes == wakes0 + 1, "B1 no waiter: woken anyway");

            /* off: the shared lock throughout, nothing reserved */
            newWorld.exaLockOpt = FALSE;
            ok = loriePrepareAccess(&xr[7], EXA_PREPARE_SRC);
            CHECK(ok && st->lock == 1 && st->cpuAccess.count == 0, "B1 off: lock %d, %u reserved", st->lock, st->cpuAccess.count);
            lorieFinishAccess(&xr[7], EXA_PREPARE_SRC);
            newWorld.exaLockOpt = TRUE;
        }

        /* a reserved access open while the new back's root copy is not settled: waited for, not taken back (no shared
         * lock is held) */
        {
            LorieBuffer *other;
            LoriePixmapPriv otherP;
            PixmapRec otherPix;
            Bool ok;
            rootGpuWorld("B1 root copy", big, 1);
            newWorld.exaLockOpt = TRUE;
            other = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 8, 8);
            otherP = (LoriePixmapPriv) { .buffer = other };
            otherPix = (PixmapRec) { { NULL }, &otherP, { 8, 8 } };
            rootRendererWaits = 1;
            rootRendererTakes = true;
            ok = lorieRootSwap(&rootGpuPriv) && loriePrepareAccess(&otherPix, EXA_PREPARE_SRC);
            CHECK(ok && lorieCpuReserved(&rootGpuState, other->desc.id) && rootGpuState.lock == 0, "B1 root copy: no reserved access open");
            ok = loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(ok && rootGpuState.rootCopy.cancelled != 1 && rootRendererCalls > 0 && rootGpuAlike() && rootGpuState.diag.rootCpuFallbacks == 0,
                  "B1 root copy: cancelled %d, the renderer asked %d times, CPU copies %u", rootGpuState.rootCopy.cancelled == 1,
                  rootRendererCalls, rootGpuState.diag.rootCpuFallbacks);
            if (ok)
                lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            lorieFinishAccess(&otherPix, EXA_PREPARE_SRC);
            CHECK(rootGpuState.lock == 0 && rootGpuState.cpuAccess.count == 0 && newWorld.accessLocks == 0,
                  "B1 root copy: lock %d, %u reserved after", rootGpuState.lock, rootGpuState.cpuAccess.count);
        }

        /* small damage: copied on the CPU at the swap, as before */
        {
            static BoxRec small[1] = { { 3, 4, 40, 30 } };
            rootGpuWorld("small damage", small, 1);
            CHECK(lorieRootSwap(&rootGpuPriv) && rootGpuState.rootCopy.seq == 0 && rootGpuState.diag.rootCpuSmall == 1 && rootGpuAlike() &&
                  rootGpuState.diag.xRootCopyUs > 0, "small damage: asked of the GPU, or not copied at the swap");
        }

        /* the option off: copied on the CPU at the swap, as before - whatever the damage */
        rootGpuWorld("the option off", big, 1);
        newWorld.rootGpuCopy = FALSE;
        CHECK(lorieRootSwap(&rootGpuPriv) && rootGpuState.rootCopy.seq == 0 && rootGpuState.diag.rootCpuSmall == 0 && rootGpuAlike(),
              "the option off: asked of the GPU, or not copied at the swap");

        /* more boxes than a request holds: those past its last slot within the bounds of the rest - every one copied */
        {
            static BoxRec many[20];      /* bands, as a region's boxes are */
            for (int i = 0; i < 20; i++)
                many[i] = (BoxRec) { (short) (i & 1), (short) (i * 26), (short) (512 - (i & 1)), (short) (i * 26 + 26) };
            rootGpuWorld("twenty boxes", many, 20);
            CHECK(lorieRootSwap(&rootGpuPriv) && rootGpuState.rootCopy.numRects == LORIE_GPU_COPY_MAX_RECTS,
                  "twenty boxes: %u rectangles asked for", rootGpuState.rootCopy.numRects);
            CHECK(loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST), "twenty boxes: no access");
            lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(rootGpuAlike(), "twenty boxes: not every box copied");
        }

        /* swaps after a swap: the X server drawing into its new back, its damage then copied by the GPU too */
        {
            static BoxRec second[1] = { { 0, 8, 512, 520 } };
            rootGpuWorld("swaps after a swap", big, 1);
            CHECK(lorieRootSwap(&rootGpuPriv), "swaps after a swap: the first not swapped");
            CHECK(loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST), "swaps after a swap: no access");
            for (int y = 8; y < 520; y++)               /* drawn into the new back */
                for (int x = 0; x < 512; x++)
                    rootGpuPriv.buffer->mem[y * 512 + x] = 0x700000u + (uint32_t) (y * 512 + x);
            lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            rootGpuDamage = (RegionRec) { 1, second };
            CHECK(lorieRootSwap(&rootGpuPriv) && rootGpuState.rootCopy.seq == 2 && rootGpuPriv.buffer == buffers[0] &&
                  rootGpuState.rootCopy.srcId == buffers[1]->desc.id, "swaps after a swap: the second not asked of the GPU");
            CHECK(loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST), "swaps after a swap: no second access");
            lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);
            CHECK(rootGpuAlike() && buffers[0]->mem[8 * 512] == 0x700000u + 8 * 512, "swaps after a swap: the buffers differ after it");
        }

        /* the ownership answer moving to PERSISTENT (the AHBs CPU-locked for good): the request taken back and copied
         * first, never left to a renderer that waits for this very answer */
        rootGpuWorld("the ownership answer", big, 1);
        CHECK(lorieRootSwap(&rootGpuPriv), "the ownership answer: not swapped");
        rootGpuState.backend.ownershipRequest = LORIE_OWNERSHIP_WORD(2, LORIE_OWNERSHIP_PERSISTENT);
        lorieApplyOwnership(LORIE_OWNERSHIP_PERSISTENT, 2);
        CHECK(rootGpuState.rootCopy.cancelled == 1 && rootRendererCalls == 0 && newWorld.rootCopy.seq == 0 && rootGpuAlike() &&
              newWorld.ownership == LORIE_OWNERSHIP_PERSISTENT, "the ownership answer: the root copy not settled first");

        /* a renderer connecting (or gone) puts the ownership back - the new one gets the shared state only after: a
         * request no renderer took taken back at once, not waited for */
        rootGpuWorld("a renderer connecting", big, 1);
        CHECK(lorieRootSwap(&rootGpuPriv), "a renderer connecting: not swapped");
        lorieResetOwnership();
        CHECK(rootGpuState.rootCopy.cancelled == 1 && rootRendererCalls == 0 && newWorld.rootCopy.seq == 0 && rootGpuAlike(),
              "a renderer connecting: the root copy waited for (%d times), or not copied", rootRendererCalls);

        /* the CPU copy failing too: the swap undone, the twin brought up to date whole next time */
        rootGpuWorld("the CPU copy failing", big, 1);
        rootRendererDoes = ROOT_FAILS;
        CHECK(lorieRootSwap(&rootGpuPriv), "the CPU copy failing: not swapped");
        buffers[1]->failLock = 1;
        CHECK(loriePrepareAccess(&rootGpuPix, EXA_PREPARE_DEST) || 1, "");
        buffers[1]->failLock = 0;
        CHECK(rootGpuPriv.buffer == buffers[0] && rootGpuPriv.twin == buffers[1] && newWorld.rootTwinStale,
              "the CPU copy failing: the swap not undone");
        if (rootGpuPix.devPrivate.ptr)
            lorieFinishAccess(&rootGpuPix, EXA_PREPARE_DEST);

        /* the screen pixmap destroyed with a request not settled: not copied by the X server any more, settled
         * before the next one is asked for */
        {
            static BoxRec again[1] = { { 0, 0, 512, 512 } };
            LorieBuffer *oldBack, *oldFront;
            rootGpuWorld("the screen pixmap destroyed", big, 1);
            oldBack = buffers[0], oldFront = buffers[1];
            rootRendererDoes = ROOT_FAILS;
            CHECK(lorieRootSwap(&rootGpuPriv), "destroyed: not swapped");
            lorieExaDestroyPixmap(NULL, &rootGpuPriv);
            CHECK(newWorld.rootCopy.abandoned && oldBack->released && oldFront->released, "destroyed: not abandoned");
            /* a new screen pixmap's swap: the old request settled (failed, nothing copied), the new one asked for */
            {
                LorieBuffer *nb = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 512, 520), *nf = newBuffer(LORIEBUFFER_AHARDWAREBUFFER, 512, 520);
                uint32_t fallbacks = rootGpuState.diag.rootCpuFallbacks;
                rootGpuPriv = (LoriePixmapPriv) { .buffer = nb, .twin = nf };
                rootGpuDamage = (RegionRec) { 1, again };
                rootRendererDoes = ROOT_COPIES;
                CHECK(lorieRootSwap(&rootGpuPriv) && rootGpuState.rootCopy.seq == 2 && rootGpuState.rootCopy.dstId == nf->desc.id &&
                      rootGpuState.diag.rootCpuFallbacks == fallbacks && rootRendererCalls > 0,
                      "destroyed: the next request %u, %u CPU copies of the old one", rootGpuState.rootCopy.seq,
                      rootGpuState.diag.rootCpuFallbacks - fallbacks);
            }
        }
        checked = NULL;
        screen.devPrivate = NULL;
    }

    printf("TOWNERSHIP AHB CPU ownership against 3c98ffa's own code: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
