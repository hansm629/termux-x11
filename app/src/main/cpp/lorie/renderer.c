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
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "list.h"
#include "lorie.h"
#include "vk.h"
#include <limits.h>

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

static void rendererNoteEglSwap(EGLSurface surface, EGLBoolean ok);

static EGLBoolean
rendererMaybeSkipEglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    EGLBoolean ok;

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
    ok = eglSwapBuffers(display, surface);
    rendererNoteEglSwap(surface, ok);
    return ok;
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
 * The Vulkan backend (vk.h). A window is drawn either by GLES, through its EGL window surface, or by Vulkan,
 * through a swapchain - never both: an ANativeWindow takes one producer at a time, so a window Vulkan is to
 * draw gets no EGL surface, and EGL stays current on the default (ImageReader) surface, where GLES still runs
 * the GPU copies when Vulkan does not.
 *
 * vkReady    device, ownership self-test and renderer objects made (kept while windows come and go)
 * vkActive   the window has a swapchain and Vulkan draws it
 * vkFailed   a failure ended Vulkan for this X server session: everything Vulkan made is gone, and the window
 *            is GLES's in auto, nobody's in a session that asked for Vulkan only (an explicit failure)
 * vkOwned    the X server answered our PER_ACCESS request vkOwnershipSeq; re-checked under its lock every frame
 * vkGiveBack ask the X server for PERSISTENT (its own CPU ownership) at the next backend selection
 * backendSelect  the window, the session or the backend's state changed: rendererSelectBackend runs
 */
static struct lorie_vk vk;
static struct lorie_vk_images vkImages;
static struct lorie_vk_renderer vkr;
static bool vkReady, vkActive, vkFailed, vkOwned, vkGiveBack, backendSelect;
static uint32_t vkOwnershipSeq;

// P11-C3: the last Vulkan frame's start and present call, for the pacing intervals, and the swapchain they were made
// on. Another swapchain, window, X server session or device starts them over: no interval spans one.
static uint64_t vkPacingLastStartUs, vkPacingLastPresentUs;
static VkSwapchainKHR vkPacingSwapchain;
// ... and what that last frame spent, and when it ended
static struct { uint64_t endUs, cpuUs, lockUs, fenceUs, afterRootUs; } vkPacingLast;
static void rendererVkPacingReset(void) {
    vkPacingLastStartUs = vkPacingLastPresentUs = 0;
    vkPacingSwapchain = VK_NULL_HANDLE;
    memset(&vkPacingLast, 0, sizeof(vkPacingLast));
}

// Counted after the real call (see rendererMaybeSkipEglSwapBuffers, which can skip a swap and still
// return EGL_TRUE), and only for a window surface: the default one is the ImageReader's, never shown.
static void rendererNoteEglSwap(EGLSurface surface, EGLBoolean ok) {
    if (ok == EGL_TRUE && surface != defaultSfc)
        LORIE_DIAG_ADD(state, eglSwapBuffers, 1);
}

// LorieBuffer_attachToGL, counting the EGLImages it actually creates.
static void rendererAttachToGL(LorieBuffer *buf) {
    if (LorieBuffer_attachToGL(buf))
        LORIE_DIAG_ADD(state, eglImageImports, 1);
}

// What draws the root now: Vulkan once a swapchain of ours is on the window, GLES once a window surface of ours
// is current, nothing otherwise.
static void rendererPublishActive(void) {
    if (state)
        state->diag.active = vkActive ? LORIE_ACTIVE_VULKAN :
                             win != defaultWin && sfc != defaultSfc ? LORIE_ACTIVE_GLES : LORIE_ACTIVE_NONE;
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

static void notifyGpuCopyPublished(uint64_t publishedUs) {
    if (conn_fd != -1) {
        lorieEvent e = { .gpuCopyDone = { .t = EVENT_GPU_COPY_DONE, .publishedUs = publishedUs } };
        write(conn_fd, &e, sizeof(e));
    }
}

// Without the time it was published (P10-B0 times only the Vulkan backend's, which pass it).
static void notifyGpuCopyDone(void) {
    notifyGpuCopyPublished(0);
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
            // The renderer took the buffer up (its GLES texture, its Vulkan image, or both may exist), so we should
            // release it from renderer thread.
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
        // These buffers were taken up by the renderer (GLES texture, Vulkan image), we must release them from renderer thread.
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

enum { RENDERER_USE_GLES, RENDERER_USE_VULKAN, RENDERER_USE_NONE };

// Who draws a window in a session asked for `requested`: GLES when asked for; Vulkan (auto or vulkan) until it
// failed in this session; after that GLES in auto, nobody in a session that asked for Vulkan only.
static int rendererBackendFor(uint8_t requested, bool vulkanFailed) {
    if (requested == LORIE_BACKEND_GLES)
        return RENDERER_USE_GLES;
    if (!vulkanFailed)
        return RENDERER_USE_VULKAN;
    return requested == LORIE_BACKEND_VULKAN ? RENDERER_USE_NONE : RENDERER_USE_GLES;
}

/*
 * A buffer's GLES texture (and EGLImage) is made when it arrives only in a session GLES draws, as 3c98ffa made
 * it; while Vulkan draws (or may still be started) nothing GLES of it is made: Vulkan imports the AHB itself.
 */
static bool rendererGlesEager(void) {
    return !vkActive && (!state || rendererBackendFor(state->backend.requested, vkFailed) == RENDERER_USE_GLES);
}

// Before GLES uses a buffer: its texture, made now if it was not when the buffer came (a fallback to GLES, or
// GLES running the copies of a session Vulkan failed in). Made once: LorieBuffer_attachToGL makes a new one per call.
static void rendererGlBuffer(LorieBuffer *buf) {
    if (buf && !LorieBuffer_getGLTextureId(buf))
        rendererAttachToGL(buf);
}

// GLES on the window: its EGL window surface made and current (what rendererRefreshContext always did).
static void rendererUseEgl(void) {
    sfc = eglCreateWindowSurface(egl_display, cfg, win, NULL);
    if (sfc == EGL_NO_SURFACE)
        return vprintEglError("eglCreateWindowSurface failed", __LINE__);

    if (eglMakeCurrent(egl_display, sfc, sfc, ctx) != EGL_TRUE) {
        if (state) {
            state->surfaceAvailable = false;
            state->diag.active = LORIE_ACTIVE_NONE;
        }
        notifyGpuCopyDone();
        return vprintEglError("eglMakeCurrent failed", __LINE__);
    }

    rendererApplyPresentMode();
    rendererPublishActive();

    // We should redraw image at least once right after surface change
    if (state)
        state->surfaceAvailable = state->drawRequested = state->cursor.updated = win != defaultWin;

    glViewport(0, 0, ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
    log("Xlorie: new surface applied: %p\n", sfc);
}

// The window's EGL surface let go of (the window kept), so that a Vulkan swapchain can be made on it.
static void rendererDropEgl(void) {
    if (sfc == EGL_NO_SURFACE || sfc == defaultSfc)
        return;
    eglSwapBuffers(egl_display, sfc);   // as releaseWinAndSurface: the dequeued buffer goes back
    if (eglMakeCurrent(egl_display, defaultSfc, defaultSfc, ctx) != EGL_TRUE)
        vprintEglError("eglMakeCurrent failed (default surface)", __LINE__);
    if (eglDestroySurface(egl_display, sfc) != EGL_TRUE)
        vprintEglError("eglDestroySurface failed", __LINE__);
    sfc = defaultSfc;
}

void rendererRefreshContext(void) {
    int width = pendingWin ? ANativeWindow_getWidth(pendingWin) : 0;
    int height = pendingWin ? ANativeWindow_getHeight(pendingWin) : 0;
    log("rendererSetWindow %p %d %d", pendingWin, width, height);

    // A swapchain goes before its window does (the device stays for the next window).
    if (vkActive) {
        lorieVkSwapchainDestroy(&vk, &vkr);
        vkActive = false;
    }
    rendererVkPacingReset();
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
        rendererPublishActive();
        if (state)
            state->surfaceAvailable = false;
        notifyGpuCopyDone(); // Wake up any GPU copy stuck waiting on a surface we no longer have.
        return;
    }

    // A window Vulkan may draw gets no EGL surface: rendererSelectBackend decides, once this change is done.
    if (state && rendererBackendFor(state->backend.requested, vkFailed) != RENDERER_USE_GLES) {
        state->surfaceAvailable = false;
        rendererPublishActive();
        backendSelect = true;
        return;
    }
    rendererUseEgl();
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
static LorieBuffer *rendererFindBufferWithRetry(uint64_t id) {
    LorieBuffer *buf;
    int attempt;

    pthread_spin_lock(&bufferLock);
    buf = LorieBufferList_findById(&buffers, id);
    if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id)))
        LorieBuffer_addToList(buf, &buffers);     // its GLES texture, if GLES uses it: rendererGlBuffer
    pthread_spin_unlock(&bufferLock);

    for (attempt = 0; attempt < 20 && !buf; attempt++) {
        usleep(5000);
        pthread_spin_lock(&bufferLock);
        buf = LorieBufferList_findById(&buffers, id);
        if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id)))
            LorieBuffer_addToList(buf, &buffers);
        pthread_spin_unlock(&bufferLock);
    }
    return buf;
}

/*
 * P11-C3-B1 (lorie.h cpuAccess): a queued copy using a buffer the X server holds reserved for a CPU access is not
 * recorded, nor any copy after it (the queue's order kept): the pass goes on without them - its frame drawn. The
 * X server is asked to wake the renderer when it lets a reservation go (`waiter`); until one is let go the copies
 * left queued are no reason to run again (rendererShouldWait), and the wait for that is bounded
 * (LORIE_COPIES_DEFERRED_RECHECK_US): its wake may come between the check and the wait.
 */
#define LORIE_COPIES_DEFERRED_RECHECK_US 4000
static bool deferredCopies;
static uint32_t deferredAt, deferredReleases;

// Under the X server's lock: whether queued copy `index` waits for a reservation - the copies left queued from it.
static bool rendererCopyDeferred(const LorieGpuCopyEntry *entry, uint32_t index) {
    if (!state->cpuAccess.count || (!lorieCpuReserved(state, entry->srcBufferId) && !lorieCpuReserved(state, entry->dstBufferId)))
        return false;
    deferredCopies = true;
    deferredAt = index;
    deferredReleases = __atomic_load_n(&state->cpuAccess.releases, __ATOMIC_ACQUIRE);
    __atomic_store_n(&state->cpuAccess.waiter, 1, __ATOMIC_RELAXED);
    LORIE_DIAG_ADD(state, copiesDeferred, 1);
    return true;
}

