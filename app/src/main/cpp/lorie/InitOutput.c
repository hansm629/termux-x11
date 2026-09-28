#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"
#pragma ide diagnostic ignored "cert-err34-c"
#pragma ide diagnostic ignored "ConstantConditionsOC"
#pragma ide diagnostic ignored "ConstantFunctionResult"
#pragma ide diagnostic ignored "bugprone-integer-division"
#pragma clang diagnostic ignored "-Wmissing-noreturn"
#pragma clang diagnostic ignored "-Wformat-nonliteral"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <time.h>
#include <sys/eventfd.h>
#include <sys/errno.h>
#include <libxcvt/libxcvt.h>
#include <X11/X.h>
#include <X11/Xmd.h>
#include <sys/wait.h>
#include <present.h>
#include <sys/mman.h>
#include <dri3.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include "fb.h"
#include "mipointer.h"
#include "micmap.h"
#include "miline.h"
#include "shmint.h"
#include "present_priv.h"
#include "misyncshm.h"
#include "glxserver.h"
#include "glxutil.h"
#include "fbconfigs.h"
#include "inpututils.h"
#include "exa.h"
#include "drm_fourcc.h"

#include "globals.h"
#include "property.h"
#include "lorie.h"

#define DRM_FORMAT_MOD_LINEAR 0

extern void android_shmem_sysv_shm_force(uint8_t enable);

#define unused __attribute__((unused))
#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

extern DeviceIntPtr lorieMouse, lorieKeyboard;

#define CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED 5

struct vblank {
    struct xorg_list link;
    uint64_t id, msc;
};

static struct present_screen_info loriePresentInfo;
static dri3_screen_info_rec lorieDri3Info;
static ExaDriverRec lorieExa;

typedef struct {
    DamagePtr damage;
    OsTimerPtr fpsTimer;

    SetWindowPixmapProcPtr SetWindowPixmap;
    CloseScreenProcPtr CloseScreen;

    int eventFd, stateFd;

    struct lorie_shared_server_state* state;
    struct {
        Bool legacyDrawing;
        uint32_t width, height;
        char name[1024];
        uint32_t framerate;
    } root;

    Bool dri3;
    Bool gpuPresentDisabled;

    uint64_t vblank_interval;
    struct xorg_list vblank_queue;
    uint64_t current_msc;

    uint64_t gpuCopySerialCounter;
    uint64_t rootGpuCopyPending;
} lorieScreenInfo;

ScreenPtr pScreenPtr;
static lorieScreenInfo lorieScreen = {
        .stateFd = -1,
        .root.width = 1280,
        .root.height = 1024,
        .root.framerate = 30,
        .root.name = "screen",
        .dri3 = TRUE,
        .vblank_queue = { &lorieScreen.vblank_queue, &lorieScreen.vblank_queue },
}, *pvfb = &lorieScreen;
static char *xstartup = NULL;

static inline __always_inline uint64_t lorieNowUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

// Interval between two frames being handed to us by a client.
static void lorieNotePresentSubmitted(void) {
    static uint64_t lastUs = 0;
    uint64_t nowUs = lorieNowUs();

    if (!pvfb->state)
        return;

    if (lastUs) {
        uint32_t gapUs = (uint32_t) (nowUs - lastUs);
        if (gapUs > pvfb->state->presentStats.submitGapMaxUs)
            pvfb->state->presentStats.submitGapMaxUs = gapUs;
    }
    lastUs = nowUs;
    pvfb->state->presentStats.presentSubmits++;
}

// Interval between two client present requests arriving, and how far ahead they aim.
void lorieNotePresentRequest(uint64_t target_msc, uint64_t crtc_msc) {
    static uint64_t lastUs = 0;
    uint64_t nowUs = lorieNowUs();
    uint32_t ahead;

    if (!pvfb->state)
        return;

    if (lastUs) {
        uint32_t gapUs = (uint32_t) (nowUs - lastUs);
        if (gapUs > pvfb->state->presentStats.requestGapMaxUs)
            pvfb->state->presentStats.requestGapMaxUs = gapUs;
    }
    lastUs = nowUs;
    pvfb->state->presentStats.requests++;

    ahead = target_msc > crtc_msc ? (uint32_t) (target_msc - crtc_msc) : 0;
    if (ahead > pvfb->state->presentStats.requestAheadMax)
        pvfb->state->presentStats.requestAheadMax = ahead;
}

// Interval between two presents becoming visible. Called from both completion paths below.
static void lorieNotePresentCompleted(void) {
    static uint64_t lastUs = 0;
    uint64_t nowUs = lorieNowUs();

    if (!pvfb->state)
        return;

    if (lastUs) {
        uint32_t gapUs = (uint32_t) (nowUs - lastUs);
        pvfb->state->presentStats.presentGapSumUs += gapUs;
        if (gapUs > pvfb->state->presentStats.presentGapMaxUs)
            pvfb->state->presentStats.presentGapMaxUs = gapUs;
        // Two frame periods at 60Hz: late enough that the content visibly missed its slot.
        if (gapUs > 33000)
            pvfb->state->presentStats.presentGapsLate++;
    }
    lastUs = nowUs;
    pvfb->state->presentStats.presentCompletions++;
}


// Root double buffering. On its own it only moved the X server's wait around, but it is what lets
// the renderer stop waiting for its own fence inside every frame (see rendererRetirePreviousFrame):
// the buffer can be handed back a frame later instead of synchronously, which is the whole point.
// -single-root-buffer restores the old synchronous behaviour.
static Bool lorieSingleRootBuffer = FALSE;

// The display's own clock.
//
// msc used to be advanced from two places - once per vsync here, and again whenever a flip
// completed - so it was not a count of anything in particular, and presents that wait on it waited
// for an arbitrary amount of time. It now counts vsyncs and nothing else: a present completing is
// not a vsync, and making it look like one is what let the counter drift away from the display.
//
// The timestamp is taken with clock_gettime() inside the Choreographer callback rather than from
// the frameTimeNanos argument the callback is handed. The NDK declares that argument as `long`,
// which is 32 bits on armeabi-v7a and x86 - both of which we ship - so it wraps every 4.3 seconds
// there. AChoreographer_postFrameCallback64() passes it intact but needs API 29 against our
// minSdk 26, and that would mean two clocks with different accuracy depending on the device.
// Reading the clock at callback entry costs the dispatch latency, is the same everywhere, and is
// still far closer to the vsync than asking for the time whenever the X server got round to it.
//
// Written by the Choreographer thread, read by the X server thread.
static volatile uint64_t lorieVsyncStampUs = 0;

// X server thread only, from lorieRedraw onwards.
static uint64_t lorieVsyncRawUs = 0;    // the newest stamp the Choreographer has given us
static uint64_t lorieVsyncUs = 0;       // when the vsync that current_msc counts happened
static uint64_t lorieVsyncPeriodUs = 16667;

static void lorieAdvanceVsyncClock(void) {
    uint64_t us = __atomic_load_n(&lorieVsyncStampUs, __ATOMIC_ACQUIRE);

    if (us > lorieVsyncRawUs) {
        uint64_t delta = us - lorieVsyncRawUs;
        // A tick we were never called for stretches the gap, so only believe plausible ones.
        // 4-40 ms covers everything from 25 to 250 Hz.
        if (lorieVsyncRawUs && delta >= 4000 && delta <= 40000)
            lorieVsyncPeriodUs = (lorieVsyncPeriodUs * 7 + delta) / 8;
        lorieVsyncRawUs = us;
    }

    // current_msc is about to count one more vsync, so ust moves with it. Normally that is the
    // stamp we were just given; if the work proc ran twice for one callback there is no new one,
    // and stepping a period keeps the two in step until the next callback resyncs them.
    lorieVsyncUs = lorieVsyncRawUs > lorieVsyncUs ? lorieVsyncRawUs
                 : lorieVsyncUs ? lorieVsyncUs + lorieVsyncPeriodUs
                 : GetTimeInMicros();
}

// When the given vsync is, or was, on screen.
static uint64_t lorieUstForMsc(uint64_t msc) {
    if (msc >= pvfb->current_msc)
        return lorieVsyncUs + (msc - pvfb->current_msc) * lorieVsyncPeriodUs;

    uint64_t back = (pvfb->current_msc - msc) * lorieVsyncPeriodUs;
    return back < lorieVsyncUs ? lorieVsyncUs - back : 0;
}

// Owned by the activity process, handed to us over the connection socket. Points at a placeholder until
// the first connection so callers don't need a NULL check.
static pthread_cond_t rendererCondPlaceholder = PTHREAD_COND_INITIALIZER;
static pthread_cond_t* volatile rendererCond = &rendererCondPlaceholder;

typedef struct {
    LorieBuffer *buffer;
    bool flipped, wasLocked, imported;
    void *locked;
    void *mem;

    /*
     * Root window only: the buffers the root rotates through (see the rootHandover comment in
     * lorie.h). rootWrite is the one being drawn into; the renderer takes the one published last.
     * rootStale[i] is the region slot i has not received yet, copied into it when it becomes the
     * drawing target.
     */
    LorieBuffer *rootBuf[LORIE_ROOT_SLOTS];
    void *rootLocked[LORIE_ROOT_SLOTS];
    RegionRec rootStale[LORIE_ROOT_SLOTS];
    int rootWrite;                  // the slot we are drawing into; only this side ever changes it
    Bool rootDouble;
} LoriePixmapPriv;

static void lorieCopyRootRegion(LoriePixmapPriv *priv, int from, int to, RegionPtr region);
static void lorieEnsureRootDoubleBuffer(PixmapPtr root);
static Bool lorieRootHandover(LoriePixmapPriv *priv);
static inline int lorieRootSampledIndex(void);
static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region);

#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap)) : NULL)
#define LORIE_BUFFER_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap))->buffer : NULL)

static LorieBuffer *lorieEnsureGpuSampleable(PixmapPtr pixmap, int8_t type) {
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap);
    const LorieBuffer_Desc *desc;
    if (!priv || !priv->buffer || priv->mem)
        return NULL;

    desc = LorieBuffer_description(priv->buffer);
    if (desc->type == LORIEBUFFER_REGULAR) {
        LorieBuffer_convert(priv->buffer, type, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
        if (desc->type != LORIEBUFFER_REGULAR) {
            // LorieBuffer_convert does not report status but it does not let the type change in the case of error.
            pScreenPtr->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, desc->stride * 4, NULL);
            LorieBuffer_lock(priv->buffer, &priv->locked);
        }
    }

    return desc->type == type ? priv->buffer : NULL;
}

