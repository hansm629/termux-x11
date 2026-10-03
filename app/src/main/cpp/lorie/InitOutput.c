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
#include <sys/syscall.h>
#include <linux/futex.h>
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
    DestroyWindowProcPtr DestroyWindow;

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
    lorieTrace(pvfb->state, LORIE_TRACE_REQUEST, (uint32_t) (target_msc - crtc_msc), crtc_msc);
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

// -output-backend auto|gpu-copy|root-direct: which final output path to use. Published to the
// renderer through the shared state so the choice can be made without rebuilding, and without
// changing the filtering preference, which is what the root-direct path otherwise keys off.
static uint8_t lorieOutputBackend = LORIE_OUTPUT_AUTO;

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
/*
 * One record per Choreographer callback, written once and never changed. Single producer (the
 * Choreographer thread), single consumer (the X server thread).
 *
 * This was a single stamp the callback overwrote. Each callback queues one lorieRedraw, so when the
 * X server fell behind, several of those ran back to back and every one of them read the newest
 * stamp: ticks that had happened in the past at different times were all dated to the last one, and
 * the ones after it were then pushed a period at a time past it, into the future - which is the time
 * Present reports to a client as when its frame was shown. Clamping that to "now plus a period"
 * limited how far off it could be, not whether it was right. With a record per tick, each one
 * keeps the time it actually happened.
 */
#define LORIE_VSYNC_RECORDS 16
static volatile uint64_t lorieVsyncRecordUs[LORIE_VSYNC_RECORDS];
static volatile uint32_t lorieVsyncProduced = 0;   // Choreographer thread only writes this

// Choreographer thread.
static void lorieRecordVsync(uint64_t stampUs) {
    uint32_t n = __atomic_load_n(&lorieVsyncProduced, __ATOMIC_RELAXED);

    lorieVsyncRecordUs[n % LORIE_VSYNC_RECORDS] = stampUs;
    __atomic_store_n(&lorieVsyncProduced, n + 1, __ATOMIC_RELEASE);
}

// X server thread only, from lorieRedraw onwards.
static uint32_t lorieVsyncConsumed = 0;
static uint64_t lorieVsyncUs = 0;       // when the vsync that current_msc counts happened
static uint64_t lorieVsyncPeriodUs = 16667;

/*
 * Takes the next tick, if there is one, and returns how many vsyncs current_msc has to move - 0 if
 * this call was not for a tick at all, more than 1 only if the X server fell so far behind that
 * records were overwritten before it read them. Those ticks did happen and are counted; only their
 * times are gone, and the newest surviving one is used for the lot.
 */
static uint32_t lorieAdvanceVsyncClock(void) {
    uint32_t produced = __atomic_load_n(&lorieVsyncProduced, __ATOMIC_ACQUIRE);
    uint32_t steps = 1, idx;
    uint64_t us;

    if (produced == lorieVsyncConsumed)
        return 0;

    if (produced - lorieVsyncConsumed > LORIE_VSYNC_RECORDS) {
        steps = produced - lorieVsyncConsumed;
        lorieVsyncConsumed = produced - 1;
        pvfb->state->presentStats.vsyncRecordsLost += steps - 1;
    }

    idx = lorieVsyncConsumed;
    us = lorieVsyncRecordUs[idx % LORIE_VSYNC_RECORDS];
    // The producer could only have overwritten this slot by lapping the whole ring while it was
    // being read. Not a practical case, but a torn read would date a tick wrongly, so it is checked.
    if (__atomic_load_n(&lorieVsyncProduced, __ATOMIC_ACQUIRE) - idx > LORIE_VSYNC_RECORDS) {
        lorieVsyncConsumed = __atomic_load_n(&lorieVsyncProduced, __ATOMIC_ACQUIRE) - 1;
        idx = lorieVsyncConsumed;
        us = lorieVsyncRecordUs[idx % LORIE_VSYNC_RECORDS];
    }
    lorieVsyncConsumed = idx + 1;

    // Only plausible gaps feed the period estimate: a tick that was never called for stretches it.
    // 4-40 ms covers everything from 25 to 250 Hz.
    if (lorieVsyncUs && us > lorieVsyncUs) {
        uint64_t delta = (us - lorieVsyncUs) / steps;

        if (delta >= 4000 && delta <= 40000)
            lorieVsyncPeriodUs = (lorieVsyncPeriodUs * 7 + delta) / 8;
    }
    lorieVsyncUs = us;
    return steps;
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

/*
 * How many copies into the drawing slot over an area it owes can be unresolved at once (see
 * rootReplacing). The renderer fences a batch of at most a queue's worth while the X server fills the
 * queue again, so no more than two queues' worth is ever outstanding. Past that a copy is refused and
 * the CPU draws it, which needs no record at all.
 */
#define LORIE_ROOT_REPLACEMENTS (2 * LORIE_GPU_COPY_QUEUE_CAPACITY)

// A copy into a root slot over an area that slot owed, and that area (see rootReplacing, rootCond).
typedef struct {
    uint64_t serial;
    RegionRec region;
} LorieRootCopyMark;

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

    /*
     * Where a GPU copy has been queued into slot i but may not have landed. The handover reads the
     * slot it is publishing with the CPU, so that area cannot be copied forward yet: it would hand
     * on the pixels from before the copy and then clear the stale mark, and the copy would land in
     * the published slot afterwards where the next slot can never pick it up.
     *
     * Kept as a region, and paired with the highest serial queued into that slot, so the wait is
     * for a finite set of copies rather than for the slot to have none. Asking whether the slot had
     * any pending copy at all never became false while a client kept presenting into the root -
     * every frame queued another one - so the publish was held back frame after frame with the
     * screen frozen and every counter looking healthy.
     */
    RegionRec rootGpuPending[LORIE_ROOT_SLOTS];
    uint64_t rootGpuPendingSerial[LORIE_ROOT_SLOTS];

    /*
     * What the slot being drawn into still lacks, and where to get it. A handover that could not copy
     * an area forward - a queued copy had not landed in the slot being published yet - leaves that
     * area stale in the new drawing slot. It used to stay there as a bare stale mark, with nothing
     * recording who had the content: the new slot was drawn into, published and used as the source
     * for the next handover with the area still old, and the next slot's stale mark was cleared from
     * it - so the area went back to its previous content and the old copy spread from there.
     *
     * rootOwedDonor is the slot that has it, once rootOwedSerial - its own pending copies, and the
     * drawing slot's leftover ones the CPU copy must not race - has landed. There is only ever one
     * donor: a slot is not published while it owes anything outside rootReplacing (below), so the
     * obligation a handover creates is the only one outstanding. Anything the CPU draws into the area
     * in the meantime is newer than what the donor would supply, and lorieRootCpuDrawn takes it out.
     */
    RegionRec rootOwed;
    int rootOwedDonor;
    uint32_t rootOwedDonorEpoch;
    uint64_t rootOwedSerial;

    /*
     * Copies queued into the drawing slot over an area it still owes, by serial.
     *
     * Queuing one used to take its area out of rootOwed on the spot - the copy would replace it, so
     * there seemed no point fetching it from the donor. But queued is not made: a copy can still be
     * cancelled, find its source never arrived, or be given up on, and then the area kept its old
     * content with nothing left saying so. It went out like that, and spread to every slot after.
     *
     * So the area stays owed until the copy is known to have been made. While it is in flight the
     * area is left alone - copying the donor's content in could race the GPU write, or land the
     * older content on top of it - and the slot may go out with it still pending; rootCond takes
     * the question over from there.
     */
    LorieRootCopyMark rootReplacing[LORIE_ROOT_REPLACEMENTS];
    int rootReplacingCount;

    /*
     * Per slot, what rootReplacing still held when it went out: areas the slot only has if one of
     * those copies was made. Where none was, the slot's content there is the one from before, and
     * the content that belongs there is wherever the slot was going to get it from - rootCondDonor,
     * as it was when it went out (rootCondDonorEpoch; a slot drawn into since holds something else).
     * Kept until the slot is drawn into again, when what was not made is marked stale in it.
     */
    LorieRootCopyMark rootCond[LORIE_ROOT_SLOTS][LORIE_ROOT_REPLACEMENTS];
    int rootCondCount[LORIE_ROOT_SLOTS];
    int rootCondDonor[LORIE_ROOT_SLOTS];
    uint32_t rootCondDonorEpoch[LORIE_ROOT_SLOTS];
    /* Moves on whenever a slot becomes the drawing slot, so a reference to its earlier content can
     * tell that it is gone. */
    uint32_t rootEpoch[LORIE_ROOT_SLOTS];
    int rootWrite;                  // the slot we are drawing into; only this side ever changes it
    Bool rootDirty;                 // drawn into but not published yet, so the retry below knows
    /* How many open CPU accesses to this pixmap took the shared lock - recorded when they took it,
     * so FinishAccess gives back exactly what PrepareAccess took (see loriePrepareAccess). */
    int sharedLockDepth;
    uint64_t rootDirtySinceUs;      // when it became so, to measure how long a publish took
    Bool rootDouble;
} LoriePixmapPriv;

static void lorieCopyRootRegion(LoriePixmapPriv *priv, int from, int to, RegionPtr region);
static void lorieEnsureRootDoubleBuffer(PixmapPtr root);
static Bool lorieRootHandover(LoriePixmapPriv *priv);
static void lorieNoteRootPublished(LoriePixmapPriv *priv);
static inline int lorieRootSampledIndex(void);
static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region);
static void lorieRootCpuDrawn(LoriePixmapPriv *priv, RegionPtr region);
static Bool lorieRootCanQueueCopy(LoriePixmapPriv *priv);
static void lorieRootNoteGpuCopy(LoriePixmapPriv *priv, RegionPtr r, uint64_t serial);

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
    LoriePixmapPriv *rootPriv = pScreenPtr ? LORIE_PIXMAP_PRIV_FROM_PIXMAP((PixmapPtr) pScreenPtr->devPrivate) : NULL;

    pvfb->state->drawRequested = pvfb->state->cursor.updated = true;
    lorieSendSharedServerState(pvfb->stateFd);
    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    /*
     * Every slot of a double-buffered root, not just the one being drawn into. The slots are only
     * registered when they are created, and a renderer that connects after that - the app restarted
     * or crashed while this server kept running - was sent the drawing slot and nothing else. The
     * renderer samples the slot published last, which is a different one, so it waited for a buffer
     * that was never coming and the screen stayed black for as long as this server lived.
     * Registering an id that is already registered is a no-op.
     */
    if (rootPriv && rootPriv->rootDouble) {
        int i;

        for (i = 0; i < LORIE_ROOT_SLOTS; i++)
            lorieRegisterBuffer(rootPriv->rootBuf[i]);
    }
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

    if (strcmp(argv[i], "-output-backend") == 0 && i + 1 < argc) {
        if (strcmp(argv[i + 1], "gpu-copy") == 0)
            lorieOutputBackend = LORIE_OUTPUT_GPU_COPY;
        else if (strcmp(argv[i + 1], "root-direct") == 0)
            lorieOutputBackend = LORIE_OUTPUT_ROOT_DIRECT;
        else
            lorieOutputBackend = LORIE_OUTPUT_AUTO;
        return 2;
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

    // Not updated this time if the lock cannot be taken; the next cursor change tries again.
    if (!lorie_mutex_lock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid))
        return;
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

/*
 * Writes trace records out to the file TERMUX_X11_TRACE names. X server thread only. Records are
 * taken in order and only once they are complete; one that a writer has lapped is counted as lost
 * rather than written half-overwritten. The file holds a 16-byte header ("LTR1", record size, start
 * time) and then fixed 24-byte records - tools/trace/analyze.py turns it into something readable.
 */
static FILE *lorieTraceFile = NULL;

static void lorieTraceOpen(void) {
    const char *path = getenv("TERMUX_X11_TRACE");
    struct { char magic[4]; uint32_t recordSize; uint64_t startUs; } header = { {'L', 'T', 'R', '1'}, 24, 0 };

    if (!path || !*path || !pvfb->state)
        return;
    if (!(lorieTraceFile = fopen(path, "wb"))) {
        log(ERROR, "TERMUX_X11_TRACE: cannot open %s: %s", path, strerror(errno));
        return;
    }
    header.startUs = lorieTraceNowUs();
    fwrite(&header, sizeof(header), 1, lorieTraceFile);
    pvfb->state->traceTail = __atomic_load_n(&pvfb->state->traceHead, __ATOMIC_ACQUIRE);
    __atomic_store_n(&pvfb->state->traceEnabled, 1, __ATOMIC_RELEASE);
    log(INFO, "TERMUX_X11_TRACE: recording to %s", path);
}

static void lorieTraceFlush(Bool force) {
    static uint64_t lastUs = 0;
    uint64_t nowUs, head, tail;

    if (!lorieTraceFile)
        return;
    nowUs = lorieTraceNowUs();
    if (!force && nowUs - lastUs < 250000)
        return;
    lastUs = nowUs;

    head = __atomic_load_n(&pvfb->state->traceHead, __ATOMIC_ACQUIRE);
    tail = pvfb->state->traceTail;
    if (head - tail > LORIE_TRACE_RECORDS) {
        pvfb->state->traceDropped += (uint32_t) (head - tail - LORIE_TRACE_RECORDS);
        tail = head - LORIE_TRACE_RECORDS;
    }

    for (; tail != head; tail++) {
        LorieTraceRecord *r = &pvfb->state->trace[tail % LORIE_TRACE_RECORDS];
        struct { uint64_t tUs; uint32_t kind; uint32_t a; uint64_t b; } out;
        uint64_t seq = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);

        if (seq == 0 || seq < tail + 1)
            break;                  // claimed but not written yet: take it next time
        if (seq != tail + 1) {
            pvfb->state->traceDropped++;
            continue;               // lapped by a writer
        }
        out.tUs = __atomic_load_n(&r->tUs, __ATOMIC_RELAXED);
        out.kind = __atomic_load_n(&r->kind, __ATOMIC_RELAXED);
        out.a = __atomic_load_n(&r->a, __ATOMIC_RELAXED);
        out.b = __atomic_load_n(&r->b, __ATOMIC_RELAXED);
        // The seqlock read: the fields must be read before seq is looked at again, and an acquire
        // fence is what keeps them there - an acquire load of seq alone would not stop the field
        // reads from being satisfied after it. Pairs with the writer's release fence.
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&r->seq, __ATOMIC_RELAXED) != seq) {
            pvfb->state->traceDropped++;
            continue;               // overwritten while being read
        }
        fwrite(&out, sizeof(out), 1, lorieTraceFile);
    }
    pvfb->state->traceTail = tail;
    fflush(lorieTraceFile);
}

