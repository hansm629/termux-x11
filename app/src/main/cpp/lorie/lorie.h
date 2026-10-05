#pragma once

#include <android/hardware_buffer.h>
#include <android/native_window_jni.h>
#include <android/choreographer.h>
#include <android/log.h>

#include <stdbool.h>
#include <X11/Xdefs.h>
#include <X11/keysymdef.h>
#include <jni.h>
#include <screenint.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "linux/input-event-codes.h"
#include "buffer.h"

#define PORT 7892
#define MAGIC "0xDEADBEEF"

struct lorie_shared_server_state;

void lorieConfigureNotify(int width, int height, int framerate, size_t name_size, char* name);
void lorieEnableClipboardSync(Bool enable);
void lorieSendClipboardData(const char* data);
void lorieInitClipboard(void);
void lorieRequestClipboard(void);
void lorieHandleClipboardAnnounce(void);
void lorieHandleClipboardData(const char* data);
void lorieSetStylusEnabled(Bool enabled);
void lorieWakeServer(void);
void lorieRecheckGpuCopies(void);
/* Hands back what a cancelled-but-unfinished GPU copy was using, once the renderer reports that
 * serial done or failed. Called from the X server thread: every frame, and on the event that says
 * the renderer made progress - the frame after may never come. */
void lorieReapAbandonedCopies(void);
void lorieNoteGpuCopyRequeued(void);
/* Both run on the X server thread and release nothing by themselves - the reaper does that, once
 * what is known about the session that owed a copy's report says it may (see lorieCopyResolve).
 * Neither a broken socket nor a new connection says the old renderer's GPU work has finished: the
 * process can outlive its socket, and the next connection can come from the same process. pid is
 * the renderer's process, as the Binder call that asked for the connection names it; 0 if unknown. */
void lorieNoteRendererLost(void);
void lorieNoteRendererConnected(int32_t pid);
/* So a socket error can name the session it belongs to, and a lost event that has been overtaken by
 * a new connection can be dropped instead of marking the new session's work over and tearing down
 * the buffers it has just registered. */
uint32_t lorieRendererSessionId(void);
Bool lorieRendererSessionIsCurrent(uint32_t session);
void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d);
/* Starts the vsync callbacks, with the 64-bit frame time where the platform has it. */
void lorieChoreographerStart(AChoreographer *d);
/* Input thread: records a pointer or touch event in the trace, if tracing is on. */
void lorieTraceInput(uint32_t eventType);
/* Called from EXA's fallback entry (xserver.patch, exa_priv.h) before any operand is prepared. */
void lorieExaFallbackBegin(void);
/* Called from exaPrepareAccess (xserver.patch, exa.c) with the drawable still known. Declared with the
 * struct tags rather than DrawablePtr/PixmapPtr: the renderer includes this header without pixmap.h. */
struct _Drawable;
struct _Pixmap;
void lorieExaAccess(struct _Drawable *pDrawable, struct _Pixmap *pPixmap, int index);
void lorieActivityConnected(void);
void lorieSendSharedServerState(int memfd);
void lorieRegisterBuffer(LorieBuffer* buffer);
void lorieUnregisterBuffer(LorieBuffer* buffer);
bool lorieConnectionAlive(void);
void lorieCopyContext(int ctx);
extern bool lorieDebugEnabled; // Set in activity.c's startLogcat, only called when TERMUX_X11_DEBUG=1.
void lorieSetRendererWakeupCond(int fd);
int rendererGetWakeupCondFd(void);

__unused void rendererInit(JNIEnv* env);
__unused void rendererSetFiltering(JNIEnv* env, jobject self, jint filtering);
__unused void rendererSetSmoothPresentationEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetDisplayRefreshRate(JNIEnv* env, jobject self, jfloat refreshRate);
__unused void rendererSetPerfLogEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetPostSwapTouchEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetPostSwapFenceWaitEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetRootFenceWaitEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetSwapBackpressureGuardEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererTestCapabilities(int* legacy_drawing);
__unused void rendererSetWindow(JNIEnv *env, jobject thiz, jobject sfc);
__unused void rendererSetViewport(JNIEnv *env, jclass clazz, int x, int y, int w, int h, int ew, int eh);
__unused void rendererSetSharedState(struct lorie_shared_server_state* newState);
__unused void rendererAddBuffer(LorieBuffer* buf);
__unused void rendererRemoveBuffer(uint64_t id);
__unused void rendererRemoveAllBuffers(void);

/*
 * Returns whether the lock was taken. False means the mutex answered with something other than
 * "busy" several times in a row and cannot be relied on; the caller must then leave the shared state
 * it guards alone, and do whatever it would do if it had nothing to draw or copy this time.
 */
/*
 * Raises a shared statistics maximum without losing a concurrent reset. The X server reads these
 * counters and zeroes them in one atomic exchange; a plain "if bigger, store" on the other side
 * could read the old value, lose the race to that exchange, and then write the stale maximum back
 * over the reset.
 */
#define LORIE_STAT_MAX(ptr, value) do { \
    __auto_type _lorieOld = __atomic_load_n((ptr), __ATOMIC_RELAXED); \
    __typeof__(_lorieOld) _lorieNew = (value); \
    while (_lorieNew > _lorieOld && \
           !__atomic_compare_exchange_n((ptr), &_lorieOld, _lorieNew, false, \
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) \
        ; \
} while (0)

/* Appends one trace record if tracing is on. Never blocks: a writer that laps the reader overwrites,
 * and the reader notices from seq and counts what it lost. */
static inline __always_inline void lorieTraceAt(struct lorie_shared_server_state *st, uint32_t kind,
                                                 uint32_t a, uint64_t b, uint64_t tUs);
static inline __always_inline uint64_t lorieTraceNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}
#define lorieTrace(st, kind, a, b) do { \
    struct lorie_shared_server_state *_lorieTs = (struct lorie_shared_server_state *) (st); \
    if (_lorieTs && _lorieTs->traceEnabled) \
        lorieTraceAt(_lorieTs, (kind), (uint32_t) (a), (uint64_t) (b), lorieTraceNowUs()); \
} while (0)

__attribute__((warn_unused_result))
static inline __always_inline bool lorie_mutex_lock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    // Unfortunately there is no robust mutexes in bionic.
    // Posix does not define any valid way to unlock stuck non-robust mutex
    // so in the case if renderer or X server process unexpectedly die with locked mutex
    // we will simply reinitialize it.
    struct timespec ts = {0};
    int timeouts = 0, failures = 0;
    while(true) {
        // CLOCK_REALTIME, because that is the clock pthread_mutex_timedlock() measures its absolute
        // deadline against. A CLOCK_MONOTONIC value is seconds since boot where this wants seconds
        // since the epoch, so the deadline was always already in the past: uncontended locks still
        // took the fast path, but a contended one returned ETIMEDOUT immediately and the loop below
        // span on the CPU instead of sleeping for the 33ms this is meant to wait.
        clock_gettime(CLOCK_REALTIME, &ts);

        // 33 msec is enough to complete any drawing operation on both X server and renderer side
        // In the case if mutex is locked most likely other thread died with the mutex locked
        ts.tv_nsec += 33UL * 1000000UL;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec  = ts.tv_nsec % 1000000000L;
        }

        int ret = pthread_mutex_timedlock(mutex, &ts);
        // Only 0 means we hold it. Anything else went down the recovery path below, which used to
        // be reached only by ETIMEDOUT - every other error returned as though the lock had been
        // taken, and the caller went on to touch shared state it did not own.
        if (ret != 0) {
            // ETIMEDOUT is the only error that waiting again can fix: it says someone else holds
            // the lock and had not finished within the deadline.
            if (ret == ETIMEDOUT && (*lockingPid == getpid() || lorieConnectionAlive())) {
                // Being alive is not the same as making progress, and this loop has no way to
                // tell. Reinitializing under a live holder would break mutual exclusion, so the
                // wait continues - but it says so, once a second, instead of spinning silently.
                if (++timeouts % 30 == 0)
                    __android_log_print(ANDROID_LOG_WARN, "lorie",
                                        "still waiting for the shared lock after %d ms (held by pid %d)",
                                        timeouts * 33, *lockingPid);
                continue;
            }

            /*
             * Anything else is not a lock that is busy, it is a lock that does not work: EINVAL on
             * memory that is no longer a mutex, EAGAIN past the recursive limit. Those used to be
             * answered by overwriting the mutex, which is only defensible when the other process is
             * known to be gone - the case below. Doing it for an error says nothing about who still
             * holds the lock, and it would release it out from under them.
             *
             * Nor is aborting the answer, which is what replaced it for a while: it takes the whole
             * app or server down over one call. The one error that can be transient is a read that
             * overlapped the other process reinitialising the mutex after deciding this one was gone,
             * so it is asked again a couple of times, a millisecond apart. Past that the caller is
             * told it did not get the lock, and skips whatever it needed it for.
             */
            if (ret != ETIMEDOUT) {
                if (++failures < 3) {
                    usleep(1000);
                    continue;
                }
                __android_log_print(ANDROID_LOG_ERROR, "lorie",
                                    "shared lock is unusable (%s, held by pid %d); skipping this access",
                                    strerror(ret), *lockingPid);
                return false;
            }

            pthread_mutexattr_t attr;
            pthread_mutex_t initializer = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            memcpy(mutex, &initializer, sizeof(initializer));
            pthread_mutex_init(mutex, &attr);
            timeouts = 0;
            // Mutex will be locked fine on the next iteration
        } else {
            *lockingPid = getpid();
            return true;
        }
    }
}

