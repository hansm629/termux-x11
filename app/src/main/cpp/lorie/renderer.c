#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma ide diagnostic ignored "UnusedParameter"
#pragma ide diagnostic ignored "DanglingPointer"
#pragma ide diagnostic ignored "ConstantConditionsOC"
#pragma ide diagnostic ignored "OCUnusedGlobalDeclarationInspection"
#pragma ide diagnostic ignored "UnreachableCode"
#pragma ide diagnostic ignored "OCUnusedMacroInspection"
#pragma ide diagnostic ignored "misc-no-recursion"
#pragma clang diagnostic ignored "-Wincompatible-pointer-types-discards-qualifiers"
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#define CVT_H_GRANULARITY 8

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <limits.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <math.h>
#include <android/looper.h>
#include <poll.h>
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "list.h"
#include "lorie.h"

#include <unistd.h>
#define log(...) __android_log_print(ANDROID_LOG_DEBUG, "gles-renderer", __VA_ARGS__)
#define loge(...) __android_log_print(ANDROID_LOG_ERROR, "gles-renderer", __VA_ARGS__)

static GLuint createProgram(const char* p_vertex_source, const char* p_fragment_source);

static int printEglError(char* msg, int line) {
    char descBuf[32] = {0};
    char* desc;
    int err = eglGetError();
    switch(err) {
#define E(code, text) case code: desc = (char*) text; break
        case EGL_SUCCESS: desc = NULL; // "No error"
        E(EGL_NOT_INITIALIZED, "EGL not initialized or failed to initialize");
        E(EGL_BAD_ACCESS, "Resource inaccessible");
        E(EGL_BAD_ALLOC, "Cannot allocate resources");
        E(EGL_BAD_ATTRIBUTE, "Unrecognized attribute or attribute value");
        E(EGL_BAD_CONTEXT, "Invalid EGL context");
        E(EGL_BAD_CONFIG, "Invalid EGL frame buffer configuration");
        E(EGL_BAD_CURRENT_SURFACE, "Current surface is no longer valid");
        E(EGL_BAD_DISPLAY, "Invalid EGL display");
        E(EGL_BAD_SURFACE, "Invalid surface");
        E(EGL_BAD_MATCH, "Inconsistent arguments");
        E(EGL_BAD_PARAMETER, "Invalid argument");
        E(EGL_BAD_NATIVE_PIXMAP, "Invalid native pixmap");
        E(EGL_BAD_NATIVE_WINDOW, "Invalid native window");
        E(EGL_CONTEXT_LOST, "Context lost");
#undef E
        default:
            snprintf(descBuf, sizeof(descBuf) - 1, "Unknown error (%d)", err);
            desc = descBuf;
    }

    if (desc)
        log("renderer: %s: %s (%s:%d)\n", msg, desc, __FILE__, line);

    return 0;
}

static inline __always_inline void vprintEglError(char* msg, int line) {
    printEglError(msg, line);
}

static void checkGlError(int line) {
    GLenum error;
    char *desc = NULL;
    for (error = glGetError(); error; error = glGetError()) {
        switch (error) {
#define E(code) case code: desc = (char*)#code; break
            E(GL_INVALID_ENUM);
            E(GL_INVALID_VALUE);
            E(GL_INVALID_OPERATION);
            E(GL_STACK_OVERFLOW_KHR);
            E(GL_STACK_UNDERFLOW_KHR);
            E(GL_OUT_OF_MEMORY);
            E(GL_INVALID_FRAMEBUFFER_OPERATION);
            E(GL_CONTEXT_LOST_KHR);
            default:
                continue;
#undef E
        }
        log("Xlorie: GLES %d ERROR: %s.\n", line, desc);
        return;
    }
}

#define checkGlError() checkGlError(__LINE__)

static const char vertexShaderSrc[] =
    "attribute vec4 position;\n"
    "attribute vec2 texCoords;"
    "varying vec2 outTexCoords;\n"
    "void main(void) {\n"
    "   outTexCoords = texCoords;\n"
    "   gl_Position = position;\n"
    "}\n";

#define FRAGMENT_SHADER(texture) \
    "precision mediump float;\n" \
    "varying vec2 outTexCoords;\n" \
    "uniform sampler2D texture;\n" \
    "void main(void) {\n" \
    "   gl_FragColor = texture2D(texture, outTexCoords)" texture ";\n" \
    "}\n"

static const char fragmentShaderSrc[] = FRAGMENT_SHADER();
static const char fragmentShaderBgraSrc[] = FRAGMENT_SHADER(".bgra");

static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext ctx = EGL_NO_CONTEXT;
static EGLSurface defaultSfc = EGL_NO_SURFACE, sfc = EGL_NO_SURFACE;
static EGLConfig cfg = 0;
static ANativeWindow *defaultWin = NULL, *win = NULL;
static volatile struct xorg_list addedBuffers, buffers, removedBuffers;
/* Removed buffers still named by a queued copy, kept imported until it has been drained. A list of
 * their own, because removedBuffers being non-empty is what wakes the renderer to release it - kept
 * there, they would wake it every time round and make the loop spin. */
static struct xorg_list retainedBuffers;
volatile jint filtering = GL_NEAREST;

static volatile bool stateChanged = false, windowChanged = false;
static volatile struct lorie_shared_server_state* pendingState = NULL;
static volatile ANativeWindow* pendingWin = NULL;
static volatile int viewportX = 0, viewportY = 0, viewportW = 0, viewportH = 0, expectedW = 0, expectedH = 0;
// Set whenever the expected root window size is (re)published, so the render thread drops a
// "waiting for a buffer of the right size" state that this new size may well have resolved.
static volatile bool expectedSizeChanged = false;

// Defined further down, next to the frame it retires; called from rendererRefreshContext() above it.
static void rendererRetireFrame(void);

/*
 * Cursor as a SurfaceControl overlay.
 *
 * Moving the pointer used to cost a whole GL frame and an eglSwapBuffers, which under ANGLE is a
 * Vulkan queue submission. Measured while a window was being dragged: 33 to 54 of those every five
 * seconds, spent entirely on the cursor, at exactly the moment the X clients were short of GPU time
 * and stalling. On API 29 and up the cursor becomes its own layer that a dedicated Choreographer
 * thread repositions, so a move touches no GL at all.
 *
 * Below API 29 the GL cursor draw stays. That split is by Android version, not by GL driver: the
 * overlay path bypasses GL rather than taking a different route through it, so it behaves the same
 * whether GLES is the platform driver or ANGLE on Vulkan.
 */
static bool cursorOverlayUsable(void);
static void ensureCursorOverlay(void);
static void teardownCursorOverlay(void);
static void markCursorOverlayDirty(bool bufferMightHaveChanged);
static void ensureRootOverlay(void);
static void teardownRootOverlay(void);
static bool rootZeroCopyUsable(const LorieBuffer_Desc *desc);
static bool rootZcDrainRetiring(void);
static void rootZcStopPresenting(void);
static bool rootZcPresent(const LorieBuffer_Desc *desc, int surfaceW, int surfaceH, int64_t frameStartNs);

static ASurfaceControl *cursorSurfaceControl = NULL;
static AChoreographer *cursorOverlayChoreographer = NULL;
static ALooper *cursorOverlayLooper = NULL; // set once by the overlay thread, then read-only
static pthread_t cursorOverlayThread;
static bool cursorOverlayThreadStarted = false, cursorOverlayFeatureAvailable = false;
static pthread_mutex_t cursorOverlayLock = PTHREAD_MUTEX_INITIALIZER;
// Guarded by cursorOverlayLock.
static bool cursorOverlayGeometryDirty = false, cursorOverlayBufferDirty = false;
static bool cursorOverlayCallbackArmed = false; // is a choreographer callback pending?
static AHardwareBuffer *cursorOverlayPendingBuffer = NULL; // reference held for the overlay thread
// Renderer thread only. Two render targets, because the compositor keeps reading the buffer it was
// given until a later one replaces it: rendering a new cursor image into that same buffer both races
// its reads and hands it a handle it already has, which is why a cursor change could leave the old
// image on screen. Alternating means every update arrives as a buffer the compositor has not seen.
static LorieBuffer *cursorOverlayRenderTarget[2] = { NULL, NULL };
static uint32_t cursorOverlayTargetW[2] = { 0, 0 }, cursorOverlayTargetH[2] = { 0, 0 };
static unsigned cursorOverlayTargetIndex = 0;
static GLuint cursorOverlayFbo = 0;
static uint32_t cursorOverlayRawW = 0, cursorOverlayRawH = 0;
// The root buffer's size as of the last frame. The overlay thread needs it to place the cursor and
// has no access to the root buffer, so the renderer publishes it here.
static volatile float cursorOverlaySourceW = 0.f, cursorOverlaySourceH = 0.f;

/*
 * Handing the root buffer straight to the compositor.
 *
 * The renderer's whole job in a frame used to be one full-screen textured quad from the root buffer
 * into the window surface, followed by eglSwapBuffers - which under ANGLE is a Vulkan queue
 * submission. The compositor then scaled and composited that surface anyway. Since the root buffer
 * is already an AHardwareBuffer, it can be given to the compositor as a layer's buffer instead, and
 * that entire pass disappears: no draw, no swap, and the display's own scaler does the scaling the
 * GL blit was doing.
 *
 * Two things had to exist first. The cursor had to stop being drawn into the same surface, which is
 * what the cursor overlay is for, and the root had to have three buffers, because the compositor
 * keeps reading one until a later one is latched.
 *
 * The buffer only goes back to the X server once the compositor says it has finished reading it -
 * the previous-release fence from the transaction's completion callback. Handing it back any earlier
 * is exactly the tearing this cannot afford, so a frame that arrives before the fence is dropped
 * rather than rushed.
 *
 * Not used when the filtering preference is nearest: the compositor's scaler is bilinear and has no
 * nearest mode, so there the GL pass is what honours the setting.
 */
static ASurfaceControl *rootSurfaceControl = NULL;
static pthread_mutex_t rootOverlayLock = PTHREAD_MUTEX_INITIALIZER;

// Two slots stay with the X server - one it draws into, one to publish into - so this is how many
// are ours. Holding one more than this blocks it from publishing at all, which is what dropped the
// desktop to 72 updates a second against a 120Hz display.
/* ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE. Spelled out because the enum is not in the headers this
 * builds against at minSdk, the same reason the calls themselves are resolved with dlsym. */
#define LORIE_SC_TRANSPARENCY_OPAQUE 2

#define LORIE_ZC_MAX_HELD (LORIE_ROOT_SLOTS - 2)

// All guarded by rootOverlayLock.
static int rootZcDisplayedSlot = -1;   // in the transaction we applied last; the compositor reads it
static uint64_t rootZcDisplayedId = 0;  // which buffer that slot held then - see rendererReleaseRootSlot

/*
 * A zero-copy frame that had to be dropped has to be tried again, and nothing else will ask for it.
 * The X server has published content that is not on screen, it does not publish the same content
 * twice, and dropping the frame clears drawRequested - so with no further damage the renderer went
 * back to sleep and that content stayed unshown until something else happened to dirty the root,
 * which on a still desktop is nothing at all.
 */
static bool rootZcRetryPending = false;

// The ones behind it, each waiting for the release fence that the next transaction's completion
// reports. More than one, because requiring the oldest to be back before presenting again meant
// waiting a whole vsync for a callback that arrives during it - which dropped every other frame and
// left the desktop updating at half the display's rate.
/*
 * seq identifies the handover, not the slot. The completion callback used to be given the slot
 * index, and matched on it: after the slot pool is recreated - a resize, a reconnect - a callback
 * still in flight from the old pool matched whichever new entry happened to land on the same index
 * and marked its release fence as arrived, with a fence from the old transaction that had long
 * since signalled. The new slot was then given back to the X server while the compositor was still
 * displaying it. Sequence numbers are not reused until they wrap at 2^32, far beyond the life of
 * any entry, so a stale callback now matches nothing.
 *
 * fenceArrivedNs is when the callback came, which is what bounds the wait if the fence it handed
 * over turns out to be unusable.
 */
static struct {
    int slot, fenceFd;
    bool fenceArrived;
    /* The fence arrived but cannot be waited on - a bad or closed descriptor. The slot stays held:
     * the compositor has not said it is finished with the buffer, and time passing is not the same
     * statement. It is given back when the pool it belongs to is destroyed, and until then the
     * pool simply runs one buffer short, which shows up as slots being unavailable rather than as
     * a torn frame. */
    bool fenceUnusable;
    uint32_t seq;
    int64_t fenceArrivedNs;
    uint64_t bufferId;   // which buffer the slot held when it went to the compositor
} rootZcRetiring[LORIE_ZC_MAX_HELD];
static int rootZcRetiringCount = 0;
static uint32_t rootZcRetireSeq = 0;
/* How many of the entries above will never resolve. Past a point there is no longer room to retire
 * a slot at all, and this path would hold every frame back forever; falling back to GL and saying
 * why beats a screen that has stopped updating. */
static int rootZcUnusableCount = 0;

// The SurfaceControl entry points are API 29, above our minSdk 26, and the NDK marks anything above
// minSdk unavailable rather than weak - so __builtin_available cannot guard a call to them and they
// cannot be linked either. Resolving them at runtime sidesteps both, and doubles as the availability
// check: on an older device the symbols are simply absent and the GL cursor draw stays. The
// alternative, turning on weak API references for the whole project, would downgrade every other
// API guard in it to a warning.
static struct {
    ASurfaceControl *(*createFromWindow)(ANativeWindow *, const char *);
    void (*release)(ASurfaceControl *);
    ASurfaceTransaction *(*txCreate)(void);
    void (*txDelete)(ASurfaceTransaction *);
    void (*txApply)(ASurfaceTransaction *);
    void (*txSetVisibility)(ASurfaceTransaction *, ASurfaceControl *, int8_t);
    void (*txSetZOrder)(ASurfaceTransaction *, ASurfaceControl *, int32_t);
    void (*txSetBuffer)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int);
    void (*txSetGeometry)(ASurfaceTransaction *, ASurfaceControl *, const ARect *, const ARect *, int32_t);
    void (*txReparent)(ASurfaceTransaction *, ASurfaceControl *, ASurfaceControl *); // optional
    void (*txSetBufferTransparency)(ASurfaceTransaction *, ASurfaceControl *, int8_t); // optional
    void (*txSetOnComplete)(ASurfaceTransaction *, void *, void (*)(void *, ASurfaceTransactionStats *));
    int (*statsPrevReleaseFenceFd)(ASurfaceTransactionStats *, ASurfaceControl *);
    void (*statsGetControls)(ASurfaceTransactionStats *, ASurfaceControl ***, size_t *);
    void (*statsReleaseControls)(ASurfaceControl **);
} scApi;

static bool cursorOverlayResolveApi(void) {
    static bool tried = false;

    if (tried)
        return scApi.createFromWindow != NULL;
    tried = true;

    scApi.createFromWindow = (ASurfaceControl *(*)(ANativeWindow *, const char *))
        dlsym(RTLD_DEFAULT, "ASurfaceControl_createFromWindow");
    scApi.release = (void (*)(ASurfaceControl *)) dlsym(RTLD_DEFAULT, "ASurfaceControl_release");
    scApi.txCreate = (ASurfaceTransaction *(*)(void)) dlsym(RTLD_DEFAULT, "ASurfaceTransaction_create");
    scApi.txDelete = (void (*)(ASurfaceTransaction *)) dlsym(RTLD_DEFAULT, "ASurfaceTransaction_delete");
    scApi.txApply = (void (*)(ASurfaceTransaction *)) dlsym(RTLD_DEFAULT, "ASurfaceTransaction_apply");
    scApi.txSetVisibility = (void (*)(ASurfaceTransaction *, ASurfaceControl *, int8_t))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setVisibility");
    scApi.txSetZOrder = (void (*)(ASurfaceTransaction *, ASurfaceControl *, int32_t))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setZOrder");
    scApi.txSetBuffer = (void (*)(ASurfaceTransaction *, ASurfaceControl *, AHardwareBuffer *, int))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBuffer");
    scApi.txSetGeometry = (void (*)(ASurfaceTransaction *, ASurfaceControl *, const ARect *, const ARect *, int32_t))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setGeometry");

    scApi.txSetOnComplete = (void (*)(ASurfaceTransaction *, void *, void (*)(void *, ASurfaceTransactionStats *)))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setOnComplete");
    if (!scApi.txSetOnComplete) // spelled this way in some header revisions
        scApi.txSetOnComplete = (void (*)(ASurfaceTransaction *, void *, void (*)(void *, ASurfaceTransactionStats *)))
            dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setOnCompleteFunc");
    scApi.statsPrevReleaseFenceFd = (int (*)(ASurfaceTransactionStats *, ASurfaceControl *))
        dlsym(RTLD_DEFAULT, "ASurfaceTransactionStats_getPreviousReleaseFenceFd");
    scApi.statsGetControls = (void (*)(ASurfaceTransactionStats *, ASurfaceControl ***, size_t *))
        dlsym(RTLD_DEFAULT, "ASurfaceTransactionStats_getASurfaceControls");
    scApi.statsReleaseControls = (void (*)(ASurfaceControl **))
        dlsym(RTLD_DEFAULT, "ASurfaceTransactionStats_releaseASurfaceControls");

    // Optional: hiding the layer is enough to get it off the screen, taking it out of the tree as
    // well is just tidier.
    scApi.txSetBufferTransparency = (void (*)(ASurfaceTransaction *, ASurfaceControl *, int8_t))
            dlsym(RTLD_DEFAULT, "ASurfaceTransaction_setBufferTransparency");
    scApi.txReparent = (void (*)(ASurfaceTransaction *, ASurfaceControl *, ASurfaceControl *))
        dlsym(RTLD_DEFAULT, "ASurfaceTransaction_reparent");

    if (!scApi.createFromWindow || !scApi.release || !scApi.txCreate || !scApi.txDelete ||
        !scApi.txApply || !scApi.txSetVisibility || !scApi.txSetZOrder || !scApi.txSetBuffer ||
        !scApi.txSetGeometry) {
        log("Xlorie: no SurfaceControl on this device, drawing the cursor in GL instead");
        scApi.createFromWindow = NULL; // one missing piece makes the whole path unusable
        return false;
    }
    return true;
}

static pthread_mutex_t stateLock;
// Shared with the X server so it can signal us directly. Only this thread ever waits on it, so stateLock
// (the companion mutex) doesn't need to be shared too.
static pthread_cond_t* stateCond;
static pthread_cond_t stateChangeFinishCond;
static pthread_spinlock_t bufferLock;
static int stateCondFd = -1;

static volatile bool smoothPresentationEnabled = false;
static volatile bool rendererPerfLogEnabled = false;
static volatile bool rendererPostSwapTouchEnabled = true;
static volatile bool rendererPostSwapFenceWaitEnabled = true;
static volatile bool rendererRootFenceWaitEnabled = true;
// Deliberately delays a redraw by a millisecond or two after a slow swap. Its own flag now:
// deriving it from the three waits above meant it silently turned itself ON in the mode that
// asks for the lowest latency, which is the opposite of what the output modes intend.
static volatile bool rendererSwapBackpressureGuardEnabled = false;
static volatile bool presentModeChanged = false;
static volatile bool rendererOptionsReady = false;
static uint64_t rendererPerfFrameNo = 0;
static int64_t rendererLastPerfFrameStartNs = 0;
static int rendererSwapPressureScore = 0;
static int rendererPreRedrawCoalesceFrames = 0;
static int64_t rendererPreRedrawCoalesceWaitUs = 0;
static int64_t rendererLastPreRedrawCoalesceWaitUs = -1;
static uint64_t rendererPreRedrawCoalescedCount = 0;
static int64_t rendererBackpressureLastSwapUs = 0;
static float rendererDisplayRefreshRateHz = 60.0f;
static bool rendererHighRefreshEnabled = false;