// Whether the copies left queued still wait: the queue where they were left, no reservation let go since.
static bool rendererCopiesStillDeferred(void) {
    return deferredCopies && state && state->gpuCopyQueue.readIndex == deferredAt &&
           __atomic_load_n(&state->cpuAccess.releases, __ATOMIC_ACQUIRE) == deferredReleases;
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
        LorieBuffer *src, *dst;

        if (entry.cancelled) {
            // Its presentation let go before it was recorded (InitOutput.c lorieGpuCopySettle): not copied, completed.
            lastSerial = entry.serial;
            state->gpuCopyQueue.readIndex++;
            continue;
        }
        if (rendererCopyDeferred(&entry, state->gpuCopyQueue.readIndex))
            break;      // P11-C3-B1: left queued, with the copies after it
        src = rendererFindBufferWithRetry(entry.srcBufferId);
        dst = rendererFindBufferWithRetry(entry.dstBufferId);
        if (!src)
            log("rendererApplyPendingGpuCopies: source buffer %llu not found after waiting, skipping\n", (unsigned long long) entry.srcBufferId);
        if (!dst)
            log("rendererApplyPendingGpuCopies: destination buffer %llu not found after waiting, skipping\n", (unsigned long long) entry.dstBufferId);

        if (src && dst) {
            const LorieBuffer_Desc *srcDesc = LorieBuffer_description(src);
            const LorieBuffer_Desc *dstDesc = LorieBuffer_description(dst);
            int i;

            rendererGlBuffer(src);
            rendererGlBuffer(dst);

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
                log("rendererApplyPendingGpuCopies: rect (%d,%d)-(%d,%d) off=(%d,%d) -> ndc=(%.3f,%.3f)-(%.3f,%.3f) uv=(%.3f,%.3f)-(%.3f,%.3f) srcTex=%u dstTex=%u swizzle=%d\n",
                    r.x1, r.y1, r.x2, r.y2, entry.xOff, entry.yOff, x0, y0, x1, y1, u0, v0, u1, v1,
                    LorieBuffer_getGLTextureId(src), LorieBuffer_getGLTextureId(dst), needsSwizzle);
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

    if (fenceWaitUs > 0)
        state->presentStats.fenceWaitUs += (uint32_t) fenceWaitUs;
    if (carriedGpuCopy)
        state->presentStats.gpuCopyFrames++;
    if (coalesceWaitUs > 0)
        state->presentStats.coalescedFrames++;
}

void rendererRedrawLocked(bool* waitingForBuffers) {
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
    // The buffer will not be released until this function ends, but main thread can modify buffer list
    pthread_spin_lock(&bufferLock);
    LorieBuffer *buffer = LorieBufferList_findById(&buffers, state->rootWindowTextureID);
    // Probably X server requested us to draw removed buffer and immediately requested to remove it. Let's display it one last time.
    if (!buffer)
        buffer = LorieBufferList_findById(&removedBuffers, state->rootWindowTextureID);
    if (!buffer)
        *waitingForBuffers = true;
    pthread_spin_unlock(&bufferLock);
    if (!buffer) {
        log("Buffer %llu not found", state->rootWindowTextureID);
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
            state->rootWindowTextureID, alignedExpectedW, expectedH, expectedW, expectedH,
            desc->width, desc->height);
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
    if (surfaceW > 0 && surfaceH > 0 &&
            (viewportX != 0 || viewportY != 0 ||
             viewportW != surfaceW || viewportH != surfaceH)) {
        glViewport(0, 0, surfaceW, surfaceH);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glViewport(viewportX, surfaceH - viewportY - viewportH, viewportW, viewportH);

    // We should signal X server to not use root window while we actively copy it
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    // Share this draw's flush+fence below instead of a separate round trip per frame.
    uint64_t gpuCopySerial = rendererApplyPendingGpuCopiesLocked();
    state->drawRequested = FALSE;

    rendererGlBuffer(buffer);
    LorieBuffer_bindTexture(buffer);
    if (desc->type == LORIEBUFFER_FD)
        xfactor = (float) desc->width/(float) desc->stride;
    draw(0, -1.f, -1.f, 1.f, 1.f, xfactor, LorieBuffer_isRgba(buffer));
    LORIE_DIAG_ADD(state, glesCopyFrames, 1);
    if (rootFenceWaitEnabled || gpuCopySerial) {
        fence = eglCreateSyncKHR(egl_display, EGL_SYNC_FENCE_KHR, NULL);
        glFlush();
    }

    if (state->cursor.updated) {
        log("Xlorie: updating cursor\n");
        lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid);
        state->cursor.updated = false;
        bindTexture(cursor.id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei) state->cursor.width, (GLsizei) state->cursor.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, state->cursor.bits);
        lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);
    }

    state->cursor.moved = FALSE;
    drawCursor((float) (LorieBuffer_getWidth(buffer)), (float) (LorieBuffer_getHeight(buffer)));
    glFlush();

    /* Wait until root window drawing is finished before giving control back to X server.
     * A GPU copy applied above is only complete once this fence signals, so a frame that carries
     * one waits even with the root fence wait turned off; otherwise the X server could hand the
     * source pixmap back to its client while the GPU still reads it. */
    if (fence != EGL_NO_SYNC_KHR) {
        int64_t waitStartNs = rendererNowNs();
        eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
        rootWaitUs = rendererNsToUs(rendererNowNs() - waitStartNs);

        eglDestroySyncKHR(egl_display, fence);
        fence = EGL_NO_SYNC_KHR;
    }
    if (gpuCopySerial) {
        __atomic_store_n(&state->gpuCopyQueue.completedSerial, gpuCopySerial, __ATOMIC_RELEASE);
        notifyGpuCopyDone();
    }
    state->waitForNextFrame = true;
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
// Gaming fast path: submit GL commands before swap without creating or waiting on fences.
    // This keeps the no-fence fast path but avoids moving all submit work into swap.
    if (!rootFenceWaitEnabled) {
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

/* ---- the Vulkan backend: choosing it, starting it, its frames ---- */

/*
 * Asks the X server for CPU ownership `mode` (struct lorie_backend_control) and waits, at most a second, for its
 * answer; stateLock held (the wait lets it go). The X server moves its CPU locks and answers under its own lock,
 * so once PER_ACCESS is answered no buffer of it is CPU-locked outside that lock; every frame checks again, under
 * the lock, that the answer is still ours (rendererVkOwnedLocked).
 */
static bool rendererVkOwnership(uint8_t mode) {
    uint32_t ack = __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE), seq = LORIE_OWNERSHIP_SEQ(ack) + 1;
    struct timespec deadline;

    vkOwned = false;
    __atomic_store_n(&state->backend.ownershipRequest, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);
    clock_gettime(CLOCK_REALTIME, &deadline);
    rendererTimespecAddUs(&deadline, 1000000);
    while (LORIE_OWNERSHIP_SEQ(ack = __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE)) != seq)
        if (pthread_cond_timedwait(stateCond, &stateLock, &deadline) == ETIMEDOUT &&
            LORIE_OWNERSHIP_SEQ(ack = __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE)) != seq)
            break;
    if (LORIE_OWNERSHIP_SEQ(ack) != seq || LORIE_OWNERSHIP_MODE(ack) != mode) {
        loge("Xlorie: X server CPU ownership %u: %s", mode, LORIE_OWNERSHIP_SEQ(ack) != seq ? "not answered" : "refused");
        return false;
    }
    vkOwnershipSeq = seq;
    vkOwned = mode == LORIE_OWNERSHIP_PER_ACCESS;
    return true;
}

// Under the X server's lock: whether it still answers our PER_ACCESS request (it returns to PERSISTENT by
// itself when a connection is made or lost).
static bool rendererVkOwnedLocked(void) {
    return vkOwned && state &&
           __atomic_load_n(&state->backend.ownershipAck, __ATOMIC_ACQUIRE) == LORIE_OWNERSHIP_WORD(vkOwnershipSeq, LORIE_OWNERSHIP_PER_ACCESS);
}

_Static_assert(LORIE_VK_MAX_FRAME_IMAGES <= LORIE_GPU_IN_FLIGHT_MAX, "a frame's buffers must all fit in gpuInFlight");

/*
 * The X server's AHBs the frame just submitted uses (lorieVkFrameUse's list): listed in flight, as this process's,
 * under the X server's lock, before it is let go. From then until rendererVkFrameDone the X server may take the
 * lock, but CPU-locks none of them (lorie.h gpuInFlight).
 */
static void rendererVkInFlight(void) {
    uint32_t i;
    if (!vkr.usedCount)
        return;
    for (i = 0; i < vkr.usedCount; i++)
        state->gpuInFlight.ids[i] = vkr.used[i].img->id;
    state->gpuInFlight.count = vkr.usedCount;
    state->gpuInFlight.pid = getpid();
    __atomic_fetch_or(&state->gpuInFlight.busy, LORIE_GPU_IN_FLIGHT_MAIN, __ATOMIC_RELEASE);
}

// P11-C3-B2: ... those of the second submission (a root copy submitted ahead), the current one then, in its own list.
static void rendererVkInFlightAux(void) {
    uint32_t i;
    if (!vkr.usedCount)
        return;
    for (i = 0; i < vkr.usedCount; i++)
        state->gpuInFlight.auxIds[i] = vkr.used[i].img->id;
    state->gpuInFlight.auxCount = vkr.usedCount;
    state->gpuInFlight.pid = getpid();
    __atomic_fetch_or(&state->gpuInFlight.busy, LORIE_GPU_IN_FLIGHT_AUX, __ATOMIC_RELEASE);
}

