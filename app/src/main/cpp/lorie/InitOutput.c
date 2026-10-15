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

#include <sys/eventfd.h>
#include <sys/errno.h>
#include <pixman.h>
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

    /* AHB CPU ownership (struct lorie_backend_control): the mode in effect, and every AHB pixmap 3c98ffa
     * keeps CPU-locked for its lifetime - locked while PERSISTENT is in effect, not under PER_ACCESS */
    uint8_t ownership;
    struct xorg_list ahbPersistent;

    /* Root double buffering (TERMUX_X11_RENDERER_ROOT_DOUBLE=1, lorieRootSwap): asked for; the twin out of date
     * (filled whole at the next swap) */
    Bool rootDouble, rootTwinStale;
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
        .ahbPersistent = { &lorieScreen.ahbPersistent, &lorieScreen.ahbPersistent },
}, *pvfb = &lorieScreen;
static char *xstartup = NULL;

// Owned by the activity process, handed to us over the connection socket. Points at a placeholder until
// the first connection so callers don't need a NULL check.
static pthread_cond_t rendererCondPlaceholder = PTHREAD_COND_INITIALIZER;
static pthread_cond_t* volatile rendererCond = &rendererCondPlaceholder;

typedef struct {
    LorieBuffer *buffer;
    /* The screen pixmap's second buffer under root double buffering (lorieRootSwap): the one the renderer samples,
     * while the X server draws into `buffer`. NULL otherwise. */
    LorieBuffer *twin;
    bool flipped, wasLocked, imported;
    void *locked;
    void *mem;
    /* An AHB pixmap 3c98ffa keeps CPU-locked for its lifetime: on pvfb->ahbPersistent (lorieHoldPersistent) */
    bool persistentWanted;
    struct xorg_list persistentLink;
} LoriePixmapPriv;

#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap)) : NULL)
#define LORIE_BUFFER_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap))->buffer : NULL)

/*
 * AHB CPU ownership (struct lorie_backend_control in lorie.h). PERSISTENT is 3c98ffa's own behaviour, and
 * every place 3c98ffa took a lifetime CPU lock on a pixmap now goes through lorieHoldPersistent: the lock
 * is taken exactly as before while PERSISTENT is in effect, and the pixmap is remembered either way, so that
 * a change of mode (lorieApplyOwnership) can let go of every such lock, or take every one of them again.
 * Pixmaps that are not AHBs (plain memory, FD) are locked as before in both modes.
 */
static Bool lorieAhbPerAccess(void) {
    return pvfb->ownership == LORIE_OWNERSHIP_PER_ACCESS;
}

static Bool lorieIsAhb(LoriePixmapPriv *priv) {
    return priv && priv->buffer && LorieBuffer_description(priv->buffer)->type == LORIEBUFFER_AHARDWAREBUFFER;
}

static void lorieHoldPersistent(LoriePixmapPriv *priv) {
    if (!priv || !priv->buffer)
        return;
    if (lorieIsAhb(priv) && !priv->persistentWanted) {
        priv->persistentWanted = TRUE;
        xorg_list_add(&priv->persistentLink, &pvfb->ahbPersistent);
    }
    if (!priv->locked && !(lorieIsAhb(priv) && lorieAhbPerAccess()))
        LorieBuffer_lock(priv->buffer, &priv->locked);
}

static void lorieDropPersistent(LoriePixmapPriv *priv) {
    if (!priv)
        return;
    if (priv->persistentWanted) {
        xorg_list_del(&priv->persistentLink);
        priv->persistentWanted = FALSE;
    }
    if (priv->buffer && priv->locked) {
        LorieBuffer_unlock(priv->buffer);
        priv->locked = NULL;
        priv->wasLocked = FALSE;
    }
}

/*
 * Puts `mode` in effect for every remembered AHB pixmap - under PER_ACCESS none stays CPU-locked, under
 * PERSISTENT each is locked again as 3c98ffa keeps it - and answers request `seq` with it. Main thread,
 * between requests, so no EXA access is open on any of them. The locks and the answer change together under
 * the shared lock, so a renderer frame (which holds the same lock) always sees an answer that describes the
 * locks: it may use the AHBs from Vulkan only in a frame whose answer is its own request with PER_ACCESS.
 * An unlock is synchronous (no fence asked for), so the CPU's writes have landed by the time the renderer
 * can take the lock and use the buffer.
 */
static void lorieApplyOwnership(uint8_t mode, uint32_t seq) {
    LoriePixmapPriv *priv;

    lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
    if (mode != pvfb->ownership) {
        // Every AHB locked: not before a Vulkan frame that may still run (a new or lost connection) is over.
        if (mode == LORIE_OWNERSHIP_PERSISTENT)
            lorieGpuWaitIdle(pvfb->state, true, 0);
        xorg_list_for_each_entry(priv, &pvfb->ahbPersistent, persistentLink) {
            if (mode == LORIE_OWNERSHIP_PER_ACCESS && priv->locked) {
                LorieBuffer_unlock(priv->buffer);
                priv->locked = NULL;
                priv->wasLocked = FALSE;
            } else if (mode == LORIE_OWNERSHIP_PERSISTENT && !priv->locked)
                LorieBuffer_lock(priv->buffer, &priv->locked);
        }
        pvfb->ownership = mode;
        pvfb->state->diag.xOwnership = mode;
        __atomic_fetch_add(&pvfb->state->diag.xOwnershipTransitions, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&pvfb->state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);
    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
}

/*
 * Takes up the renderer's ownership request, if there is a new one, and answers it with the mode now in
 * effect. PER_ACCESS is refused in a session asked for gles: that one stays 3c98ffa's throughout.
 */
static void lorieProcessOwnershipRequest(void) {
    uint32_t req = __atomic_load_n(&pvfb->state->backend.ownershipRequest, __ATOMIC_ACQUIRE);
    uint32_t ack = __atomic_load_n(&pvfb->state->backend.ownershipAck, __ATOMIC_ACQUIRE);
    uint8_t mode = LORIE_OWNERSHIP_MODE(req);

    if (LORIE_OWNERSHIP_SEQ(req) == LORIE_OWNERSHIP_SEQ(ack))
        return;
    if (mode != LORIE_OWNERSHIP_PERSISTENT &&
        (mode != LORIE_OWNERSHIP_PER_ACCESS || pvfb->state->backend.requested == LORIE_BACKEND_GLES)) {
        __atomic_fetch_add(&pvfb->state->diag.xOwnershipRefused, 1, __ATOMIC_RELAXED);
        mode = LORIE_OWNERSHIP_PERSISTENT;
    }
    lorieApplyOwnership(mode, LORIE_OWNERSHIP_SEQ(req));
    pthread_cond_signal(rendererCond);
}

/*
 * Back to 3c98ffa's ownership when no renderer is there to use AHBs from Vulkan: its connection went away, or
 * a new one was made (which starts on GLES). Whatever was asked before is answered as PERSISTENT, so the
 * next renderer's first request is one past it.
 */
static void lorieResetOwnership(void) {
    uint32_t req = __atomic_load_n(&pvfb->state->backend.ownershipRequest, __ATOMIC_ACQUIRE);

    lorieApplyOwnership(LORIE_OWNERSHIP_PERSISTENT, LORIE_OWNERSHIP_SEQ(req));
}

static Bool lorieRendererGoneWorkProc(__unused ClientPtr client, __unused void *closure) {
    lorieResetOwnership();
    return TRUE;
}

// Input thread (handleLorieEvents), when the renderer's connection closes: taken up on the main thread.
void lorieRendererDisconnected(void) {
    QueueWorkProc(lorieRendererGoneWorkProc, NULL, NULL);
    lorieWakeServer();
}

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
            // The conversion dropped the old mapping, which priv->locked still points into; 3c98ffa
            // overwrote it with a new lock, which lorieHoldPersistent takes while PERSISTENT is in effect.
            priv->locked = NULL;
            lorieHoldPersistent(priv);
        }
    }

    return desc->type == type ? priv->buffer : NULL;
}

