/* TPRESENTCOPY: a Present copy end to end, from the real code (gen.py "presentcopy"): the X server queues it
 * (InitOutput.c lorieTryScheduleGpuCopy), the renderer's Vulkan backend draws it from the shared queue
 * (renderer.c rendererVkApplyPendingGpuCopies alone, rendererVkRedraw in a frame; vk.c on fakevk.h's small
 * device) and publishes completedSerial, the X server sees it done (lorieGpuCopyIsDone) and acknowledges it
 * (lorieGpuCopyAck). The renderer's GLES copies (rendererApplyPendingGpuCopiesLocked and drawRegion, on a GL
 * that records each draw) run on the same queue entries: Vulkan must draw what GLES draws.
 *
 *   X server: a copy is queued only with a renderer that can draw it, both pixmaps AHBs, 1-16 rectangles and
 *   room in the queue; otherwise FALSE (the X server's own CPU copy) with nothing queued, registered or
 *   referenced. Queued: the rectangles (the whole pixmap without a region), the offset, both buffer ids and the
 *   next serial; the source registered and referenced, a redirected window's pixmap too, the root counted; the
 *   renderer woken; not done.
 *   Renderer: copies only with the X server's PER_ACCESS answer; each rectangle drawn from the source into the
 *   destination and counted as imported_destination_draws once submitted, as GLES draws it - the same corners
 *   (both put clip-space y = -1 on the buffer's first row: GLES's framebuffer origin is its bottom row, which is
 *   the texture's first; Vulkan's is its top row) and texture coordinates, red and blue swapped exactly when GLES
 *   swaps them, the source's alpha kept; several entries in one submission; completedSerial and readIndex
 *   published only after the fence, with every buffer back with the foreign owner and out of the in-flight list; a
 *   failed submission publishes nothing; a frame with no copy queued draws none; an entry whose buffer the
 *   renderer never got is skipped and completed, as GLES does. Frames of eight copies from new sources, the
 *   cache passing 16 and 32 images in the middle of a frame: every image a frame already used stays valid (its
 *   acquire released, its draw from the right buffer), every image keeps its address until removed.
 *   GLES textures: none made for Vulkan's copies and frames; GLES's first use of a buffer makes its texture,
 *   once.
 *   While the device runs a frame (the fence waited for) the renderer does not hold the X server's lock, and
 *   exactly the frame's buffers are listed in flight. The list is cleared once the frame is over: fence
 *   signalled, or the device idle or lost after a fence that did not; kept while vkDeviceWaitIdle answers out
 *   of memory, until teardown asks again and it answers. Meanwhile the X server (lorieGpuWaitIdle, another
 *   thread here) gets an AHB the frame does not use at once - id 0 too, an id like any other - and one it uses
 *   only after the frame is over. A lost connection changes nothing while the submitter is there; a submitter
 *   that no longer exists ends the wait, and the list is cleared then. A list another renderer process left is
 *   not cleared by this one, and a frame of this one is recorded only once it is over.
 *   X server again: done only then; the CPU may lock the source; the acknowledgement drops what was held.
 *   TERMUX_X11_RENDERER_VK_TRANSFER_COPY (a new session with it, every image imported anew with the transfer
 *   usage): RGBA into RGBX below depth 32 copied by vkCmdCopyImage, no draw - rectangles at an offset, past the
 *   destination's edge and at a negative offset, each pixel (all four bytes) what the draw gives, published after
 *   the fence as the draws are, in a frame too, the root then drawn from what the copy wrote; two copies into one
 *   destination ordered by a barrier, the second over the first,
 *   RGBX into RGBX copying the 4th byte as it is; BGRA into RGBX, RGBX into RGBA, RGBX into depth 32 and a
 *   rectangle past its source drawn exactly as GLES draws them; a device refusing the usage, or failing the image
 *   made with it: imported without it, nothing recorded, drawn; BGRA into RGBX of depth 24 once the device check
 *   passed: copied as raw bits, every byte the source's and what the shader copy (quad.frag, modelled from the
 *   quad's flags) gives, red and blue apart in every source pixel - not passed, or depth 32: drawn; the option off: drawn, even between images that have the usage, and no import asks
 *   for it.
 *   TERMUX_X11_RENDERER_VK_SUBMIT_OPT (P11-C1), one submission in flight at a time still: under MAILBOX the copies
 *   queued before a frame go ahead of it in a submission of their own and are published at its fence - their two
 *   buffers alone in flight then, before the frame is submitted or presented - and the frame, carrying none, is
 *   presented before its fence is waited for (the presentation engine on its semaphore), the root alone in flight
 *   then; a copy queued while the image is acquired goes in the frame, which is then presented only after it is
 *   published; with the image not given (the acquire timing out) the copies are published all the same (off: not
 *   before a frame gets an image); an entry cancelled goes ahead too, skipped and completed; presented early and out
 *   of date - the fence waited for, the buffers given back, the swapchain made again for the next frame; presented
 *   early and the surface lost - the renderer gives up only after the fence and the buffers; FIFO not counted as the
 *   option's. Off, no frame is ever presented before its fence.
 *   TERMUX_X11_RENDERER_VK_FRAME_DIAG (P11-C3): each fence in its own timing, once - a root copy's, the copies sent
 *   ahead of a frame, the frame's (the copies in it when they go in its submission) - adding up to vk_fence_wait_us;
 *   a root copy right before a frame as its after_root, once; a tick's request as the frame's wake, once; the pacing
 *   intervals from given stamps (rendererVkFrameTimed): none for the first frame, 8, 17 and 35 ms in their buckets, the
 *   worst frame's parts, a stamp out of order and a gap over 250 ms counted once each and not timed, another swapchain
 *   starting over. Off: no sample, no stamp, the frames as on. */
#define _GNU_SOURCE         /* pthread_timedjoin_np: an X server thread never let go must not hang the test */
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fakevk.h"

/* ---- LorieBuffer: buffer.h's description; registration and references as the test keeps them ---- */
#define __unused __attribute__((unused))
#define _Nullable
#define __LIBC_HIDDEN__ static
#include "buffertypes_src.inc"
typedef struct LorieBuffer { LorieBuffer_Desc desc; int refs, gpuCopyPending; bool registered; unsigned glTexture; } LorieBuffer;
static LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }

/* ---- the X server's side ---- */
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxRec *rects; } RegionRec, *RegionPtr;
#define RegionNumRects(r) ((r)->n)
#define RegionRects(r) ((r)->rects)
typedef struct { LorieBuffer *buffer; void *locked, *mem; } LoriePixmapPriv;
typedef struct { struct { int width, height, depth; } drawable; LoriePixmapPriv *priv; } PixmapRec, *PixmapPtr;
#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(p) ((p)->priv)
static struct {
    Bool gpuPresentDisabled;
    struct { Bool legacyDrawing; } root;
    struct lorie_shared_server_state *state;
    uint64_t gpuCopySerialCounter, rootGpuCopyPending;
} vfb, *pvfb = &vfb;
static struct { void *devPrivate; } screen, *pScreenPtr = &screen;
static uint64_t gpuCopyAttempts, gpuCopyOffloads;
static bool connectionAlive = true;
static Bool lorieRootTwinned(void) { return FALSE; }     /* root double buffering off: the copies into the root are GPU copies */
static bool lorieConnectionAlive(void) { return connectionAlive; }
/* InitOutput.c's, but a pixmap not yet a buffer is not converted to one: every pixmap here has its buffer */
static LorieBuffer *lorieEnsureGpuSampleable(PixmapPtr pixmap, int8_t type) {
    LoriePixmapPriv *priv = LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap);
    return priv && priv->buffer && !priv->mem && priv->buffer->desc.type == type ? priv->buffer : NULL;
}
static void lorieRegisterBuffer(LorieBuffer *b) { b->registered = true; }     /* the renderer gets it */
static void LorieBuffer_acquire(LorieBuffer *b) { b->refs++; }
static void LorieBuffer_release(LorieBuffer *b) { b->refs--; }
static void LorieBuffer_gpuCopyPendingInc(LorieBuffer *b) { b->gpuCopyPending++; }
static void LorieBuffer_gpuCopyPendingDec(LorieBuffer *b) { b->gpuCopyPending--; }
static int LorieBuffer_lock(LorieBuffer *b, void **out) { *out = b; return 0; }
static void LorieBuffer_unlock(LorieBuffer *b) { (void) b; }
static void FatalError(const char *f, ...) { printf("FatalError %s\n", f); abort(); }
#include "wake_src.inc"
static struct lorie_renderer_wake wakeRegion, *rendererWake = &wakeRegion;
static int wakeups;
static void countedWake(volatile struct lorie_renderer_wake *w) {
    wakeups++;
    lorieWakeRenderer(w);
}
#define lorieWakeRenderer(w) countedWake(w)

/* ---- GL, as far as GLES's copies use it: each draw's corners, program, texture and framebuffer texture ---- */
typedef unsigned GLuint, GLenum;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean;
enum { GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_VIEWPORT = 0x0BA2, GL_FRAMEBUFFER = 0x8D40, GL_COLOR_ATTACHMENT0 = 0x8CE0,
       GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800,
       GL_FLOAT = 0x1406, GL_FALSE = 0, GL_TRIANGLE_STRIP = 5 };
static GLuint g_texture_program = 1, gv_pos = 0, gv_coords = 1, g_texture_program_bgra = 2, gv_pos_bgra = 2, gv_coords_bgra = 3;
static GLuint gpuCopyFbo, glProgram, glTexture, glTarget;
static const float *glVertices;
static struct { float v[16]; GLuint program, texture, target; } glDraws[32];
static int glDrawCount;
static void glGetIntegerv(GLenum e, GLint *v) { (void) e; v[0] = v[1] = v[2] = v[3] = 0; }
static void glGenFramebuffers(GLsizei n, GLuint *f) { (void) n; *f = 1; }
static void glBindFramebuffer(GLenum t, GLuint f) { (void) t; (void) f; }
static void glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { (void) t; (void) a; (void) tt; (void) l; glTarget = tex; }
static void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void) x; (void) y; (void) w; (void) h; }
static void glActiveTexture(GLenum t) { (void) t; }
static void glUseProgram(GLuint p) { glProgram = p; }
static void glBindTexture(GLenum t, GLuint id) { (void) t; glTexture = id; }
static void glTexParameteri(GLenum t, GLenum p, GLint v) { (void) t; (void) p; (void) v; }
static void glVertexAttribPointer(GLuint i, GLint n, GLenum t, GLboolean norm, GLsizei stride, const void *p) {
    (void) n; (void) t; (void) norm; (void) stride;
    if (i == gv_pos || i == gv_pos_bgra)
        glVertices = p;     /* four vertices of x, y, u, v */
}
static void glEnableVertexAttribArray(GLuint i) { (void) i; }
static void glDrawArrays(GLenum mode, GLint first, GLsizei n) {
    CHECK(mode == GL_TRIANGLE_STRIP && first == 0 && n == 4 && glDrawCount < 32, "GL: not one quad");
    memcpy(glDraws[glDrawCount].v, glVertices, sizeof glDraws[0].v);
    glDraws[glDrawCount].program = glProgram;
    glDraws[glDrawCount].texture = glTexture;
    glDraws[glDrawCount++].target = glTarget;
}
#define checkGlError() ((void) 0)
static int glTextures;     /* GLES textures made for buffers (renderer.c rendererAttachToGL) */
static void rendererAttachToGL(LorieBuffer *b) { b->glTexture = (GLuint) b->desc.id; glTextures++; }
static GLuint LorieBuffer_getGLTextureId(LorieBuffer *b) { return b->glTexture; }
static void LorieBuffer_bindTexture(LorieBuffer *b) { glTexture = b->glTexture; }
static bool lorieDebugEnabled;
static int filtering = GL_NEAREST;

/* ---- the renderer's side ---- */
static volatile struct lorie_shared_server_state *state;
static struct lorie_vk vk;
static struct lorie_vk_images vkImages;
static struct lorie_vk_renderer vkr;
static bool vkOwned, backendSelect, vkActive = true;    /* the mock rendererVkFail tears nothing down */
static uint32_t vkOwnershipSeq;
static volatile int viewportX, viewportY, viewportW = 1080, viewportH = 2400, expectedW, expectedH;
#define CVT_H_GRANULARITY 8
static ANativeWindow window = { 1080, 2400 }, *win = &window;
static pthread_spinlock_t bufferLock;
static LorieBuffer bufs[96];
static int nbufs, vkFailures;
#define buffers rendererBuffers     /* fakevk.h has its own `buffers` */
static int buffers, removedBuffers, addedBuffers;
#define LorieBuffer_addToList(buf, list) ((void) (buf), (void) (list))
/* the renderer's lists: what the X server registered */
static LorieBuffer *LorieBufferList_findById(volatile void *list, uint64_t id) {
    for (int i = 0; list == &buffers && i < nbufs; i++)
        if (bufs[i].registered && bufs[i].desc.id == id)
            return &bufs[i];
    return NULL;
}
static uint64_t rootAtLock;     /* non-zero: the root id the X server publishes as the renderer takes the lock */
static void lorie_mutex_lock(volatile int *m, volatile int *pid);
static void lorie_mutex_unlock(volatile int *m, volatile int *pid) { (*m)--; *pid = 0; }
/* how things stood when the renderer gave up: its frame's fence waited for, its buffers out of the in-flight list */
static struct { bool fencePending, busy; } atFail;
static void rendererVkAuxForget(void);
static bool failEndsVulkan;     /* the mock tears Vulkan down as the real one does: no longer active, the surface given up */
static void rendererVkFail(void) {
    vkFailures++;
    atFail.fencePending = emu.fencePending;
    atFail.busy = state->gpuInFlight.busy;
    rendererVkAuxForget();      /* P11-C3-B2: as rendererVkTeardown does, once the device is idle */
    if (failEndsVulkan) {
        vkActive = false;
        state->surfaceAvailable = false;
    }
}
/* GLES's frame and its copies alone, as the renderer's pass (rendererPass) would call them: counted */
static int glesRedraws, glesCopies;
static void rendererRedrawLocked(bool *waitingForBuffers) {
    (void) waitingForBuffers;
    glesRedraws++;
}
static void rendererApplyPendingGpuCopies(void) {
    glesCopies++;
}
/* the X server woken for a finished batch: how things stood at that moment */
static struct { int count; bool cleared, fenceWaited, released; uint64_t completed; int presents, fenceWaits; } notified;
static int fenceWaits(void);
static void notifyGpuCopyDone(void) {
    notified.count++;
    notified.presents = emu.presents;
    notified.fenceWaits = fenceWaits();
    notified.cleared = !state->gpuInFlight.busy;
    notified.fenceWaited = !emu.fencePending;
    notified.released = true;
    for (int i = 0; i < nbufs; i++)
        if (bufs[i].desc.buffer && bufs[i].desc.buffer->owned)
            notified.released = false;
    notified.completed = state->gpuCopyQueue.completedSerial;
}
/* the Vulkan backend's notification carries when it published (P10-B0): the stamp it stored */
static uint64_t notifiedPublishedUs, notifiedSerial;
static void notifyGpuCopyPublished(uint64_t publishedUs, uint64_t serial) {
    notifiedPublishedUs = publishedUs;
    notifiedSerial = serial;    /* P11-C3-C: the serial it published */
    notifyGpuCopyDone();
}
static void (*atLock)(void);     /* P11-C3-C: run once, as the renderer takes the shared lock */
static void lorie_mutex_lock(volatile int *m, volatile int *pid) {
    (*m)++;
    *pid = 1;
    if (rootAtLock && m == &state->lock)
        state->rootWindowTextureID = rootAtLock;
    if (atLock && m == &state->lock) {
        void (*f)(void) = atLock;
        atLock = NULL;
        f();
    }
}
/* the shared lock if no one holds it (lorieGpuCopySettle): a case makes a renderer process hold it (lockHolder) */
static pid_t lockHolder;
static bool lorie_mutex_trylock(volatile int *m, volatile int *pid) {
    if (lockHolder)
        return false;
    (*m)++;
    *pid = 1;
    return true;
}
#include "settleconsts_src.inc"
#include "presentcopy_src.inc"

/* the moment the renderer waits for a frame's fence: the device runs the frame */
static struct { int waits; bool lockHeld, otherOver; uint32_t busy, count; uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX]; int presents;
                struct { uint32_t busy, count; uint64_t ids[LORIE_GPU_IN_FLIGHT_MAX]; } prev; } atFence;
static int fenceWaits(void) { return atFence.waits; }
static int gpuMs;                   /* how long the device takes, at the next fence */
static int fenceMs[8], fenceMsCount, fenceMsAt;     /* P11-C3: how long each of the next fences takes, in order (then gpuMs) */
static volatile bool gpuFinished;
static volatile bool otherOver;     /* the frame another submitter listed is over (otherSubmitter) */
/* P11-C3-B2: the order the frame's and the second submission's fences were waited for, and what stood then */
static struct { int n; VkFence fence[8]; bool rootDone[8], auxListed[8], dstInFlight[8], rootInFlight[8]; uint32_t submits[8]; } fenceLog;
static uint64_t fenceLogDst, fenceLogRoot;
static uint32_t stageAtFence;   /* P11-C3-C: the renderer's stage as it waits for a frame's fence */
static void lookAtFence(VkFence fence) {
    if (fence != vkr.frameFence)
        return;             /* a swapchain image's */
    stageAtFence = state->progress.rStage;
    if (fenceLog.n < 8) {
        fenceLog.fence[fenceLog.n] = fence;
        fenceLog.rootDone[fenceLog.n] = state->rootCopy.done == state->rootCopy.seq;
        fenceLog.auxListed[fenceLog.n] = (state->gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX) != 0;
        fenceLog.dstInFlight[fenceLog.n] = lorieGpuInFlight(state, fenceLogDst);
        fenceLog.rootInFlight[fenceLog.n] = lorieGpuInFlight(state, fenceLogRoot);
        fenceLog.submits[fenceLog.n] = (uint32_t) emu.submits;
        fenceLog.n++;
    }
    atFence.waits++;
    atFence.lockHeld |= state->lock != 0;
    atFence.otherOver = otherOver;
    atFence.presents = emu.presents;
    atFence.prev.busy = atFence.busy;
    atFence.prev.count = atFence.count;
    memcpy(atFence.prev.ids, atFence.ids, sizeof atFence.ids);
    atFence.busy = state->gpuInFlight.busy;
    atFence.count = state->gpuInFlight.count;
    memcpy(atFence.ids, (const void *) state->gpuInFlight.ids, sizeof atFence.ids);
    if (fenceMsAt < fenceMsCount)
        usleep((useconds_t) fenceMs[fenceMsAt++] * 1000);
    else if (gpuMs)
        usleep(gpuMs * 1000);
    gpuFinished = true;
}
// whether the last fence found exactly these buffers in flight
static bool inFlightAtFence(int n, ...) {
    va_list ap;
    bool all = atFence.busy && atFence.count == (uint32_t) n;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        uint64_t id = va_arg(ap, LorieBuffer *)->desc.id;
        bool found = false;
        for (uint32_t k = 0; k < atFence.count; k++)
            found |= atFence.ids[k] == id;
        all &= found;
    }
    va_end(ap);
    return all;
}

/* the X server, on its own thread while the renderer draws a frame using the root */
static struct { uint64_t rootId, otherId; bool otherWhileBusy, rootAfterGpu, rootListCleared, started; } xThread;
static void *xServer(void *unused) {
    (void) unused;
    for (int spins = 0; spins < 5000 && !__atomic_load_n(&state->gpuInFlight.busy, __ATOMIC_ACQUIRE); spins++)
        usleep(200);
    xThread.started = __atomic_load_n(&state->gpuInFlight.busy, __ATOMIC_ACQUIRE);
    lorieGpuWaitIdle(state, false, xThread.otherId);
    xThread.otherWhileBusy = !gpuFinished;
    lorieGpuWaitIdle(state, false, xThread.rootId);
    xThread.rootAfterGpu = gpuFinished;
    xThread.rootListCleared = !__atomic_load_n(&state->gpuInFlight.busy, __ATOMIC_ACQUIRE);
    return NULL;
}