#ifndef RENDERER_V330_ONSCREEN_SWAP_INTERVAL
#define RENDERER_V330_ONSCREEN_SWAP_INTERVAL 1
#endif
#ifndef RENDERER_V330_LOW_REFRESH_SWAP_INTERVAL
#define RENDERER_V330_LOW_REFRESH_SWAP_INTERVAL 0
#endif

/* v3.30-swap-interval-helper-begin */
static int rendererV330LastSwapInterval = -999;
static int rendererV330LogBudget = 8;

static void
rendererV330MaybeSetSwapInterval(EGLDisplay display)
{
    // v3.30: do not sleep, skip, wait, or coalesce.
    // High-refresh on-screen uses EGL native pacing.
    // Low-refresh / DeX keeps the existing immediate path.
    int target = rendererHighRefreshEnabled ?
            RENDERER_V330_ONSCREEN_SWAP_INTERVAL :
            RENDERER_V330_LOW_REFRESH_SWAP_INTERVAL;

    if (rendererV330LastSwapInterval == target)
        return;

    EGLBoolean ok = eglSwapInterval(display, target);

    if (ok) {
        rendererV330LastSwapInterval = target;

        if (rendererV330LogBudget > 0) {
            __android_log_print(ANDROID_LOG_DEBUG, "gles-renderer",
                                "v3.30 eglSwapInterval target=%d hr_enabled=%d",
                                target, rendererHighRefreshEnabled ? 1 : 0);
            rendererV330LogBudget--;
        }
    } else {
        if (rendererV330LogBudget > 0) {
            __android_log_print(ANDROID_LOG_WARN, "gles-renderer",
                                "v3.30 eglSwapInterval failed target=%d hr_enabled=%d",
                                target, rendererHighRefreshEnabled ? 1 : 0);
            rendererV330LogBudget--;
        }
    }
}
/* v3.30-swap-interval-helper-end */

static int64_t rendererRefreshBudgetUs = 16667;
static int rendererHighRefreshPlateauScore = 0;
static int rendererHighRefreshGoodFrames = 0;
static int rendererHighRefreshLimitFrames = 0;
static int64_t rendererHighRefreshLimitWaitUs = 0;
static int64_t rendererLastHighRefreshLimitWaitUs = -1;
static uint64_t rendererHighRefreshLimitedCount = 0;

#define RENDERER_MEDIUM_SWAP_US 8000
#define RENDERER_SLOW_SWAP_US 12000
#define RENDERER_PRESEVERE_SWAP_US 18000
#define RENDERER_SEVERE_SWAP_US 20000
#define RENDERER_VERY_SEVERE_SWAP_US 25000
#define RENDERER_RECOVERED_SWAP_US 8000
#define RENDERER_PRESSURE_SCORE_MAX 6
#define RENDERER_COALESCE_WAIT_PRESEVERE_US 1000
#define RENDERER_COALESCE_WAIT_SEVERE_US 2000
#define RENDERER_COALESCE_WAIT_VERY_SEVERE_US 3000
#ifndef RENDERER_DEX_PRESENT_STORM_SWAP_US
#define RENDERER_DEX_PRESENT_STORM_SWAP_US 10500
#endif
#ifndef RENDERER_DEX_PRESENT_STORM_STREAK_FRAMES
#define RENDERER_DEX_PRESENT_STORM_STREAK_FRAMES 2
#endif
#ifndef RENDERER_DEX_PRESENT_SKIP_FRAMES
#define RENDERER_DEX_PRESENT_SKIP_FRAMES 1
#endif
#ifndef RENDERER_DEX_PRESENT_SKIP_COOLDOWN_FRAMES
#define RENDERER_DEX_PRESENT_SKIP_COOLDOWN_FRAMES 8
#endif

/* v3.22-swap-skip-helper-begin */
static int rendererDexPresentStormStreakFrames = 0;
static int rendererDexPresentSkipCooldownFrames = 0;
static int rendererDexPresentSkipFrames = 0;

static EGLBoolean
rendererMaybeSkipEglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (rendererDexPresentSkipFrames > 0) {
        rendererDexPresentSkipFrames--;
        __android_log_print(ANDROID_LOG_DEBUG, "gles-renderer",
                            "v3.22 DeX present skip remaining=%d",
                            rendererDexPresentSkipFrames);
        return EGL_TRUE;
    }

    /* v3.30-swap-interval-call-begin */
    rendererV330MaybeSetSwapInterval(display);
    /* v3.30-swap-interval-call-end */
    return eglSwapBuffers(display, surface);
}

#define eglSwapBuffers(display, surface) rendererMaybeSkipEglSwapBuffers((display), (surface))
/* v3.22-swap-skip-helper-end */
#ifndef RENDERER_ONSCREEN_SPIKE_SWAP_US
#define RENDERER_ONSCREEN_SPIKE_SWAP_US 6500
#endif
#ifndef RENDERER_ONSCREEN_SPIKE_WAIT_US
#define RENDERER_ONSCREEN_SPIKE_WAIT_US 500
#endif
#ifndef RENDERER_ONSCREEN_SPIKE_FRAMES
#define RENDERER_ONSCREEN_SPIKE_FRAMES 1
#endif
#ifndef RENDERER_ONSCREEN_SPIKE_COOLDOWN_FRAMES
#define RENDERER_ONSCREEN_SPIKE_COOLDOWN_FRAMES 24
#endif
#ifndef RENDERER_ONSCREEN_BURST_WINDOW_FRAMES
#define RENDERER_ONSCREEN_BURST_WINDOW_FRAMES 36
#endif
#ifndef RENDERER_ONSCREEN_BURST_MAX_ARMS
#define RENDERER_ONSCREEN_BURST_MAX_ARMS 4
#endif
static int rendererOnscreenSpikeCooldownFrames = 0;
static int rendererOnscreenSpikeWindowFrames = 0;
static int rendererOnscreenSpikeArmsInWindow = 0;
#define RENDERER_HR_PLATEAU_SWAP_US 12000
#define RENDERER_HR_SEVERE_SWAP_US 15000
#define RENDERER_HR_VERY_SEVERE_SWAP_US 18000
#define RENDERER_HR_RECOVERED_SWAP_US 9500
#define RENDERER_HR_PLATEAU_SCORE_MAX 12
#define RENDERER_HR_GOOD_FRAMES_TO_RECOVER 4
#define RENDERER_HR_LIMIT_WAIT_PLATEAU_US 500
#define RENDERER_HR_LIMIT_WAIT_SEVERE_US 1000
#define RENDERER_HR_LIMIT_WAIT_VERY_SEVERE_US 1500
static int64_t rendererLastFrameStartNs = 0;

static int64_t rendererNowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t) ts.tv_sec * 1000000000LL) + ts.tv_nsec;
}

static inline int64_t rendererNsToUs(int64_t ns) {
    return ns / 1000LL;
}






static EGLint rendererGetSwapInterval(void) {
    return smoothPresentationEnabled ? 1 : 0;
}

static const char *rendererGetPresentModeName(void) {
    return smoothPresentationEnabled ? "smooth" : "immediate";
}

static void rendererApplyPresentMode(void) {
    EGLint interval;

    if (egl_display == EGL_NO_DISPLAY)
        return;

    interval = rendererGetSwapInterval();

    if (eglSwapInterval(egl_display, interval) != EGL_TRUE) {
        printEglError("eglSwapInterval failed", __LINE__);
        return;
    }

    log("Xlorie: present mode=%s swap_interval=%d\n",
        rendererGetPresentModeName(), interval);
}

static volatile struct lorie_shared_server_state* state = NULL;

/*
 * One place sets it, because three places have to agree on it: the sleep predicate, the redraw
 * dispatcher, and the test for a frame that is only a cursor move. The flag was added to the
 * predicate alone, so the thread woke up, decided there was work, reached a dispatcher that did not
 * know about it, did nothing, and went round again - a busy loop that submitted nothing. And a
 * cursor move arriving first took the cursor-only path, which skips the output entirely.
 *
 * Also published to the X server, which is what opens the vsync gate and only signalled for new
 * damage or a cursor move.
 */
static void rendererSetOutputRetry(bool pending) {
    rootZcRetryPending = pending;
    if (state)
        state->outputRetryPending = pending ? 1u : 0u;
}
static struct {
    GLuint id;
    bool cursorChanged;
} cursor;

// FBO used to blit deferred Present "copy" entries (see lorieTryScheduleGpuCopy) into the root texture.
static GLuint gpuCopyFbo = 0;

// The renderer's end of activity.c's socket to the X server; used to notify it immediately when a
// GPU copy batch finishes instead of it waiting for the next vblank-tick poll.
extern volatile int conn_fd;

static void notifyGpuCopyDone(void) {
    if (conn_fd != -1) {
        lorieEvent e = { .type = EVENT_GPU_COPY_DONE };
        write(conn_fd, &e, sizeof(e));
    }
}

GLuint g_texture_program = 0, gv_pos = 0, gv_coords = 0;
GLuint g_texture_program_bgra = 0, gv_pos_bgra = 0, gv_coords_bgra = 0;

static void* rendererThread(void);

static inline __always_inline void bindTexture(GLuint id) {
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE
};

const EGLint ctxattribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE
};

static void onImageAvailable(void* context, AImageReader* reader) {
    AImage* image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) == AMEDIA_OK && image)
        AImage_delete(image);
}

int rendererInitThread(void) {
    EGLint major, minor;
    EGLint numConfigs;
    EGLint *const alphaAttrib = &configAttribs[11];
    AImageReader* reader = NULL; // We will use this ImageReader each time surface is destroyed, zero reasons to clean it up

    pthread_setname_np(pthread_self(), "LorieRendererThread");

    xorg_list_init(&addedBuffers);
    xorg_list_init(&buffers);
    xorg_list_init(&removedBuffers);
    xorg_list_init(&retainedBuffers);

    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY)
        return printEglError("Got no EGL display", __LINE__);

    if (eglInitialize(egl_display, &major, &minor) != EGL_TRUE)
        return printEglError("Unable to initialize EGL", __LINE__);

    log("Xlorie: Initialized EGL version %d.%d\n", major, minor);


    eglBindAPI(EGL_OPENGL_ES_API);

    if (eglChooseConfig(egl_display, configAttribs, &cfg, 1, &numConfigs) != EGL_TRUE &&
        (*alphaAttrib = 8) &&
        eglChooseConfig(egl_display, configAttribs, &cfg, 1, &numConfigs) != EGL_TRUE)
        return printEglError("eglChooseConfig failed", __LINE__);

    ctx = eglCreateContext(egl_display, cfg, NULL, ctxattribs);
    if (ctx == EGL_NO_CONTEXT)
        return printEglError("eglCreateContext failed", __LINE__);

    // Weird devices without proper EGL_KHR_surfaceless_context support
    // We can not use pbuffer-based surfaces because it will require searching for configs supporting it
    // and I am not sure all devices have configs supporting both pbuffers and regular surfaces simultaneously
    if (AImageReader_new(1, 1, AIMAGE_FORMAT_RGBA_8888, 2, &reader) != AMEDIA_OK) {
        log("Failed to initialise ImageReader");
        return 1;
    }

    if (AImageReader_setImageListener(reader, &(AImageReader_ImageListener) { .context = NULL, .onImageAvailable = onImageAvailable }) != AMEDIA_OK) {
        log("Failed to set ImageReader listener");
        AImageReader_delete(reader);
        return 1;
    }

    if (AImageReader_getWindow(reader, &defaultWin) != AMEDIA_OK) {
        log("Failed to obtain ImageReader native window");
        AImageReader_delete(reader);
        return 1;
    }

    win = defaultWin;
    ANativeWindow_acquire(defaultWin);

    sfc = defaultSfc = eglCreateWindowSurface(egl_display, cfg, win, NULL);

    eglMakeCurrent(egl_display, sfc, sfc, ctx);
    rendererApplyPresentMode();

    g_texture_program = createProgram(vertexShaderSrc, fragmentShaderSrc);
    if (!g_texture_program)
        log("Xlorie: GLESv2: Unable to create shader program.\n");

    g_texture_program_bgra = createProgram(vertexShaderSrc, fragmentShaderBgraSrc);
    if (!g_texture_program_bgra)
        log("Xlorie: GLESv2: Unable to create bgra shader program.\n");

    gv_pos = (GLuint) glGetAttribLocation(g_texture_program, "position");
    gv_coords = (GLuint) glGetAttribLocation(g_texture_program, "texCoords");

    gv_pos_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "position");
    gv_coords_bgra = (GLuint) glGetAttribLocation(g_texture_program_bgra, "texCoords");

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &cursor.id);
    // Allocated once at the largest cursor the shared state can carry (lorieSetCursor rejects
    // anything bigger). Respecifying a texture with glTexImage2D on every cursor shape change means
    // a fresh image allocation inside the frame, which is expensive on a driver that has to back it
    // with a new VkImage; glTexSubImage2D into this one costs nothing but the upload.
    glBindTexture(GL_TEXTURE_2D, cursor.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, LORIE_CURSOR_TEX_SIZE, LORIE_CURSOR_TEX_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    rendererThread();
    return 1;
}

void rendererInit(JNIEnv* env) {
    pthread_t t;

    if (ctx)
        return;

    pthread_mutex_init(&stateLock, NULL);

    // Created once, never recreated; only the fd is (re)sent to the X server whenever it (re)connects.
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    stateCondFd = LorieBuffer_createRegion("renderer-cond", sizeof(pthread_cond_t));
    stateCond = stateCondFd == -1 ? MAP_FAILED : mmap(NULL, sizeof(pthread_cond_t), PROT_READ|PROT_WRITE, MAP_SHARED, stateCondFd, 0);
    if (stateCond == MAP_FAILED) {
        loge("Failed to allocate renderer wakeup cond var, aborting");
        abort();
    }
    pthread_cond_init(stateCond, &cond_attr);

    pthread_cond_init(&stateChangeFinishCond, NULL);
    pthread_spin_init(&bufferLock, false);

    rendererOptionsReady = true; pthread_create(&t, NULL, (void*(*)(void*)) rendererInitThread, NULL);
}

int rendererGetWakeupCondFd(void) {
    return stateCondFd;
}

void rendererSetFiltering(JNIEnv* env, jobject self, jint f) {
    filtering = f;
}

void rendererSetSmoothPresentationEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    smoothPresentationEnabled = enabled == JNI_TRUE;

    if (!rendererOptionsReady)
        return;

    pthread_mutex_lock(&stateLock);
    presentModeChanged = true;
    pthread_cond_signal(stateCond);
    pthread_mutex_unlock(&stateLock);
}

void rendererSetPerfLogEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    rendererPerfLogEnabled = enabled == JNI_TRUE;
}

void rendererSetPostSwapTouchEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    rendererPostSwapTouchEnabled = enabled == JNI_TRUE;
}

void rendererSetPostSwapFenceWaitEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    rendererPostSwapFenceWaitEnabled = enabled == JNI_TRUE;
}


void rendererSetRootFenceWaitEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    rendererRootFenceWaitEnabled = enabled == JNI_TRUE;
}

void rendererSetSwapBackpressureGuardEnabled(JNIEnv* env, jobject self, jboolean enabled) {
    (void) env;
    (void) self;

    rendererSwapBackpressureGuardEnabled = enabled == JNI_TRUE;
}















void rendererTestCapabilities(int* legacy_drawing) {
    // Some devices do not support sampling from HAL_PIXEL_FORMAT_BGRA_8888, here we are checking it.
    const EGLint imageAttributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLint numConfigs;
    EGLClientBuffer clientBuffer;
    EGLImageKHR img;
    EGLint major, minor;
    AHardwareBuffer *new = NULL;
    int status;
    AHardwareBuffer_Desc d0 = {
            .width = 64,
            .height = 64,
            .layers = 1,
            .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
            .format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
    };

    if (egl_display == EGL_NO_DISPLAY) {
        egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (egl_display == EGL_NO_DISPLAY)
            return vprintEglError("Got no EGL display", __LINE__);
    }

    if (eglInitialize(egl_display, &major, &minor) != EGL_TRUE)
        return vprintEglError("Unable to initialize EGL", __LINE__);

    loge("Xlorie: Initialized EGL version %d.%d\n", major, minor);
    eglBindAPI(EGL_OPENGL_ES_API);

    status = AHardwareBuffer_allocate(&d0, &new);
    if (status != 0 || new == NULL) {
        loge("Failed to allocate native buffer (%p, error %d)", new, status);
        loge("Forcing legacy drawing");
        *legacy_drawing = 1;
        return;
    }

    uint32_t *pixels;
    if (AHardwareBuffer_lock(new, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, (void **) &pixels) == 0) {
        pixels[0] = 0xAABBCCDD;
        AHardwareBuffer_unlock(new, NULL);
    } else {
        loge("Failed to lock native buffer (%p, error %d)", new, status);
        loge("Forcing legacy drawing");
        *legacy_drawing = 1;
        AHardwareBuffer_release(new);
        return;
    }

    clientBuffer = eglGetNativeClientBufferANDROID(new);
    if (!clientBuffer) {
        *legacy_drawing = 1;
        AHardwareBuffer_release(new);
        return vprintEglError("Failed to obtain EGLClientBuffer from AHardwareBuffer, forcing legacy drawing", __LINE__);
    }

    if (!(img = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttributes))) {
        loge("Failed to obtain EGLImageKHR from EGLClientBuffer");
        loge("Forcing legacy drawing");
        *legacy_drawing = 1;
        AHardwareBuffer_release(new);
    } else {
        // For some reason all devices I checked had no GL_EXT_texture_format_BGRA8888 support, but some of them still provided BGRA extension.
        // EGL does not provide functions to query texture format in runtime.
        // Workarounds are less performant but at least they let us use Termux:X11 on devices with missing BGRA support.
        // We handle two cases.
        // If resulting texture has BGRA format but still drawing RGBA we should flip format to RGBA and flip pixels manually in shader.
        // In the case if for some reason we can not use HAL_PIXEL_FORMAT_BGRA_8888 we should fallback to legacy drawing method (uploading pixels via glTexImage2D).
        configAttribs[1] = EGL_PBUFFER_BIT;
        EGLConfig checkcfg = 0;
        GLuint fbo = 0, texture = 0;
        if (eglChooseConfig(egl_display, configAttribs, &checkcfg, 1, &numConfigs) != EGL_TRUE)
            return vprintEglError("check eglChooseConfig failed", __LINE__);

        EGLContext testctx = eglCreateContext(egl_display, checkcfg, NULL, ctxattribs);
        if (testctx == EGL_NO_CONTEXT)
            return vprintEglError("check eglCreateContext failed", __LINE__);

        const EGLint pbufferAttributes[] = {
                EGL_WIDTH, 64,
                EGL_HEIGHT, 64,
                EGL_NONE,
        };
        EGLSurface checksfc = eglCreatePbufferSurface(egl_display, checkcfg, pbufferAttributes);

        if (eglMakeCurrent(egl_display, checksfc, checksfc, testctx) != EGL_TRUE)
            return vprintEglError("check eglMakeCurrent failed", __LINE__);

        // Which GLES implementation we ended up on decides a lot of the present path's cost
        // (an ANGLE-on-Vulkan driver imports AHardwareBuffers and waits on fences very
        // differently from a native one), so make it visible in the log instead of guessing.
        loge("Xlorie: GL_VENDOR=%s GL_RENDERER=%s GL_VERSION=%s",
             (const char *) glGetString(GL_VENDOR),
             (const char *) glGetString(GL_RENDERER),
             (const char *) glGetString(GL_VERSION));

        glActiveTexture(GL_TEXTURE0); checkGlError();
        glGenTextures(1, &texture); checkGlError();
        bindTexture(texture);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img); checkGlError();
        glGenFramebuffers(1, &fbo); checkGlError();
        glBindFramebuffer(GL_FRAMEBUFFER, fbo); checkGlError();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0); checkGlError();
        uint32_t pixel[64*64];
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pixel); checkGlError();
        if (pixel[0] != 0xAABBCCDD && pixel[0] != 0xFFBBCCDD) {
            log("Xlorie: GLES receives broken pixels. Forcing legacy drawing. 0x%X\n", pixel[0]);
            *legacy_drawing = 1;
        }
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(egl_display, testctx);
        eglDestroyImageKHR(egl_display, img);
        eglDestroySurface(egl_display, checksfc);
        AHardwareBuffer_release(new);
    }
}