static inline __always_inline void lorie_mutex_unlock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    *lockingPid = 0;
    pthread_mutex_unlock(mutex);
}

typedef enum {
    EVENT_UNKNOWN __unused = 0,
    EVENT_SHARED_SERVER_STATE,
    EVENT_ADD_BUFFER,
    EVENT_REMOVE_BUFFER,
    EVENT_SCREEN_SIZE,
    EVENT_TOUCH,
    EVENT_MOUSE,
    EVENT_KEY,
    EVENT_STYLUS,
    EVENT_STYLUS_ENABLE,
    EVENT_UNICODE,
    EVENT_CLIPBOARD_ENABLE,
    EVENT_CLIPBOARD_ANNOUNCE,
    EVENT_CLIPBOARD_REQUEST,
    EVENT_CLIPBOARD_SEND,
    EVENT_WINDOW_FOCUS_CHANGED,
    EVENT_RENDERER_WAKEUP_COND,
    EVENT_GPU_COPY_DONE,
} eventType;

typedef union {
    uint8_t type;
    struct {
        uint8_t t;
        uint16_t width, height, framerate;
        int dpi; size_t name_size;
        char *name;
    } screenSize;
    struct {
        uint8_t t;
        unsigned long id;
    } removeBuffer;
    struct {
        uint8_t t;
        uint16_t type, id, x, y;
    } touch;
    struct {
        uint8_t t;
        float x, y;
        uint8_t detail, down, relative;
    } mouse;
    struct {
        uint8_t t;
        uint16_t key;
        uint8_t state;
    } key;
    struct {
        uint8_t t;
        float x, y;
        uint16_t pressure;
        int8_t tilt_x, tilt_y;
        int16_t orientation;
        uint8_t buttons, eraser, mouse;
    } stylus;
    struct {
        uint8_t t, enable;
    } stylusEnable;
    struct {
        uint8_t t;
        uint32_t code;
    } unicode;
    struct {
        uint8_t t;
        uint8_t enable;
    } clipboardEnable;
    struct {
        uint8_t t;
        uint32_t count;
    } clipboardSend;
} lorieEvent;

typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;

/* The cursor texture is allocated once at this size and updated in place. */
#define LORIE_CURSOR_TEX_SIZE 512

/* Two 60Hz frames: a gap this long is a hitch a user can see, not just a missed vsync. */
#define LORIE_LONG_FRAME_US 33000

/* Rects per queued copy, and copies the queue holds (a power of two: indices wrap at 2^32). Sized so a
 * handover's carry and a client's presents fit side by side, rather than either being turned over to
 * the CPU for want of room (cpuCarryKept, cpuPresents). */
#define LORIE_GPU_COPY_MAX_RECTS 64
#define LORIE_GPU_COPY_QUEUE_CAPACITY 32

/* Why a present was drawn by the CPU instead of being copied by the GPU (presentStats.cpuPresents). */
enum {
    LORIE_CPU_PRESENT_DISABLED,       /* GPU presents turned off, or legacy drawing */
    LORIE_CPU_PRESENT_NO_RENDERER,    /* no renderer connected, or it has no surface */
    LORIE_CPU_PRESENT_NOT_SAMPLEABLE, /* source or destination not in a buffer the GPU can use */
    LORIE_CPU_PRESENT_RECTS,          /* more rectangles than one queue entry holds */
    LORIE_CPU_PRESENT_QUEUE_FULL,
    LORIE_CPU_PRESENT_REPLACING_FULL, /* too many copies in flight over owed root areas */
    LORIE_CPU_PRESENT_NO_RECORD,      /* no room to keep track of it */
    LORIE_CPU_PRESENT_REASONS
};

/* The copies X core rendering makes with the CPU, by the EXA fallback that made them (lorieNoteCoreCopy). */
enum {
    LORIE_CORE_COPY_WINDOW,           /* CopyWindow: a window moved */
    LORIE_CORE_COPY_AREA,             /* CopyArea between drawables, and what is built on it */
    LORIE_CORE_COPY_KINDS
};
/* Why a copy X core rendering makes stayed with the CPU instead of going to the GPU (coreGpuKept). */
enum {
    LORIE_CORE_KEPT_OFF,              /* TERMUX_X11_CORE_GPU_COPY=0, GPU presents off, or legacy drawing */
    LORIE_CORE_KEPT_NO_RENDERER,      /* no renderer connected, or it has no surface */
    LORIE_CORE_KEPT_OP,               /* not a plain copy: a raster op, a plane mask, a bit plane, not depth 24 */
    LORIE_CORE_KEPT_NOT_GPU,          /* source or destination not in a buffer the GPU can use */
    LORIE_CORE_KEPT_SAME_PIXMAP,      /* within one pixmap other than the root: nothing to stage it through */
    LORIE_CORE_KEPT_NO_SLOT,          /* within the root, and no slot free to stage it through */
    LORIE_CORE_KEPT_RECTS,            /* within the root, more rects than one queue entry holds */
    LORIE_CORE_KEPT_OWED,             /* the root's drawing slot still owes an area, not had in time */
    LORIE_CORE_KEPT_BUSY,             /* no room in the queue, or the shared lock already held */
    LORIE_CORE_KEPT_TIMEOUT,          /* queued, and not done in time or not made: taken back */
    LORIE_CORE_KEPT_REASONS
};

/* What a CPU copy made through X core rendering is a part of, set around the callers whose copy is a
 * framebuffer copy of its own (lorieCopyContext). The values are also used by the xserver patch
 * (present_priv.h), which cannot include this header. */
#define LORIE_COPY_CTX_NONE    0
#define LORIE_COPY_CTX_PRESENT 1          /* a present the CPU draws because the GPU path turned it down */
#define LORIE_COPY_CTX_RESIZE  2          /* the old root into a resized one */
#define LORIE_COPY_CTX_UNFLIP  3          /* a flipped client buffer back into the root as the flip ends */

/* Why a handover's carry stayed with the CPU instead of going to the GPU (presentStats.cpuCarryKept). */
enum {
    LORIE_CARRY_KEPT_OFF,             /* TERMUX_X11_ROOT_GPU_CARRY=0, GPU presents off, or legacy drawing */
    LORIE_CARRY_KEPT_NO_RENDERER,     /* no renderer connected, or it has no surface */
    LORIE_CARRY_KEPT_BUSY,            /* the queue or the copy records are half taken already */
    LORIE_CARRY_KEPT_RECTS,           /* more rectangles than its queue entries hold */
    LORIE_CARRY_KEPT_REASONS
};

typedef struct {
    uint64_t serial;
    uint64_t srcBufferId;
    uint64_t dstBufferId;
    int16_t xOff, yOff;
    uint16_t numRects;
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
} LorieGpuCopyEntry;   /* immutable once published - see entryState for the one thing that is not */

