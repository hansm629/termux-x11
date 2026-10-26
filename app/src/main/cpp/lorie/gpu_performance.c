/*
 * Universal GPU Performance Controller (experimental).
 *
 * The original Termux-Desktop gpu_keepalive used a tight 16x16 glClear/glFinish
 * loop to keep GPU power management out of long idle intervals. This runs as a
 * self-contained EGL context in the Termux-X11 Android process, NOT in the X
 * server's GLES/Vulkan command buffers. It supports any EGL/GLES-capable GPU
 * and never writes cpuset, sysfs, frequency, governor or driver-private state.
 *
 * Balanced/performance regulate helper CPU duty and stop while X11 is idle,
 * hidden or thermally severe. Maximum deliberately retains the original
 * unlimited loop as an explicit benchmark-only opt-in.
 *
 * Do not share EGL contexts or render targets with the real X11 compositor:
 * that would introduce new GPU fences, renderer locks or ordering constraints.
 */
#define _POSIX_C_SOURCE 200809L
#include "gpu_performance.h"
#include "gpu_performance_policy.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GPU_LOG(...) __android_log_print(ANDROID_LOG_INFO, "LorieGpuPerf", __VA_ARGS__)
#define GPU_ERR(...) __android_log_print(ANDROID_LOG_WARN, "LorieGpuPerf", __VA_ARGS__)

static _Atomic int selectedMode;
static _Atomic bool modeConfiguredOnce;
static _Atomic int thermalStatus;
static _Atomic bool appForeground;
static _Atomic uint64_t lastPresentedNs;
static _Atomic uint64_t presentedCount;
static _Atomic unsigned displayRefreshHz = 60;
static pthread_mutex_t workerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workerWake = PTHREAD_COND_INITIALIZER;
static bool workerStarted;

static uint64_t monoNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t cpuNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void sleepForUs(unsigned us) {
    struct timespec delay = { .tv_sec = us / 1000000,
                              .tv_nsec = (long)(us % 1000000) * 1000L };
    /* A blocking nanosleep, not a busy poll. Normal signals may end it early. */
    nanosleep(&delay, NULL);
}

static void sleepForNs(uint64_t ns) {
    if (!ns)
        return;
    struct timespec delay = { .tv_sec = (time_t)(ns / UINT64_C(1000000000)),
                              .tv_nsec = (long)(ns % UINT64_C(1000000000)) };
    nanosleep(&delay, NULL);
}

static void waitForNewWork(void) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 150000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&workerMutex);
    pthread_cond_timedwait(&workerWake, &workerMutex, &until);
    pthread_mutex_unlock(&workerMutex);
}

struct gpu_pbuffer {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    PFNEGLCREATESYNCKHRPROC createSync;
    PFNEGLCLIENTWAITSYNCKHRPROC waitSync;
    PFNEGLDESTROYSYNCKHRPROC destroySync;
    EGLSyncKHR pending[2];
    unsigned pendingHead, pendingCount;
    uint64_t oldestQueuedNs;
    bool asyncEnabled;
    /* A renderer-agnostic test detects ANGLE/Vulkan clear-only paths that
     * return from glFinish in ~1-2us and otherwise spin a big CPU core. */
    bool fastClear;
    bool readbackEnabled;
    uint64_t probeClearNs;
    uint64_t lastReadbackNs;
    GLuint verificationProgram;
    GLuint verificationVertices;
    GLint verificationColorUniform;
};

static void gpuPerfDropOutstandingFences(struct gpu_pbuffer *gpu) {
    if (!gpu->destroySync)
        return;
    while (gpu->pendingCount) {
        gpu->destroySync(gpu->display, gpu->pending[gpu->pendingHead]);
        gpu->pendingHead = (gpu->pendingHead + 1) % 2;
        gpu->pendingCount--;
    }
}