static Bool lorieServerDebugEnabled = FALSE;

// TERMUX_X11_RENDERER_BACKEND: auto, vulkan or gles; unset or empty is auto, and so is anything else, flagged.
static uint8_t lorieRequestedBackend(const char *value, volatile uint8_t *invalid) {
    if (value && !strcmp(value, "vulkan"))
        return LORIE_BACKEND_VULKAN;
    if (value && !strcmp(value, "gles"))
        return LORIE_BACKEND_GLES;
    if (value && *value && strcmp(value, "auto"))
        *invalid = 1;
    return LORIE_BACKEND_AUTO;
}

// TERMUX_X11_RENDERER_VK_FAIL_AT, lowercase, LORIE_VK_FAIL_POINTS without NONE: unset or empty is none, and so
// is anything else, flagged.
// TERMUX_X11_RENDERER_VK_SELFTEST: full runs the full ownership self-test (vk.h) when the Vulkan backend starts;
// unset or empty is the quick one, and so is anything else, flagged.
static uint8_t lorieRequestedVkSelftest(const char *value, volatile uint8_t *invalid) {
    if (value && !strcmp(value, "full"))
        return 1;
    if (value && *value)
        *invalid = 1;
    return 0;
}

static const char *const lorieVkFailAtNames[] = {
        "none", "instance", "device", "surface", "swapchain", "import", "submit", "present" };
_Static_assert(sizeof(lorieVkFailAtNames) / sizeof(*lorieVkFailAtNames) == LORIE_VK_FAIL_AT_COUNT,
               "lorieVkFailAtNames is LORIE_VK_FAIL_POINTS");

static uint8_t lorieRequestedVkFailAt(const char *value, volatile uint8_t *invalid) {
    uint8_t i;
    for (i = LORIE_VK_FAIL_AT_NONE + 1; value && i < LORIE_VK_FAIL_AT_COUNT; i++)
        if (!strcmp(value, lorieVkFailAtNames[i]))
            return i;
    if (value && *value)
        *invalid = 1;
    return LORIE_VK_FAIL_AT_NONE;
}

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

    // The renderer backend wanted (struct lorie_backend_control). Only a request: what runs is the
    // renderer's to report (diag.active), and AHB ownership follows that, never this.
    lorieScreen.state->backend.requested = lorieRequestedBackend(getenv("TERMUX_X11_RENDERER_BACKEND"),
                                                                 &lorieScreen.state->backend.requestedInvalid);
    lorieScreen.state->backend.vkFailAt = lorieRequestedVkFailAt(getenv("TERMUX_X11_RENDERER_VK_FAIL_AT"),
                                                                 &lorieScreen.state->backend.vkFailAtInvalid);
    lorieScreen.state->backend.vkSelftestFull = lorieRequestedVkSelftest(getenv("TERMUX_X11_RENDERER_VK_SELFTEST"),
                                                                         &lorieScreen.state->backend.vkSelftestInvalid);
    // Experimental: TERMUX_X11_RENDERER_VK_TRANSFER_COPY=1, Present copies by vkCmdCopyImage where equivalent (renderer.c).
    lorieScreen.state->backend.vkTransferCopy = getenv("TERMUX_X11_RENDERER_VK_TRANSFER_COPY") &&
                                                !strcmp(getenv("TERMUX_X11_RENDERER_VK_TRANSFER_COPY"), "1");
    // Experimental: TERMUX_X11_RENDERER_ROOT_DOUBLE=1 gives the screen pixmap a second buffer (lorieRootSwap).
    lorieScreen.rootDouble = getenv("TERMUX_X11_RENDERER_ROOT_DOUBLE") && !strcmp(getenv("TERMUX_X11_RENDERER_ROOT_DOUBLE"), "1");
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

// The screen pixmap's buffers, to the renderer: the one the X server draws into and, under root double buffering,
// its twin.
static void lorieRegisterRoot(void) {
    LoriePixmapPriv *priv = pScreenPtr && pScreenPtr->devPrivate ? exaGetPixmapDriverPrivate(pScreenPtr->devPrivate) : NULL;
    if (!priv)
        return;
    lorieRegisterBuffer(priv->buffer);
    if (priv->twin)
        lorieRegisterBuffer(priv->twin);
}