static Bool lorieServerDebugEnabled = FALSE;

void OsVendorInit(void) {
    pthread_mutexattr_t mutex_attr;

    if (lorieScreen.stateFd != -1) // already initialized
        return;

    lorieServerDebugEnabled = getenv("TERMUX_X11_DEBUG") != NULL;

    if (-1 == (lorieScreen.stateFd = LorieBuffer_createRegion("xserver", sizeof(*lorieScreen.state)))) {
        dprintf(2, "FATAL: Failed to allocate server state.\n");
        _exit(1);
    }

    if (!(lorieScreen.state = mmap(NULL, sizeof(*lorieScreen.state), PROT_READ|PROT_WRITE, MAP_SHARED, lorieScreen.stateFd, 0))) {
        dprintf(2, "FATAL: Failed to map server state.\n");
        _exit(1);
    }

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lorieScreen.state->lock, &mutex_attr);
    pthread_mutex_init(&lorieScreen.state->cursor.lock, &mutex_attr);
}

// Queued from handleLorieEvents (input thread) to run on the main thread, i.e. the same thread that
// signals rendererCond from lorieRedraw/lorieMoveCursor - so the swap and the unmap below can never race
// a signal.
static Bool lorieSetRendererWakeupCondWorkProc(__unused ClientPtr client, void* closure) {
    int fd = (int) (intptr_t) closure;
    pthread_cond_t* newCond = mmap(NULL, sizeof(pthread_cond_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // mmap already keeps the region alive.
    if (newCond == MAP_FAILED) {
        log(ERROR, "Failed to map renderer wakeup cond var, keeping the old one");
        return TRUE;
    }

    pthread_cond_t* old = rendererCond;
    rendererCond = newCond;
    pthread_cond_signal(newCond); // in case a signal was sent to `old` right before this swap

    if (old != &rendererCondPlaceholder)
        munmap(old, sizeof(pthread_cond_t));

    return TRUE;
}

void lorieSetRendererWakeupCond(int fd) {
    QueueWorkProc(lorieSetRendererWakeupCondWorkProc, NULL, (void*) (intptr_t) fd);
    lorieWakeServer();
}

void lorieActivityConnected(void) {
    pvfb->state->drawRequested = pvfb->state->cursor.updated = true;
    lorieSendSharedServerState(pvfb->stateFd);
    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));
}

static LoriePixmapPriv* lorieRootWindowPixmapPriv(void) {
    void* devPriv = pScreenPtr ? pScreenPtr->devPrivate : NULL;
    return devPriv ? exaGetPixmapDriverPrivate(devPriv) : NULL;
}

static Bool TrueNoop() { return TRUE; }
static Bool FalseNoop() { return FALSE; }
static void VoidNoop() {}

void ddxGiveUp(unused enum ExitCode error) {
    log(ERROR, "Server stopped (%d)", error);
    CloseWellKnownConnections();
    UnlockServer();
    exit(error);
}

static void* ddxReadyThread(unused void* cookie) {
    if (xstartup && serverGeneration == 1) {
        pid_t pid = fork();

        if (!pid) {
            char DISPLAY[16] = "";
            sprintf(DISPLAY, ":%s", display);
            setenv("DISPLAY", DISPLAY, 1);

#define INHERIT_VAR(v) char *v = getenv("XSTARTUP_" #v); if (v && strlen(v)) setenv(#v, v, 1); unsetenv("XSTARTUP_" #v);
            INHERIT_VAR(CLASSPATH)
            INHERIT_VAR(LD_LIBRARY_PATH)
            INHERIT_VAR(LD_PRELOAD)
#undef INHERIT_VAR

            execlp(xstartup, xstartup, NULL);
            execlp("sh", "sh", "-c", xstartup, NULL);
            dprintf(2, "Failed to start command `sh -c \"%s\"`: %s\n", xstartup, strerror(errno));
            abort();
        } else {
            int status;
            do {
                pid_t w = waitpid(pid, &status, 0);
                if (w == -1) {
                    perror("waitpid");
                    GiveUp(SIGKILL);
                }

                if (WIFEXITED(status)) {
                    printf("%d exited, status=%d\n", w, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("%d killed by signal %d\n", w, WTERMSIG(status));
                } else if (WIFSTOPPED(status)) {
                    printf("%d stopped by signal %d\n", w, WSTOPSIG(status));
                } else if (WIFCONTINUED(status)) {
                    printf("%d continued\n", w);
                }
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
            GiveUp(SIGINT);
        }
    }

    return NULL;
}

void drawSquare(int x, int y, int l, uint32_t color, uint32_t stride, uint32_t* pixels) {
    for (int i=0; i<l; i++) for (int j=0; j<l; j++)
        pixels[(j+y)*stride + x + i] = color;
}

Bool drawSquares() {
    LoriePixmapPriv* priv = lorieRootWindowPixmapPriv();
    uint32_t* pixels = !priv ? NULL : priv->locked;
    if (pixels) {
        const LorieBuffer_Desc *d = LorieBuffer_description(priv->buffer);
        int l = min(d->width, d->height) / 4, x = (d->width - l)/2, y = (d->height - l)/2;

        drawSquare(x - l/3, y - l/3, l, 0x00FF0000, d->stride, pixels);
        drawSquare(x, y, l, 0x0000FF00, d->stride, pixels);
        drawSquare(x + l/3, y + l/3, l, 0x000000FF, d->stride, pixels);
    }

    return FALSE;
}

void ddxReady(void) {
    CursorVisible = TRUE;
    pScreenPtr->DisplayCursor(lorieMouse, pScreenPtr, rootCursor);
    if (NoListenAll)
        return;
    if (xstartup && !strlen(xstartup)) // allow overriding $TERMUX_X11_XSTARTUP with empty xstartup arg
        return;
    if (!xstartup || !strlen(xstartup))
        xstartup = getenv("TERMUX_X11_XSTARTUP");
    if (!xstartup || !strlen(xstartup))
        return;

    pthread_t t;
    pthread_create(&t, NULL, ddxReadyThread, NULL);
}

void OsVendorFatalError(unused const char *f, unused va_list args) {
    log(ERROR, f, args);
}

#if defined(DDXBEFORERESET)
void ddxBeforeReset(void) {}
#endif

#if INPUTTHREAD
/** This function is called in Xserver/os/inputthread.c when starting
    the input thread. */
void ddxInputThreadInit(void) {}
#endif

void ddxUseMsg(void) {
    ErrorF("-xstartup \"command\"    start `command` after server startup\n");
    ErrorF("-legacy-drawing        use legacy drawing, without using AHardwareBuffers\n");
    ErrorF("-force-bgra            force flipping colours (RGBA->BGRA)\n");
    ErrorF("-disable-dri3          disabling DRI3 support (to let lavapipe work)\n");
    ErrorF("-force-sysvshm         force using SysV shm syscalls\n");
    ErrorF("-check-drawing         run server only able to draw some test image (for testing if rendering root window works or not),\n");
    ErrorF("-disable-gpu-present   disable offloading Present copies to the GPU, always use the CPU path\n");
}

int ddxProcessArgument(unused int argc, unused char *argv[], unused int i) {
    if (strcmp(argv[i], "-xstartup") == 0) {  /* -xstartup "command" */
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        xstartup = argv[++i];
        return 2;
    }

    if (strcmp(argv[i], "-legacy-drawing") == 0) {
        pvfb->root.legacyDrawing = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-force-bgra") == 0)
        return 1;

    if (strcmp(argv[i], "-disable-dri3") == 0) {
        pvfb->dri3 = FALSE;
        return 1;
    }

    if (strcmp(argv[i], "-force-sysvshm") == 0) {
        android_shmem_sysv_shm_force(1);
        return 1;
    }

    if (strcmp(argv[i], "-check-drawing") == 0) {
        NoListenAll = TRUE;
        QueueWorkProc(drawSquares, NULL, NULL);
        return 1;
    }

    if (strcmp(argv[i], "-single-root-buffer") == 0) {
        lorieSingleRootBuffer = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-double-root-buffer") == 0) {
        lorieSingleRootBuffer = FALSE;
        return 1;
    }

    if (strcmp(argv[i], "-disable-gpu-present") == 0) {
        pvfb->gpuPresentDisabled = TRUE;
        return 1;
    }

    return 0;
}

static RRModePtr lorieCvt(int width, int height, int framerate) {
    struct libxcvt_mode_info *info;
    char name[128];
    xRRModeInfo modeinfo = {0};
    RRModePtr mode;

    info = libxcvt_gen_mode_info(width, height, framerate, 0, 0);

    snprintf(name, sizeof name, "%dx%d", info->hdisplay, info->vdisplay);
    modeinfo.nameLength = strlen(name);
    modeinfo.width      = info->hdisplay;
    modeinfo.height     = info->vdisplay;
    modeinfo.dotClock   = info->dot_clock * 1000.0;
    modeinfo.hSyncStart = info->hsync_start;
    modeinfo.hSyncEnd   = info->hsync_end;
    modeinfo.hTotal     = info->htotal;
    modeinfo.vSyncStart = info->vsync_start;
    modeinfo.vSyncEnd   = info->vsync_end;
    modeinfo.vTotal     = info->vtotal;
    modeinfo.modeFlags  = info->mode_flags;

    mode = RRModeGet(&modeinfo, name);
    free(info);
    return mode;
}

static void lorieMoveCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, int x, int y) {
    pvfb->state->cursor.x = x;
    pvfb->state->cursor.y = y;
    pvfb->state->cursor.moved = TRUE;
    pvfb->state->presentStats.pointerMoves++;
    // No need to explicitly lock the mutex, it will cause waiting for rendering to be finished.
    // We are simply signaling the renderer in the case if it sleeps.
    pthread_cond_signal(rendererCond);
}

static void lorieConvertCursor(CursorPtr pCurs, uint32_t *data) {
    CursorBitsPtr bits = pCurs->bits;
    if (bits->argb) {
        for (int i = 0; i < bits->width * bits->height; i++) {
            /* Convert bgra to rgba */
            CARD32 p = bits->argb[i];
            data[i] = (p & 0xFF000000) | ((p & 0x00FF0000) >> 16) | (p & 0x0000FF00) | ((p & 0x000000FF) << 16);
        }
    } else {
        uint32_t d, fg, bg, *p;
        int x, y, stride, i, bit;

        p = data;
        fg = ((pCurs->foreBlue & 0xff00) << 8) | (pCurs->foreGreen & 0xff00) | (pCurs->foreRed >> 8);
        bg = ((pCurs->backBlue & 0xff00) << 8) | (pCurs->backGreen & 0xff00) | (pCurs->backRed >> 8);
        stride = BitmapBytePad(bits->width);
        for (y = 0; y < bits->height; y++)
            for (x = 0; x < bits->width; x++) {
                i = y * stride + x / 8;
                bit = 1 << (x & 7);
                d = (bits->source[i] & bit) ? fg : bg;
                d = (bits->mask[i] & bit) ? d | 0xff000000 : 0x00000000;
                *p++ = d;
            }
    }
}

static uint32_t lorieLastCursorChecksum = 0;

static void lorieSetCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, CursorPtr pCurs, int x0, int y0) {
    CursorBitsPtr bits = pCurs ? pCurs->bits : NULL;
    if (pCurs && (pCurs->bits->width >= 512 || pCurs->bits->height >= 512))
        // We do not have enough memory allocated for such a big cursor, let's display default "X" cursor
        pCurs = rootCursor;

    lorie_mutex_lock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid);
    if (pCurs && bits) {
        uint32_t sum = 2166136261u;
        int i, pixels;

        pvfb->state->cursor.xhot = bits->xhot;
        pvfb->state->cursor.yhot = bits->yhot;
        pvfb->state->cursor.width = bits->width;
        pvfb->state->cursor.height = bits->height;
        lorieConvertCursor(pCurs, (uint32_t *) pvfb->state->cursor.bits);

        /*
         * X hands us a cursor again on every crossing, a grab, a pointer confine - usually the very
         * same image. Uploading it to the GPU each time means an out of band texture update in the
         * middle of a frame, so only say it changed when it actually did.
         */
        pixels = bits->width * bits->height;
        for (i = 0; i < pixels; i++)
            sum = (sum ^ pvfb->state->cursor.bits[i]) * 16777619u;
        sum ^= ((uint32_t) bits->width << 16) ^ (uint32_t) bits->height;

        if (sum != lorieLastCursorChecksum) {
            lorieLastCursorChecksum = sum;
            pvfb->state->cursor.updated = true;
        }
    } else {
        pvfb->state->cursor.xhot = pvfb->state->cursor.yhot = 0;
        pvfb->state->cursor.width = pvfb->state->cursor.height = 0;
        lorieLastCursorChecksum = 0;
    }
    // The hot spot or the position can change without the image doing so, and that still has to be
    // redrawn - but it does not need an upload.
    pvfb->state->cursor.moved = TRUE;
    lorie_mutex_unlock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid);

    lorieMoveCursor(NULL, NULL, x0, y0);
}