/* Who a queued copy belongs to. Both sides move it out of QUEUED with a compare-and-swap, so exactly
 * one of them wins: the renderer claiming it to run, or the X server cancelling it. A cancelled copy
 * is never run; a claimed one is run under the shared lock and finished - fence and all - before that
 * lock is let go, so an X server that holds the lock never sees a claimed copy still running. */
enum { LORIE_JOB_QUEUED = 0, LORIE_JOB_CLAIMED = 1, LORIE_JOB_CANCELLED = 2 };

/*
 * Event trace, off unless TERMUX_X11_TRACE names a file. Every step a frame takes on its way to the
 * screen, from both processes, on one CLOCK_MONOTONIC time line, so a stall can be followed to the step
 * it is in - which the five-second totals cannot do. Written by the X server thread, the input thread
 * and the renderer; read and written out by the X server. Record layout is fixed (32 bytes) because
 * tools/trace/analyze.py reads it.
 */
typedef struct {
    volatile uint64_t seq;   /* index + 1, published last; a reader takes a record only when it matches */
    uint64_t tUs;            /* CLOCK_MONOTONIC */
    uint32_t kind;
    uint32_t a;
    uint64_t b;
} LorieTraceRecord;

enum {
    LORIE_TRACE_REQUEST = 1,  /* X: client present request arrived    a = target - crtc msc, b = crtc msc */
    LORIE_TRACE_ENQUEUE,      /* X: copy offloaded to the renderer     a = 1 if into the root, 2 if a
                               *    handover's carry,             b = serial */
    LORIE_TRACE_RESOLVED,     /* X: copy finished at the X server      a = 1 if made,           b = serial */
    LORIE_TRACE_PUBLISH,      /* X: root slot handed on                a = slot,                b = buffer id */
    LORIE_TRACE_TICK,         /* X: vsync redraw                       a = steps,               b = msc */
    LORIE_TRACE_INPUT,        /* input thread: pointer/touch event     a = event type,          b = 0 */
    LORIE_TRACE_DRAIN,        /* renderer: queue drained               a = 1 if GPU work issued, b = last serial */
    LORIE_TRACE_FENCE,        /* renderer: copies' fence signalled     a = wait us,             b = serial */
    LORIE_TRACE_DIRECT,       /* renderer: root buffer to compositor   a = slot,                b = buffer id */
    LORIE_TRACE_GLSWAP,       /* renderer: GL frame swapped            a = 1 if ok,             b = root id */
    LORIE_TRACE_RELEASE,      /* renderer: compositor gave a slot back a = slot,                b = buffer id */
    LORIE_TRACE_HOLD,         /* renderer: frame held back             a = 1 incomplete, 2 no slot back, 3 lock; b = slot */
    LORIE_TRACE_CANCEL,       /* X: queued copy cancelled              a = 1 for a conflicting CPU write, b = serial */
    LORIE_TRACE_PREFLIGHT,    /* X: EXA fallback waited for the queue  a = result (below),  b = wait us */
    LORIE_TRACE_XLOCK,        /* X: shared lock taken for a CPU access a = wait us,             b = buffer id */
    LORIE_TRACE_ROOTCOPY,     /* X: CPU copy between root slots        a = us,                  b = bytes */
    LORIE_TRACE_REMAP,        /* X: root buffer unlocked and relocked  a = us,                  b = 0 */
    LORIE_TRACE_RLOCK,        /* renderer: shared lock taken (dated then) a = wait us before,   b = held us after */
};
/* LORIE_TRACE_PREFLIGHT results. Nothing is recorded when nothing was queued - the common case,
 * which would otherwise fill the ring during exactly the drags being looked at. A queued copy
 * cancelled for an overlapping write is its own event, LORIE_TRACE_CANCEL. */
enum { LORIE_PREFLIGHT_DRAINED = 1, LORIE_PREFLIGHT_TIMEOUT = 2, LORIE_PREFLIGHT_SKIP_LOCKED = 3,
       LORIE_PREFLIGHT_SKIP_STUCK = 4, LORIE_PREFLIGHT_NO_RENDERER = 5 };
/* Lock waits shorter than this are not recorded (XLOCK, RLOCK): there is one per CPU access to a
 * buffer with a copy pending, and the uncontended ones would crowd everything else out of the ring.
 * They are all still counted in presentStats. */
#define LORIE_TRACE_MIN_WAIT_US 100

#define LORIE_TRACE_RECORDS 4096

struct lorie_shared_server_state {
    /*
     * Renderer and X server are separated into 2 different processes.
     * Root window and cursor content and properties are shared across these 2 processes.
     * Reading/drawing root window in renderer the same time X server writes it can cause
     * tearing, texture garbling and other visual artifacts so we should block X server while we are drawing.
     */
    pthread_mutex_t lock; // initialized at X server side.
    pid_t lockingPid;

    /*
     * Single-producer (X server, present_execute_copy)/single-consumer (renderer) ring buffer
     * of deferred GPU copies to be applied to the root window texture before it is drawn to screen.
     * X server only ever advances writeIndex, renderer only ever advances readIndex and completedSerial.
     */
    struct {
        volatile uint32_t writeIndex;
        volatile uint32_t readIndex;
        /* The GPU has finished with every entry up to and including this serial - applied, given up
         * on or skipped alike. Advanced only after the batch's fence has signalled, and past every
         * entry drained, so it is a statement about buffer use and nothing else. Whether a given
         * entry was actually made is the failed list below. */
        volatile uint64_t completedSerial;

        /* Serials the renderer gave up on because a buffer never reached it. completedSerial is a
         * watermark and cannot express this: if 5 is abandoned and 6 succeeds, publishing 6 would
         * say 5 succeeded too. So they are listed. At most CAPACITY copies can be outstanding, but
         * cancelled ones wait here to be reaped as well, so the list is not guaranteed to outlast
         * every reader - hence failedLostUpTo. */
#define LORIE_GPU_COPY_FAILED_SLOTS 16
        volatile uint64_t failedSerials[LORIE_GPU_COPY_FAILED_SLOTS];
        volatile uint32_t failedCount;

        /* The highest serial whose entry above has been overwritten, published before the overwrite
         * happens. Without it, a serial pushed out of the list read as "not in the failed list",
         * which is indistinguishable from never having failed - and completedSerial, being a
         * watermark, would by then have stepped over it. The copy was then acked as made and the
         * client's pixmap released although the renderer had given up on it. A serial at or below
         * this and absent from the list is unknown, which is not the same as succeeded. */
        volatile uint64_t failedLostUpTo;
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];

        /* The only thing about a published entry that still changes: whether it is queued, claimed by
         * the renderer or cancelled by the X server (LORIE_JOB_*). Kept out of the entry itself,
         * because the renderer takes the entry with a plain struct copy - a field the other process
         * can write concurrently has no business in something read that way.
         *
         * Set to QUEUED by the X server before it publishes the entry, and only ever moved out of
         * QUEUED by a compare-and-swap. A slot is reused only after readIndex has moved past it, so a
         * state can never be confused with the next job's. */
        volatile uint32_t entryState[LORIE_GPU_COPY_QUEUE_CAPACITY];