void lorieActivityConnected(void) {
    lorieResetOwnership();      // a new renderer starts on GLES, with 3c98ffa's ownership
    pvfb->state->drawRequested = pvfb->state->cursor.updated = true;
    lorieSendSharedServerState(pvfb->stateFd);
    lorieRegisterRoot();
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
    // PER_ACCESS: drawn into where 3c98ffa would have it CPU-locked, under the shared lock for the drawing only.
    Bool perAccess = !pixels && lorieAhbPerAccess() && priv && priv->persistentWanted;
    if (perAccess) {
        uint64_t waitStartUs = GetTimeInMicros(), gpuStartUs, lockStartUs;
        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
        gpuStartUs = GetTimeInMicros();
        lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
        lockStartUs = GetTimeInMicros();
        __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, lockStartUs - gpuStartUs, __ATOMIC_RELAXED);
        __atomic_fetch_add(&pvfb->state->diag.xStateLockWaitUs, lockStartUs - waitStartUs, __ATOMIC_RELAXED);
        if (LorieBuffer_lock(priv->buffer, (void **) &pixels))
            pixels = NULL;
        else
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuAcquires, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - lockStartUs, __ATOMIC_RELAXED);
    }
    if (pixels) {
        const LorieBuffer_Desc *d = LorieBuffer_description(priv->buffer);
        int l = min(d->width, d->height) / 4, x = (d->width - l)/2, y = (d->height - l)/2;

        drawSquare(x - l/3, y - l/3, l, 0x00FF0000, d->stride, pixels);
        drawSquare(x, y, l, 0x0000FF00, d->stride, pixels);
        drawSquare(x + l/3, y + l/3, l, 0x000000FF, d->stride, pixels);
        pvfb->rootTwinStale = TRUE;     // drawn past Damage: the twin is brought up to date whole
    }
    if (perAccess) {
        if (pixels) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
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

static void lorieSetCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, CursorPtr pCurs, int x0, int y0) {
    CursorBitsPtr bits = pCurs ? pCurs->bits : NULL;
    if (pCurs && (pCurs->bits->width >= 512 || pCurs->bits->height >= 512))
        // We do not have enough memory allocated for such a big cursor, let's display default "X" cursor
        pCurs = rootCursor;

    lorie_mutex_lock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid);
    if (pCurs && bits) {
        pvfb->state->cursor.xhot = bits->xhot;
        pvfb->state->cursor.yhot = bits->yhot;
        pvfb->state->cursor.width = bits->width;
        pvfb->state->cursor.height = bits->height;
        lorieConvertCursor(pCurs, pvfb->state->cursor.bits);
    } else {
        pvfb->state->cursor.xhot = pvfb->state->cursor.yhot = 0;
        pvfb->state->cursor.width = pvfb->state->cursor.height = 0;
    }
    pvfb->state->cursor.updated = true;
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

/*
 * Root double buffering (TERMUX_X11_RENDERER_ROOT_DOUBLE=1). The screen pixmap has two buffers: `buffer`, which the X
 * server draws into (back), and `twin`, which the renderer samples (front). The X server never draws into the
 * front, the renderer never samples the back, so while the device runs a frame sampling the front the X server's
 * accesses to the root (in the back) wait for nothing. At a tick (lorieRedraw) the back's damage since the last
 * swap is copied into the front - once the frame sampling it is over - and the two swap: the renderer then has
 * what the X server finished, the X server goes on in the other buffer, which now holds the same.
 *
 * In effect only while Vulkan draws (PER_ACCESS: under PERSISTENT GLES samples `buffer`, as 3c98ffa) and the
 * screen pixmap is the root window's (not while a client's pixmap is flipped in); Present copies into the root are
 * then the X server's own (lorieTryScheduleGpuCopy refuses them), so nothing but the X server writes the back.
 */
static Bool lorieRootTwinned(void) {
    PixmapPtr screen = pScreenPtr ? pScreenPtr->devPrivate : NULL;
    LoriePixmapPriv *priv = screen ? exaGetPixmapDriverPrivate(screen) : NULL;
    return priv && priv->twin && lorieAhbPerAccess() && pScreenPtr->root && pScreenPtr->GetWindowPixmap(pScreenPtr->root) == screen;
}

/*
 * The swap, between requests, under the shared lock (the renderer reads which buffer is the front under it). The
 * front is written only once no frame uses it (lorieGpuWaitIdle); what is copied is the damage since the last swap
 * (Damage on the screen pixmap sees every drawing into it), or all of it when the twin is out of date. False, and
 * nothing swapped, if a buffer cannot be CPU-locked.
 */
static Bool lorieRootSwap(LoriePixmapPriv *priv) {
    LorieBuffer *back = priv->buffer, *front = priv->twin;
    const LorieBuffer_Desc *d = LorieBuffer_description(back);
    RegionPtr damage = DamageRegion(pvfb->damage);
    BoxRec full = { 0, 0, (short) d->width, (short) d->height };
    BoxPtr box = pvfb->rootTwinStale ? &full : RegionRects(damage);
    int n = pvfb->rootTwinStale ? 1 : RegionNumRects(damage), i, failed;
    uint32_t *src = NULL, *dst = NULL;
    uint64_t waitStartUs = GetTimeInMicros(), gpuStartUs, lockStartUs, copyStartUs, copyEndUs;

    if (priv->locked)
        return FALSE;   // never between requests under PER_ACCESS
    lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
    gpuStartUs = GetTimeInMicros();
    lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(front)->id);     // the last frame sampling it
    lorieGpuWaitIdle(pvfb->state, false, d->id);
    lockStartUs = GetTimeInMicros();
    failed = LorieBuffer_lock(back, (void **) &src);
    failed |= LorieBuffer_lock(front, (void **) &dst);
    failed |= !src || !dst;
    copyStartUs = GetTimeInMicros();
    for (i = 0; !failed && i < n; i++) {
        int x1 = max(box[i].x1, 0), y1 = max(box[i].y1, 0), x2 = min(box[i].x2, d->width), y2 = min(box[i].y2, d->height);
        if (x2 > x1 && y2 > y1)
            pixman_blt(src, dst, d->stride, d->stride, 32, 32, x1, y1, x1, y1, x2 - x1, y2 - y1);
    }
    copyEndUs = GetTimeInMicros();
    LorieBuffer_unlock(back);           // LorieBuffer_lock marks a buffer locked even when it failed
    LorieBuffer_unlock(front);
    __atomic_fetch_add(&pvfb->state->diag.xStateLockWaitUs, lockStartUs - waitStartUs, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, lockStartUs - gpuStartUs, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pvfb->state->diag.xAhbCpuAcquires, 2, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, (copyStartUs - lockStartUs) + (GetTimeInMicros() - copyEndUs), __ATOMIC_RELAXED);
    __atomic_fetch_add(&pvfb->state->diag.xRootCopyUs, copyEndUs - copyStartUs, __ATOMIC_RELAXED);
    if (!failed) {
        priv->buffer = front;
        priv->twin = back;
        pvfb->rootTwinStale = FALSE;
        pvfb->state->rootWindowTextureID = LorieBuffer_description(back)->id;
    }
    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    return !failed;
}