// Input thread. Only pointer and touch events: they are what a drag is made of.
void lorieTraceInput(uint32_t eventType) {
    if (pvfb->state)
        lorieTrace(pvfb->state, LORIE_TRACE_INPUT, eventType, 0);
}

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

    lorieReapAbandonedCopies();
    pvfb->state->outputBackend = lorieOutputBackend;
    {
        uint32_t steps = lorieAdvanceVsyncClock();

        // Only a tick moves the counter. It moved once per call, and a call is not a vsync.
        if (steps) {
            pvfb->current_msc += steps;
            lorieTrace(pvfb->state, LORIE_TRACE_TICK, steps, pvfb->current_msc);
            lorieTraceFlush(FALSE);
            loriePerformVblanks();
            pvfb->state->waitForNextFrame = false;
        }
    }

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
        if (priv->rootDouble) {
            lorieMarkRootStale(priv, DamageRegion(pvfb->damage));
            priv->rootDirty = TRUE;
        }

        DamageEmpty(pvfb->damage);
        if (lorieRootHandover(priv)) {
            priv->rootDirty = FALSE;
            lorieNoteRootPublished(priv);
        } else if (!priv->rootDirtySinceUs)
            priv->rootDirtySinceUs = lorieNowUs();
        pvfb->state->drawRequested = TRUE;
    }

    /*
     * A handover can decline - no slot free, or a GPU write still running into the one being
     * published - and the damage that produced the content has already been cleared by then. The
     * block above is only entered when there is fresh damage, so without this the content sat
     * unpublished until something else happened to dirty the root, which on a still desktop is
     * nothing at all.
     *
     * Retried here rather than by holding drawRequested high: this runs once per vsync, which is
     * both the next opportunity to show anything and the rate at which the conditions it is waiting
     * on change.
     */
    if (priv->rootDouble && priv->rootDirty && lorieRootHandover(priv)) {
        priv->rootDirty = FALSE;
        lorieNoteRootPublished(priv);
        pvfb->state->drawRequested = TRUE;
    }

    /*
     * A wait that is still running, kept apart from the finished ones above. A wait that never ends
     * has no endpoint to be measured at, and reporting only finished waits would show nothing at
     * all for exactly the case that matters most.
     *
     * Note what this does and does not say: it is the gap between the X server drawing into a root
     * slot and handing that slot on. It is not a measure of what reached the screen - the renderer
     * still has to pick the slot up and submit it, and the compositor still has to show it.
     */
    if (priv->rootDirty && priv->rootDirtySinceUs) {
        uint32_t ageUs = (uint32_t) (lorieNowUs() - priv->rootDirtySinceUs);

        if (ageUs > pvfb->state->presentStats.rootUnpublishedNowMaxUs)
            pvfb->state->presentStats.rootUnpublishedNowMaxUs = ageUs;
    } else if (!priv->rootDirty)
        priv->rootDirtySinceUs = 0;

    /*
     * outputRetryPending is the renderer saying it has content it could not put on screen. This is
     * the only thing that opens its vsync gate, and the condition here was new damage or a cursor
     * move - so a frame the renderer had to hold back waited for a signal that was never sent, and
     * on a still desktop it waited forever.
     */
    if (pvfb->state->drawRequested || pvfb->state->outputRetryPending ||
        pvfb->state->cursor.moved || pvfb->state->cursor.updated) {
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
    /*
     * Everything this reports is taken in one pass at the start, each counter read and zeroed in the
     * same atomic exchange, and the report is built from that copy. It used to read the live counters
     * line by line while the renderer kept adding to them and then zero them all at the end, so an
     * update landing between a read and its reset was lost, and two numbers on one line could come
     * from different moments. The renderer's side adds atomically to match. Fields that are values
     * rather than counters are copied as they are.
     */
    __typeof__(pvfb->state->presentStats) snap;
    int renderedFrames = __atomic_exchange_n(&pvfb->state->renderedFrames, 0, __ATOMIC_RELAXED);

    snap.coalescedFrames = __atomic_exchange_n(&pvfb->state->presentStats.coalescedFrames, 0, __ATOMIC_RELAXED);
    snap.copyAbandons = __atomic_exchange_n(&pvfb->state->presentStats.copyAbandons, 0, __ATOMIC_RELAXED);
    snap.copyCompletions = __atomic_exchange_n(&pvfb->state->presentStats.copyCompletions, 0, __ATOMIC_RELAXED);
    snap.copyDeferrals = __atomic_exchange_n(&pvfb->state->presentStats.copyDeferrals, 0, __ATOMIC_RELAXED);
    snap.copyForcedSettle = __atomic_exchange_n(&pvfb->state->presentStats.copyForcedSettle, 0, __ATOMIC_RELAXED);
    snap.copyLatencyMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.copyLatencyMaxUs, 0, __ATOMIC_RELAXED);
    snap.copyLatencySumUs = __atomic_exchange_n(&pvfb->state->presentStats.copyLatencySumUs, 0, __ATOMIC_RELAXED);
    snap.copyRecordExhausted = __atomic_exchange_n(&pvfb->state->presentStats.copyRecordExhausted, 0, __ATOMIC_RELAXED);
    snap.exaPreflightWaits = __atomic_exchange_n(&pvfb->state->presentStats.exaPreflightWaits, 0, __ATOMIC_RELAXED);
    snap.exaPreflightWaitUs = __atomic_exchange_n(&pvfb->state->presentStats.exaPreflightWaitUs, 0, __ATOMIC_RELAXED);
    snap.exaPreflightTimeouts = __atomic_exchange_n(&pvfb->state->presentStats.exaPreflightTimeouts, 0, __ATOMIC_RELAXED);
    snap.exaPreflightSkipped = __atomic_exchange_n(&pvfb->state->presentStats.exaPreflightSkipped, 0, __ATOMIC_RELAXED);
    snap.copyCancelledForCpuWrite = __atomic_exchange_n(&pvfb->state->presentStats.copyCancelledForCpuWrite, 0, __ATOMIC_RELAXED);
    snap.copyClaimedBeforeCpuWrite = __atomic_exchange_n(&pvfb->state->presentStats.copyClaimedBeforeCpuWrite, 0, __ATOMIC_RELAXED);
    snap.copyRequeues = __atomic_exchange_n(&pvfb->state->presentStats.copyRequeues, 0, __ATOMIC_RELAXED);
    snap.copySkips = __atomic_exchange_n(&pvfb->state->presentStats.copySkips, 0, __ATOMIC_RELAXED);
    snap.copyWaitHeld = __atomic_exchange_n(&pvfb->state->presentStats.copyWaitHeld, 0, __ATOMIC_RELAXED);
    snap.cursorOnlyFrames = __atomic_exchange_n(&pvfb->state->presentStats.cursorOnlyFrames, 0, __ATOMIC_RELAXED);
    snap.cursorOverlayMoves = __atomic_exchange_n(&pvfb->state->presentStats.cursorOverlayMoves, 0, __ATOMIC_RELAXED);
    snap.cursorUploadUs = __atomic_exchange_n(&pvfb->state->presentStats.cursorUploadUs, 0, __ATOMIC_RELAXED);
    snap.cursorUploads = __atomic_exchange_n(&pvfb->state->presentStats.cursorUploads, 0, __ATOMIC_RELAXED);
    snap.directBufferSubmits = __atomic_exchange_n(&pvfb->state->presentStats.directBufferSubmits, 0, __ATOMIC_RELAXED);
    snap.directHeldIncomplete = __atomic_exchange_n(&pvfb->state->presentStats.directHeldIncomplete, 0, __ATOMIC_RELAXED);
    snap.directReuseNoSubmit = __atomic_exchange_n(&pvfb->state->presentStats.directReuseNoSubmit, 0, __ATOMIC_RELAXED);
    snap.displayRefreshMHz = pvfb->state->presentStats.displayRefreshMHz;
    snap.fenceFallbacks = __atomic_exchange_n(&pvfb->state->presentStats.fenceFallbacks, 0, __ATOMIC_RELAXED);
    snap.fenceWaitMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.fenceWaitMaxUs, 0, __ATOMIC_RELAXED);
    snap.fenceWaitUs = __atomic_exchange_n(&pvfb->state->presentStats.fenceWaitUs, 0, __ATOMIC_RELAXED);
    snap.flushUs = __atomic_exchange_n(&pvfb->state->presentStats.flushUs, 0, __ATOMIC_RELAXED);
    snap.frameSamples = __atomic_exchange_n(&pvfb->state->presentStats.frameSamples, 0, __ATOMIC_RELAXED);
    snap.frameSumUs = __atomic_exchange_n(&pvfb->state->presentStats.frameSumUs, 0, __ATOMIC_RELAXED);
    snap.glOutputSubmitFailures = __atomic_exchange_n(&pvfb->state->presentStats.glOutputSubmitFailures, 0, __ATOMIC_RELAXED);
    snap.glOutputSubmits = __atomic_exchange_n(&pvfb->state->presentStats.glOutputSubmits, 0, __ATOMIC_RELAXED);
    snap.gpuCopyFrames = __atomic_exchange_n(&pvfb->state->presentStats.gpuCopyFrames, 0, __ATOMIC_RELAXED);
    snap.lockHeldUs = __atomic_exchange_n(&pvfb->state->presentStats.lockHeldUs, 0, __ATOMIC_RELAXED);
    snap.lockWaitMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.lockWaitMaxUs, 0, __ATOMIC_RELAXED);
    snap.lockWaitUs = __atomic_exchange_n(&pvfb->state->presentStats.lockWaitUs, 0, __ATOMIC_RELAXED);
    snap.longFrames = __atomic_exchange_n(&pvfb->state->presentStats.longFrames, 0, __ATOMIC_RELAXED);
    snap.maxFrameUs = __atomic_exchange_n(&pvfb->state->presentStats.maxFrameUs, 0, __ATOMIC_RELAXED);
    snap.pointerMoves = __atomic_exchange_n(&pvfb->state->presentStats.pointerMoves, 0, __ATOMIC_RELAXED);
    snap.presentCompletions = __atomic_exchange_n(&pvfb->state->presentStats.presentCompletions, 0, __ATOMIC_RELAXED);
    snap.presentGapMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.presentGapMaxUs, 0, __ATOMIC_RELAXED);
    snap.presentGapSumUs = __atomic_exchange_n(&pvfb->state->presentStats.presentGapSumUs, 0, __ATOMIC_RELAXED);
    snap.presentGapsLate = __atomic_exchange_n(&pvfb->state->presentStats.presentGapsLate, 0, __ATOMIC_RELAXED);
    snap.presentSubmits = __atomic_exchange_n(&pvfb->state->presentStats.presentSubmits, 0, __ATOMIC_RELAXED);
    snap.requestAheadMax = __atomic_exchange_n(&pvfb->state->presentStats.requestAheadMax, 0, __ATOMIC_RELAXED);
    snap.requestGapMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.requestGapMaxUs, 0, __ATOMIC_RELAXED);
    snap.requests = __atomic_exchange_n(&pvfb->state->presentStats.requests, 0, __ATOMIC_RELAXED);
    snap.rootCopies = __atomic_exchange_n(&pvfb->state->presentStats.rootCopies, 0, __ATOMIC_RELAXED);
    snap.rootCopyBytes = __atomic_exchange_n(&pvfb->state->presentStats.rootCopyBytes, 0, __ATOMIC_RELAXED);
    snap.rootCopyUs = __atomic_exchange_n(&pvfb->state->presentStats.rootCopyUs, 0, __ATOMIC_RELAXED);
    snap.rootOwedRepairs = __atomic_exchange_n(&pvfb->state->presentStats.rootOwedRepairs, 0, __ATOMIC_RELAXED);
    snap.rootReplacementsNotMade = __atomic_exchange_n(&pvfb->state->presentStats.rootReplacementsNotMade, 0, __ATOMIC_RELAXED);
    snap.rootOwedFromOlder = __atomic_exchange_n(&pvfb->state->presentStats.rootOwedFromOlder, 0, __ATOMIC_RELAXED);
    snap.rootOwedLost = __atomic_exchange_n(&pvfb->state->presentStats.rootOwedLost, 0, __ATOMIC_RELAXED);
    snap.rootReplacingFull = __atomic_exchange_n(&pvfb->state->presentStats.rootReplacingFull, 0, __ATOMIC_RELAXED);
    snap.rootPublishAttempts = __atomic_exchange_n(&pvfb->state->presentStats.rootPublishAttempts, 0, __ATOMIC_RELAXED);
    snap.rootPublishHeldForRepair = __atomic_exchange_n(&pvfb->state->presentStats.rootPublishHeldForRepair, 0, __ATOMIC_RELAXED);
    snap.rootPublishNoSlot = __atomic_exchange_n(&pvfb->state->presentStats.rootPublishNoSlot, 0, __ATOMIC_RELAXED);
    snap.rootPublishes = __atomic_exchange_n(&pvfb->state->presentStats.rootPublishes, 0, __ATOMIC_RELAXED);
    snap.rootRemapUs = __atomic_exchange_n(&pvfb->state->presentStats.rootRemapUs, 0, __ATOMIC_RELAXED);
    snap.rootRemaps = __atomic_exchange_n(&pvfb->state->presentStats.rootRemaps, 0, __ATOMIC_RELAXED);
    snap.rootStalePostponed = __atomic_exchange_n(&pvfb->state->presentStats.rootStalePostponed, 0, __ATOMIC_RELAXED);
    snap.rootUnpublishedMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.rootUnpublishedMaxUs, 0, __ATOMIC_RELAXED);
    snap.rootUnpublishedNowMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.rootUnpublishedNowMaxUs, 0, __ATOMIC_RELAXED);
    snap.submitGapMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.submitGapMaxUs, 0, __ATOMIC_RELAXED);
    snap.vsyncRecordsLost = __atomic_exchange_n(&pvfb->state->presentStats.vsyncRecordsLost, 0, __ATOMIC_RELAXED);
    snap.vsyncDispatchMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.vsyncDispatchMaxUs, 0, __ATOMIC_RELAXED);
    snap.xDispatchMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.xDispatchMaxUs, 0, __ATOMIC_RELAXED);
    snap.xLockWaitMaxUs = __atomic_exchange_n(&pvfb->state->presentStats.xLockWaitMaxUs, 0, __ATOMIC_RELAXED);
    snap.xLockWaitUs = __atomic_exchange_n(&pvfb->state->presentStats.xLockWaitUs, 0, __ATOMIC_RELAXED);
    snap.xLockWaits = __atomic_exchange_n(&pvfb->state->presentStats.xLockWaits, 0, __ATOMIC_RELAXED);
    snap.zeroCopyFenceErrors = __atomic_exchange_n(&pvfb->state->presentStats.zeroCopyFenceErrors, 0, __ATOMIC_RELAXED);
    snap.rootStaleSlotReleases = __atomic_exchange_n(&pvfb->state->presentStats.rootStaleSlotReleases, 0, __ATOMIC_RELAXED);
    snap.rootClaimsAcrossPools = __atomic_exchange_n(&pvfb->state->presentStats.rootClaimsAcrossPools, 0, __ATOMIC_RELAXED);
    snap.zeroCopyStalls = __atomic_exchange_n(&pvfb->state->presentStats.zeroCopyStalls, 0, __ATOMIC_RELAXED);


    if (!driverLogged && pvfb->state->rendererDriver[0]) {
        driverLogged = TRUE;
        log(INFO, "XlorieFrames: renderer GLES driver: %s", (const char *) pvfb->state->rendererDriver);
    }

    if (renderedFrames || gpuCopyAttempts)
        log(INFO, gpuCopyAttempts ? "%d frames in 5.0 seconds = %.1f FPS, %llu/%llu present copies offloaded to GPU"
                                   : "%d frames in 5.0 seconds = %.1f FPS",
            renderedFrames, ((float) renderedFrames) / 5,
            (unsigned long long) gpuCopyOffloads, (unsigned long long) gpuCopyAttempts);

    /*
     * The FPS above counts renderer redraws, which says nothing about how evenly they landed - a
     * high number with a visibly stuttering picture is exactly what long frames look like. These
     * come from the renderer through the shared state, since its own logcat is unreachable from
     * here.
     */
    samples = snap.frameSamples;
    if (samples) {
        log(INFO, "XlorieFrames: frame avg %.1f ms, max %.1f ms, hitches(>=%d ms) %u, "
                  "submit %.1f ms + fence wait %.1f ms total (worst wait %.1f ms), "
                  "copies on %u frames, coalesced %u, glFinish fallbacks %u",
            (double) snap.frameSumUs / samples / 1000.0,
            snap.maxFrameUs / 1000.0,
            LORIE_LONG_FRAME_US / 1000,
            snap.longFrames,
            snap.flushUs / 1000.0,
            snap.fenceWaitUs / 1000.0,
            snap.fenceWaitMaxUs / 1000.0,
            snap.gpuCopyFrames,
            snap.coalescedFrames,
            snap.fenceFallbacks);
        log(INFO, "XlorieLock: renderer held the root lock %.1f%% of the time (%.0f ms), "
                  "spent %.0f ms getting it (worst %.1f ms), "
                  "X server blocked on it %.0f ms over %u accesses (worst %.1f ms), "
                  "cursor-only frames %u (overlay %u) of %u pointer moves, display %.1f Hz",
            snap.lockHeldUs / 50000.0,
            snap.lockHeldUs / 1000.0,
            snap.lockWaitUs / 1000.0,
            snap.lockWaitMaxUs / 1000.0,
            snap.xLockWaitUs / 1000.0,
            snap.xLockWaits,
            snap.xLockWaitMaxUs / 1000.0,
            snap.cursorOnlyFrames,
            snap.cursorOverlayMoves,
            snap.pointerMoves,
            snap.displayRefreshMHz / 1000.0);
        if (snap.presentCompletions > 1) {
            log(INFO, "XloriePresent: %u client presents reached the screen in 5.0 s "
                      "(avg %.1f ms apart, longest %.1f ms, %u later than 33 ms)",
                snap.presentCompletions,
                snap.presentGapSumUs / 1000.0 /
                    (snap.presentCompletions - 1),
                snap.presentGapMaxUs / 1000.0,
                snap.presentGapsLate);
            log(INFO, "XloriePresent: %u submitted by clients (longest gap between submissions %.1f ms)",
                snap.presentSubmits,
                snap.submitGapMaxUs / 1000.0);
        }
        /*
         * Which path the frames in this window actually took, and why not the other one. The
         * renderer's own log says this too, but only in its process' logcat, which from the
         * terminal cannot be read - so a forced backend that turned out to be impossible looked
         * exactly like one that was working.
         *
         * Each number is its own counted event. Deriving the GL count as total minus direct made a
         * tick that submitted nothing into a GL frame, so a run that was entirely direct with a few
         * no-submit ticks reported most of its frames as having gone through GL.
         */
        {
            const char *asked = lorieOutputBackend == LORIE_OUTPUT_ROOT_DIRECT ? "root-direct"
                              : lorieOutputBackend == LORIE_OUTPUT_GPU_COPY ? "gpu-copy" : "auto";

            if (snap.rootStaleSlotReleases || snap.rootClaimsAcrossPools)
                log(INFO, "XlorieBackend: %u slot releases arrived after their pool was replaced and were "
                          "not applied to the new one; %u claims made again because the pool was "
                          "replaced while they were being made",
                    snap.rootStaleSlotReleases, snap.rootClaimsAcrossPools);
            if (snap.zeroCopyFenceErrors)
                log(INFO, "XlorieBackend: %u release fences could not be waited on; those slots "
                          "stay held until the pool is replaced",
                    snap.zeroCopyFenceErrors);

            log(INFO, "XlorieBackend: asked for %s, filtering %s%s; %u direct submits, %u nothing-new, "
                      "%u held incomplete, %u GL submits (%u failed), %u held for a buffer back%s%s",
                asked,
                pvfb->state->outputFilterNearest ? "nearest" : "linear",
                pvfb->state->outputFilterNearest && snap.directBufferSubmits
                    ? " (direct frames were scaled bilinearly regardless)" : "",
                snap.directBufferSubmits,
                snap.directReuseNoSubmit,
                snap.directHeldIncomplete,
                snap.glOutputSubmits,
                snap.glOutputSubmitFailures,
                snap.zeroCopyStalls,
                pvfb->state->outputBackendReason[0] ? "; not direct because: " : "",
                pvfb->state->outputBackendReason[0] ? (const char *) pvfb->state->outputBackendReason : "");
        }

        if (snap.requests)
            log(INFO, "XlorieRequest: %u arrived, longest gap between arrivals %.1f ms, furthest target +%u vsyncs",
                snap.requests,
                snap.requestGapMaxUs / 1000.0,
                snap.requestAheadMax);

        if (snap.copyCompletions)
            log(INFO, "XlorieCopy: %u copies took avg %.1f ms, longest %.1f ms, found unfinished %u times",
                snap.copyCompletions,
                snap.copyLatencySumUs / 1000.0 / snap.copyCompletions,
                snap.copyLatencyMaxUs / 1000.0,
                snap.copyRequeues);

        // How the copies that did not end in an ack ended instead. Both are invisible from outside
        // - an abandoned copy looks like a client that stopped sending, and a copy that was never
        // offered looks like the GPU path simply not being taken.
        // What putting queued copies ahead of the X server's own drawing costs. The wait total is the
        // number to compare against a run with TERMUX_X11_EXA_PREFLIGHT=0.
        // Each cancellation is a client frame dropped because the X server drew over the area it
        // would have landed in - newer content, but a frame the client will not see presented.
        if (snap.copyCancelledForCpuWrite || snap.copyClaimedBeforeCpuWrite)
            log(INFO, "XlorieCopy: %u queued copies cancelled for an overlapping CPU write, "
                      "%u already claimed by the renderer and waited out",
                snap.copyCancelledForCpuWrite, snap.copyClaimedBeforeCpuWrite);
        if (snap.exaPreflightWaits || snap.exaPreflightSkipped)
            log(INFO, "XlorieCopy: %u drawing operations waited %.1f ms in total for queued copies to "
                      "drain first (%u ran out of time, %u could not wait)",
                snap.exaPreflightWaits, snap.exaPreflightWaitUs / 1000.0,
                snap.exaPreflightTimeouts, snap.exaPreflightSkipped);
        if (snap.copyAbandons || snap.copyRecordExhausted ||
            snap.copyForcedSettle)
            log(INFO, "XlorieCopy: %u cancelled while still running, %u not offered (no tracking room), "
                      "%u let go without a result because their session ended",
                snap.copyAbandons,
                snap.copyRecordExhausted,
                snap.copyForcedSettle);

        if (snap.copyDeferrals || snap.copyWaitHeld ||
            snap.copySkips)
            log(INFO, "XloriePresent: copies waited %u times for a late buffer and %u times for a slot "
                      "still on screen; %u given up on or skipped",
                snap.copyDeferrals, snap.copyWaitHeld,
                snap.copySkips);
        if (snap.rootCopies || snap.rootStalePostponed)
            log(INFO, "XlorieRootCopy: %u copies, %.1f MB, %.1f ms, %u handovers left an area for later",
                snap.rootCopies,
                snap.rootCopyBytes / 1048576.0,
                snap.rootCopyUs / 1000.0,
                snap.rootStalePostponed);
        if (snap.rootPublishAttempts)
            log(INFO, "XlorieRootPublish: %u of %u attempts published, %u found no free slot, "
                      "%u held for a repair (%u areas repaired), "
                      "longest wait to publish %.1f ms (still waiting, worst so far %.1f ms)",
                snap.rootPublishes,
                snap.rootPublishAttempts,
                snap.rootPublishNoSlot,
                snap.rootPublishHeldForRepair,
                snap.rootOwedRepairs,
                snap.rootUnpublishedMaxUs / 1000.0,
                snap.rootUnpublishedNowMaxUs / 1000.0);
        if (snap.rootReplacementsNotMade || snap.rootOwedFromOlder || snap.rootOwedLost || snap.rootReplacingFull)
            log(INFO, "XlorieRootOwed: %u copies over an owed area not made, %u areas fetched from an "
                      "older slot, %u areas no slot still had, %u copies refused with too many in flight",
                snap.rootReplacementsNotMade,
                snap.rootOwedFromOlder,
                snap.rootOwedLost,
                snap.rootReplacingFull);
        log(INFO, "XlorieStall: root remap %.1f ms over %u frames, longest X server gap %.1f ms, "
                  "%u vsync times lost to backlog, vsync callback up to %.1f ms late",
            snap.rootRemapUs / 1000.0,
            snap.rootRemaps,
            snap.xDispatchMaxUs / 1000.0,
            snap.vsyncRecordsLost,
            snap.vsyncDispatchMaxUs / 1000.0);
        if (snap.cursorUploads)
            log(INFO, "XlorieLock: cursor image uploaded %u times, %.1f ms total",
                snap.cursorUploads,
                snap.cursorUploadUs / 1000.0);
    }


    gpuCopyAttempts = gpuCopyOffloads = 0;
    return 5000;
}

