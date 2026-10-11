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
void lorieRecheckGpuCopies(void);
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
        clock_gettime(CLOCK_MONOTONIC, &ts);

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

/* Two 60Hz frames: a gap this long is a hitch a user can see, not just a missed vsync. */
#define LORIE_LONG_FRAME_US 33000

#define LORIE_GPU_COPY_MAX_RECTS 16
#define LORIE_GPU_COPY_QUEUE_CAPACITY 8
#define LORIE_GPU_IN_FLIGHT_MAX 32      /* AHBs one Vulkan frame may use (vk.h LORIE_VK_MAX_FRAME_IMAGES) */

typedef struct {
    uint64_t serial;
    uint64_t srcBufferId;
    uint64_t dstBufferId;
    int16_t xOff, yOff;
    uint16_t numRects;
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
     * locked an AHB per access, and the time those spent waiting for the shared lock */
    volatile uint8_t xOwnership;
    volatile uint32_t xOwnershipTransitions, xOwnershipRefused;
    volatile uint64_t xAhbCpuAcquires, xStateLockWaitUs;
};

#define LORIE_DIAG_ADD(st, field, n) \
    do { if (st) __atomic_fetch_add(&(st)->diag.field, (n), __ATOMIC_RELAXED); } while (0)

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
     */
    struct {
        volatile uint32_t busy;
        volatile pid_t pid;
        uint32_t count;
        uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX];
    } gpuInFlight;

    /* ID of root window texture to be drawn. */
    uint64_t rootWindowTextureID;

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
    uint32_t i;
    if (!__atomic_load_n(&st->gpuInFlight.busy, __ATOMIC_ACQUIRE))
        return false;
    for (i = 0; i < st->gpuInFlight.count && i < LORIE_GPU_IN_FLIGHT_MAX; i++)
        if (st->gpuInFlight.ids[i] == id)
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
 * Holding `lock`: returns once AHB `id` - or, with `every`, any AHB - is in no submitted Vulkan frame that is not
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