        /* X server threads waiting on a futex for readIndex to move. The renderer only makes the wake
         * call when this is non-zero, so draining costs nothing extra when nobody is waiting. */
        volatile uint32_t readIndexWaiters;
    } gpuCopyQueue;

    /* ID of root window texture to be drawn. */
    uint64_t rootWindowTextureID;

    /*
     * Root window double buffering.
     *
     * With a single root buffer the renderer has to hold state->lock across its whole frame,
     * including the fence wait for the GPU to finish sampling the root texture, or the X server
     * would overwrite pixels the GPU is still reading. That makes every X server drawing operation
     * wait for a GPU round trip - measured at ~230 waits/s of ~0.85ms each on an ANGLE-backed
     * driver, which is what makes a dragged window advance unevenly.
     *
     * So the root gets several buffers: the renderer takes the one the X server published last, the
     * X server draws into one nobody else needs, and neither ever waits for the other.
     *
     * Five, because handing a buffer straight to the compositor means it keeps reading it until a
     * later one is latched, and both sides need room at once.
     *
     * The renderer holds up to three: the one on screen, and up to two waiting for the release
     * fence that says the compositor has finished with them - two because that fence arrives during
     * the vsync after the one that queued it, so insisting the older one be back first costs a
     * frame every time. The X server needs two: the one it is drawing into and one to move to when
     * it publishes. Take either side down to what looks sufficient and the publishing rate halves,
     * which on a 120Hz panel is plainly visible when a window is dragged.
     *
     * rootHandover carries all of it in one word, so the handover needs no mutex:
     *
     *   bits 0..4    one bit per slot, set while the renderer still needs that slot
     *   bits 5..7    the slot published most recently - what the renderer takes next
     *   bits 8..15   publish counter, for debugging; wraps within its own bits
     *   bits 16..31  which pool of buffers the rest is about - odd while the X server is replacing it
     *
     * Only the renderer writes the held bits and only the X server writes the published slot, but
     * both compare-and-swap the whole word, so neither can lose the other's update. The X server
     * publishes only when a slot is free for it to draw into next, and otherwise keeps drawing into
     * the one it has for another frame - so it never blocks, it just drops a frame the display could
     * not have shown anyway.
     *
     * The pool generation is in the same word for the same reason. Slots are named by index, and a
     * replaced pool reuses the indexes: a claim or a release made against the old pool must not
     * land on the new one. The X server marks the generation odd before it writes the new buffer
     * ids and makes it even again with the word it resets, so the renderer can tell a claim that
     * straddled a replacement - and a release, being a compare-and-swap of this word, cannot clear a
     * bit in a generation it was not made in.
     */
#define LORIE_ROOT_SLOTS 5
#define LORIE_ROOT_HELD_MASK 0x1fu
#define LORIE_ROOT_NEWEST_SHIFT 5
#define LORIE_ROOT_NEWEST_MASK 0x7u
#define LORIE_ROOT_COUNT_STEP 0x100u
#define LORIE_ROOT_COUNT_MASK 0xff00u
#define LORIE_ROOT_GEN_SHIFT 16
#define LORIE_ROOT_GEN(word) ((uint32_t) (word) >> LORIE_ROOT_GEN_SHIFT)

    volatile uint64_t rootBufferIds[LORIE_ROOT_SLOTS];

    /*
     * Renderer sessions. The X server numbers each connection and writes the number here before it
     * hands the state over; a renderer process notes it when it takes the state up.
     *
     * When a renderer lets go of the state - its connection broke, or it is about to take up a new
     * one - it first waits out the GPU work it has submitted (rendererRetireFrame), and then says so
     * in `retired`: for that session, every copy it took from the queue up to `serial` has finished
     * on the GPU, made or not. Nothing else can tell the X server that about a renderer whose socket
     * has gone, and that process may well still be alive. A ring with a sequence per entry, because
     * more than one process can let go of the state before the X server looks; entries are written
     * under `retiredClaim` and are complete once their seq is the claim number plus one.
     */
    volatile uint32_t sessionTag;
#define LORIE_RETIRE_RECORDS 4
    struct {
        volatile uint32_t seq;
        volatile uint32_t session;
        volatile int32_t pid;
        volatile uint64_t serial;
    } retired[LORIE_RETIRE_RECORDS];
    volatile uint32_t retiredClaim;
    volatile uint32_t rootHandover;
    volatile uint8_t rootDoubleBuffered;

    /* Which final output path to use, so the two can be compared in one build without also
     * changing the filtering, the buffer pool or the cursor path along the way.
     * 0 = choose per frame, 1 = always the GL blit, 2 = hand the root buffer over when it is
     * possible at all. Set from the X server's -output-backend argument. */