// This process's submitted frame is over (lorieVkFrameWait's *over, or the device idle or lost since): its buffers
// are the X server's again. Without the X server's lock, which it may be holding while it waits for exactly this.
// A list another renderer process left is not ours to clear (lorieGpuWaitIdle clears it once that one is gone).
static void rendererVkListDone(uint32_t list) {
    if (state && state->gpuInFlight.pid == getpid() && (__atomic_fetch_and(&state->gpuInFlight.busy, ~list, __ATOMIC_RELEASE) & list))
        syscall(SYS_futex, &state->gpuInFlight.busy, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static void rendererVkFrameDone(void) {
    rendererVkListDone(LORIE_GPU_IN_FLIGHT_MAIN);
}

// P11-C3-B2: ... the second submission's.
static void rendererVkAuxDone(void) {
    rendererVkListDone(LORIE_GPU_IN_FLIGHT_AUX);
}

/*
 * Until the device is idle, or lost (Lost Device: for whether resources are in use, VK_ERROR_DEVICE_LOST is
 * equivalent to VK_SUCCESS). An out-of-memory answer establishes neither, and is asked again: the X server keeps
 * off the buffers of a frame not known to be over meanwhile.
 */
static void rendererVkIdle(void) {
    VkResult r;
    while ((r = vk.DeviceWaitIdle(vk.device)) != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) {
        loge("Xlorie: vkDeviceWaitIdle: %d, asked again", r);
        usleep(10000);
    }
}

static void rendererRootCopySettle(uint32_t seq, bool failed);
static void rendererVkAuxForget(void);

// Everything Vulkan made, gone once the device is idle (or lost); a frame left not over is over then.
static void rendererVkTeardown(void) {
    if (vk.device) {
        rendererVkIdle();
        rendererVkFrameDone();
        rendererVkAuxDone();
        lorieVkImagesClear(&vk, &vkImages);
        lorieVkRendererDestroy(&vk, &vkr);
        lorieVkDestroyDevice(&vk);
    }
    rendererVkAuxForget();
    vkReady = vkActive = vkOwned = false;
    rendererVkPacingReset();
}

/*
 * A failure that ends Vulkan for this session - the fallback was recorded where it happened, with its class and
 * reason. In Stage 1 a resource-local failure ends it too. The device is idle when this returns; the X server
 * is asked for its own ownership back and the window is given to GLES (auto) or to nobody (vulkan) by the next
 * rendererSelectBackend, which the main loop runs right away.
 */
static void rendererVkFail(void) {
    vkFailed = true;
    rendererVkTeardown();
    if (state) {
        state->surfaceAvailable = false;
        rendererPublishActive();
    }
    vkGiveBack = backendSelect = true;
}

/*
 * Device, self-test and renderer objects (once), the X server's PER_ACCESS ownership, the window's swapchain;
 * stateLock held. It is let go while the device and self-test are made (the full self-test takes seconds), so
 * that the activity's surface callbacks are not held up: they only set pendingWin, windowChanged and the like,
 * which the main loop takes up afterwards, and win - which this uses - stays valid until it does.
 */
static bool rendererVkStart(void) {
    if (!vkReady) {
        PFN_vkGetInstanceProcAddr gipa;
        bool made;
        pthread_mutex_unlock(&stateLock);
        made = (gipa = lorieVkLoader(state)) && lorieVkCreateDevice(&vk, gipa, state);
        if (made && (!lorieVkSelftest(&vk, state, state->backend.vkSelftestFull ? LORIE_VK_SELFTEST_ITERATIONS
                                                                                : LORIE_VK_SELFTEST_QUICK_ITERATIONS) ||
                     !lorieVkRendererCreate(&vk, &vkr, state))) {
            lorieVkDestroyDevice(&vk);
            made = false;
        }
        pthread_mutex_lock(&stateLock);
        if (!made)
            return false;
        vkReady = true;
    }
    // TERMUX_X11_RENDERER_VK_TRANSFER_COPY: its BGRA into RGBX copy checked on this device once, before any is made
    // (a device made again checks again); every session's diagnostics say how it went.
    if (state->backend.vkTransferCopy) {
        if (vk.transferCheck == LORIE_VK_XFER_CHECK_NONE) {
            pthread_mutex_unlock(&stateLock);
            lorieVkTransferCheck(&vk, state);
            pthread_mutex_lock(&stateLock);
        }
        state->diag.vkXferCheck = vk.transferCheck;
    }
    if (!vkOwned && !rendererVkOwnership(LORIE_OWNERSHIP_PER_ACCESS)) {
        lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_OWNERSHIP, VK_SUCCESS);
        return false;
    }
    vkr.smooth = smoothPresentationEnabled;
    if (!lorieVkSwapchainCreate(&vk, &vkr, win, state)) {
        lorieVkSwapchainDestroy(&vk, &vkr);
        return false;
    }
    return true;
}

// The present mode setting changed (GLES: rendererApplyPresentMode): the swapchain is made again before the next
// frame only if the mode it would be made with now is not the one it has.
static void rendererVkApplyPresentMode(void) {
    vkr.smooth = smoothPresentationEnabled;
    if (vkActive && vkr.swapchain && lorieVkPresentModeFor(&vk, &vkr, vkr.smooth) != vkr.presentMode)
        vkr.recreate = true;
}

/*
 * Gives the window to whoever rendererBackendFor says draws it now, and the X server the ownership that goes
 * with it; stateLock held. Runs after a window or session change and after a Vulkan failure.
 */
static void rendererSelectBackend(void) {
    int want;

    backendSelect = false;
    if (vkGiveBack && state && !rendererVkOwnership(LORIE_OWNERSHIP_PERSISTENT))
        loge("Xlorie: X server did not take its CPU ownership back; GLES works with either");
    vkGiveBack = false;
    if (!state || win == defaultWin)
        return;

    want = rendererBackendFor(state->backend.requested, vkFailed);
    if (want != RENDERER_USE_VULKAN && (vkReady || vkActive)) {
        // The session no longer asks for Vulkan (a new X server): ours goes, the X server's ownership comes back.
        rendererVkTeardown();
        if (!rendererVkOwnership(LORIE_OWNERSHIP_PERSISTENT))
            loge("Xlorie: X server did not take its CPU ownership back; GLES works with either");
    }
    if (want == RENDERER_USE_VULKAN) {
        if (vkActive && vkOwned)
            return;
        if (vkActive) {
            if (rendererVkOwnership(LORIE_OWNERSHIP_PER_ACCESS)) {
                state->drawRequested = state->cursor.updated = true;
                return;
            }
            lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_OWNERSHIP, VK_SUCCESS);
        } else {
            rendererDropEgl();
            if (rendererVkStart()) {
                vkActive = true;
                state->surfaceAvailable = state->drawRequested = state->cursor.updated = true;
                rendererPublishActive();
                log("Xlorie: Vulkan draws the window");
                return;
            }
        }
        vkFailed = true;
        rendererVkTeardown();
        if (!rendererVkOwnership(LORIE_OWNERSHIP_PERSISTENT))
            loge("Xlorie: X server did not take its CPU ownership back; GLES works with either");
        want = rendererBackendFor(state->backend.requested, true);
    }
    if (want == RENDERER_USE_GLES) {
        if (sfc == EGL_NO_SURFACE || sfc == defaultSfc)
            rendererUseEgl();
        return;
    }
    // A session that asked for Vulkan only, and Vulkan failed: nothing draws the window (explicit failure).
    rendererDropEgl();
    state->surfaceAvailable = false;
    rendererPublishActive();
    notifyGpuCopyDone();
    loge("Xlorie: Vulkan was asked for and failed: nothing draws the window");
}

/*
 * One rectangle of a GPU copy, as GLES's rendererApplyPendingGpuCopiesLocked draws it: destination pixels
 * (r.x1 + xOff .. r.x2 + xOff, r.y1 + yOff .. r.y2 + yOff) from source pixels (r.x1 .. r.x2, r.y1 .. r.y2), red and
 * blue swapped when `swizzle` (the two formats' orders differ: GLES's LorieBuffer_isRgba rule, vk.h rgbaOrder).
 * The clip-space values of GLES's vertices once drawRegion negated y: both put y = -1 on the buffer's first row
 * (Vulkan's framebuffer origin is its top row; GL's is its bottom row, a texture's first). tpresentcopy.c checks
 * this against GLES's own code.
 */
static struct lorie_vk_quad rendererVkCopyQuad(LorieGpuCopyRect r, int16_t xOff, int16_t yOff, const LorieBuffer_Desc *src,
                                               const LorieBuffer_Desc *dst, bool swizzle) {
    return (struct lorie_vk_quad) {
            .rect = { 2.f * (float) (r.x1 + xOff) / (float) dst->width - 1.f, 2.f * (float) (r.y1 + yOff) / (float) dst->height - 1.f,
                      2.f * (float) (r.x2 + xOff) / (float) dst->width - 1.f, 2.f * (float) (r.y2 + yOff) / (float) dst->height - 1.f },
            .uv = { (float) r.x1 / (float) src->width, (float) r.y1 / (float) src->height,
                    (float) r.x2 / (float) src->width, (float) r.y2 / (float) src->height },
            .flags = swizzle ? LORIE_VK_QUAD_SWIZZLE : 0 };
}

/*
 * TERMUX_X11_RENDERER_VK_TRANSFER_COPY's verdict on a copy, counted (lorie.h struct lorie_renderer_diag vkXfer*,
 * logged every 5 seconds as XlorieVkTransfer): what kept it from vkCmdCopyImage (`not`, bit n LORIE_VK_XFER_NOT_n),
 * why an image without the transfer usage has none, and the last copy kept from it as it was.
 */
static void rendererVkCountTransfer(const LorieGpuCopyEntry *entry, uint32_t n, const struct lorie_vk_image *s,
                                    const struct lorie_vk_image *d, uint32_t why) {
    volatile struct lorie_renderer_diag *diag = &state->diag;
    uint32_t i;

    LORIE_DIAG_ADD(state, vkXferChecked, 1);
    for (i = 0; i < LORIE_VK_XFER_NOT_COUNT; i++)
        if (why & 1u << i)
            LORIE_DIAG_ADD(state, vkXferNot[i], 1);
    if (why & 1u << LORIE_VK_XFER_NOT_SRC_USAGE)
        LORIE_DIAG_ADD(state, vkXferUsageMissing[s->optionalVerdict % LORIE_VK_XFER_USAGE_COUNT], 1);
    if (why & 1u << LORIE_VK_XFER_NOT_DST_USAGE)
        LORIE_DIAG_ADD(state, vkXferUsageMissing[d->optionalVerdict % LORIE_VK_XFER_USAGE_COUNT], 1);
    if (!why)
        return;
    diag->vkXferLastNot = why;
    diag->vkXferLastSrcFormat = s->ahbFormat;
    diag->vkXferLastDstFormat = d->ahbFormat;
    diag->vkXferLastSrcUsage = s->usage;
    diag->vkXferLastDstUsage = d->usage;
    diag->vkXferLastSrcAhbUsage = s->ahbUsage;
    diag->vkXferLastDstAhbUsage = d->ahbUsage;
    diag->vkXferLastSrcWidth = s->width;
    diag->vkXferLastSrcHeight = s->height;
    diag->vkXferLastDstWidth = d->width;
    diag->vkXferLastDstHeight = d->height;
    diag->vkXferLastDstDepth = entry->dstDepth;
    diag->vkXferLastRects = n;
    diag->vkXferLastRect[0] = n ? entry->rects[0].x1 : 0;
    diag->vkXferLastRect[1] = n ? entry->rects[0].y1 : 0;
    diag->vkXferLastRect[2] = n ? entry->rects[0].x2 : 0;
    diag->vkXferLastRect[3] = n ? entry->rects[0].y2 : 0;
    diag->vkXferLastOff[0] = entry->xOff;
    diag->vkXferLastOff[1] = entry->yOff;
}

/*
 * The one copy between two formats made by vkCmdCopyImage: a BGRA AHB (B8G8R8A8_UNORM) into an RGBX AHB
 * (R8G8B8A8_UNORM) of depth 24, once this device copied it byte-exact (lorieVkTransferCheck). The draw reads the BGRA
 * texel, swaps its red and blue (rendererVkCopyQuad's swizzle: the two byte orders differ) and writes it as RGBA:
 * every byte where the source has it, the alpha in the 4th, X padding at depth 24 - the raw copy's bytes
 * (lorieVkCopyFormats), as the X server's own CopyArea copies them.
 */
