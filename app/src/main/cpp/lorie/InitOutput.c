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
#include "client.h"
#include "extnsionst.h"
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

// P11-C3-C (TERMUX_X11_FRAME_FREEZE_DIAG=1): what the X server's thread is in - for the freeze watchdog - and since when
// (lorieMonotonicUs): the stage to go back to once it is over (lorieXEnter, lorieXLeave).
enum { LORIE_X_LOOP, LORIE_X_TICK, LORIE_X_REQUEST, LORIE_X_ROOT_COPY_WAIT, LORIE_X_REPORT, LORIE_X_STAGES };
#define LORIE_CONTENT_CLIENTS 8     /* P11-C3-D0: the presenting clients counted apart */
#define LORIE_COMPOSITE_CLASSES 8   /* P11-C3-D1: the RENDER composites' classes counted apart */
struct lorieXMark { uint32_t stage; uint64_t us; };

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
    /* Its swap's own CPU locks (TERMUX_X11_RENDERER_ROOT_COPY_OPT=1, lorieRootSwap): asked for; given up for the
     * session once one was refused */
    Bool rootCopyOpt, rootCopyOptFailed;
    /* P11-C2, its copy by the renderer's GPU (TERMUX_X11_RENDERER_ROOT_GPU_COPY=1, lorie.h rootCopy): asked for; the
     * last request's number; the request not settled yet (seq 0: none) - its buffers and boxes, for the CPU copy
     * should it come to that, and whether they went meanwhile (abandoned); the shared locks EXA's open accesses hold
     * (loriePrepareAccess), which a renderer would wait for */
    Bool rootGpuCopy;
    uint32_t rootCopySeq;
    struct {
        uint32_t seq;
        LorieBuffer *from, *to;
        int n;
        BoxRec boxes[LORIE_GPU_COPY_MAX_RECTS];
        Bool abandoned;
    } rootCopy;
    int accessLocks;

    /* TERMUX_X11_X_YIELD_ON_WAKE=1 (lorieQueueUrgentWorkProc): asked for - read from other threads, atomically */
    int yieldOnWake;

    /* P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1, lorie.h struct lorie_hist): asked for; whether the last tick asked
     * the renderer for a frame, and the frames presented then (lorieFramesShown); the period's slowest client request
     * (ddxRequestTimed) - its time, its client's index and command, its opcodes and their extension */
    Bool frameDiag, frameAsked;
    uint32_t framesAtAsk;
    struct {
        uint32_t us;
        int client, major, minor;
        char cmd[24], ext[24];
    } worstRequest;
    /* ... and its slowest PER_ACCESS AHB access - its time, its pixmap's size, EXA's index, reserved or not */
    struct {
        uint32_t us;
        int width, height, index;
        Bool reserved;
    } worstAccess;

    /* P11-C3-B1 (TERMUX_X11_EXA_LOCK_OPT=1, lorie.h cpuAccess): PER_ACCESS AHB accesses reserve the AHB instead of
     * holding the shared lock through the access */
    Bool exaLockOpt;

    /* P11-C3-D1-A (TERMUX_X11_EXA_COMPOSITE_OPT=1, lorieDeferCopyDamage): a Present GPU copy's damage reported once the
     * copy is done */
    Bool exaCompositeOpt;

    /* P11-C3-C (TERMUX_X11_FRAME_FREEZE_DIAG=1, the freeze watchdog: lorieFreezeWatch): asked for; where this thread is
     * (LORIE_X_*, since when), the request it is in and the stage it came from, the requests run; the 5 second
     * report's own time (the last, and the most since the watchdog last logged the threads); the last GPU copy
     * completion the input thread was told of, and the last this thread rechecked the copies for (serial, when) - all
     * read by the watchdog's thread, atomically. The threads it looks at in /proc: this one, the input thread, the
     * Choreographer's. */
    Bool freezeDiag;
    uint32_t xStage, xRequests;
    uint64_t xStageUs;
    int xRequestClient, xRequestMajor, xRequestMinor;
    struct lorieXMark requestMark;
    uint64_t reportUs, reportMaxUs;
    uint64_t copyNotifiedSerial, copyNotifiedUs, copyAckedSerial, copyAckedUs;
    pid_t mainTid, inputTid, vsyncTid;

    /* P11-C3-D0 (TERMUX_X11_CONTENT_FRAME_DIAG=1, lorieContent*): asked for; the presentations numbered, the screen
     * pixmap's publishings and the content last published; the CPU accesses open (a bit by EXA's index) and the content
     * each index's sources carry (into the destination as its access finishes) - and what it carried before its last
     * opening, for an opening that fails; the clients presenting - LORIE_CONTENT_CLIENTS of them, each named as it
     * came, its pointer only compared (never followed) */
    Bool contentDiag;
    uint32_t contentSeq, contentGen, contentPublishedSeq;
    uint32_t contentOpen;
    struct lorie_content_stamp contentFrom[EXA_NUM_PREPARE_INDICES], contentBefore[EXA_NUM_PREPARE_INDICES];
    /* ... and, for each slot of the GPU copy queue, the copy the X server queued there last and whether its damage was
     * reported (lorieContentDamaged): the stale reads' (lorieContentStaleRead) */
    struct { uint64_t serial; Bool damaged; } contentQueued[LORIE_GPU_COPY_QUEUE_CAPACITY];
    struct {
        const void *client;
        int index;
        char cmd[24];
        uint32_t requests, skipped, requestsAt, skippedAt, maxGapUs;
        uint64_t lastUs;
    } contentClients[LORIE_CONTENT_CLIENTS];

    /* P11-C3-D1 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1, lorieCompositeTimed): the RENDER composites, by class (operator,
     * source, destination, mask, transform: lorieCompositeKey) - LORIE_COMPOSITE_CLASSES of them this period, the rest
     * counted together in the last slot - each with its time split into what it waited for (the GPU, the root copy,
     * the shared lock), its AHB CPU locks and what is left (pixman's and EXA's own); the EXA accesses opened
     * (loriePrepareAccess) and the AHBs CPU-locked; the period's slowest */
    CompositeProcPtr composite;
    uint32_t xAccessesOpened;
    uint64_t composites;
    struct lorie_composite_class {
        uint32_t key, n, accesses, locks, maxUs;
        uint64_t px, us, gpuUs, rootUs, lockUs, ahbUs;
    } compositeClasses[LORIE_COMPOSITE_CLASSES + 1];
    struct { uint32_t key, us, gpuUs, rootUs, lockUs, ahbUs, width, height; } worstComposite;
} lorieScreenInfo;

// P11-C2: root copies smaller than this stay on the X server's CPU. A guess to check on the device: the S24+'s root
// copies ran at about 2.6 GB/s on the CPU (x_root_copy_us), so 1 MiB took about 0.4 ms, about a round trip to the
// renderer's GPU and back.
#define LORIE_ROOT_GPU_COPY_MIN_BYTES (1u << 20)
static void lorieRootCopyWait(Bool takeBack);
static void lorieCompositeTimed(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 xSrc, INT16 ySrc,
                                INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst, CARD16 width, CARD16 height);
static void lorieCompositeReport(char *totals, size_t totalsSize, char *classes, size_t classesSize);

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

// P11-C3-C: this thread into `stage`, if the freeze watchdog is on - what it was in, to go back to
static struct lorieXMark lorieXEnter(uint32_t stage) {
    struct lorieXMark prev = { LORIE_X_LOOP, 0 };
    if (pvfb->freezeDiag) {
        prev.stage = __atomic_load_n(&pvfb->xStage, __ATOMIC_RELAXED);
        prev.us = __atomic_load_n(&pvfb->xStageUs, __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->xStageUs, lorieMonotonicUs(), __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->xStage, stage, __ATOMIC_RELEASE);
    }
    return prev;
}

// ... and back: to the stage it was in, since when it was (the main loop: since now). A cleanup attribute's too.
static void lorieXLeave(struct lorieXMark *prev) {
    if (pvfb->freezeDiag) {
        __atomic_store_n(&pvfb->xStageUs, prev->stage == LORIE_X_LOOP ? lorieMonotonicUs() : prev->us, __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->xStage, prev->stage, __ATOMIC_RELEASE);
    }
}

static void *lorieFreezeWatch(void *arg);

// The renderer's wake (lorie.h struct lorie_renderer_wake), owned by the activity process, handed to us over the
// connection socket. Points at a placeholder until the first connection so callers don't need a NULL check.
static struct lorie_renderer_wake rendererWakePlaceholder;
static struct lorie_renderer_wake* volatile rendererWake = &rendererWakePlaceholder;

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
    /* P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1): when its PER_ACCESS access took the shared lock (0: not timed),
     * and how long it held it before letting it go for a reservation */
    uint64_t accessStartUs;
    uint32_t accessHoldUs;
    /* P11-C3-B1: its open access holds a reservation (lorie.h cpuAccess) instead of the shared lock */
    bool reserved;
    /* P11-C3-D0 (TERMUX_X11_CONTENT_FRAME_DIAG=1): the newest client content in it */
    struct lorie_content_stamp content;
} LoriePixmapPriv;
static void lorieContentPublish(LoriePixmapPriv *priv);

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

    // P11-C2: no root copy left to a GPU that may lose the buffers - taken back, if not taken: the renderer waits for
    // this answer, or another one, which gets the shared state only once it is given, is to take over
    lorieRootCopyWait(TRUE);
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
    lorieWakeRenderer(rendererWake);
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
    lorieRecheckGpuCopies();    // P10-S1: copies no renderer can run any more settle now, not at the next tick
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
    // Experimental: TERMUX_X11_RENDERER_VK_SUBMIT_OPT=1, the Vulkan frames' submissions and presents (renderer.c).
    lorieScreen.state->backend.vkSubmitOpt = getenv("TERMUX_X11_RENDERER_VK_SUBMIT_OPT") &&
                                             !strcmp(getenv("TERMUX_X11_RENDERER_VK_SUBMIT_OPT"), "1");
    // Experimental: TERMUX_X11_RENDERER_ROOT_DOUBLE=1 gives the screen pixmap a second buffer (lorieRootSwap).
    lorieScreen.rootDouble = getenv("TERMUX_X11_RENDERER_ROOT_DOUBLE") && !strcmp(getenv("TERMUX_X11_RENDERER_ROOT_DOUBLE"), "1");
    // Experimental: TERMUX_X11_RENDERER_ROOT_COPY_OPT=1 has that swap lock the back for reading and the front for writing,
    // each only within the bounds of the damage copied (lorieRootSwap).
    lorieScreen.rootCopyOpt = getenv("TERMUX_X11_RENDERER_ROOT_COPY_OPT") && !strcmp(getenv("TERMUX_X11_RENDERER_ROOT_COPY_OPT"), "1");
    // Experimental: TERMUX_X11_RENDERER_ROOT_GPU_COPY=1 has the renderer's GPU make that swap's copy, after the swap
    // (lorieRootGpuCopyAsk, lorieRootCopyWait).
    lorieScreen.rootGpuCopy = getenv("TERMUX_X11_RENDERER_ROOT_GPU_COPY") && !strcmp(getenv("TERMUX_X11_RENDERER_ROOT_GPU_COPY"), "1");
    // Experimental: TERMUX_X11_X_YIELD_ON_WAKE=1 lets a vsync tick or a GPU copy's completion break off the client
    // dispatch round in progress at its next request boundary, to run before the round goes on (lorieQueueUrgentWorkProc).
    __atomic_store_n(&lorieScreen.yieldOnWake, getenv("TERMUX_X11_X_YIELD_ON_WAKE") && !strcmp(getenv("TERMUX_X11_X_YIELD_ON_WAKE"), "1"),
                     __ATOMIC_RELAXED);
    // Diagnostics: TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 times the Vulkan frames, the root copies, and the X server's
    // requests, ticks and AHB accesses, logged every 5 seconds (XlorieVkFramePacing and the lines after it). Off, none
    // of them reads a clock.
    lorieScreen.frameDiag = getenv("TERMUX_X11_RENDERER_VK_FRAME_DIAG") && !strcmp(getenv("TERMUX_X11_RENDERER_VK_FRAME_DIAG"), "1");
    lorieScreen.state->backend.vkFrameDiag = lorieScreen.frameDiag;
    // Diagnostics: TERMUX_X11_FRAME_FREEZE_DIAG=1 watches, from a thread of its own, for work waiting with nothing
    // moving - in the X server or the renderer - and logs each such freeze as it begins and ends, and this process's
    // threads every 5 seconds (XlorieFreeze, XlorieXSched: lorieFreezeWatch). Off, none of it runs: no thread, no stage,
    // no clock.
    lorieScreen.freezeDiag = getenv("TERMUX_X11_FRAME_FREEZE_DIAG") && !strcmp(getenv("TERMUX_X11_FRAME_FREEZE_DIAG"), "1");
    // Diagnostics: TERMUX_X11_CONTENT_FRAME_DIAG=1 follows each client presentation's content from its request through
    // its copy, the compositor's CPU accesses and the screen pixmap to the renderer's frame (XlorieContent* lines:
    // lorieContent*, renderer.c rendererContentFrame). Off, none of it reads a clock or stamps a pixmap.
    lorieScreen.contentDiag = getenv("TERMUX_X11_CONTENT_FRAME_DIAG") && !strcmp(getenv("TERMUX_X11_CONTENT_FRAME_DIAG"), "1");
    lorieScreen.state->backend.contentDiag = lorieScreen.contentDiag;
    lorieScreen.state->backend.freezeDiag = lorieScreen.freezeDiag;
    lorieScreen.mainTid = gettid();
    dispatchRequestTimed = lorieScreen.frameDiag || lorieScreen.freezeDiag;
    // Experimental: TERMUX_X11_EXA_LOCK_OPT=1 has a PER_ACCESS AHB access reserve the AHB for itself instead of holding
    // the shared lock - and so the renderer's frames - through the CPU drawing (loriePrepareAccess, lorie.h cpuAccess).
    lorieScreen.exaLockOpt = getenv("TERMUX_X11_EXA_LOCK_OPT") && !strcmp(getenv("TERMUX_X11_EXA_LOCK_OPT"), "1");
    // Experimental: TERMUX_X11_EXA_COMPOSITE_OPT=1 reports a Present GPU copy's damage once the copy is done, not as it is
    // queued - a compositor then reads (composites) the window's content with the copy in it, without waiting for the
    // frame that runs it (lorieDeferCopyDamage, patches/xserver.patch present_damage_gpu_copy).
    lorieScreen.exaCompositeOpt = getenv("TERMUX_X11_EXA_COMPOSITE_OPT") && !strcmp(getenv("TERMUX_X11_EXA_COMPOSITE_OPT"), "1");
    // Experimental: TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1 has the renderer submit root double buffering's GPU copy
    // ahead of the frame, which waits for it on the device instead of the renderer waiting for its fence (renderer.c).
    lorieScreen.state->backend.vkScheduleOpt = getenv("TERMUX_X11_RENDERER_VK_SCHEDULE_OPT") &&
                                               !strcmp(getenv("TERMUX_X11_RENDERER_VK_SCHEDULE_OPT"), "1");
    // Experimental: TERMUX_X11_ROOT_COPY_SCHED_OPT=1 has the renderer settle that root copy submitted ahead as soon as it
    // may - its fence signalled, or the X server waiting for it - at each step of the frame that would keep it
    // unsettled until the frame is submitted, its wait for this lock too (renderer.c rendererVkAuxEarly). Only with
    // TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1: without it the root copy is settled before the frame begins.
    lorieScreen.state->backend.rootCopySchedOpt = getenv("TERMUX_X11_ROOT_COPY_SCHED_OPT") &&
                                                  !strcmp(getenv("TERMUX_X11_ROOT_COPY_SCHED_OPT"), "1");
    if (lorieScreen.freezeDiag) {
        pthread_t watch;
        if (pthread_create(&watch, NULL, lorieFreezeWatch, NULL) == 0)
            pthread_detach(watch);
        else
            log(ERROR, "XlorieFreeze: the watchdog's thread was not made");
    }
}

// A name as one word for a log line: a path's last part, up to its first space, anything else odd as '_'.
static void lorieDiagWord(char *out, size_t size, const char *in) {
    size_t i = 0;
    const char *base = in ? strrchr(in, '/') : NULL;
    for (in = base ? base + 1 : in ?: "unknown"; *in && *in != ' ' && i + 1 < size; in++)
        out[i++] = (*in >= 'a' && *in <= 'z') || (*in >= 'A' && *in <= 'Z') || (*in >= '0' && *in <= '9') ||
                   *in == '-' || *in == '.' || *in == '_' ? *in : '_';
    out[i] = 0;
}