static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {
    int status, nonEmpty;
    LoriePixmapPriv* priv;
    PixmapPtr root = pScreenPtr && pScreenPtr->root ? pScreenPtr->GetWindowPixmap(pScreenPtr->root) : NULL;
    Bool twinned, rootDouble;

    lorieProcessOwnershipRequest();
    twinned = lorieRootTwinned();

    pvfb->current_msc++;
    // Vulkan (PER_ACCESS): the frame this tick allows is allowed before the vblanks run, so that the Present copies
    // they queue go into that frame's submission instead of one of their own ahead of it. GLES keeps 3c98ffa's order,
    // and so does root double buffering: its swap, after the vblanks, comes before the frame.
    if (lorieAhbPerAccess() && !twinned)
        pvfb->state->waitForNextFrame = false;
    loriePerformVblanks();

    pvfb->state->waitForNextFrame = false;

    if (!lorieConnectionAlive() || !pvfb->state->surfaceAvailable)
        return TRUE;

    nonEmpty = RegionNotEmpty(DamageRegion(pvfb->damage));
    priv = root ? exaGetPixmapDriverPrivate(root) : NULL;

    if (!priv)
        // Impossible situation, but let's skip this step
        return TRUE;

    // Root double buffering, once no GPU copy into the root (from before it was in effect) is still to be
    // acknowledged: those went into the buffer the X server draws into. Not in effect, the twin falls out of date.
    rootDouble = twinned && lorieRootTwinned() && pvfb->rootGpuCopyPending == 0;
    if (rootDouble && (nonEmpty || pvfb->rootTwinStale)) {
        if (lorieRootSwap(priv))
            pvfb->state->drawRequested = TRUE;
        else
            rootDouble = FALSE;     // the back drawn this tick, as with one buffer; the twin filled whole next time
    }
    if (!rootDouble)
        pvfb->rootTwinStale = TRUE;
    pvfb->state->diag.xRootBuffers = rootDouble ? 2 : 1;

    if (nonEmpty && priv->buffer) {
        // We should unlock and lock buffer in order to update texture content on some devices
        // In most cases AHardwareBuffer uses DMA memory which is shared between CPU and GPU
        // and this is not needed. But according to docs we should do it for any case.
        // Also according to AHardwareBuffer docs simultaneous reading in rendering thread and
        // locking for writing in other thread is fine.
        if (priv->locked) {
            LorieBuffer_unlock(priv->buffer);
            status = LorieBuffer_lock(priv->buffer, &priv->locked);
            if (status)
                FatalError("Failed to lock the surface: %d\n", status);
        }

        DamageEmpty(pvfb->damage);
        pvfb->state->drawRequested = TRUE;
    }

    if (pvfb->state->drawRequested || pvfb->state->cursor.moved || pvfb->state->cursor.updated) {
        pvfb->state->rootWindowTextureID = LorieBuffer_description(rootDouble ? priv->twin : priv->buffer)->id;

        // Sending signal about pending root window changes to renderer thread.
        // We do not explicitly lock the pvfb->state->lock here because we do not want to wait
        // for all drawing operations to be finished.
        // Renderer thread will check the `drawRequested` flag right before going to sleep.
        pthread_cond_signal(rendererCond);
    }

    return TRUE;
}

static uint64_t gpuCopyAttempts = 0, gpuCopyOffloads = 0;

/*
 * The renderer's diagnostics (struct lorie_renderer_diag), every 5 seconds: each counter as its total and,
 * in brackets, what it moved by since the last report - key=total(+delta). Only read here; the renderer
 * moves them, after the calls they count have succeeded. tools/diag/renderer-proof.sh parses these lines,
 * so their keys are part of its contract.
 */
static const char *lorieActiveName(uint8_t a) {
    return a == LORIE_ACTIVE_GLES ? "GLES" : a == LORIE_ACTIVE_VULKAN ? "VULKAN" : "NONE";
}

// P9-B0: one latency's samples since the last report - count, mean, p95 (the bound of the bucket the 95th
// percentile falls in, ">66000" past the last) and the maximum, which is taken back to 0 here; with `entryUs` (the
// entries' copy_total_us added up over the same time), the share of it this part makes up in entry time (weightedUs).
static void lorieLatencyText(char *out, size_t size, const struct lorie_latency *now, const struct lorie_latency *before,
                             volatile uint32_t *max, uint64_t entryUs) {
    static const uint32_t bounds[LORIE_LATENCY_BUCKETS - 1] = { LORIE_LATENCY_BOUNDS };
    uint32_t n = now->count - before->count, want = (uint32_t) (((uint64_t) n * 95 + 99) / 100), seen = 0, b;
    uint32_t top = __atomic_exchange_n(max, 0, __ATOMIC_RELAXED);
    unsigned long long mean = n ? (unsigned long long) ((now->sumUs - before->sumUs) / n) : 0;
    unsigned long long permille = entryUs ? (unsigned long long) ((now->weightedUs - before->weightedUs) * 1000 / entryUs) : 0;
    int used;

    for (b = 0; b < LORIE_LATENCY_BUCKETS - 1; b++)
        if ((seen += now->buckets[b] - before->buckets[b]) >= want)
            break;
    if (!n)
        used = snprintf(out, size, "n:0");
    else if (b < LORIE_LATENCY_BUCKETS - 1)
        used = snprintf(out, size, "n:%u,avg:%llu,p95:%u,max:%u", n, mean, bounds[b], top);
    else
        used = snprintf(out, size, "n:%u,avg:%llu,p95:>%u,max:%u", n, mean, bounds[LORIE_LATENCY_BUCKETS - 2], top);
    if (n && entryUs && used > 0 && (size_t) used < size)
        snprintf(out + used, size - (size_t) used, ",share:%llu.%llu%%", permille / 10, permille % 10);
}

