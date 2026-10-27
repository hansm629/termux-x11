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
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/futex.h>
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
void lorieQueueUrgentWorkProc(Bool (*proc)(struct _Client *, void *), void *closure);
void lorieRecheckGpuCopies(void);
void lorieCountCopyAck(void);
uint32_t lorieGpuCopyDoneReceived(uint64_t publishedUs, uint64_t serial);
void lorieGpuCopyDoneHandled(uint32_t receivedUs);
void lorieCountGpuCopyEarly(int how);
int lorieGpuCopySettle(uint64_t serial);
void lorieCountGpuCopyRetired(int made);
void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d);
void lorieActivityConnected(void);
void lorieSendSharedServerState(int memfd);
void lorieRegisterBuffer(LorieBuffer* buffer);
void lorieUnregisterBuffer(LorieBuffer* buffer);
bool lorieConnectionAlive(void);
void lorieRendererDisconnected(void);
extern bool lorieDebugEnabled; // Set in activity.c's startLogcat, only called when TERMUX_X11_DEBUG=1.
void lorieSetRendererWakeupCond(int fd);
int rendererGetWakeupCondFd(void);

__unused void rendererInit(JNIEnv* env);
__unused void rendererSetFiltering(JNIEnv* env, jobject self, jint filtering);
__unused void rendererSetSmoothPresentationEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetDisplayRefreshRate(JNIEnv* env, jobject self, jfloat refreshRate);
__unused void rendererSetPerfLogEnabled(JNIEnv* env, jobject self, jboolean enabled);
__unused void rendererSetGpuPerformanceMode(JNIEnv* env, jobject self, jint mode);
__unused void rendererSetGpuPerformanceForeground(JNIEnv* env, jobject self, jboolean foreground);
__unused void rendererSetGpuPerformanceThermal(JNIEnv* env, jobject self, jint status);
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

static inline __always_inline void lorie_mutex_lock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    // Unfortunately there is no robust mutexes in bionic.
    // Posix does not define any valid way to unlock stuck non-robust mutex
    // so in the case if renderer or X server process unexpectedly die with locked mutex
    // we will simply reinitialize it.
    struct timespec ts = {0};
    while(true) {
        // P11-C3-C: pthread_mutex_timedlock's deadline is on CLOCK_REALTIME (POSIX; bionic's pthread.h says so too: only
        // pthread_mutex_timedlock_monotonic_np, API 28, takes CLOCK_MONOTONIC). A CLOCK_MONOTONIC one is long past on
        // that clock, so each wait ended at once - a busy loop of timed-out locks instead of a sleep until the unlock,
        // and a lock left by a dead process reinitialised without the 33 msec. A jump of the clock only moves when the
        // next of those checks is made: an unlock ends the wait whatever the clock says.
        clock_gettime(CLOCK_REALTIME, &ts);

        // 33 msec is enough to complete any drawing operation on both X server and renderer side
        // In the case if mutex is locked most likely other thread died with the mutex locked
        ts.tv_nsec += 33UL * 1000000UL;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec  += ts.tv_nsec / 1000000000L;
            ts.tv_nsec  = ts.tv_nsec % 1000000000L;
        }

        int ret = pthread_mutex_timedlock(mutex, &ts);
        if (ret == ETIMEDOUT) {
            if (*lockingPid == getpid() || lorieConnectionAlive())
                continue;

            pthread_mutexattr_t attr;
            pthread_mutex_t initializer = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            memcpy(mutex, &initializer, sizeof(initializer));
            pthread_mutex_init(mutex, &attr);
            // Mutex will be locked fine on the next iteration
        } else {
            *lockingPid = getpid();
            return;
        }
    }
}

// The lock if no one holds it, without waiting (P10-S1: what the X server may ask about without waiting for the renderer).
static inline __always_inline bool lorie_mutex_trylock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    if (pthread_mutex_trylock(mutex))
        return false;
    *lockingPid = getpid();
    return true;
}

static inline __always_inline void lorie_mutex_unlock(pthread_mutex_t* mutex, pid_t* lockingPid) {
    *lockingPid = 0;
    pthread_mutex_unlock(mutex);
}

/*
 * P11-C3-C: the renderer's wake - an event count in the region the renderer makes and hands the X server
 * (rendererGetWakeupCondFd, lorieSetRendererWakeupCond). Whoever gives the renderer work publishes it first, then
 * calls lorieWakeRenderer: the count moves (release) and a renderer sleeping on it is woken (a futex on a shared
 * mapping: across the two processes). The renderer reads the count (lorieWakeSeen, acquire) before it looks for work
 * and sleeps only while the count is still what it read - FUTEX_WAIT compares it under the futex's own lock, and a
 * wake after that compare finds the renderer queued. So work published after the look moved the count and the sleep
 * returns at once, and work published before it is seen by the look. A condition variable signalled without its
 * mutex, as the X server did (it cannot hold the renderer's process-local stateLock), loses a signal sent between the
 * look and pthread_cond_wait: the wait reads the condition's state only once it is entered.
 */
struct lorie_renderer_wake {
    uint32_t seq;
};