/* another submitter: its listed frame over after 80 ms (it clears the list, as rendererVkFrameDone does) */
static void *otherSubmitter(void *unused) {
    (void) unused;
    usleep(80000);
    otherOver = true;
    __atomic_store_n(&state->gpuInFlight.busy, 0, __ATOMIC_RELEASE);
    syscall(SYS_futex, &state->gpuInFlight.busy, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    return NULL;
}

// a process id no process has any more
static pid_t deadPid(void) {
    pid_t pid = fork();
    if (!pid)
        _exit(0);
    waitpid(pid, NULL, 0);
    return pid;
}

static void list(uint64_t id, pid_t pid) {
    state->gpuInFlight.ids[0] = id;
    state->gpuInFlight.count = 1;
    state->gpuInFlight.pid = pid;
    __atomic_store_n(&state->gpuInFlight.busy, 1, __ATOMIC_RELEASE);
}

static long msSince(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

#define X_USAGE (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)

static struct lorie_shared_server_state st, glesState;
typedef __typeof__(st.gpuCopyQueue) Queue;

/* an X server pixmap's buffer, described as buffer.c's allocate() describes an AHB: by the AHB itself */
static LorieBuffer *buffer(uint32_t format, uint32_t width, uint32_t height) {
    AHardwareBuffer_Desc d = { .width = width, .height = height, .layers = 1, .format = format, .usage = X_USAGE };
    LorieBuffer *b = &bufs[nbufs++];
    AHardwareBuffer *a = NULL;
    if (nbufs > (int) (sizeof bufs / sizeof *bufs)) {
        printf("  FAIL more buffers than bufs holds\n");
        exit(1);
    }
    CHECK(AHardwareBuffer_allocate(&d, &a) == 0, "no buffer");
    AHardwareBuffer_describe(a, &d);
    b->desc = (LorieBuffer_Desc) { .width = (int32_t) d.width, .height = (int32_t) d.height, .stride = (int32_t) d.stride,
                                   .format = (uint8_t) d.format, .type = LORIEBUFFER_AHARDWAREBUFFER, .id = (uint64_t) nbufs, .buffer = a };
    return b;
}

static void own(uint32_t seq) {
    vkOwned = true;
    vkOwnershipSeq = seq;
    st.backend.ownershipAck = LORIE_OWNERSHIP_WORD(seq, LORIE_OWNERSHIP_PER_ACCESS);
}

/* GLES's copies of the same entries, on a state of their own: the serial they complete */
static uint64_t gles(const Queue *q) {
    volatile struct lorie_shared_server_state *vkState = state;
    uint64_t serial;
    memcpy((void *) &glesState.gpuCopyQueue, q, sizeof *q);
    glDrawCount = 0;
    state = &glesState;
    serial = rendererApplyPendingGpuCopiesLocked();
    state = vkState;
    return serial;
}

/* GLES draw k as Vulkan quad q: GLES's first and last corners (x, y as drawRegion passes them, u, v) are the
 * quad's two corners and texture coordinates, its BGRA program the quad's swizzle; a copy is never made opaque */
static bool sameAsGles(int k, const struct lorie_vk_quad *q) {
    const float *v = glDraws[k].v;
    const float want[8] = { v[0], v[1], v[12], v[13], v[2], v[3], v[14], v[15] };
    const float got[8] = { q->rect[0], q->rect[1], q->rect[2], q->rect[3], q->uv[0], q->uv[1], q->uv[2], q->uv[3] };
    for (int i = 0; i < 8; i++)
        if (fabsf(want[i] - got[i]) > 1e-6f)
            return false;
    return (glDraws[k].program == g_texture_program_bgra) == !!(q->flags & LORIE_VK_QUAD_SWIZZLE) && !(q->flags & LORIE_VK_QUAD_OPAQUE);
}

static void printQuads(int n) {
    for (int k = 0; k < n && k < glDrawCount; k++)
        printf("    GLES (%g,%g)-(%g,%g) uv (%g,%g)-(%g,%g) program %u | Vulkan (%g,%g)-(%g,%g) uv (%g,%g)-(%g,%g) flags %u\n",
               glDraws[k].v[0], glDraws[k].v[1], glDraws[k].v[12], glDraws[k].v[13], glDraws[k].v[2], glDraws[k].v[3],
               glDraws[k].v[14], glDraws[k].v[15], glDraws[k].program, pushed[k].rect[0], pushed[k].rect[1], pushed[k].rect[2],
               pushed[k].rect[3], pushed[k].uv[0], pushed[k].uv[1], pushed[k].uv[2], pushed[k].uv[3], pushed[k].flags);
}

/* ---- TERMUX_X11_RENDERER_VK_TRANSFER_COPY: copies by vkCmdCopyImage, checked by their pixels ---- */
/* a buffer's pixels as the X server draws them, through the CPU lock: (x, y, tag, alpha), alpha < 0: (x + y) | 0x80 */
static void fill(LorieBuffer *b, uint8_t tag, int alpha) {
    AHardwareBuffer *a = b->desc.buffer;
    uint8_t *cpu = NULL;
    CHECK(AHardwareBuffer_lock(a, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, (void **) &cpu) == 0, "fill: no CPU lock");
    for (uint32_t y = 0; y < a->desc.height; y++)
        for (uint32_t x = 0; x < a->desc.width; x++) {
            uint8_t *q = cpu + ((size_t) y * a->desc.stride + x) * 4;
            q[0] = (uint8_t) x, q[1] = (uint8_t) y, q[2] = tag, q[3] = (uint8_t) (alpha < 0 ? (int) ((x + y) | 0x80) : alpha);
        }
    AHardwareBuffer_unlock(a, NULL);
}
/* one rectangle of a source copied at an offset */
struct copied { LorieBuffer *src; BoxRec box; int xOff, yOff; };
/* dst's pixels that are not what the copies give, in order (a later one over an earlier one): the source's pixel,
 * all four bytes, where a copy lands inside dst; dst's own fill (tag, alpha 0x11) everywhere else */
static int wrongPixels(LorieBuffer *dst, uint8_t tag, const struct copied *c, int n) {
    const AHardwareBuffer *d = dst->desc.buffer;
    int wrong = 0;
    for (int y = 0; y < (int) d->desc.height; y++)
        for (int x = 0; x < (int) d->desc.width; x++) {
            const uint8_t own[4] = { (uint8_t) x, (uint8_t) y, tag, 0x11 }, *want = own;
            for (int k = 0; k < n; k++) {
                int sx = x - c[k].xOff, sy = y - c[k].yOff;
                const AHardwareBuffer *s = c[k].src->desc.buffer;
                if (sx >= c[k].box.x1 && sx < c[k].box.x2 && sy >= c[k].box.y1 && sy < c[k].box.y2)
                    want = s->cpu + ((size_t) sy * s->desc.stride + sx) * 4;
            }
            wrong += memcmp(d->cpu + ((size_t) y * d->desc.stride + x) * 4, want, 4) != 0;
        }
    return wrong;
}
/* the last submission: a barrier on `image` orders the second copy into it after the first (transfer write after
 * transfer write) */
static bool copiesOrdered(VkImage image) {
    int first = -1, second = -1, barrier = -1;
    for (int i = 0; i < cmdObj.n; i++) {
        const struct fake_op *op = &cmdObj.ops[i];
        if (op->kind == OP_COPY_IMAGE && op->b == image) {
            if (first < 0)
                first = i;
            else if (second < 0)
                second = i;
        } else if (op->kind == OP_IMAGE_BARRIER && op->image.image == image && first >= 0 && second < 0 &&
                   (op->src & VK_PIPELINE_STAGE_TRANSFER_BIT) && (op->dst & VK_PIPELINE_STAGE_TRANSFER_BIT) &&
                   (op->image.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) && (op->image.dstAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT))
            barrier = i;
    }
    return first >= 0 && second > first && barrier > first && barrier < second;
}
static VkImageUsageFlags usageOfBuffer(LorieBuffer *b) {
    struct lorie_vk_image *img = lorieVkImageFind(&vkImages, b->desc.id);
    return img ? img->usage : 0;
}
#define TRANSFER_USAGE (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
/* its verdict counters (lorie.h vkXfer*): what a run moved them by, as "checked+N", then each condition, each reason
 * an image lacked the usage (missing_*) and each import's (imports_*) that moved, in their order */
#define XFER_NAME(n) #n,
static const char *const xferNots[] = { LORIE_VK_XFER_NOTS(XFER_NAME) }, *const xferUsages[] = { LORIE_VK_XFER_USAGES(XFER_NAME) };
#undef XFER_NAME
static struct lorie_renderer_diag xferBefore;
static void xferMark(void) { memcpy(&xferBefore, (const void *) &st.diag, sizeof xferBefore); }
static const char *xferMoves(void) {
    static char out[512];
    int u = snprintf(out, sizeof out, "checked+%u", st.diag.vkXferChecked - xferBefore.vkXferChecked);
    for (int i = 0; i < LORIE_VK_XFER_NOT_COUNT; i++)
        if (st.diag.vkXferNot[i] != xferBefore.vkXferNot[i])
            u += snprintf(out + u, sizeof out - u, " %s+%u", xferNots[i], st.diag.vkXferNot[i] - xferBefore.vkXferNot[i]);
    for (int i = 0; i < LORIE_VK_XFER_USAGE_COUNT; i++)
        if (st.diag.vkXferUsageMissing[i] != xferBefore.vkXferUsageMissing[i])
            u += snprintf(out + u, sizeof out - u, " missing_%s+%u", xferUsages[i],
                          st.diag.vkXferUsageMissing[i] - xferBefore.vkXferUsageMissing[i]);
    for (int i = 0; i < LORIE_VK_XFER_USAGE_COUNT; i++)
        if (st.diag.vkXferImports[i] != xferBefore.vkXferImports[i])
            u += snprintf(out + u, sizeof out - u, " imports_%s+%u", xferUsages[i], st.diag.vkXferImports[i] - xferBefore.vkXferImports[i]);
    return out;
}
#define XFER_MOVED(want, what) CHECK(!strcmp(xferMoves(), want), what ": verdicts %s (want %s)", xferMoves(), want)
/* the shader copy (shaders/quad.frag) of one pixel, by what the formats and the quad's flags make of its bytes: the
 * texel read in the source's format (an RGBX source's 4th byte read as 1), red and blue swapped by
 * LORIE_VK_QUAD_SWIZZLE, alpha 1 by LORIE_VK_QUAD_OPAQUE, written in the destination's format */
static void shaderPixel(const uint8_t *in, VkFormat sf, bool rgbx, uint32_t flags, VkFormat df, uint8_t *out) {
    uint8_t c[4] = { in[0], in[1], in[2], in[3] }, t;   /* red, green, blue, alpha */
    if (sf == VK_FORMAT_B8G8R8A8_UNORM)
        c[0] = in[2], c[2] = in[0];
    if (rgbx)
        c[3] = 255;
    if (flags & LORIE_VK_QUAD_SWIZZLE)
        t = c[0], c[0] = c[2], c[2] = t;
    if (flags & LORIE_VK_QUAD_OPAQUE)
        c[3] = 255;
    if (df == VK_FORMAT_B8G8R8A8_UNORM)
        t = c[0], c[0] = c[2], c[2] = t;
    memcpy(out, c, 4);
}
/* dst's pixels inside src's boxes copied at an offset that are not the shader copy's (all four bytes) */
static int notAsShader(LorieBuffer *src, LorieBuffer *dst, const BoxRec *box, int n, int xOff, int yOff) {
    const AHardwareBuffer *s = src->desc.buffer, *d = dst->desc.buffer;
    struct lorie_vk_image *si = lorieVkImageFind(&vkImages, src->desc.id), *di = lorieVkImageFind(&vkImages, dst->desc.id);
    int wrong = 0;
    for (int k = 0; k < n; k++) {
        LorieGpuCopyRect r = { box[k].x1, box[k].y1, box[k].x2, box[k].y2 };
        uint32_t flags = rendererVkCopyQuad(r, (int16_t) xOff, (int16_t) yOff, &src->desc, &dst->desc, si->rgbaOrder != di->rgbaOrder).flags;
        for (int y = r.y1; y < r.y2; y++)
            for (int x = r.x1; x < r.x2; x++) {
                uint8_t want[4];
                if (x + xOff < 0 || y + yOff < 0 || x + xOff >= (int) d->desc.width || y + yOff >= (int) d->desc.height)
                    continue;
                shaderPixel(s->cpu + ((size_t) y * s->desc.stride + x) * 4, si->format, si->rgbx, flags, di->format, want);
                wrong += memcmp(d->cpu + ((size_t) (y + yOff) * d->desc.stride + x + xOff) * 4, want, 4) != 0;
            }
    }
    return wrong;
}

// whether the fence waited for before the last found exactly these buffers in flight
static bool inFlightBeforeLast(int n, ...) {
    va_list ap;
    bool all = atFence.prev.busy && atFence.prev.count == (uint32_t) n;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        uint64_t id = va_arg(ap, LorieBuffer *)->desc.id;
        bool found = false;
        for (uint32_t k = 0; k < atFence.prev.count; k++)
            found |= atFence.prev.ids[k] == id;
        all &= found;
    }
    va_end(ap);
    return all;
}
/* P11-C2: a root copy asked for as the X server asks for it (InitOutput.c lorieRootGpuCopyAsk) */
static uint32_t rootSeq;
static void rootAsk(LorieBuffer *src, LorieBuffer *dst, const LorieGpuCopyRect *r, uint32_t n) {
    state->rootCopy.srcId = src->desc.id;
    state->rootCopy.dstId = dst->desc.id;
    state->rootCopy.numRects = n;
    memcpy((void *) state->rootCopy.rects, r, sizeof *r * n);
    state->rootCopy.queuedUs = lorieMonotonicUs();
    __atomic_store_n(&state->rootCopy.seq, ++rootSeq, __ATOMIC_RELEASE);
}
/* the root copy's draws (the last `n` pushed): each covers exactly rectangle k of the destination - in pixels, the
 * clip-space corners the copy's vertex shader maps - and samples the same pixels of the source, 1:1 (both buffers the
 * same size), unswizzled: what pixman_blt copies there, red, green and blue (an RGBX buffer's 4th byte is undefined
 * to Vulkan, and padding to the X server's depth 24). The fake device draws no pixels: its quads are checked. */
static bool rootQuadsExact(LorieBuffer *src, LorieBuffer *dst, const LorieGpuCopyRect *r, uint32_t n) {
    float w = (float) dst->desc.width, h = (float) dst->desc.height, sw = (float) src->desc.width, sh = (float) src->desc.height;
    if (pushes < (int) n || sw != w || sh != h)
        return false;
    for (uint32_t k = 0; k < n; k++) {
        const struct lorie_vk_quad *q = &pushed[pushes - (int) n + (int) k];
        const float want[8] = { (float) r[k].x1, (float) r[k].y1, (float) r[k].x2, (float) r[k].y2,
                                (float) r[k].x1, (float) r[k].y1, (float) r[k].x2, (float) r[k].y2 };
        const float got[8] = { (q->rect[0] + 1.f) * w / 2.f, (q->rect[1] + 1.f) * h / 2.f, (q->rect[2] + 1.f) * w / 2.f,
                               (q->rect[3] + 1.f) * h / 2.f, q->uv[0] * sw, q->uv[1] * sh, q->uv[2] * sw, q->uv[3] * sh };
        for (int i = 0; i < 8; i++)
            if (fabsf(got[i] - want[i]) > 1e-3f)
                return false;
        if (q->flags & LORIE_VK_QUAD_SWIZZLE)
            return false;
    }
    return true;
}
/* the X server waiting for a root copy on its futex (as lorieRootCopyWait does) while the device runs it */
static struct { uint32_t seq; bool woke, gpuOverAtWake, timedOut; } rootWaiter;
static void *rootWaiterThread(void *arg) {
    (void) arg;
    for (;;) {
        uint32_t done = __atomic_load_n(&state->rootCopy.done, __ATOMIC_ACQUIRE);
        struct timespec t = { 3, 0 };
        if (done == rootWaiter.seq)
            break;
        if (syscall(SYS_futex, &state->rootCopy.done, FUTEX_WAIT, done, &t, NULL, 0) && errno == ETIMEDOUT) {
            rootWaiter.timedOut = true;
            break;
        }
    }
    rootWaiter.woke = true;
    rootWaiter.gpuOverAtWake = gpuFinished;
    return NULL;
}

/* P11-C3: every timing's samples so far (struct lorie_hist counts), and one timing's samples and sum since `before` */
static volatile struct lorie_hist *const allHists[] = {
    &st.diag.vkFrameStart, &st.diag.vkPresentDelta, &st.diag.vkFrameWake, &st.diag.vkFrameCpu, &st.diag.vkFrameAcquire,
    &st.diag.vkFrameLock, &st.diag.vkFrameRecord, &st.diag.vkFrameSubmit, &st.diag.vkFrameFence, &st.diag.vkFramePresent,
    &st.diag.vkFrameAfterRoot, &st.diag.vkFenceRoot, &st.diag.vkFenceCopy, &st.diag.rootTake, &st.diag.rootRecord,
    &st.diag.rootResume, &st.diag.rootBlock, &st.diag.xRequest, &st.diag.xAccess, &st.diag.xAccessLock, &st.diag.xAccessGpu,
    &st.diag.xTickDelay, &st.diag.xTickWork };
static uint32_t histSamples(void) {
    uint32_t n = 0;
    for (size_t i = 0; i < sizeof allHists / sizeof *allHists; i++)
        n += allHists[i]->count;
    return n;
}
struct histMark { uint32_t count; uint64_t sumUs; };
static struct histMark mark(volatile struct lorie_hist *h) { return (struct histMark) { h->count, h->sumUs }; }
static uint32_t newSamples(volatile struct lorie_hist *h, struct histMark before) { return h->count - before.count; }
static uint64_t newUs(volatile struct lorie_hist *h, struct histMark before) { return h->sumUs - before.sumUs; }

/* P11-C1: the early presents the option's cases made - none other may be made (with the option off) */
static int earlyWithOption;
/* P11-C1: a copy the X server queues while the renderer acquires the frame's image (after the copies went ahead) */
static struct { PixmapPtr src, dst; uint64_t serial; void *dstBuffer; bool queued; } duringAcquire;
static void queueDuringAcquire(void) {
    if (duringAcquire.src && !duringAcquire.queued)
        duringAcquire.queued = lorieTryScheduleGpuCopy(duringAcquire.src, duringAcquire.dst, NULL, 0, 0, &duringAcquire.serial,
                                                       &duringAcquire.dstBuffer);
}

/* P11-C3-C: a buffer coming from the connection `ms` later (rendererAddBuffer's part: in the list, then the wake) - or
 * none, only a look then at the shared lock: whether the renderer held it while it waited */
static struct arrival { LorieBuffer *buf; int ms, lockHeld; } arrival;
static void *arrive(void *arg) {
    struct arrival *a = arg;
    usleep((useconds_t) a->ms * 1000);
    a->lockHeld = st.lock;
    if (a->buf) {
        __atomic_store_n(&a->buf->registered, true, __ATOMIC_RELEASE);
        lorieWakeRenderer(rendererWake);
    }
    return NULL;
}
static LorieBuffer *gone;
static void bufferGone(void) { gone->registered = false; }

int main(void) {
    LorieBuffer *root, *src, *bgra, *redirected, *fresh, *fd;
    LoriePixmapPriv rootP, srcP, bgraP, redirectedP, freshP, fdP, memP;
    PixmapRec rootPix, srcPix, bgraPix, redirectedPix, freshPix, fdPix, memPix;
    BoxRec boxes[17] = { { 0, 0, 50, 20 }, { 60, 30, 200, 100 } };
    RegionRec two = { 2, boxes };
    LorieGpuCopyEntry *e;
    uint32_t submits0;
    uint64_t serial = 0;
    void *dstBuffer = NULL;
    bool waiting = false;
    int d0;
    Queue saved;

    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(30);          /* a wait that never ends fails the test (in a few seconds otherwise) instead of hanging it */
    pthread_spin_init(&bufferLock, PTHREAD_PROCESS_PRIVATE);
    good();
    emu.presentModes[1] = VK_PRESENT_MODE_MAILBOX_KHR;    /* immediate: MAILBOX, copies in the frame's submission */
    emu.presentModeCount = 2;
    state = pvfb->state = &st;
    CHECK(lorieVkCreateDevice(&vk, fGetInstanceProcAddr, &st), "no device");
    vk.verifiedFormats = 7;
    CHECK(lorieVkRendererCreate(&vk, &vkr, &st) && lorieVkSwapchainCreate(&vk, &vkr, win, &st), "no renderer");

    root = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 640, 480);
    src = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 200, 100);
    bgra = buffer(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 64, 32);
    redirected = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 300, 200);
    fresh = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 40, 40);
    fd = &bufs[nbufs++];
    fd->desc = (LorieBuffer_Desc) { .width = 40, .height = 40, .stride = 40, .format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM,
                                    .type = LORIEBUFFER_FD, .id = (uint64_t) nbufs };
    rootP = (LoriePixmapPriv) { root }; srcP = (LoriePixmapPriv) { src }; bgraP = (LoriePixmapPriv) { bgra };
    redirectedP = (LoriePixmapPriv) { redirected }; freshP = (LoriePixmapPriv) { fresh }; fdP = (LoriePixmapPriv) { fd };
    memP = (LoriePixmapPriv) { fresh, NULL, &memP };    /* a pixmap in memory a client gave */
    rootPix = (PixmapRec) { { 640, 480 }, &rootP }; srcPix = (PixmapRec) { { 200, 100 }, &srcP };
    bgraPix = (PixmapRec) { { 64, 32 }, &bgraP }; redirectedPix = (PixmapRec) { { 300, 200 }, &redirectedP };
    freshPix = (PixmapRec) { { 40, 40 }, &freshP }; fdPix = (PixmapRec) { { 40, 40 }, &fdP }; memPix = (PixmapRec) { { 40, 40 }, &memP };
    pScreenPtr->devPrivate = &rootPix;
    root->registered = true;            /* the X server registers its root when it makes it */
    st.rootWindowTextureID = root->desc.id;
    expectedW = 640;
    expectedH = 480;
    st.surfaceAvailable = 1;
    own(7);
    emuFenceWait = lookAtFence;

    /* a client's window presented into the root: two rectangles at an offset, the copies alone */
    CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 37, 11, &serial, &dstBuffer), "into the root: not queued");
    e = &st.gpuCopyQueue.entries[0];
    CHECK(serial == 1 && e->serial == 1 && st.gpuCopyQueue.writeIndex == 1 && st.gpuCopyQueue.readIndex == 0 &&
          e->srcBufferId == src->desc.id && e->dstBufferId == root->desc.id && e->xOff == 37 && e->yOff == 11 && e->numRects == 2 &&
          e->rects[0].x1 == 0 && e->rects[0].y2 == 20 && e->rects[1].x1 == 60 && e->rects[1].y1 == 30 && e->rects[1].x2 == 200 &&
          e->rects[1].y2 == 100, "into the root: entry serial %llu, %u rectangles, offset %d,%d", (unsigned long long) e->serial,
          e->numRects, e->xOff, e->yOff);
    CHECK(!dstBuffer && vfb.rootGpuCopyPending == 1 && src->registered && src->refs == 1 && src->gpuCopyPending == 1 &&
          root->refs == 0 && root->gpuCopyPending == 0 && wakeups == 1 && gpuCopyOffloads == 1,
          "into the root: held %p, root pending %llu, source registered %d refs %d pending %d, renderer woken %d",
          dstBuffer, (unsigned long long) vfb.rootGpuCopyPending, src->registered, src->refs, src->gpuCopyPending, wakeups);
    CHECK(!lorieGpuCopyIsDone(serial), "into the root: done before the renderer ran");
    memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
    pushes = 0;
    d0 = emu.draws;
    rendererVkApplyPendingGpuCopies();
    CHECK(vkFailures == 0 && emu.violations == 0 && st.lock == 0, "copies alone: %d failures, %d violations", vkFailures, emu.violations);
    CHECK(st.diag.vkQueueSubmits == 1 && st.diag.vkImportedDestinationDraws == 2 && st.diag.vkImportedSourceDraws == 2,
          "copies alone: submits %u, destination draws %u, source draws %u", st.diag.vkQueueSubmits,
          st.diag.vkImportedDestinationDraws, st.diag.vkImportedSourceDraws);
    CHECK(emu.draws - d0 == 2 && emu.drawnFrom[d0 % 64] == src->desc.buffer && emu.drawnInto[d0 % 64] == root->desc.buffer &&
          emu.drawnFrom[(d0 + 1) % 64] == src->desc.buffer && emu.drawnInto[(d0 + 1) % 64] == root->desc.buffer,
          "copies alone: not drawn from the source into the root");
    CHECK(st.gpuCopyQueue.readIndex == 1 && st.gpuCopyQueue.completedSerial == 1 && notified.count == 1 && notified.completed == 1,
          "copies alone: readIndex %u, completed %llu, notified %d", st.gpuCopyQueue.readIndex,
          (unsigned long long) st.gpuCopyQueue.completedSerial, notified.count);
    CHECK(atFence.waits == 1 && !atFence.lockHeld && inFlightAtFence(2, src, root) && !st.gpuInFlight.busy,
          "copies alone: at the fence the X server's lock %s, %u buffers in flight; afterwards the list %s",
          atFence.lockHeld ? "held" : "free", atFence.busy ? atFence.count : 0, st.gpuInFlight.busy ? "still set" : "cleared");
    st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
    CHECK(notified.cleared && notified.fenceWaited && notified.released,
          "copies alone: published %s", !notified.cleared ? "with its buffers still in flight" : !notified.fenceWaited ?
          "before the fence was waited for" : "with a buffer still the renderer's");
    CHECK(glTextures == 0, "copies alone: a GLES texture made for Vulkan");
    CHECK(gles(&saved) == 1 && glesState.gpuCopyQueue.readIndex == 1 && glDrawCount == 2 && pushes == 2, "GLES: %d draws", glDrawCount);
    CHECK(glTextures == 2 && src->glTexture && root->glTexture, "GLES's first copies: %d textures made (want the source's and the root's)",
          glTextures);
    CHECK(sameAsGles(0, &pushed[0]) && sameAsGles(1, &pushed[1]) && glDraws[0].texture == src->desc.id && glDraws[0].target == root->desc.id,
          "copies alone: not what GLES draws");
    if (!sameAsGles(0, &pushed[0]) || !sameAsGles(1, &pushed[1]))
        printQuads(2);
    CHECK(lorieGpuCopyIsDone(serial), "copies alone: not done");
    {
        void *cpu;
        CHECK(AHardwareBuffer_lock(src->desc.buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &cpu) == 0 &&
              AHardwareBuffer_unlock(src->desc.buffer, NULL) == 0 && emu.violations == 0, "the source not the CPU's once done");
    }
    lorieGpuCopyAck(&srcPix, dstBuffer);
    CHECK(src->refs == 0 && src->gpuCopyPending == 0 && vfb.rootGpuCopyPending == 0, "acknowledged: something still held");

    /* a frame: a BGRA client's whole window into a redirected window's pixmap, then the root on screen - in one
     * submission (MAILBOX) */
    CHECK(vkr.presentMode == VK_PRESENT_MODE_MAILBOX_KHR, "the swapchain not MAILBOX");
    submits0 = st.diag.vkQueueSubmits;
    CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &serial, &dstBuffer), "into a redirected window: not queued");
    e = &st.gpuCopyQueue.entries[1];
    CHECK(serial == 2 && e->numRects == 1 && e->rects[0].x1 == 0 && e->rects[0].y1 == 0 && e->rects[0].x2 == 64 && e->rects[0].y2 == 32 &&
          dstBuffer == redirected && redirected->registered && redirected->refs == 1 && redirected->gpuCopyPending == 1 &&
          vfb.rootGpuCopyPending == 0, "into a redirected window: serial %llu, %u rectangles, held %p",
          (unsigned long long) serial, e->numRects, dstBuffer);
    memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
    pushes = 0;
    d0 = emu.draws;
    st.drawRequested = 1;
    rendererVkRedraw(&waiting);
    CHECK(vkFailures == 0 && emu.violations == 0 && st.lock == 0 && !waiting && !st.drawRequested && st.diag.vkFramesPresented == 1,
          "frame: %d failures, %d violations, presented %u", vkFailures, emu.violations, st.diag.vkFramesPresented);
    CHECK(st.diag.vkImportedDestinationDraws == 3 && emu.draws - d0 == 2 && emu.drawnFrom[d0 % 64] == bgra->desc.buffer &&
          emu.drawnInto[d0 % 64] == redirected->desc.buffer && emu.drawnFrom[(d0 + 1) % 64] == root->desc.buffer &&
          !emu.drawnInto[(d0 + 1) % 64], "frame: destination draws %u; the copy, then the root on screen, not drawn",
          st.diag.vkImportedDestinationDraws);
    CHECK(!atFence.lockHeld && inFlightAtFence(3, bgra, redirected, root) && !st.gpuInFlight.busy,
          "frame: at the fence the X server's lock %s, %u buffers in flight (want the copy's two and the root)",
          atFence.lockHeld ? "held" : "free", atFence.busy ? atFence.count : 0);
    st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
    CHECK(st.gpuCopyQueue.readIndex == 2 && st.gpuCopyQueue.completedSerial == 2 && notified.count == 2 && notified.cleared &&
          notified.fenceWaited && notified.released, "frame: readIndex %u, completed %llu", st.gpuCopyQueue.readIndex,
          (unsigned long long) st.gpuCopyQueue.completedSerial);
    CHECK(gles(&saved) == 2 && glDrawCount == 1 && sameAsGles(0, &pushed[0]) && glDraws[0].program == g_texture_program_bgra,
          "frame: BGRA into RGBX not drawn as GLES draws it");
    if (!sameAsGles(0, &pushed[0]))
        printQuads(1);
    CHECK(lorieGpuCopyIsDone(2), "frame: not done");
    lorieGpuCopyAck(&bgraPix, dstBuffer);
    CHECK(bgra->refs == 0 && redirected->refs == 0 && redirected->gpuCopyPending == 0, "frame acknowledged: something still held");
    CHECK(st.diag.vkQueueSubmits == submits0 + 1, "frame with a copy (MAILBOX): %u submissions, want 1", st.diag.vkQueueSubmits - submits0);

    /* a plain desktop frame: nothing queued, no copy drawn; the X server, meanwhile, on its own thread */
    {
        pthread_t x;
        /* the other: AHB id 0 (buffer.c's first id; this test's start at 1), not in the frame - not "every AHB" */
        xThread = (__typeof__(xThread)) { .rootId = root->desc.id, .otherId = 0 };
        gpuMs = 60;
        gpuFinished = false;
        pthread_create(&x, NULL, xServer, NULL);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            if (pthread_timedjoin_np(x, NULL, &deadline)) {
                CHECK(0, "the X server still waiting for the root 3 s after the frame finished");
                st.gpuInFlight.pid = deadPid();     /* lets it give up */
                pthread_join(x, NULL);
            }
        }
        gpuMs = 0;
        CHECK(!atFence.lockHeld && inFlightAtFence(1, root) && !st.gpuInFlight.busy, "plain frame: not the root alone in flight");
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
        CHECK(xThread.started && xThread.otherWhileBusy && xThread.rootAfterGpu && xThread.rootListCleared,
              "the X server meanwhile: %s", !xThread.started ? "never saw the frame in flight" :
              !xThread.otherWhileBusy ? "waited for a buffer the frame does not use" :
              !xThread.rootAfterGpu ? "got the root before the frame finished" : "got the root with the list still set");
    }
    CHECK(st.diag.vkImportedDestinationDraws == 3 && st.gpuCopyQueue.completedSerial == 2 && notified.count == 2 &&
          st.diag.vkFramesPresented == 2 && vkFailures == 0, "plain frame: destination draws %u, completed %llu",
          st.diag.vkImportedDestinationDraws, (unsigned long long) st.gpuCopyQueue.completedSerial);

    /* eight presents before the renderer runs: the queue full, the ninth the X server's own; then one submission */
    {
        RegionRec one = { 1, &boxes[1] };
        uint64_t serials[8], attempts;
        uint32_t submits = st.diag.vkQueueSubmits;
        for (int k = 0; k < 8; k++)
            CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, (int16_t) (k * 10), 0, &serials[k], &dstBuffer), "present %d: not queued", k);
        attempts = gpuCopyAttempts;
        CHECK(!lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer) && st.gpuCopyQueue.writeIndex == 10 &&
              src->refs == 8 && gpuCopyAttempts == attempts + 1 && gpuCopyOffloads == 10, "queue full: queued, or something taken");
        memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
        pushes = 0;
        rendererVkApplyPendingGpuCopies();
        CHECK(st.diag.vkQueueSubmits == submits + 1 && st.diag.vkImportedDestinationDraws == 11 && st.gpuCopyQueue.readIndex == 10 &&
              st.gpuCopyQueue.completedSerial == serials[7] && emu.violations == 0 && lorieGpuCopyIsDone(serials[0]) &&
              lorieGpuCopyIsDone(serials[7]), "eight: submits %u, destination draws %u, completed %llu",
              st.diag.vkQueueSubmits - submits, st.diag.vkImportedDestinationDraws, (unsigned long long) st.gpuCopyQueue.completedSerial);
        CHECK(gles(&saved) == serials[7] && glDrawCount == 8 && glTextures == 4, "eight: GLES drew %d, %d textures made in all (want 4)",
              glDrawCount, glTextures);
        for (int k = 0; k < 8; k++)
            CHECK(sameAsGles(k, &pushed[k]), "eight: copy %d not what GLES draws", k);
        for (int k = 0; k < 8; k++)
            lorieGpuCopyAck(&srcPix, NULL);
        CHECK(src->refs == 0 && vfb.rootGpuCopyPending == 0, "eight acknowledged: something still held");
    }

    /* the most rectangles an entry holds, and the X server's own copy for everything it cannot queue */
    for (int k = 0; k < 17; k++)
        boxes[k] = (BoxRec) { (short) k, (short) k, (short) (k + 2), (short) (k + 3) };
    {
        RegionRec sixteen = { 16, boxes };
        uint32_t submits = st.diag.vkQueueSubmits, dests = st.diag.vkImportedDestinationDraws;
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &sixteen, 1, 2, &serial, &dstBuffer) && st.gpuCopyQueue.entries[10 % 8].numRects == 16,
              "sixteen rectangles: not queued");
        memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
        pushes = 0;
        rendererVkApplyPendingGpuCopies();
        CHECK(st.diag.vkQueueSubmits == submits + 1 && st.diag.vkImportedDestinationDraws == dests + 16 && lorieGpuCopyIsDone(serial),
              "sixteen rectangles: %u drawn", st.diag.vkImportedDestinationDraws - dests);
        CHECK(gles(&saved) == serial && glDrawCount == 16, "sixteen: GLES drew %d", glDrawCount);
        for (int k = 0; k < 16; k++)
            CHECK(sameAsGles(k, &pushed[k]), "sixteen: copy %d not what GLES draws", k);
        lorieGpuCopyAck(&srcPix, dstBuffer);
    }
    {
        static const char *const what[] = { "-disable-gpu-present", "legacy drawing", "no connection", "no surface", "a source not an AHB",
                                             "a source in a client's memory", "a destination not an AHB", "no rectangles", "17 rectangles" };
        for (int k = 0; k < 9; k++) {
            RegionRec region = { k == 7 ? 0 : k == 8 ? 17 : 1, boxes };
            uint32_t writeIndex = st.gpuCopyQueue.writeIndex;
            uint64_t offloads = gpuCopyOffloads, attempts = gpuCopyAttempts, pending = vfb.rootGpuCopyPending;
            int woken = wakeups;
            PixmapPtr from = k == 4 ? &fdPix : k == 5 ? &memPix : &freshPix, to = k == 6 ? &fdPix : &rootPix;
            vfb.gpuPresentDisabled = k == 0;
            vfb.root.legacyDrawing = k == 1;
            connectionAlive = k != 2;
            st.surfaceAvailable = k != 3;
            CHECK(!lorieTryScheduleGpuCopy(from, to, &region, 0, 0, &serial, &dstBuffer), "%s: queued", what[k]);
            CHECK(st.gpuCopyQueue.writeIndex == writeIndex && gpuCopyOffloads == offloads && gpuCopyAttempts == attempts + 1 &&
                  wakeups == woken && vfb.rootGpuCopyPending == pending && !fresh->registered && fresh->refs == 0 &&
                  fresh->gpuCopyPending == 0 && fd->refs == 0, "%s: something queued, registered or held", what[k]);
        }
        vfb.gpuPresentDisabled = vfb.root.legacyDrawing = FALSE;
        connectionAlive = true;
        st.surfaceAvailable = 1;
    }

    /* the X server no longer answers PER_ACCESS (a new connection): nothing touched, ownership asked for again */
    {
        RegionRec one = { 1, boxes };
        uint32_t submits = st.diag.vkQueueSubmits, readIndex = st.gpuCopyQueue.readIndex, dests;
        int acquires = emu.acquires;
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "not owned: not queued");
        st.backend.ownershipAck = LORIE_OWNERSHIP_WORD(8, LORIE_OWNERSHIP_PERSISTENT);
        rendererVkApplyPendingGpuCopies();
        CHECK(st.diag.vkQueueSubmits == submits && emu.acquires == acquires && st.gpuCopyQueue.readIndex == readIndex &&
              !lorieGpuCopyIsDone(serial) && !vkOwned && backendSelect && st.lock == 0, "copies alone, not owned: something touched");
        own(9);
        st.backend.ownershipAck = LORIE_OWNERSHIP_WORD(10, LORIE_OWNERSHIP_PERSISTENT);
        dests = st.diag.vkImportedDestinationDraws;
        backendSelect = false;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(st.diag.vkImportedDestinationDraws == dests && emu.acquires == acquires && st.gpuCopyQueue.readIndex == readIndex &&
              !lorieGpuCopyIsDone(serial) && !vkOwned && backendSelect && st.lock == 0 && emu.violations == 0,
              "frame, not owned: readIndex %u (was %u), copies drawn %u", st.gpuCopyQueue.readIndex, readIndex,
              st.diag.vkImportedDestinationDraws - dests);

        CHECK(!atFence.busy, "frame, not owned: %u buffers in flight", atFence.count);

        /* owned again, the fence does not signal: the device waited out, the list cleared, nothing published */
        own(12);
        {
            int idle = emu.deviceWaits;
            emu.fenceResult = VK_TIMEOUT;
            rendererVkApplyPendingGpuCopies();
            emu.fenceResult = VK_SUCCESS;
            CHECK(vkFailures == 1 && emu.deviceWaits == idle + 1 && !st.gpuInFlight.busy && st.gpuCopyQueue.readIndex == readIndex &&
                  !lorieGpuCopyIsDone(serial) && emu.violations == 0, "late fence: device waits %d, list %s, readIndex %u (was %u)",
                  emu.deviceWaits - idle, st.gpuInFlight.busy ? "set" : "cleared", st.gpuCopyQueue.readIndex, readIndex);
            st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
            vkFailures = 0;
            emu.fencePending = false;
        }

        /* the fence lost (and the device): over - the device's external memory is other APIs' again, content undefined */
        own(13);
        emu.fenceResult = emu.idleResult = VK_ERROR_DEVICE_LOST;
        rendererVkApplyPendingGpuCopies();
        emu.fenceResult = emu.idleResult = VK_SUCCESS;
        CHECK(vkFailures == 1 && !st.gpuInFlight.busy && st.gpuCopyQueue.readIndex == readIndex && !lorieGpuCopyIsDone(serial),
              "lost device: list %s, readIndex %u (was %u)", st.gpuInFlight.busy ? "kept" : "cleared", st.gpuCopyQueue.readIndex, readIndex);
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
        vkFailures = 0;
        emu.fencePending = false;

        /* the fence late, vkDeviceWaitIdle out of memory: not known to be over, the list kept; teardown asks again */
        own(14);
        {
            int idle;
            emu.fenceResult = VK_TIMEOUT;
            emu.idleFailures = 3;
            rendererVkApplyPendingGpuCopies();
            emu.fenceResult = VK_SUCCESS;
            CHECK(vkFailures == 1 && st.gpuInFlight.busy && st.gpuInFlight.pid == getpid() && !lorieGpuCopyIsDone(serial),
                  "device not known idle: list %s", st.gpuInFlight.busy ? "kept" : "cleared");
            /* nor its fence reset: it may still be associated with the submission (VUID-vkResetFences-pFences-01123) */
            CHECK(signaled(vkr.frameFence) && *signaled(vkr.frameFence) && emu.violations == 0,
                  "device not known idle: the fence reset, %d violations", emu.violations);
            idle = emu.deviceWaits;
            rendererVkIdle();           /* rendererVkFail's teardown, before anything else */
            rendererVkFrameDone();
            vk.ResetFences(vk.device, 1, &vkr.frameFence);  /* the teardown destroys it; the same renderer goes on here */
            CHECK(emu.deviceWaits == idle + 3 && !st.gpuInFlight.busy, "teardown: vkDeviceWaitIdle asked %d times (want 3), list %s",
                  emu.deviceWaits - idle, st.gpuInFlight.busy ? "kept" : "cleared");
            st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
            emu.idleFailures = 0;
            vkFailures = 0;
        }

        /* owned again, the submission fails: nothing published */
        own(11);
        emu.submitResult = VK_ERROR_DEVICE_LOST;
        rendererVkApplyPendingGpuCopies();
        CHECK(vkFailures == 1 && st.gpuCopyQueue.readIndex == readIndex && !lorieGpuCopyIsDone(serial) && st.lock == 0,
              "failed submission: readIndex %u (was %u), done %d", st.gpuCopyQueue.readIndex, readIndex, lorieGpuCopyIsDone(serial));
        emu.submitResult = VK_SUCCESS;
        vkFailures = 0;
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(serial) && st.gpuCopyQueue.readIndex == readIndex + 1, "after the failure: not done");
        lorieGpuCopyAck(&srcPix, dstBuffer);
    }

    /* an entry whose source the renderer never got: skipped and completed, as GLES does - once the pass's wait for it
     * (rendererAwaitBuffers, before the copies, as rendererPass has it) gave it up */
    {
        RegionRec one = { 1, boxes };
        uint32_t dests = st.diag.vkImportedDestinationDraws;
        CHECK(lorieTryScheduleGpuCopy(&freshPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "never got: not queued");
        fresh->registered = false;
        memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
        rendererAwaitBuffers();
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(serial) && st.diag.vkImportedDestinationDraws == dests && vkFailures == 0 &&
              gles(&saved) == serial && glDrawCount == 0 && !fresh->glTexture, "never got: not skipped as GLES skips it");
        lorieGpuCopyAck(&freshPix, dstBuffer);
    }

    /* frames of eight copies from new sources: the cache grows past 16 and 32 images in the middle of a frame */
    {
        enum { SOURCES = 40 };
        LorieBuffer *many[SOURCES];
        LoriePixmapPriv manyP[SOURCES];
        PixmapRec manyPix[SOURCES];
        struct lorie_vk_image *kept[SOURCES], *rootImage = lorieVkImageFind(&vkImages, root->desc.id);
        VkImage keptImage[SOURCES];
        RegionRec one = { 1, boxes };
        bool crossed16 = false, crossed32 = false;
        for (int k = 0; k < SOURCES; k++) {
            many[k] = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 32, 16);
            manyP[k] = (LoriePixmapPriv) { many[k] };
            manyPix[k] = (PixmapRec) { { 32, 16 }, &manyP[k] };
        }
        CHECK(rootImage && vkImages.count < 16, "many: %u images before", vkImages.count);
        for (int f = 0; f < SOURCES / 8; f++) {
            uint32_t before = vkImages.count;
            int acquires = emu.acquires, releases = emu.releases, violations = emu.violations;
            for (int k = f * 8; k < f * 8 + 8; k++)
                CHECK(lorieTryScheduleGpuCopy(&manyPix[k], &rootPix, &one, (int16_t) k, 0, &serial, &dstBuffer), "many: copy %d not queued", k);
            d0 = emu.draws;
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            crossed16 |= before < 16 && vkImages.count > 16;
            crossed32 |= before < 32 && vkImages.count > 32;
            /* eight sources and the root, each acquired once and released once */
            CHECK(vkFailures == 0 && emu.violations == violations && emu.acquires - acquires == 9 && emu.releases - releases == 9 &&
                  lorieGpuCopyIsDone(serial), "many, frame %d (%u -> %u images): %d violations, %d acquires, %d releases", f, before,
                  vkImages.count, emu.violations - violations, emu.acquires - acquires, emu.releases - releases);
            for (int k = 0; k < 8; k++)
                CHECK(emu.drawnFrom[(d0 + k) % 64] == many[f * 8 + k]->desc.buffer && emu.drawnInto[(d0 + k) % 64] == root->desc.buffer,
                      "many, frame %d: copy %d not drawn from its source into the root", f, k);
            CHECK(emu.drawnFrom[(d0 + 8) % 64] == root->desc.buffer, "many, frame %d: the root not drawn", f);
            for (int k = f * 8; k < f * 8 + 8; k++) {
                kept[k] = lorieVkImageFind(&vkImages, many[k]->desc.id);
                keptImage[k] = kept[k] ? kept[k]->image : VK_NULL_HANDLE;
                lorieGpuCopyAck(&manyPix[k], dstBuffer);
            }
        }
        CHECK(crossed16 && crossed32, "many: the cache did not pass 16 and 32 images in a frame");
        for (int k = 0; k < SOURCES; k++)
            CHECK(!many[k]->glTexture, "many: a GLES texture made for source %d, which only Vulkan used", k);
        CHECK(lorieVkImageFind(&vkImages, root->desc.id) == rootImage, "many: the root's image moved");
        for (int k = 0; k < SOURCES; k++)
            CHECK(kept[k] && lorieVkImageFind(&vkImages, many[k]->desc.id) == kept[k] && kept[k]->image == keptImage[k] &&
                  kept[k]->id == many[k]->desc.id, "many: source %d's image moved or changed", k);
        /* removing one leaves every other where it was */
        lorieVkImageRemove(&vk, &vkImages, many[0]->desc.id);
        CHECK(!lorieVkImageFind(&vkImages, many[0]->desc.id) && lorieVkImageFind(&vkImages, root->desc.id) == rootImage,
              "many: a removal moved another image");
        for (int k = 1; k < SOURCES; k++)
            CHECK(lorieVkImageFind(&vkImages, many[k]->desc.id) == kept[k], "many: source %d moved by a removal", k);
    }

    /* FIFO (smooth): the copies first, in a submission of their own, then the frame - the copies not held up by an
     * image the display may give back only at its next refresh */
    {
        RegionRec one = { 1, boxes };
        own(30);
        vkr.smooth = true;
        vkr.recreate = true;                    /* as rendererVkApplyPresentMode */
        submits0 = st.diag.vkQueueSubmits;
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "FIFO: not queued");
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR && st.diag.vkQueueSubmits == submits0 + 2 && lorieGpuCopyIsDone(serial) &&
              vkFailures == 0 && emu.violations == 0, "FIFO frame with a copy: %u submissions (want 2), done %d",
              st.diag.vkQueueSubmits - submits0, lorieGpuCopyIsDone(serial));
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
        lorieGpuCopyAck(&srcPix, dstBuffer);
        vkr.smooth = false;
        vkr.recreate = true;
    }

    /* P11-C1, TERMUX_X11_RENDERER_VK_SUBMIT_OPT=1 - still one submission in flight at a time (the fake device fails
     * a submission made before the last one's fence was waited for) */
    st.backend.vkSubmitOpt = 1;
    {
        int waits, presents, early = emu.earlyPresents, n0, d1, fails0 = vkFailures;
        uint32_t ahead = st.diag.vkCopiesAhead, earlyCount = st.diag.vkEarlyPresents, presented;

        /* MAILBOX, a copy queued before the frame: ahead of it, in a submission of its own, published at its own
         * fence - its buffers alone in flight then, before the frame was submitted or presented; the frame,
         * carrying no copy, presented before its fence was waited for, the root alone in flight */
        own(31);
        rendererVkRedraw(&waiting);             /* the present mode asked for above made again: MAILBOX */
        st.gpuInFlight.busy = 0;
        CHECK(vkr.presentMode == VK_PRESENT_MODE_MAILBOX_KHR, "submit opt: the swapchain not MAILBOX");
        submits0 = st.diag.vkQueueSubmits;
        ahead = st.diag.vkCopiesAhead;
        earlyCount = st.diag.vkEarlyPresents;
        early = emu.earlyPresents;
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &serial, &dstBuffer), "submit opt: not queued");
        waits = atFence.waits, presents = emu.presents, n0 = notified.count, d1 = emu.draws;
        presented = st.diag.vkFramesPresented;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(vkFailures == fails0 && emu.violations == 0 && st.lock == 0 && st.diag.vkQueueSubmits == submits0 + 2 &&
              st.diag.vkCopiesAhead == ahead + 1 && st.diag.vkFramesPresented == presented + 1,
              "submit opt, a copy queued before the frame: %d failures, %d violations, %u submissions (want 2), %u ahead, %u presented",
              vkFailures - fails0, emu.violations, st.diag.vkQueueSubmits - submits0, st.diag.vkCopiesAhead - ahead,
              st.diag.vkFramesPresented - presented);
        CHECK(lorieGpuCopyIsDone(serial) && notified.count == n0 + 1 && notified.fenceWaits == waits + 1 && notified.presents == presents &&
              notified.cleared && notified.released && notified.fenceWaited,
              "submit opt, a copy queued before the frame: published %d times, after %d fences (want 1), %d presents (want 0), "
              "in flight %s", notified.count - n0, notified.fenceWaits - waits, notified.presents - presents,
              notified.cleared ? "cleared" : "not cleared");
        CHECK(atFence.waits == waits + 2 && inFlightBeforeLast(2, bgra, redirected) && inFlightAtFence(1, root) &&
              atFence.presents == presents + 1 && emu.earlyPresents == early + 1 && st.diag.vkEarlyPresents == earlyCount + 1,
              "submit opt, a copy queued before the frame: the copy's fence with %u buffers (want its two), the frame's with %u "
              "(want the root), presented %d times before the frame's fence (want 1)", atFence.prev.count, atFence.count,
              atFence.presents - presents);
        CHECK(emu.draws - d1 == 2 && emu.drawnFrom[d1 % 64] == bgra->desc.buffer && emu.drawnInto[d1 % 64] == redirected->desc.buffer &&
              emu.drawnFrom[(d1 + 1) % 64] == root->desc.buffer && !emu.drawnInto[(d1 + 1) % 64],
              "submit opt, a copy queued before the frame: not the copy, then the root on screen");
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&bgraPix, dstBuffer);

        /* a copy queued while the frame's image is acquired (after the copies went ahead): in the frame - which is
         * presented only once that copy is published, as with the option off */
        duringAcquire = (__typeof__(duringAcquire)) { &bgraPix, &redirectedPix };
        emuOnAcquire = queueDuringAcquire;
        submits0 = st.diag.vkQueueSubmits;
        ahead = st.diag.vkCopiesAhead, early = emu.earlyPresents, presents = emu.presents, n0 = notified.count;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        emuOnAcquire = NULL;
        CHECK(duringAcquire.queued && lorieGpuCopyIsDone(duringAcquire.serial) && st.diag.vkQueueSubmits == submits0 + 1 &&
              st.diag.vkCopiesAhead == ahead && notified.count == n0 + 1 && notified.presents == presents &&
              emu.presents == presents + 1 && emu.earlyPresents == early && inFlightAtFence(3, bgra, redirected, root) &&
              vkFailures == fails0 && emu.violations == 0,
              "submit opt, a copy queued during the acquire: queued %d, done %d, %u submissions (want 1), published after %d "
              "presents (want 0), %d early presents (want 0)", duringAcquire.queued, lorieGpuCopyIsDone(duringAcquire.serial),
              st.diag.vkQueueSubmits - submits0, notified.presents - presents, emu.earlyPresents - early);
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&bgraPix, duringAcquire.dstBuffer);

        /* the image not given (the acquire timing out): the copy queued is published all the same - with the option
         * off it waits for the next frame that gets an image */
        for (int v = 0; v < 2; v++) {
            uint64_t s;
            void *db;
            st.backend.vkSubmitOpt = (uint8_t) v;
            CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 0, 0, &s, &db), "acquire timing out: not queued");
            emu.acquireResult = VK_TIMEOUT;
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            emu.acquireResult = VK_SUCCESS;
            CHECK(lorieGpuCopyIsDone(s) == (v == 1) && vkFailures == fails0 && emu.violations == 0,
                  "acquire timing out, the option %s: the copy %s", v ? "on" : "off", lorieGpuCopyIsDone(s) ? "done" : "not done");
            rendererVkRedraw(&waiting);         /* the image given: the copy done either way */
            CHECK(lorieGpuCopyIsDone(s) && vkFailures == fails0 && emu.violations == 0, "acquire timing out, the option %s: never done",
                  v ? "on" : "off");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&bgraPix, db);
        }
        st.backend.vkSubmitOpt = 1;

        /* an entry the X server cancelled: ahead too, skipped and completed, nothing drawn from it */
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 0, 0, &serial, &dstBuffer), "cancelled: not queued");
        st.gpuCopyQueue.entries[(st.gpuCopyQueue.writeIndex - 1) % LORIE_GPU_COPY_QUEUE_CAPACITY].cancelled = 1;
        d1 = emu.draws;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(lorieGpuCopyIsDone(serial) && emu.draws - d1 == 1 && emu.drawnFrom[d1 % 64] == root->desc.buffer && vkFailures == fails0 &&
              emu.violations == 0, "cancelled: done %d, %d draws (want the root's alone)", lorieGpuCopyIsDone(serial), emu.draws - d1);
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&bgraPix, dstBuffer);

        /* presented early, out of date: the frame's fence still waited for, its buffers given back, the swapchain made
         * again for the next frame - no failure */
        {
            int made = emu.swapchainsMade;
            emu.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
            early = emu.earlyPresents;
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            emu.presentResult = VK_SUCCESS;
            CHECK(emu.earlyPresents == early + 1 && vkr.recreate && !st.gpuInFlight.busy && !emu.fencePending && vkFailures == fails0 &&
                  emu.violations == 0, "early present out of date: recreate %d, in flight %d, fence %s, %d failures", vkr.recreate,
                  st.gpuInFlight.busy, emu.fencePending ? "not waited for" : "waited for", vkFailures - fails0);
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            CHECK(emu.swapchainsMade == made + 1 && !vkr.recreate && vkFailures == fails0 && emu.violations == 0,
                  "early present out of date: the swapchain not made again by the next frame");
        }

        /* presented early, the surface lost: the renderer gives up only once the frame's fence was waited for and its
         * buffers given back */
        emu.presentResult = VK_ERROR_SURFACE_LOST_KHR;
        st.drawRequested = 1;
        atFail.fencePending = atFail.busy = true;
        rendererVkRedraw(&waiting);
        emu.presentResult = VK_SUCCESS;
        CHECK(vkFailures == fails0 + 1 && !atFail.fencePending && !atFail.busy && emu.violations == 0,
              "early present failing: %d failures, at the failure the fence %s and the buffers %s", vkFailures - fails0,
              atFail.fencePending ? "not waited for" : "waited for", atFail.busy ? "still in flight" : "given back");
        vkFailures = fails0;
        st.diag.lastFallbackClass = st.diag.lastFallbackReason = 0;

        /* FIFO: the copies went ahead already - not counted as the option's */
        own(32);
        vkr.smooth = true;
        vkr.recreate = true;
        ahead = st.diag.vkCopiesAhead;
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 0, 0, &serial, &dstBuffer), "FIFO with the option: not queued");
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(vkr.presentMode == VK_PRESENT_MODE_FIFO_KHR && lorieGpuCopyIsDone(serial) && st.diag.vkCopiesAhead == ahead &&
              vkFailures == fails0 && emu.violations == 0, "FIFO with the option: done %d, counted ahead %u", lorieGpuCopyIsDone(serial),
              st.diag.vkCopiesAhead - ahead);
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&bgraPix, dstBuffer);
        vkr.smooth = false;
        vkr.recreate = true;
    }
    st.backend.vkSubmitOpt = 0;
    earlyWithOption = emu.earlyPresents;

    /* P11-C2: root double buffering's copy by the GPU (rendererVkRootCopy) - taken under the X server's lock, drawn
     * from the buffer the X server drew into into the other as GPU copies are drawn (each buffer acquired from the
     * foreign owner and given back), both listed in flight while the device runs it and the lock let go, settled as
     * copied only once its fence signalled (and the X server woken then); settled as not copied - nothing drawn,
     * for the X server to copy - without the PER_ACCESS answer, with a buffer the renderer does not have, with Vulkan
     * not running; a failed submission or device leaves it not copied too, the buffers out of flight first */
    {
        LorieBuffer *ra = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48), *rb = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
        static const LorieGpuCopyRect one[1] = { { 3, 5, 40, 30 } }, two[2] = { { 0, 0, 64, 10 }, { 8, 20, 30, 48 } };
        int fails0 = vkFailures, d1, waits;
        uint32_t submits = st.diag.vkQueueSubmits;

        ra->registered = rb->registered = true;
        fill(ra, 1, 0x22);
        fill(rb, 2, 0x11);
        own(41);
        rootAsk(ra, rb, one, 1);
        CHECK(rendererRootCopyPending(), "root copy: not pending once asked for");
        d1 = emu.draws, waits = atFence.waits;
        pushes = 0;                             /* the fake device keeps the first 64 */
        rendererVkRootCopy();
        CHECK(st.rootCopy.taken == rootSeq && st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && !rendererRootCopyPending() &&
              vkFailures == fails0 && emu.violations == 0 && st.lock == 0, "root copy: taken %u, done %u, failed %u (want %u, %u, not it)",
              st.rootCopy.taken, st.rootCopy.done, st.rootCopy.failed, rootSeq, rootSeq);
        CHECK(rootQuadsExact(ra, rb, one, 1) && emu.draws - d1 == 1 && emu.drawnFrom[d1 % 64] == ra->desc.buffer &&
              emu.drawnInto[d1 % 64] == rb->desc.buffer, "root copy: the draw not the rectangle 1:1, %d draws", emu.draws - d1);
        CHECK(atFence.waits == waits + 1 && !atFence.lockHeld && inFlightAtFence(2, ra, rb) && !st.gpuInFlight.busy &&
              !ra->desc.buffer->owned && !rb->desc.buffer->owned && st.diag.vkQueueSubmits == submits + 1,
              "root copy: at its fence the lock %s, %u buffers in flight (want the two), given back %d", atFence.lockHeld ? "held" : "free",
              atFence.count, !ra->desc.buffer->owned && !rb->desc.buffer->owned);
        CHECK(st.diag.rootGpuCopySubmits == 1 && st.diag.rootGpuCopyCompletions == 1 && st.diag.rootGpuCopyRects == 1 &&
              st.diag.rootGpuCopyBytes == 37 * 25 * 4, "root copy: counted %u submitted, %u completed, %u rectangles, %llu bytes",
              st.diag.rootGpuCopySubmits, st.diag.rootGpuCopyCompletions, st.diag.rootGpuCopyRects,
              (unsigned long long) st.diag.rootGpuCopyBytes);

        /* two rectangles, the other way: the swap after the swap */
        fill(ra, 3, 0x11);
        rootAsk(rb, ra, two, 2);
        pushes = 0;
        rendererVkRootCopy();
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && rootQuadsExact(rb, ra, two, 2) &&
              emu.drawnFrom[(emu.draws - 1) % 64] == rb->desc.buffer && emu.drawnInto[(emu.draws - 1) % 64] == ra->desc.buffer &&
              vkFailures == fails0 && emu.violations == 0, "root copy, two rectangles: not drawn as they are");

        /* a request the X server took back: nothing done */
        rootAsk(ra, rb, one, 1);
        st.rootCopy.cancelled = rootSeq;
        d1 = emu.draws;
        CHECK(!rendererRootCopyPending(), "root copy taken back: still pending");
        rendererVkRootCopy();
        CHECK(st.rootCopy.taken != rootSeq && st.rootCopy.done != rootSeq && emu.draws == d1, "root copy taken back: taken or drawn");

        /* without the PER_ACCESS answer: not copied, nothing drawn, the answer asked for again */
        fill(rb, 2, 0x11);
        rootAsk(ra, rb, one, 1);
        vkOwned = false;
        d1 = emu.draws;
        rendererVkRootCopy();
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && emu.draws == d1 && backendSelect && st.lock == 0, "root copy, not owned: settled %d, failed %d, %d draws", st.rootCopy.done == rootSeq, st.rootCopy.failed == rootSeq,
              emu.draws - d1);
        backendSelect = false;
        own(42);

        /* a buffer the renderer does not have (a root gone): not copied, Vulkan going on */
        rootAsk(ra, rb, one, 1);
        st.rootCopy.dstId = 99999;
        d1 = emu.draws;
        rendererVkRootCopy();
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && emu.draws == d1 && vkFailures == fails0,
              "root copy, a buffer missing: not settled as not copied, or Vulkan given up");

        /* Vulkan not running: declined, under the lock */
        rootAsk(ra, rb, one, 1);
        rendererRootCopyDecline();
        CHECK(st.rootCopy.taken == rootSeq && st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && st.lock == 0,
              "root copy declined: not settled as not copied");

        /* the submission failing: not copied, nothing in flight, Vulkan given up */
        rootAsk(ra, rb, one, 1);
        emu.submitResult = VK_ERROR_DEVICE_LOST;
        rendererVkRootCopy();
        emu.submitResult = VK_SUCCESS;
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && !st.gpuInFlight.busy && vkFailures == fails0 + 1,
              "root copy, submission failing: settled %d failed %d, in flight %d, %d failures",
              st.rootCopy.done == rootSeq, st.rootCopy.failed == rootSeq, st.gpuInFlight.busy, vkFailures - fails0);
        vkFailures = fails0;

        /* the device lost at the fence: not copied (its contents undefined), settled once the buffers are out of flight */
        rootAsk(ra, rb, one, 1);
        emu.fenceResult = VK_ERROR_DEVICE_LOST;
        atFail.busy = true;
        rendererVkRootCopy();
        emu.fenceResult = VK_SUCCESS;
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && !st.gpuInFlight.busy && vkFailures == fails0 + 1 && !atFail.busy,
              "root copy, device lost: settled %d failed %d, in flight %d at the failure", st.rootCopy.done == rootSeq,
              st.rootCopy.failed == rootSeq, atFail.busy);
        vkFailures = fails0;
        st.diag.lastFallbackClass = st.diag.lastFallbackReason = 0;

        /* the X server waiting on its futex while the device takes its time: woken once the copy is done - after the
         * fence, never before */
        {
            pthread_t x;
            struct timespec deadline;
            fill(rb, 2, 0x11);
            rootAsk(ra, rb, one, 1);
            rootWaiter = (__typeof__(rootWaiter)) { rootSeq };
            gpuMs = 60;
            gpuFinished = false;
            pthread_create(&x, NULL, rootWaiterThread, NULL);
            usleep(10000);
            CHECK(!rootWaiter.woke, "root copy waited for: the X server woken before anything was done");
            rendererVkRootCopy();
            gpuMs = 0;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 3;
            if (pthread_timedjoin_np(x, NULL, &deadline)) {
                CHECK(0, "root copy waited for: the X server still waiting 3 s after it was done");
                st.rootCopy.done = rootSeq;
                syscall(SYS_futex, &st.rootCopy.done, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
                pthread_join(x, NULL);
            }
            CHECK(rootWaiter.woke && !rootWaiter.timedOut && rootWaiter.gpuOverAtWake && st.rootCopy.failed != rootSeq,
                  "root copy waited for: woken %d, timed out %d, the device done at the wake %d", rootWaiter.woke, rootWaiter.timedOut,
                  rootWaiter.gpuOverAtWake);
        }
        ra->registered = rb->registered = false;
    }

    /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG=1: each fence in its own timing - a root copy's 30 ms in fence_root
     * alone, the copies a frame sends ahead (TERMUX_X11_RENDERER_VK_SUBMIT_OPT) 10 ms in fence_copy alone, the frame's
     * 20 ms in fence_frame alone: one sample each, together exactly what vk_fence_wait_us counts; the root copy right
     * before the frame as its after_root, once; its parts, and when it was settled (for the X server's resume); a
     * tick's request before the frame as its wake; the first frame no interval, the next one; copies in the frame's own
     * submission (the option off) in the frame's fence. Off: nothing sampled, nothing stamped, the frames as on. */
    {
        LorieBuffer *ra = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48), *rb = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
        static const LorieGpuCopyRect one[1] = { { 3, 5, 40, 30 } };
        volatile struct lorie_renderer_diag *dg = &st.diag;
        struct histMark fRoot, fCopy, fFrame, after, take, rec, wake, start, delta, cpu, present;
        uint64_t total, queuedUs;
        int fails0 = vkFailures, early = emu.earlyPresents;
        uint32_t presented, samples;

        ra->registered = rb->registered = true;
        own(43);
        st.backend.vkFrameDiag = 1;
        st.backend.vkSubmitOpt = 1;
        rendererVkPacingReset();
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);             /* the swapchain the frames below are timed on */
        rendererVkPacingReset();
        st.gpuInFlight.busy = 0;
        fRoot = mark(&dg->vkFenceRoot), fCopy = mark(&dg->vkFenceCopy), fFrame = mark(&dg->vkFrameFence);
        after = mark(&dg->vkFrameAfterRoot), take = mark(&dg->rootTake), rec = mark(&dg->rootRecord), wake = mark(&dg->vkFrameWake);
        start = mark(&dg->vkFrameStart), delta = mark(&dg->vkPresentDelta), cpu = mark(&dg->vkFrameCpu);
        present = mark(&dg->vkFramePresent);
        total = dg->vkFenceWaitUs;
        st.rootCopy.doneUs = 0;
        fenceMs[0] = 30, fenceMs[1] = 10, fenceMs[2] = 20, fenceMsCount = 3, fenceMsAt = 0;
        rootAsk(ra, rb, one, 1);
        queuedUs = st.rootCopy.queuedUs;
        vkRootCopyBusyUs = 0;                   /* as the main loop does before each pass's root copy */
        rendererVkRootCopy();
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && st.rootCopy.doneUs >= queuedUs &&
              newSamples(&dg->rootTake, take) == 1 && newSamples(&dg->rootRecord, rec) == 1 && newSamples(&dg->vkFenceRoot, fRoot) == 1 &&
              dg->rootWorstUs >= 30000 && dg->rootWorstFenceUs >= 30000,
              "frame diag, root copy: settled %d, stamped %d, take %u record %u fence %u samples, worst %u us fence %u us",
              st.rootCopy.done == rootSeq, st.rootCopy.doneUs >= queuedUs, newSamples(&dg->rootTake, take),
              newSamples(&dg->rootRecord, rec), newSamples(&dg->vkFenceRoot, fRoot), dg->rootWorstUs, dg->rootWorstFenceUs);
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &serial, &dstBuffer), "frame diag: not queued");
        presented = st.diag.vkFramesPresented;
        st.frameWantedUs = lorieMonotonicUs();  /* the tick asking for it */
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        fenceMsCount = 0;
        st.gpuInFlight.busy = 0;
        CHECK(vkFailures == fails0 && emu.violations == 0 && st.diag.vkFramesPresented == presented + 1 && lorieGpuCopyIsDone(serial),
              "frame diag: the frame not presented, or its copy not done");
        CHECK(newSamples(&dg->vkFenceRoot, fRoot) == 1 && newSamples(&dg->vkFenceCopy, fCopy) == 1 &&
              newSamples(&dg->vkFrameFence, fFrame) == 1 && newUs(&dg->vkFenceRoot, fRoot) >= 30000 &&
              newUs(&dg->vkFenceCopy, fCopy) >= 10000 && newUs(&dg->vkFrameFence, fFrame) >= 20000 &&
              newUs(&dg->vkFenceRoot, fRoot) + newUs(&dg->vkFenceCopy, fCopy) + newUs(&dg->vkFrameFence, fFrame) == dg->vkFenceWaitUs - total,
              "frame diag, fences: root %u samples %llu us, copy %u %llu us, frame %u %llu us; vk_fence_wait_us moved %llu us",
              newSamples(&dg->vkFenceRoot, fRoot), (unsigned long long) newUs(&dg->vkFenceRoot, fRoot),
              newSamples(&dg->vkFenceCopy, fCopy), (unsigned long long) newUs(&dg->vkFenceCopy, fCopy),
              newSamples(&dg->vkFrameFence, fFrame), (unsigned long long) newUs(&dg->vkFrameFence, fFrame),
              (unsigned long long) (dg->vkFenceWaitUs - total));
        CHECK(newSamples(&dg->vkFrameAfterRoot, after) == 1 && newUs(&dg->vkFrameAfterRoot, after) >= 30000 &&
              newSamples(&dg->vkFrameWake, wake) == 1 && newSamples(&dg->vkFrameStart, start) == 0 &&
              newSamples(&dg->vkPresentDelta, delta) == 0 && newSamples(&dg->vkFrameCpu, cpu) == 1 &&
              newSamples(&dg->vkFramePresent, present) == 1 && newUs(&dg->vkFrameCpu, cpu) >= 30000,
              "frame diag, the frame: after_root %u (%llu us), wake %u, start %u and present delta %u (want 0: the first), cpu %u "
              "(%llu us), present %u", newSamples(&dg->vkFrameAfterRoot, after),
              (unsigned long long) newUs(&dg->vkFrameAfterRoot, after), newSamples(&dg->vkFrameWake, wake),
              newSamples(&dg->vkFrameStart, start), newSamples(&dg->vkPresentDelta, delta), newSamples(&dg->vkFrameCpu, cpu),
              (unsigned long long) newUs(&dg->vkFrameCpu, cpu), newSamples(&dg->vkFramePresent, present));
        lorieGpuCopyAck(&bgraPix, dstBuffer);

        /* the next frame: no root copy before it, the tick's request already taken up - an interval now */
        after = mark(&dg->vkFrameAfterRoot), wake = mark(&dg->vkFrameWake);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(newSamples(&dg->vkFrameAfterRoot, after) == 0 && newSamples(&dg->vkFrameWake, wake) == 0 &&
              newSamples(&dg->vkFrameStart, start) == 1 && newSamples(&dg->vkPresentDelta, delta) == 1,
              "frame diag, the next frame: after_root %u, wake %u (want 0 each), start %u, present delta %u (want 1 each)",
              newSamples(&dg->vkFrameAfterRoot, after), newSamples(&dg->vkFrameWake, wake), newSamples(&dg->vkFrameStart, start),
              newSamples(&dg->vkPresentDelta, delta));

        /* the option off: the copy in the frame's submission, its time in the frame's fence */
        st.backend.vkSubmitOpt = 0;
        fCopy = mark(&dg->vkFenceCopy), fFrame = mark(&dg->vkFrameFence);
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &serial, &dstBuffer), "frame diag: not queued");
        fenceMs[0] = 15, fenceMsCount = 1, fenceMsAt = 0;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        fenceMsCount = 0;
        st.gpuInFlight.busy = 0;
        CHECK(lorieGpuCopyIsDone(serial) && newSamples(&dg->vkFenceCopy, fCopy) == 0 && newSamples(&dg->vkFrameFence, fFrame) == 1 &&
              newUs(&dg->vkFrameFence, fFrame) >= 15000, "frame diag, copies in the frame: copy fence %u samples, frame fence %u "
              "(%llu us)", newSamples(&dg->vkFenceCopy, fCopy), newSamples(&dg->vkFrameFence, fFrame),
              (unsigned long long) newUs(&dg->vkFrameFence, fFrame));
        lorieGpuCopyAck(&bgraPix, dstBuffer);

        /* off: the same root copy, copy and frame - nothing sampled or stamped */
        st.backend.vkFrameDiag = 0;
        st.backend.vkSubmitOpt = 1;
        samples = histSamples();
        st.rootCopy.doneUs = 0;
        presented = st.diag.vkFramesPresented;
        total = dg->vkFenceWaitUs;
        fenceMs[0] = fenceMs[1] = fenceMs[2] = 2, fenceMsCount = 3, fenceMsAt = 0;
        rootAsk(ra, rb, one, 1);
        vkRootCopyBusyUs = 0;
        rendererVkRootCopy();
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &serial, &dstBuffer), "frame diag off: not queued");
        st.frameWantedUs = lorieMonotonicUs();
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        fenceMsCount = 0;
        st.gpuInFlight.busy = 0;
        CHECK(st.rootCopy.done == rootSeq && lorieGpuCopyIsDone(serial) && st.diag.vkFramesPresented == presented + 1 &&
              vkFailures == fails0 && emu.violations == 0, "frame diag off: the root copy, copy or frame not made");
        CHECK(histSamples() == samples && !st.rootCopy.doneUs && !vkRootCopyBusyUs && dg->vkFenceWaitUs > total,
              "frame diag off: %u samples taken, settled stamped %d, root copy time kept %d", histSamples() - samples,
              st.rootCopy.doneUs != 0, vkRootCopyBusyUs != 0);
        lorieGpuCopyAck(&bgraPix, dstBuffer);
        st.backend.vkSubmitOpt = 0;
        st.frameWantedUs = 0;
        ra->registered = rb->registered = false;
        earlyWithOption += emu.earlyPresents - early;   /* the option's, above */
    }

    /* P11-C3-B1 (lorie.h cpuAccess): a queued copy using a buffer the X server holds reserved is not recorded, nor any
     * copy after it - the frame drawn without them, nothing drawn into or from the reserved buffer, the copies left
     * queued and the X server asked to wake the renderer, a pass without a release no reason to run again; once it
     * is let go, they are copied. Alone: nothing submitted. A cancelled copy ahead of it still completed. GLES's copies
     * likewise. A root copy with a reserved buffer: not copied. The published root reserved (never, by the X server's
     * rule): not sampled - the image held. */
    {
        LorieBuffer *bd = redirected;
        uint64_t s1 = 0, s2 = 0, s3 = 0;
        uint32_t read0, submits, deferred = st.diag.copiesDeferred, presented;
        int d1, fails0 = vkFailures, waits;
        void *db1 = NULL, *db2 = NULL, *db3 = NULL;

        own(44);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);                 /* nothing queued: a frame, the image released */
        st.gpuInFlight.busy = 0;
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 1, 1, &s1, &db1), "B1: not queued");    /* cancelled below */
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &s2, &db2), "B1: not queued");
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 2, 2, &s3, &db3), "B1: not queued");
        st.gpuCopyQueue.entries[(st.gpuCopyQueue.writeIndex - 3) % LORIE_GPU_COPY_QUEUE_CAPACITY].cancelled = 1;
        st.cpuAccess.ids[0] = bd->desc.id, st.cpuAccess.count = 1;          /* the X server's reservation */
        st.cpuAccess.waiter = 0;
        read0 = st.gpuCopyQueue.readIndex;
        presented = st.diag.vkFramesPresented;
        d1 = emu.draws;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(vkFailures == fails0 && emu.violations == 0 && st.diag.vkFramesPresented == presented + 1 && emu.draws == d1 + 1 &&
              emu.drawnFrom[d1 % 64] == root->desc.buffer, "B1 frame: presented %u, %d draws (want the root alone)",
              st.diag.vkFramesPresented - presented, emu.draws - d1);
        CHECK(st.gpuCopyQueue.readIndex == read0 + 1 && lorieGpuCopyIsDone(s1) && !lorieGpuCopyIsDone(s2) && !lorieGpuCopyIsDone(s3) &&
              st.cpuAccess.waiter == 1 && st.diag.copiesDeferred == deferred + 1 && rendererCopiesStillDeferred(),
              "B1 frame: read %u (want past the cancelled one), done %d %d %d, waiter %u, deferred %u, still %d",
              st.gpuCopyQueue.readIndex - read0, lorieGpuCopyIsDone(s1), lorieGpuCopyIsDone(s2), lorieGpuCopyIsDone(s3),
              st.cpuAccess.waiter, st.diag.copiesDeferred - deferred, rendererCopiesStillDeferred());
        /* alone, the reservation still held: nothing submitted, nothing waited for */
        submits = st.diag.vkQueueSubmits, waits = atFence.waits;
        rendererVkApplyPendingGpuCopies();
        CHECK(st.diag.vkQueueSubmits == submits && atFence.waits == waits && !lorieGpuCopyIsDone(s2) && st.lock == 0,
              "B1 alone: %u submissions, %d fence waits while the reservation is held", st.diag.vkQueueSubmits - submits,
              atFence.waits - waits);
        /* GLES: likewise, from the same queue */
        {
            Queue saved;
            uint32_t glesRead;
            memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
            memcpy((void *) &glesState.cpuAccess, (const void *) &st.cpuAccess, sizeof st.cpuAccess);
            CHECK(gles(&saved) == 0 && glDrawCount == 0, "B1 GLES: copied past the reservation (%d draws)", glDrawCount);
            glesRead = glesState.gpuCopyQueue.readIndex;
            CHECK(glesRead == saved.readIndex, "B1 GLES: the queue moved past the reservation");
            glesState.cpuAccess.count = 0;
        }
        /* let go: copied */
        st.cpuAccess.count = 0;
        st.cpuAccess.releases++;
        CHECK(!rendererCopiesStillDeferred(), "B1: still deferred after the release");
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(s2) && lorieGpuCopyIsDone(s3) && vkFailures == fails0 && emu.violations == 0,
              "B1 let go: done %d %d", lorieGpuCopyIsDone(s2), lorieGpuCopyIsDone(s3));
        lorieGpuCopyAck(&bgraPix, db2);
        lorieGpuCopyAck(&srcPix, db3);
        lorieGpuCopyAck(&srcPix, db1);
        st.gpuInFlight.busy = 0;

        /* a root copy with a reserved buffer: not copied, nothing drawn */
        {
            LorieBuffer *ra = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48), *rb = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
            static const LorieGpuCopyRect one[1] = { { 3, 5, 40, 30 } };
            ra->registered = rb->registered = true;
            rootAsk(ra, rb, one, 1);
            st.cpuAccess.ids[0] = rb->desc.id, st.cpuAccess.count = 1;
            d1 = emu.draws;
            rendererVkRootCopy();
            st.cpuAccess.count = 0;
            CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && emu.draws == d1 && vkFailures == fails0,
                  "B1 root copy: settled %d, failed %d, %d draws", st.rootCopy.done == rootSeq, st.rootCopy.failed == rootSeq,
                  emu.draws - d1);
            ra->registered = rb->registered = false;
        }

        /* the published root reserved: not sampled, the image held */
        {
            int acquires = emu.acquires2;
            presented = st.diag.vkFramesPresented;
            d1 = emu.draws;
            st.cpuAccess.ids[0] = root->desc.id, st.cpuAccess.count = 1;
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            st.cpuAccess.count = 0;
            CHECK(vkr.held && emu.draws == d1 && st.diag.vkFramesPresented == presented && st.lock == 0 && emu.acquires2 == acquires + 1,
                  "B1 published root reserved: held %d, %d draws, %u presented", vkr.held, emu.draws - d1,
                  st.diag.vkFramesPresented - presented);
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            st.gpuInFlight.busy = 0;
            CHECK(!vkr.held && st.diag.vkFramesPresented == presented + 1 && emu.drawnFrom[d1 % 64] == root->desc.buffer,
                  "B1 published root let go: held %d, %u presented", vkr.held, st.diag.vkFramesPresented - presented);
        }
        st.cpuAccess.waiter = 0;
    }

    /* P11-C3-C, TERMUX_X11_FRAME_FREEZE_DIAG: the renderer's stage as its frame waits for its fence, and once the pass is
     * over; the asks its frame took up - those there were as it began, once it is presented; none of it with the option
     * off. */
    {
        uint32_t presented = st.diag.vkFramesPresented;
        own(47);
        st.backend.freezeDiag = 1;
        st.progress.askSeq = 7, st.progress.takenSeq = 3, st.progress.rStage = LORIE_R_SLEEP, st.progress.rStageUs = 0;
        stageAtFence = LORIE_R_STAGES;
        st.drawRequested = 1, st.waitForNextFrame = 0;     /* a tick's */
        rendererPass(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(st.diag.vkFramesPresented == presented + 1 && stageAtFence == LORIE_R_FENCE_WAIT && st.progress.rStage == LORIE_R_PASS &&
              st.progress.rStageUs && st.progress.takenSeq == 7 && st.progress.takenUs,
              "freeze diag: stage %u at the fence (want %d), %u after the pass, taken %u (want 7)", stageAtFence, LORIE_R_FENCE_WAIT,
              st.progress.rStage, st.progress.takenSeq);
        st.backend.freezeDiag = 0;
        st.progress.askSeq = 9, st.progress.rStage = LORIE_R_SLEEP, st.progress.rStageUs = 0, st.progress.takenUs = 0;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(st.progress.rStage == LORIE_R_SLEEP && st.progress.rStageUs == 0 && st.progress.takenSeq == 7 && st.progress.takenUs == 0,
              "freeze diag off: stage %u, taken %u - kept", st.progress.rStage, st.progress.takenSeq);
    }

    /* P11-C3-C, TERMUX_X11_RENDERER_VK_FRAME_DIAG: a frame's start 100 ms or more after the last one's, by what waited in
     * that gap (rendererVkFrameGap, the real one) - asleep through a frame's 3 s wait: unwoken, at every length up to 3 s,
     * the period's worst with its wake and gap; woken at once but the frame 150 ms late: late; awake when asked and
     * back to look 120 ms later: late; nothing asked for 1 s, ticks run: idle, not a stall; ticks not run: the X server
     * stalled; 60 ms: no gap. The frame's start takes the first ask still waiting from the ring, not the last. */
    {
        volatile struct lorie_renderer_diag *dg = &st.diag;
        struct rendererVkFrameTimes t;
        uint32_t idle = dg->vkGapIdle, xs = dg->vkGapXStalled, unw = dg->vkGapUnwoken, late = dg->vkGapLate;
        uint32_t ge[LORIE_GAP_GE], ige[LORIE_GAP_GE], k, waits;
        uint64_t T = 50000000;
        for (k = 0; k < LORIE_GAP_GE; k++)
            ge[k] = dg->vkGapStallGe[k], ige[k] = dg->vkGapIdleGe[k];
        dg->vkGapWorstStallUs = 0;
        t = (struct rendererVkFrameTimes) { .startUs = T + 3100000, .firstAskUs = T + 100000, .askSeq = 9, .takenSeq = 8,
                                            .seenSeq = 9, .seenUs = T + 3099000, .sleepUs = T + 50000, .ticksRun = 400 };
        rendererVkFrameGap(&t, T, 30);
        CHECK(dg->vkGapUnwoken == unw + 1 && dg->vkGapStallGe[0] == ge[0] + 1 && dg->vkGapStallGe[5] == ge[5] + 1 &&
              dg->vkGapWorstStallUs == 3000000 && dg->vkGapWorstCause == LORIE_GAP_UNWOKEN && dg->vkGapWorstWakeUs == 2999000 &&
              dg->vkGapWorstGapUs == 3100000 && dg->vkGapWorstTicks == 370,
              "gaps, asleep through a 3 s wait: unwoken +%u, >=3 s +%u, worst %u us cause %u wake %u gap %u ticks %u",
              dg->vkGapUnwoken - unw, dg->vkGapStallGe[5] - ge[5], dg->vkGapWorstStallUs, dg->vkGapWorstCause,
              dg->vkGapWorstWakeUs, dg->vkGapWorstGapUs, dg->vkGapWorstTicks);
        t = (struct rendererVkFrameTimes) { .startUs = T + 400000, .firstAskUs = T + 250000, .askSeq = 5, .takenSeq = 4,
                                            .seenSeq = 5, .seenUs = T + 251000, .sleepUs = T + 100000, .ticksRun = 80 };
        rendererVkFrameGap(&t, T, 30);
        t = (struct rendererVkFrameTimes) { .startUs = T + 500000, .firstAskUs = T + 300000, .askSeq = 6, .takenSeq = 5,
                                            .seenSeq = 6, .seenUs = T + 420000, .sleepUs = T - 5000, .ticksRun = 90 };
        rendererVkFrameGap(&t, T, 30);
        CHECK(dg->vkGapLate == late + 2 && dg->vkGapUnwoken == unw + 1 && dg->vkGapStallGe[0] == ge[0] + 3 &&
              dg->vkGapStallGe[1] == ge[1] + 1 && dg->vkGapWorstStallUs == 3000000,
              "gaps, woken but late: late +%u (want 2), >=100 ms +%u (want 3), >=250 ms +%u (want 1)", dg->vkGapLate - late,
              dg->vkGapStallGe[0] - ge[0], dg->vkGapStallGe[1] - ge[1]);
        t = (struct rendererVkFrameTimes) { .startUs = T + 1000000, .firstAskUs = T + 999000, .askSeq = 3, .takenSeq = 2,
                                            .ticksRun = 150 };
        rendererVkFrameGap(&t, T, 30);
        CHECK(dg->vkGapIdle == idle + 1 && dg->vkGapIdleGe[3] == ige[3] + 1 && dg->vkGapIdleGe[4] == ige[4] &&
              dg->vkGapXStalled == xs, "gaps, nothing asked for 1 s: idle +%u, >=1 s +%u, >=2 s +%u, X stalled +%u",
              dg->vkGapIdle - idle, dg->vkGapIdleGe[3] - ige[3], dg->vkGapIdleGe[4] - ige[4], dg->vkGapXStalled - xs);
        t = (struct rendererVkFrameTimes) { .startUs = T + 1000000, .askSeq = 3, .takenSeq = 3, .ticksRun = 31 };
        rendererVkFrameGap(&t, T, 30);
        t = (struct rendererVkFrameTimes) { .startUs = T + 60000, .firstAskUs = T + 30000, .askSeq = 4, .takenSeq = 3, .ticksRun = 40 };
        waits = dg->vkAskWait.count;
        rendererVkFrameGap(&t, T, 30);
        CHECK(dg->vkGapXStalled == xs + 1 && dg->vkGapIdle == idle + 1 && dg->vkGapLate == late + 2 && dg->vkAskWait.count == waits + 1,
              "gaps, ticks not run 1 s: X stalled +%u; 60 ms: counted as a gap, or its ask's wait not sampled",
              dg->vkGapXStalled - xs);
        /* the frame's start: the first ask still waiting (11, 5 ms ago), not the last (12, now) */
        {
            uint32_t presented = st.diag.vkFramesPresented, maxBefore;
            own(48);
            st.backend.vkFrameDiag = 1;
            st.progress.takenSeq = 10, st.progress.askSeq = 12;
            st.progress.askUs[11 % LORIE_ASK_RING] = lorieMonotonicUs() - 5000;
            st.progress.askUs[12 % LORIE_ASK_RING] = lorieMonotonicUs();
            dg->vkAskWait.maxUs = 0;
            maxBefore = dg->vkAskWait.count;
            st.drawRequested = 1, st.waitForNextFrame = 0;
            rendererVkRedraw(&waiting);
            st.gpuInFlight.busy = 0;
            CHECK(st.diag.vkFramesPresented == presented + 1 && dg->vkAskWait.count == maxBefore + 1 && dg->vkAskWait.maxUs >= 5000 &&
                  st.progress.takenSeq == 12, "gaps: the frame's first waiting ask - wait %u us (want 5000 or more), taken %u",
                  dg->vkAskWait.maxUs, st.progress.takenSeq);
            st.backend.vkFrameDiag = 0;
        }
    }

    /* P11-C3-C: a buffer a queued copy uses that has not come yet from the connection - waited for before the shared
     * lock, not under it (the X server may take the lock meanwhile): it comes, its wake ends the wait, the copy is made
     * in that same pass. Never coming: given up after 100 ms, the copy skipped - completed - the lock free all along.
     * Missing only once the frame has the lock (queued after the wait): left queued, not skipped, and made by the next
     * pass. GLES's copies likewise: left queued, or, given up on, skipped. */
    {
        uint64_t s1 = 0, s2 = 0, s3 = 0;
        void *db1 = NULL, *db2 = NULL, *db3 = NULL;
        uint32_t awaited = st.diag.buffersAwaited, givenUp = st.diag.buffersGivenUp, left = st.diag.copiesLeftForBuffers;
        int fails0 = vkFailures;
        struct timespec t0;
        pthread_t t;
        long ms;

        own(46);
        st.gpuInFlight.busy = 0;
        CHECK(st.gpuCopyQueue.readIndex == st.gpuCopyQueue.writeIndex, "buffers: copies still queued from before");
        /* comes 40 ms later */
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 1, 1, &s1, &db1), "buffers: not queued");
        src->registered = false;
        arrival = (struct arrival) { src, 40, -1 };
        pthread_create(&t, NULL, arrive, &arrival);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        st.drawRequested = 1;
        rendererPass(&waiting);
        ms = msSince(&t0);
        pthread_join(t, NULL);
        st.gpuInFlight.busy = 0;
        CHECK(lorieGpuCopyIsDone(s1) && ms >= 30 && arrival.lockHeld == 0 && st.diag.buffersAwaited == awaited + 1 &&
              st.diag.buffersGivenUp == givenUp && vkFailures == fails0 && emu.violations == 0,
              "buffers, come after 40 ms: done %d after %ld ms, the shared lock %s while waited for, %u awaited, %u given up",
              lorieGpuCopyIsDone(s1), ms, arrival.lockHeld ? "held" : "free", st.diag.buffersAwaited - awaited,
              st.diag.buffersGivenUp - givenUp);
        lorieGpuCopyAck(&srcPix, db1);
        /* never coming */
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 2, 2, &s2, &db2), "buffers: not queued");
        src->registered = false;
        arrival = (struct arrival) { NULL, 50, -1 };
        pthread_create(&t, NULL, arrive, &arrival);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        st.drawRequested = 1;
        rendererPass(&waiting);
        ms = msSince(&t0);
        pthread_join(t, NULL);
        st.gpuInFlight.busy = 0;
        src->registered = true;
        CHECK(lorieGpuCopyIsDone(s2) && ms >= 90 && ms < 500 && arrival.lockHeld == 0 && st.diag.buffersGivenUp == givenUp + 1 &&
              vkFailures == fails0, "buffers, never come: done %d after %ld ms, the lock %s meanwhile, %u given up",
              lorieGpuCopyIsDone(s2), ms, arrival.lockHeld ? "held" : "free", st.diag.buffersGivenUp - givenUp);
        lorieGpuCopyAck(&srcPix, db2);
        /* missing only once the frame has the lock: left for the next pass */
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &two, 3, 3, &s3, &db3), "buffers: not queued");
        gone = src;
        atLock = bufferGone;
        arrival = (struct arrival) { NULL, 30, -1 };    /* the lock looked at meanwhile: not slept on under it */
        pthread_create(&t, NULL, arrive, &arrival);
        st.drawRequested = 1;
        rendererPass(&waiting);
        pthread_join(t, NULL);
        atLock = NULL;
        st.gpuInFlight.busy = 0;
        CHECK(!lorieGpuCopyIsDone(s3) && st.diag.copiesLeftForBuffers == left + 1 && vkFailures == fails0 && arrival.lockHeld == 0,
              "buffers, missing under the lock: done %d (want left queued), %u left, the lock %s 30 ms on", lorieGpuCopyIsDone(s3),
              st.diag.copiesLeftForBuffers - left, arrival.lockHeld ? "held" : "free");
        /* GLES, the same entry: left queued; given up on: skipped */
        {
            Queue saved;
            memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
            buffersGivenUpCount = 0;
            CHECK(gles(&saved) == 0 && glesState.gpuCopyQueue.readIndex == saved.readIndex && glDrawCount == 0,
                  "buffers, GLES: a copy whose buffer may still come not left queued");
            buffersGivenUp[0] = src->desc.id, buffersGivenUpCount = 1;
            CHECK(gles(&saved) == s3 && glesState.gpuCopyQueue.readIndex == saved.writeIndex && glDrawCount == 0,
                  "buffers, GLES: a copy whose buffer was given up on not skipped");
            buffersGivenUpCount = 0;
        }
        src->registered = true;
        st.drawRequested = 1;
        rendererPass(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(lorieGpuCopyIsDone(s3) && vkFailures == fails0 && emu.violations == 0,
              "buffers: the copy left queued not made by the next pass");
        lorieGpuCopyAck(&srcPix, db3);
    }

    /* P11-C3-B2, TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1: the root copy submitted ahead of the frame, in the second
     * submission (its own command buffer, fence and list in flight), signalling the chain semaphore; the frame recorded
     * meanwhile and submitted waiting for it on the device. The root copy's fence waited for first, then the frame's:
     * settled as copied at its own fence - its list (the buffer the X server draws into next) out of flight while the
     * frame still runs; the frame waiting on the CPU for no more than its submission (after_root), not for the root
     * copy's 10 ms. The X server already waiting for it: waited for before the frame. No frame after it, or a frame
     * that returns early: its fence waited for and an empty submission consumes the semaphore - which works again
     * after. A queued copy using its buffers: left for the next pass. Its submission failing: not copied; the frame's
     * failing: copied, then Vulkan given up; its fence lost: not copied. The second submission's objects not made: the
     * root copy as before, counted. With P11-C1's copies ahead too. */
    {
        LorieBuffer *ra = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 640, 480), *rb = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 640, 480);
        LoriePixmapPriv rbP = { rb };
        PixmapRec rbPix = { { 640, 480 }, &rbP };
        static const LorieGpuCopyRect one[1] = { { 3, 5, 40, 30 } };
        volatile struct lorie_renderer_diag *dg = &st.diag;
        uint32_t ahead0, presented, submits;
        int fails0 = vkFailures, early = emu.earlyPresents;
        struct histMark afterRoot0, fenceRoot0;
        uint64_t s1 = 0;
        void *db1 = NULL;

        ra->registered = rb->registered = true;
        own(45);
        st.backend.vkScheduleOpt = 1;
        st.backend.vkFrameDiag = 1;
        fenceLogDst = rb->desc.id, fenceLogRoot = root->desc.id;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        st.gpuInFlight.busy = 0;

        /* ahead, then the frame */
        ahead0 = dg->rootAhead;
        presented = dg->vkFramesPresented;
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1;
        st.waitForNextFrame = 0;            /* the tick */
        CHECK(rendererVkFrameWanted(), "B2: no frame wanted after the swap");
        afterRoot0 = mark(&dg->vkFrameAfterRoot), fenceRoot0 = mark(&dg->vkFenceRoot);
        submits = (uint32_t) emu.submits;
        fenceMs[0] = 10, fenceMs[1] = 0, fenceMsCount = 2, fenceMsAt = 0;
        fenceLog.n = 0;
        pushes = 0;
        rendererVkRootCopyAhead();
        CHECK(vkAux.pending && vkAux.chained && cmdAux.pending && dg->rootAhead == ahead0 + 1 && st.rootCopy.taken == rootSeq &&
              st.rootCopy.done != rootSeq && (st.gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX) && st.gpuInFlight.auxCount == 2 &&
              lorieGpuInFlight(state, ra->desc.id) && lorieGpuInFlight(state, rb->desc.id) && emu.submits == submits + 1 &&
              vkRootCopyBusyUs < 10000 && st.lock == 0,
              "B2 ahead: pending %d chained %d, aux running %d, taken %d done %d, aux listed %d (%u), %ld submissions, busy %llu us",
              vkAux.pending, vkAux.chained, cmdAux.pending, st.rootCopy.taken == rootSeq, st.rootCopy.done == rootSeq,
              (st.gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX) != 0, st.gpuInFlight.auxCount, (long) (emu.submits - submits),
              (unsigned long long) vkRootCopyBusyUs);
        rendererVkRedraw(&waiting);
        fenceMsCount = 0;
        st.gpuInFlight.busy = 0;
        CHECK(vkFailures == fails0 && emu.violations == 0 && dg->vkFramesPresented == presented + 1 && !vkAux.pending && !vkAux.chained &&
              st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && emu.submits == submits + 2,
              "B2 frame: %d failures, %d violations, presented %u, pending %d chained %d, done %d failed %d, %ld submissions",
              vkFailures - fails0, emu.violations, dg->vkFramesPresented - presented, vkAux.pending, vkAux.chained,
              st.rootCopy.done == rootSeq, st.rootCopy.failed == rootSeq, (long) (emu.submits - submits));
        CHECK(fenceLog.n == 2 && fenceLog.fence[0] != fenceLog.fence[1] && fenceLog.submits[0] == submits + 2 && !fenceLog.rootDone[0] &&
              fenceLog.rootDone[1] && !fenceLog.auxListed[1] && !fenceLog.dstInFlight[1] && fenceLog.rootInFlight[1],
              "B2 frame: %d fences (want the root copy's, then the frame's), the frame submitted before the first %d, at the frame's "
              "fence settled %d, aux listed %d, its buffer in flight %d, the root in flight %d", fenceLog.n,
              fenceLog.n > 0 && fenceLog.submits[0] == submits + 2, fenceLog.n > 1 && fenceLog.rootDone[1],
              fenceLog.n > 1 && fenceLog.auxListed[1], fenceLog.n > 1 && fenceLog.dstInFlight[1], fenceLog.n > 1 && fenceLog.rootInFlight[1]);
        CHECK(pushes >= 2 && fabsf((pushed[0].rect[0] + 1.f) * 640.f / 2.f - 3.f) < 1e-3f && emu.drawnInto[(emu.draws - 2) % 64] == rb->desc.buffer &&
              emu.drawnFrom[(emu.draws - 1) % 64] == root->desc.buffer, "B2 frame: not the root copy, then the frame");
        CHECK(newSamples(&dg->vkFrameAfterRoot, afterRoot0) == 1 && newUs(&dg->vkFrameAfterRoot, afterRoot0) < 10000 &&
              newSamples(&dg->vkFenceRoot, fenceRoot0) == 1 && newUs(&dg->vkFenceRoot, fenceRoot0) >= 10000,
              "B2 frame: after_root %llu us, the root copy's fence %llu us", (unsigned long long) newUs(&dg->vkFrameAfterRoot, afterRoot0),
              (unsigned long long) newUs(&dg->vkFenceRoot, fenceRoot0));

        /* the X server already waiting: the root copy waited for before the frame */
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1;
        rendererVkRootCopyAhead();
        st.rootCopy.waiting = rootSeq;
        submits = (uint32_t) emu.submits;
        fenceLog.n = 0;
        rendererVkRedraw(&waiting);
        st.rootCopy.waiting = 0;
        st.gpuInFlight.busy = 0;
        CHECK(dg->rootAheadXWaiting == 1 && fenceLog.n == 2 && fenceLog.submits[0] == submits && st.rootCopy.done == rootSeq &&
              !vkAux.pending && !vkAux.chained && emu.violations == 0 && vkFailures == fails0,
              "B2 X waiting: counted %u, %d fences, the root copy's before the frame's submission %d, done %d",
              dg->rootAheadXWaiting, fenceLog.n, fenceLog.n > 0 && fenceLog.submits[0] == submits, st.rootCopy.done == rootSeq);

        /* no frame after it: waited for, the semaphore consumed by an empty submission - and it works again after */
        rootAsk(ra, rb, one, 1);
        rendererVkRootCopyAhead();
        submits = (uint32_t) emu.submits;
        rendererVkAuxSettle();
        CHECK(!vkAux.pending && !vkAux.chained && st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq &&
              dg->rootChainConsumed == 1 && emu.submits == submits + 1 && emu.violations == 0 && !emu.fencePending,
              "B2 no frame: pending %d chained %d, done %d, consumed %u, %ld submissions", vkAux.pending, vkAux.chained,
              st.rootCopy.done == rootSeq, dg->rootChainConsumed, (long) (emu.submits - submits));
        /* a frame that returns early (the root not found): likewise */
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1;
        rendererVkRootCopyAhead();
        st.rootWindowTextureID = 999999;
        rendererVkRedraw(&waiting);
        st.rootWindowTextureID = root->desc.id;
        rendererVkAuxSettle();
        waiting = false;
        CHECK(st.rootCopy.done == rootSeq && dg->rootChainConsumed == 2 && emu.violations == 0 && vkFailures == fails0,
              "B2 early return: done %d, consumed %u", st.rootCopy.done == rootSeq, dg->rootChainConsumed);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(emu.violations == 0 && vkFailures == fails0, "B2: the frame after a consumed semaphore");

        /* a queued copy into the root copy's buffer: not in the frame, copied by the next pass */
        rootAsk(ra, rb, one, 1);
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rbPix, &two, 1, 1, &s1, &db1), "B2: not queued");
        st.drawRequested = 1;
        rendererVkRootCopyAhead();
        rendererVkRedraw(&waiting);
        rendererVkAuxSettle();
        st.gpuInFlight.busy = 0;
        CHECK(!lorieGpuCopyIsDone(s1) && st.rootCopy.done == rootSeq && emu.violations == 0, "B2: a copy into the root copy's buffer recorded "
              "while the root copy ran (done %d)", lorieGpuCopyIsDone(s1));
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(s1) && emu.violations == 0, "B2: the copy left for the next pass not made");
        lorieGpuCopyAck(&srcPix, db1);
        st.gpuInFlight.busy = 0;

        /* its submission failing: not copied, Vulkan given up */
        rootAsk(ra, rb, one, 1);
        emu.submitResult = VK_ERROR_DEVICE_LOST;
        rendererVkRootCopyAhead();
        emu.submitResult = VK_SUCCESS;
        CHECK(!vkAux.pending && st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && vkFailures == fails0 + 1 &&
              !(st.gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX), "B2 submission failing: pending %d, failed %d, %d failures",
              vkAux.pending, st.rootCopy.failed == rootSeq, vkFailures - fails0);
        vkFailures = fails0;
        vkr.chainSignal = false;        /* the teardown would have made everything again */
        emu.fencePending = false;

        /* the frame's submission failing: the root copy copied all the same, then Vulkan given up */
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1;
        rendererVkRootCopyAhead();
        emu.submitFailAfter = 1;
        rendererVkRedraw(&waiting);
        emu.submitFailAfter = 0;
        CHECK(!vkAux.pending && st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && vkFailures == fails0 + 1,
              "B2 frame failing: pending %d, done %d failed %d, %d failures", vkAux.pending, st.rootCopy.done == rootSeq,
              st.rootCopy.failed == rootSeq, vkFailures - fails0);
        vkFailures = fails0;
        vkAux.chained = false;
        vkr.chainWait = false;          /* the teardown would have made the semaphore again */
        emu.fencePending = false;
        st.gpuInFlight.busy = 0;
        semaphoreSignalled(vkr.chain, 0);  /* the one the root copy signalled and no frame waited for: gone with the teardown */

        /* its fence lost: not copied, Vulkan given up */
        rootAsk(ra, rb, one, 1);
        rendererVkRootCopyAhead();
        emu.fenceResult = emu.idleResult = VK_ERROR_DEVICE_LOST;
        rendererVkAuxSettle();
        emu.fenceResult = emu.idleResult = VK_SUCCESS;
        CHECK(!vkAux.pending && st.rootCopy.done == rootSeq && st.rootCopy.failed == rootSeq && vkFailures == fails0 + 1 &&
              !(st.gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX), "B2 fence lost: pending %d, failed %d, %d failures, aux listed %d",
              vkAux.pending, st.rootCopy.failed == rootSeq, vkFailures - fails0, (st.gpuInFlight.busy & LORIE_GPU_IN_FLIGHT_AUX) != 0);
        vkFailures = fails0;
        vkAux.chained = false;
        vkr.chainWait = false;
        semaphoreSignalled(vkr.chain, 0);
        emu.fencePending = false;
        st.gpuInFlight.busy = 0;
        st.diag.lastFallbackClass = st.diag.lastFallbackReason = 0;

        /* with P11-C1's copies ahead too: three submissions, two of them running at once at most */
        st.backend.vkSubmitOpt = 1;
        rootAsk(ra, rb, one, 1);
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &s1, &db1), "B2+C1: not queued");
        st.drawRequested = 1;
        submits = (uint32_t) emu.submits;
        rendererVkRootCopyAhead();
        fenceLog.n = 0;
        rendererVkRedraw(&waiting);
        rendererVkAuxSettle();
        st.gpuInFlight.busy = 0;
        CHECK(fenceLog.n == 3 && fenceLog.auxListed[0] && fenceLog.auxListed[1] && !fenceLog.auxListed[2],
              "B2+C1: the copies ahead over, the root copy's list %s", fenceLog.n == 3 && fenceLog.auxListed[1] ? "kept" : "cleared too");
        CHECK(lorieGpuCopyIsDone(s1) && st.rootCopy.done == rootSeq && emu.submits == submits + 3 && emu.violations == 0 &&
              vkFailures == fails0, "B2+C1: done %d, root done %d, %ld submissions", lorieGpuCopyIsDone(s1), st.rootCopy.done == rootSeq,
              (long) (emu.submits - submits));
        lorieGpuCopyAck(&bgraPix, db1);
        st.backend.vkSubmitOpt = 0;
        earlyWithOption += emu.earlyPresents - early;

        /* B1 and B2 together: a reserved copy left queued, the root copy ahead and the frame as without it */
        rootAsk(ra, rb, one, 1);
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &s1, &db1), "B1+B2: not queued");
        st.cpuAccess.ids[0] = redirected->desc.id, st.cpuAccess.count = 1;
        presented = dg->vkFramesPresented;
        st.drawRequested = 1;
        rendererVkRootCopyAhead();
        rendererVkRedraw(&waiting);
        rendererVkAuxSettle();
        st.gpuInFlight.busy = 0;
        CHECK(!lorieGpuCopyIsDone(s1) && rendererCopiesStillDeferred() && st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq &&
              dg->vkFramesPresented == presented + 1 && emu.violations == 0 && vkFailures == fails0,
              "B1+B2: copy done %d (want not), deferred %d, root done %d, presented %u", lorieGpuCopyIsDone(s1),
              rendererCopiesStillDeferred(), st.rootCopy.done == rootSeq, dg->vkFramesPresented - presented);
        st.cpuAccess.count = 0;
        st.cpuAccess.releases++;
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(s1) && emu.violations == 0, "B1+B2: not copied once let go");
        lorieGpuCopyAck(&bgraPix, db1);
        st.cpuAccess.waiter = 0;
        st.gpuInFlight.busy = 0;

        /* the second submission's objects not made: the root copy as before, counted */
        {
            struct lorie_slot_saved { struct lorie_vk_slot aux; VkSemaphore chain; } saved = { vkr.aux, vkr.chain };
            memset(&vkr.aux, 0, sizeof vkr.aux);
            vkr.chain = VK_NULL_HANDLE;
            cmdAux.allocated = true;    /* the pool has no other to give */
            emu.auxCmdFail = 1;
            rootAsk(ra, rb, one, 1);
            rendererVkRootCopyAhead();
            emu.auxCmdFail = 0;
            CHECK(dg->rootAheadUnavailable == 1 && !vkAux.pending && st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq &&
                  emu.violations == 0, "B2 unavailable: counted %u, pending %d, done %d", dg->rootAheadUnavailable, vkAux.pending,
                  st.rootCopy.done == rootSeq);
            vkr.aux = saved.aux;
            vkr.chain = saved.chain;
        }
        st.backend.vkScheduleOpt = 0;
        st.backend.vkFrameDiag = 0;
        ra->registered = rb->registered = false;
        fenceLogDst = fenceLogRoot = 0;
    }

    /* The renderer's pass (rendererPass), as its loop runs it: the root copy first, then the frame or the copies alone -
     * by Vulkan while it is active, by GLES otherwise, never both in one pass. Vulkan active: an acquire that times out
     * (drawRequested kept), a root not there yet with copies queued, a new request with nothing ahead - no GLES frame,
     * no GLES copies. P11-C3-B2: a root copy submitted ahead and a frame that returned early, skipped, or found the
     * swapchain out of date - settled and its semaphore consumed within the pass, no GLES. B1 and B2 on and off, all four: Vulkan alone, the root copy
     * settled as copied, the frame presented, a reserved copy left queued. Vulkan failing in a pass: no GLES frame in
     * it (its window given up), GLES's in the next - and no Vulkan then. */
    {
        LorieBuffer *ra = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 640, 480), *rb = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 640, 480);
        static const LorieGpuCopyRect one[1] = { { 3, 5, 40, 30 } };
        volatile struct lorie_renderer_diag *dg = &st.diag;
        int fails0 = vkFailures, early = emu.earlyPresents;
        uint32_t presented, consumed;
        uint64_t s1 = 0;
        void *db1 = NULL;

        ra->registered = rb->registered = true;
        own(46);
        st.surfaceAvailable = 1;
        glesRedraws = glesCopies = 0;

        /* the acquire timing out: the request kept, no GLES */
        presented = dg->vkFramesPresented;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        emu.acquireResult = VK_TIMEOUT;
        rendererPass(&waiting);
        emu.acquireResult = VK_SUCCESS;
        CHECK(glesRedraws == 0 && glesCopies == 0 && st.drawRequested && dg->vkFramesPresented == presented && emu.violations == 0 &&
              vkFailures == fails0, "pass, acquire timed out: %d GLES frames, %d GLES copies, request %s", glesRedraws, glesCopies,
              st.drawRequested ? "kept" : "dropped");

        /* the root not there yet, a copy queued: Vulkan's copies alone, no GLES */
        CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &s1, &db1), "pass: not queued");
        st.rootWindowTextureID = 999999;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        st.rootWindowTextureID = root->desc.id;
        waiting = false;
        st.gpuInFlight.busy = 0;
        CHECK(lorieGpuCopyIsDone(s1) && glesRedraws == 0 && glesCopies == 0 && emu.violations == 0, "pass, root not there: copy done %d, "
              "%d GLES frames, %d GLES copies", lorieGpuCopyIsDone(s1), glesRedraws, glesCopies);
        lorieGpuCopyAck(&bgraPix, db1);

        /* a new request, nothing ahead: a Vulkan frame, no GLES */
        presented = dg->vkFramesPresented;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(dg->vkFramesPresented == presented + 1 && glesRedraws == 0 && glesCopies == 0 && !vkAux.pending && !vkAux.chained,
              "pass, a request: %u Vulkan frames, %d GLES frames, %d GLES copies", dg->vkFramesPresented - presented, glesRedraws, glesCopies);

        /* B2: the root copy ahead, the frame returning early (the root not there) - settled and consumed in the pass */
        st.backend.vkScheduleOpt = 1;
        consumed = dg->rootChainConsumed;
        rootAsk(ra, rb, one, 1);
        st.rootWindowTextureID = 999999;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        st.rootWindowTextureID = root->desc.id;
        waiting = false;
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && !vkAux.pending && !vkAux.chained &&
              dg->rootChainConsumed == consumed + 1 && glesRedraws == 0 && glesCopies == 0 && emu.violations == 0 && !emu.fencePending,
              "pass, ahead and an early return: done %d, pending %d chained %d, consumed %u, GLES %d/%d", st.rootCopy.done == rootSeq,
              vkAux.pending, vkAux.chained, dg->rootChainConsumed - consumed, glesRedraws, glesCopies);
        /* ... the acquire timing out */
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1, st.waitForNextFrame = 0;
        emu.acquireResult = VK_TIMEOUT;
        rendererPass(&waiting);
        emu.acquireResult = VK_SUCCESS;
        CHECK(st.rootCopy.done == rootSeq && !vkAux.pending && !vkAux.chained && dg->rootChainConsumed == consumed + 2 &&
              glesRedraws == 0 && glesCopies == 0 && emu.violations == 0, "pass, ahead and a skipped acquire: done %d, consumed %u, GLES %d/%d",
              st.rootCopy.done == rootSeq, dg->rootChainConsumed - consumed, glesRedraws, glesCopies);
        /* ... the swapchain out of date (made again by the frame) */
        rootAsk(ra, rb, one, 1);
        st.drawRequested = 1, st.waitForNextFrame = 0;
        emu.acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
        rendererPass(&waiting);
        emu.acquireResult = VK_SUCCESS;
        CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && !vkAux.pending && !vkAux.chained &&
              dg->rootChainConsumed == consumed + 3 && glesRedraws == 0 && glesCopies == 0 && emu.violations == 0 && vkFailures == fails0,
              "pass, ahead and an out-of-date swapchain: done %d, consumed %u, GLES %d/%d, %d failures", st.rootCopy.done == rootSeq,
              dg->rootChainConsumed - consumed, glesRedraws, glesCopies, vkFailures - fails0);
        /* ... and the next pass draws, its semaphore signalled anew */
        rootAsk(ra, rb, one, 1);
        presented = dg->vkFramesPresented;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        st.gpuInFlight.busy = 0;
        CHECK(dg->vkFramesPresented == presented + 1 && st.rootCopy.done == rootSeq && dg->rootChainConsumed == consumed + 3 &&
              emu.violations == 0 && glesRedraws == 0, "pass after: %u frames, done %d", dg->vkFramesPresented - presented,
              st.rootCopy.done == rootSeq);

        /* B1 and B2, on and off: Vulkan alone, the root copy copied, the frame presented, a reserved copy left queued */
        for (int b1 = 0; b1 < 2; b1++)
            for (int b2 = 0; b2 < 2; b2++) {
                uint32_t ahead = dg->rootAhead;
                st.backend.vkScheduleOpt = (uint8_t) b2;
                rootAsk(ra, rb, one, 1);
                CHECK(lorieTryScheduleGpuCopy(&bgraPix, &redirectedPix, NULL, 5, 7, &s1, &db1), "pass B1 %d B2 %d: not queued", b1, b2);
                if (b1)
                    st.cpuAccess.ids[0] = redirected->desc.id, st.cpuAccess.count = 1;
                presented = dg->vkFramesPresented;
                st.drawRequested = 1, st.waitForNextFrame = 0;
                rendererPass(&waiting);
                st.gpuInFlight.busy = 0;
                CHECK(st.rootCopy.done == rootSeq && st.rootCopy.failed != rootSeq && dg->vkFramesPresented == presented + 1 &&
                      lorieGpuCopyIsDone(s1) == !b1 && dg->rootAhead == ahead + (uint32_t) b2 && !vkAux.pending && !vkAux.chained &&
                      glesRedraws == 0 && glesCopies == 0 && emu.violations == 0 && vkFailures == fails0,
                      "pass B1 %d B2 %d: root done %d, %u frames, copy done %d, ahead %u, GLES %d/%d, %d violations", b1, b2,
                      st.rootCopy.done == rootSeq, dg->vkFramesPresented - presented, lorieGpuCopyIsDone(s1), dg->rootAhead - ahead,
                      glesRedraws, glesCopies, emu.violations);
                if (b1) {
                    st.cpuAccess.count = 0;
                    st.cpuAccess.releases++;
                    st.cpuAccess.waiter = 0;
                    rendererPass(&waiting);
                    st.gpuInFlight.busy = 0;
                    CHECK(lorieGpuCopyIsDone(s1) && glesCopies == 0, "pass B1 %d B2 %d: not copied once let go", b1, b2);
                }
                lorieGpuCopyAck(&bgraPix, db1);
            }
        st.backend.vkScheduleOpt = 0;
        earlyWithOption += emu.earlyPresents - early;

        /* Vulkan failing in a pass (its frame's submission): no GLES frame in it - its window given up; GLES's in the
         * next, and no Vulkan then */
        failEndsVulkan = true;
        presented = dg->vkFramesPresented;
        st.drawRequested = 1, st.waitForNextFrame = 0;
        emu.submitResult = VK_ERROR_DEVICE_LOST;
        rendererPass(&waiting);
        emu.submitResult = VK_SUCCESS;
        CHECK(vkFailures == fails0 + 1 && !vkActive && glesRedraws == 0 && glesCopies == 0, "pass, Vulkan failing: %d failures, active %d, "
              "GLES %d/%d in the same pass", vkFailures - fails0, vkActive, glesRedraws, glesCopies);
        st.surfaceAvailable = 1;            /* GLES has the window now (rendererSelectBackend) */
        st.drawRequested = 1, st.waitForNextFrame = 0;
        rendererPass(&waiting);
        CHECK(glesRedraws == 1 && dg->vkFramesPresented == presented && !emu.fencePending, "pass after the fallback: %d GLES frames, %u Vulkan",
              glesRedraws, dg->vkFramesPresented - presented);
        failEndsVulkan = false;
        vkActive = true;
        vkFailures = fails0;
        vkr.chainSignal = vkr.chainWait = false;
        emu.fencePending = false;
        st.gpuInFlight.busy = 0;
        st.diag.lastFallbackClass = st.diag.lastFallbackReason = 0;
        glesRedraws = glesCopies = 0;
        ra->registered = rb->registered = false;
    }

    /* P11-C3: the pacing intervals from given stamps (rendererVkFrameTimed): the first frame none - no false hitch;
     * 8, 17 and 35 ms in their buckets, the largest the period's worst with what that frame spent; a stamp before the
     * last none (counted invalid), the next interval from it; a gap over 250 ms none (counted idle); another swapchain
     * starting over; a frame without a present call no present interval and no present time; its wake only for a
     * request made after the last frame started and before this one did */
    {
        volatile struct lorie_renderer_diag *dg = &st.diag;
        VkSwapchainKHR real = vkr.swapchain;
        struct histMark start, delta, wake, present;
        uint32_t invalid = dg->vkFrameIdleGaps, idle, bucket8, bucket20, bucket50;
        uint64_t t = 5000000;
#define FRAME(at, ...) rendererVkFrameTimed(&(struct rendererVkFrameTimes) { .startUs = (at), .lockStartUs = (at) + 100, \
            .lockedUs = (at) + 150, .submitStartUs = (at) + 400, .submittedUs = (at) + 450, .presentStartUs = (at) + 900, \
            .presentEndUs = (at) + 1000, .endUs = (at) + 1100, __VA_ARGS__ })
        invalid = dg->vkFrameStampsInvalid, idle = dg->vkFrameIdleGaps;
        rendererVkPacingReset();
        dg->vkWorstStartUs = 0;
        start = mark(&dg->vkFrameStart), delta = mark(&dg->vkPresentDelta), wake = mark(&dg->vkFrameWake);
        present = mark(&dg->vkFramePresent);
        bucket8 = dg->vkFrameStart.buckets[14], bucket20 = dg->vkFrameStart.buckets[21], bucket50 = dg->vkFrameStart.buckets[24];
        FRAME(t);
        CHECK(newSamples(&dg->vkFrameStart, start) == 0 && newSamples(&dg->vkPresentDelta, delta) == 0 &&
              newSamples(&dg->vkFramePresent, present) == 1, "timed frames: the first one an interval");
        t += 8000, FRAME(t);
        t += 17000, FRAME(t);
        t += 35000, FRAME(t, .acquireUs = 21000, .fenceUs = 9000, .afterRootUs = 4000, .wantedUs = t - 500);
        CHECK(newSamples(&dg->vkFrameStart, start) == 3 && newUs(&dg->vkFrameStart, start) == 60000 &&
              newSamples(&dg->vkPresentDelta, delta) == 3 && dg->vkFrameStart.buckets[14] == bucket8 + 1 &&
              dg->vkFrameStart.buckets[21] == bucket20 + 1 && dg->vkFrameStart.buckets[24] == bucket50 + 1,
              "timed frames, 8, 17, 35 ms: %u intervals of %llu us in all, buckets 8 ms +%u, 20 ms +%u, 50 ms +%u",
              newSamples(&dg->vkFrameStart, start), (unsigned long long) newUs(&dg->vkFrameStart, start),
              dg->vkFrameStart.buckets[14] - bucket8, dg->vkFrameStart.buckets[21] - bucket20, dg->vkFrameStart.buckets[24] - bucket50);
        CHECK(dg->vkWorstStartUs == 35000 && dg->vkWorstAcquireUs == 21000 && dg->vkWorstFenceUs == 9000 &&
              dg->vkWorstAfterRootUs == 4000 && dg->vkWorstLockUs == 50 && dg->vkWorstPresentUs == 100 && dg->vkWorstWakeUs == 500 &&
              newSamples(&dg->vkFrameWake, wake) == 1, "timed frames: the worst %u us, acquire %u, fence %u, after root %u, lock %u, "
              "present %u, wake %u", dg->vkWorstStartUs, dg->vkWorstAcquireUs, dg->vkWorstFenceUs, dg->vkWorstAfterRootUs,
              dg->vkWorstLockUs, dg->vkWorstPresentUs, dg->vkWorstWakeUs);
        /* the frame that began it (17 ms after the one before it, 1100 us long, its lock 50 us): the gap from its end
         * to the next start, 33900 us, of which 33400 with nothing asked */
        CHECK(dg->vkWorstPrevCpuUs == 1100 && dg->vkWorstPrevLockUs == 50 && dg->vkWorstPrevFenceUs == 0 &&
              dg->vkWorstPrevAfterRootUs == 0 && dg->vkWorstGapUs == 33900 && dg->vkWorstUnaskedUs == 33400,
              "timed frames: the frame before the worst interval %u us (lock %u, fence %u, after root %u), gap %u, unasked %u",
              dg->vkWorstPrevCpuUs, dg->vkWorstPrevLockUs, dg->vkWorstPrevFenceUs, dg->vkWorstPrevAfterRootUs, dg->vkWorstGapUs,
              dg->vkWorstUnaskedUs);
        start = mark(&dg->vkFrameStart);
        FRAME(t - 100);                          /* out of order */
        t += 8000, FRAME(t);
        CHECK(dg->vkFrameStampsInvalid == invalid + 1 && newSamples(&dg->vkFrameStart, start) == 1 &&
              newUs(&dg->vkFrameStart, start) == 8100, "timed frames, a stamp before the last: %u invalid, %u intervals (%llu us)",
              dg->vkFrameStampsInvalid - invalid, newSamples(&dg->vkFrameStart, start), (unsigned long long) newUs(&dg->vkFrameStart, start));
        start = mark(&dg->vkFrameStart);
        t += 300000, FRAME(t);                   /* idle */
        t += 8000, FRAME(t);
        CHECK(dg->vkFrameIdleGaps == idle + 1 && newSamples(&dg->vkFrameStart, start) == 1 && newUs(&dg->vkFrameStart, start) == 8000,
              "timed frames, a 300 ms gap: %u idle, %u intervals", dg->vkFrameIdleGaps - idle, newSamples(&dg->vkFrameStart, start));
        start = mark(&dg->vkFrameStart), idle = dg->vkFrameIdleGaps;
        t += 250000, FRAME(t);
        t += 250001, FRAME(t);
        CHECK(newSamples(&dg->vkFrameStart, start) == 1 && newUs(&dg->vkFrameStart, start) == 250000 && dg->vkFrameIdleGaps == idle + 1,
              "timed frames: 250 ms %s, a microsecond more %s", newSamples(&dg->vkFrameStart, start) ? "an interval" : "not one",
              dg->vkFrameIdleGaps == idle + 1 ? "a gap" : "not one");
        start = mark(&dg->vkFrameStart);
        vkr.swapchain = (VkSwapchainKHR) (uintptr_t) 0x5150;
        t += 8000, FRAME(t);
        vkr.swapchain = real;
        t += 8000, FRAME(t);
        CHECK(newSamples(&dg->vkFrameStart, start) == 0, "timed frames, other swapchains: %u intervals across them",
              newSamples(&dg->vkFrameStart, start));
        delta = mark(&dg->vkPresentDelta), present = mark(&dg->vkFramePresent), wake = mark(&dg->vkFrameWake);
        t += 8000, FRAME(t, .presentStartUs = 0, .presentEndUs = 0, .wantedUs = t - 9000);  /* before the last start */
        t += 8000, FRAME(t, .wantedUs = t + 10);                                            /* after its own start */
        CHECK(newSamples(&dg->vkPresentDelta, delta) == 1 && newSamples(&dg->vkFramePresent, present) == 1 &&
              newSamples(&dg->vkFrameWake, wake) == 0, "timed frames: without a present call, present samples %u and %u (want 1, 1: "
              "the one after), wake %u (want 0)", newSamples(&dg->vkPresentDelta, delta), newSamples(&dg->vkFramePresent, present),
              newSamples(&dg->vkFrameWake, wake));