static miPointerSpriteFuncRec loriePointerSpriteFuncs = {
    .RealizeCursor = TrueNoop,
    .UnrealizeCursor = TrueNoop,
    .SetCursor = lorieSetCursor,
    .MoveCursor = lorieMoveCursor,
    .DeviceCursorInitialize = TrueNoop,
    .DeviceCursorCleanup = VoidNoop
};

static miPointerScreenFuncRec loriePointerCursorFuncs = {
    .CursorOffScreen = FalseNoop,
    .CrossScreen = VoidNoop,
    .WarpCursor = miPointerWarpCursor
};

static void loriePerformVblanks(void);

static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {
    static uint64_t lastRedrawUs = 0;
    uint64_t nowUs = lorieNowUs();
    int status, nonEmpty;

    // The renderer ticks this once per vsync. A gap much longer than a frame means the X server
    // thread was stuck doing something else - which is exactly what a visible hitch is.
    if (lastRedrawUs && pvfb->state) {
        uint32_t gapUs = (uint32_t) (nowUs - lastRedrawUs);
        if (gapUs > pvfb->state->presentStats.xDispatchMaxUs)
            pvfb->state->presentStats.xDispatchMaxUs = gapUs;
    }
    lastRedrawUs = nowUs;
    LoriePixmapPriv* priv;
    PixmapPtr root = pScreenPtr && pScreenPtr->root ? pScreenPtr->GetWindowPixmap(pScreenPtr->root) : NULL;

    lorieAdvanceVsyncClock();
    pvfb->current_msc++;
    loriePerformVblanks();

    pvfb->state->waitForNextFrame = false;

    if (!lorieConnectionAlive() || !pvfb->state->surfaceAvailable)
        return TRUE;

    nonEmpty = RegionNotEmpty(DamageRegion(pvfb->damage));
    priv = root ? exaGetPixmapDriverPrivate(root) : NULL;

    if (!priv)
        // Impossible situation, but let's skip this step
        return TRUE;

    if (nonEmpty && priv->buffer) {
        // We should unlock and lock buffer in order to update texture content on some devices
        // In most cases AHardwareBuffer uses DMA memory which is shared between CPU and GPU
        // and this is not needed. But according to docs we should do it for any case.
        // Also according to AHardwareBuffer docs simultaneous reading in rendering thread and
        // locking for writing in other thread is fine.
        if (priv->locked) {
            // Remapping the root for every frame with damage: gralloc can make this a cache
            // maintenance pass over the whole buffer, on this thread, which nothing else measures.
            uint64_t remapStartUs = lorieNowUs();
            LorieBuffer_unlock(priv->buffer);
            status = LorieBuffer_lock(priv->buffer, &priv->locked);
            pvfb->state->presentStats.rootRemapUs += (uint32_t) (lorieNowUs() - remapStartUs);
            pvfb->state->presentStats.rootRemaps++;
            if (status)
                FatalError("Failed to lock the surface: %d\n", status);
        }

        lorieEnsureRootDoubleBuffer(root);
        if (priv->rootDouble)
            lorieMarkRootStale(priv, DamageRegion(pvfb->damage));

        DamageEmpty(pvfb->damage);
        lorieRootHandover(priv);
        pvfb->state->drawRequested = TRUE;
    }

    if (pvfb->state->drawRequested || pvfb->state->cursor.moved || pvfb->state->cursor.updated) {
        // With a double buffered root this must name the buffer the renderer samples, not the one
        // we draw into.
        // While a client is flipping, the id above is that client's pixmap rather than one of our
        // slots, so the renderer must not go looking in the slot table for it either.
        pvfb->state->rootDoubleBuffered = priv->rootDouble ? 1 : 0;
        pvfb->state->rootWindowTextureID = LorieBuffer_description(
                priv->rootDouble ? priv->rootBuf[lorieRootSampledIndex()] : priv->buffer)->id;

        // Sending signal about pending root window changes to renderer thread.
        // We do not explicitly lock the pvfb->state->lock here because we do not want to wait
        // for all drawing operations to be finished.
        // Renderer thread will check the `drawRequested` flag right before going to sleep.
        pthread_cond_signal(rendererCond);
    }

    return TRUE;
}

static uint64_t gpuCopyAttempts = 0, gpuCopyOffloads = 0;


