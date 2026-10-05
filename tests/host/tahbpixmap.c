/* Clients' pixmaps given AHardwareBuffers from the start (lorieCreatePixmap, lorieAhbPixmapWanted,
 * extracted from InitOutput.c by gen.py): the ones the GPU copies - depth 24, back-buffer sized, not
 * scratch or glyph pixmaps - so their copies to and from the root are the GPU's and none is moved into
 * such a buffer by the CPU later. What has to hold: exactly those get one; the root keeps its own path;
 * an allocation that fails falls back to plain memory; the pitch is the buffer's; turned off, or under
 * legacy drawing, nothing changes. Run again by itself with the environment set, for those. */
#include <unistd.h>
#include <sys/wait.h>
#include "rootharness.h"
#define __unused __attribute__((unused))
#define CREATE_PIXMAP_USAGE_SCRATCH 1
#define CREATE_PIXMAP_USAGE_BACKING_PIXMAP 2
#define CREATE_PIXMAP_USAGE_GLYPH_PICTURE 3
#define CREATE_PIXMAP_USAGE_SHARED 4
static int failAhb, allocations;
static LorieBuffer made[64];
static LorieBuffer *LorieBuffer_allocate(int w, int h, int format, int type) {
    if (type == LORIEBUFFER_AHARDWAREBUFFER && failAhb)
        return NULL;
    LorieBuffer *b = &made[allocations++ % 64];
    /* AHardwareBuffers come back with a stride of the allocator's choosing */
    b->desc = (LorieBuffer_Desc) { w, type == LORIEBUFFER_AHARDWAREBUFFER ? (w + 63) / 64 * 64 : w, h, 0, format, type };
    return b;
}
static LorieBuffer rootBuffer;
static LorieBuffer *lorieAllocateRootBuffer(int w, int h, bool *granted) {
    *granted = false;
    rootBuffer.desc = (LorieBuffer_Desc) { w, w, h, 0, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, LORIEBUFFER_AHARDWAREBUFFER };
    return &rootBuffer;
}
static int LorieBuffer_lock(LorieBuffer *b, void **out) { static uint32_t mem; b->locked = 1; *out = &mem; return 0; }
#include "ahbpixmap_src.inc"

static int typeOf(int w, int h, int depth, int usage, int *pitch) {
    LoriePixmapPriv *p = lorieCreatePixmap(NULL, w, h, depth, usage, 32, pitch);
    int t = p && p->buffer ? LorieBuffer_description(p->buffer)->type : -1;
    if (p && p->buffer)
        CHECK(p->locked && *pitch == LorieBuffer_description(p->buffer)->stride * 4,
              "%dx%d: not mapped, or the pitch is not the buffer's", w, h);
    free(p);
    return t;
}

int main(int argc, char **argv) {
    int pitch;
    const char *mode = argc > 1 ? argv[1] : "";

    if (!strcmp(mode, "off")) {
        CHECK(typeOf(800, 600, 24, 0, &pitch) == LORIEBUFFER_REGULAR, "turned off: an AHardwareBuffer all the same");
        return fails != 0;
    }
    if (!strcmp(mode, "min")) {
        CHECK(typeOf(20, 20, 24, 0, &pitch) == LORIEBUFFER_AHARDWAREBUFFER &&
              typeOf(5, 5, 24, 0, &pitch) == LORIEBUFFER_REGULAR, "the minimum not taken from the environment");
        return fails != 0;
    }

    CHECK(typeOf(800, 600, 24, 0, &pitch) == LORIEBUFFER_AHARDWAREBUFFER, "a window-sized pixmap: plain memory");
    CHECK(pitch == (800 + 63) / 64 * 64 * 4, "the pitch is not the AHardwareBuffer's stride (%d)", pitch);
    CHECK(typeOf(640, 480, 24, CREATE_PIXMAP_USAGE_BACKING_PIXMAP, &pitch) == LORIEBUFFER_AHARDWAREBUFFER,
          "a redirected window's pixmap: plain memory");
    CHECK(typeOf(128, 128, 24, 0, &pitch) == LORIEBUFFER_AHARDWAREBUFFER &&
          typeOf(127, 128, 24, 0, &pitch) == LORIEBUFFER_REGULAR, "the size it starts at is not 128 x 128");
    CHECK(typeOf(48, 48, 24, 0, &pitch) == LORIEBUFFER_REGULAR, "an icon: an AHardwareBuffer");
    CHECK(typeOf(800, 600, 32, 0, &pitch) == LORIEBUFFER_REGULAR && typeOf(800, 600, 16, 0, &pitch) == LORIEBUFFER_REGULAR,
          "depth 32 or 16: an AHardwareBuffer the GPU's copy would not do");
    CHECK(typeOf(800, 600, 24, CREATE_PIXMAP_USAGE_SCRATCH, &pitch) == LORIEBUFFER_REGULAR &&
          typeOf(800, 600, 24, CREATE_PIXMAP_USAGE_GLYPH_PICTURE, &pitch) == LORIEBUFFER_REGULAR,
          "a scratch or glyph pixmap: an AHardwareBuffer");
    uint32_t before = fakeState.presentStats.ahbPixmaps;
    CHECK(typeOf(800, 600, 24, 5 /* CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED */, &pitch) == LORIEBUFFER_AHARDWAREBUFFER &&
          fakeState.presentStats.ahbPixmaps == before && rootBuffer.desc.format == AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
          "the root: not its own buffer");
    failAhb = 1;
    CHECK(typeOf(800, 600, 24, 0, &pitch) == LORIEBUFFER_REGULAR && fakeState.presentStats.ahbPixmapFailures == 1,
          "an AHardwareBuffer that could not be had: no pixmap, or not counted");
    failAhb = 0;
    fakePvfb.root.legacyDrawing = TRUE;
    CHECK(typeOf(800, 600, 24, 0, &pitch) == LORIEBUFFER_REGULAR, "legacy drawing: an AHardwareBuffer");
    fakePvfb.root.legacyDrawing = FALSE;
    {
        LoriePixmapPriv *empty = lorieCreatePixmap(NULL, 0, 0, 24, 0, 32, &pitch);
        CHECK(empty && !empty->buffer && pitch == 0, "a 0 x 0 pixmap: given a buffer");
        free(empty);
    }

    /* the environment, read once: run again with it set */
    for (int k = 0; k < 2; k++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (k == 0) setenv("TERMUX_X11_AHB_PIXMAPS", "0", 1);
            else setenv("TERMUX_X11_AHB_PIXMAP_MIN", "100", 1);
            execl("/proc/self/exe", argv[0], k == 0 ? "off" : "min", (char *) NULL);
            _exit(2);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "with %s set: failed", k == 0 ? "TERMUX_X11_AHB_PIXMAPS=0" :
              "TERMUX_X11_AHB_PIXMAP_MIN=100");
    }

    printf("pixmaps in AHardwareBuffers from the start: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