static inline __always_inline void lorieWakeRenderer(volatile struct lorie_renderer_wake *w) {
    __atomic_fetch_add(&w->seq, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &w->seq, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static inline __always_inline uint32_t lorieWakeSeen(volatile struct lorie_renderer_wake *w) {
    return __atomic_load_n(&w->seq, __ATOMIC_ACQUIRE);
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
    struct {
        uint8_t t;
        uint64_t publishedUs;   /* the Vulkan renderer: when it published completedSerial (lorieMonotonicUs); GLES: 0 */
        uint64_t serial;        /* P11-C3-C: the Vulkan renderer: the completedSerial it published; GLES: 0 */
    } gpuCopyDone;
} lorieEvent;

typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;

/* Two 60Hz frames: a gap this long is a hitch a user can see, not just a missed vsync. */
#define LORIE_LONG_FRAME_US 33000

#define LORIE_GPU_COPY_MAX_RECTS 16
#define LORIE_GPU_COPY_QUEUE_CAPACITY 8
/* XRender's unmasked Porter-Duff subset. Present always uses SRC. Saturate and the
 * Conjoint/Disjoint operators require programmable per-pixel factors, so still use pixman. */
#define LORIE_GPU_COPY_OP_SRC  0u
#define LORIE_GPU_COPY_OP_OVER 1u
#define LORIE_GPU_COPY_OP_CLEAR 2u
#define LORIE_GPU_COPY_OP_OVER_REVERSE 3u
#define LORIE_GPU_COPY_OP_IN 4u
#define LORIE_GPU_COPY_OP_IN_REVERSE 5u
#define LORIE_GPU_COPY_OP_OUT 6u
#define LORIE_GPU_COPY_OP_OUT_REVERSE 7u
#define LORIE_GPU_COPY_OP_ATOP 8u
#define LORIE_GPU_COPY_OP_ATOP_REVERSE 9u
#define LORIE_GPU_COPY_OP_XOR 10u
#define LORIE_GPU_COPY_OP_ADD 11u
#define LORIE_GPU_COPY_OP_COUNT 12u

enum lorie_gpu_factor {
    LORIE_FACTOR_ZERO, LORIE_FACTOR_ONE, LORIE_FACTOR_SRC_ALPHA,
    LORIE_FACTOR_ONE_MINUS_SRC_ALPHA, LORIE_FACTOR_DST_ALPHA, LORIE_FACTOR_ONE_MINUS_DST_ALPHA
};

/* Both Vulkan and GLES use the same premultiplied Porter-Duff factors, including alpha.
 * Non-premultiplied source-over (used for the mouse cursor) is a distinct pipeline. */
static inline bool lorieGpuCompositeFactors(uint8_t op, uint8_t *src, uint8_t *dst) {
    switch (op) {
        case LORIE_GPU_COPY_OP_SRC:          *src = LORIE_FACTOR_ONE;                 *dst = LORIE_FACTOR_ZERO; break;
        case LORIE_GPU_COPY_OP_OVER:         *src = LORIE_FACTOR_ONE;                 *dst = LORIE_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_CLEAR:        *src = LORIE_FACTOR_ZERO;                *dst = LORIE_FACTOR_ZERO; break;
        case LORIE_GPU_COPY_OP_OVER_REVERSE: *src = LORIE_FACTOR_ONE_MINUS_DST_ALPHA; *dst = LORIE_FACTOR_ONE; break;
        case LORIE_GPU_COPY_OP_IN:           *src = LORIE_FACTOR_DST_ALPHA;           *dst = LORIE_FACTOR_ZERO; break;
        case LORIE_GPU_COPY_OP_IN_REVERSE:   *src = LORIE_FACTOR_ZERO;                *dst = LORIE_FACTOR_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_OUT:          *src = LORIE_FACTOR_ONE_MINUS_DST_ALPHA; *dst = LORIE_FACTOR_ZERO; break;
        case LORIE_GPU_COPY_OP_OUT_REVERSE:  *src = LORIE_FACTOR_ZERO;                *dst = LORIE_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_ATOP:         *src = LORIE_FACTOR_DST_ALPHA;           *dst = LORIE_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_ATOP_REVERSE: *src = LORIE_FACTOR_ONE_MINUS_DST_ALPHA; *dst = LORIE_FACTOR_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_XOR:          *src = LORIE_FACTOR_ONE_MINUS_DST_ALPHA; *dst = LORIE_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case LORIE_GPU_COPY_OP_ADD:          *src = LORIE_FACTOR_ONE;                 *dst = LORIE_FACTOR_ONE; break;
        default: return false;
    }
    return true;
}
#define LORIE_GPU_COPY_NOT_RECORDED 0u
#define LORIE_GPU_COPY_RECORDED     1u
#define LORIE_GPU_COPY_SKIPPED      2u
#define LORIE_GPU_IN_FLIGHT_MAX 32      /* AHBs one Vulkan frame may use (vk.h LORIE_VK_MAX_FRAME_IMAGES) */
#define LORIE_CPU_ACCESS_MAX 8          /* P11-C3-B1: AHBs the X server may hold reserved at once (cpuAccess) */
#define LORIE_GPU_IN_FLIGHT_MAIN 1u     /* gpuInFlight.busy: the renderer's (first) submission's list, ids */
#define LORIE_GPU_IN_FLIGHT_AUX 2u      /* P11-C3-B2: its second submission's (a root copy's), auxIds */

typedef struct {
    uint64_t serial;
    uint64_t srcBufferId;
    uint64_t dstBufferId;
    uint64_t queuedUs;      /* CLOCK_MONOTONIC microseconds when the X server queued it (lorieMonotonicUs) */
    int16_t xOff, yOff;
    uint16_t numRects;
    uint8_t dstDepth;       /* the destination's depth: 32 keeps alpha in its RGBX 4th byte, below that it is padding */
    uint8_t cancelled;      /* X, under the shared lock: its presentation was let go before any renderer recorded it -
                             * the renderers (which record under that lock) skip it, its serial completed as any other's
                             * (InitOutput.c lorieGpuCopySettle, P10-S1) */
    uint8_t operator;       /* P11-C4: SRC or OVER; OVER uses premultiplied-alpha GPU blending */
    uint8_t recorded;       /* renderer: RECORDED or SKIPPED; read by X only after completedSerial's release store */
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
} LorieGpuCopyEntry;

/*
 * What renderer backend was asked for, what actually ran, and the proof of it.
 *
 * The renderer (renderer.c, in the activity's process) writes everything here except the fields marked
 * X; the X server writes those and only reads the rest, which it logs every 5 seconds (XlorieBackend,
 * XlorieGles, XlorieVk, XlorieFallback lines) - the renderer's own logcat cannot be read without root or
 * adb, the X server's can. tools/diag/renderer-proof.sh judges a run from those lines.
 *
 * Every counter is cumulative and never reset, and is moved only right after the API call it names has
 * returned success (or, for the fallback counters, right after the failure it records). Nothing here is
 * set from a setting: a backend can only be shown as used by counters that only its own successful calls
 * move.
 */
enum { LORIE_ACTIVE_NONE = 0, LORIE_ACTIVE_GLES = 1, LORIE_ACTIVE_VULKAN = 2 };

/*
 * Which renderer backend is wanted (X: TERMUX_X11_RENDERER_BACKEND = auto | vulkan | gles, auto when unset),
 * and the handshake that moves the X server's CPU ownership of AHardwareBuffers with the backend that is
 * actually active - never with the one requested.
 *
 *   PERSISTENT  3c98ffa: the X server keeps every AHB pixmap it allocates, converts for the GPU or puts on
 *               the root window CPU-locked for as long as 3c98ffa does; a CPU access to an AHB takes the
 *               shared lock only where 3c98ffa did (lorieNeedsGpuLock).
 *   PER_ACCESS  for Vulkan: no AHB stays CPU-locked; every CPU access to an AHB pixmap locks it at
 *               PrepareAccess and unlocks it (synchronously) at FinishAccess, under the shared lock - which
 *               the renderer holds for the whole of its use of an AHB - so the X server's CPU ownership and
 *               the renderer's never overlap.
 *
 * The renderer asks: ownershipRequest = (seq << 8) | mode, seq one past the last one acknowledged. The X
 * server takes the request up on its main thread (lorieRedraw, every frame), and under the shared lock moves
 * its locks and answers with ownershipAck = (seq << 8) | the mode now in effect - which is PERSISTENT when it
 * refused PER_ACCESS (a session asked for gles never leaves PERSISTENT). Since the locks and the answer only
 * change together under that lock, the renderer may use AHBs from Vulkan in a frame only when, holding the
 * lock, it reads its own request answered with PER_ACCESS; it asks for PERSISTENT again once Vulkan work has
 * stopped and its fences have signalled. The X server also returns to PERSISTENT by itself (answering
 * whatever was asked) when the renderer's connection goes away or a new one is made.
 */
enum { LORIE_BACKEND_AUTO = 0, LORIE_BACKEND_VULKAN = 1, LORIE_BACKEND_GLES = 2 };
enum { LORIE_OWNERSHIP_PERSISTENT = 0, LORIE_OWNERSHIP_PER_ACCESS = 1 };
#define LORIE_OWNERSHIP_WORD(seq, mode) (((uint32_t) (seq) << 8) | ((uint32_t) (mode) & 0xffu))
#define LORIE_OWNERSHIP_SEQ(word) ((uint32_t) (word) >> 8)
#define LORIE_OWNERSHIP_MODE(word) ((uint8_t) ((word) & 0xffu))

/*
 * Fault injection for the Vulkan backend (X: TERMUX_X11_RENDERER_VK_FAIL_AT = instance | device | surface |
 * swapchain | import | submit | present, none when unset): at that point the renderer does not make the call
 * and handles it as failed, recording the fallback with class INJECTED and the point's own reason.
 */
#define LORIE_VK_FAIL_POINTS(X) X(NONE) X(INSTANCE) X(DEVICE) X(SURFACE) X(SWAPCHAIN) X(IMPORT) X(SUBMIT) X(PRESENT)
#define LORIE_ENUM_FAIL_AT(n) LORIE_VK_FAIL_AT_ ## n,
enum { LORIE_VK_FAIL_POINTS(LORIE_ENUM_FAIL_AT) LORIE_VK_FAIL_AT_COUNT };
#undef LORIE_ENUM_FAIL_AT

struct lorie_backend_control {
    volatile uint8_t requested;                 /* X: LORIE_BACKEND_* */
    volatile uint8_t requestedInvalid;          /* X: the variable was set, to none of the three */
    volatile uint8_t vkFailAt;                  /* X: LORIE_VK_FAIL_AT_* */
    volatile uint8_t vkFailAtInvalid;           /* X: the variable was set, to none of the seven */
    volatile uint8_t vkSelftestFull;            /* X: TERMUX_X11_RENDERER_VK_SELFTEST=full - the full ownership self-test */
    volatile uint8_t vkSelftestInvalid;         /* X: the variable was set, to something else */
    volatile uint8_t vkTransferCopy;            /* X: TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1 - experimental: Present copies
                                                 * by vkCmdCopyImage where that gives what the shader copy gives */
    volatile uint8_t vkFrameDiag;               /* X: TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 - P11-C3's timings (lorie_hist) */
    volatile uint8_t vkScheduleOpt;             /* X: TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1 - P11-C3-B2 (renderer.c) */
    volatile uint8_t vkSubmitOpt;               /* X: TERMUX_X11_RENDERER_VK_SUBMIT_OPT=1 - experimental (renderer.c
                                                 * rendererVkRedraw): the copies queued go ahead of a frame, in a
                                                 * submission of their own, in every present mode; a frame with none of
                                                 * them is presented before its fence is waited for */
    volatile uint8_t vkPreferImmediate;         /* X: TERMUX_X11_RENDERER_VK_PRESENT_IMMEDIATE=1: original WSI
                                                 * IMMEDIATE preference, otherwise MAILBOX where supported */
    volatile uint8_t vkEarlyPresent;            /* X: no-copy screen frame can queue present before CPU
                                                 * fence wait. Default enabled, 0 restores serialized legacy. */
    volatile uint8_t freezeDiag;                /* X: TERMUX_X11_FRAME_FREEZE_DIAG=1 - P11-C3-C's freeze watchdog: the
                                                 * renderer keeps its stage (struct lorie_progress) */
    volatile uint8_t contentDiag;               /* X: TERMUX_X11_CONTENT_FRAME_DIAG=1 - P11-C3-D0: client content from
                                                 * its request to the screen (struct lorie_content) */
    volatile uint8_t rootCopySchedOpt;          /* X: TERMUX_X11_ROOT_COPY_SCHED_OPT=1 - P11-C3-D2: the root copy
                                                 * submitted ahead (B2) settled as soon as it may be (renderer.c) */
    volatile uint8_t xrenderGpu;               /* X: TERMUX_X11_XRENDER_GPU=1 - opt-in premultiplied Over pipeline */
    volatile uint32_t ownershipRequest;         /* renderer */
    volatile uint32_t ownershipAck;             /* X */
};

#define LORIE_FALLBACK_CLASSES(X) X(FATAL_BACKEND) X(RESOURCE_LOCAL) X(INJECTED)
#define LORIE_FALLBACK_REASONS(X) \
    X(NONE) X(VK_LOADER) X(VK_INSTANCE) X(VK_NO_DEVICE) X(VK_EXT_ANDROID_SURFACE) X(VK_EXT_SWAPCHAIN) \
    X(VK_EXT_AHB) X(VK_EXT_FOREIGN) X(VK_DEVICE) X(VK_SELFTEST) X(VK_SURFACE) X(VK_SURFACE_FORMAT) \
    X(VK_SWAPCHAIN) X(VK_ROOT_NOT_AHB) X(VK_OWNERSHIP) X(VK_DEVICE_LOST) X(VK_SUBMIT) X(VK_PRESENT) \
    X(AHB_PROPERTIES) X(AHB_EXTERNAL_FORMAT_UNSUPPORTED_BY_STAGE1) X(AHB_FORMAT_UNSUPPORTED_BY_STAGE1) \
    X(AHB_IMAGE_USAGE_UNSUPPORTED) X(AHB_MEMORY_TYPE) X(AHB_IMPORT) X(VK_VERSION)
#define LORIE_VK_SELFTEST_STEPS(X) X(NONE) X(SETUP) X(ALLOCATE) X(IMPORT) X(CPU_LOCK) X(SUBMIT) X(FENCE) X(COMPARE)
#define LORIE_ENUM_STEP(n) LORIE_VK_SELFTEST_STEP_ ## n,
enum { LORIE_VK_SELFTEST_STEPS(LORIE_ENUM_STEP) LORIE_VK_SELFTEST_STEP_COUNT };
#undef LORIE_ENUM_STEP
#define LORIE_ENUM_CLASS(n) LORIE_FALLBACK_CLASS_ ## n,
#define LORIE_ENUM_REASON(n) LORIE_FALLBACK_ ## n,
enum { LORIE_FALLBACK_CLASSES(LORIE_ENUM_CLASS) LORIE_FALLBACK_CLASS_COUNT };
enum { LORIE_FALLBACK_REASONS(LORIE_ENUM_REASON) LORIE_FALLBACK_REASON_COUNT };
#undef LORIE_ENUM_CLASS
#undef LORIE_ENUM_REASON
/* TERMUX_X11_RENDERER_VK_TRANSFER_COPY: the conditions that keep a queued Present copy from vkCmdCopyImage
 * (renderer.c rendererVkCopyRegions; RECT_SOURCE a rectangle empty or past its source, RECT_OVERLAP two
 * destinations overlapping, RECT_NONE nothing left inside the destination), and what an import got of the transfer usage (vk.c lorieVkImport): given, not
 * asked for (imported while no copy asked for it), the format's features without it, refused by the AHB's
 * image-format query, the image made with it failing */
#define LORIE_VK_XFER_NOTS(X) X(SAME) X(FORMAT) X(RGBX_ALPHA) X(DEPTH32) X(SRC_USAGE) X(DST_USAGE) X(RECT_SOURCE) \
    X(RECT_OVERLAP) X(RECT_NONE)
#define LORIE_VK_XFER_USAGES(X) X(GRANTED) X(UNASKED) X(NO_FEATURE) X(REFUSED) X(FAILED)
/* ... and the device check of its one copy between two formats, BGRA into RGBX (vk.c lorieVkTransferCheck): not run,
 * byte-exact, not byte-exact (or failed on the way), not possible here (no BGRA import with the transfer usage) */
#define LORIE_VK_XFER_CHECKS(X) X(NONE) X(PASS) X(FAIL) X(UNAVAILABLE)
/* P10-B0: how a GPU-copied presentation ended before its copy was done (patches/xserver.patch calls
 * lorieCountGpuCopyEarly with present_priv.h's LORIE_GPU_COPY_EARLY_*, the same values): overwritten in the same
 * frame (present_vblank_scrap), destroyed with its window (present_vblank_destroy), its first or a later poll not
 * queued (present_execute_copy), the renderer without a surface or disconnected (present_execute_copy, stalled) */
#define LORIE_X_EARLY(X) X(SCRAP) X(DESTROY) X(POLL_FIRST) X(POLL_LATER) X(STALLED)
#define LORIE_ENUM_X_EARLY(n) LORIE_X_EARLY_ ## n,
enum { LORIE_X_EARLY(LORIE_ENUM_X_EARLY) LORIE_X_EARLY_COUNT };
#undef LORIE_ENUM_X_EARLY
#define LORIE_ENUM_XFER_NOT(n) LORIE_VK_XFER_NOT_ ## n,
#define LORIE_ENUM_XFER_USAGE(n) LORIE_VK_XFER_USAGE_ ## n,
#define LORIE_ENUM_XFER_CHECK(n) LORIE_VK_XFER_CHECK_ ## n,
enum { LORIE_VK_XFER_NOTS(LORIE_ENUM_XFER_NOT) LORIE_VK_XFER_NOT_COUNT };
enum { LORIE_VK_XFER_USAGES(LORIE_ENUM_XFER_USAGE) LORIE_VK_XFER_USAGE_COUNT };
enum { LORIE_VK_XFER_CHECKS(LORIE_ENUM_XFER_CHECK) LORIE_VK_XFER_CHECK_COUNT };
#undef LORIE_ENUM_XFER_NOT
#undef LORIE_ENUM_XFER_USAGE
#undef LORIE_ENUM_XFER_CHECK

// CLOCK_MONOTONIC in microseconds: one clock for the X server and the renderer, whichever process reads it.
static inline uint64_t lorieMonotonicUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000;
}

/* A latency's samples (P9-B0): how many, their sum, how many fell at or under each bound in microseconds (the last
 * bucket: over the last bound), all counted up; and the largest since the X server last logged it, which it takes
 * back to 0. The X server logs every 5 seconds' samples: count, mean, p95 (the bound of the bucket the 95th
 * percentile falls in) and maximum. weightedUs: each sample times the entries it held up (a submission's part times
 * its entries, an entry's own 1) - what it adds up to in entry time, the unit of copy_total_us. */
#define LORIE_LATENCY_BOUNDS 250, 500, 750, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 12000, 16000, 25000, 33000, 66000
#define LORIE_LATENCY_BUCKETS 16
struct lorie_latency {
    volatile uint32_t count, maxUs;
    volatile uint64_t sumUs, weightedUs;
    volatile uint32_t buckets[LORIE_LATENCY_BUCKETS];
};

static inline void lorieLatencyAddWeighted(volatile struct lorie_latency *l, uint64_t us, uint32_t entries) {
    static const uint32_t bounds[LORIE_LATENCY_BUCKETS - 1] = { LORIE_LATENCY_BOUNDS };
    uint32_t v = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us, b = 0, max;
    while (b < LORIE_LATENCY_BUCKETS - 1 && v > bounds[b])
        b++;
    __atomic_fetch_add(&l->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&l->sumUs, us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&l->weightedUs, us * entries, __ATOMIC_RELAXED);
    __atomic_fetch_add(&l->buckets[b], 1, __ATOMIC_RELAXED);
    max = __atomic_load_n(&l->maxUs, __ATOMIC_RELAXED);
    while (v > max && !__atomic_compare_exchange_n(&l->maxUs, &max, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

static inline void lorieLatencyAdd(volatile struct lorie_latency *l, uint64_t us) {
    lorieLatencyAddWeighted(l, us, 1);
}

/*
 * P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1): a timing's samples on bounds fine around a 120 Hz refresh (8.333 ms)
 * that hold 16.667 and 33.333 ms (two and four refreshes): how many, their sum, how many fell at or under each bound
 * in microseconds (the last bucket: over the last bound), all counted up; and the largest since the X server last
 * logged it, which it takes back to 0. The X server logs every 5 seconds' samples: count, mean, p95 and p99 (the
 * bound of the bucket each falls in) and maximum, and how many were over 16.667 ms and over 33.333 ms (h16, h33).
 */
#define LORIE_HIST_BOUNDS 50, 100, 200, 300, 500, 750, 1000, 1500, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 8500, \
                          9000, 10000, 12000, 14000, 16667, 20000, 25000, 33333, 50000, 100000
#define LORIE_HIST_BUCKETS 27
#define LORIE_HIST_OVER_16MS 21     /* the buckets from this one on: over 16.667 ms */
#define LORIE_HIST_OVER_33MS 24     /* over 33.333 ms */
_Static_assert(sizeof((uint32_t[]) { LORIE_HIST_BOUNDS }) == sizeof(uint32_t) * (LORIE_HIST_BUCKETS - 1),
               "LORIE_HIST_BUCKETS is LORIE_HIST_BOUNDS and one over the last");
struct lorie_hist {
    volatile uint32_t count, maxUs;
    volatile uint64_t sumUs;
    volatile uint32_t buckets[LORIE_HIST_BUCKETS];
};

static inline void lorieHistAdd(volatile struct lorie_hist *h, uint64_t us) {
    static const uint32_t bounds[LORIE_HIST_BUCKETS - 1] = { LORIE_HIST_BOUNDS };
    uint32_t v = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us, b = 0, max;
    while (b < LORIE_HIST_BUCKETS - 1 && v > bounds[b])
        b++;
    __atomic_fetch_add(&h->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->sumUs, us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&h->buckets[b], 1, __ATOMIC_RELAXED);
    max = __atomic_load_n(&h->maxUs, __ATOMIC_RELAXED);
    while (v > max && !__atomic_compare_exchange_n(&h->maxUs, &max, v, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

struct lorie_renderer_diag {
    volatile uint8_t active;                    /* LORIE_ACTIVE_*: what draws the root right now */

    /* EGL/GLES (the 3c98ffa path) */
    volatile uint32_t eglImageImports;          /* eglCreateImageKHR returned an image for a buffer */
    volatile uint32_t glesCopyFrames;           /* GLES frames that drew the root (draw calls issued) */
    volatile uint32_t eglSwapBuffers;           /* eglSwapBuffers returned EGL_TRUE on a window surface */

    /* Vulkan */
    volatile uint32_t vkInstanceCreated, vkDeviceCreated, vkSurfaceCreated, vkSwapchainCreated;
    volatile uint32_t vkAhbImportAttempts, vkAhbImportSuccess;
    volatile uint32_t vkCopyCommands, vkBlitCommands, vkRenderCommands;
    /* draws that used an imported AHB's image as sampled source / colour destination, in a command
     * buffer whose submission succeeded and whose fence then signalled */
    volatile uint32_t vkImportedSourceDraws, vkImportedDestinationDraws;
    volatile uint32_t vkQueueSubmits, vkQueuePresentSuccess, vkFramesPresented;
    volatile uint32_t vkAcquireSuccess, vkAcquireSuboptimal, vkAcquireTimeout, vkAcquireNotReady, vkAcquireOutOfDate;
    volatile uint64_t vkAcquireWaitUs, vkSubmitCpuUs, vkFenceWaitUs, vkPresentCallUs;
    /* TERMUX_X11_RENDERER_VK_SUBMIT_OPT: frames whose queued copies went ahead in a submission of their own where
     * they would have gone in the frame's (not FIFO); frames presented before their fence was waited for */
    volatile uint32_t vkCopiesAhead, vkEarlyPresents;
    volatile uint8_t vkSelftest;                /* 0 not run, 1 passed, 2 failed */
    /* the ownership self-test (vk.c lorieVkSelftest): byte-exact compares that passed, iterations per format and
     * direction, formats that passed / could not be tested (bit 0 RGBA, 1 RGBX, 2 BGRA), its duration; where it
     * failed: step (LORIE_VK_SELFTEST_STEP_*), AHB format, direction (1 CPU->Vulkan->CPU, 2 Vulkan->CPU->Vulkan),
     * iteration, byte offset, expected and actual byte, VkResult */
    volatile uint32_t vkSelftestCompares, vkSelftestIterations;
    volatile uint8_t vkSelftestFormatsPassed, vkSelftestFormatsUnavailable;
    volatile uint64_t vkSelftestUs;
    volatile uint8_t vkSelftestFailStep, vkSelftestFailDirection, vkSelftestFailExpected, vkSelftestFailActual;
    volatile uint32_t vkSelftestFailFormat, vkSelftestFailIteration, vkSelftestFailOffset;
    volatile int32_t vkSelftestFailResult;
    volatile char vkDevice[96];                 /* VkPhysicalDeviceProperties.deviceName of the device used */
    volatile uint32_t vkDriverVersion, vkApiVersion;
    volatile char vkPresentMode[16];            /* the present mode the swapchain was created with */

    /* fallbacks: every one counted, with its class and reason; the last one in detail */
    volatile uint32_t fallbackTotal;
    volatile uint32_t fallbackByClass[LORIE_FALLBACK_CLASS_COUNT];
    volatile uint32_t fallbackByReason[LORIE_FALLBACK_REASON_COUNT];
    volatile uint8_t lastFallbackClass, lastFallbackReason;
    volatile int32_t lastVkResult;
    volatile uint64_t lastResourceId, lastResourceUsage;
    volatile uint32_t lastResourceFormat;

    /* X: the AHB ownership mode in effect, its transitions, PER_ACCESS requests refused, CPU accesses that
     * locked an AHB per access, the time those spent waiting before they could (for the shared lock and for a
     * Vulkan frame using the AHB), of that the wait for the frame alone (lorieGpuWaitIdle), and the time spent
     * in the AHB's CPU lock and unlock themselves; the root buffers in use (2: root double buffering, InitOutput.c
     * lorieRootSwap) and the time spent copying damage between them */
    volatile uint8_t xOwnership, xRootBuffers;
    volatile uint32_t xOwnershipTransitions, xOwnershipRefused;
    volatile uint64_t xAhbCpuAcquires, xStateLockWaitUs, xGpuWaitUs, xAhbCpuLockUs, xRootCopyUs;

    /* P11-C2, TERMUX_X11_RENDERER_ROOT_GPU_COPY (struct rootCopy below): the root's damage copies asked of the
     * renderer (X); submitted, and completed - their fence signalled - with their rectangles, pixels in bytes and
     * the time from the request to the completion (renderer, us: not the GPU's time alone); copied by the X server's
     * CPU instead - the renderer could not, or the request was taken back - and copied on the CPU being smaller than
     * asking the GPU is worth (X); the X server's time waiting for one before it could use its buffer (X, us) */
    volatile uint32_t rootGpuCopyRequests, rootGpuCopySubmits, rootGpuCopyCompletions, rootGpuCopyRects;
    volatile uint64_t rootGpuCopyBytes, rootGpuCopyCompletionUs;
    volatile uint32_t rootCpuFallbacks, rootCpuSmall;
    volatile uint64_t rootCopyCpuBlockUs;

    /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG (struct lorie_hist), CLOCK_MONOTONIC microseconds throughout.
     * The renderer's Vulkan screen frames (renderer.c rendererVkFrameTimed), one sample per frame presented: the
     * interval since the last frame's start, and since the last present call (none for the first after a swapchain
     * is made, none over 250 ms: idle gaps, counted apart, as stamps out of order are); from the X server's tick
     * asking for it to its start (wake); its start to its end (cpu); its acquire, its wait for the X server's lock,
     * its recording, its vkQueueSubmit, its fence (that frame's own), its vkQueuePresentKHR call; a root copy made
     * right before it in the same loop pass (after_root). The fences of the root copies and of the Present copies
     * submitted alone, each its own. The period's largest frame interval and what the frame after it spent. */
    struct lorie_hist vkFrameStart, vkPresentDelta, vkFrameWake, vkFrameCpu, vkFrameAcquire, vkFrameLock, vkFrameRecord,
                      vkFrameSubmit, vkFrameFence, vkFramePresent, vkFrameAfterRoot, vkFenceRoot, vkFenceCopy;
    volatile uint32_t vkFrameIdleGaps, vkFrameStampsInvalid;
    volatile uint32_t vkWorstStartUs, vkWorstWakeUs, vkWorstAcquireUs, vkWorstLockUs, vkWorstFenceUs, vkWorstPresentUs,
                      vkWorstAfterRootUs;
    /* ... and the frame that began that interval - the one that may have taken long: its whole, its wait for the
     * lock, its fence, the root copy before it - then the gap from its end to the next start, and of that gap how long
     * no tick asked for a frame (the X server had nothing new to show) */
    volatile uint32_t vkWorstPrevCpuUs, vkWorstPrevLockUs, vkWorstPrevFenceUs, vkWorstPrevAfterRootUs, vkWorstGapUs, vkWorstUnaskedUs;
    /* P11-C3-C: a frame's start 100 ms or more after the last one's (renderer.c rendererVkFrameGap), by what waited in
     * that gap (struct lorie_progress): a tick's ask had waited 100 ms or more as the frame began - the renderer asleep
     * when it was asked and not woken for most of the wait (unwoken), or awake and late (late) - or else nothing was
     * asked for most of it: the X server's ticks not run in it (x_stalled), or run with nothing to show (idle). Each by
     * its length - a stall's the ask's wait (X stalled: the gap), idle's the gap - at 100, 250, 500 ms and 1, 2, 3 s
     * (LORIE_GAP_GE); the first waiting ask's wait at every frame's start (vkAskWait); the period's longest stall:
     * its length, cause (LORIE_GAP_*), the ask's wait, the wake (the ask to the renderer back to look for work), the
     * gap and the ticks run in it. */
#define LORIE_GAP_GE 6
    struct lorie_hist vkAskWait;
    volatile uint32_t vkGapIdle, vkGapXStalled, vkGapUnwoken, vkGapLate;
    volatile uint32_t vkGapStallGe[LORIE_GAP_GE], vkGapIdleGe[LORIE_GAP_GE];
    volatile uint32_t vkGapWorstStallUs, vkGapWorstCause, vkGapWorstAskWaitUs, vkGapWorstWakeUs, vkGapWorstGapUs, vkGapWorstTicks;
    /* P11-C3-D0 (TERMUX_X11_CONTENT_FRAME_DIAG=1): client content (struct lorie_content_stamp) from its request to the
     * screen. X (InitOutput.c lorieContent*): presentations asked for, skipped (overwritten in their frame, or their GPU
     * copy cancelled), copied by the GPU (queued) or the CPU, GPU copies completed (taken up by the X server), their
     * CompleteNotify sent (mode copy; skip apart); CPU reads of a pixmap a GPU copy into which is still queued - not
     * recorded by the renderer yet: the read sees the content before it - its damage already reported (stale: the
     * compositor was told of content not there) or not yet (pending: what it was told of is there), or whose damage
     * could not be told (unknown); content carried by a CPU composite or copy into another pixmap
     * (compositor) and into the screen pixmap (root, copies straight into it too); the screen pixmap published with new
     * content. Its waits: the request to its copy queued or made, queued to completed, completed to the screen pixmap,
     * the screen pixmap to its publishing. Renderer (renderer.c rendererContentFrame): Vulkan screen frames presented
     * with new content and with the last one's again; publishing to the frame's present, the request to it (total),
     * from one frame with new content to the next (interval); intervals of 50 ms or more, the content shown then asked
     * for in the first half of the interval (pending: it waited) or later (client: none was asked for); the longest
     * interval; the period's slowest content - its total and its stages (to its copy complete, to the screen pixmap, to
     * publishing, to the screen). */
    struct lorie_hist contentPresentToCopy, contentCopyToComplete, contentCompleteToRoot, contentRootToPublish,
                      contentPublishToScreen, contentTotal, contentInterval;
    volatile uint32_t contentRequests, contentSkipped, contentGpuCopies, contentCpuCopies, contentCopiesDone, contentCompletes,
                      contentCompletesSkip, contentStaleReads, contentCompositorUpdates, contentRootUpdates, contentRootPublishes;
    volatile uint32_t contentPendingReads, contentUnknownReads;
    volatile uint32_t contentScreenNew, contentScreenReused, contentGapsPending, contentGapsClient;
    volatile uint32_t contentScreenUnknown;     /* frames presented with no whole copy of the content taken (torn) */
    volatile uint32_t contentSkippedUnattributed;   /* skips whose client's slot went to another since its request */
    volatile uint32_t contentMaxGapUs, contentWorstUs, contentWorstCopyUs, contentWorstCompositeUs, contentWorstPublishUs,
                      contentWorstScreenUs;
    /* The root copies (rootCopy): the request to the renderer taking it, its recording and submission; and, when the
     * X server had to wait for one (lorieRootCopyWait), that wait, and from the renderer settling the copy to the X
     * server going on. The period's slowest request to completion and its parts (its fence: vkFenceRoot's). */
    struct lorie_hist rootTake, rootRecord, rootResume, rootBlock;
    /* P11-C3-D2: a root copy submitted ahead (P11-C3-B2), from its submission to its fence waited for - and so settled */
    struct lorie_hist rootAheadSettle;
    volatile uint32_t rootWorstUs, rootWorstTakeUs, rootWorstRecordUs, rootWorstFenceUs;
    /* X: each client request's handling (Dispatch); each PER_ACCESS AHB access's hold of the shared lock (from taking
     * it in PrepareAccess to letting it go in FinishAccess), its wait for that lock and for a Vulkan frame using the
     * AHB; each tick's wait from its Choreographer callback, and its work up to asking for the frame (lorieRedraw).
     * The ticks that asked the renderer for a frame, and the ticks that found none presented since the last one did. */
    struct lorie_hist xRequest, xAccess, xAccessLock, xAccessGpu, xTickDelay, xTickWork;
    volatile uint32_t xTicksFrameWanted, xTicksFrameMissed;
    /* P11-C3-B1, with TERMUX_X11_RENDERER_VK_FRAME_DIAG: how long each PER_ACCESS AHB access kept `lock` - all of it,
     * or, reserved (cpuAccess), only while reserving and letting go. */
    struct lorie_hist xAccessHold;

    /* P11-C3-B1 (TERMUX_X11_EXA_LOCK_OPT=1), always counted: the X server's PER_ACCESS AHB accesses made under a
     * reservation instead of `lock`, and those that held `lock` because no reservation was free; the renderer's
     * passes that left a copy queued for a reserved AHB, and the X server's wakes of a renderer that did. */
    volatile uint32_t xAccessReserved, xAccessReserveFull, copiesDeferred, copiesDeferredWakes;
    /* P11-C3-C, always counted: the renderer's passes that waited, before taking `lock`, for a buffer their copies use
     * not come yet from the connection, and how long; buffers given up on (not come within 100 ms: their copies
     * skipped); passes that left copies queued for a buffer that may still come (for the next pass to wait for). */
    volatile uint32_t buffersAwaited, buffersGivenUp, copiesLeftForBuffers;
    volatile uint64_t bufferAwaitUs;
    /* P11-C3-B2 (TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1), always counted: root copies submitted ahead of the frame (the
     * frame chained after them on the device); of those, waited for first because the X server already waited for
     * them; chain semaphores no frame waited for (an empty submission did); and root copies made the old way because
     * the second submission's objects could not be made. */
    volatile uint32_t rootAhead, rootAheadXWaiting, rootChainConsumed, rootAheadUnavailable;
    /* P11-C3-D2 (TERMUX_X11_ROOT_COPY_SCHED_OPT=1), always counted: root copies submitted ahead settled before the
     * frame's submission - their fence found signalled once the frame's image was acquired, or while the X server's
     * lock was not free - and those the X server was found waiting for then. */
    volatile uint32_t rootEarlyAcquired, rootEarlyLock, rootEarlyXWaiting;
    /* P11-C3-D1-A (TERMUX_X11_EXA_COMPOSITE_OPT=1), always counted: Present GPU copies whose damage was put off until
     * the copy was done (patches/xserver.patch present_damage_gpu_copy), and how each went - reported with its copy
     * done; reported before, its presentation scrapped while the copy may still run (as without the option); dropped,
     * the copy never to run or its window destroyed. Put off less those: copies still running. */
    volatile uint32_t copyDamageDeferred, copyDamageDone, copyDamageEarly, copyDamageDropped;

    /* TERMUX_X11_RENDERER_VK_TRANSFER_COPY, while it is on: queued copies checked; for each condition, the copies it
     * kept from vkCmdCopyImage (one copy may count under several); for each image of a copy without the transfer
     * usage it needs, what its import got (LORIE_VK_XFER_USAGE_*); imports that asked for the usage, by what they
     * got. The last copy kept from it: its conditions (bit n: LORIE_VK_XFER_NOT_*), both AHBs' format, size, AHB
     * usage and Vulkan usage, the destination's depth, its rectangles, the first one and its offset. */
    volatile uint32_t vkXferChecked;
    volatile uint32_t vkXferNot[LORIE_VK_XFER_NOT_COUNT];
    volatile uint32_t vkXferUsageMissing[LORIE_VK_XFER_USAGE_COUNT];
    volatile uint32_t vkXferImports[LORIE_VK_XFER_USAGE_COUNT];
    volatile uint32_t vkXferLastNot, vkXferLastSrcFormat, vkXferLastDstFormat, vkXferLastSrcUsage, vkXferLastDstUsage;
    volatile uint32_t vkXferLastSrcWidth, vkXferLastSrcHeight, vkXferLastDstWidth, vkXferLastDstHeight, vkXferLastDstDepth;
    volatile uint64_t vkXferLastSrcAhbUsage, vkXferLastDstAhbUsage;
    volatile uint32_t vkXferLastRects;
    volatile int32_t vkXferLastRect[4], vkXferLastOff[2];
    /* the BGRA into RGBX device check (LORIE_VK_XFER_CHECK_*); where it failed: the self-test step, the byte (of the
     * destination, tightly packed), its expected and actual value, the VkResult */
    volatile uint8_t vkXferCheck, vkXferCheckStep, vkXferCheckExpected, vkXferCheckActual;
    volatile int32_t vkXferCheckOffset, vkXferCheckResult;

    /* P9-B0, the Vulkan backend's Present copies (renderer.c rendererVkCountCopies), counted once their
     * completedSerial is published. Per queued entry: from the X server queuing it to the renderer starting the
     * submission that records it (copyQueue), and to its completedSerial published (copyTotal). Per submission that
     * records copies (weighted by its entries): the shared lock's wait (copyLock); under it, the wait for a submitted
     * frame to be over (copyInflightWait: lorieGpuWaitIdle, another renderer process's list); lookups, imports,
     * recording and the submission itself (copyRecord - in a frame the root and the cursor too); from the submission
     * to its fence waited for - the copies alone (copyGpuWait) or a frame's: its copies, the root and the cursor
     * (copyFrameGpuWait). X: from the latest publish to the X server having re-executed what it completed (copyAck,
     * once per publish). Entries and submissions on their own (standalone) or in a frame with the root. */
    struct lorie_latency copyQueue, copyLock, copyInflightWait, copyRecord, copyGpuWait, copyFrameGpuWait, copyTotal, copyAck;
    volatile uint32_t copyStandaloneEntries, copyFrameEntries, copyStandaloneSubmits, copyFrameSubmits;

    /* P10-B0, X: the vsync's frame time (Choreographer's, CLOCK_MONOTONIC) to its callback (xVsyncToCallback, 64-bit
     * only: a 32-bit long cannot hold it); the callback to the tick it posts running on the X server's thread
     * (xCallbackToTick); a GPU copy completion published by the renderer to the X server's input thread receiving
     * it (xCopyNotify), and received to the X server's thread done with its recheck (xCopyRecheck). Each pair is
     * one event's own two stamps: the tick and the recheck carry theirs to the X server's thread. Ticks and
     * completions posted to the X server's thread and run there - posted minus run is what is waiting, not lost -
     * and the most still waiting behind one when it ran (taken back to 0 by each report). Stamps out of order (a
     * pair with the later before the earlier, or over 10 s apart) are counted, not timed. GPU-copied presentations
     * ended before the copy was done, by how (LORIE_X_EARLY_*). */
    struct lorie_latency xVsyncToCallback, xCallbackToTick, xCopyNotify, xCopyRecheck;
    volatile uint32_t xTicksPosted, xTicksRun, xTicksWaitingMax, xCopyDonePosted, xCopyDoneRun, xCopyDoneWaitingMax;
    volatile uint32_t xStampsInvalid;
    volatile uint32_t xCopyEarly[LORIE_X_EARLY_COUNT];
    /* P10-S1, X: what kept those early ends from letting go early - presentations whose copy was retired (its
     * buffers kept past the presentation, patches/xserver.patch present_retire_copy) and retired copies let go once
     * settled (made minus let go: still held), copies cancelled before any renderer recorded them (lorieGpuCopySettle) */
    volatile uint32_t xCopyRetired, xCopyRetiredReleased, xCopyCancelled;
};

/*
 * P11-C3-C: what the X server's ticks asked of the renderer and where the renderer is - for the X server's freeze
 * watchdog (TERMUX_X11_FRAME_FREEZE_DIAG=1, InitOutput.c lorieFreezeCheck), which reads it from a thread of its own
 * without `lock` and waits for nothing, and for the frame gaps (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1, renderer.c
 * rendererVkFrameGap). Kept only while one of the two is on, each field by one side, with plain stores: hints, read
 * as they are. Times are lorieMonotonicUs.
 *   askSeq, askUs        X: the frames its ticks asked for (lorieTickAsked), counted, and when the last LORIE_ASK_RING
 *                        of them were asked for (lorieAskUs)
 *   takenSeq, takenUs    renderer: the asks its last frame presented took up (askSeq as it began), and its start
 *   seenSeq, seenUs      renderer: the asks it saw when it last woke to work (rendererWaitForWork), and when
 *   sleepUs              renderer: when it last went to sleep there
 *   rStage, rStageUs     renderer, the watchdog only: what it is doing (LORIE_R_*), and since when
 */
enum {
    LORIE_R_SLEEP,          /* asleep, nothing to do it knows of (rendererWaitForWork) */
    LORIE_R_PASS,           /* a pass of its loop, none of the below */
    LORIE_R_BUFFER_WAIT,    /* buffers its copies use not come yet (rendererAwaitBuffers) */
    LORIE_R_ROOT_COPY,      /* a root copy: taken, submitted, its fence (P11-C2, P11-C3-B2) */
    LORIE_R_FRAME,          /* a Vulkan frame, none of the below */
    LORIE_R_ACQUIRE,        /* its swapchain image (lorieVkAcquire) */
    LORIE_R_LOCK_WAIT,      /* the shared lock, asked for by the frame */
    LORIE_R_FENCE_WAIT,     /* the frame's fence (lorieVkFrameWait) */
    LORIE_R_PRESENT,        /* vkQueuePresentKHR (lorieVkPresent) */
    LORIE_R_COPIES,         /* Vulkan copies alone */
    LORIE_R_GLES,           /* a GLES frame, or GLES copies alone */
    LORIE_R_BACKEND,        /* a backend chosen, started or given up; a window taken up */
    LORIE_R_DEVICE_IDLE,    /* vkDeviceWaitIdle */
    LORIE_R_STAGES
};
#define LORIE_ASK_RING 32
struct lorie_progress {
    volatile uint32_t askSeq, takenSeq, seenSeq, rStage;
    volatile uint64_t askUs[LORIE_ASK_RING], takenUs, seenUs, sleepUs, rStageUs;
};

// When ask `seq` was made, `last` being the last one made: kept for the last LORIE_ASK_RING; for an older one, the
// oldest kept - a lower bound of how long it waited. 0: not made (or not kept yet).
static inline __always_inline uint64_t lorieAskUs(volatile struct lorie_progress *p, uint32_t seq, uint32_t last) {
    if (last - seq >= LORIE_ASK_RING)
        seq = last - (LORIE_ASK_RING - 1);
    return p->askUs[seq % LORIE_ASK_RING];
}

/*
 * P11-C3-D0 (TERMUX_X11_CONTENT_FRAME_DIAG=1): client content - one client's presentation (PresentPixmap) - followed from
 * its request to the screen. A pixmap's stamp (InitOutput.c LoriePixmapPriv) is the newest content in it: its number
 * (wrapping: compared by lorieContentNewer), when it was asked for, when its copy completed, when it reached this pixmap
 * (copied in, or carried by a CPU composite or copy from a source holding it). 0: none.
 */
struct lorie_content_stamp {
    uint32_t seq;
    uint64_t requestUs, copyUs, readyUs;
};
/* ... and what the X server last published with the screen pixmap (InitOutput.c lorieContentPublish), for the renderer:
 * a generation, the content's number and stamps, when it reached the screen pixmap, when it was published. The X server
 * is its only writer, under a sequence lock: `gen` odd while it writes the rest, even once they are all written - the
 * new generation (0: none yet); the renderer keeps a copy only if `gen` was even and the same before and after it read
 * the rest (renderer.c rendererContentRead; LORIE_CONTENT_TORN: no whole copy). Each field is read and written
 * atomically (LORIE_CONTENT_GET, LORIE_CONTENT_PUT: relaxed - the fences around `gen` order them), so neither side races
 * the other; the 64-bit ones 8-byte aligned, so that 32-bit ABIs' atomics on them are the processor's own - lock-free
 * across the two processes (ARMv7 ldrexd/strexd, x86 movq) - not a library's lock. */
typedef uint64_t lorie_u64 __attribute__((aligned(8)));
struct lorie_content {
    volatile uint32_t gen, seq;
    volatile lorie_u64 requestUs, copyUs, rootUs, publishUs;
};
_Static_assert(__atomic_always_lock_free(sizeof(lorie_u64), 0) && _Alignof(struct lorie_content) == 8,
               "struct lorie_content needs lock-free, 8-byte aligned 64-bit atomics");
#define LORIE_CONTENT_TORN 1u       /* a copy no whole one was taken of (an odd generation: never published) */

static inline __always_inline bool lorieContentNewer(uint32_t a, uint32_t b) {
    return (int32_t) (a - b) > 0;
}

#define LORIE_DIAG_ADD(st, field, n) \
    do { if (st) __atomic_fetch_add(&(st)->diag.field, (n), __ATOMIC_RELAXED); } while (0)

/* P11-C3-D0: struct lorie_content's fields, as its sequence lock reads and writes them (relaxed atomics) */
#define LORIE_CONTENT_PUT(field, value) __atomic_store_n(&(field), (value), __ATOMIC_RELAXED)
#define LORIE_CONTENT_GET(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)

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
        volatile uint64_t completedSerial;
        volatile uint64_t completedUs;      /* the Vulkan renderer: when it last published completedSerial (lorieMonotonicUs) */
        LorieGpuCopyEntry entries[LORIE_GPU_COPY_QUEUE_CAPACITY];
    } gpuCopyQueue;

    /*
     * The X server's AHBs that a submitted Vulkan frame uses, until it is known to be over. The renderer fills
     * `ids` and `pid` (its process) and sets `busy` under `lock`, right after submitting the frame, and then lets
     * `lock` go while the device runs it. The frame is over when the renderer saw its fence signal, the device
     * idle, or the device lost (renderer.c rendererVkFrameDone clears `busy` then - without `lock`, which the X
     * server may be holding while it waits - and wakes the waiters, a futex on `busy`), or when the process
     * `pid` no longer exists (lorieGpuWaitIdle clears it then). Nothing else clears it: a lost connection or
     * elapsed time says nothing of the device. Under PER_ACCESS the X server, holding `lock`, CPU-locks no AHB
     * listed here while `busy`: the device owns those (FOREIGN ownership, vk.h) until the frame is over.
     * P11-C3-B2: the renderer may have two submissions running - a root copy submitted ahead of the frame, in its own
     * list (auxIds) - each list in effect while its bit of `busy` is set (LORIE_GPU_IN_FLIGHT_*), each cleared on its
     * own once that submission is over.
     */
    struct {
        volatile uint32_t busy;
        volatile pid_t pid;
        uint32_t count;
        uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX];
        uint32_t auxCount;
        uint64_t auxIds[LORIE_GPU_IN_FLIGHT_MAX];
    } gpuInFlight;

    /*
     * P11-C3-B1 (TERMUX_X11_EXA_LOCK_OPT=1): the AHBs the X server CPU-accesses without holding `lock` through the
     * access (InitOutput.c loriePrepareAccess). Under PER_ACCESS, holding `lock`, it reserves one only once no submitted
     * Vulkan frame lists it (gpuInFlight), lets `lock` go, CPU-locks it; after the access it unlocks it, and only then
     * lets the reservation go, under `lock` again. A renderer, holding `lock`, records no work using a reserved AHB:
     * a queued copy that would use one stays queued, with every copy after it (renderer.c). The root the X server
     * publishes (rootWindowTextureID, which the renderer samples) it never reserves: accesses to it hold `lock` as
     * before. `releases` counts the reservations let go; `waiter`, set under `lock` by a renderer that left a copy
     * queued for one, the X server clears at the next release, waking the renderer then (lorieWakeRenderer, and a
     * futex on `releases`).
     */
    struct {
        uint32_t count;
        uint64_t ids[LORIE_CPU_ACCESS_MAX];
        volatile uint32_t releases, waiter;
    } cpuAccess;

    /*
     * P11-C2 (TERMUX_X11_RENDERER_ROOT_GPU_COPY=1): root double buffering's copy of the damage from the buffer the
     * X server drew into - the front from the swap on, which the renderer samples - into the other, the one it draws
     * into from then on, made by the renderer's GPU instead of the X server's CPU (InitOutput.c lorieRootSwap,
     * renderer.c rendererVkRootCopy). One at a time: the X server asks for one only once the last is settled.
     *   X, under `lock`: the request (srcId, dstId, rects, queuedUs), then a new `seq`; or, for a request no renderer
     *   took, `cancelled` = seq - the X server then copies it itself.
     *   The renderer, under `lock`: `taken` = seq (and `pid`), records and submits the copy - its two AHBs listed in
     *   gpuInFlight - and lets the lock go; once the fence signalled, `done` = seq, woken (a futex on `done`). Not
     *   copied (no PER_ACCESS answer, a buffer missing, Vulkan not running or failing): `failed` = seq, then `done`.
     * Until it is settled the X server CPU-locks neither buffer; it waits for it without `lock` (lorieRootCopyWait) -
     * or, already holding `lock`, takes back a request not taken rather than wait for a renderer that needs `lock`.
     */
    struct {
        volatile uint32_t seq, taken, cancelled, failed, done;
        volatile pid_t pid;
        uint64_t srcId, dstId, queuedUs;
        volatile uint64_t doneUs;   /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG: when the renderer settled `done` */
        volatile uint32_t waiting;  /* P11-C3-B2: X - the request it waits for now (lorieRootCopyWait), else 0 */
        uint32_t numRects;
        LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
    } rootCopy;

    /* ID of root window texture to be drawn. */
    uint64_t rootWindowTextureID;

    /* A signal to renderer to update root window texture content from shared fragment if needed */
    volatile uint8_t drawRequested;
    /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG: when the X server's tick last asked for a frame (lorieMonotonicUs) */
    volatile uint64_t frameWantedUs;

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
        volatile uint32_t gpuCopyFrames;  /* frames that carried at least one present copy */
        volatile uint32_t coalescedFrames;/* redraws the backpressure guard delayed */
    } presentStats;

    /*
     * GL_VENDOR | GL_RENDERER of the context the renderer actually draws with, published once so
     * the X server can log it. testCapabilities() runs its probe in the X server's own process,
     * which can be given a different GLES implementation than the activity (ANGLE is selected per
     * package), and it is the activity's one that decides what the present path costs.
     */
    volatile char rendererDriver[192];

    /* Renderer backend and the proof of what actually ran (see struct lorie_renderer_diag). */
    struct lorie_renderer_diag diag;
    /* Backend selection and the AHB ownership handshake (see struct lorie_backend_control). */
    struct lorie_backend_control backend;
    /* P11-C3-C: the asks and the renderer's stage (see struct lorie_progress). */
    struct lorie_progress progress;
    /* P11-C3-D0: the content last published with the screen pixmap (see struct lorie_content). */
    struct lorie_content content;

    struct {
        // We should not allow updating cursor content the same time renderer draws it.
        // locking the mutex protecting the root window can cause waiting for the frame to be drawn which is unacceptable
        pthread_mutex_t lock; // initialized at X server side.
        pid_t lockingPid;
        uint32_t x, y, xhot, yhot, width, height;
        uint32_t bits[512*512]; // 1 megabyte should be enough for any cursor up to 512x512
        // Signals to renderer to update cursor's texture or its coordinates
        volatile uint8_t updated, moved;
    } cursor;
};

// Whether AHB `id` (any id, 0 as well - LorieBuffer ids start at 0) is used by a submitted Vulkan frame not over.
static inline __always_inline bool lorieGpuInFlight(volatile struct lorie_shared_server_state *st, uint64_t id) {
    uint32_t i, busy = __atomic_load_n(&st->gpuInFlight.busy, __ATOMIC_ACQUIRE);
    if (busy & LORIE_GPU_IN_FLIGHT_MAIN)
        for (i = 0; i < st->gpuInFlight.count && i < LORIE_GPU_IN_FLIGHT_MAX; i++)
            if (st->gpuInFlight.ids[i] == id)
                return true;
    if (busy & LORIE_GPU_IN_FLIGHT_AUX)
        for (i = 0; i < st->gpuInFlight.auxCount && i < LORIE_GPU_IN_FLIGHT_MAX; i++)
            if (st->gpuInFlight.auxIds[i] == id)
                return true;
    return false;
}

// P11-C3-B1: whether the X server holds AHB `id` reserved for a CPU access (cpuAccess); under `lock`.
static inline __always_inline bool lorieCpuReserved(volatile struct lorie_shared_server_state *st, uint64_t id) {
    uint32_t i;
    for (i = 0; i < st->cpuAccess.count && i < LORIE_CPU_ACCESS_MAX; i++)
        if (st->cpuAccess.ids[i] == id)
            return true;
    return false;
}

// Whether any AHB is.
static inline __always_inline bool lorieGpuAnyInFlight(volatile struct lorie_shared_server_state *st) {
    return __atomic_load_n(&st->gpuInFlight.busy, __ATOMIC_ACQUIRE) != 0;
}

/*
 * Whether the process that submitted the listed frame no longer exists. Its files went with it, the device's
 * among them, and the kernel's GPU driver ended its work when they were released - what the drivers do with a
 * process's GPU contexts; Vulkan itself says nothing of a process that is gone. Only ESRCH: a process that
 * exists but may not be signalled (another app's: EPERM, EACCES) is there, and so is one not yet reaped.
 */
static inline __always_inline bool lorieGpuSubmitterGone(volatile struct lorie_shared_server_state *st) {
    pid_t pid = st->gpuInFlight.pid;
    return pid > 0 && kill(pid, 0) == -1 && errno == ESRCH;
}

/*
 * Holding `lock`, or (when every is false) holding a cpuAccess reservation
 * for this exact AHB:
 * returns once AHB `id` - or, with `every`, any AHB - is in no submitted Vulkan frame that is not
 * over. The submitter ends the wait without `lock` (futex wake on `busy`); every 33 ms the submitter is looked
 * for, and a list whose submitter no longer exists is cleared here. Waits for as long as the submitter exists
 * and has not said so: it does, in bounded time, unless its device hangs.
 */
static inline __always_inline void lorieGpuWaitIdle(volatile struct lorie_shared_server_state *st, bool every, uint64_t id) {
    while (every ? lorieGpuAnyInFlight(st) : lorieGpuInFlight(st, id)) {
        struct timespec slice = { 0, 33 * 1000000 };
        uint32_t busy = __atomic_load_n(&st->gpuInFlight.busy, __ATOMIC_ACQUIRE);
        if (busy && syscall(SYS_futex, &st->gpuInFlight.busy, FUTEX_WAIT, busy, &slice, NULL, 0) && errno == ETIMEDOUT &&
            lorieGpuSubmitterGone(st))
            __atomic_store_n(&st->gpuInFlight.busy, 0, __ATOMIC_RELEASE);
    }
}

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