/*
 * Root damage, reported after each drawing operation (DamageSetReportAfterOp). What the CPU has drawn
 * is newer than whatever the drawing slot still owes there, so it comes out of the obligation at once:
 * at the next block handler would be too late, because a PrepareAccess in between brings the slot up
 * to date (lorieRepairRootOwed) and would copy the older content over it. After the operation, not
 * before, so that its own PrepareAccess has repaired the area first - an operation that reads what it
 * draws over then reads the right pixels.
 *
 * A GPU copy's damage arrives here as well, through lorieDamageGpuCopy, and is not drawing that has
 * happened: a queued copy may yet not be made (see rootReplacing).
 */
static Bool lorieReportingGpuCopyDamage = FALSE;

static void lorieRootDamaged(__unused DamagePtr pDamage, RegionPtr pRegion, __unused void *closure) {
    PixmapPtr screenPix;
    LoriePixmapPriv *priv;

    if (lorieReportingGpuCopyDamage || !pScreenPtr)
        return;
    screenPix = (*pScreenPtr->GetScreenPixmap)(pScreenPtr);
    priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(screenPix);
    if (priv && priv->rootDouble)
        lorieRootCpuDrawn(priv, pRegion);
}

// Damage for a GPU copy into a window, which writes the pixmap past the GC ops Damage hooks into.
void lorieDamageGpuCopy(DrawablePtr pDrawable, RegionPtr pRegion) {
    lorieReportingGpuCopyDamage = TRUE;
    DamageDamageRegion(pDrawable, pRegion);
    lorieReportingGpuCopyDamage = FALSE;
}