__unused void rendererSetSharedState(struct lorie_shared_server_state* newState) {
    pthread_mutex_lock(&stateLock);
    pendingState = newState;
    stateChanged = true;
    pthread_cond_signal(stateCond);

    while(stateChanged)
        pthread_cond_wait(&stateChangeFinishCond, &stateLock);

    pthread_mutex_unlock(&stateLock);
}

void rendererAddBuffer(LorieBuffer* buf) {
    pthread_spin_lock(&bufferLock);
    LorieBuffer_addToList(buf, &addedBuffers);
    pthread_cond_signal(stateCond);
    pthread_spin_unlock(&bufferLock);
}

void rendererRemoveBuffer(uint64_t id) {
    pthread_spin_lock(&bufferLock);
    LorieBuffer* buf = LorieBufferList_findById(&addedBuffers, id);
    if (buf)
        // Buffer was not attached to GL yet, it is safe to release it now.
        LorieBuffer_release(buf);
    else {
        buf = LorieBufferList_findById(&buffers, id);
        if (buf) {
            // The buffer is attached to GL so we should release it from renderer thread.
            LorieBuffer_removeFromList(buf);
            LorieBuffer_addToList(buf, &removedBuffers);
        }
    }
    pthread_spin_unlock(&bufferLock);
}

void rendererRemoveAllBuffers(void) {
    LorieBuffer *buf = NULL;

    pthread_spin_lock(&bufferLock);
    while ((buf = LorieBufferList_first(&addedBuffers))) {
        // These buffers are not yet attached to GL, it is safe to release them
        LorieBuffer_release(buf);
    }
    while ((buf = LorieBufferList_first(&buffers))) {
        // These buffers are attached to GL, we must release them from renderer thread.
        LorieBuffer_removeFromList(buf);
        LorieBuffer_addToList(buf, &removedBuffers);
    }
    pthread_spin_unlock(&bufferLock);
}

void rendererSetWindow(JNIEnv *env, __unused jobject thiz, jobject jsfc) {
    ANativeWindow* newWin = jsfc ? ANativeWindow_fromSurface(env, jsfc) : NULL;
    if (newWin)
        ANativeWindow_acquire(newWin);

    pthread_mutex_lock(&stateLock);
    if (newWin && pendingWin == newWin) {
        ANativeWindow_release(newWin);
        pthread_mutex_unlock(&stateLock);
        return;
    }

    if (pendingWin)
        ANativeWindow_release(pendingWin);

    pendingWin = newWin;
    expectedW = expectedH = 0;
    windowChanged = TRUE;

    pthread_cond_signal(stateCond);

    // We should wait until renderer destroys EGLSurface before SurfaceCallback::surfaceDestroyed finishes
    // Otherwise we will have weird errors like
    // `freeAllBuffers: 1 buffers were freed while being dequeued!`
    // or
    // `query: BufferQueue has been abandoned`
    while(windowChanged)
        pthread_cond_wait(&stateChangeFinishCond, &stateLock);

    pthread_mutex_unlock(&stateLock);
}

static inline __always_inline void releaseWinAndSurface(ANativeWindow** anw, EGLSurface *esfc) {
    if (esfc && *esfc && *esfc != defaultSfc) {
        // Requeue the dequeued buffer, causes flickering during window reconfiguring
        eglSwapBuffers(egl_display, *esfc);
        if (eglMakeCurrent(egl_display, defaultSfc, defaultSfc, ctx) != EGL_TRUE)
            return vprintEglError("eglMakeCurrent failed (EGL_NO_SURFACE)", __LINE__);
        if (eglDestroySurface(egl_display, *esfc) != EGL_TRUE)
            return vprintEglError("eglDestoySurface failed", __LINE__);
        *esfc = defaultSfc;
    }

    if (anw && *anw && *anw != defaultWin) {
        ANativeWindow_release(*anw);
        *anw = defaultWin;
    }
}

void rendererSetViewport(__unused JNIEnv *env, __unused jclass clazz, int x, int y, int w, int h, int ew, int eh) {
    pthread_mutex_lock(&stateLock);
    viewportX = x;
    viewportY = y;
    viewportW = w;
    viewportH = h;
    expectedW = ew;
    expectedH = eh;
    expectedSizeChanged = true;
    if (state)
        state->drawRequested = true;
    pthread_cond_signal(stateCond);
    pthread_mutex_unlock(&stateLock);
}

void rendererRefreshContext(void) {
    // The surface this frame was drawn into is about to go away.
    rendererRetireFrame();

    int width = pendingWin ? ANativeWindow_getWidth(pendingWin) : 0;
    int height = pendingWin ? ANativeWindow_getHeight(pendingWin) : 0;
    log("rendererSetWindow %p %d %d", pendingWin, width, height);

    teardownCursorOverlay(); // bound to the window we are about to drop; recreated for the new one
    teardownRootOverlay();

    releaseWinAndSurface(&win, &sfc);

    if (pendingWin && (width <= 0 || height <= 0)) {
        log("Xlorie: We've got invalid surface. Probably it became invalid before we started working with it.\n");
        releaseWinAndSurface(&pendingWin, NULL);
    }

    win = pendingWin;
    pendingWin = NULL;
    windowChanged = FALSE;

    if (!win) {
        win = defaultWin;
        eglMakeCurrent(egl_display, defaultSfc, defaultSfc, ctx);
        if (state)
            state->surfaceAvailable = false;
        notifyGpuCopyDone(); // Wake up any GPU copy stuck waiting on a surface we no longer have.
        return;
    }

    sfc = eglCreateWindowSurface(egl_display, cfg, win, NULL);
    if (sfc == EGL_NO_SURFACE)
        return vprintEglError("eglCreateWindowSurface failed", __LINE__);

    if (eglMakeCurrent(egl_display, sfc, sfc, ctx) != EGL_TRUE) {
        if (state)
            state->surfaceAvailable = false;
        notifyGpuCopyDone();
        return vprintEglError("eglMakeCurrent failed", __LINE__);
    }

    rendererApplyPresentMode();

    // We should redraw image at least once right after surface change
    if (state)
        state->surfaceAvailable = state->drawRequested = state->cursor.updated = win != defaultWin;

    glViewport(0, 0, ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
    log("Xlorie: new surface applied: %p\n", sfc);

    ensureCursorOverlay();
    ensureRootOverlay();
}

static void draw(GLuint id, float x0, float y0, float x1, float y1, float xfactor, uint8_t flip);
static void drawRegion(GLuint id, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t flip);
static void drawCursor(float displayWidth, float displayHeight);


static void rendererTimespecAddUs(struct timespec *ts, int64_t us) {
    ts->tv_sec += us / 1000000;
    ts->tv_nsec += (long) ((us % 1000000) * 1000);

    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

__unused void rendererSetDisplayRefreshRate(JNIEnv* env, jobject self, jfloat refreshRate) {
    (void) env;
    (void) self;

    if (refreshRate < 30.0f || refreshRate > 240.0f)
        refreshRate = 60.0f;

    rendererDisplayRefreshRateHz = refreshRate;

    if (refreshRate >= 90.0f) {
        rendererHighRefreshEnabled = true;
        rendererRefreshBudgetUs = (int64_t) (1000000.0f / refreshRate + 0.5f);
    } else {
        rendererHighRefreshEnabled = false;
        rendererRefreshBudgetUs = 16667;
        rendererHighRefreshPlateauScore = 0;
        rendererHighRefreshGoodFrames = 0;
        rendererHighRefreshLimitFrames = 0;
        rendererHighRefreshLimitWaitUs = 0;
        rendererLastHighRefreshLimitWaitUs = -1;
    }
}

static void rendererUpdateHighRefreshPlateauLimiter(bool enabled, int64_t swapUs, int64_t totalUs) {
    if (!enabled || !rendererHighRefreshEnabled) {
        rendererHighRefreshPlateauScore = 0;
        rendererHighRefreshGoodFrames = 0;
        rendererHighRefreshLimitFrames = 0;
        rendererHighRefreshLimitWaitUs = 0;
        return;
    }

    int64_t pressureUs = swapUs;

    if (totalUs > pressureUs)
        pressureUs = totalUs;

    // v3.8 latency-first:
    // Keep high-refresh pressure scoring for logs, but do not delay redraws.
    // v3.7 proved that 500~1500us sleeps do not break the 12~16ms plateau
    // and may add input/display latency on on-screen 120Hz.
    if (pressureUs >= RENDERER_HR_VERY_SEVERE_SWAP_US) {
        rendererHighRefreshPlateauScore += 3;
        rendererHighRefreshGoodFrames = 0;
    } else if (pressureUs >= RENDERER_HR_SEVERE_SWAP_US) {
        rendererHighRefreshPlateauScore += 2;
        rendererHighRefreshGoodFrames = 0;
    } else if (pressureUs >= RENDERER_HR_PLATEAU_SWAP_US) {
        rendererHighRefreshPlateauScore += 1;
        rendererHighRefreshGoodFrames = 0;
    } else if (pressureUs <= RENDERER_HR_RECOVERED_SWAP_US) {
        rendererHighRefreshGoodFrames++;

        if (rendererHighRefreshGoodFrames >= RENDERER_HR_GOOD_FRAMES_TO_RECOVER) {
            if (rendererHighRefreshPlateauScore > 0)
                rendererHighRefreshPlateauScore--;
        }
    } else {
        rendererHighRefreshGoodFrames = 0;
    }

    if (rendererHighRefreshPlateauScore < 0)
        rendererHighRefreshPlateauScore = 0;
    else if (rendererHighRefreshPlateauScore > RENDERER_HR_PLATEAU_SCORE_MAX)
        rendererHighRefreshPlateauScore = RENDERER_HR_PLATEAU_SCORE_MAX;

    // Latency-first mode: never arm the high-refresh wait path.
    rendererHighRefreshLimitFrames = 0;
    rendererHighRefreshLimitWaitUs = 0;
}



static void rendererUpdateSwapBackpressureGuard(bool enabled, int64_t swapUs, int64_t totalUs) {
    if (!enabled) {
        rendererSwapPressureScore = 0;
        rendererPreRedrawCoalesceFrames = 0;
        rendererPreRedrawCoalesceWaitUs = 0;
        rendererBackpressureLastSwapUs = swapUs;
        return;
    }

    rendererBackpressureLastSwapUs = swapUs;

    if (swapUs >= RENDERER_VERY_SEVERE_SWAP_US) {
        // Very severe swap: give the queue a slightly longer recovery window.
        rendererSwapPressureScore = RENDERER_PRESSURE_SCORE_MAX;
        rendererPreRedrawCoalesceFrames = 3;
        rendererPreRedrawCoalesceWaitUs = RENDERER_COALESCE_WAIT_VERY_SEVERE_US;
    } else if (swapUs >= RENDERER_SEVERE_SWAP_US) {
        // Severe swap: react clearly, but keep it short.
        if (rendererSwapPressureScore < 4)
            rendererSwapPressureScore = 4;
        else
            rendererSwapPressureScore++;

        rendererPreRedrawCoalesceFrames = 2;
        rendererPreRedrawCoalesceWaitUs = RENDERER_COALESCE_WAIT_SEVERE_US;
    } else if (swapUs >= RENDERER_PRESEVERE_SWAP_US) {
        // 18~20ms is a warning zone. Do one light coalesce to avoid crossing into very severe.
        if (rendererSwapPressureScore < 2)
            rendererSwapPressureScore = 2;

        if (rendererPreRedrawCoalesceFrames < 1)
            rendererPreRedrawCoalesceFrames = 1;

        rendererPreRedrawCoalesceWaitUs = RENDERER_COALESCE_WAIT_PRESEVERE_US;
    } else if (swapUs >= RENDERER_SLOW_SWAP_US) {
        // Ordinary 12~18ms swaps are common. Track lightly, but do not arm coalescing.
        if (rendererSwapPressureScore < 1)
            rendererSwapPressureScore = 1;

        if (rendererPreRedrawCoalesceFrames <= 0)
            rendererPreRedrawCoalesceWaitUs = 0;
    } else if (swapUs >= RENDERER_MEDIUM_SWAP_US) {
        // Medium swaps should not keep pressure around for long.
        if (rendererSwapPressureScore > 0)
            rendererSwapPressureScore--;

        if (rendererPreRedrawCoalesceFrames <= 0)
            rendererPreRedrawCoalesceWaitUs = 0;
    } else if (swapUs <= RENDERER_RECOVERED_SWAP_US) {
        // Fast recovery: non-mailbox should stay untouched.
        //
        // v3.3 sticky cooldown:
        // If a severe/very-severe swap armed pre-redraw coalescing, do not cancel
        // the remaining cooldown just because the first coalesced frame recovered.
        // This prevents mailbox from oscillating:
        //   severe swap -> one good coalesced frame -> severe swap again.
        rendererSwapPressureScore = 0;

        if (rendererPreRedrawCoalesceFrames <= 0)
            rendererPreRedrawCoalesceWaitUs = 0;
    } else if (rendererSwapPressureScore > 0) {
        rendererSwapPressureScore--;

        if (rendererPreRedrawCoalesceFrames <= 0)
            rendererPreRedrawCoalesceWaitUs = 0;
    }

    if (rendererSwapPressureScore < 0)
        rendererSwapPressureScore = 0;
    else if (rendererSwapPressureScore > RENDERER_PRESSURE_SCORE_MAX)
        rendererSwapPressureScore = RENDERER_PRESSURE_SCORE_MAX;
    // v3.11A latency-first high-refresh:
    // On 90Hz+ on-screen output, do not add any pre-redraw coalescing wait.
    // DeX/60Hz keeps the existing v3.3 severe coalescing path because
    // rendererHighRefreshEnabled is false below 90Hz.
    if (rendererHighRefreshEnabled && rendererPreRedrawCoalesceWaitUs > 0) {
        rendererPreRedrawCoalesceFrames = 0;
        rendererPreRedrawCoalesceWaitUs = 0;
    }
rendererUpdateHighRefreshPlateauLimiter(enabled, swapUs, totalUs);
    /* v3.22-dex-present-skip-begin */
    // v3.22 low-refresh present skip:
    // v3.21 confirmed the storm and applied 3ms waits, but eglSwapBuffers
    // still stayed stuck around 12-18ms. For DeX/60Hz only, confirm the
    // storm, then skip exactly one present to stop feeding BufferQueue.
    // High-refresh/on-screen output remains no-smoother/no-wait here.
    if (rendererHighRefreshEnabled) {
        rendererDexPresentStormStreakFrames = 0;
        rendererDexPresentSkipCooldownFrames = 0;
        rendererDexPresentSkipFrames = 0;
    } else {
        if (rendererDexPresentSkipCooldownFrames > 0)
            rendererDexPresentSkipCooldownFrames--;

        if (swapUs >= RENDERER_DEX_PRESENT_STORM_SWAP_US) {
            if (rendererDexPresentStormStreakFrames < RENDERER_DEX_PRESENT_STORM_STREAK_FRAMES)
                rendererDexPresentStormStreakFrames++;
        } else {
            rendererDexPresentStormStreakFrames = 0;
        }

        if (rendererDexPresentStormStreakFrames >= RENDERER_DEX_PRESENT_STORM_STREAK_FRAMES &&
            rendererDexPresentSkipCooldownFrames <= 0 &&
            rendererDexPresentSkipFrames <= 0) {
            rendererDexPresentSkipFrames = RENDERER_DEX_PRESENT_SKIP_FRAMES;
            rendererDexPresentSkipCooldownFrames = RENDERER_DEX_PRESENT_SKIP_COOLDOWN_FRAMES;
            rendererDexPresentStormStreakFrames = 0;

            // Do not combine v3.22 present skip with old wait-based recovery.
            rendererPreRedrawCoalesceFrames = 0;
            rendererPreRedrawCoalesceWaitUs = 0;

            __android_log_print(ANDROID_LOG_DEBUG, "gles-renderer",
                                "v3.22 DeX present skip armed swap_us=%lld",
                                (long long) swapUs);
        }

        if (rendererDexPresentSkipFrames > 0 ||
            rendererDexPresentSkipCooldownFrames > 0) {
            // Suppress legacy wait/coalesce during the present-skip recovery
            // window. v3.21 showed waits do not drain this storm.
            rendererPreRedrawCoalesceFrames = 0;
            rendererPreRedrawCoalesceWaitUs = 0;
        }
    }
    /* v3.22-dex-present-skip-end */

}









// Drains the deferred GPU copy queue (filled by present_execute_copy) into the root texture via
// an FBO. Assumes the caller holds state->lock and will flush/fence before unlocking - returns
// the highest drained serial WITHOUT publishing it to completedSerial, since the caller must only
// do that after the fence confirms the GPU actually finished (not just submitted) the draws;
// publishing early would let the client's next write race our still-in-flight read.
// Looks up a registered buffer by id, waiting briefly (bounded) if it hasn't arrived over the
// async registration socket yet instead of busy-spinning the outer loop.
// How many frames a queued copy may wait for its buffer to be registered before it is given up on.
// How long a copy may wait for its buffers to be registered. This used to be a count of calls to
// rendererApplyPendingGpuCopiesLocked(), described as frames - but rendererShouldWait() returns
// "do not wait" whenever anything is queued, so the thread span and burned all thirty of them in
// microseconds rather than over thirty displayed frames. A deadline says what was meant.
#define LORIE_COPY_DEFER_NS 250000000LL

// When the queue is blocked on a buffer that has not been registered yet, and until when. Read by
// rendererShouldWait() so the thread sleeps instead of spinning, and by the wait itself so it wakes
// when the deadline passes even if no registration ever arrives.
static int64_t rendererCopyBlockedUntilNs = 0;
/* Which entry the deadline above belongs to. Both are cleared whenever the queue they refer to goes
 * away - the state is swapped, the queue drains - because a deadline left over from a queue nobody
 * is draining any more is a timed wait with nothing to wait for. */
static uint64_t rendererCopyDeferredSerial = 0;

// Looks the buffer up and attaches it if it has arrived but not been attached yet. It used to sleep
// here - up to 20 x 5 ms - when a buffer had not been registered yet, which stops completedSerial
// from advancing: every present waiting on it is then re-queued one vblank at a time, and a single
// unregistered buffer holds up every copy behind it. The caller defers instead.
/*
 * Unregistered is not the same as gone. The X server unregisters a buffer when the pixmap behind it is
 * destroyed or the root pool is replaced, and a copy queued before that still names it - with the
 * X server still holding a reference to the memory for exactly as long as the copy is outstanding.
 * The import was destroyed at the end of the same loop iteration regardless, so the copy found
 * nothing, waited out the 250 ms registration deadline at the head of the queue with every copy
 * behind it waiting too, and was then given up on and its present scrapped. A removed buffer is
 * still found here, and kept imported until nothing in the queue names it.
 */
static LorieBuffer *rendererFindBuffer(uint64_t id) {
    LorieBuffer *buf;

    pthread_spin_lock(&bufferLock);
    buf = LorieBufferList_findById(&buffers, id);
    if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id))) {
        LorieBuffer_attachToGL(buf);
        LorieBuffer_addToList(buf, &buffers);
    }
    if (!buf)
        buf = LorieBufferList_findById(&removedBuffers, id);
    if (!buf)
        buf = LorieBufferList_findById(&retainedBuffers, id);
    pthread_spin_unlock(&bufferLock);

    return buf;
}