static CARD32 lorieFramecounter(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {
    uint32_t samples;
    static Bool driverLogged = FALSE;

    if (!driverLogged && pvfb->state->rendererDriver[0]) {
        driverLogged = TRUE;
        log(INFO, "XlorieFrames: renderer GLES driver: %s", (const char *) pvfb->state->rendererDriver);
    }

    if (pvfb->state->renderedFrames || gpuCopyAttempts)
        log(INFO, gpuCopyAttempts ? "%d frames in 5.0 seconds = %.1f FPS, %llu/%llu present copies offloaded to GPU"
                                   : "%d frames in 5.0 seconds = %.1f FPS",
            pvfb->state->renderedFrames, ((float) pvfb->state->renderedFrames) / 5,
            (unsigned long long) gpuCopyOffloads, (unsigned long long) gpuCopyAttempts);

    /*
     * The FPS above counts renderer redraws, which says nothing about how evenly they landed - a
     * high number with a visibly stuttering picture is exactly what long frames look like. These
     * come from the renderer through the shared state, since its own logcat is unreachable from
     * here.
     */
    samples = pvfb->state->presentStats.frameSamples;
    if (samples) {
        log(INFO, "XlorieFrames: frame avg %.1f ms, max %.1f ms, hitches(>=%d ms) %u, "
                  "fence wait %.1f ms total (worst %.1f ms), copies on %u frames, coalesced %u",
            (double) pvfb->state->presentStats.frameSumUs / samples / 1000.0,
            pvfb->state->presentStats.maxFrameUs / 1000.0,
            LORIE_LONG_FRAME_US / 1000,
            pvfb->state->presentStats.longFrames,
            pvfb->state->presentStats.fenceWaitUs / 1000.0,
            pvfb->state->presentStats.fenceWaitMaxUs / 1000.0,
            pvfb->state->presentStats.gpuCopyFrames,
            pvfb->state->presentStats.coalescedFrames);
        log(INFO, "XlorieLock: renderer held the root lock %.1f%% of the time (%.0f ms), "
                  "X server blocked on it %.0f ms over %u accesses (worst %.1f ms), "
                  "cursor-only frames %u (overlay %u) of %u pointer moves, display %.1f Hz",
            pvfb->state->presentStats.lockHeldUs / 50000.0,
            pvfb->state->presentStats.lockHeldUs / 1000.0,
            pvfb->state->presentStats.xLockWaitUs / 1000.0,
            pvfb->state->presentStats.xLockWaits,
            pvfb->state->presentStats.xLockWaitMaxUs / 1000.0,
            pvfb->state->presentStats.cursorOnlyFrames,
            pvfb->state->presentStats.cursorOverlayMoves,
            pvfb->state->presentStats.pointerMoves,
            pvfb->state->presentStats.displayRefreshMHz / 1000.0);
        if (pvfb->state->presentStats.presentCompletions > 1) {
            log(INFO, "XloriePresent: %u client presents reached the screen in 5.0 s "
                      "(avg %.1f ms apart, longest %.1f ms, %u later than 33 ms)",
                pvfb->state->presentStats.presentCompletions,
                pvfb->state->presentStats.presentGapSumUs / 1000.0 /
                    (pvfb->state->presentStats.presentCompletions - 1),
                pvfb->state->presentStats.presentGapMaxUs / 1000.0,
                pvfb->state->presentStats.presentGapsLate);
            log(INFO, "XloriePresent: %u submitted by clients (longest gap between submissions %.1f ms)",
                pvfb->state->presentStats.presentSubmits,
                pvfb->state->presentStats.submitGapMaxUs / 1000.0);
        }
        if (pvfb->state->presentStats.zeroCopyFrames || pvfb->state->presentStats.zeroCopyStalls)
            log(INFO, "XlorieZeroCopy: %u root buffers handed to the compositor with no GL, %u frames dropped waiting for one back",
                pvfb->state->presentStats.zeroCopyFrames, pvfb->state->presentStats.zeroCopyStalls);

        if (pvfb->state->presentStats.requests)
            log(INFO, "XlorieRequest: %u arrived, longest gap between arrivals %.1f ms, furthest target +%u vsyncs",
                pvfb->state->presentStats.requests,
                pvfb->state->presentStats.requestGapMaxUs / 1000.0,
                pvfb->state->presentStats.requestAheadMax);

        if (pvfb->state->presentStats.copyCompletions)
            log(INFO, "XlorieCopy: %u copies took avg %.1f ms, longest %.1f ms, found unfinished %u times",
                pvfb->state->presentStats.copyCompletions,
                pvfb->state->presentStats.copyLatencySumUs / 1000.0 / pvfb->state->presentStats.copyCompletions,
                pvfb->state->presentStats.copyLatencyMaxUs / 1000.0,
                pvfb->state->presentStats.copyRequeues);

        if (pvfb->state->presentStats.copyDeferrals || pvfb->state->presentStats.copySkips)
            log(INFO, "XloriePresent: %u copies deferred for a late buffer, %u given up on",
                pvfb->state->presentStats.copyDeferrals, pvfb->state->presentStats.copySkips);
        log(INFO, "XlorieStall: root remap %.1f ms over %u frames, longest X server gap %.1f ms",
            pvfb->state->presentStats.rootRemapUs / 1000.0,
            pvfb->state->presentStats.rootRemaps,
            pvfb->state->presentStats.xDispatchMaxUs / 1000.0);
        if (pvfb->state->presentStats.cursorUploads)
            log(INFO, "XlorieLock: cursor image uploaded %u times, %.1f ms total",
                pvfb->state->presentStats.cursorUploads,
                pvfb->state->presentStats.cursorUploadUs / 1000.0);
    }

    pvfb->state->presentStats.frameSamples = 0;
    pvfb->state->presentStats.frameSumUs = 0;
    pvfb->state->presentStats.maxFrameUs = 0;
    pvfb->state->presentStats.longFrames = 0;
    pvfb->state->presentStats.fenceWaitUs = 0;
    pvfb->state->presentStats.gpuCopyFrames = 0;
    pvfb->state->presentStats.coalescedFrames = 0;
    pvfb->state->presentStats.lockHeldUs = 0;
    pvfb->state->presentStats.cursorOnlyFrames = 0;
    pvfb->state->presentStats.xLockWaitMaxUs = 0;
    pvfb->state->presentStats.fenceWaitMaxUs = 0;
    pvfb->state->presentStats.xLockWaitUs = 0;
    pvfb->state->presentStats.xLockWaits = 0;
    pvfb->state->presentStats.pointerMoves = 0;
    pvfb->state->presentStats.cursorUploads = 0;
    pvfb->state->presentStats.cursorUploadUs = 0;
    pvfb->state->presentStats.rootRemapUs = 0;
    pvfb->state->presentStats.rootRemaps = 0;
    pvfb->state->presentStats.xDispatchMaxUs = 0;
    pvfb->state->presentStats.presentCompletions = 0;
    pvfb->state->presentStats.presentGapSumUs = 0;
    pvfb->state->presentStats.presentGapMaxUs = 0;
    pvfb->state->presentStats.zeroCopyFrames = 0;
    pvfb->state->presentStats.zeroCopyStalls = 0;
    pvfb->state->presentStats.cursorOverlayMoves = 0;
    pvfb->state->presentStats.requests = 0;
    pvfb->state->presentStats.requestGapMaxUs = 0;
    pvfb->state->presentStats.requestAheadMax = 0;
    pvfb->state->presentStats.copyLatencySumUs = 0;
    pvfb->state->presentStats.copyLatencyMaxUs = 0;
    pvfb->state->presentStats.copyCompletions = 0;
    pvfb->state->presentStats.copyRequeues = 0;
    pvfb->state->presentStats.copyDeferrals = 0;
    pvfb->state->presentStats.copySkips = 0;
    pvfb->state->presentStats.presentGapsLate = 0;
    pvfb->state->presentStats.presentSubmits = 0;
    pvfb->state->presentStats.submitGapMaxUs = 0;

    pvfb->state->renderedFrames = 0;
    gpuCopyAttempts = gpuCopyOffloads = 0;
    return 5000;
}

static Bool lorieCreateScreenResources(ScreenPtr pScreen) {
    pScreen->devPrivate = pScreen->CreatePixmap(pScreen, pScreen->width, pScreen->height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, pvfb->damage);
    pvfb->fpsTimer = TimerSet(NULL, 0, 5000, lorieFramecounter, pScreen);

    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    return TRUE;
}

static Bool lorieCloseScreen(ScreenPtr pScreen) {
    pScreenPtr = NULL;
    pScreen->DestroyPixmap(pScreen->devPrivate);
    pScreen->devPrivate = NULL;
    pScreen->CloseScreen = pvfb->CloseScreen;
    return pScreen->CloseScreen(pScreen);
}

void lorieSetWindowPixmap(WindowPtr pWindow, PixmapPtr newPixmap) {
    bool isRoot = pWindow == pScreenPtr->root;
    PixmapPtr oldPixmap = isRoot ? pScreenPtr->GetWindowPixmap(pWindow) : NULL;
    LoriePixmapPriv *old, *new;
    if (isRoot) {
        old = LORIE_PIXMAP_PRIV_FROM_PIXMAP(oldPixmap);
        new = LORIE_PIXMAP_PRIV_FROM_PIXMAP(newPixmap);
        if (old && old->buffer && old->locked) {
            LorieBuffer_unlock(old->buffer);
            old->locked = NULL;
            old->wasLocked = false;
        }
        if (new && new->buffer && !new->locked) {
            LorieBuffer_lock(new->buffer, &new->locked);
            new->wasLocked = false;
        }
    }

    pScreenPtr->SetWindowPixmap = pvfb->SetWindowPixmap;
    (*pScreenPtr->SetWindowPixmap) (pWindow, newPixmap);
    pvfb->SetWindowPixmap = pScreenPtr->SetWindowPixmap;
    pScreenPtr->SetWindowPixmap = lorieSetWindowPixmap;
}

static int lorieSetPixmapVisitWindow(WindowPtr window, void *data) {
    ScreenPtr screen = window->drawable.pScreen;

    if (screen->GetWindowPixmap(window) == data) {
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }

    return WT_DONTWALKCHILDREN;
}

static Bool lorieRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height, unused CARD32 mmWidth, unused CARD32 mmHeight) {
    PixmapPtr oldPixmap, newPixmap;
    BoxRec box = { 0, 0, width, height };

    // Drain all pending vblanks.
    loriePerformVblanks();

    // Restore root window pixmap.
    present_restore_screen_pixmap(pScreenPtr);

    SetRootClip(pScreen, ROOT_CLIP_NONE);

    pScreen->root->drawable.width = pvfb->root.width = pScreen->width = width;
    pScreen->root->drawable.height = pvfb->root.height = pScreen->height = height;
    pScreen->mmWidth = ((double) (width)) * 25.4 / monitorResolution;
    pScreen->mmHeight = ((double) (height)) * 25.4 / monitorResolution;

    oldPixmap = pScreen->GetScreenPixmap(pScreen);
    newPixmap = pScreen->CreatePixmap(pScreen, width, height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);
    pScreen->SetScreenPixmap(newPixmap);
    if (pvfb->damage) {
        DamageUnregister(pvfb->damage);
        DamageDestroy(pvfb->damage);
    }

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&newPixmap->drawable, pvfb->damage);

    if (oldPixmap) {
        GCPtr gc = GetScratchGC(newPixmap->drawable.depth, pScreen);
        if (gc) {
            ValidateGC(&newPixmap->drawable, gc);
            gc->ops->CopyArea(&oldPixmap->drawable, &newPixmap->drawable, gc, 0, 0, min(oldPixmap->drawable.width, newPixmap->drawable.width), min(oldPixmap->drawable.height, newPixmap->drawable.height), 0, 0);
            FreeScratchGC(gc);
        }
        TraverseTree(pScreen->root, lorieSetPixmapVisitWindow, oldPixmap);
        pScreen->DestroyPixmap(oldPixmap);
    }

    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    pScreen->ResizeWindow(pScreen->root, 0, 0, width, height, NULL);
    RegionReset(&pScreen->root->winSize, &box);

    SetRootClip(pScreen, ROOT_CLIP_FULL);

    RRScreenSizeNotify(pScreen);
    update_desktop_dimensions();
    pvfb->state->cursor.moved = TRUE;

    return TRUE;
}

static Bool lorieRRCrtcSet(unused ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x, int y,
               Rotation rotation, int numOutput, RROutputPtr *outputs) {
    return (crtc && mode) ? RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutput, outputs) : FALSE;
}

static Bool lorieRRGetInfo(unused ScreenPtr pScreen, Rotation *rotations) {
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool lorieRandRInit(ScreenPtr pScreen) {
    rrScrPrivPtr pScrPriv;
    RROutputPtr output;
    RRCrtcPtr crtc;
    RRModePtr mode;

    if (!RRScreenInit(pScreen))
       return FALSE;

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = lorieRRGetInfo;
    pScrPriv->rrCrtcSet = lorieRRCrtcSet;
    pScrPriv->rrScreenSetSize = lorieRRScreenSetSize;

    RRScreenSetSizeRange(pScreen, 1, 1, 32767, 32767);

    if (FALSE
        || !(mode = lorieCvt(pScreen->width, pScreen->height, pvfb->root.framerate))
        || !(crtc = RRCrtcCreate(pScreen, NULL))
        || !RRCrtcGammaSetSize(crtc, 256)
        || !(output = RROutputCreate(pScreen, pvfb->root.name, sizeof(pvfb->root.name), NULL))
        || (output->nameLength = strlen(output->name), FalseNoop())
        || !RROutputSetClones(output, NULL, 0)
        || !RROutputSetModes(output, &mode, 1, 0)
        || !RROutputSetCrtcs(output, &crtc, 1)
        || !RROutputSetConnection(output, RR_Connected)
        || !RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output))
        return FALSE;
    return TRUE;
}

void lorieWakeServer(void) {
    // Wake the server if it sleeps.
    eventfd_write(pvfb->eventFd, 1);
}

static void lorieWorkingQueueCallback(int fd, int __unused ready, void __unused *data) {
    // Nothing to do here. It is needed to interrupt ospoll_wait.
    eventfd_t dummy;
    eventfd_read(fd, &dummy);
}