static bool rendererVkCopyBgraIntoRgbx(const LorieGpuCopyEntry *entry, const struct lorie_vk_image *s, const struct lorie_vk_image *d) {
    return vk.transferCheck == LORIE_VK_XFER_CHECK_PASS && entry->dstDepth == 24 &&
           s->ahbFormat == AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM && s->format == VK_FORMAT_B8G8R8A8_UNORM &&
           d->ahbFormat == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM && d->format == VK_FORMAT_R8G8B8A8_UNORM;
}

/*
 * Experimental, TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1: an entry's rectangles as vkCmdCopyImage regions, where the
 * copy gives exactly the pixels rendererVkCopyQuad's draws give - one VkFormat and one byte order (no swizzle), or
 * BGRA into RGBX of depth 24 (rendererVkCopyBgraIntoRgbx: the swizzle gives the raw bytes there); an
 * RGBX source, whose 4th byte the draw reads as 1, only into an RGBX destination below depth 32 (that byte X
 * padding there, read by no one: the copy leaves the source's byte as the X server's own CopyArea would, the draw
 * 255); both images made with the transfer usage (lorieVkImport gives it only where the device allows it); each
 * rectangle a real one inside the source (a draw past the source samples its edge); the destination clipped to the
 * destination, as the draw's render area clips it; no two destinations overlapping (the regions of one copy are
 * written in no order). One pixel per texel: no filter changes a draw's pixel. The number of regions, 0: the entry
 * is drawn.
 */