// Whether any copy still waiting in the queue reads or writes this buffer.
static bool rendererBufferNamedByQueue(uint64_t bufferId) {
    uint32_t i, end;

    if (!state)
        return false;

    end = __atomic_load_n(&state->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);
    for (i = state->gpuCopyQueue.readIndex; i != end; i++) {
        const LorieGpuCopyEntry *e = &state->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY];

        if (e->srcBufferId == bufferId || e->dstBufferId == bufferId)
            return true;
    }
    return false;
}

/*
 * Says a copy was not made, which completedSerial cannot: it is a watermark, so publishing a later
 * success would claim this one succeeded too. The X server scraps the present instead of claiming
 * it, and tells the client its frame was skipped.
 */
static void rendererPublishFailedSerial(uint64_t serial) {
    uint32_t slot = __atomic_load_n(&state->gpuCopyQueue.failedCount, __ATOMIC_RELAXED);

    // The list is short, and a serial pushed out of it used to read back as "never failed" - which
    // the X server then took as "copied", because completedSerial had stepped over it. Saying how
    // far the losses reach, before the entry actually goes, keeps that from becoming a false ack.
    if (slot >= LORIE_GPU_COPY_FAILED_SLOTS) {
        uint64_t evicted = state->gpuCopyQueue.failedSerials[slot % LORIE_GPU_COPY_FAILED_SLOTS];

        if (evicted > __atomic_load_n(&state->gpuCopyQueue.failedLostUpTo, __ATOMIC_RELAXED))
            __atomic_store_n(&state->gpuCopyQueue.failedLostUpTo, evicted, __ATOMIC_RELEASE);
    }

    state->gpuCopyQueue.failedSerials[slot % LORIE_GPU_COPY_FAILED_SLOTS] = serial;
    __atomic_store_n(&state->gpuCopyQueue.failedCount, slot + 1, __ATOMIC_RELEASE);
}

/*
 * Hands the slot back to the X server. A release store, because this is what tells the X server it may
 * overwrite the entry, and the entry was read above: a plain increment does not order those reads
 * before the X server's next writes on a weakly ordered CPU, and the X server reads this with acquire.
 */
static inline void rendererAdvanceReadIndex(void) {
    __atomic_store_n(&state->gpuCopyQueue.readIndex, state->gpuCopyQueue.readIndex + 1, __ATOMIC_RELEASE);
    // An X server thread may be asleep on a futex waiting for exactly this (lorieExaFallbackBegin).
    // Not FUTEX_PRIVATE: the word is in memory shared with the X server process.
    if (__atomic_load_n(&state->gpuCopyQueue.readIndexWaiters, __ATOMIC_ACQUIRE))
        syscall(__NR_futex, &state->gpuCopyQueue.readIndex, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

// Takes a queued entry to run. False means the X server cancelled it first, and it must not run.
static bool rendererClaimEntry(uint32_t slot) {
    uint32_t expected = LORIE_JOB_QUEUED;

    return __atomic_compare_exchange_n(&state->gpuCopyQueue.entryState[slot], &expected, LORIE_JOB_CLAIMED,
                                       false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/*
 * Moves past an entry the X server cancelled. Reported as not made rather than left to ride out on
 * completedSerial, so the present is scrapped and the client told its frame was skipped. Still drained
 * like any other entry: nothing of it is left for the GPU to do.
 */
static void rendererSkipCancelledEntry(uint64_t serial, uint64_t *lastSerial) {
    rendererPublishFailedSerial(serial);
    __atomic_fetch_add(&state->presentStats.copySkips, 1, __ATOMIC_RELAXED);
    *lastSerial = serial;
    rendererAdvanceReadIndex();
}

/*
 * Whether a copy into this buffer is still waiting in the queue. Entries between readIndex and
 * writeIndex are published and no longer change, so reading them here is safe.
 *
 * This is what makes a frame ready or not. The drain stops at the first entry it has to wait on and
 * everything behind it waits too, so a frame could be submitted while its own copies sat further
 * down the queue - and once submitted the slot is held, and those copies could no longer be written
 * into it at all.
 */
static bool rendererBufferHasQueuedCopies(uint64_t bufferId) {
    uint32_t i, end = __atomic_load_n(&state->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);

    for (i = state->gpuCopyQueue.readIndex; i != end; i++)
        if (state->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY].dstBufferId == bufferId)
            return true;
    return false;
}

// Which root slot a buffer id names, or -1 if it is not a root slot at all.
static int rendererRootSlotForBufferId(uint64_t id) {
    int i;

    if (!state->rootDoubleBuffered)
        return -1;

    for (i = 0; i < LORIE_ROOT_SLOTS; i++)
        if (state->rootBufferIds[i] == id)
            return i;
    return -1;
}

/*
 * safeSlot is the root slot this frame has claimed and not yet submitted a read of, so writing into
 * it here still lands before anything reads it. -1 when there is no frame in progress, which
 * includes the case where the previous frame deferred its fence: its read of the slot has been
 * submitted, so that slot is no safer than any other held one.
 */
/*
 * Returns the highest serial drained - applied, given up on, or skipped - and says separately whether
 * any GPU work was issued.
 *
 * Those are two answers to two questions, and they were one. The serial becomes completedSerial,
 * which the X server reads as "the GPU has finished with everything up to here": once the fence for
 * this batch has signalled that is true of every entry drained, whether it drew or not. A skipped
 * entry used to leave it behind, so a batch of nothing but skips reported nothing, woke nobody, and
 * left the X server to find out on a later poll. And whether to wait for a fence at all depends only
 * on whether anything was drawn, which a serial cannot say.
 */
static uint64_t rendererApplyPendingGpuCopiesLocked(int safeSlot, bool *gpuWorkIssued) {
    bool fboSetUp = false;
    uint64_t lastSerial = 0;
    uint64_t boundDstId = 0;
    GLint prevViewport[4];

    *gpuWorkIssued = false;
    if (!state || state->gpuCopyQueue.readIndex == state->gpuCopyQueue.writeIndex)
        return 0;

    while (state->gpuCopyQueue.readIndex != __atomic_load_n(&state->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE)) {
        uint32_t slot = state->gpuCopyQueue.readIndex % LORIE_GPU_COPY_QUEUE_CAPACITY;
        LorieGpuCopyEntry entry = state->gpuCopyQueue.entries[slot];
        LorieBuffer *src, *dst;
        int dstSlot = rendererRootSlotForBufferId(entry.dstBufferId);
        bool heldOnScreen;

        // Already cancelled by the X server - its request torn down, or newer drawing written over
        // the area it would have landed in. Read on its own rather than from the struct copy above,
        // because this is the one field the X server writes after publishing the entry.
        if (__atomic_load_n(&state->gpuCopyQueue.entryState[slot], __ATOMIC_ACQUIRE) == LORIE_JOB_CANCELLED) {
            rendererSkipCancelledEntry(entry.serial, &lastSerial);
            continue;
        }

        src = rendererFindBuffer(entry.srcBufferId);
        dst = rendererFindBuffer(entry.dstBufferId);

        /*
         * A root slot the compositor or the GPU still holds is a buffer that is already being
         * displayed, and writing into it is tearing by definition. The X server no longer holds a
         * publish back for a copy it has queued - it cannot, or it stops publishing altogether
         * while a client keeps presenting - so this is where that copy waits instead.
         *
         * The wait is short by construction: the X server publishes a newer slot every frame, and
         * the hold is dropped when the compositor releases this one. The give-up deadline below
         * bounds it if that does not happen.
         */
        heldOnScreen = dstSlot >= 0 && dstSlot != safeSlot &&
                       (__atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE) & (1u << dstSlot));

        if (!src || !dst || heldOnScreen) {
            // Not ready. Leave it queued and look again on the next frame rather than waiting here,
            // which would stall every present behind it as well.
            int64_t nowNs = rendererNowNs();

            if (rendererCopyDeferredSerial != entry.serial) {
                rendererCopyDeferredSerial = entry.serial;
                rendererCopyBlockedUntilNs = nowNs + LORIE_COPY_DEFER_NS;
            }

            if (nowNs < rendererCopyBlockedUntilNs) {
                // Two different waits, counted apart: a buffer that has not been imported yet, and a
                // destination slot the compositor is still showing. The second ends when a release
                // arrives, not when an import does, and lumping them together hid which one a stall
                // was actually made of.
                if (heldOnScreen)
                    __atomic_fetch_add(&state->presentStats.copyWaitHeld, 1, __ATOMIC_RELAXED);
                else
                    __atomic_fetch_add(&state->presentStats.copyDeferrals, 1, __ATOMIC_RELAXED);
                break;
            }
            rendererCopyBlockedUntilNs = 0;
            rendererCopyDeferredSerial = 0;

            log("rendererApplyPendingGpuCopies: giving up on serial %llu - %s\n",
                (unsigned long long) entry.serial,
                heldOnScreen ? "its destination root slot stayed on screen"
                             : src ? "its destination buffer never arrived"
                                   : "its source buffer never arrived");
            __atomic_fetch_add(&state->presentStats.copySkips, 1, __ATOMIC_RELAXED);

            // Say so. Letting this serial ride out on completedSerial told the X server the copy
            // had been made, which told the client its frame was on screen when nothing had been
            // drawn at all.
            rendererPublishFailedSerial(entry.serial);
        }

        /*
         * Claimed only now, at the moment it is about to run - not before the waits above. An entry
         * claimed and then left waiting would be one the X server could no longer cancel, while it
         * also had not happened yet: the X server would draw, let go of the lock, and the copy would
         * land on top afterwards. Unclaimed, it stays cancellable for as long as it is waiting.
         *
         * Losing the race means the X server cancelled it in the meantime, and it is not run.
         */
        if (src && dst && !heldOnScreen && !rendererClaimEntry(slot)) {
            rendererSkipCancelledEntry(entry.serial, &lastSerial);
            continue;
        }

        if (src && dst && !heldOnScreen) {
            const LorieBuffer_Desc *srcDesc = LorieBuffer_description(src);
            const LorieBuffer_Desc *dstDesc = LorieBuffer_description(dst);
            int i;

            if (!fboSetUp) {
                glGetIntegerv(GL_VIEWPORT, prevViewport);
                if (!gpuCopyFbo)
                    glGenFramebuffers(1, &gpuCopyFbo);
                glBindFramebuffer(GL_FRAMEBUFFER, gpuCopyFbo);
                fboSetUp = true;
            }
            // Different entries can target different pixmaps (root, or a Composite-redirected
            // window's own backing pixmap); only rebind the FBO's attachment when it changes.
            if (boundDstId != entry.dstBufferId) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, LorieBuffer_getGLTextureId(dst), 0);
                glViewport(0, 0, dstDesc->width, dstDesc->height);
                boundDstId = entry.dstBufferId;

                // Diagnostic: GLES2 has no glGetTexLevelParameteriv, so ask the AHardwareBuffer
                // itself what it was actually allocated as, instead of trusting our own desc.
                {
                    static uint64_t dstSizeLogCount = 0;
                    if (lorieDebugEnabled && (dstSizeLogCount++ & 15) == 0 && dstDesc->buffer) {
                        AHardwareBuffer_Desc realDstDesc;
                        AHardwareBuffer_describe(dstDesc->buffer, &realDstDesc);
                        loge("gpucopy dst texId=%u real AHB size %ux%u stride=%u vs LorieBuffer desc %dx%d\n",
                             LorieBuffer_getGLTextureId(dst), realDstDesc.width, realDstDesc.height,
                             realDstDesc.stride, dstDesc->width, dstDesc->height);
                    }
                }
            }

            LorieBuffer_bindTexture(src);
            {
                static uint64_t srcSizeLogCount = 0;
                if (lorieDebugEnabled && (srcSizeLogCount++ & 15) == 0 && srcDesc->buffer) {
                    AHardwareBuffer_Desc realSrcDesc;
                    AHardwareBuffer_describe(srcDesc->buffer, &realSrcDesc);
                    loge("gpucopy src texId=%u real AHB size %ux%u stride=%u vs LorieBuffer desc %dx%d (stride=%d)\n",
                         LorieBuffer_getGLTextureId(src), realSrcDesc.width, realSrcDesc.height,
                         realSrcDesc.stride, srcDesc->width, srcDesc->height, srcDesc->stride);
                }
            }
            for (i = 0; i < entry.numRects; i++) {
                LorieGpuCopyRect r = entry.rects[i];
                float x0 = 2.f * (float) (r.x1 + entry.xOff) / (float) dstDesc->width - 1.f;
                float x1 = 2.f * (float) (r.x2 + entry.xOff) / (float) dstDesc->width - 1.f;
                // FBO writes and on-screen draws use opposite y conventions here, unlike x.
                float y0 = 1.f - 2.f * (float) (r.y1 + entry.yOff) / (float) dstDesc->height;
                float y1 = 1.f - 2.f * (float) (r.y2 + entry.yOff) / (float) dstDesc->height;
                // EGLImage-backed textures sample by logical width regardless of row stride;
                // only our own CPU-uploaded LORIEBUFFER_FD texture is stride-wide.
                float srcUvDivisor = srcDesc->type == LORIEBUFFER_FD ? (float) srcDesc->stride : (float) srcDesc->width;
                float u0 = (float) r.x1 / srcUvDivisor;
                float u1 = (float) r.x2 / srcUvDivisor;
                float v0 = (float) r.y1 / (float) srcDesc->height;
                float v1 = (float) r.y2 / (float) srcDesc->height;
                // Only swap channels if src/dst storage formats actually differ.
                uint8_t needsSwizzle = LorieBuffer_isRgba(src) != LorieBuffer_isRgba(dst);
                drawRegion(0, x0, y0, x1, y1, u0, v0, u1, v1, needsSwizzle);
            }
        }

        rendererCopyBlockedUntilNs = 0;
        rendererCopyDeferredSerial = 0;
        lastSerial = entry.serial;
        rendererAdvanceReadIndex();
    }

    if (fboSetUp) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
    *gpuWorkIssued = fboSetUp;
    if (lastSerial)
        lorieTrace(state, LORIE_TRACE_DRAIN, fboSetUp, lastSerial);
    return lastSerial;
}

/*
 * Blocks until GL work already issued has actually finished, and says so honestly.
 *
 * Every EGL return value along this path used to be ignored. eglCreateSyncKHR can fail and hand
 * back EGL_NO_SYNC_KHR, which was then passed straight to the wait, and the wait itself can return
 * EGL_FALSE - in either case nothing had been waited for, and the copy's serial was published
 * anyway, telling the X server it could hand the source pixmap back while the GPU was still reading
 * it. glFinish is the blunt version of the same guarantee and the one thing left that gives it.
 */
static int64_t rendererWaitForFence(EGLSync fence) {
    int64_t waitStartNs = rendererNowNs();

    if (fence == EGL_NO_SYNC_KHR ||
        eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER) != EGL_CONDITION_SATISFIED_KHR) {
        glFinish();
        if (state)
            __atomic_fetch_add(&state->presentStats.fenceFallbacks, 1, __ATOMIC_RELAXED);
    }
    return rendererNsToUs(rendererNowNs() - waitStartNs);
}

/*
 * Submit what has been issued, then wait for it. The two are timed apart because they move for
 * different reasons: charging both to the fence wait made every reading of "how long does the GPU
 * take" include the cost of handing it the work.
 */
static void rendererFinishIssuedWork(int64_t *flushUs, int64_t *waitUs) {
    /* After the commands it is meant to cover, and before the flush that submits it. */
    EGLSync fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
    int64_t flushStartNs = rendererNowNs(), waitStartNs;

    glFlush();
    waitStartNs = rendererNowNs();

    *waitUs = rendererWaitForFence(fence);
    if (fence != EGL_NO_SYNC_KHR)
        eglDestroySyncKHR(egl_display, fence);
    *flushUs = rendererNsToUs(waitStartNs - flushStartNs);
}

// Accounts for the lock as two separate things: getting it, and holding it. They were one number
// measured from before the acquire, so a frame that waited 20 ms on the X server and held the lock
// for 1 ms read the same as the reverse - and it is the reverse that says the renderer is what
// blocks the X server.
static void rendererNoteLock(int64_t waitUs, int64_t heldUs) {
    if (!state)
        return;
    __atomic_fetch_add(&state->presentStats.lockWaitUs, (uint32_t) waitUs, __ATOMIC_RELAXED);
    LORIE_STAT_MAX(&state->presentStats.lockWaitMaxUs, (uint32_t) waitUs);
    __atomic_fetch_add(&state->presentStats.lockHeldUs, (uint32_t) heldUs, __ATOMIC_RELAXED);
}

// Standalone entry point used by the renderer thread's main loop. Used when no redraw is going to
// happen on this tick (rare for GPU copies in practice, since scheduling one also marks damage
// non-empty - see lorieTryScheduleGpuCopy), so it has to take the lock and fence/unlock itself.
static void rendererApplyPendingGpuCopies(void) {
    uint64_t serial;
    if (!state || state->gpuCopyQueue.readIndex == state->gpuCopyQueue.writeIndex)
        return;
    int64_t lockWaitStartNs = rendererNowNs(), lockHeldStartNs;
    int64_t flushUs = 0, waitUs = 0;
    bool gpuWorkIssued;

    if (!lorie_mutex_lock(&state->lock, &state->lockingPid))
        return;   // the queue stays as it is and is drained next time
    lockHeldStartNs = rendererNowNs();
    // No frame in progress, so no slot is safe merely because this renderer claimed it.
    serial = rendererApplyPendingGpuCopiesLocked(-1, &gpuWorkIssued);
    if (serial) {
        // Nothing to wait for if nothing was drawn - a batch of skips still has to be reported.
        if (gpuWorkIssued)
            rendererFinishIssuedWork(&flushUs, &waitUs);
        // Only now that the GPU has actually finished (not just been told to start) is it safe to
        // let present_execute_copy release/idle the source pixmap back to the client.
        __atomic_store_n(&state->gpuCopyQueue.completedSerial, serial, __ATOMIC_RELEASE);
        lorieTrace(state, LORIE_TRACE_FENCE, 0, serial);
        notifyGpuCopyDone();
    }
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
    rendererNoteLock(rendererNsToUs(lockHeldStartNs - lockWaitStartNs),
                     rendererNsToUs(rendererNowNs() - lockHeldStartNs));
    __atomic_fetch_add(&state->presentStats.flushUs, (uint32_t) flushUs, __ATOMIC_RELAXED);
    __atomic_fetch_add(&state->presentStats.fenceWaitUs, (uint32_t) waitUs, __ATOMIC_RELAXED);
    LORIE_STAT_MAX(&state->presentStats.fenceWaitMaxUs, (uint32_t) waitUs);
}