/*
 * P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1): one client request's handling, from right before its request procedure
 * to right after it (dix/dispatch.c Dispatch, which reads the clock only while this is on); what it was is taken
 * before it ran, as it may have closed its own client. The period's slowest is kept with its client's command name
 * and its extension's name.
 */
/*
 * P11-C3-C: a client request about to be handled (dix/dispatch.c Dispatch, while dispatchRequestTimed): for the freeze
 * watchdog, which request this thread is in; the clock for ddxRequestTimed.
 */
CARD64 ddxRequestStart(int clientIndex, int major, int minor) {
    if (pvfb->freezeDiag) {
        __atomic_store_n(&pvfb->xRequestClient, clientIndex, __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->xRequestMajor, major, __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->xRequestMinor, minor, __ATOMIC_RELAXED);
        pvfb->requestMark = lorieXEnter(LORIE_X_REQUEST);
    }
    return GetTimeInMicros();
}

void ddxRequestTimed(int clientIndex, int major, int minor, CARD64 startUs) {
    CARD64 us;
    if (pvfb->freezeDiag) {
        __atomic_fetch_add(&pvfb->xRequests, 1, __ATOMIC_RELAXED);
        lorieXLeave(&pvfb->requestMark);
    }
    if (!pvfb->frameDiag)
        return;
    us = GetTimeInMicros() - startUs;
    lorieHistAdd(&pvfb->state->diag.xRequest, us);
    if (us > pvfb->worstRequest.us) {
        ClientPtr client = clientIndex >= 0 && clientIndex < MAXCLIENTS ? clients[clientIndex] : NULL;
        ExtensionEntry *ext = major >= EXTENSION_BASE ? GetExtensionEntry(major) : NULL;
        pvfb->worstRequest.us = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
        pvfb->worstRequest.client = clientIndex;
        pvfb->worstRequest.major = major;
        pvfb->worstRequest.minor = major >= EXTENSION_BASE ? minor : 0;
        lorieDiagWord(pvfb->worstRequest.cmd, sizeof(pvfb->worstRequest.cmd), client ? GetClientCmdName(client) : NULL);
        lorieDiagWord(pvfb->worstRequest.ext, sizeof(pvfb->worstRequest.ext), major < EXTENSION_BASE ? "core" : ext ? ext->name : NULL);
    }
}