#define LORIE_OUTPUT_AUTO 0
#define LORIE_OUTPUT_GPU_COPY 1
#define LORIE_OUTPUT_ROOT_DIRECT 2
    volatile uint8_t outputBackend;

    /* A signal to renderer to update root window texture content from shared fragment if needed */
    volatile uint8_t drawRequested;

    /* We should avoid triggering renderer if there is no output surface */
    volatile uint8_t surfaceAvailable;

    /*
     * We do not want to block the X server for an extended period; ideally, we would avoid blocking it at all.
     * However, if we don’t block the X server, it will overwrite root window memory fragment, causing tearing or frame distortion.
     * On some devices, there is no way to make EGL/GLES2 render a frame without calling eglSwapBuffers;
     * calls like glFinish, eglWaitGL, and eglWaitClient have no effect.
     * The only way to force EGL to render a frame and flush the command queue is by invoking eglSwapBuffers.
     * But eglSwapBuffers will not return until Android actually displays the frame.
     * Since we want to proceed as quickly as possible, waiting for the frame to be shown is not acceptable.
     *
     * Therefore, we set eglSwapInterval(dpy, 1), so that eglSwapBuffers does not block until the frame is displayed.
     * Even then, we do not want to waste GPU resources rendering more than one full-screen quad per vsync,
     * because that would spend GPU time on a frame that will never be shown.
     * To handle this, we use a waitForNextFrame flag, which we set after a successful render and clear from the AChoreographer’s frame callback.
     */
    volatile uint8_t waitForNextFrame;

    /* Needed to show FPS counter in logcat */
    volatile int renderedFrames;

    /*
     * Renderer-side frame pacing counters. The renderer runs in the activity's process, whose
     * logcat needs root or adb to read, while the 5 second counter that consumes these runs in the
     * X server process, next to the terminal - publishing them here is what makes them readable at
     * all without a PC. The renderer only ever adds, the X server only ever resets; a lost update
     * right at the 5 second boundary is not worth synchronizing for.
     */
    struct {
        volatile uint32_t frameSamples;   /* frame-to-frame gaps measured */
        volatile uint64_t frameSumUs;     /* their sum, for the average */
        volatile uint32_t maxFrameUs;     /* the worst one */
        volatile uint32_t longFrames;     /* gaps >= LORIE_LONG_FRAME_US, i.e. a visible hitch */
        volatile uint32_t fenceWaitUs;    /* total time blocked on the root/present-copy fence */
        volatile uint32_t fenceWaitMaxUs; /* and the worst single one - a total hides a lone 100 ms wait */
        /* Submitting the copies, separate from waiting for them. Both used to be charged to
         * fenceWaitUs, which made every reading of "how long does the GPU take" include the cost of
         * handing it the work - the two move for entirely different reasons. */
        volatile uint32_t flushUs;
        /* Waits that had to fall back to glFinish because the fence could not be created or the
         * wait on it failed. Not an error we can ignore: without this the fallback is invisible and
         * a failed wait reads exactly like a fast one. */
        volatile uint32_t fenceFallbacks;
        volatile uint32_t gpuCopyFrames;  /* frames that carried at least one present copy */
        volatile uint32_t coalescedFrames;/* redraws the backpressure guard delayed */
        volatile uint32_t lockHeldUs;     /* renderer time holding state->lock, i.e. time the X
                                           * server's own drawing (loriePrepareAccess) can not run */
        /* Time the renderer spent getting the lock, as opposed to holding it. These were one number
         * measured from before the acquire, so a frame that waited 20 ms on the X server and held
         * the lock for 1 ms was indistinguishable from the reverse - and it is the reverse that
         * says the renderer is the one blocking the X server. */
        volatile uint32_t lockWaitUs;
        volatile uint32_t lockWaitMaxUs;
        volatile uint32_t cursorOnlyFrames;/* redraws with no damage at all: only the cursor moved */
        volatile uint32_t displayRefreshMHz;/* what the renderer paces to, milli-Hz */
        /* Written by the X server side, not the renderer, but reset together with the rest. */
        volatile uint32_t xLockWaitUs;    /* time the X server spent blocked on state->lock */
        volatile uint32_t xLockWaits;     /* how many of its accesses had to take that lock */
        volatile uint32_t xLockWaitMaxUs; /* worst single wait; 190 ms spread over 360 accesses and one
                                           * 190 ms wait look identical in the total */
        volatile uint32_t pointerMoves;   /* cursor motions fed to the renderer (each forces a frame) */
        volatile uint32_t cursorUploads;  /* cursor shape changes that reached the GPU */
        volatile uint32_t cursorUploadUs; /* and what they cost */
        volatile uint32_t rootRemapUs;    /* AHardwareBuffer unlock+lock of the root, per frame */
        volatile uint32_t rootRemaps;

        /* The CPU copy that brings a root slot up to date when it becomes the drawing target
         * again. Nothing counted it, so the cost of carrying more slots could not be seen. */
        volatile uint64_t rootCopyBytes;
        volatile uint32_t rootCopyUs;
        volatile uint32_t rootCopies;
        /* Every CPU framebuffer copy, by where it comes from - what there is to replace. The root
         * ones split rootCopyBytes: a handover carrying the drawing slot forward, an owed area
         * fetched into it later, the slots filled when the root becomes double buffered. The rest:
         * the old root copied into a resized one, a pixmap's memory moved into a buffer the GPU can
         * read, and presents the CPU drew because the GPU copy was turned down, by why
         * (LORIE_CPU_PRESENT_*). In bytes the CPU moved. */
        volatile uint64_t cpuCarryBytes;
        volatile uint64_t cpuOwedFetchBytes;
        volatile uint64_t cpuSeedBytes;
        volatile uint64_t cpuResizeBytes;
        volatile uint64_t cpuConvertBytes;
        volatile uint32_t cpuConverts;
        volatile uint64_t cpuPresentBytes;
        volatile uint32_t cpuPresents[LORIE_CPU_PRESENT_REASONS];
        /* Presents with more rects than a GPU copy takes, widened to fit (lorieWidenPresentRegion). */
        volatile uint32_t presentsWidened;
        /* Presents the GPU path had no room for, held back for the renderer to make some
         * (lorieMakeRoomForPresent): the waits, their time, and how many then went to the GPU after all. */
        volatile uint32_t presentRoomWaits;
        volatile uint32_t presentRoomWaitUs;
        volatile uint32_t presentRoomMade;
        volatile uint64_t cpuUnflipBytes;
        /* Every byte above and below, once: with a renderer there and the GPU path on - what has to
         * reach 0 - and apart from that, with no renderer to copy anything or GPU copies turned off. */
        volatile uint64_t cpuTotalBytes;
        volatile uint64_t cpuDegradedBytes;
        /* Of cpuTotalBytes, what the CPU copied with the renderer behind - the copy queue at least half
         * full, so the GPU could not be given it (cpuCarryKept BUSY) - rather than with nothing in its way. */
        volatile uint64_t cpuBehindBytes;
        /* The copies X core rendering makes with the CPU (EXA accelerates none), outside the contexts
         * above: calls, bytes, time and the longest, and of the bytes those written into the root,
         * those copied within one buffer, and those whose source and destination overlap. */
        volatile uint32_t coreCopyCalls[LORIE_CORE_COPY_KINDS];
        volatile uint64_t coreCopyBytes[LORIE_CORE_COPY_KINDS];
        volatile uint32_t coreCopyUs[LORIE_CORE_COPY_KINDS];
        volatile uint32_t coreCopyMaxUs[LORIE_CORE_COPY_KINDS];
        volatile uint64_t coreCopyRootBytes[LORIE_CORE_COPY_KINDS];
        volatile uint64_t coreCopySameBytes[LORIE_CORE_COPY_KINDS];
        volatile uint64_t coreCopyOverlapBytes[LORIE_CORE_COPY_KINDS];
        /* The same copies made by the GPU instead (lorieCoreCopyOnGpu): how many and their bytes, how
         * long the X server waited for them and the longest wait, and the ones kept on the CPU, by why
         * (LORIE_CORE_KEPT_*). */
        volatile uint32_t coreGpuCopies[LORIE_CORE_COPY_KINDS];
        volatile uint64_t coreGpuBytes[LORIE_CORE_COPY_KINDS];
        volatile uint32_t coreGpuWaitUs;
        volatile uint32_t coreGpuWaitMaxUs;
        volatile uint32_t coreGpuKept[LORIE_CORE_KEPT_REASONS];
        /* The CPU's mappings of the buffers those copies touched, let go of and taken again
         * (lorieRemapForGpu): how many times, and how long it took. */
        volatile uint32_t coreGpuRemaps;
        volatile uint32_t coreGpuRemapUs;
        /* The old root copied into a resized one by the GPU instead (lorieResizeCopyOnGpu): its bytes, and
         * the times it stayed with the CPU (cpuResizeBytes), by why (LORIE_CORE_KEPT_*). */
        volatile uint64_t resizeGpuBytes;
        volatile uint32_t resizeGpuKept[LORIE_CORE_KEPT_REASONS];
        /* The carry the GPU did instead (lorieRootCarryOnGpu): copies queued and their bytes, the ones
         * a CPU access took back before the renderer got to them - the CPU then fetched the area, which
         * is in cpuOwedFetchBytes - and the ones not made. cpuCarryKept is a handover whose carry
         * stayed with the CPU, by why (LORIE_CARRY_KEPT_*). */
        volatile uint32_t gpuCarryJobs;
        volatile uint64_t gpuCarryBytes;
        volatile uint32_t gpuCarryTakenBack;
        volatile uint32_t gpuCarryNotMade;
        volatile uint32_t cpuCarryKept[LORIE_CARRY_KEPT_REASONS];
        /* Owed areas given to the GPU instead of fetched by the CPU (lorieRootRepairOnGpu), and the EXA
         * fallbacks that waited for a queue holding only carries to drain - how long in total, and how
         * many ran out of time, after which the CPU takes the carries back. */
        volatile uint32_t gpuOwedRepairs;
        volatile uint32_t carryWaits;
        volatile uint32_t carryWaitUs;
        volatile uint32_t carryWaitTimeouts;
        /* Handovers that could not copy the whole stale area forward, because a queued GPU copy
         * had not written part of it yet; that part stays owed to the slot and goes across on a
         * later handover. It replaces a count of publishes held back entirely, which is what the
         * handover used to do and what could stop it publishing at all. */
        volatile uint32_t rootStalePostponed;

        /* Whether the screen is actually being updated, which nothing else here answers. Every
         * counter upstream can look healthy - clients presenting, copies completing, frames pacing
         * at the display rate - while nothing new reaches the screen. NoSlot is publishing faster
         * than the display can show it; UnpublishedMax is how long content stayed drawn but
         * unshown, which is the symptom itself. */
        volatile uint32_t rootPublishAttempts;
        volatile uint32_t rootPublishes;
        volatile uint32_t rootPublishNoSlot;
        /* A publish held back because the slot still lacked an area a previous handover had to leave
         * behind, and the copy that would supply it had not landed in the donor yet. rootOwedRepairs
         * is those areas being brought across. */
        volatile uint32_t rootPublishHeldForRepair;
        volatile uint32_t rootOwedRepairs;
        /* Copies queued into the drawing slot over an area it still owed. Queuing one used to take
         * the area out of the obligation, and one that then failed left it old for good. Now the
         * area stays owed until the copy is known to have been made: NotMade are those that were
         * not, and their area came from the donor after all. FromOlder is owed content fetched from
         * a slot further back, because the donor had gone out before its own copy over that area
         * failed. Lost is content no slot still held - which keeping the slots still needed from
         * reuse is there to rule out; should it happen anyway, the area stays owed, so it is not
         * published old, until its windows have been exposed (rootOwedExposed below).
         * ReplacingFull is copies refused - drawn by the CPU instead - with too many in flight. */
        volatile uint32_t rootReplacementsNotMade;
        volatile uint32_t rootOwedFromOlder;
        volatile uint32_t rootOwedLost;
        volatile uint32_t rootReplacingFull;
        /* Publishes held back because the only free slots held content the slot going out may still
         * need (lorieRootPinnedSlots), and owed areas no slot had any more that were exposed to have
         * their windows' content brought in again (lorieRootExposeLost) - never published old. */
        volatile uint32_t rootPublishHeldForDonor;
        volatile uint32_t rootOwedExposed;
        /* Waits that finished, measured where they ended, and the worst one still running. Sampling
         * a running wait once per vsync missed the stretch that mattered, because a handover that
         * succeeded cleared the outstanding mark first - so a publish one tick after a failed
         * attempt recorded a longest wait of zero. Both are the gap between the X server drawing a
         * slot and handing it on; neither says what reached the screen. */
        volatile uint32_t rootUnpublishedMaxUs;
        volatile uint32_t rootUnpublishedNowMaxUs;
        volatile uint32_t xDispatchMaxUs; /* longest gap between two X server redraw ticks */
        /* Vsync ticks the X server fell so far behind on that their records were overwritten before
         * it read them. The ticks are still counted in msc; only their individual times are lost. */
        volatile uint32_t vsyncRecordsLost;
        /* Longest gap between a frame's start, as the Choreographer reports it, and its callback
         * actually running here. 0 where the 64-bit frame time is not available. */
        volatile uint32_t vsyncDispatchMaxUs;
        /*
         * When a client's present actually reaches the screen, measured where present reports it as
         * completed. This is the one thing that matches what a person sees: the renderer can put out
         * a perfectly even 120 frames a second while the client content inside them arrives in
         * bursts, and then the picture stutters with every counter above looking healthy.
         */
        volatile uint32_t presentCompletions;
        volatile uint32_t presentGapSumUs;
        volatile uint32_t presentGapMaxUs;
        volatile uint32_t copyDeferrals;  /* copies left queued because a buffer was not registered yet */
        volatile uint32_t copySkips;      /* and the ones eventually given up on */
        volatile uint32_t presentGapsLate; /* gaps over two frame periods - the distribution, not
                                            * just the worst one, is what costs throughput */
        /*
         * The same thing measured one step earlier, where a client hands a frame to the X server.
         * A long gap here means the client stopped producing; a long gap only in the completion
         * numbers above means we are holding its frames up.
         */
        volatile uint32_t presentSubmits;
        volatile uint32_t submitGapMaxUs;

        /* How long an offloaded copy takes from being handed to the renderer to being acked back.
         * Without this the copy path can only be observed from outside, where a slow copy and a
         * client that simply did not send a frame look exactly the same. */
        volatile uint64_t copyLatencySumUs;
        volatile uint32_t copyLatencyMaxUs;
        volatile uint32_t copyCompletions;
        volatile uint32_t copyRequeues;   /* vblanks spent waiting for one to finish */

        /* How a copy ends when it does not end in an ack. A cancelled request whose copy was still
         * running (copyAbandons) keeps its buffers and its IdleNotify here until the renderer is
         * done; copyRecordExhausted is a copy that was never offered to the GPU because there was
         * no room to account for it, so the CPU path took it. Both are silent from outside - the
         * first looks like a client that stopped sending, the second like the GPU path not being
         * taken - which is why they are counted separately from copyCompletions. */
        volatile uint32_t copyAbandons;
        volatile uint32_t copyRecordExhausted;
        /* EXA fallbacks that waited for queued copies to drain before drawing (see
         * lorieExaFallbackBegin), how long in total, how many ran out of time, and how many could
         * not wait because a lock was already held or the head entry was known to be stuck. */
        volatile uint32_t exaPreflightWaits;
        volatile uint32_t exaPreflightWaitUs;
        volatile uint32_t exaPreflightTimeouts;
        volatile uint32_t exaPreflightSkipped;
        /* Queued copies cancelled because the X server was about to write the area they touch, and
         * copies the renderer had already claimed by then (which the shared lock then waited out). */
        volatile uint32_t copyCancelledForCpuWrite;
        volatile uint32_t copyClaimedBeforeCpuWrite;
        /* How copies whose session ended were let go of, apart from the ones their own live
         * session reported (see lorieCopyResolve):
         *   Retired   - the renderer process said, as it let go of the state, that everything it
         *               had taken from the queue had finished on the GPU;
         *   Dead      - the process that could still have touched them has exited;
         *   ForcedSettle - recovery, not completion: nothing could say whether that process was
         *               still alive, and the bounded wait ran out. */
        volatile uint32_t copySettledRetired;
        volatile uint32_t copySettledProcessDead;
        volatile uint32_t copyForcedSettle;

        /* When client requests actually arrive, which is the one hop everything else is measured
         * relative to. requestAheadMax is how many vsyncs ahead the furthest one asked to be shown:
         * a client whose idea of msc has drifted from ours asks for a frame far in the future and
         * then waits for it, which from outside looks exactly like a client that is simply slow. */
        volatile uint32_t requests;
        volatile uint32_t requestGapMaxUs;
        volatile uint32_t requestAheadMax;
        volatile uint32_t cursorOverlayMoves; /* pointer moves the overlay absorbed without any GL */
        /* What actually went out, counted as separate events rather than derived from each other.
         * The summary printed GL frames as total minus direct, and a tick that submitted nothing at
         * all was counted in the total - so a run that was entirely direct with a few no-submit
         * ticks reported most of its frames as having gone through GL. A failed swap was counted
         * as a frame too. */
        volatile uint32_t glOutputSubmits;        /* GL frames whose eglSwapBuffers succeeded */
        volatile uint32_t glOutputSubmitFailures; /* and the ones where it did not */
        volatile uint32_t directBufferSubmits;    /* a new root buffer put into a SurfaceControl transaction */
        volatile uint32_t directReuseNoSubmit;    /* nothing new to submit; the compositor keeps what it has */
        volatile uint32_t zeroCopyStalls;         /* frames held back because the previous buffer was not released */
        /* A frame not submitted because a copy into its own slot was still queued behind one the
         * drain had to wait on. What was on screen stays, and it is tried again next vsync. */
        volatile uint32_t directHeldIncomplete;
        /* Copies waiting because their destination slot is still on screen - kept apart from
         * copyDeferrals, which is a buffer that has not been imported yet. */
        volatile uint32_t copyWaitHeld;
        /* Release fences the compositor handed over that cannot be waited on. Each one costs the
         * pool a buffer for the rest of that pool's life, because nothing has said the compositor
         * is finished with it and time passing does not say so either. */
        volatile uint32_t zeroCopyFenceErrors;
        /* Slot releases that arrived after the X server had replaced the pool, and so named an index
         * that now belongs to a different buffer. Not released - see rendererReleaseRootSlot. */
        volatile uint32_t rootStaleSlotReleases;
        /* Claims of a root slot made again because the X server replaced the pool while the claim
         * was being made (see rendererClaimRootBuffer). */
        volatile uint32_t rootClaimsAcrossPools;
        /* Cursor image updates put off because every cursor buffer was still with the compositor
         * (see cursorPool in renderer.c); each is redone on a later frame. */
        volatile uint32_t cursorOverlayWaits;
    } presentStats;

    /*
     * GL_VENDOR | GL_RENDERER of the context the renderer actually draws with, published once so
     * the X server can log it. testCapabilities() runs its probe in the X server's own process,
     * which can be given a different GLES implementation than the activity (ANGLE is selected per
     * package), and it is the activity's one that decides what the present path costs.
     */
    volatile char rendererDriver[192];

    /* Which output path frames are actually taking, and when it is not the direct one, why. The
     * renderer decides this and used to only say so in its own logcat, which is out of reach
     * without root or adb - so from the terminal there was no way to tell a backend that was asked
     * for from the one that ran. Written by the renderer on a change, read by the X server for its
     * five-second summary. */
    /* The renderer has content it could not put on screen and needs another frame to try again.
     * Published because the X server is what opens the vsync gate, and its wake-up condition was
     * new damage or a cursor move - neither of which a retry is. Without this the renderer sat
     * waiting for a signal that was not coming, and the content stayed off the screen until
     * something else happened to dirty the root. Written by the renderer, read by the X server. */
    volatile uint8_t outputRetryPending;

    /* See LorieTraceRecord. traceHead is claimed by writers with a fetch-add, so any thread of either
     * process can write; traceTail belongs to the X server, which drains it. */
    volatile uint8_t traceEnabled;
    volatile uint64_t traceHead;
    volatile uint64_t traceTail;
    volatile uint32_t traceDropped;
    LorieTraceRecord trace[LORIE_TRACE_RECORDS];

    volatile uint8_t outputBackendActive;
    /* The scaling filter the user asked for. Published alongside the backend because the direct path
     * cannot honour nearest - the compositor scales bilinearly - so a comparison between the two
     * paths is only like for like when this says linear. */
    volatile uint8_t outputFilterNearest;
    volatile char outputBackendReason[96];

    struct {
        // We should not allow updating cursor content the same time renderer draws it.
        // locking the mutex protecting the root window can cause waiting for the frame to be drawn which is unacceptable
        pthread_mutex_t lock; // initialized at X server side.
        pid_t lockingPid;
        uint32_t x, y, xhot, yhot, width, height;
        uint32_t bits[LORIE_CURSOR_TEX_SIZE*LORIE_CURSOR_TEX_SIZE]; // 1 MiB, any cursor up to 512x512
        // Signals to renderer to update cursor's texture or its coordinates
        volatile uint8_t updated, moved;
    } cursor;
};