// Frame pacing numbers the X server prints every 5 seconds (see lorieFramecounter). Kept separate
// from the XloriePerf logging above because those lines only exist in this process' logcat, which
// is out of reach without root or adb, while this ends up in the terminal's own log.
static void rendererPublishFrameStats(int64_t frameStartNs, int64_t fenceWaitUs,
                                      bool carriedGpuCopy, int64_t coalesceWaitUs) {
    static int64_t lastFrameStartNs = 0;

    if (!state) {
        lastFrameStartNs = 0;
        return;
    }

    if (!state->rendererDriver[0]) {
        const char *vendor = (const char *) glGetString(GL_VENDOR);
        const char *renderer = (const char *) glGetString(GL_RENDERER);

        snprintf((char *) state->rendererDriver, sizeof(state->rendererDriver), "%s | %s",
                 vendor ? vendor : "?", renderer ? renderer : "?");
    }

    if (lastFrameStartNs) {
        uint32_t deltaUs = (uint32_t) rendererNsToUs(frameStartNs - lastFrameStartNs);

        __atomic_fetch_add(&state->presentStats.frameSumUs, deltaUs, __ATOMIC_RELAXED);
        __atomic_fetch_add(&state->presentStats.frameSamples, 1, __ATOMIC_RELAXED);
        LORIE_STAT_MAX(&state->presentStats.maxFrameUs, deltaUs);
        if (deltaUs >= LORIE_LONG_FRAME_US)
            __atomic_fetch_add(&state->presentStats.longFrames, 1, __ATOMIC_RELAXED);
    }

    lastFrameStartNs = frameStartNs;

    if (fenceWaitUs > 0) {
        __atomic_fetch_add(&state->presentStats.fenceWaitUs, (uint32_t) fenceWaitUs, __ATOMIC_RELAXED);
        LORIE_STAT_MAX(&state->presentStats.fenceWaitMaxUs, (uint32_t) fenceWaitUs);
    }
    if (carriedGpuCopy)
        __atomic_fetch_add(&state->presentStats.gpuCopyFrames, 1, __ATOMIC_RELAXED);
    if (coalesceWaitUs > 0)
        __atomic_fetch_add(&state->presentStats.coalescedFrames, 1, __ATOMIC_RELAXED);

    state->presentStats.displayRefreshMHz = (uint32_t) (rendererDisplayRefreshRateHz * 1000.0f);
}

// Root double buffering handshake, see the rootHandover comment in lorie.h. Claiming tells the X
// server "I am sampling this slot, do not take it back"; it is the only thing that keeps the X
// server from having to wait for our fence, so every path out of a claimed frame must release.
// Which slot this frame took, so releasing it clears the right bit. Renderer thread only.
static int rendererRootSlot = -1;
static uint64_t rendererRootSlotId = 0;   // the buffer that slot held when it was claimed

static uint64_t rendererClaimRootBuffer(void) {
    uint32_t old, claimed;
    int slot;
    uint64_t id;

    if (!state->rootDoubleBuffered)
        return state->rootWindowTextureID;

    /*
     * When the X server replaces the slot pool it writes the new buffer ids and then clears the whole
     * held mask. A claim made just before that clear is wiped by it, and the id read for it may
     * already be the replacement's - a slot sampled with no held bit, which the X server is then free
     * to draw into. So the claim is checked to have survived, and made again if it did not. Only the
     * renderer sets these bits, so a bit still set after the id is read is this claim's.
     */
    do {
        do {
            old = __atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE);
            slot = (int) ((old >> LORIE_ROOT_NEWEST_SHIFT) & LORIE_ROOT_NEWEST_MASK);
            claimed = old | (1u << slot);
        } while (!__atomic_compare_exchange_n(&state->rootHandover, &old, claimed, false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        id = state->rootBufferIds[slot];
    } while (!(__atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE) & (1u << slot)));

    rendererRootSlot = slot;
    rendererRootSlotId = id;
    return id;
}

// The frame's fence is no longer waited for between the drawing and the swap. Waiting there drains
// the pipeline while the lock is held and before the frame has even been submitted, which on a
// driver where every flush is a queue submission (ANGLE on Vulkan) costs milliseconds that have
// nothing to do with how much is being drawn. The fence is created without a flush, eglSwapBuffers
// submits the whole frame as one, and the wait happens right after it - a point where the thread
// would be waiting for vsync anyway and where the GPU has already had the frame.
//
// The root buffer stays claimed until that wait returns, so the X server still cannot get it back
// before the GPU is done reading it. This cannot tear.
static void rendererReleaseRootBuffer(void);

static EGLSync rendererPendingFence = EGL_NO_SYNC_KHR;
static uint64_t rendererPendingGpuCopySerial = 0;

static void rendererRetireFrame(void) {
    EGLSync fence = rendererPendingFence;

    // A serial still to publish with no fence to wait on means eglCreateSyncKHR failed when the
    // frame was submitted. Returning here left that serial unpublished and the root slot claimed,
    // so the X server waited for a copy nobody would ever report and the renderer ran a slot short.
    if (fence == EGL_NO_SYNC_KHR && !rendererPendingGpuCopySerial)
        return;

    rendererPendingFence = EGL_NO_SYNC_KHR;

    if (fence == EGL_NO_SYNC_KHR) {
        // Nothing to wait on, and publishing the serial regardless would say the GPU had finished.
        glFinish();
        if (state)
            __atomic_fetch_add(&state->presentStats.fenceFallbacks, 1, __ATOMIC_RELAXED);
    } else {
        // Zero timeout: this has normally signalled long ago. The flush bit only matters for the
        // rare case where nothing has been submitted since, and the blocking wait is the safety
        // net - taken for an error as well as a timeout, since neither says the work is done.
        if (eglClientWaitSyncKHR(egl_display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0) != EGL_CONDITION_SATISFIED_KHR)
            rendererWaitForFence(fence);

        eglDestroySyncKHR(egl_display, fence);
    }

    if (rendererPendingGpuCopySerial && state) {
        __atomic_store_n(&state->gpuCopyQueue.completedSerial, rendererPendingGpuCopySerial, __ATOMIC_RELEASE);
        lorieTrace(state, LORIE_TRACE_FENCE, 0, rendererPendingGpuCopySerial);
        notifyGpuCopyDone();
    }
    rendererPendingGpuCopySerial = 0;

    rendererReleaseRootBuffer();
}

/*
 * Gives a slot back - but only the slot that was taken. A slot is named by its index, and indexes are
 * reused when the X server replaces the pool: it clears the held mask for the new buffers at that
 * point, so a release arriving later for an old buffer at the same index would clear a bit belonging
 * to a new one, which may be the buffer the compositor is showing right now. The X server would then
 * draw into it. Released by index only while that index still holds the same buffer; otherwise the
 * pool has moved on, the old bit is already gone, and there is nothing to give back.
 */
static void rendererReleaseRootSlot(int slot, uint64_t bufferId) {
    uint32_t old, released;

    if (slot < 0)
        return;

    if (state->rootBufferIds[slot] != bufferId) {
        __atomic_fetch_add(&state->presentStats.rootStaleSlotReleases, 1, __ATOMIC_RELAXED);
        return;
    }

    do {
        old = __atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE);
        released = old & ~(1u << slot);
    } while (!__atomic_compare_exchange_n(&state->rootHandover, &old, released, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

static void rendererReleaseRootBuffer(void) {
    int slot = rendererRootSlot;

    // Deliberately not conditional on rootDoubleBuffered: the X server clears that for the frames
    // where a client has flipped its own pixmap in, and a slot claimed before that still has to be
    // given back or it stays marked held forever and the X server loses it.
    if (slot < 0)
        return;

    rendererRootSlot = -1;
    rendererReleaseRootSlot(slot, rendererRootSlotId);
}

void rendererRedrawLocked(bool* waitingForBuffers) {
    // Captured before the locked section clears it: a frame with no damage at all is one the
    // cursor alone asked for, which rendererShouldWait() refuses to coalesce.
    bool cursorOnlyFrame = state && !state->drawRequested &&
                           (state->cursor.moved || state->cursor.updated);
    int64_t lockHeldUs = 0, lockWaitUs = 0;
    bool swapOk = false, fenceWanted = false;
    float xfactor = 1.f;
    LorieBuffer_Desc *desc = NULL;
    EGLSync fence = EGL_NO_SYNC_KHR;
    bool rootFenceWaitEnabled = rendererRootFenceWaitEnabled;
    bool postSwapTouchEnabled = rendererPostSwapTouchEnabled;
    bool postSwapFenceWaitEnabled = postSwapTouchEnabled && rendererPostSwapFenceWaitEnabled;
    bool swapBackpressureGuardEnabled = rendererSwapBackpressureGuardEnabled;
    // Unconditional: rendererPublishFrameStats() needs it even with the perf log off.
    int64_t frameStartNs = rendererNowNs();
    int64_t rootWaitUs = rootFenceWaitEnabled ? 0 : -1;
    int64_t swapUs = 0;
    int64_t preSwapFlushUs = -1;
    int64_t coalesceWaitUs = rendererLastPreRedrawCoalesceWaitUs;
    int64_t highRefreshWaitUs = rendererLastHighRefreshLimitWaitUs;
    rendererLastPreRedrawCoalesceWaitUs = -1;
    rendererLastHighRefreshLimitWaitUs = -1;
    int64_t postSwapTouchUs = postSwapTouchEnabled ? 0 : -1;
    int64_t postSwapWaitUs = postSwapFenceWaitEnabled ? 0 : -1;
    int64_t frameDeltaUs = 0;
    if (rendererPerfLogEnabled) {
        if (rendererLastPerfFrameStartNs != 0)
            frameDeltaUs = rendererNsToUs(frameStartNs - rendererLastPerfFrameStartNs);

        rendererLastPerfFrameStartNs = frameStartNs;
    }
    // Safety net: a frame that never reached its swap would otherwise leave the root claimed.
    rendererRetireFrame();
    bool deferFence = state->rootDoubleBuffered != 0;
    uint64_t rootId = rendererClaimRootBuffer();
    // The buffer will not be released until this function ends, but main thread can modify buffer list
    pthread_spin_lock(&bufferLock);
    LorieBuffer *buffer = LorieBufferList_findById(&buffers, rootId);
    // Probably X server requested us to draw removed buffer and immediately requested to remove it. Let's display it one last time.
    if (!buffer)
        buffer = LorieBufferList_findById(&removedBuffers, rootId);
    if (!buffer)
        *waitingForBuffers = true;
    pthread_spin_unlock(&bufferLock);
    if (!buffer) {
        log("Buffer %llu not found", (unsigned long long) rootId);
        rendererReleaseRootBuffer();
        // The locked apply further down is now unreachable, so drain the queue here: otherwise a
        // copy queued for a window unrelated to root stalls until root recovers, and it also keeps
        // this thread spinning, since rendererShouldWait()'s gpuCopyPending check runs before it
        // ever looks at *waitingForBuffers (upstream 9050f87).
        rendererApplyPendingGpuCopies();
        return;
    }

    desc = LorieBuffer_description(buffer);

    int alignedExpectedW = expectedW - (expectedW % CVT_H_GRANULARITY);

    if (!expectedW || !expectedH || desc->height != expectedH ||
        (desc->width != alignedExpectedW && desc->width != expectedW)) {
        log("Buffer %llu is not of expected size, expecting %dx%d or %dx%d, got %dx%d",
            (unsigned long long) rootId, alignedExpectedW, expectedH, expectedW, expectedH,
            desc->width, desc->height);
        rendererReleaseRootBuffer();
        // Otherwise rendererShouldWait sees drawRequested or a pending cursor update and busy-spins
        // retrying this same mismatch instead of waiting for the buffer of the requested size.
        // A surface change also raises cursor.updated, which keeps this thread awake on its own,
        // so clearing drawRequested is not enough (upstream e4475ff).
        state->drawRequested = FALSE;
        *waitingForBuffers = true;
        // The locked apply further down is now unreachable, so drain the queue here: otherwise a
        // copy queued for a window unrelated to root stalls until root recovers, and it also keeps
        // this thread spinning, since rendererShouldWait()'s gpuCopyPending check runs before it
        // ever looks at *waitingForBuffers (upstream 9050f87).
        rendererApplyPendingGpuCopies();
        return;
    }

    int surfaceH = ANativeWindow_getHeight(win);
    int surfaceW = ANativeWindow_getWidth(win);

    if (rootZeroCopyUsable(desc)) {
        // The cursor is the GL frame's job in the other path, and this one returns before reaching
        // it. It has to happen here, and before the drain below, because a frame that cannot present
        // the root can still move the pointer - otherwise the pointer only catches up in the gaps
        // between the X server's own frames, which while dragging a window is not often.
        cursorOverlaySourceW = (float) desc->width;
        cursorOverlaySourceH = (float) desc->height;
        if (cursorOverlayUsable() && (state->cursor.moved || state->cursor.updated)) {
            markCursorOverlayDirty(state->cursor.updated);
            state->cursor.moved = state->cursor.updated = FALSE;
        }

        if (!rootZcDrainRetiring()) {
            // The compositor has not finished with the buffer before last. Reusing it now is exactly
            // the tearing this cannot afford, so drop the frame instead - what is on screen stays.
            __atomic_fetch_add(&state->presentStats.zeroCopyStalls, 1, __ATOMIC_RELAXED);
            lorieTrace(state, LORIE_TRACE_HOLD, 2, rendererRootSlot);
            // The slot just claimed may be one already held for the compositor; only give back one
            // that is not.
            {
                int i;
                bool mine;

                // Under the lock: the completion callback runs on a binder thread and rewrites
                // these entries.
                pthread_mutex_lock(&rootOverlayLock);
                mine = rendererRootSlot == rootZcDisplayedSlot && rendererRootSlotId == rootZcDisplayedId;
                for (i = 0; !mine && i < rootZcRetiringCount; i++)
                    mine = rootZcRetiring[i].slot == rendererRootSlot &&
                           rootZcRetiring[i].bufferId == rendererRootSlotId;
                pthread_mutex_unlock(&rootOverlayLock);

                if (!mine)
                    rendererReleaseRootBuffer();
            }
            rendererRootSlot = -1;
            state->drawRequested = FALSE;
            // Ask to come back for it, since clearing drawRequested above is what would otherwise
            // lose it. Gated behind waitForNextFrame, so this is one more attempt at the next vsync
            // rather than the thread coming straight back round millions of times a second.
            rendererSetOutputRetry(true);
            state->waitForNextFrame = true;
            return;
        }
        if (rootZcPresent(desc, surfaceW, surfaceH, frameStartNs))
            return;
    } else if (rootZcDisplayedSlot >= 0 || rootZcRetiringCount > 0) {
        // Dropped out of the path - the filtering preference changed, or the root stopped being one
        // of our slots. Give everything back before drawing through GL again.
        rootZcStopPresenting();
    }

/*
     * clear stale area outside X viewport
     *
     * When viewport changes after PiP/Desktop Mode/DPI refresh, only the
     * X viewport is redrawn. Pixels outside the new viewport can keep old
     * Surface contents, which appears as a PiP-like ghost image.
     */
    {
        // The letterbox only has to be cleared when the geometry it surrounds changes - doing it on
        // every frame is a full surface write that also splits the frame into an extra render pass.
        // Cleared for a few frames after a change because the surface has several buffers.
        static int lastSurfaceW = -1, lastSurfaceH = -1, lastViewport[4] = { -1, -1, -1, -1 };
        static int clearFramesLeft = 0;
        Bool letterboxed = surfaceW > 0 && surfaceH > 0 &&
                (viewportX != 0 || viewportY != 0 || viewportW != surfaceW || viewportH != surfaceH);

        if (surfaceW != lastSurfaceW || surfaceH != lastSurfaceH ||
            viewportX != lastViewport[0] || viewportY != lastViewport[1] ||
            viewportW != lastViewport[2] || viewportH != lastViewport[3]) {
            lastSurfaceW = surfaceW; lastSurfaceH = surfaceH;
            lastViewport[0] = viewportX; lastViewport[1] = viewportY;
            lastViewport[2] = viewportW; lastViewport[3] = viewportH;
            clearFramesLeft = 4;
        }

        if (letterboxed && clearFramesLeft > 0) {
            clearFramesLeft--;
            glViewport(0, 0, surfaceW, surfaceH);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }

    glViewport(viewportX, surfaceH - viewportY - viewportH, viewportW, viewportH);

    // Outside the root lock: this only needs the cursor's own lock, and it used to sit in the middle
    // of the frame holding up the X server for an upload that has nothing to do with the root.
    if (state->cursor.updated && !cursorOverlayUsable()) {
        int64_t uploadStartNs = rendererNowNs();
        // Left marked as updated if the lock cannot be taken, so the next frame uploads it.
        if (lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid)) {
            state->cursor.updated = false;
            if (state->cursor.width && state->cursor.height &&
                state->cursor.width <= LORIE_CURSOR_TEX_SIZE && state->cursor.height <= LORIE_CURSOR_TEX_SIZE) {
                bindTexture(cursor.id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) state->cursor.width, (GLsizei) state->cursor.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, (const void *) state->cursor.bits);
            }
            lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);
        }
        __atomic_fetch_add(&state->presentStats.cursorUploads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&state->presentStats.cursorUploadUs, (uint32_t) rendererNsToUs(rendererNowNs() - uploadStartNs), __ATOMIC_RELAXED);
    }

    // We should signal X server to not use root window while we actively copy it
    int64_t lockStartNs = rendererNowNs(), lockHeldStartNs;
    if (!lorie_mutex_lock(&state->lock, &state->lockingPid)) {
        // Nothing of the root can be read without it. The claim is given back and the frame is
        // tried again at the next vsync; what is on screen stays.
        lorieTrace(state, LORIE_TRACE_HOLD, 3, rendererRootSlot);
        rendererReleaseRootBuffer();
        state->waitForNextFrame = true;
        return;
    }
    lockHeldStartNs = rendererNowNs();
    // Share this draw's flush+fence below instead of a separate round trip per frame.
    bool gpuWorkIssued;
    uint64_t gpuCopySerial = rendererApplyPendingGpuCopiesLocked(rendererRootSlot, &gpuWorkIssued);
    state->drawRequested = FALSE;

    /*
     * Deferring the fence means letting go of the shared lock before the GPU has finished, and that
     * is only safe for what this frame reads and nobody else touches: the root slot it has claimed,
     * which its held bit keeps the X server out of. The copies drained just above are a different
     * matter - they write the X server's current drawing slot or a redirected window's own pixmap,
     * and read the client pixmaps they came from, none of which anything but this lock keeps the X
     * server away from. With the lock released early it could take it for a CPU access to a buffer
     * the GPU was still writing, and since the queue had already moved past those entries nothing
     * else would stop it.
     *
     * So a frame that carried copies waits inside the lock, as the single-buffered path always has.
     * A frame that carried none still defers, which is the case the deferral was made for.
     */
    if (gpuWorkIssued)
        deferFence = false;

    LorieBuffer_bindTexture(buffer);
    if (desc->type == LORIEBUFFER_FD)
        xfactor = (float) desc->width/(float) desc->stride;
    draw(0, -1.f, -1.f, 1.f, 1.f, xfactor, LorieBuffer_isRgba(buffer));
    // With a double buffered root nothing here has to be waited for inside this frame, so no fence
    // is created yet and no flush is issued: eglSwapBuffers below becomes the single submission
    // point of the frame, and the fence made just before it is retired at the start of the next one.
    // Whether this frame has to wait at all, decided once. The wait below keys off this and not off
    // whether the fence exists, because those are different questions: no fence because none was
    // wanted means there is nothing to wait for, while no fence because creating one failed means
    // the wait still has to happen by other means.
    fenceWanted = !deferFence && (rootFenceWaitEnabled || gpuWorkIssued);
    if (fenceWanted) {
        fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
        glFlush();
    }

    state->cursor.moved = FALSE;
    // The overlay thread places the cursor against the root's size and the viewport, and cannot read
    // the root buffer itself, so hand it this frame's numbers.
    int rootW = LorieBuffer_getWidth(buffer), rootH = LorieBuffer_getHeight(buffer);
    cursorOverlaySourceW = (float) rootW;
    cursorOverlaySourceH = (float) rootH;
    if (cursorOverlayUsable())
        markCursorOverlayDirty(state->cursor.updated);
    else
        drawCursor(cursorOverlaySourceW, cursorOverlaySourceH);
    state->cursor.updated = FALSE;
    if (!deferFence)
        glFlush();

    /* Wait until root window drawing is finished before giving control back to X server.
     * A GPU copy applied above is only complete once this fence signals, so a frame that carries
     * one waits even with the root fence wait turned off; otherwise the X server could hand the
     * source pixmap back to its client while the GPU still reads it. */
    if (deferFence) {
        // Created here so it covers everything drawn above; eglSwapBuffers below submits it, and
        // rendererRetireFrame() right after the swap waits for it and hands the buffer back.
        rendererPendingFence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
        rendererPendingGpuCopySerial = gpuCopySerial;
        rootWaitUs = 0;
    } else {
        /*
         * A fence that could not be created is not a fence that has signalled. The wait used to be
         * skipped whenever fence was EGL_NO_SYNC_KHR, and the serial below then told the X server
         * the copy was made when nothing had been waited for at all.
         *
         * Keyed off fenceWanted, not off the fence: the previous fix made this unconditional, which
         * turned every frame that deliberately has no fence - root fence wait off and no copy
         * carried, the fast path - into a glFinish and a counted fallback. rendererWaitForFence()
         * is for a wait that was meant to happen; given no fence it falls back to glFinish.
         */
        if (fenceWanted) {
            rootWaitUs = rendererWaitForFence(fence);
            if (fence != EGL_NO_SYNC_KHR) {
                eglDestroySyncKHR(egl_display, fence);
                fence = EGL_NO_SYNC_KHR;
            }
        }
        // Sampling of the root buffer is complete, so hand it back.
        rendererReleaseRootBuffer();
        if (gpuCopySerial) {
            __atomic_store_n(&state->gpuCopyQueue.completedSerial, gpuCopySerial, __ATOMIC_RELEASE);
        lorieTrace(state, LORIE_TRACE_FENCE, 0, gpuCopySerial);
            notifyGpuCopyDone();
        }
    }
    state->waitForNextFrame = true;
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
    lockHeldUs = rendererNsToUs(rendererNowNs() - lockHeldStartNs);
    lockWaitUs = rendererNsToUs(lockHeldStartNs - lockStartNs);
// Gaming fast path: submit GL commands before swap without creating or waiting on fences.
    // This keeps the no-fence fast path but avoids moving all submit work into swap.
    if (!rootFenceWaitEnabled && !deferFence) {
        if (rendererPerfLogEnabled) {
            int64_t preSwapFlushStartNs = rendererNowNs();
            glFlush();
            preSwapFlushUs = rendererNsToUs(rendererNowNs() - preSwapFlushStartNs);
        } else {
            glFlush();
        }
    }

    if (rendererPerfLogEnabled) {
        int64_t swapStartNs = rendererNowNs();

        swapOk = eglSwapBuffers(egl_display, sfc) == EGL_TRUE;
        swapUs = rendererNsToUs(rendererNowNs() - swapStartNs);
    } else
        swapOk = eglSwapBuffers(egl_display, sfc) == EGL_TRUE;

    if (!swapOk)
        printEglError("Failed to swap buffers", __LINE__);

    // The frame is submitted now, so this is where its fence is waited for and the root buffer is
    // handed back - leaving the X server the whole remainder of the frame interval to take it.
    if (deferFence) {
        int64_t retireStartNs = rendererNowNs();
        rendererRetireFrame();
        rootWaitUs = rendererNsToUs(rendererNowNs() - retireStartNs);
    }

    int64_t guardTotalUs = rendererNsToUs(rendererNowNs() - frameStartNs);
    rendererUpdateSwapBackpressureGuard(swapBackpressureGuardEnabled, swapUs, guardTotalUs);

    // Perform a little drawing operation to make sure the next buffer is ready on the next invocation of drawing.
    // In gaming fast mode this is disabled, so eglSwapBuffers() is the only submit point.
    if (postSwapTouchEnabled) {
        int64_t postSwapTouchStartNs = rendererNowNs();

        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 1, 1);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glFlush();

        postSwapTouchUs = rendererNsToUs(rendererNowNs() - postSwapTouchStartNs);

        if (postSwapFenceWaitEnabled) {
            fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);

            if (rendererPerfLogEnabled) {
                int64_t postSwapStartNs = rendererNowNs();
                eglClientWaitSyncKHR(egl_display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER);
                postSwapWaitUs = rendererNsToUs(rendererNowNs() - postSwapStartNs);
            } else {
                eglClientWaitSyncKHR(egl_display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, EGL_FOREVER);
            }

            eglDestroySyncKHR(egl_display, fence);
            fence = EGL_NO_SYNC_KHR;
        }
    }

    // A frame that failed to swap put nothing on screen, so it is not one.
    lorieTrace(state, LORIE_TRACE_GLSWAP, swapOk, rootId);
    if (swapOk) {
        __atomic_fetch_add(&state->presentStats.glOutputSubmits, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&state->renderedFrames, 1, __ATOMIC_RELAXED);
    } else
        __atomic_fetch_add(&state->presentStats.glOutputSubmitFailures, 1, __ATOMIC_RELAXED);
    rendererPublishFrameStats(frameStartNs, rootWaitUs, gpuCopySerial != 0, coalesceWaitUs);
    rendererNoteLock(lockWaitUs, lockHeldUs);
    if (cursorOnlyFrame)
        __atomic_fetch_add(&state->presentStats.cursorOnlyFrames, 1, __ATOMIC_RELAXED);

    if (rendererPerfLogEnabled) {
        int64_t totalUs = rendererNsToUs(rendererNowNs() - frameStartNs);
        int64_t renderTotalUs = totalUs;

        rendererPerfFrameNo++;

        if ((rendererPerfFrameNo % 60) == 0 || totalUs > 20000 || swapUs > 12000 || coalesceWaitUs > 0 || highRefreshWaitUs > 0 || rendererPreRedrawCoalesceFrames > 0 || rendererHighRefreshLimitFrames > 0) {
            log("XloriePerf: frame=%llu mode=%s root_fence_wait=%d post_swap_touch=%d "
                "post_swap_fence_wait=%d frame_delta_us=%lld root_wait_us=%lld "
                "swap_us=%lld pre_swap_flush_us=%lld applied_coalesce_wait_us=%lld pending_coalesce_wait_us=%lld pressure_score=%d coalesce_active=%d coalesce_frames=%d coalesced_count=%llu display_refresh_hz=%.1f refresh_budget_us=%lld hr_enabled=%d hr_score=%d hr_good_frames=%d hr_wait_us=%lld hr_active=%d hr_frames=%d hr_count=%llu last_swap_us=%lld post_swap_touch_us=%lld post_swap_wait_us=%lld render_total_us=%lld total_us=%lld",
                (unsigned long long) rendererPerfFrameNo,
                rendererGetPresentModeName(),
                rootFenceWaitEnabled ? 1 : 0,
                postSwapTouchEnabled ? 1 : 0,
                postSwapFenceWaitEnabled ? 1 : 0,
                (long long) frameDeltaUs,
                (long long) rootWaitUs,
                (long long) swapUs,
                (long long) preSwapFlushUs,
                (long long) coalesceWaitUs,
                (long long) rendererPreRedrawCoalesceWaitUs,
                rendererSwapPressureScore,
                rendererPreRedrawCoalesceFrames > 0 ? 1 : 0,
                rendererPreRedrawCoalesceFrames,
                (unsigned long long) rendererPreRedrawCoalescedCount,
                rendererDisplayRefreshRateHz,
                (long long) rendererRefreshBudgetUs,
                rendererHighRefreshEnabled ? 1 : 0,
                rendererHighRefreshPlateauScore,
                rendererHighRefreshGoodFrames,
                (long long) highRefreshWaitUs,
                rendererHighRefreshLimitFrames > 0 ? 1 : 0,
                rendererHighRefreshLimitFrames,
                (unsigned long long) rendererHighRefreshLimitedCount,
                (long long) rendererBackpressureLastSwapUs,
                (long long) postSwapTouchUs,
                (long long) postSwapWaitUs,
                (long long) renderTotalUs,
                (long long) totalUs);
        }
    }
}

