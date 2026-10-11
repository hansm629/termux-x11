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
 *   published only after the fence, under the X server's lock, with every buffer back with the foreign owner; a
 *   failed submission publishes nothing; a frame with no copy queued draws none; an entry whose buffer the
 *   renderer never got is skipped and completed, as GLES does. Frames of eight copies from new sources, the
 *   cache passing 16 and 32 images in the middle of a frame: every image a frame already used stays valid (its
 *   acquire released, its draw from the right buffer), every image keeps its address until removed.
 *   X server again: done only then; the CPU may lock the source; the acknowledgement drops what was held. */
#include <math.h>
#include <pthread.h>
#include "fakevk.h"

/* ---- LorieBuffer: buffer.h's description; registration and references as the test keeps them ---- */
#define __unused __attribute__((unused))
#define _Nullable
#define __LIBC_HIDDEN__ static
#include "buffertypes_src.inc"
typedef struct LorieBuffer { LorieBuffer_Desc desc; int refs, gpuCopyPending; bool registered; } LorieBuffer;
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
typedef struct { struct { int width, height; } drawable; LoriePixmapPriv *priv; } PixmapRec, *PixmapPtr;
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
static pthread_cond_t *rendererCond;
static int wakeups;
#define pthread_cond_signal(c) ((void) (c), wakeups++)

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
static GLuint LorieBuffer_getGLTextureId(LorieBuffer *b) { return (GLuint) b->desc.id; }
static void LorieBuffer_bindTexture(LorieBuffer *b) { glTexture = (GLuint) b->desc.id; }
static bool lorieDebugEnabled;
static int filtering = GL_NEAREST;

/* ---- the renderer's side ---- */
static volatile struct lorie_shared_server_state *state;
static struct lorie_vk vk;
static struct lorie_vk_images vkImages;
static struct lorie_vk_renderer vkr;
static bool vkOwned, backendSelect;
static uint32_t vkOwnershipSeq;
static volatile int viewportX, viewportY, viewportW = 1080, viewportH = 2400, expectedW, expectedH;
#define CVT_H_GRANULARITY 8
static ANativeWindow window = { 1080, 2400 }, *win = &window;
static pthread_spinlock_t bufferLock;
static LorieBuffer bufs[64];
static int nbufs, vkFailures;
#define buffers rendererBuffers     /* fakevk.h has its own `buffers` */
static int buffers, removedBuffers;
/* the renderer's lists: what the X server registered */
static LorieBuffer *LorieBufferList_findById(volatile void *list, uint64_t id) {
    for (int i = 0; list == &buffers && i < nbufs; i++)
        if (bufs[i].registered && bufs[i].desc.id == id)
            return &bufs[i];
    return NULL;
}
static LorieBuffer *rendererFindBufferWithRetry(uint64_t id) { return LorieBufferList_findById(&buffers, id); }
static void lorie_mutex_lock(volatile int *m, volatile int *pid) { (*m)++; *pid = 1; }
static void lorie_mutex_unlock(volatile int *m, volatile int *pid) { (*m)--; *pid = 0; }
static void rendererVkFail(void) { vkFailures++; }
/* the X server woken for a finished batch: how things stood at that moment */
static struct { int count; bool locked, fenceWaited, released; uint64_t completed; } notified;
static void notifyGpuCopyDone(void) {
    notified.count++;
    notified.locked = state->lock == 1;
    notified.fenceWaited = !emu.fencePending;
    notified.released = true;
    for (int i = 0; i < nbufs; i++)
        if (bufs[i].desc.buffer && bufs[i].desc.buffer->owned)
            notified.released = false;
    notified.completed = state->gpuCopyQueue.completedSerial;
}
#include "presentcopy_src.inc"

#define X_USAGE (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | \
                 AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER)

static struct lorie_shared_server_state st, glesState;
typedef __typeof__(st.gpuCopyQueue) Queue;

