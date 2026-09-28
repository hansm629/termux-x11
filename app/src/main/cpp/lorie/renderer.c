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
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <math.h>
#include <android/looper.h>
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
// Renderer thread only.
static LorieBuffer *cursorOverlayRenderTarget = NULL;
static GLuint cursorOverlayFbo = 0;
static uint32_t cursorOverlayRawW = 0, cursorOverlayRawH = 0;
// The root buffer's size as of the last frame. The overlay thread needs it to place the cursor and
// has no access to the root buffer, so the renderer publishes it here.
static volatile float cursorOverlaySourceW = 0.f, cursorOverlaySourceH = 0.f;

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
#define LORIE_COPY_DEFER_FRAMES 30

// Looks the buffer up and attaches it if it has arrived but not been attached yet. It used to sleep
// here - up to 20 x 5 ms - when a buffer had not been registered yet, which stops completedSerial
// from advancing: every present waiting on it is then re-queued one vblank at a time, and a single
// unregistered buffer holds up every copy behind it. The caller defers instead.
static LorieBuffer *rendererFindBuffer(uint64_t id) {
    LorieBuffer *buf;

    pthread_spin_lock(&bufferLock);
    buf = LorieBufferList_findById(&buffers, id);
    if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id))) {
        LorieBuffer_attachToGL(buf);
        LorieBuffer_addToList(buf, &buffers);
    }
    pthread_spin_unlock(&bufferLock);

    return buf;
}

static uint64_t rendererApplyPendingGpuCopiesLocked(void) {
    bool fboSetUp = false;
    uint64_t lastSerial = 0;
    uint64_t boundDstId = 0;
    GLint prevViewport[4];

    if (!state || state->gpuCopyQueue.readIndex == state->gpuCopyQueue.writeIndex)
        return 0;

    while (state->gpuCopyQueue.readIndex != __atomic_load_n(&state->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE)) {
        LorieGpuCopyEntry entry = state->gpuCopyQueue.entries[state->gpuCopyQueue.readIndex % LORIE_GPU_COPY_QUEUE_CAPACITY];
        LorieBuffer *src = rendererFindBuffer(entry.srcBufferId);
        LorieBuffer *dst = rendererFindBuffer(entry.dstBufferId);

        if (!src || !dst) {
            // Its buffer has not reached us yet. Leave it queued and look again on the next frame
            // rather than waiting here, which would stall every present behind it as well.
            static uint64_t deferredSerial = 0;
            static int deferredFrames = 0;

            if (deferredSerial != entry.serial) {
                deferredSerial = entry.serial;
                deferredFrames = 0;
            }

            if (++deferredFrames <= LORIE_COPY_DEFER_FRAMES) {
                state->presentStats.copyDeferrals++;
                break;
            }

            log("rendererApplyPendingGpuCopies: %s buffer %llu never arrived, skipping\n",
                src ? "destination" : "source",
                (unsigned long long) (src ? entry.dstBufferId : entry.srcBufferId));
            state->presentStats.copySkips++;
        }

        if (src && dst) {
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

        lastSerial = entry.serial;
        state->gpuCopyQueue.readIndex++;
    }

    if (fboSetUp) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
    return lastSerial;
}

// Standalone entry point used by the renderer thread's main loop. Used when no redraw is going to
// happen on this tick (rare for GPU copies in practice, since scheduling one also marks damage
// non-empty - see lorieTryScheduleGpuCopy), so it has to take the lock and fence/unlock itself.
static void rendererApplyPendingGpuCopies(void) {
    uint64_t serial;
    if (!state || state->gpuCopyQueue.readIndex == state->gpuCopyQueue.writeIndex)
        return;
    int64_t lockStartNs = rendererNowNs();
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    serial = rendererApplyPendingGpuCopiesLocked();
    if (serial) {
        EGLSync fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
        glFlush();
        eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
        eglDestroySyncKHR(egl_display, fence);
        // Only now that the GPU has actually finished (not just been told to start) is it safe to
        // let present_execute_copy release/idle the source pixmap back to the client.
        __atomic_store_n(&state->gpuCopyQueue.completedSerial, serial, __ATOMIC_RELEASE);
        notifyGpuCopyDone();
    }
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
    state->presentStats.lockHeldUs += (uint32_t) rendererNsToUs(rendererNowNs() - lockStartNs);
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

        state->presentStats.frameSumUs += deltaUs;
        state->presentStats.frameSamples++;
        if (deltaUs > state->presentStats.maxFrameUs)
            state->presentStats.maxFrameUs = deltaUs;
        if (deltaUs >= LORIE_LONG_FRAME_US)
            state->presentStats.longFrames++;
    }

    lastFrameStartNs = frameStartNs;

    if (fenceWaitUs > 0) {
        state->presentStats.fenceWaitUs += (uint32_t) fenceWaitUs;
        if ((uint32_t) fenceWaitUs > state->presentStats.fenceWaitMaxUs)
            state->presentStats.fenceWaitMaxUs = (uint32_t) fenceWaitUs;
    }
    if (carriedGpuCopy)
        state->presentStats.gpuCopyFrames++;
    if (coalesceWaitUs > 0)
        state->presentStats.coalescedFrames++;

    state->presentStats.displayRefreshMHz = (uint32_t) (rendererDisplayRefreshRateHz * 1000.0f);
}

