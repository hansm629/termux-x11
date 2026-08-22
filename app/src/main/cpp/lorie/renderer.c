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
    if (state)
        state->drawRequested = true;
    pthread_cond_signal(stateCond);
    pthread_mutex_unlock(&stateLock);
}

void rendererRefreshContext(void) {
    int width = pendingWin ? ANativeWindow_getWidth(pendingWin) : 0;
    int height = pendingWin ? ANativeWindow_getHeight(pendingWin) : 0;
    log("rendererSetWindow %p %d %d", pendingWin, width, height);

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
}

static void draw(GLuint id, float x0, float y0, float x1, float y1, float xfactor, uint8_t flip);
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
    if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id))) {
        LorieBuffer_attachToGL(buf);
        LorieBuffer_addToList(buf, &buffers);
    }
    pthread_spin_unlock(&bufferLock);

    for (attempt = 0; attempt < 20 && !buf; attempt++) {
        usleep(5000);
        pthread_spin_lock(&bufferLock);
        buf = LorieBufferList_findById(&buffers, id);
        if (!buf && (buf = LorieBufferList_findById(&addedBuffers, id))) {
            LorieBuffer_attachToGL(buf);
            LorieBuffer_addToList(buf, &buffers);
        }
        pthread_spin_unlock(&bufferLock);
    }
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
        LorieBuffer *src = rendererFindBufferWithRetry(entry.srcBufferId);
        LorieBuffer *dst = rendererFindBufferWithRetry(entry.dstBufferId);

        if (!src)
            log("rendererApplyPendingGpuCopies: source buffer %llu not found after waiting, skipping\n", (unsigned long long) entry.srcBufferId);
        if (!dst)
            log("rendererApplyPendingGpuCopies: destination buffer %llu not found after waiting, skipping\n", (unsigned long long) entry.dstBufferId);

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

void rendererRedrawLocked(bool* waitingForBuffers) {
    float xfactor = 1.f;
    LorieBuffer_Desc *desc = NULL;
    EGLSync fence = EGL_NO_SYNC_KHR;
    bool rootFenceWaitEnabled = rendererRootFenceWaitEnabled;
    bool postSwapTouchEnabled = rendererPostSwapTouchEnabled;
    bool postSwapFenceWaitEnabled = postSwapTouchEnabled && rendererPostSwapFenceWaitEnabled;
    bool swapBackpressureGuardEnabled = !rootFenceWaitEnabled && !postSwapTouchEnabled && !postSwapFenceWaitEnabled;
    int64_t frameStartNs = rendererPerfLogEnabled ? rendererNowNs() : 0;
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
        return;
    }

    desc = LorieBuffer_description(buffer);

    int alignedExpectedW = expectedW - (expectedW % CVT_H_GRANULARITY);

    if (!expectedW || !expectedH || desc->height != expectedH ||
        (desc->width != alignedExpectedW && desc->width != expectedW)) {
        log("Buffer %llu is not of expected size, expecting %dx%d or %dx%d, got %dx%d",
            state->rootWindowTextureID, alignedExpectedW, expectedH, expectedW, expectedH,
            desc->width, desc->height);
        // Otherwise rendererShouldWait sees drawRequested still set and busy-spins retrying this
        // same mismatch instead of waiting for the next real trigger (e.g. the pending resize).
        state->drawRequested = FALSE;
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

    LorieBuffer_bindTexture(buffer);
    if (desc->type == LORIEBUFFER_FD)
        xfactor = (float) desc->width/(float) desc->stride;
    draw(0, -1.f, -1.f, 1.f, 1.f, xfactor, LorieBuffer_isRgba(buffer));
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
        if (rendererPerfLogEnabled) {
            int64_t waitStartNs = rendererNowNs();
            eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
            rootWaitUs = rendererNsToUs(rendererNowNs() - waitStartNs);
        } else {
            eglClientWaitSyncKHR(egl_display, fence, 0, EGL_FOREVER);
        }

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

        if (windowChanged) rendererRefreshContext(); if (presentModeChanged) { presentModeChanged = false; rendererApplyPresentMode(); } // Attach all pending buffers to GL.
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
        if (state && state->surfaceAvailable && !state->waitForNextFrame &&
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