static DamagePtr lorieCreateRootDamage(ScreenPtr pScreen, PixmapPtr pixmap) {
    DamagePtr damage = DamageCreate(lorieRootDamaged, NULL, DamageReportRawRegion, TRUE, pScreen, NULL);

    if (!damage)
        FatalError("Couldn't setup damage\n");
    DamageSetReportAfterOp(damage, TRUE);
    DamageRegister(&pixmap->drawable, damage);
    return damage;
}

static Bool lorieCreateScreenResources(ScreenPtr pScreen) {
    pScreen->devPrivate = pScreen->CreatePixmap(pScreen, pScreen->width, pScreen->height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);

    pvfb->damage = lorieCreateRootDamage(pScreen, (*pScreen->GetScreenPixmap)(pScreen));
    pvfb->fpsTimer = TimerSet(NULL, 0, 5000, lorieFramecounter, pScreen);

    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    return TRUE;
}

typedef struct {
    struct xorg_list link;    /* only while waiting to be reaped */
    Bool inUse;

    /* Taken at enqueue, not looked up again later. The pixmap's current buffer rotates on, so
     * asking it at completion time asked about a different buffer than the one the work was given. */
    LorieBuffer *src, *dst;
    uint64_t serial;

    /* Which renderer connection was supposed to report this serial. The renderer is a separate
     * process that can be replaced while copies are outstanding - the activity is restarted, the
     * surface is lost - and nothing in a serial says whose answer it was waiting for. */
    uint32_t session;
    uint64_t settleByUs;   /* once its session has ended: how long to hold it (see lorieCopySettled) */

    /* Kept here rather than in a ring keyed by the serial: the ring aliased once more serials had
     * gone by than it had slots, so a cancelled copy could pick up a later copy's start time. */
    uint64_t startUs;

    /* What a cancelled present left behind. The pixmap reference and the idle fence are simply
     * held: dropping them while the GPU still reads the pixmap is the thing being avoided.
     *
     * The notify is separate, because telling the client its pixmap is free again is a statement
     * about the GPU, not about the request - holding a LorieBuffer reference keeps the object
     * alive, it does not stop the client writing that memory, and nothing orders a client's next
     * write against a copy the renderer has already submitted. A cancelled present owes that
     * notify and it is sent once the renderer is done; a present whose window is being torn down
     * owes nobody anything, and only the references need handing over, so idleWindow is left NULL.
     *
     * The window is a pointer, and lorieDestroyWindow() clears it before it can dangle. Holding an
     * XID and looking it up later meant trusting the XID still named the same window: X reuses
     * resource ids once the owning client has gone, so a record sitting here across a disconnect
     * could deliver a stale idle to whatever window inherited the id. */
    PixmapPtr heldPixmap;
    struct present_fence *heldFence;
    WindowPtr idleWindow;
    CARD32 idleSerial;
} LorieAbandonedCopy;

static struct xorg_list lorieAbandonedCopies = { &lorieAbandonedCopies, &lorieAbandonedCopies };

/* Bumped on each renderer connection, so a copy can tell whether the process that owed it an answer
 * is the one that is there now. 0 is "no renderer has ever connected". */
static uint32_t lorieRendererSession;

/* How long a copy is held once the connection that owed it an answer has gone and nothing has taken
 * its place. The thing being waited out is GPU work that renderer already submitted and that may
 * still be reading the source pixmap; that work is bounded by the driver's own timeout, on the
 * order of a second or two. So this is a bound on the wait, not a guess that the process has
 * exited - and if a new renderer connects first, that is the real answer and this never applies. */
#define LORIE_LOST_SESSION_SETTLE_US (2 * 1000 * 1000ULL)

// A deferred IdleNotify names the window it is owed to, and that window can be destroyed while the
// renderer is still reading the pixmap. Forgetting it here is what keeps the pointer from dangling,
// and what stops a stale idle reaching whatever later inherits the resource id.
static Bool lorieDestroyWindow(WindowPtr pWin) {
    ScreenPtr pScreen = pWin->drawable.pScreen;
    LorieAbandonedCopy *c;
    Bool ret;

    xorg_list_for_each_entry(c, &lorieAbandonedCopies, link)
        if (c->idleWindow == pWin)
            c->idleWindow = NULL;

    pScreen->DestroyWindow = pvfb->DestroyWindow;
    ret = pScreen->DestroyWindow ? pScreen->DestroyWindow(pWin) : TRUE;
    pScreen->DestroyWindow = lorieDestroyWindow;
    return ret;
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

    pvfb->damage = lorieCreateRootDamage(pScreen, newPixmap);

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

/*
 * The 64-bit frame callback, resolved at runtime: it is API 29 and minSdk is 26, the same reason the
 * SurfaceControl calls are resolved this way. Without it the callback is handed the frame time as a
 * `long`, which is 32 bits on the 32-bit ABIs this ships for and wraps every 4.3 seconds there, so
 * that path takes the time at callback entry instead.
 */
static void (*loriePostFrameCallback64)(AChoreographer *, void (*)(int64_t, void *), void *);
static void lorieChoreographerFrameCallback64(int64_t frameTimeNanos, void *data);

static void loriePostVsyncCallback(AChoreographer *d) {
    if (loriePostFrameCallback64)
        loriePostFrameCallback64(d, lorieChoreographerFrameCallback64, d);
    else
        AChoreographer_postFrameCallback(d, (AChoreographer_frameCallback) lorieChoreographerFrameCallback, d);
}

// Choreographer thread, from CmdEntryPoint.start.
void lorieChoreographerStart(AChoreographer *d) {
    loriePostFrameCallback64 = (void (*)(AChoreographer *, void (*)(int64_t, void *), void *))
        dlsym(RTLD_DEFAULT, "AChoreographer_postFrameCallback64");
    loriePostVsyncCallback(d);
}

/*
 * One vsync. frameUs is when the frame actually began according to the Choreographer, or 0 when that
 * is not available; the record then takes the time the callback ran, which is later by however long
 * the Choreographer thread took to get to it. That gap is reported separately rather than being
 * folded into the vsync time, because it is a property of this process's scheduling, not of the
 * display - and a frame time that is in the future, or older than any plausible delay, is not used.
 */
static void lorieVsyncTick(uint64_t frameUs) {
    uint64_t nowUs = lorieNowUs(), stampUs = nowUs;

    if (frameUs && frameUs <= nowUs && nowUs - frameUs < 1000000) {
        stampUs = frameUs;
        if (pvfb->state)
            LORIE_STAT_MAX(&pvfb->state->presentStats.vsyncDispatchMaxUs, (uint32_t) (nowUs - frameUs));
    }

    lorieRecordVsync(stampUs);
    if (pScreenPtr) {
        QueueWorkProc(lorieRedraw, NULL, NULL);
        lorieWakeServer();
    }
}

static void lorieChoreographerFrameCallback64(int64_t frameTimeNanos, void *data) {
    loriePostVsyncCallback((AChoreographer *) data);
    lorieVsyncTick(frameTimeNanos > 0 ? (uint64_t) frameTimeNanos / 1000u : 0);
}

void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d) {
    loriePostVsyncCallback(d);
    // t is deliberately unused: it is 32 bits on the 32-bit ABIs and wraps every 4.3 seconds there.
    lorieVsyncTick(0);
}

static Bool lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {
    static int eventFd = -1;
    pScreenPtr = pScreen;

    if (!lorieTraceFile)
        lorieTraceOpen();

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
    pvfb->DestroyWindow = pScreen->DestroyWindow;
    pScreen->DestroyWindow = lorieDestroyWindow;
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

/* A record is claimed before a copy is enqueued, never after. Claiming it at cancellation time
 * meant the claim could fail exactly when failing was unacceptable - the only thing left to do
 * would be to release buffers the GPU had not finished with. With the claim first, a copy that
 * cannot be tracked is simply not offered to the GPU, and the CPU path takes it. */
#define LORIE_COPY_RECORDS (LORIE_GPU_COPY_QUEUE_CAPACITY * 2)
static LorieAbandonedCopy lorieCopyRecords[LORIE_COPY_RECORDS];

static LorieAbandonedCopy *lorieTakeCopyRecord(void) {
    int i;

    for (i = 0; i < LORIE_COPY_RECORDS; i++)
        if (!lorieCopyRecords[i].inUse) {
            memset(&lorieCopyRecords[i], 0, sizeof(lorieCopyRecords[i]));
            lorieCopyRecords[i].inUse = TRUE;
            return &lorieCopyRecords[i];
        }
    return NULL;
}

static void lorieGiveBackCopyRecord(LorieAbandonedCopy *c) {
    c->inUse = FALSE;
}

Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,
                              uint64_t *out_serial, void **out_record) {
    LorieBuffer *srcBuffer, *dstBuffer;
    LoriePixmapPriv *priv;
    const LorieBuffer_Desc *desc, *dstDesc;
    LorieGpuCopyEntry *entry;
    LorieAbandonedCopy *record;
    uint64_t serial;
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
    // Acquire, pairing with the renderer's release when it hands a slot back: seeing the slot free
    // has to mean the renderer's reads of its previous entry are done before this overwrites it.
    readIndex = __atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE);
    if (writeIndex - readIndex >= LORIE_GPU_COPY_QUEUE_CAPACITY) {
        gpuCopyAttempts++;
        return FALSE;
    }

    // A copy into the root over an area the drawing slot still owes needs a record of its own until
    // it resolves (see rootReplacing). With no room for one it is not offered, and the CPU draws it.
    if (dst == pScreenPtr->devPrivate) {
        LoriePixmapPriv *rootPriv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(dst);

        if (rootPriv && rootPriv->rootDouble && !lorieRootCanQueueCopy(rootPriv)) {
            pvfb->state->presentStats.rootReplacingFull++;
            gpuCopyAttempts++;
            return FALSE;
        }
    }

    // Last thing that can refuse, and the last thing before any of this becomes visible to the
    // renderer. Without a record there is no way to account for the copy afterwards, and the one
    // moment that costs is cancellation - where the only remaining options are to release buffers
    // the GPU may still be reading, or to keep them forever. So a copy that cannot be tracked is
    // never offered: the caller falls back to the CPU, which needs no tracking at all.
    record = lorieTakeCopyRecord();
    if (!record) {
        pvfb->state->presentStats.copyRecordExhausted++;
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

    // The destination is identified by the buffer the copy was actually aimed at, root or not.
    // Naming the root with a NULL meant the work could not be matched back to the slot it targeted:
    // by the time it finished the pixmap had rotated on to another one, so neither its pending mark
    // nor its lifetime could be undone against the right buffer. A root slot destroyed by a resize
    // now outlives the copy reading it, which is the point - the renderer looks buffers up by id,
    // and an id that has been withdrawn is not asked for again.
    if (!dstIsRoot)
        lorieRegisterBuffer(dstBuffer);
    LorieBuffer_acquire(dstBuffer);
    // Tracked so CPU reads of this pixmap only pay for the GPU lock (see lorieNeedsGpuLock) while a
    // GPU write into it can actually be in flight, instead of on every access.
    LorieBuffer_gpuCopyPendingInc(dstBuffer);

    // Taken before the entry is filled in, because the bookkeeping below needs to name this copy.
    serial = ++pvfb->gpuCopySerialCounter;
    {
        LoriePixmapPriv *rootPriv = dstIsRoot ? LORIE_PIXMAP_PRIV_FROM_PIXMAP(dst) : NULL;
        if (rootPriv && rootPriv->rootDouble) {
            /*
             * The copy lands in the buffer we are drawing into, so every other slot misses it too.
             *
             * In the destination's own coordinates, which is where the GPU writes. The three
             * coordinate spaces in play, for an update rect R that Present gave us:
             *
             *   p = the present's x_off/y_off      (offset within the window)
             *   w = the window's x/y               (window's origin on screen)
             *   o = the destination's screen_x/y   (destination pixmap's origin on screen)
             *   d = p + w - o                      (what arrives here as x_off/y_off)
             *
             *   the GPU writes      R + d
             *   the window's Damage R + p + w      (screen space; see lorieScheduleGpuCopyClipped)
             *   what is marked here R + d
             *
             * So for R = [10,20,74,84] presented at p = (0,0) into a window at w = (400,200) with
             * the root as destination, o = (0,0) and d = (400,200): the GPU writes and this marks
             * [410,220,474,284]. It is R + d, not R + p - marking the source-local rect recorded
             * the wrong area whenever either offset was nonzero, and the region carried between
             * slots then did not match the region that changed.
             */
            RegionRec r;

            if (update) {
                RegionNull(&r);
                RegionCopy(&r, update);
            } else
                RegionInit(&r, &fullBox, 1);
            RegionTranslate(&r, x_off, y_off);
            lorieRootNoteGpuCopy(rootPriv, &r, serial);
            RegionUninit(&r);
        }
    }
    // What the work was actually given, recorded now. Both were looked up again at completion time
    // from the pixmap's private, which by then could name a different buffer: the source pixmap's
    // buffer is replaced on resize and the root's slot rotates every frame, so the references
    // released were not always the ones taken here.
    record->src = srcBuffer;
    record->dst = dstBuffer;

    entry = &pvfb->state->gpuCopyQueue.entries[writeIndex % LORIE_GPU_COPY_QUEUE_CAPACITY];
    entry->serial = serial;
    entry->srcBufferId = desc->id;
    entry->dstBufferId = dstDesc->id;
    entry->xOff = x_off;
    entry->yOff = y_off;
    entry->numRects = (uint16_t) numRects;
    // Queued, before the release below publishes the entry, so the renderer can never pair the new
    // entry with the previous occupant's state.
    __atomic_store_n(&pvfb->state->gpuCopyQueue.entryState[writeIndex % LORIE_GPU_COPY_QUEUE_CAPACITY],
                     (uint32_t) LORIE_JOB_QUEUED, __ATOMIC_RELAXED);
    for (i = 0; i < numRects; i++)
        entry->rects[i] = (LorieGpuCopyRect) { box[i].x1, box[i].y1, box[i].x2, box[i].y2 };

    __atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE); // release-publish entry writes above
    pthread_cond_signal(rendererCond);

    record->serial = entry->serial;
    record->session = lorieRendererSession;
    lorieTrace(pvfb->state, LORIE_TRACE_ENQUEUE, dstIsRoot ? 1 : 0, entry->serial);
    record->startUs = lorieNowUs();
    *out_serial = entry->serial;
    *out_record = record;
    gpuCopyAttempts++;
    gpuCopyOffloads++;
    lorieNotePresentSubmitted();
    return TRUE;
}

