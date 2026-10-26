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
#include <GLES2/gl2.h>
#include <android/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define GPU_LOG(...) __android_log_print(ANDROID_LOG_INFO, "LorieGpuPerf", __VA_ARGS__)
#define GPU_ERR(...) __android_log_print(ANDROID_LOG_WARN, "LorieGpuPerf", __VA_ARGS__)

static _Atomic int selectedMode;
static _Atomic int thermalStatus;
static _Atomic bool appForeground;
static _Atomic uint64_t lastPresentedNs;
static _Atomic uint64_t presentedCount;
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
};

static void releasePbuffer(struct gpu_pbuffer *gpu) {
    if (gpu->display == EGL_NO_DISPLAY)
        return;
    eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gpu->surface != EGL_NO_SURFACE)
        eglDestroySurface(gpu->display, gpu->surface);
    if (gpu->context != EGL_NO_CONTEXT)
        eglDestroyContext(gpu->display, gpu->context);
    /* The compositor may use the same EGLDisplay in another thread. Avoid
     * eglTerminate here: never tear down its display from the helper. */
    gpu->surface = EGL_NO_SURFACE;
    gpu->context = EGL_NO_CONTEXT;
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
    GPU_LOG("EGL pbuffer ready: ES%d 16x16; all GPU vendors use the same path", esVersion);
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
    pthread_setname_np(pthread_self(), "LorieGpuPerformance");
    struct gpu_pbuffer gpu = { EGL_NO_DISPLAY, EGL_NO_SURFACE, EGL_NO_CONTEXT };
    bool initialized = false, wasActive = false;
    int previousMode = -1;
    unsigned delayUs = 400;
    uint64_t sampleNs = monoNs(), sampleCpu = cpuNs(), reportNs = sampleNs;
    uint64_t loops = 0, previousLoops = 0, oldFrames = 0;
    unsigned measuredCpu = 0;

    for (;;) {
        int mode = atomic_load_explicit(&selectedMode, memory_order_relaxed);
        bool foreground = atomic_load_explicit(&appForeground, memory_order_relaxed);
        int thermal = atomic_load_explicit(&thermalStatus, memory_order_relaxed);
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
            reportNs = now;
            oldFrames = atomic_load(&presentedCount);
            previousLoops = loops;
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

        if (mode != previousMode) {
            delayUs = mode == GPU_PERF_MAXIMUM ? 0 :
                      mode == GPU_PERF_PERFORMANCE ? 200 : 700;
            previousMode = mode;
            sampleNs = reportNs = now;
            sampleCpu = cpuNs();
            previousLoops = loops;
            oldFrames = atomic_load(&presentedCount);
            GPU_LOG("active mode=%s initial_delay_us=%u foreground=%d thermal=%d",
                    modeName(mode), delayUs, foreground, thermal);
        }
        wasActive = true;

        /* Change one pixel colour to avoid drivers discarding an identical
         * back-to-back clear. No window surface, swapchain or X11 locks. */
        uint64_t pulseStartNs = monoNs();
        uint64_t pulseCpuStartNs = cpuNs();
        glClearColor((float)(loops & 255) / 255.0f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        loops++;
        if ((loops & 1023u) == 0 && glGetError() != GL_NO_ERROR) {
            GPU_ERR("GLES context error; disabling assist (X11 unaffected)");
            releasePbuffer(&gpu);
            initialized = false;
            atomic_store(&selectedMode, GPU_PERF_OFF);
            continue;
        }

        if (mode != GPU_PERF_MAXIMUM) {
            /* Hard per-pulse CPU duty guard: feedback alone cannot protect the
             * CPU if a vendor's glFinish spins for more time than max_sleep_us.
             * Charge the helper's actual CPU time to the budget and yield the
             * difference. This preserves kernel scheduling for all OTHER tasks. */
            unsigned budget = gpuPerfLimits(mode).cpu_budget_percent;
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
            delayUs = gpuPerfNextSleep(mode, delayUs, measuredCpu);
            sampleNs = now;
            sampleCpu = c;
        }
        if (now - reportNs >= UINT64_C(5000000000)) {
            uint64_t totalFrames = atomic_load(&presentedCount);
            GPU_LOG("mode=%s pulses=%llu/5s cpu_pct=%u sleep_us=%u frames=%llu/5s "
                    "gpu_clock_raw=%ld thermal=%d foreground=%d",
                    modeName(mode), (unsigned long long)(loops - previousLoops),
                    measuredCpu, delayUs,
                    (unsigned long long)(totalFrames - oldFrames),
                    tryReadGpuClock(), thermal, foreground);
            oldFrames = totalFrames;
            previousLoops = loops;
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
    atomic_store_explicit(&selectedMode, mode, memory_order_relaxed);
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

void gpuPerformanceNoteFrame(void) {
    if (atomic_load_explicit(&selectedMode, memory_order_relaxed) == GPU_PERF_OFF)
        return;
    atomic_store_explicit(&lastPresentedNs, monoNs(), memory_order_relaxed);
    atomic_fetch_add_explicit(&presentedCount, 1, memory_order_relaxed);
    wakeWorker();
}