void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d) {
    AChoreographer_postFrameCallback(d, (AChoreographer_frameCallback) lorieChoreographerFrameCallback, d);
    // t is deliberately unused - see lorieVsyncStampUs.
    __atomic_store_n(&lorieVsyncStampUs, lorieNowUs(), __ATOMIC_RELEASE);
    if (pScreenPtr) {
        QueueWorkProc(lorieRedraw, NULL, NULL);
        lorieWakeServer();
    }
}

static Bool lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {
    static int eventFd = -1;
    pScreenPtr = pScreen;

    if (eventFd == -1)
        eventFd = eventfd(0, EFD_CLOEXEC);

    pvfb->eventFd = eventFd;
    SetNotifyFd(eventFd, lorieWorkingQueueCallback, X_NOTIFY_READ, NULL);

    miSetZeroLineBias(pScreen, 0);
    pScreen->blackPixel = 0;
    pScreen->whitePixel = 1;

    pvfb->vblank_interval = 1000000 / pvfb->root.framerate;

    if (FALSE
          || !miSetVisualTypesAndMasks(24, ((1 << TrueColor) | (1 << DirectColor)), 8, TrueColor, 0xFF0000, 0x00FF00, 0x0000FF)
          || !miSetPixmapDepths()
          || !fbScreenInit(pScreen, NULL, pvfb->root.width, pvfb->root.height, monitorResolution, monitorResolution, 0, 32)
          || !(pScreen->CreateScreenResources = lorieCreateScreenResources) // Simply replace unneeded function
          || !(!pvfb->dri3 || dri3_screen_init(pScreen, &lorieDri3Info))
          || !fbPictureInit(pScreen, 0, 0)
          || !exaDriverInit(pScreen, &lorieExa)
          || !lorieRandRInit(pScreen)
          || !miPointerInitialize(pScreen, &loriePointerSpriteFuncs, &loriePointerCursorFuncs, TRUE)
          || !fbCreateDefColormap(pScreen)
          || !present_screen_init(pScreen, &loriePresentInfo))
        return FALSE;

    pvfb->CloseScreen = pScreen->CloseScreen;
    pvfb->SetWindowPixmap = pScreenPtr->SetWindowPixmap;
    pScreen->CloseScreen = lorieCloseScreen;
    pScreen->SetWindowPixmap = lorieSetWindowPixmap;

    ShmRegisterFbFuncs(pScreen);
    miSyncShmScreenInit(pScreen);

    return TRUE;
}                               /* end lorieScreenInit */



static void lorieSetXftDpiResource(int dpi) {
    if (dpi <= 0)
        dpi = 96;

    if (pScreenPtr == NULL || pScreenPtr->root == NULL)
        return;

    char resources[128];
    int len = snprintf(resources, sizeof(resources), "Xft.dpi:\t%d\n", dpi);

    if (len <= 0)
        return;

    if (len >= (int)sizeof(resources))
        len = (int)sizeof(resources) - 1;

    Atom resourceManager = MakeAtom("RESOURCE_MANAGER", strlen("RESOURCE_MANAGER"), TRUE);
    Atom stringAtom = MakeAtom("STRING", strlen("STRING"), TRUE);

    dixChangeWindowProperty(
        serverClient,
        pScreenPtr->root,
        resourceManager,
        stringAtom,
        8,
        PropModeReplace,
        len,
        resources,
        TRUE
    );
}

void lorieSetMonitorResolution(int dpi) {
    if (dpi <= 0)
        dpi = 96;

    /*
     * Do not change RandR/mm size here.
     *
     * Changing the X server physical DPI changes xdpyinfo DPI and can affect
     * cursor size, hotspot behavior, and input hit testing.  For desktop UI
     * scaling we only publish Xft.dpi as a toolkit/session hint.
     *
     * Desktop environments such as XFCE should consume this through a session
     * helper and apply their own icon/panel/xsettings scaling.
     */
    lorieSetXftDpiResource(dpi);
}

void lorieConfigureNotify(int width, int height, int framerate, size_t name_size, char* name) {
    ScreenPtr pScreen = pScreenPtr;
    RROutputPtr output = RRFirstOutput(pScreen);
    framerate = framerate ? framerate : 30;

    if (output && name) {
        // We should save this name in pvfb to make sure the name will be restored in the case if the server is being reset.
        memset(pvfb->root.name, 0, 1024);
        memset(output->name, 0, 1024);
        strncpy(pvfb->root.name, name, name_size < 1024 ? name_size : 1024);
        strncpy(output->name, name, name_size < 1024 ? name_size : 1024);
        output->name[1023] = '\0';
        output->nameLength = strlen(output->name);
    }

    if (output && width && height && (pScreen->width != width || pScreen->height != height || pvfb->root.framerate != framerate)) {
        CARD32 mmWidth, mmHeight;
        RRModePtr mode = lorieCvt(width, height, framerate);
        mmWidth = ((double) (mode->mode.width)) * 25.4 / monitorResolution;
        mmHeight = ((double) (mode->mode.height)) * 25.4 / monitorResolution;
        RROutputSetModes(output, &mode, 1, 0);
        RRCrtcNotify(RRFirstEnabledCrtc(pScreen), mode, 0, 0, RR_Rotate_0, NULL, 1, &output);
        RRScreenSizeSet(pScreen, mode->mode.width, mode->mode.height, mmWidth, mmHeight);

        log(VERBOSE, "New reported framerate is %d", framerate);
        pvfb->root.framerate = framerate;
        pvfb->vblank_interval = 1000000 / pvfb->root.framerate;
    }
}

void InitOutput(ScreenInfo * screen_info, int argc, char **argv) {
    int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int bpp[] =    { 1, 8, 8, 16, 16, 32, 32 };
    int i;

    if (monitorResolution == 0)
        monitorResolution = 96;

    for(i = 0; i < ARRAY_SIZE(depths); i++) {
        screen_info->formats[i].depth = depths[i];
        screen_info->formats[i].bitsPerPixel = bpp[i];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = ARRAY_SIZE(depths);

    rendererTestCapabilities(&pvfb->root.legacyDrawing);
    xorgGlxCreateVendor();
    lorieInitClipboard();

    if (-1 == AddScreen(lorieScreenInit, argc, argv)) {
        FatalError("Couldn't add screen\n");
    }
}

// This Present implementation mostly copies the one from `present/present_fake.c`
// The only difference is performing vblanks right before redrawing root window (in lorieRedraw) instead of using timers.
static RRCrtcPtr loriePresentGetCrtc(WindowPtr w) {
    return RRFirstEnabledCrtc(w->drawable.pScreen);
}

static int loriePresentGetUstMsc(__unused RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc) {
    // ust has to be the time of that msc, not the time of this call. Returning "now" made every
    // msc look like it had just happened, which is a lie clients pace themselves against.
    *ust = lorieVsyncUs ? lorieVsyncUs : GetTimeInMicros();
    *msc = pvfb->current_msc;
    return Success;
}

static Bool loriePresentQueueVblank(__unused RRCrtcPtr crtc, uint64_t event_id, uint64_t msc) {
#pragma clang diagnostic push
#pragma ide diagnostic ignored "MemoryLeak" // it is not leaked, it is destroyed in lorieRedraw
    struct vblank* vblank = calloc (1, sizeof (*vblank));
    if (!vblank)
        return BadAlloc;

    *vblank = (struct vblank) { .id = event_id, .msc = msc };
    xorg_list_add(&vblank->link, &pvfb->vblank_queue);

    return Success;
#pragma clang diagnostic pop
}

static void loriePresentAbortVblank(__unused RRCrtcPtr crtc, uint64_t id, __unused uint64_t msc) {
    struct vblank *vblank, *tmp;

    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, link) {
        if (vblank->id == id) {
            xorg_list_del(&vblank->link);
            free (vblank);
            break;
        }
    }
}

static void loriePerformVblanks(void) {
    struct vblank *vblank, *tmp;
    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, link) {
        if (vblank->msc <= pvfb->current_msc) {
            present_event_notify(vblank->id, lorieVsyncUs, pvfb->current_msc);
            xorg_list_del(&vblank->link);
            free (vblank);
        }
    }
}

// Whether the renderer currently has a surface to draw into (e.g. false while the activity is
// backgrounded). Unlike lorieConnectionAlive(), this can go false without the socket connection
// itself dropping - the renderer process/thread stays up, it just has nothing to render into.
bool lorieRendererAvailable(void) {
    return pvfb->state->surfaceAvailable;
}