static uint32_t rendererVkCopyRegions(const LorieGpuCopyEntry *entry, uint32_t n, const struct lorie_vk_image *s,
                                      const struct lorie_vk_image *d, VkImageCopy *regions) {
    uint32_t k, j, count = 0, why = 0;

    if (!state->backend.vkTransferCopy)
        return 0;
    // Every condition is looked at, each one that fails recorded (rendererVkCountTransfer): the copy is made only
    // when none does.
    if (s == d)
        why |= 1u << LORIE_VK_XFER_NOT_SAME;
    if ((s->format != d->format || s->rgbaOrder != d->rgbaOrder) && !rendererVkCopyBgraIntoRgbx(entry, s, d))
        why |= 1u << LORIE_VK_XFER_NOT_FORMAT;
    if (s->rgbx && !d->rgbx)
        why |= 1u << LORIE_VK_XFER_NOT_RGBX_ALPHA;
    if (s->rgbx && d->rgbx && entry->dstDepth >= 32)
        why |= 1u << LORIE_VK_XFER_NOT_DEPTH32;
    if (!(s->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        why |= 1u << LORIE_VK_XFER_NOT_SRC_USAGE;
    if (!(d->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        why |= 1u << LORIE_VK_XFER_NOT_DST_USAGE;
    for (k = 0; k < n; k++) {
        LorieGpuCopyRect r = entry->rects[k];
        int32_t sx = r.x1, sy = r.y1, w = r.x2 - r.x1, h = r.y2 - r.y1, dx = sx + entry->xOff, dy = sy + entry->yOff;

        if (w <= 0 || h <= 0 || sx < 0 || sy < 0 || r.x2 > (int32_t) s->width || r.y2 > (int32_t) s->height) {
            why |= 1u << LORIE_VK_XFER_NOT_RECT_SOURCE;
            break;
        }
        if (dx < 0)
            sx -= dx, w += dx, dx = 0;
        if (dy < 0)
            sy -= dy, h += dy, dy = 0;
        if (w > (int32_t) d->width - dx)
            w = (int32_t) d->width - dx;
        if (h > (int32_t) d->height - dy)
            h = (int32_t) d->height - dy;
        if (w <= 0 || h <= 0)
            continue;       // all of it outside the destination: the draw draws no pixel of it either
        for (j = 0; j < count; j++)
            if (dx < regions[j].dstOffset.x + (int32_t) regions[j].extent.width && regions[j].dstOffset.x < dx + w &&
                dy < regions[j].dstOffset.y + (int32_t) regions[j].extent.height && regions[j].dstOffset.y < dy + h)
                break;
        if (j < count) {
            why |= 1u << LORIE_VK_XFER_NOT_RECT_OVERLAP;
            break;
        }
        regions[count++] = (VkImageCopy) {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { sx, sy, 0 },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffset = { dx, dy, 0 },
                .extent = { (uint32_t) w, (uint32_t) h, 1 } };
    }
    if (k == n && !count)
        why |= 1u << LORIE_VK_XFER_NOT_RECT_NONE;
    rendererVkCountTransfer(entry, n, s, d, why);
    return why ? 0 : count;
}

/*
 * The pending GPU copies recorded into the frame (lorieVkFrameCopy), as GLES's rendererApplyPendingGpuCopiesLocked
 * does them: the same rectangles, offsets and swizzle rule - or, with TERMUX_X11_RENDERER_VK_TRANSFER_COPY, by
 * vkCmdCopyImage where that gives the same pixels (rendererVkCopyRegions; lorieVkFrameCopyImage). The queue is
 * not advanced here: *readIndex and *serial are what the caller publishes once the frame's fence signalled. False
 * on a buffer Vulkan cannot use (recorded as RESOURCE_LOCAL).
 */
// A frame another renderer process listed is over first (lorieGpuWaitIdle; holding the X server's lock). This
// process's own are its to wait for: none is running here but, P11-C3-B2, a root copy submitted ahead.
static void rendererVkWaitOthers(void) {
    if (state->gpuInFlight.pid != getpid())
        lorieGpuWaitIdle(state, true, 0);
}

// A root copy (rendererVkRootCopy, rendererVkRootCopyAhead): what was asked for, and its times (P11-C3).
struct rendererVkRootWork {
    uint32_t seq, n;
    uint64_t srcId, dstId, bytes, queuedUs, startUs, takenUs, submittedUs;
    bool diag;
};

/*
 * P11-C3-B2 (TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1): a root copy submitted ahead of the frame this loop pass draws,
 * in the second submission (vk.h lorie_vk_slot): `pending` until its fence was waited for and it was settled;
 * `chained` while the chain semaphore it signalled has not been waited for - by that frame's submission, or by an
 * empty one (rendererVkAuxSettle).
 */
static struct { struct rendererVkRootWork work; bool pending, chained; } vkAux;

static bool rendererVkCopiesLocked(uint64_t *serial, uint32_t *readIndex) {
    uint32_t i = state->gpuCopyQueue.readIndex, end = __atomic_load_n(&state->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);
    int filter = filtering == GL_LINEAR;

    // The transfer usage the experimental copies need, asked for every image this X server session imports.
    vkImages.optionalUsage = state->backend.vkTransferCopy ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;

    for (; i != end; i++) {
        LorieGpuCopyEntry entry = state->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY];
        LorieBuffer *src, *dst;

        if (entry.cancelled) {
            // Its presentation let go before it was recorded (InitOutput.c lorieGpuCopySettle): not copied, completed.
            *serial = entry.serial;
            continue;
        }
        if (rendererCopyDeferred(&entry, i))
            break;      // P11-C3-B1: left queued, with the copies after it
        // P11-C3-B2: nor a copy using a buffer of the root copy submitted ahead, still running: for the next pass
        if (vkAux.pending && (entry.srcBufferId == vkAux.work.srcId || entry.srcBufferId == vkAux.work.dstId ||
                              entry.dstBufferId == vkAux.work.srcId || entry.dstBufferId == vkAux.work.dstId))
            break;
        src = rendererFindBufferWithRetry(entry.srcBufferId);
        dst = rendererFindBufferWithRetry(entry.dstBufferId);
        if (!src || !dst)
            log("rendererVkCopies: buffer %llu not found after waiting, skipping\n",
                (unsigned long long) (!src ? entry.srcBufferId : entry.dstBufferId));
        if (src && dst) {
            const LorieBuffer_Desc *sd = LorieBuffer_description(src), *dd = LorieBuffer_description(dst);
            struct lorie_vk_image *s = lorieVkImport(&vk, &vkImages, sd->id, sd->buffer, 0, state);
            struct lorie_vk_image *d = s ? lorieVkImport(&vk, &vkImages, dd->id, dd->buffer, 0, state) : NULL;
            struct lorie_vk_quad quads[LORIE_GPU_COPY_MAX_RECTS];
            VkImageCopy regions[LORIE_GPU_COPY_MAX_RECTS];
            uint32_t n = entry.numRects < LORIE_GPU_COPY_MAX_RECTS ? entry.numRects : LORIE_GPU_COPY_MAX_RECTS, k, count;

            if (!s || !d)
                return false;
            if (!d->attachView) {
                lorieVkFallback(state, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED, VK_SUCCESS);
                return false;
            }
            count = rendererVkCopyRegions(&entry, n, s, d, regions);
            if (!count || !lorieVkFrameCopyImage(&vk, &vkr, s, d, regions, count)) {
                for (k = 0; k < n; k++)
                    quads[k] = rendererVkCopyQuad(entry.rects[k], entry.xOff, entry.yOff, sd, dd, s->rgbaOrder != d->rgbaOrder);
                if (!lorieVkFrameCopy(&vk, &vkr, s, d, quads, n, filter)) {
                    lorieVkFallback(state, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_IMPORT, VK_SUCCESS);
                    return false;
                }
            }
        }
        *serial = entry.serial;
    }
    *readIndex = i;
    return true;
}

/*
 * P9-B0: when the entries a submission recorded (readIndex up to `readIndex`) were queued - taken from the queue
 * before readIndex is published (the X server reuses their slots after). Their number.
 */
static uint32_t rendererVkQueuedTimes(uint32_t readIndex, uint64_t *queued) {
    uint32_t i, n = 0;
    for (i = state->gpuCopyQueue.readIndex; i != readIndex && n < LORIE_GPU_COPY_QUEUE_CAPACITY; i++)
        queued[n++] = state->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY].queuedUs;
    return n;
}

/*
 * P9-B0: those `n` entries counted once their completedSerial was published at `publishedUs` (after it: nothing
 * of the publish waits for this). Per entry: queued to `startUs` (the submission begun: the lock asked for) and
 * queued to `publishedUs`; per submission, weighted by its entries: the lock's wait, the wait under it for a frame
 * listed in flight (`inflightUs`), the rest up to the submission (the recording), the submission to its fence
 * waited for - a frame's (the root and cursor too) counted apart. Nothing without entries.
 */
static void rendererVkCountCopies(const uint64_t *queued, uint32_t n, bool frame, uint64_t startUs, uint64_t lockedUs,
                                  uint64_t inflightUs, uint64_t submittedUs, uint64_t fencedUs, uint64_t publishedUs) {
    uint32_t i;

    if (!n)
        return;
    for (i = 0; i < n; i++) {
        lorieLatencyAdd(&state->diag.copyQueue, startUs > queued[i] ? startUs - queued[i] : 0);
        lorieLatencyAdd(&state->diag.copyTotal, publishedUs > queued[i] ? publishedUs - queued[i] : 0);
    }
    lorieLatencyAddWeighted(&state->diag.copyLock, lockedUs - startUs, n);
    lorieLatencyAddWeighted(&state->diag.copyInflightWait, inflightUs, n);
    lorieLatencyAddWeighted(&state->diag.copyRecord, submittedUs - lockedUs - inflightUs, n);
    lorieLatencyAddWeighted(frame ? &state->diag.copyFrameGpuWait : &state->diag.copyGpuWait, fencedUs - submittedUs, n);
    if (frame) {
        LORIE_DIAG_ADD(state, copyFrameEntries, n);
        LORIE_DIAG_ADD(state, copyFrameSubmits, 1);
    } else {
        LORIE_DIAG_ADD(state, copyStandaloneEntries, n);
        LORIE_DIAG_ADD(state, copyStandaloneSubmits, 1);
    }
}

/* ---- P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG: the screen frames' timings (lorie.h struct lorie_hist) ---- */

// Gaps between frames longer than this: nothing asked to be drawn (an idle desktop), not a slow frame.
#define LORIE_VK_IDLE_GAP_US 250000

// A root copy's time in this loop pass, ahead of the frame that may follow it (rendererVkRootCopy).
static uint64_t vkRootCopyBusyUs;

/*
 * One interval between two of the renderer's events, against the last (`*last`, 0: none yet), sampled unless it is the
 * first, its stamp is before the last, or it is a gap over LORIE_VK_IDLE_GAP_US; which of these, and the interval in *us
 * when sampled.
 */
enum { LORIE_VK_INTERVAL_SAMPLED, LORIE_VK_INTERVAL_FIRST, LORIE_VK_INTERVAL_INVALID, LORIE_VK_INTERVAL_IDLE };
static int rendererVkInterval(volatile struct lorie_hist *h, uint64_t *last, uint64_t now, uint64_t *us) {
    uint64_t prev = *last;
    *last = now;
    if (!prev)
        return LORIE_VK_INTERVAL_FIRST;
    if (now < prev)
        return LORIE_VK_INTERVAL_INVALID;
    if (now - prev > LORIE_VK_IDLE_GAP_US)
        return LORIE_VK_INTERVAL_IDLE;
    *us = now - prev;
    lorieHistAdd(h, *us);
    return LORIE_VK_INTERVAL_SAMPLED;
}

// A screen frame's stamps (lorieMonotonicUs) and the durations taken around it; 0: not reached (no present call).
struct rendererVkFrameTimes {
    uint64_t startUs, wantedUs, acquireUs, lockStartUs, lockedUs, inflightUs, submitStartUs, submittedUs, fenceUs,
             presentStartUs, presentEndUs, endUs, afterRootUs;
};

/*
 * A frame presented: its samples, and, should its start make the period's largest interval, what it spent; a start
 * before the last one's, or long after it, counted once (the present call's interval goes the same way). Its wake only
 * for a tick's request made after the last frame started (an older one that frame took up already) and before this
 * one did.
 */
static void rendererVkFrameTimed(const struct rendererVkFrameTimes *t) {
    volatile struct lorie_renderer_diag *d = &state->diag;
    uint64_t interval = 0, presentInterval = 0, wake = 0, lastStartUs;
    int sampled;

    if (vkPacingSwapchain != vkr.swapchain) {
        rendererVkPacingReset();
        vkPacingSwapchain = vkr.swapchain;
    }
    lastStartUs = vkPacingLastStartUs;
    sampled = rendererVkInterval(&d->vkFrameStart, &vkPacingLastStartUs, t->startUs, &interval);
    if (sampled == LORIE_VK_INTERVAL_INVALID)
        LORIE_DIAG_ADD(state, vkFrameStampsInvalid, 1);
    else if (sampled == LORIE_VK_INTERVAL_IDLE)
        LORIE_DIAG_ADD(state, vkFrameIdleGaps, 1);
    if (t->presentStartUs)
        rendererVkInterval(&d->vkPresentDelta, &vkPacingLastPresentUs, t->presentStartUs, &presentInterval);
    if (t->wantedUs && t->wantedUs > lastStartUs && t->wantedUs <= t->startUs && t->startUs - t->wantedUs < 1000000)
        lorieHistAdd(&d->vkFrameWake, wake = t->startUs - t->wantedUs);
    lorieHistAdd(&d->vkFrameCpu, t->endUs - t->startUs);
    lorieHistAdd(&d->vkFrameAcquire, t->acquireUs);
    lorieHistAdd(&d->vkFrameLock, t->lockedUs - t->lockStartUs);
    lorieHistAdd(&d->vkFrameRecord, t->submitStartUs - t->lockedUs - t->inflightUs);
    lorieHistAdd(&d->vkFrameSubmit, t->submittedUs - t->submitStartUs);
    lorieHistAdd(&d->vkFrameFence, t->fenceUs);
    if (t->presentStartUs)
        lorieHistAdd(&d->vkFramePresent, t->presentEndUs - t->presentStartUs);
    if (t->afterRootUs)
        lorieHistAdd(&d->vkFrameAfterRoot, t->afterRootUs);
    if (sampled == LORIE_VK_INTERVAL_SAMPLED && interval > d->vkWorstStartUs) {
        uint64_t lastEndUs = vkPacingLast.endUs;
        d->vkWorstStartUs = (uint32_t) interval;
        d->vkWorstWakeUs = (uint32_t) wake;
        d->vkWorstAcquireUs = (uint32_t) t->acquireUs;
        d->vkWorstLockUs = (uint32_t) (t->lockedUs - t->lockStartUs);
        d->vkWorstFenceUs = (uint32_t) t->fenceUs;
        d->vkWorstPresentUs = (uint32_t) (t->presentStartUs ? t->presentEndUs - t->presentStartUs : 0);
        d->vkWorstAfterRootUs = (uint32_t) t->afterRootUs;
        // the frame that began it, and the gap after it (its end on the same clock, before this start)
        d->vkWorstPrevCpuUs = (uint32_t) vkPacingLast.cpuUs;
        d->vkWorstPrevLockUs = (uint32_t) vkPacingLast.lockUs;
        d->vkWorstPrevFenceUs = (uint32_t) vkPacingLast.fenceUs;
        d->vkWorstPrevAfterRootUs = (uint32_t) vkPacingLast.afterRootUs;
        d->vkWorstGapUs = (uint32_t) (lastEndUs && lastEndUs <= t->startUs ? t->startUs - lastEndUs : 0);
        d->vkWorstUnaskedUs = (uint32_t) (lastEndUs && t->wantedUs > lastEndUs && t->wantedUs <= t->startUs ? t->wantedUs - lastEndUs : 0);
    }
    vkPacingLast.endUs = t->endUs;
    vkPacingLast.cpuUs = t->endUs - t->startUs;
    vkPacingLast.lockUs = t->lockedUs - t->lockStartUs;
    vkPacingLast.fenceUs = t->fenceUs;
    vkPacingLast.afterRootUs = t->afterRootUs;
}

// The GPU copies alone, when no frame is drawn (rendererApplyPendingGpuCopies's Vulkan twin).
static void rendererVkApplyPendingGpuCopies(void) {
    uint64_t serial = 0, startUs, lockedUs, inflightUs, submittedUs = 0, fencedUs = 0;
    uint32_t readIndex = 0;
    VkResult res;

    if (!state || state->gpuCopyQueue.readIndex == state->gpuCopyQueue.writeIndex)
        return;
    startUs = lorieMonotonicUs();
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    lockedUs = lorieMonotonicUs();
    if (!rendererVkOwnedLocked()) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        vkOwned = false;
        backendSelect = true;   // asked for again first
        return;
    }
    inflightUs = lorieMonotonicUs();
    rendererVkWaitOthers();
    inflightUs = lorieMonotonicUs() - inflightUs;
    if (lorieVkFrameBegin(&vk, &vkr) != VK_SUCCESS) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SUBMIT, VK_ERROR_INITIALIZATION_FAILED);
        return rendererVkFail();
    }
    if (!rendererVkCopiesLocked(&serial, &readIndex)) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        return rendererVkFail();
    }
    if (readIndex == state->gpuCopyQueue.readIndex) {
        // P11-C3-B1: the first one left queued for a reservation - nothing recorded, nothing submitted
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        return;
    }
    res = lorieVkFrameQueue(&vk, &vkr, 0, false, state);
    submittedUs = lorieMonotonicUs();
    if (res == VK_SUCCESS)
        rendererVkInFlight();
    lorie_mutex_unlock(&state->lock, &state->lockingPid);

    if (res == VK_SUCCESS) {
        // The device runs the copies; the X server goes on, without their buffers (rendererVkInFlight).
        bool over;
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
        if (res == VK_SUCCESS && state->backend.vkFrameDiag)
            lorieHistAdd(&state->diag.vkFenceCopy, vkr.fenceWaitUs);    // P11-C3: these copies' own fence
    }
    if (res == VK_SUCCESS) {
        // Only now that the device finished (and gave the buffers back) may the X server reuse the source.
        uint64_t queued[LORIE_GPU_COPY_QUEUE_CAPACITY], publishedUs;
        uint32_t n = rendererVkQueuedTimes(readIndex, queued);
        state->gpuCopyQueue.readIndex = readIndex;
        publishedUs = lorieMonotonicUs();
        if (serial) {
            __atomic_store_n(&state->gpuCopyQueue.completedUs, publishedUs, __ATOMIC_RELAXED);
            __atomic_store_n(&state->gpuCopyQueue.completedSerial, serial, __ATOMIC_RELEASE);
            notifyGpuCopyPublished(publishedUs);
        }
        rendererVkCountCopies(queued, n, false, startUs, lockedUs, inflightUs, submittedUs, fencedUs, publishedUs);
    }
    if (res != VK_SUCCESS)
        rendererVkFail();
}

/* ---- P11-C2: root double buffering's copy by the GPU (lorie.h rootCopy) ---- */

// A root copy the X server asked for that no renderer took, settled or saw taken back yet (read without the lock: a hint).
static bool rendererRootCopyPending(void) {
    uint32_t seq = state ? __atomic_load_n(&state->rootCopy.seq, __ATOMIC_ACQUIRE) : 0;
    return seq && seq != state->rootCopy.taken && seq != state->rootCopy.cancelled && seq != state->rootCopy.done;
}