static int android_to_linux_keycode[304] = {
        [ 4   /* ANDROID_KEYCODE_BACK */] = KEY_ESC,
        [ 7   /* ANDROID_KEYCODE_0 */] = KEY_0,
        [ 8   /* ANDROID_KEYCODE_1 */] = KEY_1,
        [ 9   /* ANDROID_KEYCODE_2 */] = KEY_2,
        [ 10  /* ANDROID_KEYCODE_3 */] = KEY_3,
        [ 11  /* ANDROID_KEYCODE_4 */] = KEY_4,
        [ 12  /* ANDROID_KEYCODE_5 */] = KEY_5,
        [ 13  /* ANDROID_KEYCODE_6 */] = KEY_6,
        [ 14  /* ANDROID_KEYCODE_7 */] = KEY_7,
        [ 15  /* ANDROID_KEYCODE_8 */] = KEY_8,
        [ 16  /* ANDROID_KEYCODE_9 */] = KEY_9,
        [ 17  /* ANDROID_KEYCODE_STAR */] = KEY_KPASTERISK,
        [ 19  /* ANDROID_KEYCODE_DPAD_UP */] = KEY_UP,
        [ 20  /* ANDROID_KEYCODE_DPAD_DOWN */] = KEY_DOWN,
        [ 21  /* ANDROID_KEYCODE_DPAD_LEFT */] = KEY_LEFT,
        [ 22  /* ANDROID_KEYCODE_DPAD_RIGHT */] = KEY_RIGHT,
        [ 23  /* ANDROID_KEYCODE_DPAD_CENTER */] = KEY_ENTER,
        [ 24  /* ANDROID_KEYCODE_VOLUME_UP */] = KEY_VOLUMEUP, // XF86XK_AudioRaiseVolume
        [ 25  /* ANDROID_KEYCODE_VOLUME_DOWN */] = KEY_VOLUMEDOWN, // XF86XK_AudioLowerVolume
        [ 26  /* ANDROID_KEYCODE_POWER */] = KEY_POWER,
        [ 27  /* ANDROID_KEYCODE_CAMERA */] = KEY_CAMERA,
        [ 28  /* ANDROID_KEYCODE_CLEAR */] = KEY_CLEAR,
        [ 29  /* ANDROID_KEYCODE_A */] = KEY_A,
        [ 30  /* ANDROID_KEYCODE_B */] = KEY_B,
        [ 31  /* ANDROID_KEYCODE_C */] = KEY_C,
        [ 32  /* ANDROID_KEYCODE_D */] = KEY_D,
        [ 33  /* ANDROID_KEYCODE_E */] = KEY_E,
        [ 34  /* ANDROID_KEYCODE_F */] = KEY_F,
        [ 35  /* ANDROID_KEYCODE_G */] = KEY_G,
        [ 36  /* ANDROID_KEYCODE_H */] = KEY_H,
        [ 37  /* ANDROID_KEYCODE_I */] = KEY_I,
        [ 38  /* ANDROID_KEYCODE_J */] = KEY_J,
        [ 39  /* ANDROID_KEYCODE_K */] = KEY_K,
        [ 40  /* ANDROID_KEYCODE_L */] = KEY_L,
        [ 41  /* ANDROID_KEYCODE_M */] = KEY_M,
        [ 42  /* ANDROID_KEYCODE_N */] = KEY_N,
        [ 43  /* ANDROID_KEYCODE_O */] = KEY_O,
        [ 44  /* ANDROID_KEYCODE_P */] = KEY_P,
        [ 45  /* ANDROID_KEYCODE_Q */] = KEY_Q,
        [ 46  /* ANDROID_KEYCODE_R */] = KEY_R,
        [ 47  /* ANDROID_KEYCODE_S */] = KEY_S,
        [ 48  /* ANDROID_KEYCODE_T */] = KEY_T,
        [ 49  /* ANDROID_KEYCODE_U */] = KEY_U,
        [ 50  /* ANDROID_KEYCODE_V */] = KEY_V,
        [ 51  /* ANDROID_KEYCODE_W */] = KEY_W,
        [ 52  /* ANDROID_KEYCODE_X */] = KEY_X,
        [ 53  /* ANDROID_KEYCODE_Y */] = KEY_Y,
        [ 54  /* ANDROID_KEYCODE_Z */] = KEY_Z,
        [ 55  /* ANDROID_KEYCODE_COMMA */] = KEY_COMMA,
        [ 56  /* ANDROID_KEYCODE_PERIOD */] = KEY_DOT,
        [ 57  /* ANDROID_KEYCODE_ALT_LEFT */] = KEY_LEFTALT,
        [ 58  /* ANDROID_KEYCODE_ALT_RIGHT */] = KEY_RIGHTALT,
        [ 59  /* ANDROID_KEYCODE_SHIFT_LEFT */] = KEY_LEFTSHIFT,
        [ 60  /* ANDROID_KEYCODE_SHIFT_RIGHT */] = KEY_RIGHTSHIFT,
        [ 61  /* ANDROID_KEYCODE_TAB */] = KEY_TAB,
        [ 62  /* ANDROID_KEYCODE_SPACE */] = KEY_SPACE,
        [ 64  /* ANDROID_KEYCODE_EXPLORER */] = KEY_WWW,
        [ 65  /* ANDROID_KEYCODE_ENVELOPE */] = KEY_MAIL,
        [ 66  /* ANDROID_KEYCODE_ENTER */] = KEY_ENTER,
        [ 67  /* ANDROID_KEYCODE_DEL */] = KEY_BACKSPACE,
        [ 68  /* ANDROID_KEYCODE_GRAVE */] = KEY_GRAVE,
        [ 69  /* ANDROID_KEYCODE_MINUS */] = KEY_MINUS,
        [ 70  /* ANDROID_KEYCODE_EQUALS */] = KEY_EQUAL,
        [ 71  /* ANDROID_KEYCODE_LEFT_BRACKET */] = KEY_LEFTBRACE,
        [ 72  /* ANDROID_KEYCODE_RIGHT_BRACKET */] = KEY_RIGHTBRACE,
        [ 73  /* ANDROID_KEYCODE_BACKSLASH */] = KEY_BACKSLASH,
        [ 74  /* ANDROID_KEYCODE_SEMICOLON */] = KEY_SEMICOLON,
        [ 75  /* ANDROID_KEYCODE_APOSTROPHE */] = KEY_APOSTROPHE,
        [ 76  /* ANDROID_KEYCODE_SLASH */] = KEY_SLASH,
        [ 81  /* ANDROID_KEYCODE_PLUS */] = KEY_KPPLUS,
        [ 82  /* ANDROID_KEYCODE_MENU */] = KEY_CONTEXT_MENU,
        [ 84  /* ANDROID_KEYCODE_SEARCH */] = KEY_SEARCH,
        [ 85  /* ANDROID_KEYCODE_MEDIA_PLAY_PAUSE */] = KEY_PLAYPAUSE,
        [ 86  /* ANDROID_KEYCODE_MEDIA_STOP */] = KEY_STOP_RECORD,
        [ 87  /* ANDROID_KEYCODE_MEDIA_NEXT */] = KEY_NEXTSONG,
        [ 88  /* ANDROID_KEYCODE_MEDIA_PREVIOUS */] = KEY_PREVIOUSSONG,
        [ 89  /* ANDROID_KEYCODE_MEDIA_REWIND */] = KEY_REWIND,
        [ 90  /* ANDROID_KEYCODE_MEDIA_FAST_FORWARD */] = KEY_FASTFORWARD,
        [ 91  /* ANDROID_KEYCODE_MUTE */] = KEY_MUTE,
        [ 92  /* ANDROID_KEYCODE_PAGE_UP */] = KEY_PAGEUP,
        [ 93  /* ANDROID_KEYCODE_PAGE_DOWN */] = KEY_PAGEDOWN,
        [ 111  /* ANDROID_KEYCODE_ESCAPE */] = KEY_ESC,
        [ 112  /* ANDROID_KEYCODE_FORWARD_DEL */] = KEY_DELETE,
        [ 113  /* ANDROID_KEYCODE_CTRL_LEFT */] = KEY_LEFTCTRL,
        [ 114  /* ANDROID_KEYCODE_CTRL_RIGHT */] = KEY_RIGHTCTRL,
        [ 115  /* ANDROID_KEYCODE_CAPS_LOCK */] = KEY_CAPSLOCK,
        [ 116  /* ANDROID_KEYCODE_SCROLL_LOCK */] = KEY_SCROLLLOCK,
        [ 117  /* ANDROID_KEYCODE_META_LEFT */] = KEY_LEFTMETA,
        [ 118  /* ANDROID_KEYCODE_META_RIGHT */] = KEY_RIGHTMETA,
        [ 120  /* ANDROID_KEYCODE_SYSRQ */] = KEY_PRINT,
        [ 121  /* ANDROID_KEYCODE_BREAK */] = KEY_BREAK,
        [ 122  /* ANDROID_KEYCODE_MOVE_HOME */] = KEY_HOME,
        [ 123  /* ANDROID_KEYCODE_MOVE_END */] = KEY_END,
        [ 124  /* ANDROID_KEYCODE_INSERT */] = KEY_INSERT,
        [ 125  /* ANDROID_KEYCODE_FORWARD */] = KEY_FORWARD,
        [ 126  /* ANDROID_KEYCODE_MEDIA_PLAY */] = KEY_PLAYCD,
        [ 127  /* ANDROID_KEYCODE_MEDIA_PAUSE */] = KEY_PAUSECD,
        [ 128  /* ANDROID_KEYCODE_MEDIA_CLOSE */] = KEY_CLOSECD,
        [ 129  /* ANDROID_KEYCODE_MEDIA_EJECT */] = KEY_EJECTCD,
        [ 130  /* ANDROID_KEYCODE_MEDIA_RECORD */] = KEY_RECORD,
        [ 131  /* ANDROID_KEYCODE_F1 */] = KEY_F1,
        [ 132  /* ANDROID_KEYCODE_F2 */] = KEY_F2,
        [ 133  /* ANDROID_KEYCODE_F3 */] = KEY_F3,
        [ 134  /* ANDROID_KEYCODE_F4 */] = KEY_F4,
        [ 135  /* ANDROID_KEYCODE_F5 */] = KEY_F5,
        [ 136  /* ANDROID_KEYCODE_F6 */] = KEY_F6,
        [ 137  /* ANDROID_KEYCODE_F7 */] = KEY_F7,
        [ 138  /* ANDROID_KEYCODE_F8 */] = KEY_F8,
        [ 139  /* ANDROID_KEYCODE_F9 */] = KEY_F9,
        [ 140  /* ANDROID_KEYCODE_F10 */] = KEY_F10,
        [ 141  /* ANDROID_KEYCODE_F11 */] = KEY_F11,
        [ 142  /* ANDROID_KEYCODE_F12 */] = KEY_F12,
        [ 143  /* ANDROID_KEYCODE_NUM_LOCK */] = KEY_NUMLOCK,
        [ 144  /* ANDROID_KEYCODE_NUMPAD_0 */] = KEY_KP0,
        [ 145  /* ANDROID_KEYCODE_NUMPAD_1 */] = KEY_KP1,
        [ 146  /* ANDROID_KEYCODE_NUMPAD_2 */] = KEY_KP2,
        [ 147  /* ANDROID_KEYCODE_NUMPAD_3 */] = KEY_KP3,
        [ 148  /* ANDROID_KEYCODE_NUMPAD_4 */] = KEY_KP4,
        [ 149  /* ANDROID_KEYCODE_NUMPAD_5 */] = KEY_KP5,
        [ 150  /* ANDROID_KEYCODE_NUMPAD_6 */] = KEY_KP6,
        [ 151  /* ANDROID_KEYCODE_NUMPAD_7 */] = KEY_KP7,
        [ 152  /* ANDROID_KEYCODE_NUMPAD_8 */] = KEY_KP8,
        [ 153  /* ANDROID_KEYCODE_NUMPAD_9 */] = KEY_KP9,
        [ 154  /* ANDROID_KEYCODE_NUMPAD_DIVIDE */] = KEY_KPSLASH,
        [ 155  /* ANDROID_KEYCODE_NUMPAD_MULTIPLY */] = KEY_KPASTERISK,
        [ 156  /* ANDROID_KEYCODE_NUMPAD_SUBTRACT */] = KEY_KPMINUS,
        [ 157  /* ANDROID_KEYCODE_NUMPAD_ADD */] = KEY_KPPLUS,
        [ 158  /* ANDROID_KEYCODE_NUMPAD_DOT */] = KEY_KPDOT,
        [ 159  /* ANDROID_KEYCODE_NUMPAD_COMMA */] = KEY_KPCOMMA,
        [ 160  /* ANDROID_KEYCODE_NUMPAD_ENTER */] = KEY_KPENTER,
        [ 161  /* ANDROID_KEYCODE_NUMPAD_EQUALS */] = KEY_KPEQUAL,
        [ 162  /* ANDROID_KEYCODE_NUMPAD_LEFT_PAREN */] = KEY_KPLEFTPAREN,
        [ 163  /* ANDROID_KEYCODE_NUMPAD_RIGHT_PAREN */] = KEY_KPRIGHTPAREN,
        [ 164  /* ANDROID_KEYCODE_VOLUME_MUTE */] = KEY_MUTE,
        [ 165  /* ANDROID_KEYCODE_INFO */] = KEY_INFO,
        [ 166  /* ANDROID_KEYCODE_CHANNEL_UP */] = KEY_CHANNELUP,
        [ 167  /* ANDROID_KEYCODE_CHANNEL_DOWN */] = KEY_CHANNELDOWN,
        [ 168  /* ANDROID_KEYCODE_ZOOM_IN */] = KEY_ZOOMIN,
        [ 169  /* ANDROID_KEYCODE_ZOOM_OUT */] = KEY_ZOOMOUT,
        [ 170  /* ANDROID_KEYCODE_TV */] = KEY_TV,
        [ 208  /* ANDROID_KEYCODE_CALENDAR */] = KEY_CALENDAR,
        [ 210  /* ANDROID_KEYCODE_CALCULATOR */] = KEY_CALC,
};

static inline __always_inline void lorieTraceAt(struct lorie_shared_server_state *st, uint32_t kind,
                                                 uint32_t a, uint64_t b, uint64_t tUs) {
    uint64_t idx = __atomic_fetch_add(&st->traceHead, 1, __ATOMIC_RELAXED);
    LorieTraceRecord *r = &st->trace[idx % LORIE_TRACE_RECORDS];

    /*
     * A seqlock write: seq is cleared, then the fields change, then seq is set. The fence is what makes
     * that order visible to the reader. Without it a weakly ordered CPU may let the new fields be seen
     * before seq is cleared, and a reader that read the old seq would then accept the new fields as the
     * old record - which the host test caught as a record turning up twice and out of order.
     */
    __atomic_store_n(&r->seq, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&r->tUs, tUs, __ATOMIC_RELAXED);
    __atomic_store_n(&r->kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&r->a, a, __ATOMIC_RELAXED);
    __atomic_store_n(&r->b, b, __ATOMIC_RELAXED);
    __atomic_store_n(&r->seq, idx + 1, __ATOMIC_RELEASE);
}