// Tries to offload a Present "copy" operation (present_execute_copy) to the renderer's GPU
// context instead of doing a CPU CopyArea here. dst is whatever GetWindowPixmap(window) is - root
// for a plain window, or a Composite-redirected window's own backing pixmap. Returns FALSE
// (caller falls back to the regular CPU present_copy_region) whenever either buffer isn't
// GPU-sampleable, or the deferred copy queue is currently full.
// When each outstanding copy was handed to the renderer, so lorieGpuCopyAck can say how long it
// took. Serials are monotonic and only a handful are ever in flight, so a small ring keyed by the
// serial is enough; a stale slot just yields a latency we discard.
#define LORIE_COPY_TIMING_SLOTS 64
static uint64_t lorieCopyStartUs[LORIE_COPY_TIMING_SLOTS];

Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,
                              uint64_t *out_serial, void **out_dst_buffer) {
    LorieBuffer *srcBuffer, *dstBuffer;
    LoriePixmapPriv *priv;
    const LorieBuffer_Desc *desc, *dstDesc;
    LorieGpuCopyEntry *entry;
    BoxRec fullBox;
    BoxPtr box;
    int numRects, i;
    uint32_t writeIndex, readIndex;

    if (pvfb->gpuPresentDisabled || pvfb->root.legacyDrawing) {
        gpuCopyAttempts++;
        return FALSE;
    }

    if (!lorieConnectionAlive() || !lorieRendererAvailable()) {
        // No renderer to drain the queue, so fall back to CPU copy.
        gpuCopyAttempts++;
        return FALSE;
    }

    if (!(srcBuffer = lorieEnsureGpuSampleable(pixmap, LORIEBUFFER_AHARDWAREBUFFER)) ||
        !(dstBuffer = lorieEnsureGpuSampleable(dst, LORIEBUFFER_AHARDWAREBUFFER))) {
        gpuCopyAttempts++;
        return FALSE;
    }
    priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap);
    if (priv->locked) {
        int status;
        LorieBuffer_unlock(priv->buffer);
        status = LorieBuffer_lock(priv->buffer, &priv->locked);
        if (status)
            FatalError("Failed to lock the surface: %d\n", status);
    }
    desc = LorieBuffer_description(srcBuffer);
    dstDesc = LorieBuffer_description(dstBuffer);

    if (update) {
        numRects = RegionNumRects(update);
        box = RegionRects(update);
    } else {
        fullBox = (BoxRec) { 0, 0, (short) pixmap->drawable.width, (short) pixmap->drawable.height };
        numRects = 1;
        box = &fullBox;
    }

    if (numRects <= 0 || numRects > LORIE_GPU_COPY_MAX_RECTS) {
        gpuCopyAttempts++;
        return FALSE;
    }

    writeIndex = pvfb->state->gpuCopyQueue.writeIndex;
    readIndex = pvfb->state->gpuCopyQueue.readIndex;
    if (writeIndex - readIndex >= LORIE_GPU_COPY_QUEUE_CAPACITY) {
        gpuCopyAttempts++;
        return FALSE;
    }

    // Make sure the renderer has (or will have) this texture. Idempotent if already registered.
    lorieRegisterBuffer(srcBuffer);
    // Extra reference: keeps the LorieBuffer struct alive on this side until lorieGpuCopyAck()
    // releases it, independently from the X pixmap's own lifetime.
    LorieBuffer_acquire(srcBuffer);
    // Read of the client's own pixmap (e.g. another client re-drawing into a buffer it already
    // handed to Present) could otherwise race this copy's GPU read of it.
    LorieBuffer_gpuCopyPendingInc(srcBuffer);
    // Root already has its own lifecycle (recreated on resize, kept alive by pScreenPtr->devPrivate)
    // - an extra reference here would outlive a resize and let the renderer keep finding a stale,
    // already-destroyed root buffer. Redirected-window destinations have no such guarantee, so they
    // still need registering and an extra reference.
    Bool dstIsRoot = dst == pScreenPtr->devPrivate;
    if (!dstIsRoot) {
        lorieRegisterBuffer(dstBuffer);
        LorieBuffer_acquire(dstBuffer);
        // Tracked so CPU reads of this window's pixmap (e.g. a compositor reading it back to
        // paint) only pay for the GPU lock (see lorieNeedsGpuLock) while a GPU write into it can
        // actually be in flight, instead of on every AHardwareBuffer-backed pixmap access.
        LorieBuffer_gpuCopyPendingInc(dstBuffer);
    } else {
        // Tracked so CPU reads of the screen pixmap only pay for the GPU lock (see
        // lorieNeedsGpuLock) while a GPU write into it can actually be in flight.
        pvfb->rootGpuCopyPending++;
        LoriePixmapPriv *rootPriv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(dst);
        if (rootPriv && rootPriv->rootDouble) {
            // The copy lands in the buffer we are drawing into, so every other slot misses it too.
            if (update)
                lorieMarkRootStale(rootPriv, update);
            else {
                RegionRec r;
                RegionInit(&r, &fullBox, 1);
                lorieMarkRootStale(rootPriv, &r);
                RegionUninit(&r);
            }
        }
    }
    *out_dst_buffer = dstIsRoot ? NULL : dstBuffer;

    entry = &pvfb->state->gpuCopyQueue.entries[writeIndex % LORIE_GPU_COPY_QUEUE_CAPACITY];
    entry->serial = ++pvfb->gpuCopySerialCounter;
    entry->srcBufferId = desc->id;
    entry->dstBufferId = dstDesc->id;
    entry->xOff = x_off;
    entry->yOff = y_off;
    entry->numRects = (uint16_t) numRects;
    for (i = 0; i < numRects; i++)
        entry->rects[i] = (LorieGpuCopyRect) { box[i].x1, box[i].y1, box[i].x2, box[i].y2 };

    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above
    pthread_cond_signal(rendererCond);

    *out_serial = entry->serial;
    lorieCopyStartUs[entry->serial % LORIE_COPY_TIMING_SLOTS] = lorieNowUs();
    gpuCopyAttempts++;
    gpuCopyOffloads++;
    lorieNotePresentSubmitted();
    return TRUE;
}

Bool lorieGpuCopyIsDone(uint64_t serial) {
    if (__atomic_load_n(&pvfb->state->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE) >= serial)
        return TRUE;

    pvfb->state->presentStats.copyRequeues++;
    return FALSE;
}

void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer, uint64_t serial) {
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap);
    uint64_t startUs = lorieCopyStartUs[serial % LORIE_COPY_TIMING_SLOTS];

    lorieNotePresentCompleted();

    if (startUs) {
        uint32_t latencyUs = (uint32_t) (lorieNowUs() - startUs);
        lorieCopyStartUs[serial % LORIE_COPY_TIMING_SLOTS] = 0;
        pvfb->state->presentStats.copyLatencySumUs += latencyUs;
        if (latencyUs > pvfb->state->presentStats.copyLatencyMaxUs)
            pvfb->state->presentStats.copyLatencyMaxUs = latencyUs;
        pvfb->state->presentStats.copyCompletions++;
    }

    if (priv && priv->buffer)
        LorieBuffer_gpuCopyPendingDec(priv->buffer);
    if (dst_buffer)
        LorieBuffer_gpuCopyPendingDec((LorieBuffer *) dst_buffer);
    else
        pvfb->rootGpuCopyPending--;

    if (priv && priv->buffer)
        LorieBuffer_release(priv->buffer);
    if (dst_buffer)
        LorieBuffer_release((LorieBuffer *) dst_buffer);
}

Bool loriePresentFlip(__unused RRCrtcPtr crtc, __unused uint64_t event_id, __unused uint64_t target_msc, PixmapPtr pixmap, __unused Bool sync_flip) {
    LoriePixmapPriv* priv = (LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap);
    if (!priv || !priv->buffer || priv->mem || pvfb->root.width != pixmap->drawable.width || pvfb->root.height != pixmap->drawable.height)
        return FALSE;

    const LorieBuffer_Desc *desc = LorieBuffer_description(priv->buffer);
    char *forceFlip = getenv("TERMUX_X11_FORCE_FLIP");
    if (desc->type == LORIEBUFFER_FD && priv->imported && !(forceFlip && strcmp(forceFlip, "1") == 0))
        return FALSE; // For some reason it does not work fine with turnip.

    // Regular buffers can not be shared to activity, we must explicitly convert LorieBuffer to FD or AHardwareBuffer
    lorieEnsureGpuSampleable(pixmap, pvfb->root.legacyDrawing ? LORIEBUFFER_FD : LORIEBUFFER_AHARDWAREBUFFER);

    if (desc->type != LORIEBUFFER_FD && desc->type != LORIEBUFFER_AHARDWAREBUFFER)
        return FALSE;

    lorieRegisterBuffer(priv->buffer);
    lorieNotePresentSubmitted();
    return TRUE;
}

void loriePresentAfterFlip(__unused RRCrtcPtr crtc, uint64_t event_id, __unused uint64_t ust, uint64_t target_msc, __unused PixmapPtr pixmap) {
    // X server was patched to call this function right after finishing all present_flip shenanigans
    // Since we do not invoke DRM API or anything similar we do not need to implement this as callback
    // For some reason calling present_event_notify in BlockHandler or as QueueWorkProc/eventfd callback
    // adds some delay which may be easily avoided this way.
    static BoxRec box = { 0, 0, 1, 1 }; // lorieRedraw only checks if it is empty or not.
    lorieNotePresentCompleted();
    RegionReset(DamageRegion(pvfb->damage), &box);
    /*
     * A flip is not a vsync, so it does not advance current_msc - lorieRedraw does, once per
     * vsync, and nothing else may. Assigning here made the counter a count of two different
     * things, and every present that waits on it inherited the error: the renderer was perfectly
     * even at 8.5 ms a frame with nothing dropped and the X server was never stuck for more than
     * 16 ms, yet individual client presents took 76-142 ms to reach the screen.
     *
     * What to report is target_msc, not where the counter is now. A sync flip executes a vsync
     * early (present_scmd.c's exec_msc--), so the frame goes up at target_msc, which is what the
     * client asked for and what it will pace its next frame against. The ust we were handed is the
     * time of the call, not of that vsync, so derive it from the display's clock instead.
     */
    present_event_notify(event_id, lorieUstForMsc(target_msc), target_msc);
}

void loriePresentUnflip(ScreenPtr screen, uint64_t event_id) {
    // Present has just copied the flipped content back into the screen pixmap, which reaches only
    // the slot being drawn into. That copy is not something our damage tracking sees in full, and a
    // slot that sat idle for a while would otherwise be handed over still carrying pre-flip pixels.
    PixmapPtr screenPix = screen ? (*screen->GetScreenPixmap)(screen) : NULL;
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(screenPix);

    if (priv && priv->rootDouble) {
        RegionRec all;
        BoxRec box = { 0, 0, (short) screen->width, (short) screen->height };

        RegionInit(&all, &box, 1);
        lorieMarkRootStale(priv, &all);
        RegionUninit(&all);
    }

    present_event_notify(event_id, 0, 0);
}

static struct present_screen_info loriePresentInfo = {
        .get_crtc = loriePresentGetCrtc,
        .get_ust_msc = loriePresentGetUstMsc,
        .queue_vblank = loriePresentQueueVblank,
        .abort_vblank = loriePresentAbortVblank,
        // check_flip is called only in present_check_flip_window during window reconfiguration.
        // The function should tell if pixmap can be used for flipping window.
        // Since there are no other drivers involved here we assume it always fits.
        .check_flip = TrueNoop,
        .flip = loriePresentFlip,
        .after_flip = loriePresentAfterFlip,
        .unflip = loriePresentUnflip,
};

void exaDDXDriverInit(__unused ScreenPtr pScreen) {}

void *lorieCreatePixmap(__unused ScreenPtr pScreen, int width, int height, __unused int depth, int usage_hint, __unused int bpp, int *new_fb_pitch) {
    LoriePixmapPriv *priv;
    size_t size = sizeof(LoriePixmapPriv);
    *new_fb_pitch = 0;

    priv = calloc(1, size);
    if (!priv)
        return NULL;

    if (width == 0 || height == 0)
        return priv;

    uint8_t type = usage_hint != CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED ? LORIEBUFFER_REGULAR : pvfb->root.legacyDrawing ? LORIEBUFFER_FD : LORIEBUFFER_AHARDWAREBUFFER;
    priv->buffer = LorieBuffer_allocate(width, height, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, type);
    *new_fb_pitch = LorieBuffer_description(priv->buffer)->stride * 4;

    LorieBuffer_lock(priv->buffer, &priv->locked);
    if (!priv->buffer) {
        free(priv);
        return NULL;
    }

    return priv;
}