/*
 * What became of one offloaded copy, asked as the two questions a caller actually has: may I treat
 * this as presented, and may I stop waiting.
 *
 * They used to be asked as lorieGpuCopyIsDone() && !lorieGpuCopyFailed(), and the second question
 * had no answer for a serial whose failure record had been pushed out of the short list the
 * renderer publishes. Absent from the list read the same as never failed, and completedSerial,
 * being a watermark, had by then stepped over it - so a copy the renderer had given up on was
 * acked as made and the client's pixmap released. failedLostUpTo says how far those losses reach,
 * which turns that case into "unknown", and unknown is settled as not made.
 */
static Bool lorieGpuCopyKnownNotMade(uint64_t serial) {
    uint32_t count = __atomic_load_n(&pvfb->state->gpuCopyQueue.failedCount, __ATOMIC_ACQUIRE);
    uint32_t i, n = min(count, LORIE_GPU_COPY_FAILED_SLOTS);

    for (i = 0; i < n; i++)
        if (pvfb->state->gpuCopyQueue.failedSerials[(count - 1 - i) % LORIE_GPU_COPY_FAILED_SLOTS] == serial)
            return TRUE;

    // Its record may have been one of the ones overwritten, and there is no way to tell from here
    // whether it said failed or nothing at all. Not made is the answer that cannot corrupt: the
    // present is scrapped instead of claimed, and the source stays held until the GPU is done.
    return serial <= __atomic_load_n(&pvfb->state->gpuCopyQueue.failedLostUpTo, __ATOMIC_ACQUIRE);
}

// The copy landed: safe to ack it and to tell the client its frame was presented.
Bool lorieGpuCopyMade(uint64_t serial) {
    return __atomic_load_n(&pvfb->state->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE) >= serial &&
           !lorieGpuCopyKnownNotMade(serial);
}

/*
 * The GPU has finished with this copy's buffers, whichever way it went - so they can be released,
 * and a present waiting on it can stop waiting. A question, not an event.
 *
 * Only the watermark answers it. This used to be "completedSerial has passed it, or it is known not
 * to have been made", and the second half is a different question: an outcome, not a statement
 * about the GPU. A skipped entry is reported as not made the moment it is drained, while earlier
 * entries of the same batch may still be running - and a serial whose failure record had been
 * overwritten counted as not made too, with nothing at all said about where the GPU was. Either one
 * released buffers the GPU could still be using.
 *
 * The renderer now advances the watermark past every entry it drains, skips included, and only
 * once the batch's fence has signalled, so the watermark alone is the complete answer. Whether the
 * copy was actually made is lorieGpuCopyMade().
 */
Bool lorieGpuCopyResolved(uint64_t serial) {
    return __atomic_load_n(&pvfb->state->gpuCopyQueue.completedSerial, __ATOMIC_ACQUIRE) >= serial;
}

// Called where a present is actually put back on the vblank queue to be asked again.
void lorieNoteGpuCopyRequeued(void) {
    pvfb->state->presentStats.copyRequeues++;
}

/*
 * Letting go of what a copy was using, which is a different thing from the request being over.
 *
 * present_vblank_scrap() and present_vblank_destroy() reach a vblank whose copy may still be in
 * flight, and used to ack it outright: the in-flight markers were cleared and the references
 * dropped while the GPU was still reading the source and writing the destination, and the present
 * was counted as one that had reached the screen. Cancelling a request says nothing about whether
 * the GPU has finished with the buffers it was given.
 */
static void lorieReleaseCopyResources(LorieBuffer *src, LorieBuffer *dst) {
    if (src) {
        LorieBuffer_gpuCopyPendingDec(src);
        LorieBuffer_release(src);
    }
    if (dst) {
        LorieBuffer_gpuCopyPendingDec(dst);
        LorieBuffer_release(dst);
    }
}


/*
 * Whether it is safe to let go of what this copy was using.
 *
 * The renderer having reported the serial is the only direct answer, and used to be the only one
 * asked - which left every copy of a connection that broke waiting forever for a report that could
 * not come. The other way it ends is that the process which held the imported buffers and submitted
 * the GPU work is gone, and a different renderer having connected since is exactly that: the
 * session number only moves when a connection is established.
 *
 * A socket error on its own is neither. It says the request channel is gone, not that the GPU
 * finished, and treating it as completion is what released buffers a still-living renderer could
 * still be reading. So a lost session without a replacement is held instead - for a bounded time,
 * because the records come from a small reserve and holding them all means no copy can be offered
 * at all, which is a visible slowdown rather than a corruption.
 */
static Bool lorieCopySettled(LorieAbandonedCopy *c) {
    // The shared result belongs to whichever renderer is connected now, so it only answers for that
    // renderer's work. It was consulted for every record: a new session advancing completedSerial
    // past an old job's serial made that job look completed by a process that had never seen it.
    if (c->session == lorieRendererSession)
        return lorieGpuCopyResolved(c->serial);

    if (!c->settleByUs || lorieNowUs() < c->settleByUs)
        return FALSE;

    /*
     * Not a statement that the GPU finished, and there is nothing available that would be one. The
     * session that owed this answer is gone and will never give it, and the alternative is holding
     * a live client's pixmap and its IdleNotify for the rest of the server's life.
     *
     * What is accepted is bounded: a renderer process that outlived its socket may still read a
     * source the client has since written, and draw a torn frame into output that is no longer on
     * screen. That is worse than a proof and better than hanging clients, so it is counted rather
     * than described as safe.
     */
    pvfb->state->presentStats.copyForcedSettle++;
    return TRUE;
}

// Lets go of what a cancelled present left with the copy, now that the renderer is done reading it.
static void lorieFinishHeldPresentResources(LorieAbandonedCopy *c) {
    // A NULL window is one that was destroyed while this waited, or a handover that owed no notify
    // in the first place - either way there is nobody left to tell, which is also what upstream
    // does for a present whose window goes away. The pixmap and the fence are ours regardless.
    if (c->idleWindow)
        present_pixmap_idle(c->heldPixmap, c->idleWindow, c->idleSerial, c->heldFence);
    if (c->heldFence)
        present_fence_destroy(c->heldFence);
    if (c->heldPixmap)
        dixDestroyPixmap(c->heldPixmap, c->heldPixmap->drawable.id);
}

// Hands back what a still-running copy is using, once the renderer is done with it. Called every
// frame and whenever the renderer reports progress.
void lorieReapAbandonedCopies(void) {
    LorieAbandonedCopy *c, *tmp;

    xorg_list_for_each_entry_safe(c, tmp, &lorieAbandonedCopies, link) {
        if (!lorieCopySettled(c))
            continue;
        xorg_list_del(&c->link);
        lorieReleaseCopyResources(c->src, c->dst);

        lorieFinishHeldPresentResources(c);

        lorieGiveBackCopyRecord(c);
    }
}

/*
 * The request is over but the GPU work may not be. Takes over what the copy is using rather than
 * dropping it, and does not count this as a present that reached the screen.
 *
 * Returns TRUE when the copy is still running: its resources are now owned here, and the caller
 * must hand the IdleNotify over too (lorieDeferPresentIdle) instead of sending it. FALSE means the
 * work has resolved and everything it held has been released, so the ordinary idle is correct.
 * There is no third answer - the record was secured before the copy was ever enqueued, so this
 * cannot fail to track a copy it is being asked about.
 */
/*
 * Tells the renderer not to bother with one queued copy, if it has not got to it yet.
 *
 * Not a cancellation: with a one-way flag the X server cannot tell whether the renderer had already
 * picked the entry up, so everything that depends on the copy possibly running still has to hold.
 * What this buys is the common case - the renderer skips it, reports the serial immediately, and the
 * pixmap a client is waiting to have back is returned a frame or two sooner.
 *
 * An atomic cancel is possible and is not done here. A per-job state that the renderer moves
 * QUEUED -> CLAIMED and the X server moves QUEUED -> CANCELLED with a compare-and-swap would tell the
 * X server which side won, without moving writeIndex back and without waiting on the renderer. It is
 * left out as a matter of scope: a won cancel only proves this job will not run, not that nothing
 * else in flight is still reading the same pixmap, so releasing on it needs per-buffer tracking
 * this code does not have yet. Run-to-completion is the choice being made, not the only one there is.
 */
/* Moves a queued entry to CANCELLED, if the renderer has not claimed it first. True if the cancel won,
 * in which case the entry will never run. */
static Bool lorieCancelQueuedEntry(uint32_t slot) {
    uint32_t expected = LORIE_JOB_QUEUED;

    return __atomic_compare_exchange_n(&pvfb->state->gpuCopyQueue.entryState[slot], &expected,
                                       (uint32_t) LORIE_JOB_CANCELLED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/*
 * A copy the X server has just cancelled will not be made, and that is known now, not only once the
 * renderer gets to it. If it was a replacement over an area the drawing slot owes, the area is owed
 * again from this moment - so the access that cancelled it finds the area brought up to date by its
 * PrepareAccess (lorieRepairRootOwed) rather than still old.
 */
static void lorieRootCopyCancelled(uint64_t serial) {
    PixmapPtr screenPix = pScreenPtr ? (*pScreenPtr->GetScreenPixmap)(pScreenPtr) : NULL;
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(screenPix);
    int i;

    if (!priv || !priv->rootDouble)
        return;
    for (i = 0; i < priv->rootReplacingCount; i++)
        if (priv->rootReplacing[i].serial == serial) {
            RegionUninit(&priv->rootReplacing[i].region);
            priv->rootReplacing[i] = priv->rootReplacing[--priv->rootReplacingCount];
            pvfb->state->presentStats.rootReplacementsNotMade++;
            return;
        }
}

static void lorieMarkQueuedCopySuperseded(uint64_t serial) {
    uint32_t readIndex = __atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE);
    uint32_t writeIndex = pvfb->state->gpuCopyQueue.writeIndex, i;

    for (i = readIndex; i != writeIndex; i++) {
        uint32_t slot = i % LORIE_GPU_COPY_QUEUE_CAPACITY;

        // By serial, which is unique: a slot that has been reused holds a different job, and this
        // cannot touch it. Losing the race means the renderer already has it, which is the
        // run-to-completion case lorieGpuCopyAbandon() still handles.
        if (pvfb->state->gpuCopyQueue.entries[slot].serial == serial) {
            if (lorieCancelQueuedEntry(slot))
                lorieRootCopyCancelled(serial);
            return;
        }
    }
}

/*
 * Whether any of a queued copy's rectangles, offset into the coordinates of the buffer they touch,
 * meets the region. A copy's rects are in its source's coordinates; it writes them at (xOff, yOff) in
 * its destination.
 */
static Bool lorieEntryTouches(const LorieGpuCopyEntry *e, int16_t dx, int16_t dy, RegionPtr region) {
    int i;

    for (i = 0; i < e->numRects && i < LORIE_GPU_COPY_MAX_RECTS; i++) {
        BoxRec b = { (short) (e->rects[i].x1 + dx), (short) (e->rects[i].y1 + dy),
                     (short) (e->rects[i].x2 + dx), (short) (e->rects[i].y2 + dy) };

        if (b.x1 < b.x2 && b.y1 < b.y2 && RegionContainsRect(region, &b) != rgnOUT)
            return TRUE;
    }
    return FALSE;
}

/*
 * The X server is about to write `region` of buffer `bufferId` with the CPU. Any copy queued before
 * now that writes into that area, or reads from it, has to be out of the way first: one that has not
 * run would otherwise run afterwards - an older frame landing on top of newer drawing, or a frame read
 * from pixels that have since been drawn over.
 *
 * Each such copy is cancelled if it is still queued. One the renderer has already claimed is being run
 * under the shared lock and is finished, fence and all, before that lock is released - so taking the
 * lock, which PrepareAccess does next, waits for it. Either way none of them can land after the write.
 * There is no waiting here and no time limit, so there is nothing that gives the ordering up.
 *
 * Copies that do not touch the area are left alone. Two windows share the root, and a write into one
 * says nothing about a frame queued for the other.
 */
static void lorieCancelConflictingCopies(uint64_t bufferId, RegionPtr region) {
    uint32_t readIndex = __atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE);
    uint32_t writeIndex = pvfb->state->gpuCopyQueue.writeIndex, i;

    for (i = readIndex; i != writeIndex; i++) {
        uint32_t slot = i % LORIE_GPU_COPY_QUEUE_CAPACITY;
        const LorieGpuCopyEntry *e = &pvfb->state->gpuCopyQueue.entries[slot];
        Bool conflict = (e->dstBufferId == bufferId && lorieEntryTouches(e, e->xOff, e->yOff, region)) ||
                        (e->srcBufferId == bufferId && lorieEntryTouches(e, 0, 0, region));

        if (!conflict)
            continue;
        if (lorieCancelQueuedEntry(slot)) {
            pvfb->state->presentStats.copyCancelledForCpuWrite++;
            lorieTrace(pvfb->state, LORIE_TRACE_CANCEL, 1, e->serial);
            lorieRootCopyCancelled(e->serial);
        } else if (__atomic_load_n(&pvfb->state->gpuCopyQueue.entryState[slot], __ATOMIC_ACQUIRE) == LORIE_JOB_CLAIMED)
            pvfb->state->presentStats.copyClaimedBeforeCpuWrite++;
    }
}