static inline __always_inline bool rendererShouldWait(bool *waitingForBuffers) {
    static uint64_t lastRequestedBufferId = 0;
    bool buffersChanged, gpuCopyPending;
    pthread_spin_lock(&bufferLock);
    buffersChanged = !xorg_list_is_empty(&addedBuffers) || !xorg_list_is_empty(&removedBuffers);
    pthread_spin_unlock(&bufferLock);
    gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex;
    // A queue held up by an unregistered buffer is not runnable work. Entries are drained in order,
    // so nothing behind it can run either, and answering "do not wait" for it is what turned the
    // wait for a registration into a spin.
    if (gpuCopyPending && rendererCopyBlockedUntilNs && rendererNowNs() < rendererCopyBlockedUntilNs)
        gpuCopyPending = false;
    else if (!gpuCopyPending) {
        // Nothing queued - the queue drained, or there is no state at all. A deadline surviving
        // that is what the wait loop below turns into pthread_cond_timedwait with a deadline
        // already in the past, woken instantly, on a thread with nothing to do: a full core spent
        // on a copy that no longer exists.
        rendererCopyBlockedUntilNs = 0;
        rendererCopyDeferredSerial = 0;
    }
    if (stateChanged || windowChanged || buffersChanged || gpuCopyPending)
        // If there are pending changes we should process them immediately.
        return false;

    if (expectedSizeChanged) {
        // The size we compare the root buffer against just changed, so a buffer we rejected as
        // wrong-sized may fit now. Without this the flag could only be cleared by a new buffer, a
        // new root texture id or a reconnect - none of which happen when the activity simply comes
        // back to the foreground with unchanged geometry, leaving the screen black until the app
        // was killed.
        expectedSizeChanged = false;
        *waitingForBuffers = false;
    }

    if (state) {
        if (lastRequestedBufferId != state->rootWindowTextureID)
            *waitingForBuffers = false;
        lastRequestedBufferId = state->rootWindowTextureID;
    }

    if (!state || !state->surfaceAvailable || state->waitForNextFrame || *waitingForBuffers)
        // Even in the case if there are pending changes, we can not draw it without rendering surface
        return true;

    if (rootZcRetryPending)
        // Content the X server published and we could not put on screen. After the vsync gate
        // above, so this is one attempt per frame.
        return false;

    if (state->cursor.moved || state->cursor.updated)
        // Cursor updates should stay responsive. Do not coalesce them.
        return false;

    if (state->drawRequested) {
        int64_t coalesceWaitUs = -1;
        int64_t highRefreshWaitUs = -1;
        int64_t waitUs = -1;

        if (rendererPreRedrawCoalesceFrames > 0 && rendererPreRedrawCoalesceWaitUs > 0)
            coalesceWaitUs = rendererPreRedrawCoalesceWaitUs;

        if (rendererHighRefreshLimitFrames > 0 && rendererHighRefreshLimitWaitUs > 0)
            highRefreshWaitUs = rendererHighRefreshLimitWaitUs;

        if (coalesceWaitUs > waitUs)
            waitUs = coalesceWaitUs;

        if (highRefreshWaitUs > waitUs)
            waitUs = highRefreshWaitUs;

        rendererLastPreRedrawCoalesceWaitUs = coalesceWaitUs;
        rendererLastHighRefreshLimitWaitUs = highRefreshWaitUs;

        if (waitUs > 0) {
            struct timespec deadline;

            clock_gettime(CLOCK_REALTIME, &deadline);
            rendererTimespecAddUs(&deadline, waitUs);

            if (coalesceWaitUs > 0) {
                rendererPreRedrawCoalescedCount++;
                rendererPreRedrawCoalesceFrames--;

                if (rendererPreRedrawCoalesceFrames <= 0)
                    rendererPreRedrawCoalesceWaitUs = 0;
            }

            if (highRefreshWaitUs > 0) {
                rendererHighRefreshLimitedCount++;
                rendererHighRefreshLimitFrames--;

                if (rendererHighRefreshLimitFrames <= 0)
                    rendererHighRefreshLimitWaitUs = 0;
            }

            pthread_cond_timedwait(stateCond, &stateLock, &deadline);
        } else {
            rendererLastPreRedrawCoalesceWaitUs = -1;
            rendererLastHighRefreshLimitWaitUs = -1;
        }

        return false;
    }

    // Probably spurious wake, no changes we can work with.
    return true;
}

__noreturn static void* rendererThread(void) {
    LorieBuffer* buf;
    bool waitingForBuffers = false;
    while (true) {
        while (rendererShouldWait(&waitingForBuffers)) {
            int64_t blockedUntilNs = rendererCopyBlockedUntilNs;
            int64_t remainingNs = blockedUntilNs ? blockedUntilNs - rendererNowNs() : 0;

            if (remainingNs > 0) {
                // A registration would signal us, but one may never come, so this wait has to end
                // by itself for the copy to be given up on. CLOCK_REALTIME because that is what an
                // untouched condvar measures its absolute timeout against.
                struct timespec ts;

                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += (time_t) ((remainingNs + ts.tv_nsec) / 1000000000LL);
                ts.tv_nsec = (long) ((remainingNs + ts.tv_nsec) % 1000000000LL);
                pthread_cond_timedwait(stateCond, &stateLock, &ts);
            } else
                // Either nothing is being waited out, or its deadline has already passed - in which
                // case rendererShouldWait() has cleared it and there is nothing to time.
                pthread_cond_wait(stateCond, &stateLock);
        }

        if (stateChanged) {
            struct lorie_shared_server_state* oldState = NULL;
            // Anything still in flight belongs to the state we are leaving.
            rendererRetireFrame();
            if (state && pendingState != state)
                oldState = state;

            state = pendingState;
            pendingState = NULL;
            stateChanged = false;
            waitingForBuffers = false;
            // Whatever was waiting to go out belonged to the state being left.
            rendererSetOutputRetry(false);
            // The queue that deadline referred to has gone with the old state.
            rendererCopyBlockedUntilNs = 0;
            rendererCopyDeferredSerial = 0;

            if (state)
                state->surfaceAvailable = win != defaultWin;
            else if (win != defaultWin) {
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(egl_display, sfc);
            }

            if (oldState)
                munmap(oldState, sizeof(*oldState));
        }

        if (windowChanged) { rendererRefreshContext(); waitingForBuffers = false; } if (presentModeChanged) { presentModeChanged = false; rendererApplyPresentMode(); } // Attach all pending buffers to GL.
        pthread_spin_lock(&bufferLock);
        while((buf = LorieBufferList_first(&addedBuffers))) {
            LorieBuffer_attachToGL(buf);
            LorieBuffer_addToList(buf, &buffers);
            waitingForBuffers = false;
        }
        pthread_spin_unlock(&bufferLock);

        pthread_cond_signal(&stateChangeFinishCond);
        pthread_mutex_unlock(&stateLock);

        // Prefer a full redraw over the standalone apply below so a pending GPU copy shares one
        // lock+fence with the root/cursor draw, instead of two GPU round trips per frame.
        bool gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex;
        // A pointer move changes where the cursor sits, not the picture under it. With the overlay
        // that is a transaction on another thread, so there is no reason to build a GL frame for it.
        // Unlike upstream there is no zoom or panning here, so a move can never need the crop
        // recomputed and this needs no further check.
        // A retry is output that still has to go out, so it is not a cursor-only frame however the
        // cursor happens to have moved in the meantime.
        bool cursorOnly = state && !state->drawRequested && !gpuCopyPending && !rootZcRetryPending &&
            (state->cursor.moved || state->cursor.updated);

        if (cursorOnly && state->surfaceAvailable && cursorOverlaySourceW > 0.f && cursorOverlayUsable()) {
            markCursorOverlayDirty(state->cursor.updated);
            state->cursor.moved = state->cursor.updated = FALSE;
            __atomic_fetch_add(&state->presentStats.cursorOverlayMoves, 1, __ATOMIC_RELAXED);
        } else if (state && state->surfaceAvailable && !state->waitForNextFrame &&
            (state->drawRequested || rootZcRetryPending || state->cursor.moved ||
             state->cursor.updated || gpuCopyPending)) {
            // rootZcRetryPending has to be here as well as in rendererShouldWait(): the predicate
            // saying there is work while this said there is none is a loop that never submits.
            rendererRedrawLocked(&waitingForBuffers);
        } else if (gpuCopyPending) {
            rendererApplyPendingGpuCopies();
        }

        pthread_spin_lock(&bufferLock);
        // Release the buffers that were attached to GL and that nothing queued still needs; the
        // rest stay imported until the copies naming them have been drained (see rendererFindBuffer).
        while ((buf = LorieBufferList_first(&removedBuffers))) {
            if (rendererBufferNamedByQueue(LorieBuffer_description(buf)->id))
                LorieBuffer_addToList(buf, &retainedBuffers);
            else
                LorieBuffer_release(buf);
        }
        // And the ones kept from before, whose copies may have been drained this time round.
        {
            struct xorg_list stillNeeded;

            xorg_list_init(&stillNeeded);
            while ((buf = LorieBufferList_first(&retainedBuffers))) {
                if (rendererBufferNamedByQueue(LorieBuffer_description(buf)->id))
                    LorieBuffer_addToList(buf, &stillNeeded);
                else
                    LorieBuffer_release(buf);
            }
            while ((buf = LorieBufferList_first(&stillNeeded)))
                LorieBuffer_addToList(buf, &retainedBuffers);
        }
        pthread_spin_unlock(&bufferLock);
        pthread_mutex_lock(&stateLock);
    }
}

static GLuint loadShader(GLenum shaderType, const char* pSource) {
    GLint compiled = 0, infoLen = 0;
    GLuint shader = glCreateShader(shaderType);
    if (!shader)
        return 0;

    glShaderSource(shader, 1, &pSource, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled)
        return shader;

    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
    if (infoLen) {
        char buf[infoLen];
        glGetShaderInfoLog(shader, infoLen, NULL, buf);
        log("renderer: Could not compile shader %d:\n%s\n", shaderType, buf);
    }
    glDeleteShader(shader);

    return 0;
}

static GLuint createProgram(const char* p_vertex_source, const char* p_fragment_source) {
    GLuint program, vertexShader, pixelShader;
    GLint linkStatus = GL_FALSE, bufLength = 0;
    vertexShader = loadShader(GL_VERTEX_SHADER, p_vertex_source);
    pixelShader = loadShader(GL_FRAGMENT_SHADER, p_fragment_source);
    if (!pixelShader || !vertexShader) {
        return 0;
    }

    program = glCreateProgram();
    if (!program)
        return 0;

    glAttachShader(program, vertexShader);
    glAttachShader(program, pixelShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus == GL_TRUE)
        return program;

    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
    if (bufLength) {
        char buf[bufLength];
        glGetProgramInfoLog(program, bufLength, NULL, buf);
        log("renderer: Could not link program:\n%s\n", buf);
    }
    glDeleteProgram(program);

    return 0;
}

// Like draw(), with explicit texture coordinates: the GPU-offloaded Present copies sample a
// sub-rectangle of their source (from upstream 4d5af156, which this branch does not carry).
static void drawRegion(GLuint id, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint8_t flip) {
    float coords[16] = {
        x0, -y0, u0, v0,
        x1, -y0, u1, v0,
        x0, -y1, u0, v1,
        x1, -y1, u1, v1,
    };

    GLuint p = flip ? gv_pos_bgra : gv_pos, c = flip ? gv_coords_bgra : gv_coords;

    glActiveTexture(GL_TEXTURE0);
    glUseProgram(flip ? g_texture_program_bgra : g_texture_program);
    if (id)
        glBindTexture(GL_TEXTURE_2D, id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
    glVertexAttribPointer(p, 2, GL_FLOAT, GL_FALSE, 16, coords);
    glVertexAttribPointer(c, 2, GL_FLOAT, GL_FALSE, 16, &coords[2]);
    glEnableVertexAttribArray(p);
    glEnableVertexAttribArray(c);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); checkGlError();
}