static const char *lorieAhbFormatName(uint32_t f) {
    return f == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ? "RGBA" : f == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ? "RGBX"
         : f == AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM ? "BGRA" : "other";
}

#define LORIE_NAME(n) #n,
static const char *const lorieFallbackClassNames[] = { LORIE_FALLBACK_CLASSES(LORIE_NAME) };
static const char *const lorieFallbackReasonNames[] = { LORIE_FALLBACK_REASONS(LORIE_NAME) };
static const char *const lorieSelftestStepNames[] = { LORIE_VK_SELFTEST_STEPS(LORIE_NAME) };
static const char *const lorieXferNotNames[] = { LORIE_VK_XFER_NOTS(LORIE_NAME) };
static const char *const lorieXferCheckNames[] = { LORIE_VK_XFER_CHECKS(LORIE_NAME) };
#undef LORIE_NAME

static void lorieLogRendererDiag(void) {
    static struct lorie_renderer_diag prev;
    struct lorie_renderer_diag d;
    char reasons[1024];
    size_t used = 0;
    int i;

    memcpy(&d, (const void *) &pvfb->state->diag, sizeof(d));
#define TD(f) (unsigned long long) d.f, (unsigned long long) (d.f - prev.f)
    log(INFO, "XlorieBackend: requested=%s%s vk_fail_at=%s%s vk_selftest=%s%s active=%s ownership=%s ownership_transitions=%u "
              "ownership_refused=%u x_ahb_cpu_acquires=%llu(+%llu) x_state_lock_wait_us=%llu(+%llu) "
              "x_gpu_wait_us=%llu(+%llu) x_ahb_cpu_lock_us=%llu(+%llu) root_buffers=%u x_root_copy_us=%llu(+%llu) "
              "vk_transfer_copy=%s",
        pvfb->state->backend.requested == LORIE_BACKEND_VULKAN ? "vulkan"
            : pvfb->state->backend.requested == LORIE_BACKEND_GLES ? "gles" : "auto",
        pvfb->state->backend.requestedInvalid ? "(invalid-value)" : "",
        lorieVkFailAtNames[pvfb->state->backend.vkFailAt < LORIE_VK_FAIL_AT_COUNT ? pvfb->state->backend.vkFailAt : 0],
        pvfb->state->backend.vkFailAtInvalid ? "(invalid-value)" : "",
        pvfb->state->backend.vkSelftestFull ? "full" : "quick", pvfb->state->backend.vkSelftestInvalid ? "(invalid-value)" : "",
        lorieActiveName(d.active),
        d.xOwnership == LORIE_OWNERSHIP_PER_ACCESS ? "per_access" : "persistent",
        d.xOwnershipTransitions, d.xOwnershipRefused, TD(xAhbCpuAcquires), TD(xStateLockWaitUs), TD(xGpuWaitUs),
        TD(xAhbCpuLockUs), d.xRootBuffers ? d.xRootBuffers : 1, TD(xRootCopyUs),
        pvfb->state->backend.vkTransferCopy ? "on" : "off");
    log(INFO, "XlorieGles: egl_image_imports=%llu(+%llu) gles_copy_frames=%llu(+%llu) egl_swap_buffers=%llu(+%llu)",
        TD(eglImageImports), TD(glesCopyFrames), TD(eglSwapBuffers));
    log(INFO, "XlorieVk: instance_created=%llu(+%llu) device_created=%llu(+%llu) surface_created=%llu(+%llu) "
              "swapchain_created=%llu(+%llu) ahb_import_attempts=%llu(+%llu) ahb_import_success=%llu(+%llu) "
              "imported_source_draws=%llu(+%llu) imported_destination_draws=%llu(+%llu) copy_commands=%llu(+%llu) "
              "blit_commands=%llu(+%llu) render_commands=%llu(+%llu) queue_submits=%llu(+%llu) "
              "queue_present_success=%llu(+%llu) frames_presented=%llu(+%llu)",
        TD(vkInstanceCreated), TD(vkDeviceCreated), TD(vkSurfaceCreated), TD(vkSwapchainCreated),
        TD(vkAhbImportAttempts), TD(vkAhbImportSuccess), TD(vkImportedSourceDraws), TD(vkImportedDestinationDraws),
        TD(vkCopyCommands), TD(vkBlitCommands), TD(vkRenderCommands), TD(vkQueueSubmits),
        TD(vkQueuePresentSuccess), TD(vkFramesPresented));
    log(INFO, "XlorieVkTiming: acquire_success=%llu(+%llu) acquire_suboptimal=%llu(+%llu) acquire_timeout=%llu(+%llu) "
              "acquire_not_ready=%llu(+%llu) acquire_out_of_date=%llu(+%llu) acquire_wait_us=%llu(+%llu) "
              "submit_cpu_us=%llu(+%llu) fence_wait_us=%llu(+%llu) present_call_us=%llu(+%llu) selftest=%s "
              "device=\"%s\" driver_version=0x%x api_version=0x%x present_mode=\"%s\"",
        TD(vkAcquireSuccess), TD(vkAcquireSuboptimal), TD(vkAcquireTimeout), TD(vkAcquireNotReady),
        TD(vkAcquireOutOfDate), TD(vkAcquireWaitUs), TD(vkSubmitCpuUs), TD(vkFenceWaitUs), TD(vkPresentCallUs),
        d.vkSelftest == 1 ? "pass" : d.vkSelftest == 2 ? "fail" : "none",
        (const char *) d.vkDevice, d.vkDriverVersion, d.vkApiVersion, (const char *) d.vkPresentMode);
    log(INFO, "XlorieVkSelftest: result=%s compares=%llu(+%llu) iterations=%u formats_passed=0x%x formats_unavailable=0x%x "
              "us=%llu fail_step=%s fail_format=%u fail_direction=%u fail_iteration=%u fail_offset=%u fail_expected=0x%02x "
              "fail_actual=0x%02x fail_vkresult=%d",
        d.vkSelftest == 1 ? "pass" : d.vkSelftest == 2 ? "fail" : "none", TD(vkSelftestCompares), d.vkSelftestIterations,
        d.vkSelftestFormatsPassed, d.vkSelftestFormatsUnavailable, (unsigned long long) d.vkSelftestUs,
        lorieSelftestStepNames[d.vkSelftestFailStep % LORIE_VK_SELFTEST_STEP_COUNT], d.vkSelftestFailFormat,
        d.vkSelftestFailDirection, d.vkSelftestFailIteration, d.vkSelftestFailOffset, d.vkSelftestFailExpected,
        d.vkSelftestFailActual, d.vkSelftestFailResult);
    reasons[0] = 0;
    for (i = 1; i < LORIE_FALLBACK_REASON_COUNT; i++)
        if (d.fallbackByReason[i] && used < sizeof(reasons))
            used += (size_t) snprintf(reasons + used, sizeof(reasons) - used, "%s%s:%u", used ? "," : "",
                                      lorieFallbackReasonNames[i], d.fallbackByReason[i]);
    log(INFO, "XlorieFallback: total=%llu(+%llu) fatal_backend=%u resource_local=%u injected=%u last_class=%s "
              "last_reason=%s last_vkresult=%d last_resource_id=%llu last_resource_format=%u "
              "last_resource_usage=0x%llx reasons=%s",
        TD(fallbackTotal), d.fallbackByClass[LORIE_FALLBACK_CLASS_FATAL_BACKEND],
        d.fallbackByClass[LORIE_FALLBACK_CLASS_RESOURCE_LOCAL], d.fallbackByClass[LORIE_FALLBACK_CLASS_INJECTED],
        d.fallbackTotal ? lorieFallbackClassNames[d.lastFallbackClass % LORIE_FALLBACK_CLASS_COUNT] : "none",
        d.fallbackTotal ? lorieFallbackReasonNames[d.lastFallbackReason % LORIE_FALLBACK_REASON_COUNT] : "none",
        d.lastVkResult, (unsigned long long) d.lastResourceId, d.lastResourceFormat,
        (unsigned long long) d.lastResourceUsage, reasons[0] ? reasons : "none");
    // P9-B0: the Vulkan backend's Present copies, how long each part took (lorie.h struct lorie_renderer_diag copy*);
    // each part's share of the entries' whole time (copy_total_us) in entry time.
    {
        char queue[112], lock[112], inflight[112], record[112], gpu[112], frameGpu[112], total[112], ack[112];
        volatile struct lorie_renderer_diag *live = &pvfb->state->diag;
        uint64_t entryUs = d.copyTotal.sumUs - prev.copyTotal.sumUs;
        lorieLatencyText(queue, sizeof(queue), &d.copyQueue, &prev.copyQueue, &live->copyQueue.maxUs, entryUs);
        lorieLatencyText(lock, sizeof(lock), &d.copyLock, &prev.copyLock, &live->copyLock.maxUs, entryUs);
        lorieLatencyText(inflight, sizeof(inflight), &d.copyInflightWait, &prev.copyInflightWait, &live->copyInflightWait.maxUs, entryUs);
        lorieLatencyText(record, sizeof(record), &d.copyRecord, &prev.copyRecord, &live->copyRecord.maxUs, entryUs);
        lorieLatencyText(gpu, sizeof(gpu), &d.copyGpuWait, &prev.copyGpuWait, &live->copyGpuWait.maxUs, entryUs);
        lorieLatencyText(frameGpu, sizeof(frameGpu), &d.copyFrameGpuWait, &prev.copyFrameGpuWait, &live->copyFrameGpuWait.maxUs, entryUs);
        lorieLatencyText(total, sizeof(total), &d.copyTotal, &prev.copyTotal, &live->copyTotal.maxUs, 0);
        lorieLatencyText(ack, sizeof(ack), &d.copyAck, &prev.copyAck, &live->copyAck.maxUs, 0);
        log(INFO, "XlorieCopyLatency: copy_standalone_entries=%llu(+%llu) copy_frame_entries=%llu(+%llu) "
                  "copy_standalone_submits=%llu(+%llu) copy_frame_submits=%llu(+%llu) copy_queue_us=%s copy_lock_us=%s "
                  "copy_inflight_wait_us=%s copy_record_us=%s copy_gpu_wait_us=%s copy_frame_gpu_wait_us=%s copy_total_us=%s "
                  "copy_ack_us=%s",
            TD(copyStandaloneEntries), TD(copyFrameEntries), TD(copyStandaloneSubmits), TD(copyFrameSubmits),
            queue, lock, inflight, record, gpu, frameGpu, total, ack);
    }
    // TERMUX_X11_RENDERER_VK_TRANSFER_COPY's verdicts (renderer.c rendererVkCountTransfer), only while it is on.
    if (pvfb->state->backend.vkTransferCopy) {
        reasons[0] = 0;
        used = 0;
        for (i = 0; i < LORIE_VK_XFER_NOT_COUNT; i++)
            if ((d.vkXferLastNot & 1u << i) && used < sizeof(reasons))
                used += (size_t) snprintf(reasons + used, sizeof(reasons) - used, "%s%s", used ? "+" : "", lorieXferNotNames[i]);
        log(INFO, "XlorieVkTransfer: checked=%llu(+%llu) copied=%llu(+%llu) not_same=%llu(+%llu) not_format=%llu(+%llu) "
                  "not_rgbx_alpha=%llu(+%llu) not_depth32=%llu(+%llu) not_src_usage=%llu(+%llu) not_dst_usage=%llu(+%llu) "
                  "not_rect_source=%llu(+%llu) not_rect_overlap=%llu(+%llu) not_rect_none=%llu(+%llu) "
                  "usage_missing_unasked=%llu(+%llu) usage_missing_no_feature=%llu(+%llu) usage_missing_refused=%llu(+%llu) "
                  "usage_missing_failed=%llu(+%llu) imports_granted=%llu(+%llu) imports_no_feature=%llu(+%llu) "
                  "imports_refused=%llu(+%llu) imports_failed=%llu(+%llu) last_not=%s "
                  "last_src=%s(%u)/%ux%u/ahb_usage=0x%llx/vk_usage=0x%x "
                  "last_dst=%s(%u)/%ux%u/depth=%u/ahb_usage=0x%llx/vk_usage=0x%x last_rects=%u last_first=%d,%d-%d,%d "
                  "last_off=%d,%d bgra_rgbx_check=%s check_step=%s check_byte=%d check_expected=0x%02x check_actual=0x%02x "
                  "check_vkresult=%d",
            TD(vkXferChecked), TD(vkCopyCommands), TD(vkXferNot[LORIE_VK_XFER_NOT_SAME]),
            TD(vkXferNot[LORIE_VK_XFER_NOT_FORMAT]), TD(vkXferNot[LORIE_VK_XFER_NOT_RGBX_ALPHA]),
            TD(vkXferNot[LORIE_VK_XFER_NOT_DEPTH32]), TD(vkXferNot[LORIE_VK_XFER_NOT_SRC_USAGE]),
            TD(vkXferNot[LORIE_VK_XFER_NOT_DST_USAGE]), TD(vkXferNot[LORIE_VK_XFER_NOT_RECT_SOURCE]),
            TD(vkXferNot[LORIE_VK_XFER_NOT_RECT_OVERLAP]), TD(vkXferNot[LORIE_VK_XFER_NOT_RECT_NONE]),
            TD(vkXferUsageMissing[LORIE_VK_XFER_USAGE_UNASKED]), TD(vkXferUsageMissing[LORIE_VK_XFER_USAGE_NO_FEATURE]),
            TD(vkXferUsageMissing[LORIE_VK_XFER_USAGE_REFUSED]), TD(vkXferUsageMissing[LORIE_VK_XFER_USAGE_FAILED]),
            TD(vkXferImports[LORIE_VK_XFER_USAGE_GRANTED]), TD(vkXferImports[LORIE_VK_XFER_USAGE_NO_FEATURE]),
            TD(vkXferImports[LORIE_VK_XFER_USAGE_REFUSED]), TD(vkXferImports[LORIE_VK_XFER_USAGE_FAILED]),
            reasons[0] ? reasons : "none",
            lorieAhbFormatName(d.vkXferLastSrcFormat), d.vkXferLastSrcFormat, d.vkXferLastSrcWidth, d.vkXferLastSrcHeight,
            (unsigned long long) d.vkXferLastSrcAhbUsage, d.vkXferLastSrcUsage,
            lorieAhbFormatName(d.vkXferLastDstFormat), d.vkXferLastDstFormat, d.vkXferLastDstWidth, d.vkXferLastDstHeight,
            d.vkXferLastDstDepth, (unsigned long long) d.vkXferLastDstAhbUsage, d.vkXferLastDstUsage, d.vkXferLastRects,
            d.vkXferLastRect[0], d.vkXferLastRect[1], d.vkXferLastRect[2], d.vkXferLastRect[3], d.vkXferLastOff[0],
            d.vkXferLastOff[1], lorieXferCheckNames[d.vkXferCheck % LORIE_VK_XFER_CHECK_COUNT],
            lorieSelftestStepNames[d.vkXferCheckStep % LORIE_VK_SELFTEST_STEP_COUNT], d.vkXferCheckOffset,
            d.vkXferCheckExpected, d.vkXferCheckActual, d.vkXferCheckResult);
    }
#undef TD
    prev = d;
}

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
                  "fence wait %.1f ms total, copies on %u frames, coalesced %u",
            (double) pvfb->state->presentStats.frameSumUs / samples / 1000.0,
            pvfb->state->presentStats.maxFrameUs / 1000.0,
            LORIE_LONG_FRAME_US / 1000,
            pvfb->state->presentStats.longFrames,
            pvfb->state->presentStats.fenceWaitUs / 1000.0,
            pvfb->state->presentStats.gpuCopyFrames,
            pvfb->state->presentStats.coalescedFrames);
    }

    pvfb->state->presentStats.frameSamples = 0;
    pvfb->state->presentStats.frameSumUs = 0;
    pvfb->state->presentStats.maxFrameUs = 0;
    pvfb->state->presentStats.longFrames = 0;
    pvfb->state->presentStats.fenceWaitUs = 0;
    pvfb->state->presentStats.gpuCopyFrames = 0;
    pvfb->state->presentStats.coalescedFrames = 0;

    pvfb->state->renderedFrames = 0;
    gpuCopyAttempts = gpuCopyOffloads = 0;

    if (lorieConnectionAlive())
        lorieLogRendererDiag();
    return 5000;
}