/*
 * EXA's single way into a CPU access (exaPrepareAccess, see xserver.patch), with the drawable still
 * known - the driver's PrepareAccess gets only the pixmap, and is skipped altogether when one pixmap is
 * both source and destination of the same operation.
 *
 * For a write, the area that can be written is bounded by the drawable: a window draws only within its
 * borderClip, a pixmap anywhere in itself. That bound is what is checked against queued copies.
 */
void lorieExaAccess(DrawablePtr pDrawable, PixmapPtr pPixmap, int index) {
    LoriePixmapPriv *priv;
    RegionRec region;

    if (index != EXA_PREPARE_DEST && index != EXA_PREPARE_AUX_DEST)
        return;
    if (!pvfb->state || !(priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pPixmap)) || !priv->buffer ||
        !LorieBuffer_hasGpuCopyPending(priv->buffer))
        return;   // nothing queued touches this buffer: the common case

    if (pDrawable->type == DRAWABLE_WINDOW) {
        RegionNull(&region);
        RegionCopy(&region, &((WindowPtr) pDrawable)->borderClip);
        // Screen coordinates to the pixmap's own.
        RegionTranslate(&region, -pPixmap->screen_x, -pPixmap->screen_y);
    } else {
        BoxRec all = { 0, 0, (short) pPixmap->drawable.width, (short) pPixmap->drawable.height };

        RegionInit(&region, &all, 1);
    }

    lorieCancelConflictingCopies(LorieBuffer_description(priv->buffer)->id, &region);
    RegionUninit(&region);
}

Bool lorieGpuCopyAbandon(void *token) {
    LorieAbandonedCopy *c = token;

    if (!c)
        return FALSE;

    lorieTrace(pvfb->state, LORIE_TRACE_RESOLVED, 0, c->serial);

    // Nobody wants this copy's result any more, so ask for it not to be made. The handover below
    // still happens either way: the mark can be missed, and a copy already under way still reads
    // the source it was given.
    lorieMarkQueuedCopySuperseded(c->serial);

    if (lorieGpuCopyResolved(c->serial)) {
        lorieReleaseCopyResources(c->src, c->dst);
        lorieGiveBackCopyRecord(c);
        return FALSE;
    }

    pvfb->state->presentStats.copyAbandons++;
    xorg_list_add(&c->link, &lorieAbandonedCopies);
    return TRUE;
}

/*
 * The connection to the renderer broke. Runs on the X server thread, because settling a copy idles
 * pixmaps and touches Present state; it used to run straight from the socket error on the input
 * thread, which is not allowed to touch either.
 *
 * Nothing is released here. A broken socket says the renderer will never report these serials - it
 * does not say its GPU work finished, and the old code took it as exactly that, releasing buffers a
 * renderer process that outlived its socket could still be reading. All this does is stop waiting
 * for a report that cannot come and start the bounded wait in lorieCopySettled(); new copies are
 * already refused while there is no connection, so nothing joins them in the meantime.
 */
/*
 * Marks every record of the session that has just ended, so lorieCopySettled() stops asking the
 * live renderer's result about work it never saw.
 *
 * settleByUs is when to stop waiting, not when the GPU is known to be done - see lorieCopySettled.
 */
static void lorieMarkSessionOver(uint32_t session, uint64_t settleByUs, const char *why) {
    LorieAbandonedCopy *c;
    unsigned held = 0;

    xorg_list_for_each_entry(c, &lorieAbandonedCopies, link)
        if (c->session == session && !c->settleByUs) {
            c->settleByUs = settleByUs;
            held++;
        }

    if (held)
        log(INFO, "renderer session %u ended (%s) with %u copies unreported", session, why, held);
}

/*
 * The connection to the renderer broke. Runs on the X server thread, because settling a copy idles
 * pixmaps and touches Present state; it used to run straight from the socket error on the input
 * thread, which is not allowed to touch either.
 *
 * Nothing is released here. A broken socket says the renderer will never report these serials - it
 * does not say its GPU work finished, and the old code took it as exactly that, releasing buffers a
 * renderer process that outlived its socket could still be reading. All this does is stop waiting
 * for a report that cannot come and start the bounded wait in lorieCopySettled(); new copies are
 * already refused while there is no connection, so nothing joins them in the meantime.
 */
void lorieNoteRendererLost(void) {
    lorieMarkSessionOver(lorieRendererSession, lorieNowUs() + LORIE_LOST_SESSION_SETTLE_US,
                         "socket closed");
    lorieReapAbandonedCopies();
}

/*
 * A renderer connected. Whatever the previous one had not reported, it is not going to: the process
 * that held the imported buffers and submitted the GPU work has been replaced. Those records are
 * marked over with no further wait - the wait exists for a session that has gone with nothing
 * taking its place, and that is no longer the case.
 */
void lorieNoteRendererConnected(void) {
    uint32_t previous = lorieRendererSession;

    lorieRendererSession++;
    if (previous)
        lorieMarkSessionOver(previous, lorieNowUs(), "replaced by a new renderer");
    lorieReapAbandonedCopies();
}

// For the input thread, so a socket error can name the session it belongs to.
uint32_t lorieRendererSessionId(void) {
    return lorieRendererSession;
}

// Ignores a lost event that has been overtaken by a new connection: it would otherwise mark the new
// session's records over and tear down the buffers it has just registered.
Bool lorieRendererSessionIsCurrent(uint32_t session) {
    return session == lorieRendererSession;
}

// Holds back the IdleNotify for a cancelled present whose copy is still running. Takes ownership of
// the pixmap reference and the idle fence; both are finished with when the copy resolves. Only ever
// called after lorieGpuCopyAbandon() answered TRUE, which is what makes the token valid here.
void lorieDeferPresentIdle(void *token, PixmapPtr pixmap, WindowPtr window, CARD32 serial,
                           struct present_fence *idleFence) {
    LorieAbandonedCopy *c = token;

    if (!c)
        return;
    c->heldPixmap = pixmap;
    c->heldFence = idleFence;
    c->idleWindow = window;
    c->idleSerial = serial;
}

// The same handover for a present that owes no IdleNotify - its window is being torn down, and
// upstream drops the pixmap there without notifying anyone. Only the references need holding, and
// the fence is destroyed rather than triggered, exactly as it would have been.
void lorieHoldPresentResources(void *token, PixmapPtr pixmap, struct present_fence *idleFence) {
    LorieAbandonedCopy *c = token;

    if (!c)
        return;
    c->heldPixmap = pixmap;
    c->heldFence = idleFence;
    c->idleWindow = NULL;
}

// The renderer made this copy and it landed. Releases exactly what the copy was given.
void lorieGpuCopyAck(void *token) {
    LorieAbandonedCopy *c = token;

    if (!c)
        return;

    lorieNotePresentCompleted();
    lorieTrace(pvfb->state, LORIE_TRACE_RESOLVED, 1, c->serial);

    if (c->startUs) {
        uint32_t latencyUs = (uint32_t) (lorieNowUs() - c->startUs);
        pvfb->state->presentStats.copyLatencySumUs += latencyUs;
        if (latencyUs > pvfb->state->presentStats.copyLatencyMaxUs)
            pvfb->state->presentStats.copyLatencyMaxUs = latencyUs;
        pvfb->state->presentStats.copyCompletions++;
    }

    lorieReleaseCopyResources(c->src, c->dst);
    lorieGiveBackCopyRecord(c);
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
        lorieRootCpuDrawn(priv, &all);
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
                RegionUninit(&rootPriv->rootGpuPending[i]);
                rootPriv->rootGpuPendingSerial[i] = 0;
                while (rootPriv->rootCondCount[i] > 0)
                    RegionUninit(&rootPriv->rootCond[i][--rootPriv->rootCondCount[i]].region);
                rootPriv->rootCondDonor[i] = -1;
                if (i == 0) {
                    RegionUninit(&rootPriv->rootOwed);
                    rootPriv->rootOwedDonor = -1;
                    rootPriv->rootOwedSerial = 0;
                    while (rootPriv->rootReplacingCount > 0)
                        RegionUninit(&rootPriv->rootReplacing[--rootPriv->rootReplacingCount].region);
                }
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
    size_t stride, copied = 0;
    uint64_t startUs;

    if (!src || !dst || nrects <= 0)
        return;

    startUs = lorieNowUs();

    d = LorieBuffer_description(priv->rootBuf[from]);
    stride = (size_t) d->stride * 4;

    for (i = 0; i < nrects; i++) {
        int x1 = max(0, box[i].x1), x2 = min((int) d->width, box[i].x2);
        int y1 = max(0, box[i].y1), y2 = min((int) d->height, box[i].y2), y;

        for (y = y1; x2 > x1 && y < y2; y++) {
            memcpy(dst + (size_t) y * stride + (size_t) x1 * 4,
                   src + (size_t) y * stride + (size_t) x1 * 4, (size_t) (x2 - x1) * 4);
            copied += (size_t) (x2 - x1) * 4;
        }
    }

    if (pvfb->state) {
        pvfb->state->presentStats.rootCopyBytes += copied;
        pvfb->state->presentStats.rootCopyUs += (uint32_t) (lorieNowUs() - startUs);
        pvfb->state->presentStats.rootCopies++;
    }
}

// The slot the renderer takes next: the one we published most recently.
static inline int lorieRootSampledIndex(void) {
    return (int) ((__atomic_load_n(&pvfb->state->rootHandover, __ATOMIC_ACQUIRE)
                   >> LORIE_ROOT_NEWEST_SHIFT) & LORIE_ROOT_NEWEST_MASK);
}

// Whether a GPU write into the slot the X server is drawing into may still be running. This used to
// be a single count of every outstanding root copy, which stays positive for as long as any client
// keeps presenting - so it said "in flight" about a buffer nothing was writing.
static Bool lorieRootWriteHasGpuCopyPending(void) {
    PixmapPtr screenPix = pScreenPtr ? (*pScreenPtr->GetScreenPixmap)(pScreenPtr) : NULL;
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(screenPix);

    return priv && LorieBuffer_hasGpuCopyPending(priv->buffer);
}

// Everything we draw lands only in the slot we are drawing into, so every other slot is behind by
// that region until it becomes our drawing target and the handover copies it forward. Says nothing
// about what the drawing slot itself owes: a GPU copy that has only been queued has not replaced
// anything yet (rootReplacing), and CPU drawing is reported on its own (lorieRootCpuDrawn).
static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region) {
    int i;

    for (i = 0; i < LORIE_ROOT_SLOTS; i++)
        if (i != priv->rootWrite)
            RegionUnion(&priv->rootStale[i], &priv->rootStale[i], region);
}

// What the CPU has drawn into the drawing slot is newer than anything owed for it, so it is no
// longer owed - copying the donor's content across now would put the older content back on top.
static void lorieRootCpuDrawn(LoriePixmapPriv *priv, RegionPtr region) {
    if (RegionNotEmpty(&priv->rootOwed))
        RegionSubtract(&priv->rootOwed, &priv->rootOwed, region);
}

/*
 * Settles the copies queued into the drawing slot over its owed area that have resolved. Made, the
 * area holds the copy's content and is owed no longer; not made, it stays owed and is fetched like
 * the rest of it. Neither follows from the copy having been queued.
 */
static void lorieRootSettleReplacements(LoriePixmapPriv *priv) {
    int i = 0;

    while (i < priv->rootReplacingCount) {
        if (!lorieGpuCopyResolved(priv->rootReplacing[i].serial)) {
            i++;
            continue;
        }
        if (lorieGpuCopyMade(priv->rootReplacing[i].serial))
            RegionSubtract(&priv->rootOwed, &priv->rootOwed, &priv->rootReplacing[i].region);
        else
            pvfb->state->presentStats.rootReplacementsNotMade++;
        RegionUninit(&priv->rootReplacing[i].region);
        priv->rootReplacing[i] = priv->rootReplacing[--priv->rootReplacingCount];
    }
}

// Whether a copy into the drawing slot can be queued: one landing over an owed area needs a record
// until it resolves, and a copy that could not be recorded must not be queued at all.
static Bool lorieRootCanQueueCopy(LoriePixmapPriv *priv) {
    lorieRootSettleReplacements(priv);
    return priv->rootReplacingCount < LORIE_ROOT_REPLACEMENTS;
}

/*
 * The X server's side of a copy queued into the drawing slot, in the slot's own coordinates: every
 * other slot misses it, the slot cannot be read forward over it until it lands, and over an area the
 * slot still owes it is recorded as a replacement that has yet to happen. The caller has made sure
 * there is room (lorieRootCanQueueCopy).
 */