// Root double buffering handshake, see the rootHandover comment in lorie.h. Claiming tells the X
// server "I am sampling this slot, do not take it back"; it is the only thing that keeps the X
// server from having to wait for our fence, so every path out of a claimed frame must release.
static uint64_t rendererClaimRootBuffer(void) {
    uint32_t old, claimed;

    if (!state->rootDoubleBuffered)
        return state->rootWindowTextureID;

    do {
        old = __atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE);
        claimed = old | 1u;
    } while (!__atomic_compare_exchange_n(&state->rootHandover, &old, claimed, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    return state->rootBufferIds[(claimed >> 1) & 1u];
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

    if (fence == EGL_NO_SYNC_KHR)
        return;

    rendererPendingFence = EGL_NO_SYNC_KHR;

    // Zero timeout: this has normally signalled long ago. The flush bit only matters for the rare
    // case where nothing has been submitted since, and the bounded wait below is the safety net.
    if (eglClientWaitSyncKHR(egl_display, fence, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0) == EGL_TIMEOUT_EXPIRED_KHR)
        eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);

    eglDestroySyncKHR(egl_display, fence);

    if (rendererPendingGpuCopySerial && state) {
        __atomic_store_n(&state->gpuCopyQueue.completedSerial, rendererPendingGpuCopySerial, __ATOMIC_RELEASE);
        notifyGpuCopyDone();
    }
    rendererPendingGpuCopySerial = 0;

    rendererReleaseRootBuffer();
}