static Bool lorieCreateScreenResources(ScreenPtr pScreen) {
    pScreen->devPrivate = pScreen->CreatePixmap(pScreen, pScreen->width, pScreen->height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, pvfb->damage);
    pvfb->fpsTimer = TimerSet(NULL, 0, 5000, lorieFramecounter, pScreen);

    lorieRegisterRoot();

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
        lorieDropPersistent(old);
        if (new && new->buffer && !new->locked) {
            lorieHoldPersistent(new);
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

    lorieRegisterRoot();

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
    *ust = GetTimeInMicros();
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
            present_event_notify(vblank->id, GetTimeInMicros(), pvfb->current_msc);
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

    // Root double buffering: the X server's own copy into the buffer it draws into. A GPU copy there would have
    // the X server wait for the device again, which is what the twin is for.
    if (dst == pScreenPtr->devPrivate && lorieRootTwinned()) {
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
    }
    *out_dst_buffer = dstIsRoot ? NULL : dstBuffer;

    entry = &pvfb->state->gpuCopyQueue.entries[writeIndex % LORIE_GPU_COPY_QUEUE_CAPACITY];
    entry->serial = ++pvfb->gpuCopySerialCounter;
    entry->srcBufferId = desc->id;
    entry->dstBufferId = dstDesc->id;
    entry->xOff = x_off;
    entry->yOff = y_off;
    entry->numRects = (uint16_t) numRects;
    entry->dstDepth = (uint8_t) dst->drawable.depth;
    entry->queuedUs = lorieMonotonicUs();
    for (i = 0; i < numRects; i++)
        entry->rects[i] = (LorieGpuCopyRect) { box[i].x1, box[i].y1, box[i].x2, box[i].y2 };

    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above
    pthread_cond_signal(rendererCond);

    *out_serial = entry->serial;
    gpuCopyAttempts++;
    gpuCopyOffloads++;
    return TRUE;
}

// P9-B0: from the Vulkan renderer's latest completedSerial publish to here (cmdentrypoint.c, after the X server
// re-executed what it completed: lorieRecheckGpuCopies), once per publish.
void lorieCountCopyAck(void) {
    static uint64_t counted;
    uint64_t published = __atomic_load_n(&pvfb->state->gpuCopyQueue.completedUs, __ATOMIC_RELAXED), now = lorieMonotonicUs();
    if (!published || published == counted)
        return;
    counted = published;
    lorieLatencyAdd(&pvfb->state->diag.copyAck, now > published ? now - published : 0);
}

Bool lorieGpuCopyIsDone(uint64_t serial) {
    return __atomic_load_n(&pvfb->state->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE) >= serial;
}

void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer) {
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap);
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
    return TRUE;
}