static void draw(GLuint id, float x0, float y0, float x1, float y1, float xfactor, uint8_t flip) {
    float coords[16] = {
        x0, -y0, 0.f, 0.f,
        x1, -y0, xfactor, 0.f,
        x0, -y1, 0.f, 1.f,
        x1, -y1, xfactor, 1.f,
    };

    GLuint p = flip ? gv_pos_bgra : gv_pos, c = flip ? gv_coords_bgra : gv_coords;

    glActiveTexture(GL_TEXTURE0);
    glUseProgram(flip ? g_texture_program_bgra : g_texture_program);
    if (id)
        glBindTexture(GL_TEXTURE_2D, id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
    glVertexAttribPointer(p, 2, GL_FLOAT, GL_FALSE, 16, coords);
    glVertexAttribPointer(c, 2, GL_FLOAT, GL_FALSE, 16, &coords[2]);
    glEnableVertexAttribArray(p);
    glEnableVertexAttribArray(c);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); checkGlError();
}

__unused static void drawCursor(float displayWidth, float displayHeight) {
    float x, y, w, h;

    if (!state->cursor.width || !state->cursor.height)
        return;

    x = 2.f * ((float) state->cursor.x - (float) state->cursor.xhot) / displayWidth - 1.f;
    y = 2.f * ((float) state->cursor.y - (float) state->cursor.yhot) / displayHeight - 1.f;
    w = 2.f * (float) state->cursor.width / displayWidth;
    h = 2.f * (float) state->cursor.height / displayHeight;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // The cursor occupies the top left corner of a fixed size texture, so sample just that part.
    drawRegion(cursor.id, x, y, x + w, y + h, 0.f, 0.f,
               (float) state->cursor.width / LORIE_CURSOR_TEX_SIZE,
               (float) state->cursor.height / LORIE_CURSOR_TEX_SIZE, false);
    glDisable(GL_BLEND);
}


/* --- Cursor SurfaceControl overlay --- */

static void cursorOverlayFrameCallback(long t, void *data);

static bool cursorOverlayUsable(void) {
    return cursorOverlayFeatureAvailable && cursorSurfaceControl != NULL;
}

static void computeCursorOverlayScale(float *scaleX, float *scaleY) {
    *scaleX = cursorOverlaySourceW > 0.f ? (float) viewportW / cursorOverlaySourceW : 1.f;
    *scaleY = cursorOverlaySourceH > 0.f ? (float) viewportH / cursorOverlaySourceH : 1.f;
}

// Destination rect in the parent window's pixel space, top left origin - unlike drawCursor's NDC.
static bool computeCursorOverlayRect(int32_t *outX, int32_t *outY, uint32_t *outW, uint32_t *outH) {
    float scaleX, scaleY;

    if (!state || !state->cursor.width || !state->cursor.height ||
        cursorOverlaySourceW <= 0.f || cursorOverlaySourceH <= 0.f)
        return false;

    computeCursorOverlayScale(&scaleX, &scaleY);
    long px = lroundf(((float) state->cursor.x - (float) state->cursor.xhot) * scaleX);
    long py = lroundf(((float) state->cursor.y - (float) state->cursor.yhot) * scaleY);
    *outX = viewportX + (int32_t) px;
    *outY = viewportY + (int32_t) py;
    float rectW = fmaxf(1.f, roundf((float) state->cursor.width * scaleX));
    float rectH = fmaxf(1.f, roundf((float) state->cursor.height * scaleY));
    *outW = (uint32_t) rectW;
    *outH = (uint32_t) rectH;
    return true;
}

// Pulls the overlay thread out of ALooper_pollOnce if it went idle with nothing scheduled. Safe from
// any thread, and a no-op when a callback is already pending.
static void wakeCursorOverlayIfIdle(void) {
    bool idle;

    pthread_mutex_lock(&cursorOverlayLock);
    idle = !cursorOverlayCallbackArmed;
    pthread_mutex_unlock(&cursorOverlayLock);

    if (idle && cursorOverlayLooper)
        ALooper_wake(cursorOverlayLooper);
}

// Overlay thread only: AChoreographer_postFrameCallback has to run on the thread that owns it.
static void armCursorOverlayCallbackIfNeeded(void) {
    bool needsArming;

    pthread_mutex_lock(&cursorOverlayLock);
    needsArming = !cursorOverlayCallbackArmed && (cursorOverlayGeometryDirty || cursorOverlayBufferDirty);
    if (needsArming)
        cursorOverlayCallbackArmed = true;
    pthread_mutex_unlock(&cursorOverlayLock);

    if (needsArming && cursorOverlayChoreographer)
        AChoreographer_postFrameCallback(cursorOverlayChoreographer,
                                         (AChoreographer_frameCallback) cursorOverlayFrameCallback, NULL);
}

// Overlay thread, once per vsync.
//
// The whole body runs under cursorOverlayLock. Copying the ASurfaceControl out and using it outside
// the lock, which is the obvious way to write this, lets teardownCursorOverlay() release it from the
// renderer thread in between. Applying a transaction is an asynchronous one-way call, so holding the
// lock across it costs the renderer nothing worth measuring.
static void applyCursorOverlayIfDirty(void) {
    bool geometryDirty, bufferDirty, visible;
    AHardwareBuffer *buf = NULL;
    ASurfaceTransaction *t;
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;

    pthread_mutex_lock(&cursorOverlayLock);

    geometryDirty = cursorOverlayGeometryDirty;
    bufferDirty = cursorOverlayBufferDirty;
    cursorOverlayGeometryDirty = cursorOverlayBufferDirty = false;
    if (bufferDirty) {
        buf = cursorOverlayPendingBuffer;
        cursorOverlayPendingBuffer = NULL;
    }

    if (cursorSurfaceControl && (geometryDirty || bufferDirty) && state) {
        visible = computeCursorOverlayRect(&x, &y, &w, &h);

        ARect src = { 0, 0, (int32_t) w, (int32_t) h };
        ARect dst = { x, y, x + (int32_t) w, y + (int32_t) h };

        t = scApi.txCreate();
        scApi.txSetVisibility(t, cursorSurfaceControl, visible ? ASURFACE_TRANSACTION_VISIBILITY_SHOW
                                                              : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        scApi.txSetZOrder(t, cursorSurfaceControl, 1); // above the window's own buffer
        if (visible) {
            if (buf)
                scApi.txSetBuffer(t, cursorSurfaceControl, buf, -1);
            // Rendered at exactly w x h already, so the compositor never scales or filters it.
            scApi.txSetGeometry(t, cursorSurfaceControl, &src, &dst, 0);
        }
        scApi.txApply(t);
        scApi.txDelete(t);
    }

    pthread_mutex_unlock(&cursorOverlayLock);

    if (buf)
        AHardwareBuffer_release(buf);
}

static void cursorOverlayFrameCallback(__unused long t, __unused void *data) {
    applyCursorOverlayIfDirty();

    pthread_mutex_lock(&cursorOverlayLock);
    cursorOverlayCallbackArmed = false; // this posting has fired
    pthread_mutex_unlock(&cursorOverlayLock);

    armCursorOverlayCallbackIfNeeded();
}

static void *cursorOverlayThreadMain(__unused void *cookie) {
    pthread_setname_np(pthread_self(), "LorieCursorOvl");
    cursorOverlayLooper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    cursorOverlayChoreographer = AChoreographer_getInstance();
    armCursorOverlayCallbackIfNeeded();
    for (;;) {
        ALooper_pollOnce(-1, NULL, NULL, NULL);
        armCursorOverlayCallbackIfNeeded(); // in case an ALooper_wake is what got us here
    }
}

// Hands the overlay thread a fresh reference to the render target's buffer. Used both after
// re-rendering it and when a brand new ASurfaceControl needs one resent.
static void resendCursorOverlayBuffer(void) {
    LorieBuffer *target = cursorOverlayRenderTarget[cursorOverlayTargetIndex];
    AHardwareBuffer *ahb;

    if (!target)
        return;

    ahb = LorieBuffer_description(target)->buffer;
    if (!ahb)
        return;
    AHardwareBuffer_acquire(ahb);

    pthread_mutex_lock(&cursorOverlayLock);
    if (cursorOverlayPendingBuffer)
        AHardwareBuffer_release(cursorOverlayPendingBuffer);
    cursorOverlayPendingBuffer = ahb;
    cursorOverlayBufferDirty = true;
    pthread_mutex_unlock(&cursorOverlayLock);
    wakeCursorOverlayIfIdle();
}

// Renderer thread, EGL context current. Draws the cursor scaled to its destination size into an
// AHardwareBuffer, so the compositor gets a buffer already the right size and never filters it.
static void renderCursorOverlayBuffer(uint32_t destW, uint32_t destH) {
    unsigned slot = cursorOverlayTargetIndex ^ 1u; // never the one the compositor is holding
    GLint prevViewport[4];
    EGLSync fence;

    if (!state->cursor.width || !state->cursor.height ||
        state->cursor.width > LORIE_CURSOR_TEX_SIZE || state->cursor.height > LORIE_CURSOR_TEX_SIZE)
        return;

    // The overlay keeps its previous image if the lock cannot be taken.
    if (!lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid))
        return;
    bindTexture(cursor.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) state->cursor.width, (GLsizei) state->cursor.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, (const void *) state->cursor.bits);
    lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);

    if (!cursorOverlayRenderTarget[slot] ||
        cursorOverlayTargetW[slot] != destW || cursorOverlayTargetH[slot] != destH) {
        if (cursorOverlayRenderTarget[slot])
            LorieBuffer_release(cursorOverlayRenderTarget[slot]);
        cursorOverlayRenderTarget[slot] = LorieBuffer_allocate((int32_t) destW, (int32_t) destH,
                                                              AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                                              LORIEBUFFER_AHARDWAREBUFFER);
        if (cursorOverlayRenderTarget[slot])
            LorieBuffer_attachToGL(cursorOverlayRenderTarget[slot]);
        cursorOverlayTargetW[slot] = cursorOverlayRenderTarget[slot] ? destW : 0;
        cursorOverlayTargetH[slot] = cursorOverlayRenderTarget[slot] ? destH : 0;
    }
    if (!cursorOverlayRenderTarget[slot])
        return;

    glGetIntegerv(GL_VIEWPORT, prevViewport);
    if (!cursorOverlayFbo)
        glGenFramebuffers(1, &cursorOverlayFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, cursorOverlayFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           LorieBuffer_getGLTextureId(cursorOverlayRenderTarget[slot]), 0);
    glViewport(0, 0, (GLsizei) destW, (GLsizei) destH);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // y is inverted because this draws into a texture rather than onto the surface. The cursor sits
    // in the top left corner of a fixed size texture, so sample only that part of it.
    drawRegion(cursor.id, -1.f, 1.f, 1.f, -1.f, 0.f, 0.f,
               (float) state->cursor.width / LORIE_CURSOR_TEX_SIZE,
               (float) state->cursor.height / LORIE_CURSOR_TEX_SIZE, false);

    // The overlay thread hands this buffer straight to the compositor, so it has to be finished.
    fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
    glFlush();
    eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
    eglDestroySyncKHR(egl_display, fence);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    cursorOverlayTargetIndex = slot;
    cursorOverlayRawW = destW;
    cursorOverlayRawH = destH;
    resendCursorOverlayBuffer();
}

static void markCursorOverlayDirty(bool bufferMightHaveChanged) {
    float scaleX, scaleY;
    uint32_t destW, destH;

    pthread_mutex_lock(&cursorOverlayLock);
    cursorOverlayGeometryDirty = true;
    pthread_mutex_unlock(&cursorOverlayLock);
    wakeCursorOverlayIfIdle();

    if (!state->cursor.width || !state->cursor.height)
        return;

    computeCursorOverlayScale(&scaleX, &scaleY);
    float wantW = fmaxf(1.f, roundf((float) state->cursor.width * scaleX));
    float wantH = fmaxf(1.f, roundf((float) state->cursor.height * scaleY));
    destW = (uint32_t) wantW;
    destH = (uint32_t) wantH;

    if (bufferMightHaveChanged || destW != cursorOverlayRawW || destH != cursorOverlayRawH)
        renderCursorOverlayBuffer(destW, destH);
}