#undef FRAME
        rendererVkPacingReset();
        dg->vkWorstStartUs = 0;
    }

    /* the root published under the lock not one the renderer has: the old one not sampled, the image kept for the
     * next frame, which uses it once the root is there */
    {
        int acquires = emu.acquires2, presented = (int) st.diag.vkFramesPresented, d1 = emu.draws;
        own(40);
        st.rootWindowTextureID = root->desc.id;
        rootAtLock = 999999;
        waiting = false;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        rootAtLock = 0;
        CHECK(vkr.held && waiting && emu.acquires2 == acquires + 1 && (int) st.diag.vkFramesPresented == presented &&
              emu.draws == d1, "root gone under the lock: held %d, waiting %d, %d frames presented, %d draws", vkr.held, waiting,
              (int) st.diag.vkFramesPresented - presented, emu.draws - d1);
        st.rootWindowTextureID = root->desc.id;
        rendererVkRedraw(&waiting);
        CHECK(!vkr.held && emu.acquires2 == acquires + 1 && (int) st.diag.vkFramesPresented == presented + 1 &&
              emu.drawnFrom[d1 % 64] == root->desc.buffer, "the frame after: held %d, %d acquires, %d presented", vkr.held,
              emu.acquires2 - acquires, (int) st.diag.vkFramesPresented - presented);
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
    }

    /* a list another renderer process (alive) left: not cleared by this one; a frame of this one waits until it is over */
    {
        pthread_t other;
        own(20);
        list(root->desc.id, getppid());
        rendererVkFrameDone();
        CHECK(st.gpuInFlight.busy, "another process's list cleared by this one");
        otherOver = false;
        pthread_create(&other, NULL, otherSubmitter, NULL);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        pthread_join(other, NULL);
        CHECK(atFence.otherOver && !st.gpuInFlight.busy, "a frame recorded while another process's listed frame was not over");
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
    }

    /* the X server's wait: a lost connection, the submitter there - until it says the frame is over; a submitter that
     * no longer exists - a slice, then the list cleared */
    {
        pthread_t other;
        struct timespec t0;
        long ms;
        bool afterSubmitter;
        list(root->desc.id, getpid());
        connectionAlive = false;
        otherOver = false;
        pthread_create(&other, NULL, otherSubmitter, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        lorieGpuWaitIdle(state, false, root->desc.id);
        afterSubmitter = otherOver;
        ms = msSince(&t0);
        pthread_join(other, NULL);
        connectionAlive = true;
        CHECK(afterSubmitter && ms >= 70, "lost connection, submitter there: the wait ended after %ld ms, %s", ms,
              afterSubmitter ? "once it said so" : "before it said so");

        list(root->desc.id, deadPid());
        clock_gettime(CLOCK_MONOTONIC, &t0);
        lorieGpuWaitIdle(state, false, root->desc.id);
        ms = msSince(&t0);
        CHECK(!st.gpuInFlight.busy && ms >= 25 && ms < 1000, "submitter gone: waited %ld ms, list %s", ms,
              st.gpuInFlight.busy ? "still set" : "cleared");
    }

    /* TERMUX_X11_RENDERER_VK_TRANSFER_COPY: a new X server session with it, every image imported anew */
    {
        LorieBuffer *ts = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 50, 40), *td = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48),
                    *tb = buffer(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 50, 40), *tx = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 50, 40),
                    *ta = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 64, 48), *t32 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48),
                    *t24 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
        LoriePixmapPriv tsP = { ts }, tdP = { td }, tbP = { tb }, txP = { tx }, taP = { ta }, t32P = { t32 }, t24P = { t24 };
        PixmapRec tsPix = { { 50, 40, 24 }, &tsP }, tdPix = { { 64, 48, 24 }, &tdP }, tbPix = { { 50, 40, 24 }, &tbP },
                  txPix = { { 50, 40, 24 }, &txP }, taPix = { { 64, 48, 32 }, &taP }, t32Pix = { { 64, 48, 32 }, &t32P },
                  t24Pix = { { 64, 48, 24 }, &t24P };
        BoxRec in[2] = { { 0, 0, 20, 10 }, { 25, 15, 50, 40 } }, corner[1] = { { 0, 0, 10, 10 } }, past[1] = { { 0, 0, 60, 10 } },
               left[1] = { { 0, 0, 20, 10 } }, right[1] = { { 10, 0, 30, 10 } };
        RegionRec inR = { 2, in }, cornerR = { 1, corner }, pastR = { 1, past }, leftR = { 1, left }, rightR = { 1, right };
        const struct copied eligible[3] = { { ts, in[0], 30, 20 }, { ts, in[1], 30, 20 }, { ts, corner[0], -5, -3 } },
                            overlapping[2] = { { ts, left[0], 0, 0 }, { tx, right[0], 0, 0 } };
        void *held[8];
        uint32_t copies0, dests0, fallbacks0 = st.diag.fallbackTotal, imports0;
        int c0, r0, v0;

        own(50);
        vk.DeviceWaitIdle(vk.device);
        lorieVkImagesClear(&vk, &vkImages);
        st.backend.vkTransferCopy = 1;
        fill(ts, 1, -1); fill(td, 2, 0x11); fill(tb, 3, -1); fill(tx, 4, 0x33); fill(ta, 5, 0x11); fill(t32, 6, 0x11); fill(t24, 7, 0x11);

        /* RGBA into RGBX below depth 32: two rectangles at an offset, one of them past the destination's edge, and one
         * at a negative offset - copies, no draw, the pixels the draws give */
        copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws, c0 = emu.copyCommands, r0 = emu.copyRegions;
        d0 = emu.draws, v0 = emu.violations;
        xferMark();
        CHECK(lorieTryScheduleGpuCopy(&tsPix, &tdPix, &inR, 30, 20, &serial, &held[0]) &&
              lorieTryScheduleGpuCopy(&tsPix, &tdPix, &cornerR, -5, -3, &serial, &held[1]), "transfer: not queued");
        rendererVkApplyPendingGpuCopies();
        CHECK(vkFailures == 0 && emu.violations == v0 && st.diag.fallbackTotal == fallbacks0, "transfer: %d failures, %d violations",
              vkFailures, emu.violations - v0);
        CHECK(st.diag.vkCopyCommands - copies0 == 2 && emu.copyCommands - c0 == 2 && emu.copyRegions - r0 == 3 &&
              st.diag.vkImportedDestinationDraws == dests0 && emu.draws == d0, "transfer: copy_commands +%u (want 2), %d regions "
              "(want 3), %u destination draws, %d draws (want none)", st.diag.vkCopyCommands - copies0, emu.copyRegions - r0,
              st.diag.vkImportedDestinationDraws - dests0, emu.draws - d0);
        CHECK((usageOfBuffer(ts) & TRANSFER_USAGE) == TRANSFER_USAGE && (usageOfBuffer(td) & TRANSFER_USAGE) == TRANSFER_USAGE,
              "transfer: images imported without the transfer usage");
        CHECK(lorieGpuCopyIsDone(serial) && !st.gpuInFlight.busy && notified.cleared && notified.fenceWaited && notified.released,
              "transfer: done %d, published %s", lorieGpuCopyIsDone(serial), !notified.cleared ? "with its buffers in flight" :
              !notified.fenceWaited ? "before the fence" : !notified.released ? "with a buffer still the renderer's" : "after the fence");
        st.gpuInFlight.busy = 0;    /* if left set, reported above: not left to stall the frames after */
        CHECK(wrongPixels(td, 2, eligible, 3) == 0, "transfer: %d pixels not what the draws give", wrongPixels(td, 2, eligible, 3));
        XFER_MOVED("checked+2 imports_GRANTED+2", "transfer");
        lorieGpuCopyAck(&tsPix, held[0]);
        lorieGpuCopyAck(&tsPix, held[1]);

        /* in a frame, into the root it then samples: the copy, then the root drawn from what the copy wrote */
        {
            const struct copied intoRoot[2] = { { ts, in[0], 100, 50 }, { ts, in[1], 100, 50 } };
            fill(root, 8, 0x11);
            copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws, v0 = emu.violations, d0 = emu.draws;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&tsPix, &rootPix, &inR, 100, 50, &serial, &held[0]), "transfer, frame: not queued");
            st.rootWindowTextureID = root->desc.id;
            st.drawRequested = 1;
            rendererVkRedraw(&waiting);
            CHECK(vkFailures == 0 && emu.violations == v0 && lorieGpuCopyIsDone(serial) && st.diag.vkCopyCommands - copies0 == 1 &&
                  st.diag.vkImportedDestinationDraws == dests0 && emu.draws - d0 == 1 && emu.drawnFrom[d0 % 64] == root->desc.buffer,
                  "transfer, frame: %d violations, copy_commands +%u (want 1), destination draws +%u, %d draws (want the root's)",
                  emu.violations - v0, st.diag.vkCopyCommands - copies0, st.diag.vkImportedDestinationDraws - dests0, emu.draws - d0);
            CHECK(wrongPixels(root, 8, intoRoot, 2) == 0, "transfer, frame: %d root pixels wrong", wrongPixels(root, 8, intoRoot, 2));
            XFER_MOVED("checked+1 imports_GRANTED+1", "transfer, frame");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&tsPix, held[0]);
        }

        /* two copies into one destination, overlapping: the second ordered after the first, and over it; RGBX into RGBX
         * below depth 32 copies the 4th byte as it is */
        c0 = emu.copyCommands, v0 = emu.violations;
        xferMark();
        CHECK(lorieTryScheduleGpuCopy(&tsPix, &t24Pix, &leftR, 0, 0, &serial, &held[0]) &&
              lorieTryScheduleGpuCopy(&txPix, &t24Pix, &rightR, 0, 0, &serial, &held[1]), "transfer, overlapping: not queued");
        rendererVkApplyPendingGpuCopies();
        CHECK(vkFailures == 0 && emu.violations == v0 && emu.copyCommands - c0 == 2 && lorieGpuCopyIsDone(serial),
              "transfer, overlapping: %d copies (want 2), %d violations", emu.copyCommands - c0, emu.violations - v0);
        CHECK(copiesOrdered(lorieVkImageFind(&vkImages, t24->desc.id)->image), "transfer, overlapping: the second copy not ordered after the first");
        CHECK(wrongPixels(t24, 7, overlapping, 2) == 0, "transfer, overlapping: %d pixels wrong", wrongPixels(t24, 7, overlapping, 2));
        XFER_MOVED("checked+2 imports_GRANTED+2", "transfer, overlapping");
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&tsPix, held[0]);
        lorieGpuCopyAck(&txPix, held[1]);

        /* drawn, as GLES draws them: BGRA into RGBX (two formats), RGBX into RGBA (the draw's alpha 1), RGBX into RGBX
         * of depth 32 (alpha there), a rectangle past its source (the draw samples the edge), two destinations
         * overlapping, a rectangle all outside the destination - each counted under its own condition, the last one
         * kept as it was */
        {
            BoxRec overlap[2] = { { 0, 0, 20, 10 }, { 10, 0, 30, 10 } };
            RegionRec overlapR = { 2, overlap };
            copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws, c0 = emu.copyCommands, v0 = emu.violations;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&tbPix, &tdPix, &inR, 3, 4, &serial, &held[0]) &&
                  lorieTryScheduleGpuCopy(&txPix, &taPix, &inR, 3, 4, &serial, &held[1]) &&
                  lorieTryScheduleGpuCopy(&txPix, &t32Pix, &inR, 3, 4, &serial, &held[2]) &&
                  lorieTryScheduleGpuCopy(&tsPix, &tdPix, &pastR, 0, 0, &serial, &held[3]) &&
                  lorieTryScheduleGpuCopy(&tsPix, &tdPix, &overlapR, 0, 0, &serial, &held[4]) &&
                  lorieTryScheduleGpuCopy(&tsPix, &tdPix, &cornerR, 100, 7, &serial, &held[5]), "drawn: not queued");
            memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
            pushes = 0;
            rendererVkApplyPendingGpuCopies();
            CHECK(vkFailures == 0 && emu.violations == v0 && lorieGpuCopyIsDone(serial) && st.diag.vkCopyCommands == copies0 &&
                  emu.copyCommands == c0 && st.diag.vkImportedDestinationDraws - dests0 == 10, "drawn: copy_commands +%u (want 0), "
                  "destination draws +%u (want 10), %d violations", st.diag.vkCopyCommands - copies0,
                  st.diag.vkImportedDestinationDraws - dests0, emu.violations - v0);
            st.gpuInFlight.busy = 0;
            CHECK(gles(&saved) == serial && glDrawCount == 10 && pushes == 10, "drawn: GLES %d draws, Vulkan %d", glDrawCount, pushes);
            for (int k = 0; k < 10 && k < glDrawCount; k++)
                CHECK(sameAsGles(k, &pushed[k]), "drawn: draw %d not what GLES draws", k);
            XFER_MOVED("checked+6 FORMAT+1 RGBX_ALPHA+1 DEPTH32+1 RECT_SOURCE+1 RECT_OVERLAP+1 RECT_NONE+1 imports_GRANTED+3", "drawn");
            CHECK(st.diag.vkXferLastNot == 1u << LORIE_VK_XFER_NOT_RECT_NONE && st.diag.vkXferLastSrcFormat == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM &&
                  st.diag.vkXferLastDstFormat == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM && st.diag.vkXferLastSrcWidth == 50 &&
                  st.diag.vkXferLastSrcHeight == 40 && st.diag.vkXferLastDstWidth == 64 && st.diag.vkXferLastDstHeight == 48 &&
                  st.diag.vkXferLastDstDepth == 24 && st.diag.vkXferLastSrcUsage == usageOfBuffer(ts) &&
                  st.diag.vkXferLastDstUsage == usageOfBuffer(td) && st.diag.vkXferLastSrcAhbUsage == X_USAGE && st.diag.vkXferLastRects == 1 &&
                  st.diag.vkXferLastRect[0] == 0 && st.diag.vkXferLastRect[1] == 0 && st.diag.vkXferLastRect[2] == 10 &&
                  st.diag.vkXferLastRect[3] == 10 && st.diag.vkXferLastOff[0] == 100 && st.diag.vkXferLastOff[1] == 7,
                  "drawn: the last copy kept from it not as it was (conditions %#x, %u -> %u, %ux%u -> %ux%u depth %u)",
                  st.diag.vkXferLastNot, st.diag.vkXferLastSrcFormat, st.diag.vkXferLastDstFormat, st.diag.vkXferLastSrcWidth,
                  st.diag.vkXferLastSrcHeight, st.diag.vkXferLastDstWidth, st.diag.vkXferLastDstHeight, st.diag.vkXferLastDstDepth);
            lorieGpuCopyAck(&tbPix, held[0]);
            lorieGpuCopyAck(&txPix, held[1]);
            lorieGpuCopyAck(&txPix, held[2]);
            lorieGpuCopyAck(&tsPix, held[3]);
            lorieGpuCopyAck(&tsPix, held[4]);
            lorieGpuCopyAck(&tsPix, held[5]);
        }

        /* a device that refuses the transfer usage of these AHBs: imported without it, nothing recorded, drawn */
        {
            LorieBuffer *rs = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 50, 40), *rd = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
            LoriePixmapPriv rsP = { rs }, rdP = { rd };
            PixmapRec rsPix = { { 50, 40, 24 }, &rsP }, rdPix = { { 64, 48, 24 }, &rdP };
            cfg.refuseTransferUsage = true;
            imports0 = st.diag.vkAhbImportSuccess, copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&rsPix, &rdPix, &inR, 0, 0, &serial, &held[0]), "refused: not queued");
            rendererVkApplyPendingGpuCopies();
            cfg.refuseTransferUsage = false;
            CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial) && st.diag.vkAhbImportSuccess - imports0 == 2 &&
                  st.diag.fallbackTotal == fallbacks0 && !(usageOfBuffer(rs) & TRANSFER_USAGE) && !(usageOfBuffer(rd) & TRANSFER_USAGE),
                  "refused: %d failures, %u imports (want 2), %u fallbacks, transfer usage %#x %#x", vkFailures,
                  st.diag.vkAhbImportSuccess - imports0, st.diag.fallbackTotal - fallbacks0, usageOfBuffer(rs) & TRANSFER_USAGE,
                  usageOfBuffer(rd) & TRANSFER_USAGE);
            CHECK(st.diag.vkCopyCommands == copies0 && st.diag.vkImportedDestinationDraws - dests0 == 2, "refused: copy_commands +%u, "
                  "destination draws +%u (want 0, 2)", st.diag.vkCopyCommands - copies0, st.diag.vkImportedDestinationDraws - dests0);
            XFER_MOVED("checked+1 SRC_USAGE+1 DST_USAGE+1 missing_REFUSED+2 imports_REFUSED+2", "refused");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&rsPix, held[0]);
        }

        /* a device that allows the usage and then fails the image made with it: made again without it, nothing
         * recorded, drawn */
        {
            LorieBuffer *fs = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 50, 40), *fdst = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
            LoriePixmapPriv fsP = { fs }, fdstP = { fdst };
            PixmapRec fsPix = { { 50, 40, 24 }, &fsP }, fdstPix = { { 64, 48, 24 }, &fdstP };
            cfg.failTransferImage = true;
            imports0 = st.diag.vkAhbImportSuccess, copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&fsPix, &fdstPix, &inR, 0, 0, &serial, &held[0]), "failed image: not queued");
            rendererVkApplyPendingGpuCopies();
            cfg.failTransferImage = false;
            CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial) && st.diag.vkAhbImportSuccess - imports0 == 2 &&
                  st.diag.fallbackTotal == fallbacks0 && !(usageOfBuffer(fs) & TRANSFER_USAGE) && !(usageOfBuffer(fdst) & TRANSFER_USAGE) &&
                  st.diag.vkCopyCommands == copies0 && st.diag.vkImportedDestinationDraws - dests0 == 2,
                  "failed image: %d failures, %u imports (want 2), %u fallbacks, copy_commands +%u, destination draws +%u (want 2)",
                  vkFailures, st.diag.vkAhbImportSuccess - imports0, st.diag.fallbackTotal - fallbacks0,
                  st.diag.vkCopyCommands - copies0, st.diag.vkImportedDestinationDraws - dests0);
            XFER_MOVED("checked+1 SRC_USAGE+1 DST_USAGE+1 missing_FAILED+2 imports_FAILED+2", "failed image");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&fsPix, held[0]);
        }

        /* BGRA into RGBX of depth 24, once the device check passed: copied as raw bits (B8G8R8A8_UNORM into
         * R8G8B8A8_UNORM, size-compatible), every byte the source's and the shader copy's - red and blue apart in every
         * source pixel, so a swap shows; the check not passed, or depth 32: drawn */
        {
            LorieBuffer *gs = buffer(AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, 50, 40), *gd = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48),
                        *g32 = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 64, 48);
            LoriePixmapPriv gsP = { gs }, gdP = { gd }, g32P = { g32 };
            PixmapRec gsPix = { { 50, 40, 24 }, &gsP }, gdPix = { { 64, 48, 24 }, &gdP }, g32Pix = { { 64, 48, 32 }, &g32P };
            const struct copied bgra[2] = { { gs, in[0], 2, 5 }, { gs, in[1], 2, 5 } };
            const AHardwareBuffer *ga = gs->desc.buffer;
            int apart = 1;
            fill(gs, 0xc3, -1); fill(gd, 9, 0x11); fill(g32, 10, 0x11);
            for (uint32_t y = 0; y < ga->desc.height; y++)
                for (uint32_t x = 0; x < ga->desc.width; x++)
                    apart &= ga->cpu[((size_t) y * ga->desc.stride + x) * 4] != ga->cpu[((size_t) y * ga->desc.stride + x) * 4 + 2];
            CHECK(apart, "BGRA into RGBX: a source pixel with red and blue alike");

            CHECK(lorieVkTransferCheck(&vk, &st) == LORIE_VK_XFER_CHECK_PASS, "BGRA into RGBX: the device check did not pass");
            copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws, c0 = emu.copyCommands, v0 = emu.violations;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&gsPix, &gdPix, &inR, 2, 5, &serial, &held[0]), "BGRA into RGBX: not queued");
            rendererVkApplyPendingGpuCopies();
            CHECK(vkFailures == 0 && emu.violations == v0 && lorieGpuCopyIsDone(serial) && st.diag.vkCopyCommands - copies0 == 1 &&
                  emu.copyCommands - c0 == 1 && st.diag.vkImportedDestinationDraws == dests0 && st.diag.fallbackTotal == fallbacks0,
                  "BGRA into RGBX: copy_commands +%u (want 1), destination draws +%u (want 0), %d violations",
                  st.diag.vkCopyCommands - copies0, st.diag.vkImportedDestinationDraws - dests0, emu.violations - v0);
            XFER_MOVED("checked+1 imports_GRANTED+2", "BGRA into RGBX");
            CHECK(wrongPixels(gd, 9, bgra, 2) == 0, "BGRA into RGBX: %d pixels not the source's bytes", wrongPixels(gd, 9, bgra, 2));
            CHECK(notAsShader(gs, gd, in, 2, 2, 5) == 0, "BGRA into RGBX: %d pixels not the shader copy's", notAsShader(gs, gd, in, 2, 2, 5));
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&gsPix, held[0]);

            /* the shader copy itself, on the same bytes: what it gives is what the copy gave */
            {
                uint8_t px[4] = { 0x11, 0x22, 0x33, 0x44 }, out[4];
                shaderPixel(px, VK_FORMAT_B8G8R8A8_UNORM, false, LORIE_VK_QUAD_SWIZZLE, VK_FORMAT_R8G8B8A8_UNORM, out);
                CHECK(!memcmp(px, out, 4), "BGRA into RGBX: the shader copy gives %02x %02x %02x %02x of 11 22 33 44", out[0], out[1],
                      out[2], out[3]);
            }

            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&gsPix, &g32Pix, &inR, 2, 5, &serial, &held[0]), "BGRA into RGBX of depth 32: not queued");
            rendererVkApplyPendingGpuCopies();
            XFER_MOVED("checked+1 FORMAT+1 imports_GRANTED+1", "BGRA into RGBX of depth 32");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&gsPix, held[0]);

            vk.transferCheck = LORIE_VK_XFER_CHECK_FAIL;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&gsPix, &gdPix, &inR, 2, 5, &serial, &held[0]), "BGRA into RGBX, check failed: not queued");
            rendererVkApplyPendingGpuCopies();
            XFER_MOVED("checked+1 FORMAT+1", "BGRA into RGBX, check failed");
            CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial), "BGRA into RGBX, check failed: %d failures", vkFailures);
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&gsPix, held[0]);
            vk.transferCheck = LORIE_VK_XFER_CHECK_NONE;
        }

        /* without it: drawn, images with the usage too; new imports neither ask for the usage nor about the features */
        {
            LorieBuffer *os = buffer(AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 50, 40);
            LoriePixmapPriv osP = { os };
            PixmapRec osPix = { { 50, 40, 24 }, &osP };
            int features0 = imp.featureQueries;
            st.backend.vkTransferCopy = 0;
            copies0 = st.diag.vkCopyCommands, dests0 = st.diag.vkImportedDestinationDraws;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&tsPix, &tdPix, &inR, 0, 0, &serial, &held[0]) &&
                  lorieTryScheduleGpuCopy(&osPix, &tdPix, &inR, 0, 0, &serial, &held[1]), "off: not queued");
            rendererVkApplyPendingGpuCopies();
            CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial) && st.diag.vkCopyCommands == copies0 &&
                  st.diag.vkImportedDestinationDraws - dests0 == 4 && imp.featureQueries == features0 &&
                  !(usageOfBuffer(os) & TRANSFER_USAGE), "off: copy_commands +%u, destination draws +%u (want 0, 4), %d feature "
                  "queries, transfer usage %#x", st.diag.vkCopyCommands - copies0, st.diag.vkImportedDestinationDraws - dests0,
                  imp.featureQueries - features0, usageOfBuffer(os) & TRANSFER_USAGE);
            XFER_MOVED("checked+0", "off");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&tsPix, held[0]);
            lorieGpuCopyAck(&osPix, held[1]);

            /* on again: the image imported while it was off has no usage, counted as not asked for */
            st.backend.vkTransferCopy = 1;
            xferMark();
            CHECK(lorieTryScheduleGpuCopy(&osPix, &tdPix, &inR, 0, 0, &serial, &held[0]), "unasked: not queued");
            rendererVkApplyPendingGpuCopies();
            CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial), "unasked: %d failures", vkFailures);
            XFER_MOVED("checked+1 SRC_USAGE+1 missing_UNASKED+1", "unasked");
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&osPix, held[0]);
            st.backend.vkTransferCopy = 0;
        }
    }

    /* P9-B0: each part of a Present copy timed - per entry its queue wait and its whole latency, per submission
     * (weighted by its entries) the lock, the wait for another process's listed frame, the recording and the fence -
     * a frame's apart; on its own or in a frame; the X server's re-execution once per publish */
    {
        struct lorie_renderer_diag b;
        RegionRec one = { 1, &boxes[1] };
        uint64_t serials[3], before;
#define MOVED(f) (st.diag.f - b.f)
        own(60);
        st.gpuInFlight.busy = 0;

        /* on its own: queued 5 ms before the renderer starts, the device 10 ms at the fence */
        memcpy(&b, (const void *) &st.diag, sizeof b);
        before = lorieMonotonicUs();
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "timed: not queued");
        e = &st.gpuCopyQueue.entries[(st.gpuCopyQueue.writeIndex - 1) % LORIE_GPU_COPY_QUEUE_CAPACITY];
        CHECK(e->queuedUs >= before && e->queuedUs <= lorieMonotonicUs(), "timed: queued at %llu, not when queued",
              (unsigned long long) e->queuedUs);
        usleep(5000);
        gpuMs = 10;
        rendererVkApplyPendingGpuCopies();
        gpuMs = 0;
        CHECK(vkFailures == 0 && lorieGpuCopyIsDone(serial) && MOVED(copyStandaloneEntries) == 1 && MOVED(copyStandaloneSubmits) == 1 &&
              MOVED(copyFrameEntries) == 0 && MOVED(copyFrameSubmits) == 0, "timed: %u/%u entries/submissions on their own, %u/%u in a frame",
              MOVED(copyStandaloneEntries), MOVED(copyStandaloneSubmits), MOVED(copyFrameEntries), MOVED(copyFrameSubmits));
        CHECK(MOVED(copyQueue.count) == 1 && MOVED(copyTotal.count) == 1 && MOVED(copyLock.count) == 1 && MOVED(copyRecord.count) == 1 &&
              MOVED(copyInflightWait.count) == 1 && MOVED(copyGpuWait.count) == 1 && MOVED(copyFrameGpuWait.count) == 0,
              "timed: samples queue %u total %u lock %u in flight %u record %u fence %u, a frame's fence %u (want 1 each, 0)",
              MOVED(copyQueue.count), MOVED(copyTotal.count), MOVED(copyLock.count), MOVED(copyInflightWait.count), MOVED(copyRecord.count),
              MOVED(copyGpuWait.count), MOVED(copyFrameGpuWait.count));
        CHECK(MOVED(copyQueue.sumUs) >= 5000 && MOVED(copyGpuWait.sumUs) >= 10000 &&
              MOVED(copyTotal.sumUs) >= MOVED(copyQueue.sumUs) + MOVED(copyLock.sumUs) + MOVED(copyInflightWait.sumUs) +
                                        MOVED(copyRecord.sumUs) + MOVED(copyGpuWait.sumUs) &&
              st.diag.copyQueue.maxUs >= 5000 && st.diag.copyGpuWait.maxUs >= 10000 && st.diag.copyTotal.maxUs >= 15000,
              "timed: queue %llu us, fence %llu us, total %llu us (want >= 5000, >= 10000, >= their sum)",
              (unsigned long long) MOVED(copyQueue.sumUs), (unsigned long long) MOVED(copyGpuWait.sumUs), (unsigned long long) MOVED(copyTotal.sumUs));
        CHECK(MOVED(copyQueue.buckets[0]) == 0 && MOVED(copyGpuWait.buckets[0]) == 0, "timed: a 5 ms wait in the first bucket");
        CHECK(st.gpuCopyQueue.completedUs >= e->queuedUs && notifiedPublishedUs == st.gpuCopyQueue.completedUs,
              "timed: published before queued, or the notification without that publish's stamp");
        lorieCountCopyAck();
        lorieCountCopyAck();
        CHECK(MOVED(copyAck.count) == 1, "timed: the X server's re-execution counted %u times for one publish", MOVED(copyAck.count));
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&srcPix, dstBuffer);

        /* three entries in one submission: three entries' latencies, one submission's */
        memcpy(&b, (const void *) &st.diag, sizeof b);
        for (int k = 0; k < 3; k++)
            CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, (int16_t) k, 0, &serials[k], &dstBuffer), "three: %d not queued", k);
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(serials[2]) && MOVED(copyStandaloneEntries) == 3 && MOVED(copyStandaloneSubmits) == 1 &&
              MOVED(copyQueue.count) == 3 && MOVED(copyTotal.count) == 3 && MOVED(copyLock.count) == 1 && MOVED(copyRecord.count) == 1 &&
              MOVED(copyGpuWait.count) == 1, "three: %u entries, %u submissions; samples queue %u total %u lock %u record %u fence %u",
              MOVED(copyStandaloneEntries), MOVED(copyStandaloneSubmits), MOVED(copyQueue.count), MOVED(copyTotal.count),
              MOVED(copyLock.count), MOVED(copyRecord.count), MOVED(copyGpuWait.count));
        CHECK(MOVED(copyLock.weightedUs) == 3 * MOVED(copyLock.sumUs) && MOVED(copyRecord.weightedUs) == 3 * MOVED(copyRecord.sumUs) &&
              MOVED(copyGpuWait.weightedUs) == 3 * MOVED(copyGpuWait.sumUs) && MOVED(copyQueue.weightedUs) == MOVED(copyQueue.sumUs) &&
              MOVED(copyTotal.weightedUs) == MOVED(copyTotal.sumUs), "three: a submission's parts not weighted by its 3 entries");
        st.gpuInFlight.busy = 0;
        for (int k = 0; k < 3; k++)
            lorieGpuCopyAck(&srcPix, NULL);

        /* in a frame */
        memcpy(&b, (const void *) &st.diag, sizeof b);
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "in a frame: not queued");
        st.rootWindowTextureID = root->desc.id;
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(lorieGpuCopyIsDone(serial) && MOVED(copyFrameEntries) == 1 && MOVED(copyFrameSubmits) == 1 &&
              MOVED(copyStandaloneEntries) == 0 && MOVED(copyStandaloneSubmits) == 0 && MOVED(copyTotal.count) == 1 &&
              MOVED(copyFrameGpuWait.count) == 1 && MOVED(copyGpuWait.count) == 0, "in a frame: %u/%u entries/submissions in a "
              "frame, %u/%u on their own; fence samples %u a frame's, %u the copies' alone", MOVED(copyFrameEntries),
              MOVED(copyFrameSubmits), MOVED(copyStandaloneEntries), MOVED(copyStandaloneSubmits), MOVED(copyFrameGpuWait.count),
              MOVED(copyGpuWait.count));
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&srcPix, dstBuffer);

        /* a frame without copies: nothing */
        memcpy(&b, (const void *) &st.diag, sizeof b);
        st.drawRequested = 1;
        rendererVkRedraw(&waiting);
        CHECK(MOVED(copyFrameSubmits) == 0 && MOVED(copyLock.count) == 0 && MOVED(copyFrameGpuWait.count) == 0,
              "a frame without copies counted as one with them");
        st.gpuInFlight.busy = 0;

        /* another renderer process's frame listed in flight for 80 ms: that wait apart, not in the recording */
        {
            pthread_t other;
            memcpy(&b, (const void *) &st.diag, sizeof b);
            CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "in flight: not queued");
            list(root->desc.id, getppid());
            otherOver = false;
            pthread_create(&other, NULL, otherSubmitter, NULL);
            rendererVkApplyPendingGpuCopies();
            pthread_join(other, NULL);
            CHECK(lorieGpuCopyIsDone(serial) && MOVED(copyInflightWait.count) == 1 && MOVED(copyInflightWait.sumUs) >= 70000 &&
                  MOVED(copyRecord.sumUs) < 70000 && MOVED(copyTotal.sumUs) >= MOVED(copyInflightWait.sumUs),
                  "in flight: the wait %llu us, the recording %llu us (want >= 70000 apart, the recording without it)",
                  (unsigned long long) MOVED(copyInflightWait.sumUs), (unsigned long long) MOVED(copyRecord.sumUs));
            st.gpuInFlight.busy = 0;
            lorieGpuCopyAck(&srcPix, dstBuffer);
        }