// Request `seq` settled - copied, or (failed) not, which the X server then copies itself - and the X server woken.
static void rendererRootCopySettle(uint32_t seq, bool failed) {
    if (failed)
        __atomic_store_n(&state->rootCopy.failed, seq, __ATOMIC_RELAXED);
    if (state->backend.vkFrameDiag)
        state->rootCopy.doneUs = lorieMonotonicUs();    // P11-C3: for the X server's time to go on (rootResume)
    __atomic_store_n(&state->rootCopy.done, seq, __ATOMIC_RELEASE);
    syscall(SYS_futex, &state->rootCopy.done, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

// Vulkan not running: the request settled as not copied, under the lock (the X server takes back the ones not taken).
static void rendererRootCopyDecline(void) {
    uint32_t seq;
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    seq = state->rootCopy.seq;
    if (seq && seq != state->rootCopy.taken && seq != state->rootCopy.cancelled) {
        state->rootCopy.taken = seq;
        state->rootCopy.pid = getpid();
        rendererRootCopySettle(seq, true);
    }
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
}

/*
 * The root's damage copied from the buffer the X server drew into (the front now) into the one it draws into now
 * (InitOutput.c lorieRootSwap), as GPU copies are drawn (lorieVkFrameCopy: imported AHBs acquired from and released
 * to the foreign owner), ahead of everything else the loop does and alone in its submission: the X server may be
 * waiting for it before it can draw again. Taken under the X server's lock, its two buffers listed in flight; once
 * its fence signalled - and only then - settled as copied. Settled as not copied when it cannot be: no PER_ACCESS
 * answer, a buffer the renderer does not have (a root gone), or Vulkan failing - the X server copies it then, once
 * no frame uses the buffers (lorieGpuWaitIdle).
 */
/*
 * The root copy asked for, taken under the X server's lock (`taken`), recorded and submitted - its two buffers listed
 * in flight - and the lock let go: true. Otherwise settled as not copied, false (a failure ends Vulkan too). P11-C3-B2,
 * `ahead`: in the second submission, signalling the chain semaphore.
 */
static bool rendererVkRootCopySubmit(struct rendererVkRootWork *w, bool ahead) {
    struct lorie_vk_quad quads[LORIE_GPU_COPY_MAX_RECTS];
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
    struct lorie_vk_image *s = NULL, *d = NULL;
    LorieBuffer *src, *dst;
    uint32_t i;
    VkResult res;

    lorie_mutex_lock(&state->lock, &state->lockingPid);
    w->seq = state->rootCopy.seq;
    if (!w->seq || w->seq == state->rootCopy.taken || w->seq == state->rootCopy.cancelled) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        return false;
    }
    state->rootCopy.taken = w->seq;
    state->rootCopy.pid = getpid();
    if (w->diag)
        w->takenUs = lorieMonotonicUs();
    if (!rendererVkOwnedLocked()) {
        // Without the X server's PER_ACCESS answer no buffer of it is touched; the answer asked for again.
        rendererRootCopySettle(w->seq, true);
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        vkOwned = false;
        backendSelect = true;
        return false;
    }
    rendererVkWaitOthers();
    w->n = state->rootCopy.numRects < LORIE_GPU_COPY_MAX_RECTS ? state->rootCopy.numRects : LORIE_GPU_COPY_MAX_RECTS;
    memcpy(rects, (const void *) state->rootCopy.rects, sizeof(rects[0]) * w->n);
    w->queuedUs = state->rootCopy.queuedUs;
    w->srcId = state->rootCopy.srcId;
    w->dstId = state->rootCopy.dstId;
    src = rendererFindBufferWithRetry(w->srcId);
    dst = rendererFindBufferWithRetry(w->dstId);
    // P11-C3-B1: nor a buffer the X server holds reserved for a CPU access - it reserves neither of these (it waits
    // for the copy before it accesses the one copied into; the other is the root it publishes): not copied, then
    if (!src || !dst || lorieCpuReserved(state, w->srcId) || lorieCpuReserved(state, w->dstId)) {
        rendererRootCopySettle(w->seq, true);
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        return false;
    }
    if (ahead)
        lorieVkSlotSwap(&vkr);
    if (!(s = lorieVkImport(&vk, &vkImages, LorieBuffer_description(src)->id, LorieBuffer_description(src)->buffer, 0, state)) ||
        !(d = lorieVkImport(&vk, &vkImages, LorieBuffer_description(dst)->id, LorieBuffer_description(dst)->buffer, 0, state)) ||
        !d->attachView || lorieVkFrameBegin(&vk, &vkr) != VK_SUCCESS) {
        if (d && !d->attachView)
            lorieVkFallback(state, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_IMAGE_USAGE_UNSUPPORTED, VK_SUCCESS);
        else if (s && d)
            lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SUBMIT, VK_ERROR_INITIALIZATION_FAILED);
        if (ahead)
            lorieVkSlotSwap(&vkr);
        rendererRootCopySettle(w->seq, true);
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        rendererVkFail();
        return false;
    }
    for (i = 0; i < w->n; i++) {
        quads[i] = rendererVkCopyQuad(rects[i], 0, 0, LorieBuffer_description(src), LorieBuffer_description(dst),
                                      s->rgbaOrder != d->rgbaOrder);
        w->bytes += (uint64_t) (rects[i].x2 - rects[i].x1) * (uint64_t) (rects[i].y2 - rects[i].y1) * 4;
    }
    if (!lorieVkFrameCopy(&vk, &vkr, s, d, quads, w->n, 0)) {
        lorieVkFallback(state, LORIE_FALLBACK_CLASS_RESOURCE_LOCAL, LORIE_FALLBACK_AHB_IMPORT, VK_SUCCESS);
        if (ahead)
            lorieVkSlotSwap(&vkr);
        rendererRootCopySettle(w->seq, true);
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        rendererVkFail();
        return false;
    }
    vkr.chainSignal = ahead;
    res = lorieVkFrameQueue(&vk, &vkr, 0, false, state);
    if (res == VK_SUCCESS) {
        if (ahead)
            rendererVkInFlightAux();
        else
            rendererVkInFlight();
        LORIE_DIAG_ADD(state, rootGpuCopySubmits, 1);
        if (w->diag)
            w->submittedUs = lorieMonotonicUs();
    }
    if (ahead)
        lorieVkSlotSwap(&vkr);
    lorie_mutex_unlock(&state->lock, &state->lockingPid);
    if (res != VK_SUCCESS) {
        rendererRootCopySettle(w->seq, true);
        rendererVkFail();
        return false;
    }
    return true;
}

/*
 * ... its fence waited for (`ahead`: the second submission's, swapped in for it), its buffers out of flight once it
 * is over, counted, settled - as copied only after its fence signalled. A failure ends Vulkan.
 */
static void rendererVkRootCopyEnd(struct rendererVkRootWork *w, bool ahead) {
    uint64_t fenceUs;
    bool over;
    VkResult res;

    // The device copies; the X server waits for it only once it touches the buffer it draws into now.
    if (ahead)
        lorieVkSlotSwap(&vkr);
    res = lorieVkFrameWait(&vk, &vkr, state, &over);
    fenceUs = vkr.fenceWaitUs;
    if (ahead)
        lorieVkSlotSwap(&vkr);
    if (over) {
        // otherwise once rendererVkFail's teardown found the device idle or lost
        if (ahead)
            rendererVkAuxDone();
        else
            rendererVkFrameDone();
    }
    if (res == VK_SUCCESS) {
        uint64_t now = lorieMonotonicUs();
        LORIE_DIAG_ADD(state, rootGpuCopyCompletions, 1);
        LORIE_DIAG_ADD(state, rootGpuCopyRects, w->n);
        LORIE_DIAG_ADD(state, rootGpuCopyBytes, w->bytes);
        LORIE_DIAG_ADD(state, rootGpuCopyCompletionUs, now > w->queuedUs ? now - w->queuedUs : 0);
        if (w->diag) {
            // P11-C3: its parts - waiting to be taken, recorded and submitted, its own fence - and the period's slowest
            volatile struct lorie_renderer_diag *dg = &state->diag;
            uint64_t take = w->takenUs > w->queuedUs ? w->takenUs - w->queuedUs : 0, total = now > w->queuedUs ? now - w->queuedUs : 0;
            lorieHistAdd(&dg->rootTake, take);
            lorieHistAdd(&dg->rootRecord, w->submittedUs - w->takenUs);
            lorieHistAdd(&dg->vkFenceRoot, fenceUs);
            if (total > dg->rootWorstUs) {
                dg->rootWorstUs = (uint32_t) total;
                dg->rootWorstTakeUs = (uint32_t) take;
                dg->rootWorstRecordUs = (uint32_t) (w->submittedUs - w->takenUs);
                dg->rootWorstFenceUs = (uint32_t) fenceUs;
            }
        }
    }
    rendererRootCopySettle(w->seq, res != VK_SUCCESS);
    if (res != VK_SUCCESS)
        rendererVkFail();
}

static void rendererVkRootCopy(void) {
    struct rendererVkRootWork w = { .diag = state->backend.vkFrameDiag };
    w.startUs = w.diag ? lorieMonotonicUs() : 0;
    if (!rendererVkRootCopySubmit(&w, false))
        return;
    rendererVkRootCopyEnd(&w, false);
    if (w.diag)
        vkRootCopyBusyUs = lorieMonotonicUs() - w.startUs;  // ahead of the frame this loop pass may draw next
}

/*
 * P11-C3-B2 (TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1): the root copy submitted ahead of the frame this loop pass draws -
 * taken as early as rendererVkRootCopy takes it - and not waited for here: that frame is recorded meanwhile, and
 * waits for it on the device (the chain semaphore), not on the CPU; rendererVkAuxFinish waits for its fence once the
 * frame is submitted. Made the old way should the second submission's objects not be there.
 */
static void rendererVkRootCopyAhead(void) {
    struct rendererVkRootWork w = { .diag = state->backend.vkFrameDiag };
    if (!lorieVkAuxCreate(&vk, &vkr)) {
        LORIE_DIAG_ADD(state, rootAheadUnavailable, 1);
        rendererVkRootCopy();
        return;
    }
    w.startUs = w.diag ? lorieMonotonicUs() : 0;
    if (!rendererVkRootCopySubmit(&w, true))
        return;
    vkAux.work = w;
    vkAux.pending = vkAux.chained = true;
    LORIE_DIAG_ADD(state, rootAhead, 1);
    if (w.diag)
        vkRootCopyBusyUs = lorieMonotonicUs() - w.startUs;  // all the frame waits for it on the CPU now
}

// P11-C3-B2: the root copy submitted ahead waited for and settled.
static void rendererVkAuxFinish(void) {
    if (!vkAux.pending)
        return;
    vkAux.pending = false;
    rendererVkRootCopyEnd(&vkAux.work, true);
}

// P11-C3-B2: ... and, should no frame have waited for its chain semaphore, an empty submission does (lorieVkChainConsume).
static void rendererVkAuxSettle(void) {
    rendererVkAuxFinish();
    if (vkAux.chained && vkActive) {
        vkAux.chained = false;
        vkr.chainWait = true;
        LORIE_DIAG_ADD(state, rootChainConsumed, 1);
        if (lorieVkChainConsume(&vk, &vkr, state) != VK_SUCCESS)
            rendererVkFail();
    }
    vkAux.chained = false;
}

// P11-C3-B2, the device idle or lost and every list over (teardown): a root copy submitted ahead, not settled, settled
// as not copied - the X server copies it itself.
static void rendererVkAuxForget(void) {
    if (vkAux.pending && state)
        rendererRootCopySettle(vkAux.work.seq, true);
    vkAux.pending = vkAux.chained = false;
}