void loriePresentAfterFlip(__unused RRCrtcPtr crtc, uint64_t event_id, uint64_t ust, uint64_t target_msc, __unused PixmapPtr pixmap) {
    // X server was patched to call this function right after finishing all present_flip shenanigans
    // Since we do not invoke DRM API or anything similar we do not need to implement this as callback
    // For some reason calling present_event_notify in BlockHandler or as QueueWorkProc/eventfd callback
    // adds some delay which may be easily avoided this way.
    static BoxRec box = { 0, 0, 1, 1 }; // lorieRedraw only checks if it is empty or not.
    RegionReset(DamageRegion(pvfb->damage), &box);
    pvfb->current_msc = min(pvfb->current_msc + 1, target_msc);
    present_event_notify(event_id, ust, pvfb->current_msc);
}

void loriePresentUnflip(__unused ScreenPtr screen, uint64_t event_id) {
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
    // Root double buffering: the screen pixmap's twin, the same in every way (the pixmap's stride serves both).
    if (pvfb->rootDouble && type == LORIEBUFFER_AHARDWAREBUFFER && priv->buffer) {
        priv->twin = LorieBuffer_allocate(width, height, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, type);
        if (priv->twin && LorieBuffer_description(priv->twin)->stride != LorieBuffer_description(priv->buffer)->stride) {
            LorieBuffer_release(priv->twin);
            priv->twin = NULL;
        }
        pvfb->rootTwinStale = TRUE;
    }

    lorieHoldPersistent(priv);
    if (!priv->buffer) {
        free(priv);
        return NULL;
    }

    return priv;
}