static void rendererReleaseRootBuffer(void) {
    uint32_t old, released;

    if (!state->rootDoubleBuffered)
        return;

    do {
        old = __atomic_load_n(&state->rootHandover, __ATOMIC_ACQUIRE);
        released = old & ~1u;
    } while (!__atomic_compare_exchange_n(&state->rootHandover, &old, released, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

void rendererRedrawLocked(bool* waitingForBuffers) {
    // Captured before the locked section clears it: a frame with no damage at all is one the
    // cursor alone asked for, which rendererShouldWait() refuses to coalesce.
    bool cursorOnlyFrame = state && !state->drawRequested &&
                           (state->cursor.moved || state->cursor.updated);
    int64_t lockHeldUs = 0;
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
        lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid);
        state->cursor.updated = false;
        if (state->cursor.width && state->cursor.height &&
            state->cursor.width <= LORIE_CURSOR_TEX_SIZE && state->cursor.height <= LORIE_CURSOR_TEX_SIZE) {
            bindTexture(cursor.id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) state->cursor.width, (GLsizei) state->cursor.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, (const void *) state->cursor.bits);
        }
        lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);
        state->presentStats.cursorUploads++;
        state->presentStats.cursorUploadUs += (uint32_t) rendererNsToUs(rendererNowNs() - uploadStartNs);
    }

    // We should signal X server to not use root window while we actively copy it
    int64_t lockStartNs = rendererNowNs();
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    // Share this draw's flush+fence below instead of a separate round trip per frame.
    uint64_t gpuCopySerial = rendererApplyPendingGpuCopiesLocked();
    state->drawRequested = FALSE;

    LorieBuffer_bindTexture(buffer);
    if (desc->type == LORIEBUFFER_FD)
        xfactor = (float) desc->width/(float) desc->stride;
    draw(0, -1.f, -1.f, 1.f, 1.f, xfactor, LorieBuffer_isRgba(buffer));
    // With a double buffered root nothing here has to be waited for inside this frame, so no fence
    // is created yet and no flush is issued: eglSwapBuffers below becomes the single submission
    // point of the frame, and the fence made just before it is retired at the start of the next one.
    if (!deferFence && (rootFenceWaitEnabled || gpuCopySerial)) {
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
        if (fence != EGL_NO_SYNC_KHR) {
            int64_t waitStartNs = rendererNowNs();
            eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
            rootWaitUs = rendererNsToUs(rendererNowNs() - waitStartNs);

            eglDestroySyncKHR(egl_display, fence);
            fence = EGL_NO_SYNC_KHR;
        }
        // Sampling of the root buffer is complete, so hand it back.
        rendererReleaseRootBuffer();
        if (gpuCopySerial) {
            __atomic_store_n(&state->gpuCopyQueue.completedSerial, gpuCopySerial, __ATOMIC_RELEASE);
            notifyGpuCopyDone();
        }
    }
    state->waitForNextFrame = true;
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
    lockHeldUs = rendererNsToUs(rendererNowNs() - lockStartNs);
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

        if (eglSwapBuffers(egl_display, sfc) != EGL_TRUE)
            printEglError("Failed to swap buffers", __LINE__);

        swapUs = rendererNsToUs(rendererNowNs() - swapStartNs);
    } else {
        if (eglSwapBuffers(egl_display, sfc) != EGL_TRUE)
            printEglError("Failed to swap buffers", __LINE__);
    }

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

    state->renderedFrames++;
    rendererPublishFrameStats(frameStartNs, rootWaitUs, gpuCopySerial != 0, coalesceWaitUs);
    state->presentStats.lockHeldUs += (uint32_t) lockHeldUs;
    if (cursorOnlyFrame)
        state->presentStats.cursorOnlyFrames++;

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
        while (rendererShouldWait(&waitingForBuffers))
            pthread_cond_wait(stateCond, &stateLock);

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
        bool cursorOnly = state && !state->drawRequested && !gpuCopyPending &&
            (state->cursor.moved || state->cursor.updated);

        if (cursorOnly && state->surfaceAvailable && cursorOverlaySourceW > 0.f && cursorOverlayUsable()) {
            markCursorOverlayDirty(state->cursor.updated);
            state->cursor.moved = state->cursor.updated = FALSE;
            state->presentStats.cursorOverlayMoves++;
        } else if (state && state->surfaceAvailable && !state->waitForNextFrame &&
            (state->drawRequested || state->cursor.moved || state->cursor.updated || gpuCopyPending)) {
            rendererRedrawLocked(&waitingForBuffers);
        } else if (gpuCopyPending) {
            rendererApplyPendingGpuCopies();
        }

        pthread_spin_lock(&bufferLock);
        // Remove all buffers which were attached to GL.
        while((buf = LorieBufferList_first(&removedBuffers)))
            LorieBuffer_release(buf);
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
static void applyCursorOverlayIfDirty(void) {
    bool geometryDirty, bufferDirty, visible;
    ASurfaceControl *sc;
    AHardwareBuffer *buf = NULL;
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;

    pthread_mutex_lock(&cursorOverlayLock);
    sc = cursorSurfaceControl;
    geometryDirty = cursorOverlayGeometryDirty;
    bufferDirty = cursorOverlayBufferDirty;
    cursorOverlayGeometryDirty = cursorOverlayBufferDirty = false;
    if (bufferDirty) {
        buf = cursorOverlayPendingBuffer;
        cursorOverlayPendingBuffer = NULL;
    }
    pthread_mutex_unlock(&cursorOverlayLock);

    if (!sc || (!geometryDirty && !bufferDirty) || !state) {
        if (buf)
            AHardwareBuffer_release(buf);
        return;
    }

    visible = computeCursorOverlayRect(&x, &y, &w, &h);

    // sc can only be non-NULL where the transaction API exists, but the guard has to be written as
    // a positive test for the compiler to accept it as one.
    if (__builtin_available(android 29, *)) {
        ARect src = { 0, 0, (int32_t) w, (int32_t) h };
        ARect dst = { x, y, x + (int32_t) w, y + (int32_t) h };
        ASurfaceTransaction *t = ASurfaceTransaction_create();

        ASurfaceTransaction_setVisibility(t, sc, visible ? ASURFACE_TRANSACTION_VISIBILITY_SHOW
                                                         : ASURFACE_TRANSACTION_VISIBILITY_HIDE);
        ASurfaceTransaction_setZOrder(t, sc, 1); // above the window's own buffer
        if (visible) {
            if (buf)
                ASurfaceTransaction_setBuffer(t, sc, buf, -1);
            // Rendered at exactly w x h already, so the compositor never scales or filters it.
            ASurfaceTransaction_setGeometry(t, sc, &src, &dst, 0);
        }
        ASurfaceTransaction_apply(t);
        ASurfaceTransaction_delete(t);
    }

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
    AHardwareBuffer *ahb;

    if (!cursorOverlayRenderTarget)
        return;

    ahb = LorieBuffer_description(cursorOverlayRenderTarget)->buffer;
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
    GLint prevViewport[4];
    EGLSync fence;

    if (!state->cursor.width || !state->cursor.height ||
        state->cursor.width > LORIE_CURSOR_TEX_SIZE || state->cursor.height > LORIE_CURSOR_TEX_SIZE)
        return;

    lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid);
    bindTexture(cursor.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei) state->cursor.width, (GLsizei) state->cursor.height,
                    GL_RGBA, GL_UNSIGNED_BYTE, (const void *) state->cursor.bits);
    lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);

    if (!cursorOverlayRenderTarget || cursorOverlayRawW != destW || cursorOverlayRawH != destH) {
        if (cursorOverlayRenderTarget)
            LorieBuffer_release(cursorOverlayRenderTarget);
        cursorOverlayRenderTarget = LorieBuffer_allocate((int32_t) destW, (int32_t) destH,
                                                        AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                                        LORIEBUFFER_AHARDWAREBUFFER);
        if (cursorOverlayRenderTarget)
            LorieBuffer_attachToGL(cursorOverlayRenderTarget);
        cursorOverlayRawW = cursorOverlayRawH = 0;
    }
    if (!cursorOverlayRenderTarget)
        return;

    glGetIntegerv(GL_VIEWPORT, prevViewport);
    if (!cursorOverlayFbo)
        glGenFramebuffers(1, &cursorOverlayFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, cursorOverlayFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           LorieBuffer_getGLTextureId(cursorOverlayRenderTarget), 0);
    glViewport(0, 0, (GLsizei) destW, (GLsizei) destH);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // y is inverted because we are drawing into a texture rather than onto the surface. The cursor
    // sits in the top left corner of a fixed size texture, so sample only that part of it.
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
        if (__builtin_available(android 29, *))
            ASurfaceControl_release(cursorSurfaceControl);
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

    if (!win || win == defaultWin)
        return;

    if (__builtin_available(android 29, *)) {
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
            cursorSurfaceControl = ASurfaceControl_createFromWindow(win, "lorie-cursor");
            if (!cursorSurfaceControl)
                log("Xlorie: could not create the cursor overlay layer, drawing the cursor in GL instead");
            else {
                cursorOverlayGeometryDirty = true; // (re)send position and visibility to the new layer
                created = true;
            }
        }
        pthread_mutex_unlock(&cursorOverlayLock);

        if (created)
            resendCursorOverlayBuffer(); // a new layer starts out with no buffer of its own
    }
}