void lorieExaDestroyPixmap(__unused ScreenPtr pScreen, void *driverPriv) {
    {
        LoriePixmapPriv *rootPriv = driverPriv;
        if (rootPriv && rootPriv->rootDouble) {
            int i;

            // Every slot belongs to this pixmap. The code below frees whichever one `buffer`
            // currently aliases, so free the others here and skip that one.
            for (i = 0; i < LORIE_ROOT_SLOTS; i++) {
                LorieBuffer *slot = rootPriv->rootBuf[i];

                RegionUninit(&rootPriv->rootStale[i]);
                rootPriv->rootBuf[i] = NULL;
                rootPriv->rootLocked[i] = NULL;
                if (!slot || slot == rootPriv->buffer)
                    continue;

                LorieBuffer_unlock(slot);
                lorieUnregisterBuffer(slot);
                LorieBuffer_release(slot);
            }
            rootPriv->rootDouble = FALSE;
            pvfb->state->rootDoubleBuffered = 0;
        }
    }
    LoriePixmapPriv *priv = driverPriv;
    if (priv->buffer) {
        if (priv->locked)
            LorieBuffer_unlock(priv->buffer);
        lorieUnregisterBuffer(priv->buffer);
        LorieBuffer_release(priv->buffer);
    }
    free(priv);
}

Bool lorieModifyPixmapHeader(PixmapPtr pPix, __unused int w, __unused int h, __unused int depth, __unused int bitsPerbppPixel, __unused int devKind, __unused void *data) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (priv && data)
        priv->mem = data;
    return FALSE;
}

// Copies region from one root buffer to the other. Both are CPU mapped and have the same geometry,
// so this is a plain per-scanline memcpy; nothing here touches the GPU or waits for it.
static void lorieCopyRootRegion(LoriePixmapPriv *priv, int from, int to, RegionPtr region) {
    const LorieBuffer_Desc *d;
    const char *src = priv->rootLocked[from];
    char *dst = priv->rootLocked[to];
    int nrects = RegionNumRects(region), i;
    BoxPtr box = RegionRects(region);
    size_t stride;

    if (!src || !dst || nrects <= 0)
        return;

    d = LorieBuffer_description(priv->rootBuf[from]);
    stride = (size_t) d->stride * 4;

    for (i = 0; i < nrects; i++) {
        int x1 = max(0, box[i].x1), x2 = min((int) d->width, box[i].x2);
        int y1 = max(0, box[i].y1), y2 = min((int) d->height, box[i].y2), y;

        for (y = y1; x2 > x1 && y < y2; y++)
            memcpy(dst + (size_t) y * stride + (size_t) x1 * 4,
                   src + (size_t) y * stride + (size_t) x1 * 4, (size_t) (x2 - x1) * 4);
    }
}

// The slot the renderer takes next: the one we published most recently.
static inline int lorieRootSampledIndex(void) {
    return (int) ((__atomic_load_n(&pvfb->state->rootHandover, __ATOMIC_ACQUIRE)
                   >> LORIE_ROOT_NEWEST_SHIFT) & LORIE_ROOT_NEWEST_MASK);
}

// Everything we draw lands only in the slot we are drawing into, so every other slot is behind by
// that region until it becomes our drawing target and the handover copies it forward.
static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region) {
    int i;

    for (i = 0; i < LORIE_ROOT_SLOTS; i++)
        if (i != priv->rootWrite)
            RegionUnion(&priv->rootStale[i], &priv->rootStale[i], region);
}

// Allocates the second root buffer. Failing is not fatal, it just leaves the old single-buffered
// behaviour (where every X server drawing operation waits for the renderer's fence) in place.
static void lorieEnsureRootDoubleBuffer(PixmapPtr root) {
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(root);
    const LorieBuffer_Desc *desc;
    LorieBuffer *orig;
    void *origLocked;
    RegionRec all;
    BoxRec box;
    int i, allocated, w, h;

    if (lorieSingleRootBuffer || !priv || priv->rootDouble || !priv->buffer || priv->mem || pvfb->root.legacyDrawing)
        return;

    desc = LorieBuffer_description(priv->buffer);
    if (desc->type != LORIEBUFFER_AHARDWAREBUFFER)
        return;

    // Only ever the screen pixmap. lorieRedraw() passes whatever pixmap the root window currently
    // has, and while a client is flipping that is the client's own - Present owns it and hands it
    // back at unflip, so rotating buffers underneath it is wrong, and it needs no rotation anyway
    // because the X server is not the one drawing into it.
    if (!pScreenPtr || root != (*pScreenPtr->GetScreenPixmap)(pScreenPtr))
        return;

    orig = priv->buffer;
    origLocked = priv->locked;
    w = (int) desc->width;
    h = (int) desc->height;

    /*
     * Every slot is allocated fresh, and as BGRA rather than the RGBX the root started out with.
     *
     * The X server writes BGRA. Declaring the buffer RGBX and letting the GL path make up the
     * difference with a swizzling shader worked as long as the only consumer was that shader, but
     * the compositor reads a buffer by its declared format and has no equivalent - the colours come
     * out with red and blue exchanged. Declaring what is actually in there fixes both at once,
     * since the shader selection follows the format: GL stops swizzling at the same moment.
     *
     * LorieBuffer_convert() cannot do this, it only converts CPU-backed buffers, so the original is
     * copied into the new slots and dropped.
     */
    for (allocated = 0; allocated < LORIE_ROOT_SLOTS; allocated++) {
        priv->rootBuf[allocated] = LorieBuffer_allocate(w, h, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
                                                        LORIEBUFFER_AHARDWAREBUFFER);
        if (!priv->rootBuf[allocated]) {
            log(ERROR, "Failed to allocate root buffer %d, keeping the single buffered root", allocated);
            break;
        }
        if (LorieBuffer_lock(priv->rootBuf[allocated], &priv->rootLocked[allocated]) ||
            !priv->rootLocked[allocated]) {
            log(ERROR, "Failed to lock root buffer %d, keeping the single buffered root", allocated);
            LorieBuffer_release(priv->rootBuf[allocated]);
            priv->rootBuf[allocated] = NULL;
            priv->rootLocked[allocated] = NULL;
            break;
        }
    }

    if (allocated < LORIE_ROOT_SLOTS) {
        // All or nothing: a partial set would mean the slot count had to be dynamic everywhere else.
        for (i = 0; i < allocated; i++) {
            LorieBuffer_unlock(priv->rootBuf[i]);
            LorieBuffer_release(priv->rootBuf[i]);
            priv->rootBuf[i] = NULL;
            priv->rootLocked[i] = NULL;
        }
        return;
    }

    // Carry the pixels the root already had into slot 0, then make every other slot match it.
    {
        const LorieBuffer_Desc *d0 = LorieBuffer_description(priv->rootBuf[0]);
        const char *src = origLocked;
        char *dst = priv->rootLocked[0];
        size_t srcStride = (size_t) desc->stride * 4, dstStride = (size_t) d0->stride * 4;
        size_t row = (size_t) w * 4;
        int y;

        if (src && dst)
            for (y = 0; y < h; y++)
                memcpy(dst + (size_t) y * dstStride, src + (size_t) y * srcStride, row);
    }

    box = (BoxRec) { 0, 0, (short) w, (short) h };
    RegionInit(&all, &box, 1);
    for (i = 0; i < LORIE_ROOT_SLOTS; i++) {
        RegionInit(&priv->rootStale[i], NULL, 0);
        if (i)
            lorieCopyRootRegion(priv, 0, i, &all);
    }
    RegionUninit(&all);

    for (i = 0; i < LORIE_ROOT_SLOTS; i++) {
        lorieRegisterBuffer(priv->rootBuf[i]);
        pvfb->state->rootBufferIds[i] = LorieBuffer_description(priv->rootBuf[i])->id;
    }

    __atomic_store_n(&pvfb->state->rootHandover, 0u, __ATOMIC_RELEASE); // published slot 0, none held
    priv->rootWrite = 1;
    priv->buffer = priv->rootBuf[1];
    priv->locked = priv->rootLocked[1];
    priv->rootDouble = TRUE;
    pvfb->state->rootDoubleBuffered = 1;

    // The new slots may be laid out differently from the buffer the root arrived with.
    pScreenPtr->ModifyPixmapHeader(root, 0, 0, 0, 0,
                                   LorieBuffer_description(priv->rootBuf[1])->stride * 4, NULL);

    // The root no longer has anything to do with the buffer it was created with.
    if (origLocked)
        LorieBuffer_unlock(orig);
    lorieUnregisterBuffer(orig);
    LorieBuffer_release(orig);

    log(INFO, "Root window has %d BGRA buffers (%dx%d, first id %llu)", LORIE_ROOT_SLOTS, w, h,
        (unsigned long long) pvfb->state->rootBufferIds[0]);
}