void lorieExaDestroyPixmap(__unused ScreenPtr pScreen, void *driverPriv) {
    LoriePixmapPriv *priv = driverPriv;
    if (priv->persistentWanted) {
        xorg_list_del(&priv->persistentLink);
        priv->persistentWanted = FALSE;
    }
    if (priv->buffer) {
        if (priv->locked)
            LorieBuffer_unlock(priv->buffer);
        lorieUnregisterBuffer(priv->buffer);
        LorieBuffer_release(priv->buffer);
    }
    if (priv->twin) {
        lorieUnregisterBuffer(priv->twin);
        LorieBuffer_release(priv->twin);
    }
    free(priv);
}

Bool lorieModifyPixmapHeader(PixmapPtr pPix, __unused int w, __unused int h, __unused int depth, __unused int bitsPerbppPixel, __unused int devKind, __unused void *data) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (priv && data)
        priv->mem = data;
    return FALSE;
}

// Whether a CPU access to pPix could race a GPU write from the renderer, and so needs state->lock.
static inline __always_inline Bool lorieNeedsGpuLock(PixmapPtr pPix, LoriePixmapPriv *priv, int index) {
    // PER_ACCESS (Vulkan): every CPU access to an AHB is exclusive with the renderer's use of it, which
    // happens only while the renderer holds the same lock.
    if (lorieAhbPerAccess() && lorieIsAhb(priv))
        return TRUE;
    if (pScreenPtr->GetScreenPixmap(pScreenPtr) == pPix)
        return index == EXA_PREPARE_DEST || (pvfb->rootGpuCopyPending && !pvfb->root.legacyDrawing);
    return !pvfb->root.legacyDrawing && priv->buffer &&
           LorieBuffer_description(priv->buffer)->type == LORIEBUFFER_AHARDWAREBUFFER &&
           LorieBuffer_hasGpuCopyPending(priv->buffer);
}

Bool loriePrepareAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    Bool perAccessAhb = lorieAhbPerAccess() && lorieIsAhb(priv);
    if (lorieNeedsGpuLock(pPix, priv, index)) {
        uint64_t waitStartUs = perAccessAhb ? GetTimeInMicros() : 0;
        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
        // PER_ACCESS: the renderer lets the lock go while the device runs a frame; not while it uses this one.
        if (perAccessAhb && !priv->locked && !priv->mem) {
            uint64_t gpuStartUs = GetTimeInMicros();
            lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
            __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, GetTimeInMicros() - gpuStartUs, __ATOMIC_RELAXED);
        }
        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xStateLockWaitUs, GetTimeInMicros() - waitStartUs, __ATOMIC_RELAXED);
    }

    if (!priv->locked && !priv->mem) {
        uint64_t lockStartUs = perAccessAhb ? GetTimeInMicros() : 0;
        int err = LorieBuffer_lock(priv->buffer, &priv->locked);
        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - lockStartUs, __ATOMIC_RELAXED);
        if (err) {
            dprintf(2, "Failed to lock buffer, err %d\n", err);
            // PER_ACCESS: no FinishAccess follows a failed PrepareAccess, and the renderer would wait on
            // this lock for good. (3c98ffa's own path is left as it was.)
            if (perAccessAhb)
                lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
            return FALSE;
        }
        priv->wasLocked = FALSE;
        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuAcquires, 1, __ATOMIC_RELAXED);
    } else
        priv->wasLocked = TRUE;

    pPix->devPrivate.ptr = priv->locked ?: priv->mem;
    return TRUE;
}

void lorieFinishAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);

    // PER_ACCESS: the CPU lock goes before the shared one, so the renderer can never take the shared lock
    // and use the AHB while the X server still has it CPU-locked. 3c98ffa's order is kept otherwise.
    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        if (!priv->wasLocked) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        return;
    }

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