static void lorieRootNoteGpuCopy(LoriePixmapPriv *priv, RegionPtr r, uint64_t serial) {
    int w = priv->rootWrite;
    RegionRec over;

    lorieMarkRootStale(priv, r);
    RegionUnion(&priv->rootGpuPending[w], &priv->rootGpuPending[w], r);
    priv->rootGpuPendingSerial[w] = serial;

    if (!RegionNotEmpty(&priv->rootOwed))
        return;

    RegionNull(&over);
    RegionIntersect(&over, &priv->rootOwed, r);
    if (!RegionNotEmpty(&over)) {
        RegionUninit(&over);
        return;
    }
    priv->rootReplacing[priv->rootReplacingCount].serial = serial;
    priv->rootReplacing[priv->rootReplacingCount].region = over;   // the record owns it from here
    priv->rootReplacingCount++;
}

/*
 * Copies `area` into the drawing slot from wherever its content actually is, starting at `slot` as
 * it was at `epoch`, and takes the area out of what the drawing slot owes.
 *
 * That is usually `slot` itself. It is not where `slot` went out with copies still pending over an
 * owed part of the area and none of them was made: `slot` then holds the content from before, and
 * the content that belongs there is wherever `slot` was going to get it from (rootCond). That can
 * repeat, a slot further back each time, until one has it. One that has been drawn into since no
 * longer does, and what no slot still holds is counted and left as the drawing slot has it.
 *
 * The one slot drawn into since that still does is the drawing slot itself, as it was just before
 * it became that: nothing changes it inside an area it owes - the handover does not carry into it,
 * and CPU drawing and a copy made over it both take it out of what is owed. Its content there is
 * already in place, and its rootCond from then is kept for this until it goes out again.
 *
 * Every copy asked about here has resolved: each was queued before the last copy into the donor,
 * and the caller has waited for that one (rootOwedSerial).
 */
static void lorieRootFetchOwed(LoriePixmapPriv *priv, RegionPtr area, int slot, uint32_t epoch) {
    RegionRec want, made, notMade, here;
    int steps, i;

    RegionNull(&want);
    RegionNull(&made);
    RegionNull(&notMade);
    RegionNull(&here);
    RegionCopy(&want, area);

    for (steps = 0; RegionNotEmpty(&want) && steps < LORIE_ROOT_SLOTS; steps++) {
        if (slot < 0 || priv->rootEpoch[slot] != epoch + (slot == priv->rootWrite))
            break;

        // An area any one of these copies was made over holds the newest of them; only an area every
        // one of them failed over still has what was there before.
        RegionEmpty(&made);
        RegionEmpty(&notMade);
        for (i = 0; i < priv->rootCondCount[slot]; i++) {
            RegionPtr into = lorieGpuCopyMade(priv->rootCond[slot][i].serial) ? &made : &notMade;
            RegionUnion(into, into, &priv->rootCond[slot][i].region);
        }
        RegionSubtract(&notMade, &notMade, &made);
        RegionIntersect(&notMade, &notMade, &want);

        RegionSubtract(&here, &want, &notMade);
        if (RegionNotEmpty(&here)) {
            if (slot != priv->rootWrite)
                lorieCopyRootRegion(priv, slot, priv->rootWrite, &here);
            if (steps)
                pvfb->state->presentStats.rootOwedFromOlder++;
        }
        RegionCopy(&want, &notMade);

        epoch = priv->rootCondDonorEpoch[slot];
        slot = priv->rootCondDonor[slot];
    }

    if (RegionNotEmpty(&want))
        pvfb->state->presentStats.rootOwedLost++;

    RegionSubtract(&priv->rootOwed, &priv->rootOwed, area);
    RegionSubtract(&priv->rootStale[priv->rootWrite], &priv->rootStale[priv->rootWrite], area);
    RegionUninit(&here);
    RegionUninit(&notMade);
    RegionUninit(&made);
    RegionUninit(&want);
}

/*
 * Brings the drawing slot up to date with what a handover had to leave behind, as far as it can by
 * now. Returns whether everything still owed is under copies in flight into the drawing slot itself
 * - which the slot may go out with, the question passing to rootCond - and so whether it may be
 * published.
 *
 * The CPU copy writes the drawing slot, which the renderer never reads, and reads slots whose pending
 * copies rootOwedSerial covers. It leaves alone any area a copy into the drawing slot is still in
 * flight over: it would race that GPU write, or land the older content on top of it.
 */
static Bool lorieRepairRootOwed(LoriePixmapPriv *priv) {
    RegionRec pending, rest;
    Bool clear;
    int i;

    if (!priv->rootDouble)
        return TRUE;

    lorieRootSettleReplacements(priv);
    if (!RegionNotEmpty(&priv->rootOwed))
        return TRUE;

    if (priv->rootOwedDonor < 0 || priv->rootOwedDonor == priv->rootWrite) {
        RegionEmpty(&priv->rootOwed);
        return TRUE;
    }

    RegionNull(&pending);
    for (i = 0; i < priv->rootReplacingCount; i++)
        RegionUnion(&pending, &pending, &priv->rootReplacing[i].region);

    RegionNull(&rest);
    RegionSubtract(&rest, &priv->rootOwed, &pending);
    if (RegionNotEmpty(&rest) && lorieGpuCopyResolved(priv->rootOwedSerial)) {
        lorieRootFetchOwed(priv, &rest, priv->rootOwedDonor, priv->rootOwedDonorEpoch);
        pvfb->state->presentStats.rootOwedRepairs++;
        RegionSubtract(&rest, &priv->rootOwed, &pending);
    }
    clear = !RegionNotEmpty(&rest);

    RegionUninit(&rest);
    RegionUninit(&pending);
    return clear;
}

/*
 * The drawing slot is going out: what is still pending over its owed area becomes the question
 * rootCond answers for it later, with the donor it was owed from.
 */
static void lorieRootKeepConditional(LoriePixmapPriv *priv, int slot) {
    int i, n = 0;

    // What it went out with last time, kept while it was being drawn into (lorieRootReuseSlot).
    while (priv->rootCondCount[slot] > 0)
        RegionUninit(&priv->rootCond[slot][--priv->rootCondCount[slot]].region);

    for (i = 0; i < priv->rootReplacingCount; i++) {
        RegionIntersect(&priv->rootReplacing[i].region, &priv->rootReplacing[i].region, &priv->rootOwed);
        if (RegionNotEmpty(&priv->rootReplacing[i].region))
            priv->rootCond[slot][n++] = priv->rootReplacing[i];
        else
            RegionUninit(&priv->rootReplacing[i].region);
    }
    priv->rootCondCount[slot] = n;
    priv->rootCondDonor[slot] = n ? priv->rootOwedDonor : -1;
    priv->rootCondDonorEpoch[slot] = priv->rootOwedDonorEpoch;
    priv->rootReplacingCount = 0;
    RegionEmpty(&priv->rootOwed);
}

/*
 * A slot is about to be drawn into again. Where it went out with copies pending over an owed area
 * and none of them is known to have been made, its content is not what it should be; marked stale,
 * the handover brings it up to date like any other area it lacks. After this its earlier content is
 * gone, which rootEpoch tells anything still referring to it - except inside what it now owes,
 * where it stays, so its rootCond is kept for lorieRootFetchOwed until the slot goes out again.
 */