static void releasePbuffer(struct gpu_pbuffer *gpu) {
    if (gpu->display == EGL_NO_DISPLAY)
        return;
    gpuPerfDropOutstandingFences(gpu);
    if (gpu->verificationVertices)
        glDeleteBuffers(1, &gpu->verificationVertices);
    if (gpu->verificationProgram)
        glDeleteProgram(gpu->verificationProgram);
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gpu->surface != EGL_NO_SURFACE)
        eglDestroySurface(gpu->display, gpu->surface);
    if (gpu->context != EGL_NO_CONTEXT)
        eglDestroyContext(gpu->display, gpu->context);
    /* The compositor may use the same EGLDisplay in another thread. Avoid
     * eglTerminate here: never tear down its display from the helper. */
    gpu->surface = EGL_NO_SURFACE;
    gpu->context = EGL_NO_CONTEXT;
    gpu->fastClear = false;
    gpu->readbackEnabled = false;
    gpu->verificationProgram = 0;
    gpu->verificationVertices = 0;
}

/* glFinish completes outstanding GL work, but does NOT guarantee that a
 * stream of overwritten off-screen clears makes the GPU execute each clear.
 * Samsung's ANGLE/Vulkan path returned in ~1.7us (~590k iterations/sec)
 * and used 99% of one big core while the GPU was often at minimum clock.
 * A short warmup followed by a measured batch classifies the actual EGL
 * behavior without hard-coding GPU families. */