/* an X server pixmap's buffer, described as buffer.c's allocate() describes an AHB: by the AHB itself */
static LorieBuffer *buffer(uint32_t format, uint32_t width, uint32_t height) {
    AHardwareBuffer_Desc d = { .width = width, .height = height, .layers = 1, .format = format, .usage = X_USAGE };
    LorieBuffer *b = &bufs[nbufs++];
    AHardwareBuffer *a = NULL;
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

int main(void) {
    LorieBuffer *root, *src, *bgra, *redirected, *fresh, *fd;
    LoriePixmapPriv rootP, srcP, bgraP, redirectedP, freshP, fdP, memP;
    PixmapRec rootPix, srcPix, bgraPix, redirectedPix, freshPix, fdPix, memPix;
    BoxRec boxes[17] = { { 0, 0, 50, 20 }, { 60, 30, 200, 100 } };
    RegionRec two = { 2, boxes };
    LorieGpuCopyEntry *e;
    uint64_t serial = 0;
    void *dstBuffer = NULL;
    bool waiting = false;
    int d0;
    Queue saved;

    pthread_spin_init(&bufferLock, PTHREAD_PROCESS_PRIVATE);
    good();
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
    CHECK(notified.locked && notified.fenceWaited && notified.released,
          "copies alone: published %s", !notified.locked ? "without the X server's lock" : !notified.fenceWaited ?
          "before the fence was waited for" : "with a buffer still the renderer's");
    CHECK(gles(&saved) == 1 && glesState.gpuCopyQueue.readIndex == 1 && glDrawCount == 2 && pushes == 2, "GLES: %d draws", glDrawCount);
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

    /* a frame: a BGRA client's whole window into a redirected window's pixmap, then the root on screen */
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
    CHECK(st.gpuCopyQueue.readIndex == 2 && st.gpuCopyQueue.completedSerial == 2 && notified.count == 2 && notified.locked &&
          notified.fenceWaited && notified.released, "frame: readIndex %u, completed %llu", st.gpuCopyQueue.readIndex,
          (unsigned long long) st.gpuCopyQueue.completedSerial);
    CHECK(gles(&saved) == 2 && glDrawCount == 1 && sameAsGles(0, &pushed[0]) && glDraws[0].program == g_texture_program_bgra,
          "frame: BGRA into RGBX not drawn as GLES draws it");
    if (!sameAsGles(0, &pushed[0]))
        printQuads(1);
    CHECK(lorieGpuCopyIsDone(2), "frame: not done");
    lorieGpuCopyAck(&bgraPix, dstBuffer);
    CHECK(bgra->refs == 0 && redirected->refs == 0 && redirected->gpuCopyPending == 0, "frame acknowledged: something still held");

    /* a plain desktop frame: nothing queued, no copy drawn */
    st.drawRequested = 1;
    rendererVkRedraw(&waiting);
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
        CHECK(gles(&saved) == serials[7] && glDrawCount == 8, "eight: GLES drew %d", glDrawCount);
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

    /* an entry whose source the renderer never got: skipped and completed, as GLES does */
    {
        RegionRec one = { 1, boxes };
        uint32_t dests = st.diag.vkImportedDestinationDraws;
        CHECK(lorieTryScheduleGpuCopy(&freshPix, &rootPix, &one, 0, 0, &serial, &dstBuffer), "never got: not queued");
        fresh->registered = false;
        memcpy(&saved, (const void *) &st.gpuCopyQueue, sizeof saved);
        rendererVkApplyPendingGpuCopies();
        CHECK(lorieGpuCopyIsDone(serial) && st.diag.vkImportedDestinationDraws == dests && vkFailures == 0 &&
              gles(&saved) == serial && glDrawCount == 0, "never got: not skipped as GLES skips it");
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

    CHECK(emu.violations == 0 && src->refs == 0 && fresh->refs == 0 && vfb.rootGpuCopyPending == 0, "at the end: %d violations, held refs",
          emu.violations);
    printf("TPRESENTCOPY a Present copy from the X server through Vulkan and back, as GLES does it: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