static void lorieRootReuseSlot(LoriePixmapPriv *priv, int slot) {
    RegionRec made, notMade;
    int i;

    RegionNull(&made);
    RegionNull(&notMade);
    for (i = 0; i < priv->rootCondCount[slot]; i++) {
        RegionPtr into = lorieGpuCopyMade(priv->rootCond[slot][i].serial) ? &made : &notMade;
        RegionUnion(into, into, &priv->rootCond[slot][i].region);
    }
    RegionSubtract(&notMade, &notMade, &made);
    RegionUnion(&priv->rootStale[slot], &priv->rootStale[slot], &notMade);
    RegionUninit(&notMade);
    RegionUninit(&made);
    priv->rootEpoch[slot]++;
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
    int i, allocated, w, h, overlayGranted = 0;
    Bool overlayAsked;

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
    /*
     * TERMUX_X11_ROOT_OVERLAY_USAGE=1 asks for the slots to be allocated with COMPOSER_OVERLAY usage
     * as well, so the compositor may put the root straight on a hardware plane. Off unless asked for,
     * because the allocator may then choose memory that is slower for the CPU to write, and the X
     * server draws the root with the CPU - whether that costs more than it saves is for an A/B on the
     * device to say. What was actually granted is logged with the slot count below.
     */
    {
        const char *overlay = getenv("TERMUX_X11_ROOT_OVERLAY_USAGE");
        overlayAsked = overlay && !strcmp(overlay, "1");
    }

    for (allocated = 0; allocated < LORIE_ROOT_SLOTS; allocated++) {
        if (overlayAsked) {
            bool granted = false;

            priv->rootBuf[allocated] = LorieBuffer_allocateForComposer(w, h, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, &granted);
            if (granted)
                overlayGranted++;
        } else
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
        RegionInit(&priv->rootGpuPending[i], NULL, 0);
        priv->rootGpuPendingSerial[i] = 0;
        priv->rootCondCount[i] = 0;
        priv->rootCondDonor[i] = -1;
        priv->rootCondDonorEpoch[i] = 0;
        priv->rootEpoch[i] = 0;
        if (i)
            lorieCopyRootRegion(priv, 0, i, &all);
    }
    RegionUninit(&all);

    for (i = 0; i < LORIE_ROOT_SLOTS; i++)
        lorieRegisterBuffer(priv->rootBuf[i]);

    /*
     * The new ids go out inside a generation change (see rootHandover in lorie.h): marked odd before
     * the first id is written, so a claim that reads one of them can tell it straddled the change, and
     * even again in the reset word, which publishes slot 0 with nothing held. Kept to the stores
     * themselves - the renderer waits out an odd generation before claiming anything.
     */
    {
        uint32_t old = __atomic_load_n(&pvfb->state->rootHandover, __ATOMIC_ACQUIRE), changing, gen;

        do {
            gen = LORIE_ROOT_GEN(old) | 1u;
            changing = (old & ((1u << LORIE_ROOT_GEN_SHIFT) - 1u)) | (gen << LORIE_ROOT_GEN_SHIFT);
        } while (!__atomic_compare_exchange_n(&pvfb->state->rootHandover, &old, changing, false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
        // Orders the mark before the ids: a claim that reads a new id then sees the mark (or later).
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (i = 0; i < LORIE_ROOT_SLOTS; i++)
            __atomic_store_n(&pvfb->state->rootBufferIds[i], LorieBuffer_description(priv->rootBuf[i])->id,
                             __ATOMIC_RELAXED);
        // Published slot 0, none held, in the generation after the odd one.
        __atomic_store_n(&pvfb->state->rootHandover, ((gen + 1u) & 0xffffu) << LORIE_ROOT_GEN_SHIFT,
                         __ATOMIC_RELEASE);
    }

    RegionInit(&priv->rootOwed, NULL, 0);
    priv->rootOwedDonor = -1;
    priv->rootOwedDonorEpoch = 0;
    priv->rootOwedSerial = 0;
    priv->rootReplacingCount = 0;

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

    log(INFO, "Root window has %d BGRA buffers (%dx%d, composer overlay usage %s), first id %llu",
        LORIE_ROOT_SLOTS, w, h,
        !overlayAsked ? "not asked for" : overlayGranted == LORIE_ROOT_SLOTS ? "granted"
                      : overlayGranted ? "granted for some" : "not supported",
        (unsigned long long) pvfb->state->rootBufferIds[0]);
}

/*
 * The part of a slot that a queued GPU copy may not have written yet, and so must not be read
 * forward out of it.
 *
 * Emptied as soon as the last copy queued into the slot has resolved, which is what makes this a
 * finite wait: it is for the copies that were queued, not for the slot to be free of them. The
 * question used to be "does this slot have any pending copy", and with a client presenting into
 * the root every frame the answer was yes every frame.
 */
static RegionPtr lorieRootPendingGpuRegion(LoriePixmapPriv *priv, int slot) {
    if (priv->rootGpuPendingSerial[slot] &&
        lorieGpuCopyResolved(priv->rootGpuPendingSerial[slot])) {
        RegionEmpty(&priv->rootGpuPending[slot]);
        priv->rootGpuPendingSerial[slot] = 0;
    }
    return &priv->rootGpuPending[slot];
}

/*
 * Closes out a wait for the root to be published, at the point it actually ends.
 *
 * The duration used to be sampled once per vsync while the wait was still running, and the sample
 * that mattered - the one covering the last stretch - never happened: a handover that succeeded
 * cleared rootDirty first, so the measurement saw nothing outstanding and simply forgot when the
 * wait had started. A publish one tick after a failed attempt therefore recorded a longest wait of
 * zero.
 */
static void lorieNoteRootPublished(LoriePixmapPriv *priv) {
    if (priv->rootDirtySinceUs) {
        uint32_t waitedUs = (uint32_t) (lorieNowUs() - priv->rootDirtySinceUs);

        if (waitedUs > pvfb->state->presentStats.rootUnpublishedMaxUs)
            pvfb->state->presentStats.rootUnpublishedMaxUs = waitedUs;
        priv->rootDirtySinceUs = 0;
    }
}

// Hands the buffer we have just finished drawing to the renderer and takes another one. Does
// nothing at all while the renderer is still sampling every other slot, in which case we simply
// keep drawing into this one for another frame - that costs one frame of freshness and never a
// stall.
static Bool lorieRootHandover(LoriePixmapPriv *priv) {
    uint32_t old, new;
    int drawn = priv->rootWrite, next, i;
    RegionPtr unsafe, nextPending;
    RegionRec blocked, carry;

    if (!priv->rootDouble)
        return FALSE;

    pvfb->state->presentStats.rootPublishAttempts++;

    /*
     * A slot is not published while it still lacks something a previous handover left behind. It
     * was, which is how an area went back to old content: the slot went out with the area stale, and
     * the next handover then used it as the source and cleared the next slot's stale mark from it.
     *
     * This cannot bring back the freeze the region tracking replaced. That freeze came from the
     * slot always having a copy in flight; this waits only for a copy into the *donor* to land,
     * which is one frame's drain at most. And an owed area a copy into the drawing slot is in flight
     * over - a client that keeps presenting does that every frame - does not hold the publish back
     * at all: the slot goes out with it, and rootCond keeps track of where the content is should that
     * copy turn out not to have been made.
     */
    if (!lorieRepairRootOwed(priv)) {
        pvfb->state->presentStats.rootPublishHeldForRepair++;
        return FALSE;
    }

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
        if (next < 0) {
            /*
             * Every other slot is still marked as held, so there is nowhere to draw next. That can
             * simply be producing faster than the display can show it - but a slot whose release
             * fence never signalled, a completion callback that was dropped, a held bit left set by
             * a torn-down pool, and an outright leak all read exactly the same from here. So this
             * counts the symptom and does not name a cause.
             */
            pvfb->state->presentStats.rootPublishNoSlot++;
            return FALSE;
        }

        new = (old & ~(LORIE_ROOT_NEWEST_MASK << LORIE_ROOT_NEWEST_SHIFT) & ~LORIE_ROOT_COUNT_MASK)
            | ((uint32_t) drawn << LORIE_ROOT_NEWEST_SHIFT)
            | ((old + LORIE_ROOT_COUNT_STEP) & LORIE_ROOT_COUNT_MASK);   // never into the generation
        // Retry rather than give up: a failed swap only means the renderer took or released a slot
        // in between, which may well have freed a different one for us.
    } while (!__atomic_compare_exchange_n(&pvfb->state->rootHandover, &old, new, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));

    // `drawn` is out, possibly with copies still in flight over an area it owed; `next` is about to be
    // drawn into again, and what it went out with that did not happen is stale in it from here.
    lorieRootKeepConditional(priv, drawn);
    lorieRootReuseSlot(priv, next);

    /*
     * The copy below reads `drawn` and writes `next`, both with the CPU and outside both the shared
     * lock and EXA's PrepareAccess. Two areas cannot be copied yet:
     *
     *   - where a queued GPU copy into `drawn` has not landed: reading it gets the pixels from
     *     before, and the GPU then writes `drawn` where `next` can never pick it up;
     *   - where a GPU copy into `next` itself, left from when it was last the drawing slot, has not
     *     landed: the CPU write would race it, and it would then land old content over new.
     *
     * Those are left behind and owed to `next`, with `drawn` as the donor once both have landed.
     * Holding the whole publish back for them instead is what froze the screen under a client that
     * presented every frame.
     */
    unsafe = lorieRootPendingGpuRegion(priv, drawn);
    nextPending = lorieRootPendingGpuRegion(priv, next);
    RegionNull(&blocked);
    RegionUnion(&blocked, unsafe, nextPending);

    RegionNull(&carry);
    RegionCopy(&carry, &priv->rootStale[next]);
    RegionSubtract(&carry, &carry, &blocked);

    pvfb->state->presentStats.rootPublishes++;
    lorieTrace(pvfb->state, LORIE_TRACE_PUBLISH, drawn, pvfb->state->rootBufferIds[drawn]);

    lorieCopyRootRegion(priv, drawn, next, &carry);
    RegionIntersect(&priv->rootStale[next], &priv->rootStale[next], &blocked);

    // What `next` still lacks, `drawn` has - `drawn` owed nothing outside copies in flight into it,
    // or it would not have been published above, and for those rootCond[drawn] says where the content
    // is instead - so record where it is and what has to land before it can be copied.
    if (RegionNotEmpty(&priv->rootStale[next])) {
        uint64_t waitFor = max(priv->rootGpuPendingSerial[drawn], priv->rootGpuPendingSerial[next]);

        RegionCopy(&priv->rootOwed, &priv->rootStale[next]);
        priv->rootOwedDonor = drawn;
        priv->rootOwedDonorEpoch = priv->rootEpoch[drawn];
        priv->rootOwedSerial = waitFor;
        pvfb->state->presentStats.rootStalePostponed++;
    }

    RegionUninit(&carry);
    RegionUninit(&blocked);

    priv->rootWrite = next;
    priv->buffer = priv->rootBuf[next];
    priv->locked = priv->rootLocked[next];

    // It may have landed already.
    lorieRepairRootOwed(priv);
    return TRUE;
}

// Whether a CPU access to pPix could race a GPU write from the renderer, and so needs state->lock.
static inline __always_inline Bool lorieNeedsGpuLock(PixmapPtr pPix, LoriePixmapPriv *priv, int index) {
    if (pScreenPtr->GetScreenPixmap(pScreenPtr) == pPix) {
        // With a double buffered root the renderer never samples the buffer we draw into, so our
        // drawing does not have to wait for its fence at all. Only a GPU copy landing in our own
        // buffer still needs the lock.
        if (pvfb->state->rootDoubleBuffered)
            return lorieRootWriteHasGpuCopyPending() && !pvfb->root.legacyDrawing;

        return index == EXA_PREPARE_DEST ||
               (lorieRootWriteHasGpuCopyPending() && !pvfb->root.legacyDrawing);
    }
    return !pvfb->root.legacyDrawing && priv->buffer &&
           LorieBuffer_description(priv->buffer)->type == LORIEBUFFER_AHARDWAREBUFFER &&
           LorieBuffer_hasGpuCopyPending(priv->buffer);
}

/* How many CPU accesses currently hold the shared lock. X server thread only. */
static int lorieSharedLockHeld = 0;

/*
 * Called as the outermost EXA fallback begins - before any operand has been prepared, so before any
 * lock is held (see the EXA_PRE_FALLBACK hunk in xserver.patch).
 *
 * Not what keeps the ordering correct any more - lorieExaAccess() does that, without waiting, by
 * cancelling any queued copy the operation is about to draw over. What this adds is letting those
 * copies run first where the renderer can get to them in time: a copy that runs is a client frame
 * shown, a copy that is cancelled is a frame dropped. It also makes a CPU read see a queued copy's
 * result rather than the pixels from before it, which cancellation cannot do.
 *
 * It can only wait here, where nothing is held: inside PrepareAccess the lock usually already is
 * (EXA prepares a copy's source before its destination), and the renderer needs it to drain. Running
 * out of time, or skipping, now costs a dropped frame or a read one frame stale - never an older frame
 * landing over newer drawing.
 *
 * The operands are not known yet, so it waits for everything queued. The count and total wait are
 * reported, and TERMUX_X11_EXA_PREFLIGHT=0 turns it off for comparison.
 */
#define LORIE_PREFLIGHT_MAX_US 20000ULL
void lorieExaFallbackBegin(void) {
    static int enabled = -1;
    static uint64_t stuckSerial = 0;
    uint32_t target;
    uint64_t startUs, headSerial;

    if (enabled < 0) {
        const char *e = getenv("TERMUX_X11_EXA_PREFLIGHT");
        enabled = !(e && !strcmp(e, "0"));
    }
    if (!enabled || !pvfb->state)
        return;

    target = pvfb->state->gpuCopyQueue.writeIndex;
    if (__atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE) == target)
        return;   // nothing queued: the common case, two loads

    // Holding the lock already - an access opened outside a fallback - means the renderer cannot
    // drain until it is released. Waiting would only run out the clock.
    if (lorieSharedLockHeld > 0) {
        pvfb->state->presentStats.exaPreflightSkipped++;
        lorieTrace(pvfb->state, LORIE_TRACE_PREFLIGHT, LORIE_PREFLIGHT_SKIP_LOCKED, 0);
        return;
    }
    if (!lorieConnectionAlive() || !lorieRendererAvailable()) {
        lorieTrace(pvfb->state, LORIE_TRACE_PREFLIGHT, LORIE_PREFLIGHT_NO_RENDERER, 0);
        return;
    }

    // An entry that already made one of these waits run out is stuck - an import that has not
    // arrived, a slot still on screen - and will stay stuck for a while. Waiting on it again for
    // every operation would stall the X server once per operation; it is skipped until it moves.
    headSerial = pvfb->state->gpuCopyQueue.entries[
            __atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE) % LORIE_GPU_COPY_QUEUE_CAPACITY].serial;
    if (headSerial == stuckSerial) {
        pvfb->state->presentStats.exaPreflightSkipped++;
        lorieTrace(pvfb->state, LORIE_TRACE_PREFLIGHT, LORIE_PREFLIGHT_SKIP_STUCK, 0);
        return;
    }

    pthread_cond_signal(rendererCond);
    startUs = lorieNowUs();
    __atomic_fetch_add(&pvfb->state->gpuCopyQueue.readIndexWaiters, 1, __ATOMIC_ACQ_REL);
    for (;;) {
        uint32_t seen = __atomic_load_n(&pvfb->state->gpuCopyQueue.readIndex, __ATOMIC_ACQUIRE);
        uint64_t elapsedUs = lorieNowUs() - startUs;
        struct timespec left;

        if ((int32_t) (seen - target) >= 0)
            break;
        if (elapsedUs >= LORIE_PREFLIGHT_MAX_US) {
            stuckSerial = headSerial;
            pvfb->state->presentStats.exaPreflightTimeouts++;
            break;
        }
        /*
         * Asleep until readIndex changes from what was just read, rather than polled. The renderer's
         * own progress notification goes to this thread's event loop - which is what is waiting here,
         * so it cannot be used - but readIndex lives in memory both processes map, and a futex on it
         * works across them; the renderer wakes it when it advances readIndex and sees a waiter.
         * Re-checked after every return, so a spurious or missed wake costs only another loop.
         */
        left.tv_sec = 0;
        left.tv_nsec = (long) ((LORIE_PREFLIGHT_MAX_US - elapsedUs) * 1000u);
        syscall(__NR_futex, &pvfb->state->gpuCopyQueue.readIndex, FUTEX_WAIT, seen, &left, NULL, 0);
    }
    __atomic_fetch_sub(&pvfb->state->gpuCopyQueue.readIndexWaiters, 1, __ATOMIC_ACQ_REL);
    lorieTrace(pvfb->state, LORIE_TRACE_PREFLIGHT,
               stuckSerial == headSerial ? LORIE_PREFLIGHT_TIMEOUT : LORIE_PREFLIGHT_DRAINED,
               lorieNowUs() - startUs);

    pvfb->state->presentStats.exaPreflightWaits++;
    pvfb->state->presentStats.exaPreflightWaitUs += (uint32_t) (lorieNowUs() - startUs);
}

Bool loriePrepareAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    Bool tookSharedLock = FALSE;

    // The drawing slot may still lack an area a handover left behind. The X server reads the root as
    // well as writing it - moving a window is a copy from the root to itself - so it is brought up to
    // date here if the content has landed, before anything reads the old pixels. If it has not, the
    // access goes ahead: waiting here is waiting on the renderer from inside a PrepareAccess, which
    // can deadlock, and the handover still will not publish the slot until it is repaired.
    if (priv && priv->rootDouble)
        lorieRepairRootOwed(priv);

    if (lorieNeedsGpuLock(pPix, priv, index)) {
        /*
         * Queued copies that this access is about to draw over have already been cancelled or waited
         * for by lorieExaAccess(), which runs first with the drawable (and so the area) still known.
         * The lock taken below is what completes that: a copy the renderer had already claimed is
         * finished, fence and all, before the renderer lets go of it.
         */

        // This is where the X server's own drawing waits for the renderer to let go of the root
        // window. Timed because it is the whole cost of the renderer's lock occupancy as the X
        // server experiences it - a client's throughput drops by exactly this.
        uint64_t waitStartUs = lorieNowUs();
        // Without the lock there is no safe way to touch this buffer, so the access fails; EXA
        // handles that as it does any other PrepareAccess failure.
        if (!lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid))
            return FALSE;
        uint32_t waitUs = (uint32_t) (lorieNowUs() - waitStartUs);
        pvfb->state->presentStats.xLockWaitUs += waitUs;
        if (waitUs > pvfb->state->presentStats.xLockWaitMaxUs)
            pvfb->state->presentStats.xLockWaitMaxUs = waitUs;
        pvfb->state->presentStats.xLockWaits++;
        tookSharedLock = TRUE;
    }

    if (!priv->locked && !priv->mem) {
        int err = LorieBuffer_lock(priv->buffer, &priv->locked);
        if (err) {
            dprintf(2, "Failed to lock buffer, err %d\n", err);
            // An access that failed to start gets no FinishAccess, so the lock taken above has to go
            // back here. Returning with it held left the renderer waiting on it for good - its own
            // lock retries while the X server is alive, and the X server was never going to let go.
            if (tookSharedLock)
                lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
            return FALSE;
        }
        priv->wasLocked = FALSE;
    } else
        priv->wasLocked = TRUE;

    if (tookSharedLock) {
        priv->sharedLockDepth++;
        lorieSharedLockHeld++;
    }
    pPix->devPrivate.ptr = priv->locked ?: priv->mem;
    return TRUE;
}

void lorieFinishAccess(PixmapPtr pPix, __unused int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);

    /*
     * Gives back what PrepareAccess recorded taking, rather than asking lorieNeedsGpuLock() again.
     * That question depends on whether a GPU copy is pending on the buffer and on whether the root
     * is double-buffered, and neither is fixed between the two calls - an answer that changed in
     * between either left the lock held for good or released one this access never took.
     */
    if (priv->sharedLockDepth > 0) {
        priv->sharedLockDepth--;
        lorieSharedLockHeld--;
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