// Queued from handleLorieEvents (input thread) to run on the main thread, which wakes the renderer from
// lorieRedraw and the rest. lorieMoveCursor wakes it from the input thread too (QueuePointerEvents moves the sprite
// there, under the input lock): the swap is made under that lock, so the unmap below can never race a wake.
static Bool lorieSetRendererWakeupCondWorkProc(__unused ClientPtr client, void* closure) {
    int fd = (int) (intptr_t) closure;
    struct lorie_renderer_wake* newWake = mmap(NULL, sizeof(struct lorie_renderer_wake), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    struct lorie_renderer_wake* old;
    close(fd); // mmap already keeps the region alive.
    if (newWake == MAP_FAILED) {
        log(ERROR, "Failed to map renderer wakeup region, keeping the old one");
        return TRUE;
    }

    input_lock();
    old = rendererWake;
    rendererWake = newWake;
    input_unlock();
    lorieWakeRenderer(newWake); // in case a wake was sent to `old` right before this swap

    if (old != &rendererWakePlaceholder)
        munmap(old, sizeof(struct lorie_renderer_wake));

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
        uint64_t waitStartUs, gpuStartUs, lockStartUs;
        lorieRootCopyWait(FALSE);   // P11-C2: not into the root's buffer before its copy settled
        waitStartUs = GetTimeInMicros();
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
void ddxInputThreadInit(void) {
    __atomic_store_n(&pvfb->inputTid, gettid(), __ATOMIC_RELEASE);     // P11-C3-C: for the freeze watchdog's look at it
}
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
    // We are simply waking the renderer in the case if it sleeps.
    lorieWakeRenderer(rendererWake);
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

// P11-B: the bounds of what lorieRootSwap copies - its boxes as it clips them - for its CPU locks; NULL (the whole
// buffer) when nothing is to be copied.
static ARect *lorieRootCopyBounds(BoxPtr box, int n, const LorieBuffer_Desc *d, ARect *bounds) {
    int x1 = d->width, y1 = d->height, x2 = 0, y2 = 0, i;
    for (i = 0; i < n; i++) {
        int bx1 = max(box[i].x1, 0), by1 = max(box[i].y1, 0), bx2 = min(box[i].x2, d->width), by2 = min(box[i].y2, d->height);
        if (bx2 > bx1 && by2 > by1)
            x1 = min(x1, bx1), y1 = min(y1, by1), x2 = max(x2, bx2), y2 = max(y2, by2);
    }
    if (x2 <= x1 || y2 <= y1)
        return NULL;
    *bounds = (ARect) { .left = x1, .top = y1, .right = x2, .bottom = y2 };
    return bounds;
}

/*
 * The CPU copy of a swap, the shared lock held: from the buffer the X server drew into (`back`) into the other
 * (`front`) - once no frame uses either (the last one sampling the front among them) - the damage since the last
 * swap (Damage on the screen pixmap sees every drawing into it), or all of it when the twin is out of date. False,
 * and nothing copied, if a buffer cannot be CPU-locked. `waitStartUs`: when the shared lock was asked for.
 */
static Bool lorieRootCopyCpu(LorieBuffer *back, LorieBuffer *front, BoxPtr box, int n, uint64_t waitStartUs) {
    const LorieBuffer_Desc *d = LorieBuffer_description(back);
    int i, failed;
    uint32_t *src = NULL, *dst = NULL;
    uint64_t gpuStartUs, lockStartUs, copyStartUs, copyEndUs;
    ARect bounds;

    gpuStartUs = GetTimeInMicros();
    lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(front)->id);     // the last frame sampling it
    lorieGpuWaitIdle(pvfb->state, false, d->id);
    lockStartUs = GetTimeInMicros();
    failed = TRUE;
    // P11-B (TERMUX_X11_RENDERER_ROOT_COPY_OPT=1): the back locked only for reading and the front only for writing,
    // each only within the bounds of what is copied - the CPU cache upkeep of the rest spared, their addresses still
    // the buffers' own. Either lock refused, what was locked goes, and so do these locks for the session: the ones
    // below (reads and writes, the whole buffers) are taken instead.
    if (pvfb->rootCopyOpt && !pvfb->rootCopyOptFailed) {
        ARect *rect = lorieRootCopyBounds(box, n, d, &bounds);
        failed = LorieBuffer_lockRect(back, false, rect, (void **) &src);
        if (!failed && (failed = LorieBuffer_lockRect(front, true, rect, (void **) &dst)))
            LorieBuffer_unlock(back);
        pvfb->rootCopyOptFailed = failed != 0;
    }
    if (failed) {
        failed = LorieBuffer_lock(back, (void **) &src);
        failed |= LorieBuffer_lock(front, (void **) &dst);
        failed |= !src || !dst;
    }
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
    return !failed;
}

// P11-C2: a renderer waits for the ownership answer the X server's thread gives (rendererVkOwnership).
static Bool lorieOwnershipAsked(void) {
    return LORIE_OWNERSHIP_SEQ(__atomic_load_n(&pvfb->state->backend.ownershipRequest, __ATOMIC_ACQUIRE)) !=
           LORIE_OWNERSHIP_SEQ(__atomic_load_n(&pvfb->state->backend.ownershipAck, __ATOMIC_ACQUIRE));
}

/*
 * P11-C2 (TERMUX_X11_RENDERER_ROOT_GPU_COPY=1), the shared lock held: the swap's copy asked of the renderer's GPU
 * (lorie.h rootCopy) - its boxes clipped as the CPU copy clips them, those past the last slot copied within the
 * bounds of the rest (outside the damage both buffers hold the same) - if a renderer is there to make it, none
 * waits for this thread's ownership answer, and it is big enough to be worth a GPU round trip. False otherwise:
 * the CPU copies it.
 */
static Bool lorieRootGpuCopyAsk(LorieBuffer *back, LorieBuffer *front, BoxPtr box, int n) {
    struct lorie_shared_server_state *st = pvfb->state;
    const LorieBuffer_Desc *d = LorieBuffer_description(back);
    LorieGpuCopyRect rects[LORIE_GPU_COPY_MAX_RECTS];
    uint64_t bytes = 0;
    int i, k = 0;

    if (!pvfb->rootGpuCopy || !lorieConnectionAlive() || !lorieRendererAvailable() || lorieOwnershipAsked())
        return FALSE;
    for (i = 0; i < n; i++) {
        int x1 = max(box[i].x1, 0), y1 = max(box[i].y1, 0), x2 = min(box[i].x2, d->width), y2 = min(box[i].y2, d->height);
        LorieGpuCopyRect *r = &rects[LORIE_GPU_COPY_MAX_RECTS - 1];
        if (x2 <= x1 || y2 <= y1)
            continue;
        if (k < LORIE_GPU_COPY_MAX_RECTS)
            rects[k++] = (LorieGpuCopyRect) { (int16_t) x1, (int16_t) y1, (int16_t) x2, (int16_t) y2 };
        else
            *r = (LorieGpuCopyRect) { (int16_t) min(r->x1, x1), (int16_t) min(r->y1, y1), (int16_t) max(r->x2, x2), (int16_t) max(r->y2, y2) };
    }
    for (i = 0; i < k; i++)
        bytes += (uint64_t) (rects[i].x2 - rects[i].x1) * (uint64_t) (rects[i].y2 - rects[i].y1) * 4;
    if (bytes < LORIE_ROOT_GPU_COPY_MIN_BYTES) {
        __atomic_fetch_add(&st->diag.rootCpuSmall, 1, __ATOMIC_RELAXED);
        return FALSE;
    }
    st->rootCopy.srcId = d->id;
    st->rootCopy.dstId = LorieBuffer_description(front)->id;
    st->rootCopy.numRects = (uint32_t) k;
    memcpy((void *) st->rootCopy.rects, rects, sizeof(rects[0]) * (size_t) k);
    st->rootCopy.queuedUs = lorieMonotonicUs();
    if (!++pvfb->rootCopySeq)
        ++pvfb->rootCopySeq;    // 0 is none
    __atomic_store_n(&st->rootCopy.seq, pvfb->rootCopySeq, __ATOMIC_RELEASE);
    pvfb->rootCopy.seq = pvfb->rootCopySeq;
    pvfb->rootCopy.from = back;
    pvfb->rootCopy.to = front;
    pvfb->rootCopy.n = k;
    for (i = 0; i < k; i++)
        pvfb->rootCopy.boxes[i] = (BoxRec) { rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
    pvfb->rootCopy.abandoned = FALSE;
    __atomic_fetch_add(&st->diag.rootGpuCopyRequests, 1, __ATOMIC_RELAXED);
    lorieWakeRenderer(rendererWake);
    return TRUE;
}

/*
 * P11-C2: the root copy asked of the renderer's GPU settled, before the X server touches its buffers. Waited for
 * without the shared lock, which the renderer takes the request under; copied here on the CPU instead when the
 * renderer could not (`failed`) or when, not taken yet, the request is taken back (`cancelled`) because no renderer
 * will take it soon: this thread holds the shared lock (an EXA access open), no renderer can draw, one waits for
 * the ownership answer this thread gives, or the caller says so (`takeBack`). A request taken is waited for while its
 * renderer exists (as
 * lorieGpuWaitIdle); one whose buffers went (abandoned) is not copied. Should the CPU copy fail too, the swap is
 * undone - the X server draws into the buffer that has everything again - and the twin brought up to date whole.
 */
static void lorieRootCopyWait(Bool takeBack) {
    struct lorie_shared_server_state *st = pvfb->state;
    uint32_t seq = pvfb->rootCopy.seq, done = 0;
    Bool held = pvfb->accessLocks > 0, copy = FALSE, waited = FALSE;
    uint64_t startUs, blockedUs;

    if (!seq)
        return;
    struct lorieXMark waitMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_ROOT_COPY_WAIT);  // P11-C3-C
    startUs = GetTimeInMicros();
    while ((done = __atomic_load_n(&st->rootCopy.done, __ATOMIC_ACQUIRE)) != seq) {
        struct timespec slice = { 0, 33 * 1000000 };
        waited = TRUE;
        __atomic_store_n(&st->rootCopy.waiting, seq, __ATOMIC_RELEASE);    // P11-C3-B2: a renderer may finish it first
        if (__atomic_load_n(&st->rootCopy.taken, __ATOMIC_ACQUIRE) != seq) {
            if (takeBack || held || !lorieConnectionAlive() || !lorieRendererAvailable() || lorieOwnershipAsked()) {
                if (!held)
                    lorie_mutex_lock(&st->lock, &st->lockingPid);
                if (st->rootCopy.taken != seq) {
                    st->rootCopy.cancelled = seq;
                    copy = TRUE;
                }
                if (!held)
                    lorie_mutex_unlock(&st->lock, &st->lockingPid);
                if (copy)
                    break;
                continue;   // taken meanwhile: waited for
            }
        } else if (st->rootCopy.pid > 0 && kill(st->rootCopy.pid, 0) == -1 && errno == ESRCH) {
            copy = TRUE;    // taken by a renderer process gone: its device work ended with it (lorieGpuSubmitterGone)
            break;
        }
        syscall(SYS_futex, &st->rootCopy.done, FUTEX_WAIT, done, &slice, NULL, 0);
    }
    if (waited)
        __atomic_store_n(&st->rootCopy.waiting, 0, __ATOMIC_RELAXED);
    if (done == seq && __atomic_load_n(&st->rootCopy.failed, __ATOMIC_RELAXED) == seq)
        copy = TRUE;
    blockedUs = GetTimeInMicros() - startUs;
    __atomic_fetch_add(&st->diag.rootCopyCpuBlockUs, blockedUs, __ATOMIC_RELAXED);
    // P11-C3: a wait the X server had to make; and, the renderer having settled the copy meanwhile, from that to here
    if (waited && pvfb->frameDiag) {
        uint64_t now = lorieMonotonicUs(), doneUs = __atomic_load_n(&st->rootCopy.doneUs, __ATOMIC_RELAXED);
        lorieHistAdd(&st->diag.rootBlock, blockedUs);
        if (done == seq && doneUs && doneUs <= now)
            lorieHistAdd(&st->diag.rootResume, now - doneUs);
    }
    if (copy && !pvfb->rootCopy.abandoned) {
        uint64_t waitStartUs = GetTimeInMicros();
        LoriePixmapPriv *priv = lorieRootWindowPixmapPriv();
        if (!held)
            lorie_mutex_lock(&st->lock, &st->lockingPid);
        if (!lorieRootCopyCpu(pvfb->rootCopy.from, pvfb->rootCopy.to, pvfb->rootCopy.boxes, pvfb->rootCopy.n, waitStartUs) &&
            priv && priv->buffer == pvfb->rootCopy.to && priv->twin == pvfb->rootCopy.from) {
            priv->buffer = pvfb->rootCopy.from;
            priv->twin = pvfb->rootCopy.to;
            pvfb->rootTwinStale = TRUE;
        }
        if (!held)
            lorie_mutex_unlock(&st->lock, &st->lockingPid);
        __atomic_fetch_add(&st->diag.rootCpuFallbacks, 1, __ATOMIC_RELAXED);
    }
    pvfb->rootCopy.seq = 0;
}

/*
 * The swap, between requests, under the shared lock (the renderer reads which buffer is the front under it): the
 * damage copied from the back into the front - by the CPU here (lorieRootCopyCpu), or, P11-C2, by the renderer's
 * GPU after the swap, the X server waiting for it only once it touches its new back (lorieRootCopyWait) - and the
 * two swapped. False, and nothing swapped, if a buffer cannot be CPU-locked.
 */
static Bool lorieRootSwap(LoriePixmapPriv *priv) {
    LorieBuffer *back, *front;
    const LorieBuffer_Desc *d;
    RegionPtr damage = DamageRegion(pvfb->damage);
    BoxRec full;
    BoxPtr box;
    int n;
    uint64_t waitStartUs;
    Bool swapped;

    if (priv->locked)
        return FALSE;   // never between requests under PER_ACCESS
    lorieRootCopyWait(FALSE);   // P11-C2: the last copy asked of the GPU settled first (and the swap undone, should it have failed)
    back = priv->buffer, front = priv->twin, d = LorieBuffer_description(back);
    full = (BoxRec) { 0, 0, (short) d->width, (short) d->height };
    box = pvfb->rootTwinStale ? &full : RegionRects(damage);
    n = pvfb->rootTwinStale ? 1 : RegionNumRects(damage);
    waitStartUs = GetTimeInMicros();
    lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
    swapped = lorieRootGpuCopyAsk(back, front, box, n) || lorieRootCopyCpu(back, front, box, n, waitStartUs);
    if (swapped) {
        priv->buffer = front;
        priv->twin = back;
        pvfb->rootTwinStale = FALSE;
        pvfb->state->rootWindowTextureID = LorieBuffer_description(back)->id;
    }
    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    return swapped;
}

// P10-B0: one event's two CLOCK_MONOTONIC stamps as a latency; out of order or over 10 s apart, counted instead.
static void lorieStampPair(volatile struct lorie_latency *l, uint64_t fromUs, uint64_t toUs) {
    if (toUs < fromUs || toUs - fromUs > 10000000)
        __atomic_fetch_add(&pvfb->state->diag.xStampsInvalid, 1, __ATOMIC_RELAXED);
    else
        lorieLatencyAdd(l, toUs - fromUs);
}

// P10-B0: a work procedure posted at `fromUs` (its low 32 bits: what a closure carries on any ABI) running now on the
// X server's thread - its wait, and how many posted after it still wait (the most since the last report).
static void lorieRanAfter(volatile struct lorie_latency *l, uint32_t fromUs, volatile uint32_t *posted, volatile uint32_t *run,
                          volatile uint32_t *waitingMax) {
    uint32_t waited = (uint32_t) lorieMonotonicUs() - fromUs, done = __atomic_add_fetch(run, 1, __ATOMIC_RELAXED);
    int32_t behind = (int32_t) (__atomic_load_n(posted, __ATOMIC_RELAXED) - done);
    uint32_t max = __atomic_load_n(waitingMax, __ATOMIC_RELAXED);
    if (waited > 10000000)
        __atomic_fetch_add(&pvfb->state->diag.xStampsInvalid, 1, __ATOMIC_RELAXED);
    else
        lorieLatencyAdd(l, waited);
    while (behind > 0 && (uint32_t) behind > max &&
           !__atomic_compare_exchange_n(waitingMax, &max, (uint32_t) behind, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

// P11-C3: the frames the renderer presented so far, Vulkan's and GLES's (never taken back to 0, as renderedFrames is).
static uint32_t lorieFramesShown(void) {
    return __atomic_load_n(&pvfb->state->diag.vkFramesPresented, __ATOMIC_RELAXED) +
           __atomic_load_n(&pvfb->state->diag.eglSwapBuffers, __ATOMIC_RELAXED);
}

/*
 * P11-C3 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1), a tick run at `tickUs`: its wait from its Choreographer callback (posted
 * at `postedUs`, low 32 bits, when `posted`), and whether the frame the last tick asked for was presented by now.
 */
static void lorieTickTimed(uint64_t tickUs, uint32_t postedUs, Bool posted) {
    uint32_t waited = (uint32_t) tickUs - postedUs;
    if (posted && waited <= 10000000)
        lorieHistAdd(&pvfb->state->diag.xTickDelay, waited);
    if (pvfb->frameAsked && lorieFramesShown() == pvfb->framesAtAsk)
        __atomic_fetch_add(&pvfb->state->diag.xTicksFrameMissed, 1, __ATOMIC_RELAXED);
    pvfb->frameAsked = FALSE;
}

// P11-C3: that tick asking the renderer for a frame - when (for the renderer's wake), the frames presented by then, its work.
static void lorieTickAsked(uint64_t tickUs) {
    uint64_t now = lorieMonotonicUs();
    volatile struct lorie_progress *p = &pvfb->state->progress;
    uint32_t seq = p->askSeq + 1;
    pvfb->frameAsked = TRUE;
    pvfb->framesAtAsk = lorieFramesShown();
    __atomic_store_n(&pvfb->state->frameWantedUs, now, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pvfb->state->diag.xTicksFrameWanted, 1, __ATOMIC_RELAXED);
    lorieHistAdd(&pvfb->state->diag.xTickWork, now - tickUs);
    // P11-C3-C: this ask, numbered, and when (lorie.h struct lorie_progress): the frame that takes it up says so
    p->askUs[seq % LORIE_ASK_RING] = now;
    __atomic_store_n(&p->askSeq, seq, __ATOMIC_RELEASE);
}

static Bool lorieRedraw(__unused ClientPtr pClient, void *closure) {
    int status, nonEmpty;
    LoriePixmapPriv* priv;
    PixmapPtr root = pScreenPtr && pScreenPtr->root ? pScreenPtr->GetWindowPixmap(pScreenPtr->root) : NULL;
    Bool twinned, rootDouble;
    uint64_t tickUs = pvfb->frameDiag || pvfb->freezeDiag ? lorieMonotonicUs() : 0;
    // P11-C3-C: in a tick, for the freeze watchdog, until it is over
    struct lorieXMark tickMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_TICK);

    // P10-B0: from its Choreographer callback to here, and the ticks still waiting behind it.
    if (closure)
        lorieRanAfter(&pvfb->state->diag.xCallbackToTick, (uint32_t) (uintptr_t) closure, &pvfb->state->diag.xTicksPosted,
                      &pvfb->state->diag.xTicksRun, &pvfb->state->diag.xTicksWaitingMax);
    if (tickUs)
        lorieTickTimed(tickUs, (uint32_t) (uintptr_t) closure, closure != NULL);

    lorieProcessOwnershipRequest();
    twinned = lorieRootTwinned();

    pvfb->current_msc++;
    // Vulkan (PER_ACCESS): the frame this tick allows is allowed before the vblanks run, so that the Present copies
    // they queue go into that frame's submission instead of one of their own ahead of it. GLES keeps 3c98ffa's order,
    // and so does root double buffering: its swap, after the vblanks, comes before the frame.
    if (lorieAhbPerAccess() && !twinned)
        pvfb->state->waitForNextFrame = false;
    loriePerformVblanks();
    // P10-S1: Present GPU copies whose poll could not be queued, a stalled renderer's and the retired ones are
    // settled at every tick too, waiting for nothing (patches/xserver.patch lorieRecheckGpuCopies).
    lorieRecheckGpuCopies();

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
        // P11-C3-D0: what the screen pixmap shows from now on - swapped to the front (root double buffering), or drawn
        lorieContentPublish(priv);
    }

    if (pvfb->state->drawRequested || pvfb->state->cursor.moved || pvfb->state->cursor.updated) {
        pvfb->state->rootWindowTextureID = LorieBuffer_description(rootDouble ? priv->twin : priv->buffer)->id;

        // Sending signal about pending root window changes to renderer thread.
        // We do not explicitly lock the pvfb->state->lock here because we do not want to wait
        // for all drawing operations to be finished.
        // P11-C3-C: published first, then the wake - the renderer cannot sleep through it (lorieWakeRenderer).
        if (tickUs)
            lorieTickAsked(tickUs);
        lorieWakeRenderer(rendererWake);
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

/*
 * P11-C3: one timing's samples since the last report (struct lorie_hist) as n/avg/p95/p99/max/h16/h33, microseconds:
 * how many, their mean, p95 and p99 (the bound of the bucket each falls in, ">100000" past the last), the maximum
 * (taken back to 0 here), and how many were over 16.667 ms and over 33.333 ms. "0" when there were none.
 */
static void lorieHistText(char *out, size_t size, const struct lorie_hist *now, const struct lorie_hist *before,
                          volatile uint32_t *max) {
    static const uint32_t bounds[LORIE_HIST_BUCKETS - 1] = { LORIE_HIST_BOUNDS };
    static const uint32_t percent[2] = { 95, 99 };
    uint32_t n = now->count - before->count, top = __atomic_exchange_n(max, 0, __ATOMIC_RELAXED), over16 = 0, over33 = 0;
    char p[2][16];
    int i, b;

    if (!n) {
        snprintf(out, size, "0");
        return;
    }
    for (b = LORIE_HIST_OVER_16MS; b < LORIE_HIST_BUCKETS; b++)
        over16 += now->buckets[b] - before->buckets[b];
    for (b = LORIE_HIST_OVER_33MS; b < LORIE_HIST_BUCKETS; b++)
        over33 += now->buckets[b] - before->buckets[b];
    for (i = 0; i < 2; i++) {
        uint32_t want = (uint32_t) (((uint64_t) n * percent[i] + 99) / 100), seen = 0;
        for (b = 0; b < LORIE_HIST_BUCKETS - 1; b++)
            if ((seen += now->buckets[b] - before->buckets[b]) >= want)
                break;
        if (b < LORIE_HIST_BUCKETS - 1)
            snprintf(p[i], sizeof(p[i]), "%u", bounds[b]);
        else
            snprintf(p[i], sizeof(p[i]), ">%u", bounds[LORIE_HIST_BUCKETS - 2]);
    }
    snprintf(out, size, "%u/%llu/%s/%s/%u/%u/%u", n, (unsigned long long) ((now->sumUs - before->sumUs) / n), p[0], p[1],
             top, over16, over33);
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
              "vk_transfer_copy=%s x_yield_on_wake=%s root_copy_opt=%s vk_submit_opt=%s vk_frame_diag=%s exa_lock_opt=%s "
              "vk_schedule_opt=%s freeze_diag=%s",
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
        pvfb->state->backend.vkTransferCopy ? "on" : "off", __atomic_load_n(&pvfb->yieldOnWake, __ATOMIC_RELAXED) ? "on" : "off",
        !pvfb->rootCopyOpt ? "off" : pvfb->rootCopyOptFailed ? "fallback" : "on", pvfb->state->backend.vkSubmitOpt ? "on" : "off",
        pvfb->frameDiag ? "on" : "off", pvfb->exaLockOpt ? "on" : "off", pvfb->state->backend.vkScheduleOpt ? "on" : "off",
        pvfb->freezeDiag ? "on" : "off");
    // P11-C3-B: what the scheduling options did - B1's reservations instead of the shared lock, and the copies the
    // renderer left queued for one
    log(INFO, "XlorieSchedule: exa_lock_opt=%s x_access_reserved=%llu(+%llu) x_access_reserve_full=%llu(+%llu) "
              "copies_deferred=%llu(+%llu) copies_deferred_wakes=%llu(+%llu) vk_schedule_opt=%s root_ahead=%llu(+%llu) "
              "root_ahead_x_waiting=%llu(+%llu) root_chain_consumed=%llu(+%llu) root_ahead_unavailable=%llu(+%llu) "
              "buffers_awaited=%llu(+%llu) buffer_await_us=%llu(+%llu) buffers_given_up=%llu(+%llu) "
              "copies_left_for_buffers=%llu(+%llu)",
        pvfb->exaLockOpt ? "on" : "off", TD(xAccessReserved), TD(xAccessReserveFull), TD(copiesDeferred), TD(copiesDeferredWakes),
        pvfb->state->backend.vkScheduleOpt ? "on" : "off", TD(rootAhead), TD(rootAheadXWaiting), TD(rootChainConsumed),
        TD(rootAheadUnavailable), TD(buffersAwaited), TD(bufferAwaitUs), TD(buffersGivenUp), TD(copiesLeftForBuffers));
    // P11-C3-D2: the root copies submitted ahead settled before their frame's submission, and why
    log(INFO, "XlorieRootCopySched: root_copy_sched_opt=%s vk_schedule_opt=%s settled_acquired=%llu(+%llu) "
              "settled_lock_wait=%llu(+%llu) settled_x_waiting=%llu(+%llu)",
        pvfb->state->backend.rootCopySchedOpt ? "on" : "off", pvfb->state->backend.vkScheduleOpt ? "on" : "off",
        TD(rootEarlyAcquired), TD(rootEarlyLock), TD(rootEarlyXWaiting));
    // P11-C3-D1-A: the Present GPU copies' damage put off until the copy was done, and how each went; those still running
    log(INFO, "XlorieCopyDamage: exa_composite_opt=%s deferred=%llu(+%llu) done=%llu(+%llu) early=%llu(+%llu) dropped=%llu(+%llu) "
              "pending=%u",
        pvfb->exaCompositeOpt ? "on" : "off", TD(copyDamageDeferred), TD(copyDamageDone), TD(copyDamageEarly), TD(copyDamageDropped),
        d.copyDamageDeferred - d.copyDamageDone - d.copyDamageEarly - d.copyDamageDropped);
    // P11-C2: the root's copies by the renderer's GPU; "fallback" when, this period, only the CPU settled them
    log(INFO, "XlorieRootCopy: root_gpu_copy=%s root_gpu_copy_requests=%llu(+%llu) root_gpu_copy_submits=%llu(+%llu) "
              "root_gpu_copy_completions=%llu(+%llu) root_gpu_copy_rects=%llu(+%llu) root_gpu_copy_bytes=%llu(+%llu) "
              "root_copy_completion_us=%llu(+%llu) root_cpu_fallbacks=%llu(+%llu) root_cpu_small=%llu(+%llu) "
              "root_copy_cpu_block_us=%llu(+%llu) root_copy_pending=%d",
        !pvfb->rootGpuCopy ? "off" : d.rootCpuFallbacks != prev.rootCpuFallbacks && d.rootGpuCopyCompletions == prev.rootGpuCopyCompletions
                                     ? "fallback" : "on",
        TD(rootGpuCopyRequests), TD(rootGpuCopySubmits), TD(rootGpuCopyCompletions), TD(rootGpuCopyRects), TD(rootGpuCopyBytes),
        TD(rootGpuCopyCompletionUs), TD(rootCpuFallbacks), TD(rootCpuSmall), TD(rootCopyCpuBlockUs), pvfb->rootCopy.seq ? 1 : 0);
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
              "submit_cpu_us=%llu(+%llu) fence_wait_us=%llu(+%llu) present_call_us=%llu(+%llu) copies_ahead=%llu(+%llu) "
              "early_presents=%llu(+%llu) selftest=%s device=\"%s\" driver_version=0x%x api_version=0x%x present_mode=\"%s\"",
        TD(vkAcquireSuccess), TD(vkAcquireSuboptimal), TD(vkAcquireTimeout), TD(vkAcquireNotReady),
        TD(vkAcquireOutOfDate), TD(vkAcquireWaitUs), TD(vkSubmitCpuUs), TD(vkFenceWaitUs), TD(vkPresentCallUs),
        TD(vkCopiesAhead), TD(vkEarlyPresents),
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
    // P10-B0: the X server's ticks and GPU copy completions, from their source to its thread (struct lorie_renderer_diag
    // x*): waiting = posted but not yet run when the report is made.
    {
        char vsync[112], tick[112], notify[112], recheck[112];
        volatile struct lorie_renderer_diag *live = &pvfb->state->diag;
        lorieLatencyText(vsync, sizeof(vsync), &d.xVsyncToCallback, &prev.xVsyncToCallback, &live->xVsyncToCallback.maxUs, 0);
        lorieLatencyText(tick, sizeof(tick), &d.xCallbackToTick, &prev.xCallbackToTick, &live->xCallbackToTick.maxUs, 0);
        lorieLatencyText(notify, sizeof(notify), &d.xCopyNotify, &prev.xCopyNotify, &live->xCopyNotify.maxUs, 0);
        lorieLatencyText(recheck, sizeof(recheck), &d.xCopyRecheck, &prev.xCopyRecheck, &live->xCopyRecheck.maxUs, 0);
        log(INFO, "XlorieTick: vsync_to_callback_us=%s callback_to_tick_us=%s ticks=%llu(+%llu) ticks_waiting=%d "
                  "ticks_waiting_max=%u copy_notify_us=%s copy_recheck_us=%s copy_done_events=%llu(+%llu) "
                  "copy_done_waiting=%d copy_done_waiting_max=%u stamps_invalid=%llu(+%llu) copy_early=scrap:%llu(+%llu),"
                  "destroy:%llu(+%llu),first_poll:%llu(+%llu),later_poll:%llu(+%llu),stalled:%llu(+%llu) "
                  "copy_retired=%llu(+%llu) copy_retired_live=%d copy_cancelled=%llu(+%llu)",
            vsync, tick, TD(xTicksPosted), (int) (int32_t) (d.xTicksPosted - d.xTicksRun),
            __atomic_exchange_n(&live->xTicksWaitingMax, 0, __ATOMIC_RELAXED), notify, recheck, TD(xCopyDonePosted),
            (int) (int32_t) (d.xCopyDonePosted - d.xCopyDoneRun), __atomic_exchange_n(&live->xCopyDoneWaitingMax, 0, __ATOMIC_RELAXED),
            TD(xStampsInvalid), TD(xCopyEarly[LORIE_X_EARLY_SCRAP]), TD(xCopyEarly[LORIE_X_EARLY_DESTROY]),
            TD(xCopyEarly[LORIE_X_EARLY_POLL_FIRST]), TD(xCopyEarly[LORIE_X_EARLY_POLL_LATER]), TD(xCopyEarly[LORIE_X_EARLY_STALLED]),
            TD(xCopyRetired), (int) (int32_t) (d.xCopyRetired - d.xCopyRetiredReleased), TD(xCopyCancelled));
    }
    // P11-C3, only while TERMUX_X11_RENDERER_VK_FRAME_DIAG=1: the Vulkan frames' pacing and parts, the root copies' parts,
    // and the X server's requests, accesses and ticks (lorie.h struct lorie_hist, lorieHistText). The worst_* fields are
    // the period's largest interval or slowest item with what it spent - read while the renderer may write them.
    if (pvfb->frameDiag) {
        char h[10][112];
        volatile struct lorie_renderer_diag *live = &pvfb->state->diag;
#define HT(i, f) lorieHistText(h[i], sizeof(h[i]), &d.f, &prev.f, &live->f.maxUs)
#define TD32(f) d.f, d.f - prev.f
        uint32_t worst = __atomic_exchange_n(&live->vkWorstStartUs, 0, __ATOMIC_RELAXED);
        HT(0, vkFrameStart), HT(1, vkPresentDelta), HT(2, vkFrameWake);
        log(INFO, "XlorieVkFramePacing: frame_start_us=%s present_delta_us=%s wake_us=%s idle_gaps=%u(+%u) "
                  "stamps_invalid=%u(+%u) ticks_frame_wanted=%u(+%u) ticks_frame_missed=%u(+%u) worst_start_us=%u "
                  "worst_wake_us=%u worst_acquire_us=%u worst_lock_us=%u worst_fence_us=%u worst_present_us=%u "
                  "worst_after_root_us=%u worst_prev_cpu_us=%u worst_prev_lock_us=%u worst_prev_fence_us=%u "
                  "worst_prev_after_root_us=%u worst_gap_us=%u worst_unasked_us=%u",
            h[0], h[1], h[2], TD32(vkFrameIdleGaps), TD32(vkFrameStampsInvalid), TD32(xTicksFrameWanted),
            TD32(xTicksFrameMissed), worst, d.vkWorstWakeUs, d.vkWorstAcquireUs, d.vkWorstLockUs, d.vkWorstFenceUs,
            d.vkWorstPresentUs, d.vkWorstAfterRootUs, d.vkWorstPrevCpuUs, d.vkWorstPrevLockUs, d.vkWorstPrevFenceUs,
            d.vkWorstPrevAfterRootUs, d.vkWorstGapUs, d.vkWorstUnaskedUs);
        // P11-C3-C: the frame gaps of 100 ms or more, by what waited in them (renderer.c rendererVkFrameGap)
        worst = __atomic_exchange_n(&live->vkGapWorstStallUs, 0, __ATOMIC_RELAXED);
        HT(0, vkAskWait);
        log(INFO, "XlorieFrameGaps: ask_wait_us=%s gaps_idle=%u(+%u) gaps_x_stalled=%u(+%u) gaps_unwoken=%u(+%u) "
                  "gaps_late=%u(+%u) stall_ge_ms=100:%u(+%u),250:%u(+%u),500:%u(+%u),1000:%u(+%u),2000:%u(+%u),3000:%u(+%u) "
                  "idle_ge_ms=100:%u(+%u),250:%u(+%u),500:%u(+%u),1000:%u(+%u),2000:%u(+%u),3000:%u(+%u) worst_stall_us=%u "
                  "worst_stall_cause=%s worst_stall_ask_wait_us=%u worst_stall_wake_us=%u worst_stall_gap_us=%u "
                  "worst_stall_ticks=%u",
            h[0], TD32(vkGapIdle), TD32(vkGapXStalled), TD32(vkGapUnwoken), TD32(vkGapLate), TD32(vkGapStallGe[0]),
            TD32(vkGapStallGe[1]), TD32(vkGapStallGe[2]), TD32(vkGapStallGe[3]), TD32(vkGapStallGe[4]), TD32(vkGapStallGe[5]),
            TD32(vkGapIdleGe[0]), TD32(vkGapIdleGe[1]), TD32(vkGapIdleGe[2]), TD32(vkGapIdleGe[3]), TD32(vkGapIdleGe[4]),
            TD32(vkGapIdleGe[5]), worst,
            !worst ? "none" : d.vkGapWorstCause == 1 ? "x_stalled" : d.vkGapWorstCause == 2 ? "unwoken" : "late",
            worst ? d.vkGapWorstAskWaitUs : 0, worst ? d.vkGapWorstWakeUs : 0, worst ? d.vkGapWorstGapUs : 0,
            worst ? d.vkGapWorstTicks : 0);
        HT(0, vkFrameCpu), HT(1, vkFrameAcquire), HT(2, vkFrameLock), HT(3, vkFrameRecord), HT(4, vkFrameSubmit);
        HT(5, vkFrameFence), HT(6, vkFenceRoot), HT(7, vkFenceCopy), HT(8, vkFramePresent), HT(9, vkFrameAfterRoot);
        log(INFO, "XlorieVkPipeline: cpu_us=%s acquire_us=%s lock_us=%s record_us=%s submit_us=%s fence_frame_us=%s "
                  "fence_root_us=%s fence_copy_us=%s present_us=%s after_root_us=%s",
            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9]);
        worst = __atomic_exchange_n(&live->rootWorstUs, 0, __ATOMIC_RELAXED);
        HT(0, rootTake), HT(1, rootRecord), HT(2, rootResume), HT(3, rootBlock), HT(4, rootAheadSettle);
        log(INFO, "XlorieRootCopyTiming: take_us=%s record_us=%s resume_us=%s x_block_us=%s worst_total_us=%u "
                  "worst_take_us=%u worst_record_us=%u worst_fence_us=%u ahead_settle_us=%s",
            h[0], h[1], h[2], h[3], worst, d.rootWorstTakeUs, d.rootWorstRecordUs, d.rootWorstFenceUs, h[4]);
        HT(0, xRequest), HT(1, xAccess), HT(2, xAccessLock), HT(3, xAccessGpu), HT(4, xTickDelay), HT(5, xTickWork);
        HT(6, xAccessHold);
        log(INFO, "XlorieXLatency: request_us=%s access_us=%s access_lock_us=%s access_gpu_us=%s tick_delay_us=%s "
                  "tick_work_us=%s worst_request_us=%u worst_request=%d:%s:%s:%d.%d access_hold_us=%s worst_access_us=%u "
                  "worst_access=%dx%d:%s:%s",
            h[0], h[1], h[2], h[3], h[4], h[5], pvfb->worstRequest.us, pvfb->worstRequest.us ? pvfb->worstRequest.client : -1,
            pvfb->worstRequest.us ? pvfb->worstRequest.cmd : "none", pvfb->worstRequest.us ? pvfb->worstRequest.ext : "none",
            pvfb->worstRequest.us ? pvfb->worstRequest.major : 0, pvfb->worstRequest.us ? pvfb->worstRequest.minor : 0, h[6],
            pvfb->worstAccess.us, pvfb->worstAccess.us ? pvfb->worstAccess.width : 0, pvfb->worstAccess.us ? pvfb->worstAccess.height : 0,
            !pvfb->worstAccess.us ? "none" : pvfb->worstAccess.index == EXA_PREPARE_DEST ? "dest"
                : pvfb->worstAccess.index == EXA_PREPARE_SRC ? "src" : pvfb->worstAccess.index == EXA_PREPARE_MASK ? "mask" : "other",
            !pvfb->worstAccess.us ? "none" : pvfb->worstAccess.reserved ? "reserved" : "locked");
        pvfb->worstRequest.us = 0;
        pvfb->worstAccess.us = 0;
        // P11-C3-D1: the RENDER composites - their time apart from their waits, by class (lorieCompositeTimed)
        {
            char composites[512], classes[1024];
            lorieCompositeReport(composites, sizeof(composites), classes, sizeof(classes));
            log(INFO, "XlorieComposite: %s", composites);
            log(INFO, "XlorieCompositeClasses: %s", classes);
        }
#undef TD32
#undef HT
    }
    // P11-C3-D0, only while TERMUX_X11_CONTENT_FRAME_DIAG=1: client content from its presentation to the screen
    // (lorieContent*, renderer.c rendererContentFrame); the clients presenting most this period, each with its share
    if (pvfb->contentDiag) {
        char h[6][112], clients[400];
        volatile struct lorie_renderer_diag *live = &pvfb->state->diag;
        uint32_t worst = __atomic_exchange_n(&live->contentWorstUs, 0, __ATOMIC_RELAXED);
        uint32_t maxGap = __atomic_exchange_n(&live->contentMaxGapUs, 0, __ATOMIC_RELAXED);
        uint32_t stages[4] = { d.contentWorstCopyUs, d.contentWorstCompositeUs, d.contentWorstPublishUs, d.contentWorstScreenUs };
        static const char *const stageNames[4] = { "copy", "composite", "publish", "screen" };
        int k, slowest = 0, shown = 0;
        size_t at = 0;
#define HT(i, f) lorieHistText(h[i], sizeof(h[i]), &d.f, &prev.f, &live->f.maxUs)
#define TD32(f) d.f, d.f - prev.f
        for (k = 1; k < 4; k++)
            if (stages[k] > stages[slowest])
                slowest = k;
        log(INFO, "XlorieContentFrame: requests=%u(+%u) skipped=%u(+%u) gpu_copies=%u(+%u) cpu_copies=%u(+%u) "
                  "copies_done=%u(+%u) completes=%u(+%u) completes_skip=%u(+%u) stale_reads=%u(+%u) pending_reads=%u(+%u) "
                  "unknown_reads=%u(+%u) "
                  "compositor_updates=%u(+%u) root_updates=%u(+%u) root_publishes=%u(+%u) screen_new=%u(+%u) "
                  "screen_reused=%u(+%u)",
            TD32(contentRequests), TD32(contentSkipped), TD32(contentGpuCopies), TD32(contentCpuCopies),
            TD32(contentCopiesDone), TD32(contentCompletes), TD32(contentCompletesSkip), TD32(contentStaleReads),
            TD32(contentPendingReads), TD32(contentUnknownReads),
            TD32(contentCompositorUpdates), TD32(contentRootUpdates), TD32(contentRootPublishes), TD32(contentScreenNew),
            TD32(contentScreenReused));
        HT(0, contentPresentToCopy), HT(1, contentCopyToComplete), HT(2, contentCompleteToRoot), HT(3, contentRootToPublish);
        HT(4, contentPublishToScreen), HT(5, contentTotal);
        log(INFO, "XlorieContentLatency: present_to_copy_us=%s copy_to_complete_us=%s complete_to_root_us=%s "
                  "root_to_publish_us=%s publish_to_screen_us=%s content_total_us=%s",
            h[0], h[1], h[2], h[3], h[4], h[5]);
        HT(0, contentInterval);
        log(INFO, "XlorieContentStall: content_interval_us=%s gaps_pending=%u(+%u) gaps_client=%u(+%u) max_gap_us=%u "
                  "worst_content_us=%u worst_stage=%s worst_copy_us=%u worst_composite_us=%u worst_publish_us=%u "
                  "worst_screen_us=%u",
            h[0], TD32(contentGapsPending), TD32(contentGapsClient), maxGap, worst, worst ? stageNames[slowest] : "none",
            worst ? stages[0] : 0, worst ? stages[1] : 0, worst ? stages[2] : 0, worst ? stages[3] : 0);
        clients[0] = 0;
        for (shown = 0; shown < 3; shown++) {
            int best = -1;
            for (k = 0; k < LORIE_CONTENT_CLIENTS; k++) {
                uint32_t n = pvfb->contentClients[k].requests - pvfb->contentClients[k].requestsAt;
                if (pvfb->contentClients[k].client && n && pvfb->contentClients[k].requestsAt != UINT32_MAX &&
                    (best < 0 || n > pvfb->contentClients[best].requests - pvfb->contentClients[best].requestsAt))
                    best = k;
            }
            if (best < 0)
                break;
            at += (size_t) snprintf(clients + at, sizeof(clients) - at, "%s%s/%d:requests=%u(+%u),skipped=%u(+%u),max_gap_us=%u",
                                    at ? " " : "", pvfb->contentClients[best].cmd, pvfb->contentClients[best].index,
                                    pvfb->contentClients[best].requests,
                                    pvfb->contentClients[best].requests - pvfb->contentClients[best].requestsAt,
                                    pvfb->contentClients[best].skipped,
                                    pvfb->contentClients[best].skipped - pvfb->contentClients[best].skippedAt,
                                    pvfb->contentClients[best].maxGapUs);
            if (at >= sizeof(clients))
                at = sizeof(clients) - 1;
            pvfb->contentClients[best].requestsAt = UINT32_MAX;     // shown: not again in this line
        }
        for (k = 0; k < LORIE_CONTENT_CLIENTS; k++) {
            pvfb->contentClients[k].requestsAt = pvfb->contentClients[k].requests;
            pvfb->contentClients[k].skippedAt = pvfb->contentClients[k].skipped;
            pvfb->contentClients[k].maxGapUs = 0;
        }
        log(INFO, "XlorieContentClients: %s", clients[0] ? clients : "none");
#undef TD32
#undef HT
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

/*
 * The 5 second report. P11-C3-C: work on this thread (lorieReportWorkProc), not the timer's own: DoTimers runs timers
 * holding the input lock, which the input thread takes for every event and GPU copy completion it reads - those
 * waited for the whole report (some 15 lines with the frame diagnostics). Its own time and stage, for the watchdog.
 */
static void lorieReport(void) {
    uint32_t samples;
    static Bool driverLogged = FALSE;
    uint64_t reportStartUs = pvfb->freezeDiag ? lorieMonotonicUs() : 0;
    struct lorieXMark reportMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_REPORT);

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
    if (reportStartUs) {
        uint64_t us = lorieMonotonicUs() - reportStartUs;
        __atomic_store_n(&pvfb->reportUs, us, __ATOMIC_RELAXED);
        if (us > __atomic_load_n(&pvfb->reportMaxUs, __ATOMIC_RELAXED))
            __atomic_store_n(&pvfb->reportMaxUs, us, __ATOMIC_RELAXED);
    }
}

static Bool lorieReportWorkProc(__unused ClientPtr client, __unused void *closure) {
    lorieReport();
    return TRUE;
}

// Every 5 seconds: the report queued as work - run as WaitForSomething goes round again (check_timers, having run the
// timers, gives ospoll_wait no wait), outside the input lock.
static CARD32 lorieFramecounter(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {
    QueueWorkProc(lorieReportWorkProc, NULL, NULL);
    return 5000;
}

/* ---- P11-C3-C, TERMUX_X11_FRAME_FREEZE_DIAG=1: the freeze watchdog ---- */

#define LORIE_FREEZE_PERIOD_US 100000       /* the watchdog looks every 100 ms */
#define LORIE_FREEZE_BEGIN_US 500000        /* work waiting, none of it moving, this long: a freeze */

static const char *const lorieXStageNames[LORIE_X_STAGES] = { "loop", "tick", "request", "root_copy_wait", "report" };
static const char *const lorieRStageNames[LORIE_R_STAGES] = {
    "sleep", "pass", "buffer_wait", "root_copy", "frame", "acquire", "lock_wait", "fence_wait", "present", "copies", "gles",
    "backend", "device_idle" };

// What the watchdog saw at nowUs - each part read without a lock, as it was then.
struct lorie_freeze_sample {
    uint64_t nowUs;
    Bool connected, surface;
    // what moves: frames presented, the last GPU copy completed, root copies settled, ticks run, requests run
    uint32_t framesShown, ticksPosted, ticksRun, requests;
    uint64_t copiesDone;
    uint32_t rootSeq, rootTaken, rootDone, rootCancelled, rootWaiting;
    uint64_t rootQueuedUs;
    // the renderer's work waiting: frames asked for and not presented (since the first such ask), GPU copies queued
    // (since the oldest)
    uint32_t askSeq, takenSeq;
    uint64_t askUs;
    uint32_t copiesQueued;
    uint64_t copyQueuedUs;
    // where each side is
    uint32_t xStage, rStage;
    uint64_t xStageUs, rStageUs;
    int requestClient, requestMajor, requestMinor;
    uint32_t gpuBusy;
    int lockPid;
    uint64_t notifiedSerial, ackedSerial;
};

// The watchdog's own: the last sample, when each kind of work last moved, and the freeze it is in, if any.
struct lorie_freeze {
    struct lorie_freeze_sample last;
    Bool started, on;
    uint64_t framesUs, copiesUs, rootUs, ticksUs;
    uint32_t id, levels, level, pending, beginPending, moved;
    uint64_t beginUs, stallUs, xStallUs, rStallUs;
    char side;
    uint32_t beginXStage, beginRStage;
};

enum { LORIE_FREEZE_NONE, LORIE_FREEZE_BEGIN, LORIE_FREEZE_LEVEL, LORIE_FREEZE_END };
// What waits (pending), what moved (ended_by)
#define LORIE_FREEZE_FRAME 1u       /* a frame asked for, not presented */
#define LORIE_FREEZE_COPY 2u        /* GPU copies queued, none completed */
#define LORIE_FREEZE_ROOT 4u        /* a root copy asked for, not settled */
#define LORIE_FREEZE_RSTAGE 8u      /* the renderer in one stage - not asleep, not between steps */
#define LORIE_FREEZE_XSTAGE 16u     /* the X server's thread in one stage - a tick, a request, a wait, the report */
#define LORIE_FREEZE_TICKS 32u      /* ticks posted to the X server's thread, none run */
#define LORIE_FREEZE_KINDS 6

// How long since the later of `a` and `b` (0: neither, or later than now).
static uint64_t lorieFreezeSince(uint64_t now, uint64_t a, uint64_t b) {
    uint64_t from = a > b ? a : b;
    return from && from <= now ? now - from : 0;
}

/*
 * One look: whether a freeze begins, goes past 1, 2 or 3 seconds (a level), or ends. A freeze is work waiting with none
 * of it moving for LORIE_FREEZE_BEGIN_US, each kind timed from when it was asked for or last moved, whichever is later:
 * the renderer's (frames asked for, GPU copies, itself in one stage - while it has a window; a root copy, while it is
 * connected), the X server's (its thread in one stage, ticks posted that it does not run). Nothing waiting is no
 * freeze, however long: an idle screen, a client drawing nothing, no window. It ends at the first look that finds what
 * waited moved or gone (`moved`: what of that, as it began, moved - a stage left counts). Times are within a period
 * (LORIE_FREEZE_PERIOD_US) of the event.
 */
static int lorieFreezeCheck(struct lorie_freeze *f, const struct lorie_freeze_sample *s) {
    uint64_t now = s->nowUs, u, x = 0, r = 0;
    uint32_t pending = 0, moved = 0, k;
    int what = LORIE_FREEZE_NONE;

    if (!f->started) {
        f->started = TRUE;
        f->framesUs = f->copiesUs = f->rootUs = f->ticksUs = now;
        f->last = *s;
        return what;
    }
    if (s->framesShown != f->last.framesShown)
        f->framesUs = now, moved |= LORIE_FREEZE_FRAME;
    if (s->copiesDone != f->last.copiesDone)
        f->copiesUs = now, moved |= LORIE_FREEZE_COPY;
    if (s->rootDone != f->last.rootDone || s->rootCancelled != f->last.rootCancelled)
        f->rootUs = now, moved |= LORIE_FREEZE_ROOT;
    if (s->ticksRun != f->last.ticksRun)
        f->ticksUs = now, moved |= LORIE_FREEZE_TICKS;
    if (s->rStage != f->last.rStage || s->rStageUs != f->last.rStageUs)
        moved |= LORIE_FREEZE_RSTAGE;
    if (s->xStage != f->last.xStage || s->xStageUs != f->last.xStageUs)
        moved |= LORIE_FREEZE_XSTAGE;
    if (s->surface) {
        if (s->askSeq != s->takenSeq && (u = lorieFreezeSince(now, s->askUs, f->framesUs)) >= LORIE_FREEZE_PERIOD_US)
            pending |= LORIE_FREEZE_FRAME, r = u > r ? u : r;
        if (s->copiesQueued && (u = lorieFreezeSince(now, s->copyQueuedUs, f->copiesUs)) >= LORIE_FREEZE_PERIOD_US)
            pending |= LORIE_FREEZE_COPY, r = u > r ? u : r;
        if (s->rStage != LORIE_R_SLEEP && s->rStage != LORIE_R_PASS &&
            (u = lorieFreezeSince(now, s->rStageUs, 0)) >= LORIE_FREEZE_PERIOD_US)
            pending |= LORIE_FREEZE_RSTAGE, r = u > r ? u : r;
    }
    if (s->connected && s->rootSeq && s->rootSeq != s->rootDone && s->rootSeq != s->rootCancelled &&
        (u = lorieFreezeSince(now, s->rootQueuedUs, f->rootUs)) >= LORIE_FREEZE_PERIOD_US)
        pending |= LORIE_FREEZE_ROOT, r = u > r ? u : r;
    if (s->xStage != LORIE_X_LOOP && (u = lorieFreezeSince(now, s->xStageUs, 0)) >= LORIE_FREEZE_PERIOD_US)
        pending |= LORIE_FREEZE_XSTAGE, x = u > x ? u : x;
    if (s->ticksPosted != s->ticksRun && (u = lorieFreezeSince(now, f->ticksUs, 0)) >= LORIE_FREEZE_PERIOD_US)
        pending |= LORIE_FREEZE_TICKS, x = u > x ? u : x;

    f->pending = pending;
    f->xStallUs = x;
    f->rStallUs = r;
    f->stallUs = x > r ? x : r;
    if (!f->on) {
        if (f->stallUs >= LORIE_FREEZE_BEGIN_US) {
            f->on = TRUE;
            f->id++;
            f->levels = f->level = 0;
            f->beginUs = now - f->stallUs;
            f->side = x >= LORIE_FREEZE_BEGIN_US ? (r >= LORIE_FREEZE_BEGIN_US ? 'b' : 'x') : 'r';
            f->beginPending = pending;
            f->beginXStage = s->xStage;
            f->beginRStage = s->rStage;
            what = LORIE_FREEZE_BEGIN;
        }
    } else if (f->stallUs < LORIE_FREEZE_BEGIN_US) {
        f->on = FALSE;
        f->moved = moved & f->beginPending;     // of what waited as it began, what moved (none: it is no longer waited for)
        what = LORIE_FREEZE_END;
    } else {
        for (k = 0; k < 3; k++)
            if (!(f->levels & (1u << k)) && now - f->beginUs >= (uint64_t) (k + 1) * 1000000) {
                f->levels |= 1u << k;
                f->level = k + 1;
                what = LORIE_FREEZE_LEVEL;
            }
    }
    f->last = *s;
    return what;
}

// The watchdog's look at the shared state and this process's own, as they are - no lock taken, nothing waited for.
static void lorieFreezeSample(struct lorie_freeze_sample *s) {
    volatile struct lorie_shared_server_state *st = pvfb->state;
    volatile struct lorie_progress *p = &st->progress;
    uint32_t r = __atomic_load_n(&st->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE);
    uint32_t w = __atomic_load_n(&st->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);

    memset(s, 0, sizeof(*s));
    s->nowUs = lorieMonotonicUs();
    s->connected = lorieConnectionAlive();
    s->surface = s->connected && st->surfaceAvailable;
    s->framesShown = lorieFramesShown();
    s->ticksPosted = __atomic_load_n(&st->diag.xTicksPosted, __ATOMIC_RELAXED);
    s->ticksRun = __atomic_load_n(&st->diag.xTicksRun, __ATOMIC_RELAXED);
    s->requests = __atomic_load_n(&pvfb->xRequests, __ATOMIC_RELAXED);
    s->copiesDone = __atomic_load_n(&st->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE);
    s->rootSeq = __atomic_load_n(&st->rootCopy.seq, __ATOMIC_ACQUIRE);
    s->rootTaken = st->rootCopy.taken;
    s->rootDone = __atomic_load_n(&st->rootCopy.done, __ATOMIC_ACQUIRE);
    s->rootCancelled = st->rootCopy.cancelled;
    s->rootWaiting = st->rootCopy.waiting;
    s->rootQueuedUs = st->rootCopy.queuedUs;
    s->askSeq = __atomic_load_n(&p->askSeq, __ATOMIC_ACQUIRE);
    s->takenSeq = __atomic_load_n(&p->takenSeq, __ATOMIC_ACQUIRE);
    s->askUs = s->askSeq != s->takenSeq ? lorieAskUs(p, s->takenSeq + 1, s->askSeq) : 0;
    s->copiesQueued = w - r;
    s->copyQueuedUs = w != r ? st->gpuCopyQueue.entries[r % LORIE_GPU_COPY_QUEUE_CAPACITY].queuedUs : 0;
    s->xStage = __atomic_load_n(&pvfb->xStage, __ATOMIC_ACQUIRE);
    s->xStageUs = __atomic_load_n(&pvfb->xStageUs, __ATOMIC_RELAXED);
    s->requestClient = __atomic_load_n(&pvfb->xRequestClient, __ATOMIC_RELAXED);
    s->requestMajor = __atomic_load_n(&pvfb->xRequestMajor, __ATOMIC_RELAXED);
    s->requestMinor = __atomic_load_n(&pvfb->xRequestMinor, __ATOMIC_RELAXED);
    s->rStage = __atomic_load_n(&p->rStage, __ATOMIC_ACQUIRE);
    s->rStageUs = p->rStageUs;
    s->gpuBusy = __atomic_load_n(&st->gpuInFlight.busy, __ATOMIC_ACQUIRE);
    s->lockPid = st->lockingPid;
    s->notifiedSerial = __atomic_load_n(&pvfb->copyNotifiedSerial, __ATOMIC_ACQUIRE);
    s->ackedSerial = __atomic_load_n(&pvfb->copyAckedSerial, __ATOMIC_ACQUIRE);
}

/*
 * A thread of this process, as /proc has it: its state (R running or waiting to, S asleep, D in the kernel and not to be
 * interrupted), its time run and waited on a run queue (schedstat, microseconds; -1: not there), and the system call it
 * is in (-1: none, asleep anyway; -2: not known) with its first argument.
 */
struct lorie_thread_info {
    char state;
    long long runUs, waitUs;
    long sys;
    unsigned long long arg;
    Bool running;
};

static ssize_t lorieReadSmall(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, size - 1);
    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static void lorieThreadInfo(pid_t tid, struct lorie_thread_info *t) {
    char path[64], buf[512], *p;
    unsigned long long run, wait;

    *t = (struct lorie_thread_info) { '?', -1, -1, -2, 0, FALSE };
    if (tid <= 0)
        return;
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    if (lorieReadSmall(path, buf, sizeof(buf)) > 0 && (p = strrchr(buf, ')')) && p[1] == ' ' && p[2])
        t->state = p[2];
    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
    if (lorieReadSmall(path, buf, sizeof(buf)) > 0 && sscanf(buf, "%llu %llu", &run, &wait) == 2)
        t->runUs = (long long) (run / 1000), t->waitUs = (long long) (wait / 1000);
    snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", tid);
    if (lorieReadSmall(path, buf, sizeof(buf)) > 0) {
        if (!strncmp(buf, "running", 7))
            t->running = TRUE;
        else if (sscanf(buf, "%ld %llx", &t->sys, &t->arg) < 1)
            t->sys = -2;
    }
}

// That system call as a word: a futex on one of the shared state's words by that word's name, else by its kind.
static void lorieSyscallWord(const struct lorie_thread_info *t, char *out, size_t size) {
    volatile struct lorie_shared_server_state *st = pvfb->state;
    uintptr_t a = (uintptr_t) t->arg;
    const char *what = t->running ? "running" : t->sys == -2 ? "unknown" : t->sys == -1 ? "none" : NULL;

    if (!what && t->sys == SYS_futex)
        what = a == (uintptr_t) &st->lock ? "futex:lock" : a == (uintptr_t) &st->cursor.lock ? "futex:cursor_lock" :
               a == (uintptr_t) &st->rootCopy.done ? "futex:root_done" : a == (uintptr_t) &st->gpuInFlight.busy ? "futex:gpu_busy" :
               "futex";
#ifdef SYS_epoll_pwait
    if (!what && t->sys == SYS_epoll_pwait)
        what = "epoll_pwait";
#endif
#ifdef SYS_ppoll
    if (!what && t->sys == SYS_ppoll)
        what = "ppoll";
#endif
    if (what)
        snprintf(out, size, "%s", what);
    else
        snprintf(out, size, "nr%ld", t->sys);
}

// What waits or moved (LORIE_FREEZE_*), as a word.
static void lorieFreezeKinds(uint32_t m, const char *none, char *out, size_t size) {
    static const char *const names[LORIE_FREEZE_KINDS] = { "frame", "copy", "root", "renderer", "x", "ticks" };
    size_t n = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < LORIE_FREEZE_KINDS && n + 1 < size; i++)
        if (m & (1u << i))
            n += (size_t) snprintf(out + n, size - n, "%s%s", n ? "," : "", names[i]);
    if (!n)
        snprintf(out, size, "%s", none);
}

// Where it was seen to stand: the X server's thread's stage, the renderer's, or both (the side that waits).
static void lorieFreezeStage(char side, uint32_t xStage, uint32_t rStage, char *out, size_t size) {
    const char *xs = lorieXStageNames[xStage < LORIE_X_STAGES ? xStage : 0];
    const char *rs = lorieRStageNames[rStage < LORIE_R_STAGES ? rStage : 0];
    if (side == 'x')
        snprintf(out, size, "x:%s", xs);
    else if (side == 'r')
        snprintf(out, size, "renderer:%s", rs);
    else
        snprintf(out, size, "x:%s+renderer:%s", xs, rs);
}

// The threads the watchdog looks at: the X server's, the input thread, the Choreographer's, its own.
enum { LORIE_THREAD_MAIN, LORIE_THREAD_INPUT, LORIE_THREAD_VSYNC, LORIE_THREAD_WATCH, LORIE_THREADS };

/*
 * The line for `what` (begin, level or end) into `out`, within logcat's 1023 bytes: `th` the threads now, `begin` them
 * as the freeze began. Begin and level: how long and what waits, where each side stands (the X server's thread also by
 * /proc: its state and system call), the asks, the copies (queued, done, told of, rechecked), the root copy
 * (seq/taken/done/cancelled), the GPU's list, who holds the shared lock. End: how long, where it stood when it began
 * and last, what moved, and the X server's threads' time run and waited for a CPU meanwhile. A stage is where it was
 * seen to stand, not a cause.
 */
static int lorieFreezeLine(char *out, size_t size, const struct lorie_freeze *f, const struct lorie_freeze_sample *s, int what,
                           const struct lorie_thread_info *th, const struct lorie_thread_info *begin) {
    static const char *const sides[] = { "x", "renderer", "both" };
    const char *side = sides[f->side == 'x' ? 0 : f->side == 'r' ? 1 : 2];
    char pending[64], stage[64], sys[32], last[64], moved[64];

    if (what == LORIE_FREEZE_END) {
        lorieFreezeStage(f->side, f->beginXStage, f->beginRStage, stage, sizeof(stage));
        lorieFreezeStage(f->side, s->xStage, s->rStage, last, sizeof(last));
        lorieFreezeKinds(f->beginPending, "none", pending, sizeof(pending));
        lorieFreezeKinds(f->moved, "nothing_waits", moved, sizeof(moved));
#define LORIE_DELTA(i, field) (th[i].field >= 0 && begin[i].field >= 0 ? th[i].field - begin[i].field : -1LL)
        return snprintf(out, size, "XlorieFreeze: phase=end id=%u duration_us=%llu side=%s stalled_stage=%s last_stage=%s "
                                   "pending_at_begin=%s ended_by=%s levels=%u x_main_run_us=%lld x_main_runq_us=%lld "
                                   "input_runq_us=%lld vsync_runq_us=%lld",
                        f->id, (unsigned long long) (s->nowUs - f->beginUs), side, stage, last, pending, moved,
                        (unsigned) __builtin_popcount(f->levels), LORIE_DELTA(LORIE_THREAD_MAIN, runUs),
                        LORIE_DELTA(LORIE_THREAD_MAIN, waitUs), LORIE_DELTA(LORIE_THREAD_INPUT, waitUs),
                        LORIE_DELTA(LORIE_THREAD_VSYNC, waitUs));
#undef LORIE_DELTA
    }
    lorieFreezeKinds(f->pending, "none", pending, sizeof(pending));
    lorieFreezeStage(f->side, s->xStage, s->rStage, stage, sizeof(stage));
    lorieSyscallWord(&th[LORIE_THREAD_MAIN], sys, sizeof(sys));
    return snprintf(out, size, "XlorieFreeze: phase=%s id=%u%s side=%s stall_us=%llu x_stall_us=%llu r_stall_us=%llu pending=%s "
                               "stage=%s x_stage=%s x_stage_us=%llu x_request=%d.%d@%d x_main=%c,%s ticks_posted=%u ticks_run=%u "
                               "r_stage=%s r_stage_us=%llu surface=%d asks=%u taken=%u ask_wait_us=%llu copies_queued=%u "
                               "copy_wait_us=%llu copies_done=%llu notified=%llu acked=%llu root=%u/%u/%u/%u root_wait_us=%llu "
                               "x_waiting_root=%u gpu_busy=%u lock_pid=%d",
                    what == LORIE_FREEZE_BEGIN ? "begin" : "level", f->id,
                    what == LORIE_FREEZE_LEVEL ? (f->level == 1 ? " level=1s" : f->level == 2 ? " level=2s" : " level=3s") : "",
                    side, (unsigned long long) f->stallUs, (unsigned long long) f->xStallUs, (unsigned long long) f->rStallUs,
                    pending, stage, lorieXStageNames[s->xStage < LORIE_X_STAGES ? s->xStage : 0],
                    (unsigned long long) lorieFreezeSince(s->nowUs, s->xStageUs, 0), s->requestMajor, s->requestMinor,
                    s->requestClient, th[LORIE_THREAD_MAIN].state, sys, s->ticksPosted, s->ticksRun,
                    lorieRStageNames[s->rStage < LORIE_R_STAGES ? s->rStage : 0],
                    (unsigned long long) lorieFreezeSince(s->nowUs, s->rStageUs, 0), s->surface ? 1 : 0, s->askSeq, s->takenSeq,
                    (unsigned long long) (s->askSeq != s->takenSeq ? lorieFreezeSince(s->nowUs, s->askUs, 0) : 0), s->copiesQueued,
                    (unsigned long long) (s->copiesQueued ? lorieFreezeSince(s->nowUs, s->copyQueuedUs, 0) : 0),
                    (unsigned long long) s->copiesDone, (unsigned long long) s->notifiedSerial, (unsigned long long) s->ackedSerial,
                    s->rootSeq, s->rootTaken, s->rootDone, s->rootCancelled,
                    (unsigned long long) (s->rootSeq && s->rootSeq != s->rootDone && s->rootSeq != s->rootCancelled
                                          ? lorieFreezeSince(s->nowUs, s->rootQueuedUs, 0) : 0),
                    s->rootWaiting, s->gpuBusy, s->lockPid);
}

/*
 * Every 5 seconds: the X server's threads over the period - time run, time waited on a run queue (runnable, not run:
 * a CPU not given), their state and the main one's system call now - and the 5 second report's own time; a thread
 * starved shows its wait, one blocked shows neither.
 */
/*
 * The X server's input thread, as the watchdog can know it: its tid once it has run (ddxInputThreadInit - dix main starts
 * it last, as Dispatch begins, which can be after the watchdog's first look); 0 until then, with why: "starting" (this
 * build has one, INPUTTHREAD, and this server too - -dumbSched turns it off, InputThreadEnable), else "none".
 */
static pid_t lorieInputTid(const char **why) {
#if INPUTTHREAD
    pid_t tid = __atomic_load_n(&pvfb->inputTid, __ATOMIC_ACQUIRE);
    *why = tid ? NULL : InputThreadEnable ? "starting" : "none";
    return tid;
#else
    *why = "none";
    return 0;
#endif
}

static int lorieXSchedLine(char *out, size_t size, const struct lorie_thread_info *th, const struct lorie_thread_info *at,
                           uint32_t freezes) {
    char sys[32], input[16];
    const char *why;
#define LORIE_D(i, field) (th[i].field >= 0 && at[i].field >= 0 ? th[i].field - at[i].field : -1LL)
    lorieSyscallWord(&th[LORIE_THREAD_MAIN], sys, sizeof(sys));
    if (!lorieInputTid(&why))
        snprintf(input, sizeof(input), "%s", why);
    else
        snprintf(input, sizeof(input), "%c", th[LORIE_THREAD_INPUT].state);
    return snprintf(out, size, "XlorieXSched: main=%c,%s,run_us=%lld,runq_us=%lld input=%s,run_us=%lld,runq_us=%lld "
                               "vsync=%c,run_us=%lld,runq_us=%lld watchdog_run_us=%lld report_us=%llu report_max_us=%llu freezes=%u",
                    th[LORIE_THREAD_MAIN].state, sys, LORIE_D(LORIE_THREAD_MAIN, runUs), LORIE_D(LORIE_THREAD_MAIN, waitUs),
                    input, LORIE_D(LORIE_THREAD_INPUT, runUs), LORIE_D(LORIE_THREAD_INPUT, waitUs),
                    th[LORIE_THREAD_VSYNC].state, LORIE_D(LORIE_THREAD_VSYNC, runUs), LORIE_D(LORIE_THREAD_VSYNC, waitUs),
                    LORIE_D(LORIE_THREAD_WATCH, runUs), (unsigned long long) __atomic_load_n(&pvfb->reportUs, __ATOMIC_RELAXED),
                    (unsigned long long) __atomic_exchange_n(&pvfb->reportMaxUs, 0, __ATOMIC_RELAXED), freezes);
#undef LORIE_D
}

/*
 * The watchdog's thread: a look every LORIE_FREEZE_PERIOD_US (lorieFreezeSample, lorieFreezeCheck), a line only when a
 * freeze begins, passes a level or ends, and the threads' line every 5 seconds. It takes no lock, waits for nothing
 * but its own sleep and restarts nothing: it only reports.
 */
static void *lorieFreezeWatch(__unused void *arg) {
    static struct lorie_freeze f;
    struct lorie_freeze_sample s;
    struct lorie_thread_info th[LORIE_THREADS], at[LORIE_THREADS], begin[LORIE_THREADS];
    pid_t tids[LORIE_THREADS];
    char line[1024], input[16];
    const char *why;
    uint64_t schedUs;
    int what, i;

    pthread_setname_np(pthread_self(), "XlorieWatch");
    usleep(1000000);    // the Choreographer's thread has run by now; the input thread may not (lorieInputTid)
    tids[LORIE_THREAD_MAIN] = pvfb->mainTid, tids[LORIE_THREAD_INPUT] = lorieInputTid(&why);
    tids[LORIE_THREAD_VSYNC] = pvfb->vsyncTid, tids[LORIE_THREAD_WATCH] = gettid();
    if (tids[LORIE_THREAD_INPUT])
        snprintf(input, sizeof(input), "%d", tids[LORIE_THREAD_INPUT]);
    else
        snprintf(input, sizeof(input), "%s", why);
    log(INFO, "XlorieFreeze: phase=armed period_us=%d begin_us=%d levels_us=1000000,2000000,3000000 main_tid=%d input_tid=%s "
              "vsync_tid=%d", LORIE_FREEZE_PERIOD_US, LORIE_FREEZE_BEGIN_US, tids[0], input, tids[2]);
    for (i = 0; i < LORIE_THREADS; i++)
        lorieThreadInfo(tids[i], &at[i]);
    memcpy(begin, at, sizeof(begin));
    schedUs = lorieMonotonicUs();
    for (;;) {
        usleep(LORIE_FREEZE_PERIOD_US);
        if (!tids[LORIE_THREAD_VSYNC])
            tids[LORIE_THREAD_VSYNC] = pvfb->vsyncTid;
        if (!tids[LORIE_THREAD_INPUT])
            tids[LORIE_THREAD_INPUT] = lorieInputTid(&why);     // run since: looked at from now on
        lorieFreezeSample(&s);
        if ((what = lorieFreezeCheck(&f, &s)) != LORIE_FREEZE_NONE) {
            for (i = 0; i < LORIE_THREADS; i++)
                lorieThreadInfo(tids[i], &th[i]);
            if (what == LORIE_FREEZE_BEGIN)
                memcpy(begin, th, sizeof(begin));
            lorieFreezeLine(line, sizeof(line), &f, &s, what, th, begin);
            log(INFO, "%s", line);
        }
        if (s.nowUs - schedUs >= 5000000) {
            for (i = 0; i < LORIE_THREADS; i++)
                lorieThreadInfo(tids[i], &th[i]);
            lorieXSchedLine(line, sizeof(line), th, at, f.id);
            log(INFO, "%s", line);
            memcpy(at, th, sizeof(at));
            schedUs = s.nowUs;
        }
    }
    return NULL;
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

// P11-A: work for the main thread that should not wait for the client dispatch round in progress to end - a vsync tick,
// a GPU copy's completion - queued from any thread. With TERMUX_X11_X_YIELD_ON_WAKE=1 that round is broken off at its next
// request boundary, the work runs, and the round goes on with the rest of its slice (patches/xserver.patch: Dispatch,
// WaitForSomething): queued before the yield is asked for, the work is always there for it. Otherwise, as any work: once
// the round is over.
void lorieQueueUrgentWorkProc(Bool (*proc)(ClientPtr, void *), void *closure) {
    if (QueueWorkProc(proc, NULL, closure) && __atomic_load_n(&pvfb->yieldOnWake, __ATOMIC_RELAXED))
        __atomic_exchange_n(&dispatchYieldRequested, 1, __ATOMIC_RELEASE);
    lorieWakeServer();
}

static void lorieWorkingQueueCallback(int fd, int __unused ready, void __unused *data) {
    // Nothing to do here. It is needed to interrupt ospoll_wait.
    eventfd_t dummy;
    eventfd_read(fd, &dummy);
}

void lorieChoreographerFrameCallback(long t, AChoreographer* d) {
    uint64_t now = lorieMonotonicUs();
    if (!pvfb->vsyncTid)
        pvfb->vsyncTid = gettid();  // P11-C3-C: for the freeze watchdog's look at it
    AChoreographer_postFrameCallback(d, (AChoreographer_frameCallback) lorieChoreographerFrameCallback, d);
    if (pScreenPtr) {
        // P10-B0: the vsync's frame time to here (a 32-bit long cannot hold it); the tick carries this callback's stamp.
        if (sizeof(long) >= 8 && t > 0)
            lorieStampPair(&pvfb->state->diag.xVsyncToCallback, (uint64_t) t / 1000, now);
        __atomic_fetch_add(&pvfb->state->diag.xTicksPosted, 1, __ATOMIC_RELAXED);
        lorieQueueUrgentWorkProc(lorieRedraw, (void *) (uintptr_t) (uint32_t) now);
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
    // P11-C3-D1 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1): the RENDER composites timed, over EXA's
    if (pvfb->frameDiag) {
        PictureScreenPtr ps = GetPictureScreen(pScreen);
        pvfb->composite = ps->Composite;
        ps->Composite = lorieCompositeTimed;
    }

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
    entry->cancelled = 0;
    entry->queuedUs = lorieMonotonicUs();
    for (i = 0; i < numRects; i++)
        entry->rects[i] = (LorieGpuCopyRect) { box[i].x1, box[i].y1, box[i].x2, box[i].y2 };

    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above
    lorieWakeRenderer(rendererWake);

    *out_serial = entry->serial;
    gpuCopyAttempts++;
    gpuCopyOffloads++;
    return TRUE;
}

// P10-B0: GPU copy completions, from the renderer's publish to the input thread (here, on it) - the low 32 bits of
// when it was received are what the work procedure carries to the X server's thread (lorieGpuCopyDoneHandled).
uint32_t lorieGpuCopyDoneReceived(uint64_t publishedUs, uint64_t serial) {
    uint64_t now = lorieMonotonicUs();
    if (pvfb->freezeDiag) {
        // P11-C3-C: the last completion told of (the Vulkan renderer's serial), and when
        __atomic_store_n(&pvfb->copyNotifiedUs, now, __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->copyNotifiedSerial, serial, __ATOMIC_RELEASE);
    }
    if (publishedUs)
        lorieStampPair(&pvfb->state->diag.xCopyNotify, publishedUs, now);
    __atomic_fetch_add(&pvfb->state->diag.xCopyDonePosted, 1, __ATOMIC_RELAXED);
    return (uint32_t) now;
}

// P10-B0: ... and from the input thread to the X server's thread done with the recheck it caused.
void lorieGpuCopyDoneHandled(uint32_t receivedUs) {
    lorieRanAfter(&pvfb->state->diag.xCopyRecheck, receivedUs, &pvfb->state->diag.xCopyDonePosted,
                  &pvfb->state->diag.xCopyDoneRun, &pvfb->state->diag.xCopyDoneWaitingMax);
    if (pvfb->freezeDiag) {
        // P11-C3-C: the copies rechecked up to the completion published by then, and when
        __atomic_store_n(&pvfb->copyAckedUs, lorieMonotonicUs(), __ATOMIC_RELAXED);
        __atomic_store_n(&pvfb->copyAckedSerial, __atomic_load_n(&pvfb->state->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
    }
}

/*
 * P10-S1: what may still happen to GPU copy `serial`, waiting for nothing - for presentations that end before it
 * is known to be done (patches/xserver.patch: present_vblank_scrap, present_vblank_destroy, present_execute_copy,
 * the retired copies). LORIE_GPU_COPY_DONE: its completion is published, or it left the queue (Vulkan publishes
 * readIndex past it after its fence; GLES records, waits for its fence and publishes all under the shared lock).
 * LORIE_GPU_COPY_CANCELLED: still queued, and no renderer can touch its buffers any more - not recorded (the renderers
 * record under the shared lock; marked cancelled under it, it never will be), or recorded by a renderer process that
 * no longer exists (marked too: a later renderer would otherwise record it again). LORIE_GPU_COPY_RUNNING: recorded,
 * its buffers listed in flight by a renderer that exists - or the shared lock held by one that exists: asked again
 * at the next completion or tick (lorieRecheckGpuCopies). Never a timeout: only those facts settle it.
 */
int lorieGpuCopySettle(uint64_t serial) {
    struct lorie_shared_server_state *st = pvfb->state;
    int settled = LORIE_GPU_COPY_DONE;
    bool nested;
    uint32_t i, end;

    if (lorieGpuCopyIsDone(serial))
        return LORIE_GPU_COPY_DONE;
    nested = st->lockingPid == getpid();    // asked while the X server holds it (recursive): no second lock
    if (!nested && !lorie_mutex_trylock(&st->lock, &st->lockingPid)) {
        pid_t holder = st->lockingPid;
        // Held by a renderer that exists: asked again later. By one that died holding it: lorie_mutex_lock takes it
        // over (it gives a lock up only when no renderer is connected and its holder is not this process).
        if (lorieConnectionAlive() || holder <= 0 || kill(holder, 0) == 0 || errno != ESRCH)
            return LORIE_GPU_COPY_RUNNING;
        lorie_mutex_lock(&st->lock, &st->lockingPid);
    }
    end = __atomic_load_n(&st->gpuCopyQueue.writeIndex, __ATOMIC_ACQUIRE);
    if (!lorieGpuCopyIsDone(serial))
        for (i = st->gpuCopyQueue.readIndex; i != end; i++) {
            LorieGpuCopyEntry *e = &st->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY];
            if (e->serial != serial)
                continue;
            if ((lorieGpuInFlight(st, e->srcBufferId) || lorieGpuInFlight(st, e->dstBufferId)) && !lorieGpuSubmitterGone(st))
                settled = LORIE_GPU_COPY_RUNNING;
            else {
                if (!e->cancelled)
                    __atomic_fetch_add(&st->diag.xCopyCancelled, 1, __ATOMIC_RELAXED);
                e->cancelled = 1;
                settled = LORIE_GPU_COPY_CANCELLED;
            }
            break;
        }
    if (!nested)
        lorie_mutex_unlock(&st->lock, &st->lockingPid);
    return settled;
}

// P10-S1: a retired copy made (made) or let go (patches/xserver.patch present_retire_copy, present_retired_copy_release).
void lorieCountGpuCopyRetired(int made) {
    __atomic_fetch_add(made ? &pvfb->state->diag.xCopyRetired : &pvfb->state->diag.xCopyRetiredReleased, 1, __ATOMIC_RELAXED);
}

// P10-B0: a GPU-copied presentation ended before its copy was done (patches/xserver.patch, LORIE_X_EARLY_*).
void lorieCountGpuCopyEarly(int how) {
    if (how >= 0 && how < LORIE_X_EARLY_COUNT)
        __atomic_fetch_add(&pvfb->state->diag.xCopyEarly[how], 1, __ATOMIC_RELAXED);
}

// P11-C3-D1-A (TERMUX_X11_EXA_COMPOSITE_OPT=1): a Present GPU copy's damage put off until the copy is done
// (patches/xserver.patch present_damage_gpu_copy), counted; and how each one put off went (LORIE_COPY_DAMAGE_*).
Bool lorieDeferCopyDamage(void) {
    if (!pvfb->exaCompositeOpt)
        return FALSE;
    __atomic_fetch_add(&pvfb->state->diag.copyDamageDeferred, 1, __ATOMIC_RELAXED);
    return TRUE;
}

void lorieCountCopyDamage(int how) {
    __atomic_fetch_add(how == LORIE_COPY_DAMAGE_DONE ? &pvfb->state->diag.copyDamageDone
                       : how == LORIE_COPY_DAMAGE_EARLY ? &pvfb->state->diag.copyDamageEarly : &pvfb->state->diag.copyDamageDropped,
                       1, __ATOMIC_RELAXED);
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

/* ---- P11-C3-D0, TERMUX_X11_CONTENT_FRAME_DIAG=1: client content from its presentation to the screen ---- */

// The client presenting (the request being dispatched), counted in its slot - named once as it came.
static void lorieContentClient(Bool skipped, uint64_t now) {
    ClientPtr client = GetCurrentClient();
    int i, free = -1, least = 0;

    if (!client)
        return;
    for (i = 0; i < LORIE_CONTENT_CLIENTS; i++) {
        if (pvfb->contentClients[i].client == client && pvfb->contentClients[i].index == client->index)
            break;
        if (!pvfb->contentClients[i].client && free < 0)
            free = i;
        if (pvfb->contentClients[i].requests < pvfb->contentClients[least].requests)
            least = i;
    }
    if (i == LORIE_CONTENT_CLIENTS) {
        if (skipped)
            return;     // counted from its first request on
        i = free >= 0 ? free : least;
        memset(&pvfb->contentClients[i], 0, sizeof(pvfb->contentClients[i]));
        pvfb->contentClients[i].client = client;
        pvfb->contentClients[i].index = client->index;
        lorieDiagWord(pvfb->contentClients[i].cmd, sizeof(pvfb->contentClients[i].cmd), GetClientCmdName(client));
    }
    if (skipped) {
        pvfb->contentClients[i].skipped++;
        return;
    }
    if (pvfb->contentClients[i].lastUs && now > pvfb->contentClients[i].lastUs &&
        now - pvfb->contentClients[i].lastUs > pvfb->contentClients[i].maxGapUs)
        pvfb->contentClients[i].maxGapUs = (uint32_t) (now - pvfb->contentClients[i].lastUs > UINT32_MAX ? UINT32_MAX
                                                                                                      : now - pvfb->contentClients[i].lastUs);
    pvfb->contentClients[i].lastUs = now;
    pvfb->contentClients[i].requests++;
}

// A presentation with a pixmap asked for (patches/xserver.patch present_scmd_pixmap): its content's number, its request.
uint32_t lorieContentRequest(PixmapPtr pixmap, uint64_t *request_us) {
    uint64_t now;
    *request_us = 0;
    if (!pvfb->contentDiag || !pixmap)
        return 0;
    *request_us = now = lorieMonotonicUs();
    if (!++pvfb->contentSeq)
        ++pvfb->contentSeq;     // 0 is none
    __atomic_fetch_add(&pvfb->state->diag.contentRequests, 1, __ATOMIC_RELAXED);
    lorieContentClient(FALSE, now);
    return pvfb->contentSeq;
}

// ... skipped: overwritten in its frame by a newer one, or its GPU copy cancelled (present_vblank_scrap).
void lorieContentSkipped(uint32_t seq) {
    if (!seq)
        return;
    __atomic_fetch_add(&pvfb->state->diag.contentSkipped, 1, __ATOMIC_RELAXED);
    lorieContentClient(TRUE, 0);
}

/*
 * Content `c` into `dst`, as of `now`, should it be newer than what `dst` holds: `dst`'s stamp - copied in, or carried
 * by a CPU composite or copy (`carried`) - counted (into the screen pixmap: its wait from its copy complete).
 */
static void lorieContentStore(PixmapPtr dst, const struct lorie_content_stamp *c, uint64_t now, Bool carried) {
    LoriePixmapPriv *priv = dst ? LORIE_PIXMAP_PRIV_FROM_PIXMAP(dst) : NULL;
    if (!priv || !c->seq || (priv->content.seq && !lorieContentNewer(c->seq, priv->content.seq)))
        return;
    priv->content = *c;
    priv->content.readyUs = now;
    if (pScreenPtr && dst == pScreenPtr->devPrivate) {
        __atomic_fetch_add(&pvfb->state->diag.contentRootUpdates, 1, __ATOMIC_RELAXED);
        lorieHistAdd(&pvfb->state->diag.contentCompleteToRoot, now >= c->copyUs ? now - c->copyUs : 0);
    } else if (carried)
        __atomic_fetch_add(&pvfb->state->diag.contentCompositorUpdates, 1, __ATOMIC_RELAXED);
}

/*
 * Its copy queued for the GPU (`gpu`, present_execute_copy: when, for its completion's wait; the copy noted with its
 * queue slot - the one lorieTryScheduleGpuCopy just filled - its damage not reported yet: lorieContentDamaged) or made
 * by the CPU (its content in `dst` now). The wait from the request.
 */
uint64_t lorieContentCopied(uint32_t seq, uint64_t request_us, PixmapPtr dst, Bool gpu) {
    LoriePixmapPriv *priv = dst ? LORIE_PIXMAP_PRIV_FROM_PIXMAP(dst) : NULL;
    uint64_t now;
    if (!seq)
        return 0;
    now = lorieMonotonicUs();
    __atomic_fetch_add(gpu ? &pvfb->state->diag.contentGpuCopies : &pvfb->state->diag.contentCpuCopies, 1, __ATOMIC_RELAXED);
    lorieHistAdd(&pvfb->state->diag.contentPresentToCopy, now >= request_us ? now - request_us : 0);
    if (gpu)
        pvfb->contentQueued[(pvfb->state->gpuCopyQueue.writeIndex - 1) % LORIE_GPU_COPY_QUEUE_CAPACITY] =
            (typeof(pvfb->contentQueued[0])) { pvfb->gpuCopySerialCounter, FALSE };
    else
        lorieContentStore(dst, &(struct lorie_content_stamp) { seq, request_us, now, now }, now, FALSE);
    return now;
}

// ... its GPU copy complete, taken up by the X server (present_execute_copy): its content in `dst` now.
void lorieContentCopyDone(uint32_t seq, uint64_t request_us, uint64_t copy_us, PixmapPtr dst) {
    uint64_t now;
    if (!seq)
        return;
    now = lorieMonotonicUs();
    __atomic_fetch_add(&pvfb->state->diag.contentCopiesDone, 1, __ATOMIC_RELAXED);
    if (copy_us)
        lorieHistAdd(&pvfb->state->diag.contentCopyToComplete, now >= copy_us ? now - copy_us : 0);
    lorieContentStore(dst, &(struct lorie_content_stamp) { seq, request_us, now, now }, now, FALSE);
}

// ... its CompleteNotify sent (present_execute_post), in that mode.
void lorieContentComplete(uint32_t seq, CARD8 mode) {
    if (!seq)
        return;
    __atomic_fetch_add(mode == PresentCompleteModeSkip ? &pvfb->state->diag.contentCompletesSkip : &pvfb->state->diag.contentCompletes,
                       1, __ATOMIC_RELAXED);
}

/*
 * A CPU access opening (loriePrepareAccess), done (lorieFinishAccess), or failed (loriePrepareAccess returning FALSE:
 * EXA calls no FinishAccess for it - ExaDoPrepareAccess). By EXA's index - one pixmap open at each at a time (EXA's
 * access[]; nested accesses to one pixmap reach the driver once): a source's content kept for its index from its
 * opening on - newest kept, should another source open there before the destination finishes - and the newest of them
 * all carried into the destination as the destination's access finishes (EXA opens the destination before the sources
 * for a composite, after them for a copy, and finishes the sources first either way); once no access is open, all
 * forgotten. A failed opening is undone: its index carries what it did before, and counts as not open.
 */
enum { LORIE_CONTENT_OPEN, LORIE_CONTENT_DONE, LORIE_CONTENT_FAILED };
static void lorieContentAccess(PixmapPtr pPix, LoriePixmapPriv *priv, int index, int how) {
    Bool dest = index == EXA_PREPARE_DEST || index == EXA_PREPARE_AUX_DEST;
    struct lorie_content_stamp in = { 0 };
    int i;

    if (index < 0 || index >= EXA_NUM_PREPARE_INDICES)
        return;
    if (how == LORIE_CONTENT_OPEN) {
        pvfb->contentOpen |= 1u << index;
        pvfb->contentBefore[index] = pvfb->contentFrom[index];
        if (!dest && priv->content.seq &&
            (!pvfb->contentFrom[index].seq || lorieContentNewer(priv->content.seq, pvfb->contentFrom[index].seq)))
            pvfb->contentFrom[index] = priv->content;
        return;
    }
    if (how == LORIE_CONTENT_FAILED)
        pvfb->contentFrom[index] = pvfb->contentBefore[index];
    else if (dest) {
        for (i = 0; i < EXA_NUM_PREPARE_INDICES; i++)
            if (pvfb->contentFrom[i].seq && (!in.seq || lorieContentNewer(pvfb->contentFrom[i].seq, in.seq)))
                in = pvfb->contentFrom[i];
        if (in.seq)
            lorieContentStore(pPix, &in, lorieMonotonicUs(), TRUE);
    }
    pvfb->contentOpen &= ~(1u << index);
    if (!pvfb->contentOpen) {
        memset(pvfb->contentFrom, 0, sizeof(pvfb->contentFrom));
        memset(pvfb->contentBefore, 0, sizeof(pvfb->contentBefore));
    }
}

// GPU copy `serial`'s damage reported (patches/xserver.patch: as it is queued, or - TERMUX_X11_EXA_COMPOSITE_OPT=1 - once it
// is done or its presentation scrapped while it may still run): noted with its queue slot.
void lorieContentDamaged(uint64_t serial) {
    uint32_t i;
    if (!pvfb->contentDiag)
        return;
    for (i = 0; i < LORIE_GPU_COPY_QUEUE_CAPACITY; i++)
        if (pvfb->contentQueued[i].serial == serial)
            pvfb->contentQueued[i].damaged = TRUE;
}

/*
 * ... a source read, holding the shared lock (no renderer takes from the queue or lists a frame meanwhile): a GPU copy
 * into it still queued - not recorded yet, so not waited for by the access (a recorded one is in a frame in flight,
 * which is) - and read now: the read sees the content before it. Stale, should its damage be reported already (the
 * compositor told of content not there yet); pending, should it not (what it was told of is there); unknown, should
 * the slot's note be another copy's. The renderers record the queue's front in order: an entry whose two buffers are
 * listed in flight, the entries before it recorded too, is taken as recorded.
 */
static void lorieContentStaleRead(LoriePixmapPriv *priv, int index) {
    struct lorie_shared_server_state *st = pvfb->state;
    uint32_t i, end, slot;
    uint64_t id;
    Bool recorded = TRUE, queued = FALSE, stale = FALSE, unknown = FALSE;

    if (index == EXA_PREPARE_DEST || index == EXA_PREPARE_AUX_DEST || !priv->buffer)
        return;
    id = LorieBuffer_description(priv->buffer)->id;
    end = st->gpuCopyQueue.writeIndex;      // this thread's own
    for (i = st->gpuCopyQueue.readIndex; i != end; i++) {
        const LorieGpuCopyEntry *e = &st->gpuCopyQueue.entries[i % LORIE_GPU_COPY_QUEUE_CAPACITY];
        if (e->cancelled)
            continue;
        if (recorded && lorieGpuInFlight(st, e->srcBufferId) && lorieGpuInFlight(st, e->dstBufferId))
            continue;
        recorded = FALSE;
        if (e->dstBufferId != id)
            continue;
        slot = i % LORIE_GPU_COPY_QUEUE_CAPACITY;
        queued = TRUE;
        if (pvfb->contentQueued[slot].serial != e->serial)
            unknown = TRUE;
        else if (pvfb->contentQueued[slot].damaged)
            stale = TRUE;
    }
    if (stale)
        __atomic_fetch_add(&st->diag.contentStaleReads, 1, __ATOMIC_RELAXED);
    else if (unknown)
        __atomic_fetch_add(&st->diag.contentUnknownReads, 1, __ATOMIC_RELAXED);
    else if (queued)
        __atomic_fetch_add(&st->diag.contentPendingReads, 1, __ATOMIC_RELAXED);
}

// The screen pixmap's newest content published with it, for the renderer's frames (lorie.h struct lorie_content).
static void lorieContentPublish(LoriePixmapPriv *priv) {
    volatile struct lorie_content *c = &pvfb->state->content;
    uint64_t now;
    if (!pvfb->contentDiag || !priv || !priv->content.seq || priv->content.seq == pvfb->contentPublishedSeq)
        return;
    now = lorieMonotonicUs();
    pvfb->contentPublishedSeq = priv->content.seq;
    c->seq = priv->content.seq;
    c->requestUs = priv->content.requestUs;
    c->copyUs = priv->content.copyUs;
    c->rootUs = priv->content.readyUs;
    c->publishUs = now;
    if (!++pvfb->contentGen)
        ++pvfb->contentGen;
    __atomic_store_n(&c->gen, pvfb->contentGen, __ATOMIC_RELEASE);
    __atomic_fetch_add(&pvfb->state->diag.contentRootPublishes, 1, __ATOMIC_RELAXED);
    lorieHistAdd(&pvfb->state->diag.contentRootToPublish, now >= priv->content.readyUs ? now - priv->content.readyUs : 0);
}

/*
 * P11-C3-D1 (TERMUX_X11_RENDERER_VK_FRAME_DIAG=1): what a RENDER picture's pixels are - none of a drawable's (a solid
 * fill or a gradient), the screen pixmap's (the root's, or a window's drawn into it), an AHB's, or this process's own
 * memory - and a composite's class from its operator, its source's and destination's, its source's alpha, its mask's
 * kind (alpha only, colour, component alpha) and whether a transform is set.
 */
enum { LORIE_PICT_SOLID, LORIE_PICT_MEM, LORIE_PICT_AHB, LORIE_PICT_ROOT };
static uint32_t lorieCompositePict(PicturePtr pict) {
    DrawablePtr d = pict->pDrawable;
    PixmapPtr pix;
    if (!d)
        return LORIE_PICT_SOLID;
    pix = d->type == DRAWABLE_WINDOW ? d->pScreen->GetWindowPixmap((WindowPtr) d) : (PixmapPtr) d;
    if (pix == pScreenPtr->devPrivate)
        return LORIE_PICT_ROOT;
    return lorieIsAhb(exaGetPixmapDriverPrivate(pix)) ? LORIE_PICT_AHB : LORIE_PICT_MEM;
}

static uint32_t lorieCompositeKey(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst) {
    uint32_t m = !mask ? 0 : mask->componentAlpha ? 3 : PICT_FORMAT_RGB(mask->format) ? 2 : 1;
    return op | lorieCompositePict(src) << 8 | (uint32_t) (PICT_FORMAT_A(src->format) > 0) << 10 | lorieCompositePict(dst) << 11 |
           m << 13 | (uint32_t) (src->transform || (mask && mask->transform)) << 15;
}

static void lorieCompositeName(char *out, size_t size, uint32_t key) {
    static const char *const ops[] = { "clear", "src", "dst", "over", "overrev", "in", "inrev", "out", "outrev", "atop",
                                       "atoprev", "xor", "add", "saturate" };
    static const char *const kinds[] = { "solid", "mem", "ahb", "root" }, *const masks[] = { "", "+mask_a", "+mask_rgb", "+mask_ca" };
    uint32_t op = key & 0xff;
    if (op < sizeof(ops) / sizeof(ops[0]))
        snprintf(out, size, "%s:%s_%s>%s%s%s", ops[op], kinds[key >> 8 & 3], key >> 10 & 1 ? "a" : "x", kinds[key >> 11 & 3],
                 masks[key >> 13 & 3], key >> 15 & 1 ? "+xf" : "");
    else
        snprintf(out, size, "op%u:%s_%s>%s%s%s", op, kinds[key >> 8 & 3], key >> 10 & 1 ? "a" : "x", kinds[key >> 11 & 3],
                 masks[key >> 13 & 3], key >> 15 & 1 ? "+xf" : "");
}

/*
 * ... and a composite timed (wrapping PictureScreen's Composite, EXA's and what it calls below it): what it waited for
 * read from this thread's own counters - the GPU before a CPU access (xGpuWaitUs), the shared lock besides
 * (xStateLockWaitUs, which holds both), the root copy (rootCopyCpuBlockUs) - and its AHB CPU locks (xAhbCpuLockUs);
 * what is left is the composite's own work on the CPU (pixman's, EXA's). Counted in its class's slot, the period's
 * first LORIE_COMPOSITE_CLASSES classes each in one, any other in the last.
 */
static void lorieCompositeTimed(CARD8 op, PicturePtr src, PicturePtr mask, PicturePtr dst, INT16 xSrc, INT16 ySrc,
                                INT16 xMask, INT16 yMask, INT16 xDst, INT16 yDst, CARD16 width, CARD16 height) {
    PictureScreenPtr ps = GetPictureScreen(dst->pDrawable->pScreen);
    volatile struct lorie_renderer_diag *dg = &pvfb->state->diag;
    struct lorie_composite_class *c = &pvfb->compositeClasses[LORIE_COMPOSITE_CLASSES];
    uint32_t key = lorieCompositeKey(op, src, mask, dst), accesses = pvfb->xAccessesOpened, i;
    uint64_t gpu = dg->xGpuWaitUs, lock = dg->xStateLockWaitUs, root = dg->rootCopyCpuBlockUs, ahb = dg->xAhbCpuLockUs;
    uint64_t locks = dg->xAhbCpuAcquires, startUs = GetTimeInMicros(), us;

    ps->Composite = pvfb->composite;
    (*ps->Composite) (op, src, mask, dst, xSrc, ySrc, xMask, yMask, xDst, yDst, width, height);
    pvfb->composite = ps->Composite;
    ps->Composite = lorieCompositeTimed;

    us = GetTimeInMicros() - startUs;
    gpu = dg->xGpuWaitUs - gpu;
    lock = dg->xStateLockWaitUs - lock;
    lock = lock > gpu ? lock - gpu : 0;
    root = dg->rootCopyCpuBlockUs - root;
    ahb = dg->xAhbCpuLockUs - ahb;
    for (i = 0; i < LORIE_COMPOSITE_CLASSES; i++)
        if (!pvfb->compositeClasses[i].n || pvfb->compositeClasses[i].key == key) {
            c = &pvfb->compositeClasses[i];
            break;
        }
    c->key = key;   // the last slot's: its last one's
    c->n++;
    c->accesses += pvfb->xAccessesOpened - accesses;
    c->locks += (uint32_t) (dg->xAhbCpuAcquires - locks);
    c->px += (uint64_t) width * height;
    c->us += us;
    c->gpuUs += gpu;
    c->rootUs += root;
    c->lockUs += lock;
    c->ahbUs += ahb;
    if (us > c->maxUs)
        c->maxUs = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
    pvfb->composites++;
    if (us > pvfb->worstComposite.us) {
        pvfb->worstComposite.key = key;
        pvfb->worstComposite.us = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
        pvfb->worstComposite.gpuUs = (uint32_t) gpu;
        pvfb->worstComposite.rootUs = (uint32_t) root;
        pvfb->worstComposite.lockUs = (uint32_t) lock;
        pvfb->worstComposite.ahbUs = (uint32_t) ahb;
        pvfb->worstComposite.width = width;
        pvfb->worstComposite.height = height;
    }
}

// ... what is left of a composite's time for its own work, its waits and AHB CPU locks taken away.
static uint64_t lorieCompositeCpuUs(uint64_t us, uint64_t gpu, uint64_t root, uint64_t lock, uint64_t ahb) {
    uint64_t other = gpu + root + lock + ahb;
    return us > other ? us - other : 0;
}

/*
 * The period's composites as XlorieComposite reports them (into `totals`): their totals and the slowest; and, as
 * XlorieCompositeClasses does (into `classes`), the classes that took longest - LORIE_COMPOSITE_SHOWN of them,
 * "name=n/kpx/us/cpu_us/gpu_us/root_us/accesses/max_us" each (the last slot's as "other") - then how many classes more;
 * the period over.
 */
#define LORIE_COMPOSITE_SHOWN 4
static void lorieCompositeReport(char *totals, size_t totalsSize, char *classes, size_t classesSize) {
    struct lorie_composite_class *c = pvfb->compositeClasses, *shown[LORIE_COMPOSITE_SHOWN] = { 0 };
    uint64_t n = 0, px = 0, us = 0, gpu = 0, root = 0, lock = 0, ahb = 0;
    uint32_t accesses = 0, locks = 0, more = 0, i, k;
    size_t at = 0;
    char worst[48];
    static uint64_t composites;

    for (i = 0; i <= LORIE_COMPOSITE_CLASSES; i++) {
        struct lorie_composite_class *e = &c[i];
        if (!e->n)
            continue;
        n += e->n, px += e->px, us += e->us, gpu += e->gpuUs, root += e->rootUs, lock += e->lockUs, ahb += e->ahbUs;
        accesses += e->accesses, locks += e->locks;
        for (k = 0; k < LORIE_COMPOSITE_SHOWN && shown[k] && shown[k]->us >= e->us; k++)
            ;
        if (k == LORIE_COMPOSITE_SHOWN) {
            more++;
            continue;
        }
        if (shown[LORIE_COMPOSITE_SHOWN - 1])
            more++;
        memmove(&shown[k + 1], &shown[k], sizeof(shown[0]) * (LORIE_COMPOSITE_SHOWN - 1 - k));
        shown[k] = e;
    }
    classes[0] = 0;
    for (k = 0; k < LORIE_COMPOSITE_SHOWN && shown[k] && at < classesSize; k++) {
        struct lorie_composite_class *e = shown[k];
        char name[48];
        int w;
        if (e == &c[LORIE_COMPOSITE_CLASSES])
            snprintf(name, sizeof(name), "other");
        else
            lorieCompositeName(name, sizeof(name), e->key);
        w = snprintf(classes + at, classesSize - at, "%s%s=%u/%llu/%llu/%llu/%llu/%llu/%u/%u", k ? "," : "", name, e->n,
                     (unsigned long long) (e->px / 1000), (unsigned long long) e->us,
                     (unsigned long long) lorieCompositeCpuUs(e->us, e->gpuUs, e->rootUs, e->lockUs, e->ahbUs),
                     (unsigned long long) e->gpuUs, (unsigned long long) e->rootUs, e->accesses, e->maxUs);
        at += w > 0 ? (size_t) w : 0;
    }
    if (at < classesSize)
        snprintf(classes + at, classesSize - at, "%s classes_more=%u", n ? "" : "none", more);
    if (pvfb->worstComposite.us)
        lorieCompositeName(worst, sizeof(worst), pvfb->worstComposite.key);
    snprintf(totals, totalsSize, "composites=%llu(+%llu) px=%llu us=%llu cpu_us=%llu gpu_us=%llu root_us=%llu lock_us=%llu "
                                 "ahb_us=%llu accesses=%u ahb_locks=%u worst_us=%u worst=%s:%ux%u:cpu=%llu,gpu=%u,root=%u,lock=%u,ahb=%u",
             (unsigned long long) pvfb->composites, (unsigned long long) (pvfb->composites - composites), (unsigned long long) px,
             (unsigned long long) us, (unsigned long long) lorieCompositeCpuUs(us, gpu, root, lock, ahb), (unsigned long long) gpu,
             (unsigned long long) root, (unsigned long long) lock, (unsigned long long) ahb, accesses, locks,
             pvfb->worstComposite.us, pvfb->worstComposite.us ? worst : "none", pvfb->worstComposite.width,
             pvfb->worstComposite.height,
             (unsigned long long) lorieCompositeCpuUs(pvfb->worstComposite.us, pvfb->worstComposite.gpuUs, pvfb->worstComposite.rootUs,
                                                      pvfb->worstComposite.lockUs, pvfb->worstComposite.ahbUs),
             pvfb->worstComposite.gpuUs, pvfb->worstComposite.rootUs, pvfb->worstComposite.lockUs, pvfb->worstComposite.ahbUs);
    composites = pvfb->composites;
    memset(c, 0, sizeof(pvfb->compositeClasses));
    memset(&pvfb->worstComposite, 0, sizeof(pvfb->worstComposite));
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
    // P11-C2: a root copy between buffers that go is not the X server's to make any more (the renderer holds its own
    // references to them meanwhile); the request is still settled before the next one is made
    if (pvfb->rootCopy.seq && priv->buffer && (priv->buffer == pvfb->rootCopy.to || priv->buffer == pvfb->rootCopy.from ||
                                               priv->twin == pvfb->rootCopy.to || priv->twin == pvfb->rootCopy.from))
        pvfb->rootCopy.abandoned = TRUE;
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

/*
 * P11-C3-B1 (TERMUX_X11_EXA_LOCK_OPT=1, lorie.h cpuAccess), holding the shared lock: AHB `id` reserved for one CPU
 * access of this X server - never the root it publishes (rootWindowTextureID: the renderer samples it), nor with no
 * slot free (counted).
 */
static Bool lorieCpuReserve(uint64_t id) {
    struct lorie_shared_server_state *st = pvfb->state;
    if (!pvfb->exaLockOpt || id == st->rootWindowTextureID)
        return FALSE;
    if (st->cpuAccess.count >= LORIE_CPU_ACCESS_MAX) {
        __atomic_fetch_add(&st->diag.xAccessReserveFull, 1, __ATOMIC_RELAXED);
        return FALSE;
    }
    st->cpuAccess.ids[st->cpuAccess.count++] = id;
    __atomic_fetch_add(&st->diag.xAccessReserved, 1, __ATOMIC_RELAXED);
    return TRUE;
}

// ... let go, holding the shared lock (its CPU lock gone already): whether a renderer waits for it - to be woken by
// lorieCpuReleasedWake once the shared lock is let go.
static Bool lorieCpuRelease(uint64_t id) {
    struct lorie_shared_server_state *st = pvfb->state;
    uint32_t i;
    for (i = 0; i < st->cpuAccess.count && i < LORIE_CPU_ACCESS_MAX; i++)
        if (st->cpuAccess.ids[i] == id) {
            st->cpuAccess.ids[i] = st->cpuAccess.ids[--st->cpuAccess.count];
            break;
        }
    __atomic_fetch_add(&st->cpuAccess.releases, 1, __ATOMIC_RELEASE);
    return __atomic_exchange_n(&st->cpuAccess.waiter, 0, __ATOMIC_RELAXED) != 0;
}

static void lorieCpuReleasedWake(void) {
    __atomic_fetch_add(&pvfb->state->diag.copiesDeferredWakes, 1, __ATOMIC_RELAXED);
    syscall(SYS_futex, &pvfb->state->cpuAccess.releases, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    lorieWakeRenderer(rendererWake);
}

Bool loriePrepareAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    Bool perAccessAhb = lorieAhbPerAccess() && lorieIsAhb(priv);
    if (pvfb->frameDiag)
        pvfb->xAccessesOpened++;    // P11-C3-D1: for each composite's (lorieCompositeTimed)
    if (pvfb->contentDiag && priv)
        lorieContentAccess(pPix, priv, index, LORIE_CONTENT_OPEN);      // P11-C3-D0
    // P11-C2: the root's buffer not before the copy into it the GPU was asked for settled
    if (pvfb->rootCopy.seq && priv && priv->buffer == pvfb->rootCopy.to)
        lorieRootCopyWait(FALSE);
    if (lorieNeedsGpuLock(pPix, priv, index)) {
        uint64_t waitStartUs = perAccessAhb ? GetTimeInMicros() : 0;
        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
        pvfb->accessLocks++;
        if (pvfb->contentDiag && perAccessAhb)
            lorieContentStaleRead(priv, index);     // P11-C3-D0: before it waits for a frame using it
        // P11-C3: its wait for the lock, and its hold of it from here (lorieFinishAccess)
        if (perAccessAhb && pvfb->frameDiag) {
            priv->accessStartUs = GetTimeInMicros();
            lorieHistAdd(&pvfb->state->diag.xAccessLock, priv->accessStartUs - waitStartUs);
        }
        // PER_ACCESS: the renderer lets the lock go while the device runs a frame; not while it uses this one.
        if (perAccessAhb && !priv->locked && !priv->mem) {
            uint64_t gpuStartUs = GetTimeInMicros(), gpuUs;
            lorieGpuWaitIdle(pvfb->state, false, LorieBuffer_description(priv->buffer)->id);
            gpuUs = GetTimeInMicros() - gpuStartUs;
            __atomic_fetch_add(&pvfb->state->diag.xGpuWaitUs, gpuUs, __ATOMIC_RELAXED);
            if (pvfb->frameDiag)
                lorieHistAdd(&pvfb->state->diag.xAccessGpu, gpuUs);
        }
        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xStateLockWaitUs, GetTimeInMicros() - waitStartUs, __ATOMIC_RELAXED);
        // P11-C3-B1: no frame uses it now; reserved, no renderer will until FinishAccess lets it go - and the shared
        // lock is let go before the CPU lock is taken, for the whole of the CPU access
        if (perAccessAhb && lorieCpuReserve(LorieBuffer_description(priv->buffer)->id)) {
            priv->reserved = TRUE;
            if (priv->accessStartUs)
                priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);
            pvfb->accessLocks--;
            lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        }
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
            if (perAccessAhb) {
                priv->accessStartUs = 0;
                if (priv->reserved) {
                    Bool wake;
                    lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
                    wake = lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
                    priv->reserved = FALSE;
                    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
                    if (wake)
                        lorieCpuReleasedWake();
                } else {
                    pvfb->accessLocks--;
                    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
                }
            }
            if (pvfb->contentDiag)
                lorieContentAccess(pPix, priv, index, LORIE_CONTENT_FAILED);    // P11-C3-D0: as if never opened
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

    if (pvfb->contentDiag && priv)
        lorieContentAccess(pPix, priv, index, LORIE_CONTENT_DONE);      // P11-C3-D0

    // PER_ACCESS: the CPU lock goes before the shared one, so the renderer can never take the shared lock
    // and use the AHB while the X server still has it CPU-locked. 3c98ffa's order is kept otherwise.
    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        Bool reserved = priv->reserved, wake = FALSE;
        if (!priv->wasLocked) {
            uint64_t unlockStartUs = GetTimeInMicros();
            LorieBuffer_unlock(priv->buffer);
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuLockUs, GetTimeInMicros() - unlockStartUs, __ATOMIC_RELAXED);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        // P11-C3-B1: likewise the CPU lock goes before the reservation, let go under the shared lock taken again
        if (reserved) {
            uint64_t lockedUs;
            lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);
            lockedUs = priv->accessStartUs ? GetTimeInMicros() : 0;
            wake = lorieCpuRelease(LorieBuffer_description(priv->buffer)->id);
            priv->reserved = FALSE;
            if (lockedUs)
                priv->accessHoldUs += (uint32_t) (GetTimeInMicros() - lockedUs);
        } else if (priv->accessStartUs)
            priv->accessHoldUs = (uint32_t) (GetTimeInMicros() - priv->accessStartUs);
        if (priv->accessStartUs) {
            uint64_t us = GetTimeInMicros() - priv->accessStartUs;
            lorieHistAdd(&pvfb->state->diag.xAccess, us);
            lorieHistAdd(&pvfb->state->diag.xAccessHold, priv->accessHoldUs);
            if (us > pvfb->worstAccess.us) {
                pvfb->worstAccess.us = us > UINT32_MAX ? UINT32_MAX : (uint32_t) us;
                pvfb->worstAccess.width = pPix->drawable.width;
                pvfb->worstAccess.height = pPix->drawable.height;
                pvfb->worstAccess.index = index;
                pvfb->worstAccess.reserved = reserved;
            }
            priv->accessStartUs = 0;
        }
        if (!reserved)
            pvfb->accessLocks--;
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (wake)
            lorieCpuReleasedWake();
        return;
    }

    if (lorieNeedsGpuLock(pPix, priv, index)) {
        pvfb->accessLocks--;
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    }

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