// A black frame (no X server buffer used), as GLES clears the window when the X server goes away.
static void rendererVkClear(void) {
    uint32_t index;
    if (lorieVkAcquire(&vk, &vkr, &index, NULL) != LORIE_VK_FRAME_OK)
        return;
    if (lorieVkFrameBegin(&vk, &vkr) == VK_SUCCESS) {
        lorieVkFrameScreen(&vk, &vkr, index, NULL, &(VkRect2D) { { 0, 0 }, vkr.extent }, NULL, NULL, 0);
        if (lorieVkFrameSubmit(&vk, &vkr, index, true, NULL) == VK_SUCCESS)
            lorieVkPresent(&vk, &vkr, index, NULL);
    }
}

/*
 * A frame, as rendererRedrawLocked draws it with GLES: the same root buffer and size checks, the pending copies
 * and the root in one submission, recorded and submitted under the X server's lock. The lock is let go once the
 * frame is submitted, its buffers listed in flight (rendererVkInFlight): while the device runs it the X server
 * may take the lock, but CPU-locks none of them; its fence is waited for, and the buffers given back
 * (rendererVkFrameDone), before the completed serial is published. The swapchain image is acquired before taking
 * the lock, presented after the fence. None of GLES's swap heuristics (backpressure guard, high-refresh limiter, DeX present skip,
 * coalescing) are fed from here: Vulkan has its own timings (vk_* in the diagnostics).
 *
 * P11-C1 (TERMUX_X11_RENDERER_VK_SUBMIT_OPT=1), still one submission in flight at a time: the copies queued go ahead
 * in a submission of their own in every present mode, as under FIFO - published at their own fence, waiting neither
 * for the image nor for the screen's draw; and a frame carrying none of the X server's copies is presented as soon
 * as it is submitted - the presentation engine waits for it on the GPU (rendered[index]) - while this thread waits
 * for its fence. A frame that does carry copies (queued after those went ahead) keeps the order above: their
 * completion is published before the present call, never after it.
 */
static void rendererVkRedraw(bool *waitingForBuffers) {
    LorieBuffer *buffer;
    LorieBuffer_Desc *desc;
    struct lorie_vk_image *root = NULL;
    struct lorie_vk_quad rootQuad, cursorQuad;
    bool cursor = false, owned, submitOpt = state->backend.vkSubmitOpt, early;
    uint64_t serial = 0, startUs, lockedUs, inflightUs = 0, submittedUs = 0, fencedUs = 0;
    uint32_t readIndex = 0, index;
    int status, presented = LORIE_VK_FRAME_OK, filter = filtering == GL_LINEAR;
    VkResult res = VK_SUCCESS;
    // P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG): this frame's stamps, sampled once it is presented
    bool diag = state->backend.vkFrameDiag;
    struct rendererVkFrameTimes ft = { 0 };

    if (diag) {
        ft.startUs = lorieMonotonicUs();
        ft.wantedUs = state->frameWantedUs;
        ft.afterRootUs = vkRootCopyBusyUs;
        vkRootCopyBusyUs = 0;
    }
    // P11-C3-B2: the X server already waits for the root copy submitted ahead - waited for (and settled) first
    if (vkAux.pending && __atomic_load_n(&state->rootCopy.waiting, __ATOMIC_ACQUIRE) == vkAux.work.seq) {
        LORIE_DIAG_ADD(state, rootAheadXWaiting, 1);
        rendererVkAuxFinish();
        if (!vkActive)
            return;
    }
    pthread_spin_lock(&bufferLock);
    buffer = LorieBufferList_findById(&buffers, state->rootWindowTextureID);
    if (!buffer)
        buffer = LorieBufferList_findById(&removedBuffers, state->rootWindowTextureID);
    if (!buffer)
        *waitingForBuffers = true;
    pthread_spin_unlock(&bufferLock);
    if (!buffer) {
        log("Buffer %llu not found", (unsigned long long) state->rootWindowTextureID);
        rendererVkApplyPendingGpuCopies();
        return;
    }

    desc = LorieBuffer_description(buffer);
    {
        int alignedExpectedW = expectedW - (expectedW % CVT_H_GRANULARITY);
        if (!expectedW || !expectedH || desc->height != expectedH ||
            (desc->width != alignedExpectedW && desc->width != expectedW)) {
            log("Buffer %llu is not of expected size, expecting %dx%d or %dx%d, got %dx%d",
                (unsigned long long) state->rootWindowTextureID, alignedExpectedW, expectedH, expectedW, expectedH, desc->width, desc->height);
            state->drawRequested = FALSE;
            *waitingForBuffers = true;
            rendererVkApplyPendingGpuCopies();
            return;
        }
    }
    if (desc->type != LORIEBUFFER_AHARDWAREBUFFER || !desc->buffer) {
        lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_ROOT_NOT_AHB, VK_SUCCESS);
        return rendererVkFail();
    }

    // A swapchain reported out of date, or wanting another present mode, is made again first. A new size or format
    // comes as a new surface (rendererSetWindow, from surfaceChanged), which makes the swapchain again anyway.
    if (vkr.recreate && !lorieVkSwapchainCreate(&vk, &vkr, win, state))
        return rendererVkFail();
    // FIFO: the image below may come only with the display's next refresh, and the copies queued do not wait for
    // it - they go in a submission of their own first. Otherwise they go in the frame's - or, P11-C1, first too.
    if (vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR || submitOpt) {
        if (vkr.presentMode != VK_PRESENT_MODE_FIFO_KHR && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex)
            LORIE_DIAG_ADD(state, vkCopiesAhead, 1);
        rendererVkApplyPendingGpuCopies();
        if (!vkActive)
            return;     // failed (rendererVkFail)
    }
    if (vkr.held) {
        index = vkr.heldIndex;      // acquired by a frame that drew nothing, and still ours
        vkr.held = false;
        status = LORIE_VK_FRAME_OK;
    } else {
        uint64_t acquireStartUs = diag ? lorieMonotonicUs() : 0;
        status = lorieVkAcquire(&vk, &vkr, &index, state);
        if (diag)
            ft.acquireUs = lorieMonotonicUs() - acquireStartUs;
    }
    if (status == LORIE_VK_FRAME_SKIP)
        return;     // drawRequested stays: tried again
    if (status == LORIE_VK_FRAME_RECREATE) {
        if (!lorieVkSwapchainCreate(&vk, &vkr, win, state))
            rendererVkFail();
        return;
    }
    if (status == LORIE_VK_FRAME_FATAL)
        return rendererVkFail();

    // We should signal X server to not use root window while we actively copy it
    startUs = lorieMonotonicUs();
    lorie_mutex_lock(&state->lock, &state->lockingPid);
    lockedUs = lorieMonotonicUs();
    owned = rendererVkOwnedLocked();
    if (owned) {
        inflightUs = lorieMonotonicUs();
        rendererVkWaitOthers();
        inflightUs = lorieMonotonicUs() - inflightUs;
    }
    // The root the X server publishes now, under this lock: root double buffering swaps it under the same lock
    // (InitOutput.c lorieRootSwap), and the one looked up before may have become the buffer it draws into.
    if (owned && state->rootWindowTextureID != desc->id) {
        LorieBuffer *now;
        pthread_spin_lock(&bufferLock);
        now = LorieBufferList_findById(&buffers, state->rootWindowTextureID);
        pthread_spin_unlock(&bufferLock);
        if (now && LorieBuffer_description(now)->type == LORIEBUFFER_AHARDWAREBUFFER && LorieBuffer_description(now)->buffer &&
            LorieBuffer_description(now)->width == desc->width && LorieBuffer_description(now)->height == desc->height) {
            buffer = now;
            desc = LorieBuffer_description(now);
        } else {
            // Not one the renderer has yet, or of another size: neither it nor the one looked up before is sampled.
            // The image acquired is kept for the next frame (vkr.held), the copies run alone, the frame is tried
            // again once the root is there - as when it was not found before acquiring.
            lorie_mutex_unlock(&state->lock, &state->lockingPid);
            vkr.held = true;
            vkr.heldIndex = index;
            *waitingForBuffers = true;
            rendererVkApplyPendingGpuCopies();
            return;
        }
    }
    // P11-C3-B1: the root reserved by the X server for a CPU access - which its rule never allows (lorie.h cpuAccess) -
    // is not sampled: the image kept for the next frame, the copies run alone, a release waited for (bounded).
    if (owned && lorieCpuReserved(state, desc->id)) {
        uint32_t releases = __atomic_load_n(&state->cpuAccess.releases, __ATOMIC_ACQUIRE);
        struct timespec slice = { 0, 33 * 1000000 };
        __atomic_store_n(&state->cpuAccess.waiter, 1, __ATOMIC_RELAXED);
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        vkr.held = true;
        vkr.heldIndex = index;
        rendererVkApplyPendingGpuCopies();
        syscall(SYS_futex, &state->cpuAccess.releases, FUTEX_WAIT, releases, &slice, NULL, 0);
        return;
    }
    if (lorieVkFrameBegin(&vk, &vkr) != VK_SUCCESS) {
        lorie_mutex_unlock(&state->lock, &state->lockingPid);
        lorieVkFallback(state, LORIE_FALLBACK_CLASS_FATAL_BACKEND, LORIE_FALLBACK_VK_SUBMIT, VK_ERROR_INITIALIZATION_FAILED);
        return rendererVkFail();
    }
    // Without the X server's PER_ACCESS answer no buffer of it is touched: the acquired image is cleared and
    // presented, the root drawn after ownership was asked for again.
    if (owned) {
        if (!rendererVkCopiesLocked(&serial, &readIndex) ||
            !(root = lorieVkImport(&vk, &vkImages, desc->id, desc->buffer, 0, state))) {
            lorie_mutex_unlock(&state->lock, &state->lockingPid);
            return rendererVkFail();
        }
        state->drawRequested = FALSE;
    }

    if (state->cursor.updated) {
        lorie_mutex_lock(&state->cursor.lock, &state->cursor.lockingPid);
        state->cursor.updated = false;
        lorieVkFrameCursor(&vk, &vkr, (const uint32_t *) state->cursor.bits, state->cursor.width, state->cursor.height);
        lorie_mutex_unlock(&state->cursor.lock, &state->cursor.lockingPid);
    }
    state->cursor.moved = FALSE;

    rootQuad = (struct lorie_vk_quad) { .rect = { -1.f, -1.f, 1.f, 1.f }, .uv = { 0.f, 0.f, 1.f, 1.f },
                                        .flags = LORIE_VK_QUAD_OPAQUE | (root && root->rgbaOrder ? LORIE_VK_QUAD_SWIZZLE : 0) };
    if (state->cursor.width && state->cursor.height && vkr.cursorWidth == state->cursor.width &&
        vkr.cursorHeight == state->cursor.height) {
        float x = 2.f * ((float) state->cursor.x - (float) state->cursor.xhot) / (float) desc->width - 1.f;
        float y = 2.f * ((float) state->cursor.y - (float) state->cursor.yhot) / (float) desc->height - 1.f;
        float w = 2.f * (float) state->cursor.width / (float) desc->width;
        float h = 2.f * (float) state->cursor.height / (float) desc->height;
        cursorQuad = (struct lorie_vk_quad) { .rect = { x, y, x + w, y + h }, .uv = { 0.f, 0.f, 1.f, 1.f }, .flags = 0 };
        cursor = true;
    }
    lorieVkFrameScreen(&vk, &vkr, index, root, &(VkRect2D) { { viewportX, viewportY }, { (uint32_t) viewportW, (uint32_t) viewportH } },
                       &rootQuad, cursor ? &cursorQuad : NULL, filter);
    if (diag)
        ft.submitStartUs = lorieMonotonicUs();
    vkr.chainWait = vkAux.chained;      // P11-C3-B2: on the device after the root copy submitted ahead
    res = lorieVkFrameQueue(&vk, &vkr, index, true, state);
    submittedUs = lorieMonotonicUs();
    if (res == VK_SUCCESS) {
        vkAux.chained = false;
        rendererVkInFlight();
    }
    state->waitForNextFrame = true;
    lorie_mutex_unlock(&state->lock, &state->lockingPid);

    // P11-C1: no copy in it (serial: the last entry it took, cancelled ones too), presented now; its result taken up
    // once the fence was waited for, as the present below.
    if ((early = submitOpt && res == VK_SUCCESS && !serial)) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();
        presented = lorieVkPresent(&vk, &vkr, index, state);
        if (diag)
            ft.presentEndUs = lorieMonotonicUs();
        LORIE_DIAG_ADD(state, vkEarlyPresents, 1);
    }
    // P11-C3-B2: the root copy submitted ahead - over by now or soon: the device runs it before this frame
    if (vkAux.pending) {
        rendererVkAuxFinish();
        if (!vkActive)
            return;
    }
    if (res == VK_SUCCESS) {
        // The device runs the frame; the X server goes on, without the frame's buffers (rendererVkInFlight).
        bool over;
        res = lorieVkFrameWait(&vk, &vkr, state, &over);
        fencedUs = lorieMonotonicUs();
        if (over)
            rendererVkFrameDone();  // otherwise once rendererVkFail's teardown found the device idle or lost
        if (diag)
            ft.fenceUs = vkr.fenceWaitUs;   // this frame's own fence (P11-C3)
    }
    if (res == VK_SUCCESS) {
        // Only now that the device finished (and gave the buffers back) may the X server reuse the source.
        uint64_t queued[LORIE_GPU_COPY_QUEUE_CAPACITY], publishedUs;
        uint32_t n = owned ? rendererVkQueuedTimes(readIndex, queued) : 0;
        if (owned)
            state->gpuCopyQueue.readIndex = readIndex;
        publishedUs = lorieMonotonicUs();
        if (serial) {
            __atomic_store_n(&state->gpuCopyQueue.completedUs, publishedUs, __ATOMIC_RELAXED);
            __atomic_store_n(&state->gpuCopyQueue.completedSerial, serial, __ATOMIC_RELEASE);
            notifyGpuCopyPublished(publishedUs);
        }
        rendererVkCountCopies(queued, n, true, startUs, lockedUs, inflightUs, submittedUs, fencedUs, publishedUs);
    }

    if (!owned) {
        vkOwned = false;
        backendSelect = true;   // asked for again before the next frame
    }
    if (res != VK_SUCCESS)
        return rendererVkFail();
    if (!early) {
        if (diag)
            ft.presentStartUs = lorieMonotonicUs();
        presented = lorieVkPresent(&vk, &vkr, index, state);
        if (diag)
            ft.presentEndUs = lorieMonotonicUs();
    }
    if (presented == LORIE_VK_FRAME_FATAL)
        return rendererVkFail();
    state->renderedFrames++;
    if (diag) {
        ft.lockStartUs = startUs;
        ft.lockedUs = lockedUs;
        ft.inflightUs = inflightUs;
        ft.submittedUs = submittedUs;
        ft.endUs = lorieMonotonicUs();
        rendererVkFrameTimed(&ft);
    }
}