static void teardownCursorOverlay(void) {
    pthread_mutex_lock(&cursorOverlayLock);

    if (cursorSurfaceControl) {
        // Releasing the handle does not take the layer off the screen. It stays, showing whatever
        // was last put on it - which is where the frozen X-shaped cursor came from, left behind by
        // the layer belonging to the previous surface while a new one drew the live cursor. It has
        // to be hidden, and taken out of the tree, in a transaction first.
        ASurfaceTransaction *t = scApi.txCreate();

        scApi.txSetVisibility(t, cursorSurfaceControl, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        if (scApi.txReparent)
            scApi.txReparent(t, cursorSurfaceControl, NULL);
        scApi.txApply(t);
        scApi.txDelete(t);

        scApi.release(cursorSurfaceControl);
        cursorSurfaceControl = NULL;
    }

    cursorOverlayGeometryDirty = cursorOverlayBufferDirty = false;
    if (cursorOverlayPendingBuffer) {
        AHardwareBuffer_release(cursorOverlayPendingBuffer);
        cursorOverlayPendingBuffer = NULL;
    }

    pthread_mutex_unlock(&cursorOverlayLock);
}

static void ensureCursorOverlay(void) {
    bool created = false;

    if (!win || win == defaultWin || !cursorOverlayResolveApi())
        return;

    {
        if (!cursorOverlayThreadStarted) {
            cursorOverlayThreadStarted = true; // only ever try once, whether it works or not
            if (pthread_create(&cursorOverlayThread, NULL, cursorOverlayThreadMain, NULL) != 0) {
                log("Xlorie: could not start the cursor overlay thread, drawing the cursor in GL instead");
                return;
            }
            cursorOverlayFeatureAvailable = true;
        }

        if (!cursorOverlayFeatureAvailable)
            return;

        pthread_mutex_lock(&cursorOverlayLock);
        if (!cursorSurfaceControl) {
            cursorSurfaceControl = scApi.createFromWindow(win, "lorie-cursor");
            if (!cursorSurfaceControl)
                log("Xlorie: could not create the cursor overlay layer, drawing the cursor in GL instead");
            else {
                cursorOverlayGeometryDirty = true; // (re)send position and visibility to the new layer
                created = true;
            }
        }
        pthread_mutex_unlock(&cursorOverlayLock);

        if (created) {
            resendCursorOverlayBuffer(); // a new layer starts out with no buffer of its own
            // Erase whatever the GL path last drew into the surface: from here on frames carry no
            // cursor, and on an idle desktop nothing else would redraw over it.
            if (state)
                state->drawRequested = true;
        }
    }
}

/* --- Handing the root buffer straight to the compositor --- */

// Says which output path a frame will take and, when it is not the direct one, why. Logged only on
// a change, so a run's log states the backend it actually used rather than the one it was asked for.
static bool rootZeroCopyUsable(const LorieBuffer_Desc *desc) {
    static const char *lastReason = NULL;
    static bool logged = false;
    /* What was last written to the shared state, and which state it was written to. Kept apart from
     * the log's own de-duplication: the direct path being available from the first frame leaves both
     * reasons NULL, so the change test never fires and the X server never learns which backend is
     * running - and a replaced shared state starts blank, so a reason that had not changed was
     * never published into it either. */
    static const char *publishedReason = NULL;
    static volatile struct lorie_shared_server_state *publishedTo = NULL;
    static bool published = false;
    const char *reason = NULL;
    uint8_t forced = state ? state->outputBackend : LORIE_OUTPUT_AUTO;

    if (!state)
        reason = "no shared state";
    else if (forced == LORIE_OUTPUT_GPU_COPY)
        reason = "forced to gpu-copy";
    else if (!rootSurfaceControl)
        reason = "no root layer";
    else if (!state->rootDoubleBuffered)
        reason = "root is not one of our slots (a client has flipped its own in)";
    else if (!desc || desc->format != AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM)
        // The compositor reads a buffer by its declared format and has no equivalent of the
        // swizzling shader the GL path uses.
        reason = "root buffer is not BGRA";
    else if (viewportW <= 0 || viewportH <= 0)
        reason = "no viewport yet";
    else if (rootZcUnusableCount + 2 > LORIE_ZC_MAX_HELD)
        // Held slots that will never come back, so there is no longer room to retire one. Going on
        // would hold every frame back instead, which stops the screen updating altogether.
        reason = "release fences could not be waited on; too many slots stuck";
    else if (forced != LORIE_OUTPUT_ROOT_DIRECT && filtering != GL_LINEAR)
        // The compositor's scaler is bilinear and has no nearest mode, so nearest is honoured by
        // keeping the GL pass. Forcing root-direct overrides that on purpose, for comparisons.
        reason = "nearest filtering";

    if (!logged || reason != lastReason) {
        logged = true;
        lastReason = reason;
        log("XlorieBackend: %s%s%s", reason ? "GPU_COPY" : "ROOT_DIRECT",
            reason ? " - " : "", reason ? reason : "");
    }

    // Also where the X server can read it: the line above lands in this process' logcat, which from
    // the terminal is not readable at all.
    if (state)
        state->outputFilterNearest = filtering == GL_NEAREST ? 1u : 0u;

    if (state && (!published || publishedReason != reason || publishedTo != state)) {
        published = true;
        publishedReason = reason;
        publishedTo = state;
        state->outputBackendActive = reason ? LORIE_OUTPUT_GPU_COPY : LORIE_OUTPUT_ROOT_DIRECT;
        snprintf((char *) state->outputBackendReason, sizeof(state->outputBackendReason),
                 "%s", reason ? reason : "");
    }
    return reason == NULL;
}

/*
 * What the compositor's release fence says about the buffer it was handed.
 *
 * A bare poll() > 0 answered yes to POLLERR and POLLNVAL as well, which is a broken or closed
 * descriptor read as a signalled fence: the slot went back to the X server with the compositor
 * still displaying it. Nor is the time since then an answer - a descriptor that cannot be waited
 * on will not start reporting POLLIN later, and letting the slot go once enough of it had passed
 * was the same claim with a delay in front of it.
 *
 * So there are three answers, and only one of them frees the buffer.
 */
typedef enum { LORIE_ZC_FENCE_DONE, LORIE_ZC_FENCE_WAITING, LORIE_ZC_FENCE_UNUSABLE } LorieZcFence;

static LorieZcFence rootZcFenceState(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    // No fence at all is the compositor's way of saying it did not need one, which is an answer.
    if (fd < 0)
        return LORIE_ZC_FENCE_DONE;
    if (poll(&pfd, 1, 0) <= 0)
        // Not yet, or poll itself failed; neither says the buffer is free. A failing poll is worth
        // asking again rather than condemning the slot on one errno.
        return LORIE_ZC_FENCE_WAITING;
    if (pfd.revents & POLLIN)
        return LORIE_ZC_FENCE_DONE;
    return LORIE_ZC_FENCE_UNUSABLE;
}

// Binder thread. Reports the release fence of the buffer set by the transaction before this one.
static void rootZcOnComplete(void *context, ASurfaceTransactionStats *stats) {
    uint32_t seq = (uint32_t) (uintptr_t) context;
    int i;

    if (!seq)
        return;   // the transaction it belongs to retired nothing

    pthread_mutex_lock(&rootOverlayLock);
    for (i = 0; i < rootZcRetiringCount; i++) {
        // By handover, not by slot: this may be a callback from a pool that no longer exists, and
        // matching it to whatever now sits at the same slot index hands over a fence from a
        // different buffer entirely.
        if (rootZcRetiring[i].seq != seq || rootZcRetiring[i].fenceArrived)
            continue;

        /*
         * The fence is asked for with the SurfaceControl the transaction itself carried, taken from
         * the stats. It was asked for with whatever rootSurfaceControl happened to be at the time,
         * and after a window change that is a different, newly created one: the platform treats a
         * control that is not in the stats as a fatal error and aborts the process. That is what
         * killed the app on its first resize, and the reconnect that followed left the screen black.
         * Every transaction here carries exactly one control, the root layer.
         */
        {
            ASurfaceControl **controls = NULL;
            size_t count = 0;

            scApi.statsGetControls(stats, &controls, &count);
            if (count > 0)
                rootZcRetiring[i].fenceFd = scApi.statsPrevReleaseFenceFd(stats, controls[0]);
            if (controls)
                scApi.statsReleaseControls(controls);
        }
        rootZcRetiring[i].fenceArrived = true;
        rootZcRetiring[i].fenceArrivedNs = rendererNowNs();
        break;
    }
    pthread_mutex_unlock(&rootOverlayLock);
}

// Gives back every slot the compositor has finished with. Renderer thread. Returns whether there is
// room to hand it another one.
static bool rootZcDrainRetiring(void) {
    int freed[LORIE_ZC_MAX_HELD], freedFds[LORIE_ZC_MAX_HELD], freedCount = 0;
    uint64_t freedIds[LORIE_ZC_MAX_HELD];
    int i, kept = 0;
    bool room;

    pthread_mutex_lock(&rootOverlayLock);
    for (i = 0; i < rootZcRetiringCount; i++) {
        LorieZcFence answer = LORIE_ZC_FENCE_WAITING;

        /*
         * A slot from a pool the X server has since replaced. Its held bit went when the pool did,
         * and the X server has dropped the buffer, so there is nothing to give back and nothing that
         * could draw into it - the compositor keeps its own reference for as long as it shows it.
         * This is also what finally lets go of a slot whose fence could never be waited on: it was
         * held "until its pool is replaced", and this is that.
         */
        if (state->rootBufferIds[rootZcRetiring[i].slot] != rootZcRetiring[i].bufferId) {
            if (rootZcRetiring[i].fenceFd >= 0)
                close(rootZcRetiring[i].fenceFd);
            if (rootZcRetiring[i].fenceUnusable)
                rootZcUnusableCount--;
            continue;
        }

        if (rootZcRetiring[i].fenceUnusable)
            answer = LORIE_ZC_FENCE_UNUSABLE;
        else if (rootZcRetiring[i].fenceArrived)
            answer = rootZcFenceState(rootZcRetiring[i].fenceFd);

        if (answer == LORIE_ZC_FENCE_DONE) {
            freedFds[freedCount] = rootZcRetiring[i].fenceFd;
            freedIds[freedCount] = rootZcRetiring[i].bufferId;
            freed[freedCount++] = rootZcRetiring[i].slot;
            continue;
        }

        if (answer == LORIE_ZC_FENCE_UNUSABLE && !rootZcRetiring[i].fenceUnusable) {
            // Said once, and the descriptor closed here because it is ours and it is of no further
            // use. The slot is not freed: nothing has told us the compositor is finished with it.
            log("XlorieRootZc: release fence for slot %d cannot be waited on; holding the slot "
                "until its pool is replaced\n", rootZcRetiring[i].slot);
            __atomic_fetch_add(&state->presentStats.zeroCopyFenceErrors, 1, __ATOMIC_RELAXED);
            if (rootZcRetiring[i].fenceFd >= 0)
                close(rootZcRetiring[i].fenceFd);
            rootZcRetiring[i].fenceFd = -1;
            rootZcRetiring[i].fenceUnusable = true;
            rootZcUnusableCount++;
        }

        rootZcRetiring[kept++] = rootZcRetiring[i];
    }
    rootZcRetiringCount = kept;
    // Presenting adds the one on screen to this list and puts a new one on screen, so there has to be
    // room for two more.
    room = kept + 2 <= LORIE_ZC_MAX_HELD;
    pthread_mutex_unlock(&rootOverlayLock);

    for (i = 0; i < freedCount; i++) {
        if (freedFds[i] >= 0)
            close(freedFds[i]);
        lorieTrace(state, LORIE_TRACE_RELEASE, freed[i], freedIds[i]);
        rendererReleaseRootSlot(freed[i], freedIds[i]);
    }
    return room;
}

// Gives back everything we are holding. Renderer thread, on the way out of the zero-copy path.
/*
 * Stops presenting through this path, without pretending the compositor is finished.
 *
 * This used to hand every slot straight back to the X server and close the fences it had not
 * waited for. Leaving the path does not stop the compositor displaying the buffer on the layer -
 * the layer is still there with that buffer on it - so the X server was free to draw into exactly
 * what was on screen. Nor does destroying the layer handle stop it: the layer stays composited,
 * showing the last buffer put on it.
 *
 * So the layer is hidden first, which is what makes a release possible at all, and the slot that
 * was on it joins the retiring list to be given back when its release actually arrives. Draining
 * continues from the GL path, so nothing is stranded by the switch.
 */
static void rootZcStopPresenting(void) {
    // Leaving this path, so there is no zero-copy frame left to retry.
    rendererSetOutputRetry(false);

    pthread_mutex_lock(&rootOverlayLock);

    if (rootZcDisplayedSlot >= 0) {
        uint32_t retireSeq = 0;

        if (rootSurfaceControl && rootZcRetiringCount < LORIE_ZC_MAX_HELD) {
            if (++rootZcRetireSeq == 0)
                rootZcRetireSeq = 1;
            retireSeq = rootZcRetireSeq;
            rootZcRetiring[rootZcRetiringCount].slot = rootZcDisplayedSlot;
            rootZcRetiring[rootZcRetiringCount].bufferId = rootZcDisplayedId;
            rootZcRetiring[rootZcRetiringCount].fenceFd = -1;
            rootZcRetiring[rootZcRetiringCount].fenceArrived = false;
            rootZcRetiring[rootZcRetiringCount].fenceUnusable = false;
            rootZcRetiring[rootZcRetiringCount].fenceArrivedNs = 0;
            rootZcRetiring[rootZcRetiringCount].seq = retireSeq;
            rootZcRetiringCount++;

            // Hiding it is the request that gets the buffer back; the completion tells us when.
            ASurfaceTransaction *t = scApi.txCreate();

            scApi.txSetVisibility(t, rootSurfaceControl, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
            if (scApi.txSetOnComplete)
                scApi.txSetOnComplete(t, (void *) (uintptr_t) retireSeq, rootZcOnComplete);
            scApi.txApply(t);
            scApi.txDelete(t);
        } else
            // Nowhere to track it, so it stays held rather than being handed back while it may
            // still be on screen. It comes back when the pool is replaced.
            log("XlorieRootZc: no room to retire slot %d on the way out; it stays held\n",
                rootZcDisplayedSlot);

        rootZcDisplayedSlot = -1;
    }

    pthread_mutex_unlock(&rootOverlayLock);

    // Whatever has actually been released is released here; the rest keeps waiting.
    rootZcDrainRetiring();
}

// The area around the viewport is the window surface's own buffer, which nothing draws into once the
// root stopped going through GL. It has to be blacked out when the geometry changes, and only then -
// this is the one place the zero-copy path still swaps.
static void rootZcClearLetterbox(int surfaceW, int surfaceH) {
    static int lastSurfaceW = -1, lastSurfaceH = -1, lastViewport[4] = { -1, -1, -1, -1 };
    static int clearFramesLeft = 0;

    if (surfaceW != lastSurfaceW || surfaceH != lastSurfaceH ||
        viewportX != lastViewport[0] || viewportY != lastViewport[1] ||
        viewportW != lastViewport[2] || viewportH != lastViewport[3]) {
        lastSurfaceW = surfaceW; lastSurfaceH = surfaceH;
        lastViewport[0] = viewportX; lastViewport[1] = viewportY;
        lastViewport[2] = viewportW; lastViewport[3] = viewportH;
        clearFramesLeft = 4; // the surface has several buffers and each needs blacking out once
    }

    if (clearFramesLeft <= 0)
        return;

    clearFramesLeft--;
    glViewport(0, 0, surfaceW, surfaceH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    eglSwapBuffers(egl_display, sfc);
}

// Returns true when the frame has been dealt with and the GL path should be skipped.
static bool rootZcPresent(const LorieBuffer_Desc *desc, int surfaceW, int surfaceH, int64_t frameStartNs) {
    AHardwareBuffer *ahb = desc->buffer;
    int slot = rendererRootSlot, retiring;
    uint32_t retireSeq;
    int64_t fenceWaitUs = 0;
    uint64_t gpuCopySerial;
    bool carriedGpuCopy = false, alreadyOnScreen, gpuWorkIssued, frameIncomplete = false;
    ASurfaceTransaction *t;

    if (!ahb || slot < 0)
        return false;

    rootZcClearLetterbox(surfaceW, surfaceH);

    // The X server had nothing newer to publish, so this is the buffer the compositor is already
    // showing - which makes it exactly as unsafe to write as any other slot on screen. Checked
    // before the copies below rather than after them, where the same test only decided whether to
    // re-publish; the copies went in either way, straight into what was being displayed.
    pthread_mutex_lock(&rootOverlayLock);
    // By buffer as well as by index: after the pool is replaced the same index names a new buffer,
    // and taking it for the one on screen would leave the new frame unsubmitted.
    alreadyOnScreen = slot == rootZcDisplayedSlot && rendererRootSlotId == rootZcDisplayedId;
    pthread_mutex_unlock(&rootOverlayLock);

    /*
     * Anything the X server asked us to blit into the root still has to happen, and has to be
     * finished before the compositor reads the buffer. This is real GPU work and a real CPU wait,
     * and both are reported - passing constants here is what made an earlier reading of this path
     * claim the renderer had no GPU work at all.
     *
     * The wait is inside the lock, and that is the only thing currently making the X server's CPU
     * access to any of these buffers safe. It was moved outside for a while, on the grounds that
     * the GPU could only still be writing slots the X server had published and rotated off. That
     * is not what this batch is: the drain takes the whole shared queue, so it can be writing the
     * X server's current write slot or a redirected window's own pixmap, neither of which has a
     * held bit protecting it - and it is still reading the client pixmaps the copies came from,
     * which the X server is kept off only by this lock. Releasing it early let a CPU write land in
     * a buffer the GPU had not finished with.
     *
     * Moving it back out needs per-buffer read/write leases for everything the batch touches, taken
     * before the work is issued and held until it completes, which is the lease work still to do.
     * Until then the X server pays for this wait, and lockHeldUs says how much.
     */
    {
        int64_t lockStartNs = rendererNowNs(), lockHeldStartNs, flushUs = 0;

        if (!lorie_mutex_lock(&state->lock, &state->lockingPid)) {
            // Nothing submitted: what is on screen stays, and the frame is asked for again at the next
            // vsync. The claim goes back unless this slot is the one on screen - that one stays held
            // for the compositor, exactly as the nothing-new path below keeps it.
            lorieTrace(state, LORIE_TRACE_HOLD, 3, slot);
            if (alreadyOnScreen)
                rendererRootSlot = -1;
            else
                rendererReleaseRootBuffer();
            rendererSetOutputRetry(true);
            state->waitForNextFrame = true;
            return true;
        }
        lockHeldStartNs = rendererNowNs();
        gpuCopySerial = rendererApplyPendingGpuCopiesLocked(alreadyOnScreen ? -1 : slot, &gpuWorkIssued);
        // Checked under the same lock as the drain, so the answer describes the queue it just left.
        frameIncomplete = !alreadyOnScreen && rendererBufferHasQueuedCopies(desc->id);
        state->drawRequested = FALSE;
        if (gpuWorkIssued)
            rendererFinishIssuedWork(&flushUs, &fenceWaitUs);
        state->waitForNextFrame = true;
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        rendererNoteLock(rendererNsToUs(lockHeldStartNs - lockStartNs),
                         rendererNsToUs(rendererNowNs() - lockHeldStartNs));
        __atomic_fetch_add(&state->presentStats.flushUs, (uint32_t) flushUs, __ATOMIC_RELAXED);
        carriedGpuCopy = gpuCopySerial != 0;

        if (gpuCopySerial) {
            // The GPU has actually finished, so the copy was made: this is what lets the X server
            // hand the source pixmap back to its client.
            __atomic_store_n(&state->gpuCopyQueue.completedSerial, gpuCopySerial, __ATOMIC_RELEASE);
        lorieTrace(state, LORIE_TRACE_FENCE, 0, gpuCopySerial);
            notifyGpuCopyDone();
        }
    }

    if (frameIncomplete) {
        /*
         * The frame this slot holds is not finished: a copy into it is still queued behind one the
         * drain had to wait on. Submitting it anyway put an incomplete frame on screen, and made the
         * slot held so the rest of its copies could not be written into it afterwards.
         *
         * So what is on screen stays, the claim is given back, and the frame is asked for again at
         * the next vsync. The wait ends when the entry at the head resolves - its buffer arrives,
         * its slot is released, or the drain gives up on it - which is bounded by the same deadline
         * that already bounds every other wait at the head of the queue.
         */
        __atomic_fetch_add(&state->presentStats.directHeldIncomplete, 1, __ATOMIC_RELAXED);
        lorieTrace(state, LORIE_TRACE_HOLD, 1, slot);
        rendererReleaseRootBuffer();
        rendererSetOutputRetry(true);
        rendererPublishFrameStats(frameStartNs, fenceWaitUs, carriedGpuCopy, 0);
        return true;
    }

    pthread_mutex_lock(&rootOverlayLock);

    if (alreadyOnScreen) {
        // Nothing new was published; the compositor is already showing this buffer. Keep holding it.
        // The copies above still happened, so they are still reported.
        pthread_mutex_unlock(&rootOverlayLock);
        rendererRootSlot = -1;
        rendererSetOutputRetry(false);   // what the X server has published is on screen
        // Nothing was submitted: the compositor keeps the buffer it already has. Counting this as a
        // frame was worse than not counting it, because the summary then derived the GL frame count
        // by subtraction and attributed it to a GL pass that never ran.
        __atomic_fetch_add(&state->presentStats.directReuseNoSubmit, 1, __ATOMIC_RELAXED);
        rendererPublishFrameStats(frameStartNs, fenceWaitUs, carriedGpuCopy, 0);
        return true;
    }

    retiring = rootZcDisplayedSlot;
    retireSeq = 0;
    if (retiring >= 0) {
        if (rootZcRetiringCount < LORIE_ZC_MAX_HELD) {
            // Never zero, and not reused until 2^32 handovers later - by which time no entry from
            // back then can still be retiring - so a callback from a pool that has since been
            // replaced matches nothing instead of matching by slot index. The entry also carries
            // its buffer id, which rendererReleaseRootSlot checks independently of this.
            if (++rootZcRetireSeq == 0)
                rootZcRetireSeq = 1;
            retireSeq = rootZcRetireSeq;
            rootZcRetiring[rootZcRetiringCount].slot = retiring;
            rootZcRetiring[rootZcRetiringCount].fenceFd = -1;
            rootZcRetiring[rootZcRetiringCount].fenceArrived = false;
            rootZcRetiring[rootZcRetiringCount].fenceUnusable = false;
            rootZcRetiring[rootZcRetiringCount].fenceArrivedNs = 0;
            rootZcRetiring[rootZcRetiringCount].seq = retireSeq;
            rootZcRetiring[rootZcRetiringCount].bufferId = rootZcDisplayedId;
            rootZcRetiringCount++;
        } else
            // rootZcDrainRetiring() leaves room for two before this is reached, so there is no
            // path here. Untracked would mean the slot is never given back at all - the pool runs
            // a buffer short for the rest of the session - so it is worth saying rather than
            // dropping quietly.
            log("XlorieRootZc: no room to track slot %d on its way off screen; it stays held\n",
                retiring);
    }
    rootZcDisplayedSlot = slot;
    rootZcDisplayedId = rendererRootSlotId;

    ARect src = { 0, 0, desc->width, desc->height };
    ARect dst = { viewportX, viewportY, viewportX + viewportW, viewportY + viewportH };

    t = scApi.txCreate();
    scApi.txSetVisibility(t, rootSurfaceControl, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    scApi.txSetZOrder(t, rootSurfaceControl, 0); // the cursor layer sits at 1, above this
    scApi.txSetBuffer(t, rootSurfaceControl, ahb, -1);
    /*
     * The root is an opaque desktop: its depth is 24, so the fourth byte of each pixel is padding,
     * not alpha. The slots are allocated BGRA because that is the layout the X server writes, which
     * leaves the compositor free to read that padding as alpha. Declaring the layer opaque says how it
     * is meant to be read - it does not claim the padding byte is 255 everywhere, because nothing
     * guarantees that, and it is exactly why the declaration is needed.
     *
     * It also removes one reason the compositor might decline to put the layer on a hardware plane.
     * Whether it then does is the compositor's decision and depends on the buffer's usage and on the
     * device; nothing here claims that it does, or that a full-screen blend was happening before.
     *
     * The cursor layer is deliberately left alone: its alpha is the whole point of it.
     */
    if (scApi.txSetBufferTransparency)
        scApi.txSetBufferTransparency(t, rootSurfaceControl, LORIE_SC_TRANSPARENCY_OPAQUE);
    scApi.txSetGeometry(t, rootSurfaceControl, &src, &dst, 0);
    if (scApi.txSetOnComplete)
        scApi.txSetOnComplete(t, (void *) (uintptr_t) retireSeq, rootZcOnComplete);
    scApi.txApply(t);
    scApi.txDelete(t);

    pthread_mutex_unlock(&rootOverlayLock);

    // This slot is ours until the compositor lets go, so the generic release must leave it alone.
    rendererRootSlot = -1;
    rendererSetOutputRetry(false);
    __atomic_fetch_add(&state->renderedFrames, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&state->presentStats.directBufferSubmits, 1, __ATOMIC_RELAXED);
    lorieTrace(state, LORIE_TRACE_DIRECT, slot, rootZcDisplayedId);
    rendererPublishFrameStats(frameStartNs, fenceWaitUs, carriedGpuCopy, 0);
    return true;
}

static void teardownRootOverlay(void) {
    rootZcStopPresenting();

    pthread_mutex_lock(&rootOverlayLock);
    if (rootSurfaceControl) {
        // Same as the cursor layer: releasing the handle leaves the layer composited, still showing
        // the last buffer put on it.
        ASurfaceTransaction *t = scApi.txCreate();

        scApi.txSetVisibility(t, rootSurfaceControl, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        if (scApi.txReparent)
            scApi.txReparent(t, rootSurfaceControl, NULL);
        scApi.txApply(t);
        scApi.txDelete(t);

        scApi.release(rootSurfaceControl);
        rootSurfaceControl = NULL;
    }
    pthread_mutex_unlock(&rootOverlayLock);
}

static void ensureRootOverlay(void) {
    if (!win || win == defaultWin || !cursorOverlayResolveApi())
        return;

    // Without these two the buffer could be handed over but never taken back, which would mean
    // reusing it while the compositor still reads it.
    if (!scApi.txSetOnComplete || !scApi.statsPrevReleaseFenceFd ||
        !scApi.statsGetControls || !scApi.statsReleaseControls) {
        // Named, so that getting one of these wrong is a one-line answer next time rather than a
        // round of guessing.
        log("Xlorie: root layer needs %s%s%s, drawing the root through GL instead",
            !scApi.txSetOnComplete ? "ASurfaceTransaction_setOnComplete " : "",
            !scApi.statsPrevReleaseFenceFd ? "ASurfaceTransactionStats_getPreviousReleaseFenceFd " : "",
            !scApi.statsGetControls || !scApi.statsReleaseControls
                ? "ASurfaceTransactionStats_getASurfaceControls/releaseASurfaceControls" : "");
        return;
    }

    pthread_mutex_lock(&rootOverlayLock);
    if (!rootSurfaceControl) {
        rootSurfaceControl = scApi.createFromWindow(win, "lorie-root");
        if (!rootSurfaceControl)
            log("Xlorie: could not create the root layer, drawing the root through GL instead");
    }
    pthread_mutex_unlock(&rootOverlayLock);
}