static uint64_t gpuPerfProbeClearNs(void) {
    for (unsigned i = 0; i < 16; i++) {
        glClearColor((float)i / 255.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
    }
    const uint64_t before = monoNs();
    for (unsigned i = 0; i < 64; i++) {
        glClearColor((float)(i + 16) / 255.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
    }
    const uint64_t after = monoNs();
    return after > before ? (after - before) / 64 : 0;
}

static GLuint gpuPerfCompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* A varying fragment shader backed by an actual VBO prevents an ANGLE/Vulkan
 * driver from treating an unlimited sequence of overwritten glClear calls as
 * CPU-side metadata updates. Shader work is extremely small (16x16 pixels)
 * and is submitted only for the occasional GPU-pixel verification. */
static bool gpuPerfPrepareVerificationDraw(struct gpu_pbuffer *gpu) {
    static const char *vsText =
        "attribute vec2 aPos;\n"
        "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char *fsText =
        "precision mediump float;\n"
        "uniform vec4 uColor;\n"
        "void main() { gl_FragColor = uColor; }\n";
    GLuint vs = gpuPerfCompileShader(GL_VERTEX_SHADER, vsText);
    GLuint fs = gpuPerfCompileShader(GL_FRAGMENT_SHADER, fsText);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(program);
        return false;
    }
    GLint color = glGetUniformLocation(program, "uColor");
    if (color < 0) {
        glDeleteProgram(program);
        return false;
    }
    const GLfloat vertices[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    if (!vbo) {
        glDeleteProgram(program);
        return false;
    }
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    gpu->verificationProgram = program;
    gpu->verificationVertices = vbo;
    gpu->verificationColorUniform = color;
    return glGetError() == GL_NO_ERROR;
}

static bool setupPbuffer(struct gpu_pbuffer *gpu) {
    EGLConfig config = NULL;
    EGLint configs = 0, major = 0, minor = 0;
    const EGLint attrs3[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    const EGLint attrs2[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    const EGLint size[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    const EGLint ctx3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint ctx2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    int esVersion = 3;

    gpu->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (gpu->display == EGL_NO_DISPLAY ||
        eglInitialize(gpu->display, &major, &minor) != EGL_TRUE ||
        eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE)
        return false;
    if (eglChooseConfig(gpu->display, attrs3, &config, 1, &configs) != EGL_TRUE || !configs) {
        esVersion = 2;
        configs = 0;
        if (eglChooseConfig(gpu->display, attrs2, &config, 1, &configs) != EGL_TRUE || !configs)
            return false;
    }
    gpu->context = eglCreateContext(gpu->display, config, EGL_NO_CONTEXT,
                                    esVersion == 3 ? ctx3 : ctx2);
    if (gpu->context == EGL_NO_CONTEXT && esVersion == 3) {
        /* A driver may advertise ES3 configs but reject ES3 context creation. */
        esVersion = 2;
        configs = 0;
        if (eglChooseConfig(gpu->display, attrs2, &config, 1, &configs) == EGL_TRUE && configs)
            gpu->context = eglCreateContext(gpu->display, config, EGL_NO_CONTEXT, ctx2);
    }
    if (gpu->context == EGL_NO_CONTEXT)
        return false;
    gpu->surface = eglCreatePbufferSurface(gpu->display, config, size);
    if (gpu->surface == EGL_NO_SURFACE ||
        eglMakeCurrent(gpu->display, gpu->surface, gpu->surface, gpu->context) != EGL_TRUE)
        return false;
    gpu->probeClearNs = gpuPerfProbeClearNs();
    gpu->fastClear = gpuPerfFastClear(gpu->probeClearNs);
    gpu->readbackEnabled = gpu->fastClear;
    if (gpu->fastClear && !gpuPerfPrepareVerificationDraw(gpu)) {
        GPU_ERR("off-screen GPU draw shader unavailable; retaining bounded readback-only mode");
        if (gpu->verificationVertices) {
            glDeleteBuffers(1, &gpu->verificationVertices);
            gpu->verificationVertices = 0;
        }
        if (gpu->verificationProgram) {
            glDeleteProgram(gpu->verificationProgram);
            gpu->verificationProgram = 0;
        }
        /* Shader availability is not a condition for continuing normal X11. */
    }
    const char *extensions = eglQueryString(gpu->display, EGL_EXTENSIONS);
    if (extensions && strstr(extensions, "EGL_KHR_fence_sync")) {
        gpu->createSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        gpu->waitSync = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
        gpu->destroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        gpu->asyncEnabled = gpu->createSync && gpu->waitSync && gpu->destroySync;
    }
    GPU_LOG("EGL pbuffer ready: ES%d 16x16; all GPU vendors use the same path", esVersion);
    GPU_LOG("EGL renderer=\"%s\" glFinish_probe_ns=%llu fast_clear=%d "
            "readback_verification=%d shader_draw=%d",
            glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "unknown",
            (unsigned long long)gpu->probeClearNs, gpu->fastClear,
            gpu->readbackEnabled, !!gpu->verificationProgram);
    GPU_LOG("async bounded GPU queue: %s (fallback glFinish available)",
            gpu->asyncEnabled ? "available" : "unavailable");
    return true;
}

/* On observed ultra-fast clear-only paths, force a *real framebuffer result*
 * to become visible in CPU memory occasionally. This defeats optimizations
 * which coalesce/discard repeated pbuffer clears without ever materializing
 * the final pixel. One RGBA pixel at most every 8ms; not once per frame or
 * per glFinish. Keep the readback isolated from X11 rendering buffers.
 * glReadPixels can stall: CPU-duty pacing immediately follows this call. */
static bool gpuPerfMaterialize(struct gpu_pbuffer *gpu, uint64_t pulseIndex,
                               uint64_t *readbacks, uint64_t *mismatches) {
    uint64_t now = monoNs();
    if (!gpu->readbackEnabled ||
        !gpuPerfReadbackDue(now, gpu->lastReadbackNs))
        return true;
    gpu->lastReadbackNs = now;
    if (gpu->verificationProgram) {
        glUseProgram(gpu->verificationProgram);
        glUniform4f(gpu->verificationColorUniform,
                    (float)(pulseIndex & 255u) / 255.f, 0.f, 0.f, 1.f);
        glBindBuffer(GL_ARRAY_BUFFER, gpu->verificationVertices);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    GLubyte pixel[4] = {0, 0, 0, 0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        GPU_ERR("1x1 GPU work verification failed GL=%#x; disabling readback, "
                "keeping CPU spin guard", error);
        gpu->readbackEnabled = false;
        return false;
    }
    (*readbacks)++;
    unsigned expected = (unsigned)(pulseIndex & 255u);
    if ((unsigned)pixel[0] + 2 < expected || (unsigned)pixel[0] > expected + 2) {
        (*mismatches)++;
        if (*mismatches == 1)
            GPU_ERR("1x1 GPU readback differs from requested color "
                    "expected=%u got=%u; driver may optimize off-screen work",
                    expected, (unsigned)pixel[0]);
    }
    return true;
}

/* Performance mode: at most two unconsumed off-screen GPU fences. Queueing a
 * tiny GL clear followed by an EGL fence and glFlush avoids repeatedly
 * blocking the helper CPU in glFinish(), without creating an unbounded GPU
 * backlog. No fence or surface is shared with X11. Balanced and Maximum keep
 * the previous glFinish workload, so real-device A/B remains possible.
 * Return false to fall back to the original glFinish path. */
static bool gpuPerfAsyncPulse(struct gpu_pbuffer *gpu, uint64_t pulseIndex,
                              bool *submitted, uint64_t *completed) {
    *submitted = false;
    if (!gpu->asyncEnabled)
        return false;

    if (gpu->pendingCount == 2) {
        EGLSyncKHR oldest = gpu->pending[gpu->pendingHead];
        EGLint result = gpu->waitSync(gpu->display, oldest, 0, 0);
        if (result == EGL_CONDITION_SATISFIED_KHR) {
            gpu->destroySync(gpu->display, oldest);
            gpu->pendingHead = (gpu->pendingHead + 1) % 2;
            gpu->pendingCount--;
            (*completed)++;
            gpu->oldestQueuedNs = monoNs();
        } else if (result == EGL_TIMEOUT_EXPIRED_KHR) {
            if (monoNs() - gpu->oldestQueuedNs > UINT64_C(1000000000)) {
                GPU_ERR("async GPU fence stalled for 1s; stopping synthetic GPU work");
                gpu->asyncEnabled = false;
                atomic_store_explicit(&selectedMode, GPU_PERF_OFF, memory_order_relaxed);
                return true;  /* Do not call a potentially unbounded glFinish. */
            }
            return true;  /* Saturated: caller yields; do not submit more. */
        } else {
            GPU_ERR("async GPU fence error %#x; stopping synthetic GPU work", eglGetError());
            gpu->asyncEnabled = false;
            atomic_store_explicit(&selectedMode, GPU_PERF_OFF, memory_order_relaxed);
            return true;
        }
    }

    glClearColor((float)(pulseIndex & 255) / 255.0f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    EGLSyncKHR sync = gpu->createSync(gpu->display, EGL_SYNC_FENCE_KHR, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        GPU_ERR("EGL fence creation failed %#x; falling back to glFinish", eglGetError());
        gpu->asyncEnabled = false;
        return false;
    }
    glFlush();  /* Flush the clear AND fence into the driver's command stream. */
    if (!gpu->pendingCount)
        gpu->oldestQueuedNs = monoNs();
    gpu->pending[(gpu->pendingHead + gpu->pendingCount) % 2] = sync;
    gpu->pendingCount++;
    *submitted = true;
    return true;
}

/* A generic GPU frequency hint for diagnostics only. Missing vendor-specific
 * sysfs nodes are normal; no policy decision depends on this readout. */
static long tryReadGpuClock(void) {
    static const char *paths[] = {
        "/sys/kernel/gpu/gpu_clock",   /* Exynos / Xclipse */
        "/sys/class/kgsl/kgsl-3d0/gpuclk", /* Qualcomm / Adreno */
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) {
            long value = -1;
            int ok = fscanf(f, "%ld", &value);
            fclose(f);
            if (ok == 1 && value >= 0)
                return value;
        }
    }
    return -1;
}

/* Read-only, optional evidence for evaluating the system GPU governor. This
 * never changes DVFS/thermal/cpuset state; unsupported nodes report -1. */
static long tryReadGpuBusyPercent(void) {
    FILE *f = fopen("/sys/kernel/gpu/gpu_busy", "r");
    if (f) {
        long pct = -1;
        int read = fscanf(f, "%ld", &pct);
        fclose(f);
        if (read == 1 && pct >= 0 && pct <= 100)
            return pct;
    }
    f = fopen("/sys/class/kgsl/kgsl-3d0/gpubusy", "r");
    if (f) {
        unsigned long busy = 0, total = 0;
        int read = fscanf(f, "%lu %lu", &busy, &total);
        fclose(f);
        if (read == 2 && total && busy <= total)
            return (long)((double)busy * 100.0 / (double)total);
    }
    return -1;
}

static const char *modeName(int mode) {
    switch (mode) {
        case GPU_PERF_BALANCED: return "balanced";
        case GPU_PERF_PERFORMANCE: return "performance";
        case GPU_PERF_MAXIMUM: return "maximum";
        default: return "off";
    }
}

static void *gpuPerformanceThread(void *unused) {
    (void)unused;
    /* Linux task comm is limited to 15 bytes. The old 19-byte name was
     * rejected and htop showed this 99%-CPU worker as simply com.termux. */
    pthread_setname_np(pthread_self(), "LorieGpuAssist");
    struct gpu_pbuffer gpu = { EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT };
    bool initialized = false, wasActive = false;
    int previousMode = -1;
    unsigned delayUs = 400;
    uint64_t sampleNs = monoNs(), sampleCpu = cpuNs(), reportNs = sampleNs;
    uint64_t loops = 0, previousLoops = 0, oldFrames = 0;
    uint64_t completedAsync = 0, oldCompletedAsync = 0;
    uint64_t readbacks = 0, oldReadbacks = 0, readbackMismatches = 0;
    uint64_t cpuPacedPulses = 0, oldCpuPacedPulses = 0;
    unsigned previousHz = 0;
    unsigned measuredCpu = 0;
    unsigned suspiciousFastStreak = 0;

    for (;;) {
        int mode = atomic_load_explicit(&selectedMode, memory_order_relaxed);
        bool foreground = atomic_load_explicit(&appForeground, memory_order_relaxed);
        int thermal = atomic_load_explicit(&thermalStatus, memory_order_relaxed);
        unsigned refreshHz = atomic_load_explicit(&displayRefreshHz, memory_order_relaxed);
        uint64_t now = monoNs();
        uint64_t last = atomic_load_explicit(&lastPresentedNs, memory_order_relaxed);
        bool work = gpuPerfShouldWork(mode, foreground, thermal, now, last);
        if (!work) {
            if (wasActive)
                GPU_LOG("paused mode=%s foreground=%d thermal=%d frames=%llu",
                        modeName(mode), foreground, thermal,
                        (unsigned long long)atomic_load(&presentedCount));
            wasActive = false;
            previousMode = -1;
            sampleNs = now;
            sampleCpu = cpuNs();
            if (mode != GPU_PERF_OFF && now - reportNs >= UINT64_C(5000000000)) {
                GPU_LOG("state=idle mode=%s visible=%d thermal=%d refresh_hz=%u "
                        "last_frame_age_ms=%lld frames=%llu/5s reason=%s",
                        modeName(mode), foreground, thermal, refreshHz,
                        last && now >= last ?
                            (long long)((now - last) / UINT64_C(1000000)) : -1LL,
                        (unsigned long long)(atomic_load(&presentedCount) - oldFrames),
                        !foreground ? "not_visible" :
                        thermal >= 4 ? "thermal" :
                        !last ? "no_frame" : "frames_idle");
            }
            if (now - reportNs >= UINT64_C(5000000000) || mode == GPU_PERF_OFF) {
                reportNs = now;
                oldFrames = atomic_load(&presentedCount);
                previousLoops = loops;
                oldCompletedAsync = completedAsync;
            }
            waitForNewWork();
            continue;
        }

        if (!initialized) {
            if (!setupPbuffer(&gpu)) {
                GPU_ERR("EGL/GLES helper unavailable (%#x); disabling GPU assist; X11 renderer unaffected",
                        eglGetError());
                releasePbuffer(&gpu);
                atomic_store(&selectedMode, GPU_PERF_OFF);
                continue;
            }
            initialized = true;
        }

        if (mode != previousMode || refreshHz != previousHz) {
            delayUs = mode == GPU_PERF_MAXIMUM ? 0 :
                      mode == GPU_PERF_PERFORMANCE ?
                          (refreshHz >= 90 ? 100 : 200) :
                          (refreshHz >= 90 ? 400 : 700);
            previousMode = mode;
            previousHz = refreshHz;
            sampleNs = reportNs = now;
            sampleCpu = cpuNs();
            previousLoops = loops;
            oldFrames = atomic_load(&presentedCount);
            oldCompletedAsync = completedAsync;
            oldReadbacks = readbacks;
            oldCpuPacedPulses = cpuPacedPulses;
            GPU_LOG("state=active mode=%s initial_delay_us=%u visible=%d thermal=%d "
                    "refresh_hz=%u gpu_path=%s fast_clear=%d",
                    modeName(mode), delayUs, foreground, thermal, refreshHz,
                    mode == GPU_PERF_PERFORMANCE && gpu.asyncEnabled && !gpu.fastClear ?
                        "bounded_async" : (gpu.fastClear ? "verified_finish" : "glFinish"),
                    gpu.fastClear);
        }
        wasActive = true;

        /* Change one pixel colour to avoid drivers discarding an identical
         * back-to-back clear. No window surface, swapchain or X11 locks. */
        uint64_t pulseStartNs = monoNs();
        uint64_t pulseCpuStartNs = cpuNs();
        bool submitted = false;
        bool asyncHandled = mode == GPU_PERF_PERFORMANCE && !gpu.fastClear &&
                gpuPerfAsyncPulse(&gpu, loops, &submitted, &completedAsync);
        if (!asyncHandled) {
            glClearColor((float)(loops & 255) / 255.0f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (gpu.fastClear)
                gpuPerfMaterialize(&gpu, loops, &readbacks, &readbackMismatches);
            glFinish();
            /* A user can switch away from asynchronous Performance without
             * destroying the EGL context. glFinish drains earlier commands;
             * release their now-unused sync objects to avoid stale slots. */
            if (gpu.pendingCount)
                gpuPerfDropOutstandingFences(&gpu);
            submitted = true;
        }
        if (!submitted) {
            // Two outstanding fences: no busy polling or unlimited queue.
            sleepForUs(100);
            continue;
        }
        loops++;
        if (!gpu.fastClear && !asyncHandled &&
            gpuPerfFastClear(monoNs() - pulseStartNs)) {
            if (++suspiciousFastStreak >= 32) {
                gpu.fastClear = true;
                gpu.readbackEnabled = true;
                GPU_LOG("runtime fast-clear detector engaged after 32 cheap "
                        "glFinish pulses; limiting CPU and verifying GPU pixels");
            }
        } else if (!gpu.fastClear) {
            suspiciousFastStreak = 0;
        }
        if ((loops & 1023u) == 0 && glGetError() != GL_NO_ERROR) {
            GPU_ERR("GLES context error; disabling assist (X11 unaffected)");
            releasePbuffer(&gpu);
            initialized = false;
            atomic_store(&selectedMode, GPU_PERF_OFF);
            continue;
        }

        if (mode == GPU_PERF_MAXIMUM && gpu.fastClear) {
            /* On Exynos/ANGLE glFinish was ~1.7us: without this guard it
             * monopolized one big core yet barely raised the GPU clock.
             * Preserve unpaced Maximum on genuinely GPU-bound drivers. */
            uint64_t wallNs = monoNs() - pulseStartNs;
            uint64_t cpuTimeNs = cpuNs() - pulseCpuStartNs;
            uint64_t pauseNs = gpuPerfFastMaximumSleepNs(wallNs, cpuTimeNs);
            if (pauseNs) {
                cpuPacedPulses++;
                sleepForNs(pauseNs);
            }
        } else if (mode != GPU_PERF_MAXIMUM) {
            /* Hard per-pulse CPU duty guard: feedback alone cannot protect the
             * CPU if a vendor's glFinish spins for more time than max_sleep_us.
             * Charge the helper's actual CPU time to the budget and yield the
             * difference. This preserves kernel scheduling for all OTHER tasks. */
            unsigned budget = gpuPerfLimitsForRefresh(mode, refreshHz).cpu_budget_percent;
            uint64_t busyNs = cpuNs() - pulseCpuStartNs;
            uint64_t elapsedNs = monoNs() - pulseStartNs;
            uint64_t requiredNs = (busyNs / budget) * 100 +
                                  ((busyNs % budget) * 100) / budget;
            if (requiredNs > elapsedNs)
                sleepForNs(requiredNs - elapsedNs);
            if (delayUs)
                sleepForUs(delayUs);
        }

        now = monoNs();
        if (now - sampleNs >= UINT64_C(250000000)) {
            uint64_t c = cpuNs();
            uint64_t deltaCpu = c >= sampleCpu ? c - sampleCpu : 0;
            uint64_t deltaWall = now - sampleNs;
            measuredCpu = (unsigned)((deltaCpu * 100) / deltaWall);
            if (measuredCpu > 100)
                measuredCpu = 100;
            delayUs = gpuPerfNextSleepForRefresh(mode, refreshHz, delayUs, measuredCpu);
            sampleNs = now;
            sampleCpu = c;
        }
        if (now - reportNs >= UINT64_C(5000000000)) {
            uint64_t totalFrames = atomic_load(&presentedCount);
            GPU_LOG("state=active mode=%s pulses=%llu/5s cpu_pct=%u sleep_us=%u "
                    "frames=%llu/5s refresh_hz=%u gpu_path=%s async_done=%llu/5s "
                    "async_pending=%u fast_clear=%d glfinish_probe_ns=%llu "
                    "gpu_readbacks=%llu/5s readback_mismatches=%llu "
                    "cpu_paced=%llu/5s gpu_clock_raw=%ld gpu_busy_pct=%ld "
                    "thermal=%d visible=%d",
                    modeName(mode), (unsigned long long)(loops - previousLoops),
                    measuredCpu, delayUs,
                    (unsigned long long)(totalFrames - oldFrames),
                    refreshHz, mode == GPU_PERF_PERFORMANCE && gpu.asyncEnabled && !gpu.fastClear ?
                        "bounded_async" : (gpu.fastClear ? "verified_finish" : "glFinish"),
                    (unsigned long long)(completedAsync - oldCompletedAsync),
                    gpu.pendingCount, gpu.fastClear,
                    (unsigned long long)gpu.probeClearNs,
                    (unsigned long long)(readbacks - oldReadbacks),
                    (unsigned long long)readbackMismatches,
                    (unsigned long long)(cpuPacedPulses - oldCpuPacedPulses),
                    tryReadGpuClock(), tryReadGpuBusyPercent(),
                    thermal, foreground);
            oldFrames = totalFrames;
            previousLoops = loops;
            oldCompletedAsync = completedAsync;
            oldReadbacks = readbacks;
            oldCpuPacedPulses = cpuPacedPulses;
            reportNs = now;
        }
    }
    /* Worker intentionally persists only for the lifetime of the Android
     * process. It is never forked and never touches the X server process. */
    return NULL;
}

static void wakeWorker(void) {
    pthread_cond_signal(&workerWake);
}

static void startWorkerIfNeeded(void) {
    pthread_mutex_lock(&workerMutex);
    if (!workerStarted) {
        pthread_t thread;
        int rc = pthread_create(&thread, NULL, gpuPerformanceThread, NULL);
        if (rc == 0) {
            workerStarted = true;
            pthread_detach(thread);
        } else {
            GPU_ERR("cannot create GPU assist thread (%d); renderer still operates normally", rc);
            atomic_store(&selectedMode, GPU_PERF_OFF);
        }
    }
    pthread_mutex_unlock(&workerMutex);
}

void gpuPerformanceSetMode(int mode) {
    if (mode < GPU_PERF_OFF || mode > GPU_PERF_MAXIMUM)
        mode = GPU_PERF_OFF;
    int previous = atomic_exchange_explicit(&selectedMode, mode, memory_order_relaxed);
    bool first = !atomic_exchange_explicit(&modeConfiguredOnce, true, memory_order_relaxed);
    if (first || previous != mode)
        GPU_LOG("configured mode=%s worker=%s", modeName(mode),
                mode == GPU_PERF_OFF ? "disabled" : "enabled");
    if (mode != GPU_PERF_OFF)
        startWorkerIfNeeded();
    wakeWorker();
}

void gpuPerformanceSetForeground(bool foreground) {
    atomic_store_explicit(&appForeground, foreground, memory_order_relaxed);
    wakeWorker();
}

void gpuPerformanceSetThermal(int status) {
    atomic_store_explicit(&thermalStatus, status, memory_order_relaxed);
    wakeWorker();
}

void gpuPerformanceSetRefreshRate(float refresh_rate) {
    if (refresh_rate < 30.0f || refresh_rate > 240.0f)
        refresh_rate = 60.0f;
    unsigned hz = (unsigned)(refresh_rate + 0.5f);
    atomic_store_explicit(&displayRefreshHz, hz, memory_order_relaxed);
    wakeWorker();
}

void gpuPerformanceNoteFrame(void) {
    if (atomic_load_explicit(&selectedMode, memory_order_relaxed) == GPU_PERF_OFF)
        return;
    atomic_store_explicit(&lastPresentedNs, monoNs(), memory_order_relaxed);
    atomic_fetch_add_explicit(&presentedCount, 1, memory_order_relaxed);
    wakeWorker();
}