#undef MOVED
    }

    /* P10-S1: what may still happen to a queued copy, as lorieGpuCopySettle finds it - waiting for nothing */
    {
        RegionRec one = { 1, &boxes[1] };
        uint32_t dests;
        own(70);
        st.gpuInFlight.busy = 0;
        connectionAlive = true;

        /* not recorded: cancelled - Vulkan then skips it, completing its serial without a draw */
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "settle: not queued");
        e = &st.gpuCopyQueue.entries[(st.gpuCopyQueue.writeIndex - 1) % LORIE_GPU_COPY_QUEUE_CAPACITY];
        {
            uint32_t counted = st.diag.xCopyCancelled;
            CHECK(!e->cancelled && lorieGpuCopySettle(serial) == LORIE_GPU_COPY_CANCELLED && e->cancelled &&
                  lorieGpuCopySettle(serial) == LORIE_GPU_COPY_CANCELLED && st.lock == 0 && st.diag.xCopyCancelled == counted + 1,
                  "not recorded: settled %d, cancelled %d, lock %d, counted %u (want CANCELLED twice, marked, let go, once)",
                  lorieGpuCopySettle(serial), e->cancelled, st.lock, st.diag.xCopyCancelled - counted);
        }
        dests = st.diag.vkImportedDestinationDraws;
        memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(serial) && st.diag.vkImportedDestinationDraws == dests && vkFailures == 0 &&
              lorieGpuCopySettle(serial) == LORIE_GPU_COPY_DONE, "cancelled: Vulkan drew it (%u draws) or did not complete it",
              st.diag.vkImportedDestinationDraws - dests);
        /* ... and GLES too */
        glDrawCount = 0;
        CHECK(gles(&saved) == serial && glDrawCount == 0, "cancelled: GLES drew it (%d draws)", glDrawCount);
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&srcPix, dstBuffer);

        /* recorded, its buffers in flight with a renderer that exists: running, not marked; that frame over (its list
         * cleared, its completion not yet published): no renderer touches it any more - settled */
        CHECK(lorieTryScheduleGpuCopy(&srcPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "settle: not queued");
        e = &st.gpuCopyQueue.entries[(st.gpuCopyQueue.writeIndex - 1) % LORIE_GPU_COPY_QUEUE_CAPACITY];
        list(src->desc.id, getppid());
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_RUNNING && !e->cancelled, "in flight: settled %d, marked %d (want RUNNING, no)",
              lorieGpuCopySettle(serial), e->cancelled);
        st.gpuInFlight.busy = 0;
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_CANCELLED && e->cancelled, "its frame over: settled %d", lorieGpuCopySettle(serial));

        /* in flight with a renderer process that no longer exists: settled, marked so no later renderer records it */
        e->cancelled = 0;
        list(root->desc.id, deadPid());
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_CANCELLED && e->cancelled, "its renderer gone: settled %d, marked %d",
              lorieGpuCopySettle(serial), e->cancelled);
        st.gpuInFlight.busy = 0;

        /* the shared lock held by a renderer that exists: running (asked again later), nothing marked; by one that died
         * holding it, with no renderer connected: taken over, settled */
        e->cancelled = 0;
        lockHolder = st.lockingPid = getppid();
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_RUNNING && !e->cancelled, "lock held: settled %d, marked %d (want RUNNING, no)",
              lorieGpuCopySettle(serial), e->cancelled);
        connectionAlive = false;
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_RUNNING, "lock held, disconnected, holder alive: settled %d (want RUNNING)",
              lorieGpuCopySettle(serial));
        lockHolder = st.lockingPid = deadPid();
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_CANCELLED && e->cancelled && st.lock == 0,
              "lock held by a dead renderer: settled %d, marked %d, lock %d", lorieGpuCopySettle(serial), e->cancelled, st.lock);
        lockHolder = 0;
        st.lockingPid = 0;
        connectionAlive = true;
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopySettle(serial) == LORIE_GPU_COPY_DONE, "consumed: settled %d (want DONE)", lorieGpuCopySettle(serial));
        st.gpuInFlight.busy = 0;
        lorieGpuCopyAck(&srcPix, dstBuffer);
    }

    CHECK(!atFence.lockHeld, "a fence waited for under the X server's lock");
    CHECK(emu.violations == 0 && src->refs == 0 && fresh->refs == 0 && vfb.rootGpuCopyPending == 0, "at the end: %d violations, held refs",
          emu.violations);
    CHECK(emu.earlyPresents == earlyWithOption, "%d presents before their frame's fence with TERMUX_X11_RENDERER_VK_SUBMIT_OPT off",
          emu.earlyPresents - earlyWithOption);
    printf("TPRESENTCOPY a Present copy from the X server through Vulkan and back, as GLES does it: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