static inline __always_inline bool rendererShouldWait(bool *waitingForBuffers) {
    static uint64_t lastRequestedBufferId = 0;
    bool buffersChanged, gpuCopyPending;
    pthread_spin_lock(&bufferLock);
    buffersChanged = !xorg_list_is_empty(&addedBuffers) || !xorg_list_is_empty(&removedBuffers);
    pthread_spin_unlock(&bufferLock);
    // P11-C3-B1: copies left queued for a reservation are pending again only once one is let go
    gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex && !rendererCopiesStillDeferred();
    if (stateChanged || windowChanged || buffersChanged || gpuCopyPending || backendSelect || rendererRootCopyPending())
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

// Whether this loop pass draws a Vulkan frame: a window, a frame allowed, something to draw.
static bool rendererVkFrameWanted(void) {
    bool gpuCopyPending = state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex && !rendererCopiesStillDeferred();
    return state->surfaceAvailable && !state->waitForNextFrame &&
           (state->drawRequested || state->cursor.moved || state->cursor.updated || gpuCopyPending);
}

/*
 * One pass of the renderer's work, stateLock let go: a root copy the X server asked for first (P11-C2), then the frame
 * or the copies alone - by Vulkan while it is active, else by GLES: never both in one pass. P11-C3-B2: a root copy
 * submitted ahead of the frame and still pending once Vulkan's part of the pass is done (no frame after it, one that
 * returned early or skipped) is waited for and its chain semaphore consumed (rendererVkAuxSettle) before the pass
 * ends; one Vulkan's failure left is settled by the teardown (rendererVkAuxForget).
 */
static void rendererPass(bool *waitingForBuffers) {
    bool gpuCopyPending;

    // P11-C2: a root copy the X server asked for, first - it may be waiting for it before it can draw again.
    // P11-C3-B2: submitted ahead of the frame this pass draws, which waits for it on the device, not the CPU.
    vkRootCopyBusyUs = 0;      // P11-C3: only one made in this pass counts as ahead of its frame
    if (state && rendererRootCopyPending()) {
        if (!vkActive)
            rendererRootCopyDecline();
        else if (state->backend.vkScheduleOpt && rendererVkFrameWanted())
            rendererVkRootCopyAhead();
        else
            rendererVkRootCopy();
    }

    // Prefer a full redraw over the standalone apply below so a pending GPU copy shares one
    // lock+fence with the root/cursor draw, instead of two GPU round trips per frame (unless FIFO or
    // TERMUX_X11_RENDERER_VK_SUBMIT_OPT: then the frame sends the copies ahead in a submission of their own).
    gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex && !rendererCopiesStillDeferred();
    if (vkActive) {
        if (state && rendererVkFrameWanted())
            rendererVkRedraw(waitingForBuffers);
        else if (gpuCopyPending)
            rendererVkApplyPendingGpuCopies();
        // P11-C3-B2: a root copy submitted ahead and no frame after it: waited for, its chain semaphore consumed
        if (vkAux.pending || vkAux.chained)
            rendererVkAuxSettle();
    } else if (state && state->surfaceAvailable && !state->waitForNextFrame &&
        (state->drawRequested || state->cursor.moved || state->cursor.updated || gpuCopyPending)) {
        rendererRedrawLocked(waitingForBuffers);
    } else if (gpuCopyPending) {
        // Also when no window is drawn (or Vulkan failed, even in a session that asked for Vulkan only): the X
        // server waits for the copies it queued, and GLES runs them on the default surface's context.
        rendererApplyPendingGpuCopies();
    }
}

__noreturn static void* rendererThread(void) {
    LorieBuffer* buf;
    bool waitingForBuffers = false;
    while (true) {
        while (rendererShouldWait(&waitingForBuffers)) {
            if (rendererCopiesStillDeferred()) {
                // P11-C3-B1: the X server's wake at the release, or this bound - should it have come just before
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                rendererTimespecAddUs(&deadline, LORIE_COPIES_DEFERRED_RECHECK_US);
                pthread_cond_timedwait(stateCond, &stateLock, &deadline);
            } else
                pthread_cond_wait(stateCond, &stateLock);
        }

        if (stateChanged) {
            struct lorie_shared_server_state* oldState = NULL;
            if (state && pendingState != state)
                oldState = state;

            state = pendingState;
            pendingState = NULL;
            stateChanged = false;
            waitingForBuffers = false;
            deferredCopies = false;

            // Another X server session (or none): nothing Vulkan holds of the last one is used again. Its buffers'
            // ids start over in a new X server, so every imported image goes now (the device idle first), not only
            // once the old buffers are released; and its ownership answer was the old session's.
            if (vkReady) {
                vk.DeviceWaitIdle(vk.device);
                lorieVkImagesClear(&vk, &vkImages);
            }
            vkOwned = false;
            rendererVkPacingReset();

            if (state) {
                // A new X server session: Vulkan may be tried again, the X server's ownership asked for again.
                vkFailed = false;
                state->surfaceAvailable = win != defaultWin && (vkActive || sfc != defaultSfc);
                rendererPublishActive();
                backendSelect = true;
            } else if (vkActive) {
                rendererVkClear();
            } else if (win != defaultWin) {
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                eglSwapBuffers(egl_display, sfc);
            }

            if (oldState)
                munmap(oldState, sizeof(*oldState));
        }

        if (windowChanged) { rendererRefreshContext(); waitingForBuffers = false; } if (presentModeChanged) { presentModeChanged = false; rendererApplyPresentMode(); rendererVkApplyPresentMode(); } // Attach all pending buffers to GL.
        pthread_spin_lock(&bufferLock);
        while((buf = LorieBufferList_first(&addedBuffers))) {
            if (rendererGlesEager())
                rendererAttachToGL(buf);
            LorieBuffer_addToList(buf, &buffers);
            waitingForBuffers = false;
        }
        pthread_spin_unlock(&bufferLock);

        pthread_cond_signal(&stateChangeFinishCond);
        // After the window or session change was acknowledged: starting Vulkan (or falling back) may take a while.
        if (backendSelect) {
            rendererSelectBackend();
            waitingForBuffers = false;
        }
        pthread_mutex_unlock(&stateLock);

        rendererPass(&waitingForBuffers);

        pthread_spin_lock(&bufferLock);
        // Remove all buffers which were attached to GL (and their Vulkan images, if any: no frame is in flight).
        while((buf = LorieBufferList_first(&removedBuffers))) {
            if (vkReady)
                lorieVkImageRemove(&vk, &vkImages, LorieBuffer_description(buf)->id);
            LorieBuffer_release(buf);
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
    draw(cursor.id, x, y, x + w, y + h, 1.f, false);
    glDisable(GL_BLEND);
}