// Hands the buffer we have just finished drawing to the renderer and takes the other one. Does
// nothing at all while the renderer is still sampling, in which case we simply keep drawing into the
// same buffer for another frame - that costs one frame of freshness and never a stall.
static Bool lorieRootHandover(LoriePixmapPriv *priv) {
    uint32_t old, new;
    int drawn = priv->rootWrite, next, i;

    if (!priv->rootDouble)
        return FALSE;

    do {
        old = __atomic_load_n(&pvfb->state->rootHandover, __ATOMIC_ACQUIRE);

        // Where we draw from here on: any slot other than the one we are publishing that the
        // renderer does not still need. With three slots there is normally one; when there is not,
        // the renderer is holding both others and we are trying to publish faster than the display
        // can show it, so keep drawing into this one for another frame.
        next = -1;
        for (i = 0; i < LORIE_ROOT_SLOTS; i++)
            if (i != drawn && !(old & (1u << i))) {
                next = i;
                break;
            }
        if (next < 0)
            return FALSE;

        new = (old & ~(LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT))
            | ((uint32_t) drawn << LORIE_ROOT_NEWEST_SHIFT);
        new += LORIE_ROOT_COUNT_STEP;
        // Retry rather than give up: a failed swap only means the renderer took or released a slot
        // in between, which may well have freed a different one for us.
    } while (!__atomic_compare_exchange_n(&pvfb->state->rootHandover, &old, new, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    lorieCopyRootRegion(priv, drawn, next, &priv->rootStale[next]);
    RegionEmpty(&priv->rootStale[next]);

    priv->rootWrite = next;
    priv->buffer = priv->rootBuf[next];
    priv->locked = priv->rootLocked[next];
    return TRUE;
}

// Whether a CPU access to pPix could race a GPU write from the renderer, and so needs state->lock.
static inline __always_inline Bool lorieNeedsGpuLock(PixmapPtr pPix, LoriePixmapPriv *priv, int index) {
    if (pScreenPtr->GetScreenPixmap(pScreenPtr) == pPix) {
        // With a double buffered root the renderer never samples the buffer we draw into, so our
        // drawing does not have to wait for its fence at all. Only a GPU copy landing in our own
        // buffer still needs the lock.
        if (pvfb->state->rootDoubleBuffered)
            return pvfb->rootGpuCopyPending && !pvfb->root.legacyDrawing;

        return index == EXA_PREPARE_DEST || (pvfb->rootGpuCopyPending && !pvfb->root.legacyDrawing);
    }
    return !pvfb->root.legacyDrawing && priv->buffer &&
           LorieBuffer_description(priv->buffer)->type == LORIEBUFFER_AHARDWAREBUFFER &&
           LorieBuffer_hasGpuCopyPending(priv->buffer);
}

Bool loriePrepareAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (lorieNeedsGpuLock(pPix, priv, index)) {
        // This is where the X server's own drawing waits for the renderer to let go of the root
        // window. Timed because it is the whole cost of the renderer's lock occupancy as the X
        // server experiences it - a client's throughput drops by exactly this.
        uint64_t waitStartUs = lorieNowUs();
        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
        uint32_t waitUs = (uint32_t) (lorieNowUs() - waitStartUs);
        pvfb->state->presentStats.xLockWaitUs += waitUs;
        if (waitUs > pvfb->state->presentStats.xLockWaitMaxUs)
            pvfb->state->presentStats.xLockWaitMaxUs = waitUs;
        pvfb->state->presentStats.xLockWaits++;
    }

    if (!priv->locked && !priv->mem) {
        int err = LorieBuffer_lock(priv->buffer, &priv->locked);
        if (err) {
            dprintf(2, "Failed to lock buffer, err %d\n", err);
            return FALSE;
        }
        priv->wasLocked = FALSE;
    } else
        priv->wasLocked = TRUE;

    pPix->devPrivate.ptr = priv->locked ?: priv->mem;
    return TRUE;
}

void lorieFinishAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (lorieNeedsGpuLock(pPix, priv, index))
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);

    if (!priv->wasLocked) {
        LorieBuffer_unlock(priv->buffer);
        priv->locked = NULL;
        priv->wasLocked = FALSE;
    }
}

static ExaDriverRec lorieExa = {
        .exa_major = EXA_VERSION_MAJOR, .exa_minor = EXA_VERSION_MINOR, .maxX = 32767, .maxY = 32767,
        .flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS, .pixmapPitchAlign = 32,
        .PrepareSolid = FalseNoop, .PrepareCopy = FalseNoop, .PrepareComposite = FalseNoop,
        .PixmapIsOffscreen = TrueNoop, .WaitMarker = VoidNoop,
        .PrepareAccess = loriePrepareAccess, .FinishAccess = lorieFinishAccess,
        .CreatePixmap2 = lorieCreatePixmap, .DestroyPixmap = lorieExaDestroyPixmap,
        .ModifyPixmapHeader = lorieModifyPixmapHeader,
};

static PixmapPtr loriePixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds, CARD16 width, CARD16 height,
                                    const CARD32 *strides, const CARD32 *offsets, CARD8 depth, __unused CARD8 bpp, CARD64 modifier) {
#define fail(msg, ...) do { log(ERROR, msg, ##__VA_ARGS__); goto fail; } while(0)
#define check(cond, msg, ...) if ((cond)) fail(msg, ##__VA_ARGS__)
    const CARD64 AHARDWAREBUFFER_SOCKET_FD = 1255;
    const CARD64 AHARDWAREBUFFER_FLIPPED_SOCKET_FD = 1256;
    const CARD64 RAW_MMAPPABLE_FD = 1274;
    AHardwareBuffer_Desc desc = {0};
    PixmapPtr pixmap = NullPixmap;
    LoriePixmapPriv *priv = NULL;

    check(num_fds > 1, "DRI3: More than 1 fd");
    check(modifier != RAW_MMAPPABLE_FD && modifier != AHARDWAREBUFFER_SOCKET_FD && modifier != AHARDWAREBUFFER_FLIPPED_SOCKET_FD &&
          modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR, "DRI3: Modifier is not RAW_MMAPPABLE_FD or AHARDWAREBUFFER_SOCKET_FD");

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    check(!pixmap, "DRI3: failed to create pixmap");

    priv = exaGetPixmapDriverPrivate(pixmap);
    check(!priv, "DRI3: failed to obtain pixmap private");

    priv->imported = true;

    if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR || modifier == RAW_MMAPPABLE_FD) {
        check(!(priv->buffer = LorieBuffer_wrapFileDescriptor(width, strides[0]/4, height, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, fds[0], offsets[0])), "DRI3: LorieBuffer_wrapAHardwareBuffer failed.");
        screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], NULL);
        if (lorieServerDebugEnabled)
            log(INFO, "DRI3: imported raw fd, modifier %llu, %ux%u stride %u", (unsigned long long) modifier, width, height, strides[0]);
        return pixmap;
    }

    if (modifier == AHARDWAREBUFFER_SOCKET_FD || modifier == AHARDWAREBUFFER_FLIPPED_SOCKET_FD) {
        AHardwareBuffer* buffer;
        struct stat info;
        uint8_t buf = 1;
        int r;

        priv->flipped = modifier == AHARDWAREBUFFER_FLIPPED_SOCKET_FD;
        check(fstat(fds[0], &info) != 0, "DRI3: fstat failed: %s", strerror(errno));
        check(!S_ISSOCK(info.st_mode), "DRI3: modifier is AHARDWAREBUFFER_SOCKET_FD but fd is not a socket");
        // Sending signal to other end of socket to send buffer.
        check(write(fds[0], &buf, 1) != 1, "DRI3: AHARDWAREBUFFER_SOCKET_FD: failed to write to socket: %s", strerror(errno));
        check((r = AHardwareBuffer_recvHandleFromUnixSocket(fds[0], &buffer)) != 0,
              "DRI3: AHARDWAREBUFFER_SOCKET_FD: failed to obtain AHardwareBuffer from socket: %d", r);
        check(!buffer, "DRI3: AHARDWAREBUFFER_SOCKET_FD: did not receive AHardwareSocket from buffer");
        AHardwareBuffer_describe(buffer, &desc);
        check(desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
            && desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
            && desc.format != AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
            "DRI3: AHARDWAREBUFFER_SOCKET_FD: wrong format of AHardwareBuffer. Must be one of: AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM (stands for 5).");
        check(!(priv->buffer = LorieBuffer_wrapAHardwareBuffer(buffer)), "DRI3: LorieBuffer_wrapAHardwareBuffer failed.");

        screen->ModifyPixmapHeader(pixmap, desc.width, desc.height, 0, 0, desc.stride * 4, NULL);
        if (lorieServerDebugEnabled)
            log(INFO, "DRI3: imported AHardwareBuffer, modifier %llu, %ux%u stride %u", (unsigned long long) modifier, desc.width, desc.height, desc.stride);
    }

    return pixmap;

    fail:
    if (pixmap)
        screen->DestroyPixmap(pixmap);

    return NULL;
}

static int lorieGetFormats(__unused ScreenPtr screen, CARD32 *num_formats, CARD32 **formats) {
    static CARD32 format = DRM_FORMAT_ARGB8888;
    *num_formats = 1;
    *formats = &format;
    return TRUE;
}

static int lorieGetModifiers(__unused ScreenPtr screen, uint32_t format, uint32_t *num_modifiers, uint64_t **modifiers) {
    static uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

    if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888) {
        *num_modifiers = 0;
        *modifiers = NULL;
        return TRUE;
    }

    *num_modifiers = 1;
    *modifiers = &modifier;
    return TRUE;
}

static dri3_screen_info_rec lorieDri3Info = {
        .version = 2,
        .fds_from_pixmap = FalseNoop,
        .pixmap_from_fds = loriePixmapFromFds,
        .get_formats = lorieGetFormats,
        .get_modifiers = lorieGetModifiers,
        .get_drawable_modifiers = FalseNoop
};

static GLboolean drawableSwapBuffers(unused ClientPtr client, unused __GLXdrawable * drawable) { return TRUE; }
static void drawableCopySubBuffer(unused __GLXdrawable * basePrivate, unused int x, unused int y, unused int w, unused int h) {}
static __GLXdrawable * createDrawable(unused ClientPtr client, __GLXscreen * screen, DrawablePtr pDraw,
                                      unused XID drawId, int type, XID glxDrawId, __GLXconfig * glxConfig) {
    __GLXdrawable *private = calloc(1, sizeof *private);
    if (private == NULL)
        return NULL;

    if (!__glXDrawableInit(private, screen, pDraw, type, glxDrawId, glxConfig)) {
        free(private);
        return NULL;
    }

    private->destroy = (void (*)(__GLXdrawable *)) free;
    private->swapBuffers = drawableSwapBuffers;
    private->copySubBuffer = drawableCopySubBuffer;

    return private;
}

static void glXDRIscreenDestroy(__GLXscreen *baseScreen) {
    free(baseScreen->GLXextensions);
    free(baseScreen->GLextensions);
    free(baseScreen->visuals);
    free(baseScreen);
}

static __GLXscreen *glXDRIscreenProbe(ScreenPtr pScreen) {
    __GLXscreen *screen;

    screen = calloc(1, sizeof *screen);
    if (screen == NULL)
        return NULL;

    screen->destroy = glXDRIscreenDestroy;
    screen->createDrawable = createDrawable;
    screen->pScreen = pScreen;
    screen->fbconfigs = configs;
    screen->glvnd = "mesa";

    __glXInitExtensionEnableBits(screen->glx_enable_bits);
    /* There is no real GLX support, but anyways swrast reports it. */
    __glXEnableExtension(screen->glx_enable_bits, "GLX_MESA_copy_sub_buffer");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_no_config_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_no_error");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es2_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_framebuffer_sRGB");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_fbconfig_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_fbconfig_packed_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_texture_from_pixmap");
    __glXScreenInit(screen, pScreen);

    return screen;
}

__GLXprovider __glXDRISWRastProvider = {
        glXDRIscreenProbe,
        "DRISWRAST",
        NULL
};
