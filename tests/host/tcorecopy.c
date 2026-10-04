/* Every copy X core rendering makes with the CPU is counted where it is made (lorieNoteCoreCopy, from the
 * EXA fallbacks in xserver.patch), into the one total Phase 1 has to take to 0 (lorieCountCpuCopy).
 * Extracted from InitOutput.c by gen.py; regions are real pixman.
 *
 * Checked: the bytes of what was written, at the drawable's depth; a move within one buffer told apart
 * from a copy between two, and an overlapping one from one that is not; what lands in the root; the
 * time and the longest; a copy made as part of a present, a resize or a flip's end counted as that and
 * not as a client's CopyArea; and the total split by whether a renderer was there to do it instead. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pixman.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef pixman_region16_t RegionRec, *RegionPtr;
typedef pixman_box16_t BoxRec, *BoxPtr;
static inline void RegionNull(RegionPtr r) { pixman_region_init(r); }
static inline void RegionInit(RegionPtr r, BoxPtr b, int n) { (void) n; pixman_region_init_rect(r, b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1); }
static inline void RegionUninit(RegionPtr r) { pixman_region_fini(r); }
static inline Bool RegionCopy(RegionPtr d, RegionPtr s) { return pixman_region_copy(d, s); }
static inline Bool RegionUnion(RegionPtr d, RegionPtr a, RegionPtr b) { return pixman_region_union(d, a, b); }
static inline Bool RegionIntersect(RegionPtr d, RegionPtr a, RegionPtr b) { return pixman_region_intersect(d, a, b); }
static inline void RegionTranslate(RegionPtr r, int x, int y) { pixman_region_translate(r, x, y); }
static inline Bool RegionNotEmpty(RegionPtr r) { return pixman_region_not_empty(r); }
static inline int RegionNumRects(RegionPtr r) { return pixman_region_n_rects(r); }
static inline BoxPtr RegionRects(RegionPtr r) { return pixman_region_rectangles(r, NULL); }
void _pixman_log_error(const char *f, const char *m) { (void) f; (void) m; }
int pixman_image_get_width(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_height(pixman_image_t *i) { (void) i; return 0; }
int pixman_image_get_stride(pixman_image_t *i) { (void) i; return 0; }
uint32_t *pixman_image_get_data(pixman_image_t *i) { (void) i; return NULL; }

typedef struct _Pixmap { int dummy; } *PixmapPtr;
static struct _Pixmap rootPix, clientPix, otherPix;
typedef struct FakeScreen *ScreenPtr;
struct FakeScreen { PixmapPtr (*GetScreenPixmap)(ScreenPtr); };
static PixmapPtr getScreenPixmap(ScreenPtr s) { (void) s; return &rootPix; }
static struct FakeScreen screen = { getScreenPixmap };
static ScreenPtr pScreenPtr = &screen;
#include "tcorecopy_types.inc"
static struct {
    struct { uint64_t cpuPresentBytes, cpuResizeBytes, cpuUnflipBytes, cpuTotalBytes, cpuDegradedBytes;
             uint32_t coreCopyCalls[LORIE_CORE_COPY_KINDS], coreCopyUs[LORIE_CORE_COPY_KINDS], coreCopyMaxUs[LORIE_CORE_COPY_KINDS];
             uint64_t coreCopyBytes[LORIE_CORE_COPY_KINDS], coreCopyRootBytes[LORIE_CORE_COPY_KINDS],
                      coreCopySameBytes[LORIE_CORE_COPY_KINDS], coreCopyOverlapBytes[LORIE_CORE_COPY_KINDS]; } presentStats;
} fakeState;
static struct { typeof(fakeState) *state; Bool gpuPresentDisabled; struct { Bool legacyDrawing; } root; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
static Bool connected = TRUE, surface = TRUE;
static bool lorieConnectionAlive(void) { return connected; }
static bool lorieRendererAvailable(void) { return surface; }
static uint64_t clock = 1000;
static uint64_t lorieNowUs(void) { return clock; }
#include "tcorecopy_funcs.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define S fakeState.presentStats

/* one copy, as the patch reports it: `boxes` written in the destination pixmap, the source moved by (sdx, sdy) */
static void copy(int kind, PixmapPtr src, PixmapPtr dst, BoxRec *boxes, int n, int sdx, int sdy, int bpp, uint32_t us) {
    RegionRec r, one;
    RegionNull(&r);
    for (int i = 0; i < n; i++) { RegionInit(&one, &boxes[i], 1); RegionUnion(&r, &r, &one); RegionUninit(&one); }
    uint64_t start = lorieCoreCopyBegin();
    clock += us;
    lorieNoteCoreCopy(kind, src, dst, &r, sdx, sdy, bpp, start);
    RegionUninit(&r);
}

int main(void) {
    /* a 100 x 50 window moved by (10, 5) in the root: the source is where it was, 10 left and 5 up */
    BoxRec moved = { 110, 55, 210, 105 };
    copy(LORIE_CORE_COPY_WINDOW, &rootPix, &rootPix, &moved, 1, -10, -5, 32, 250);
    CHECK(S.coreCopyCalls[LORIE_CORE_COPY_WINDOW] == 1 && S.coreCopyBytes[LORIE_CORE_COPY_WINDOW] == 100 * 50 * 4,
          "window move: %u calls, %llu bytes", S.coreCopyCalls[LORIE_CORE_COPY_WINDOW],
          (unsigned long long) S.coreCopyBytes[LORIE_CORE_COPY_WINDOW]);
    CHECK(S.coreCopyRootBytes[LORIE_CORE_COPY_WINDOW] == 20000 && S.coreCopySameBytes[LORIE_CORE_COPY_WINDOW] == 20000 &&
          S.coreCopyOverlapBytes[LORIE_CORE_COPY_WINDOW] == 20000, "window move: not counted as into the root, within "
          "one buffer, overlapping");
    CHECK(S.coreCopyUs[LORIE_CORE_COPY_WINDOW] == 250 && S.coreCopyMaxUs[LORIE_CORE_COPY_WINDOW] == 250, "window move: %u us",
          S.coreCopyUs[LORIE_CORE_COPY_WINDOW]);
    CHECK(S.cpuTotalBytes == 20000 && S.cpuDegradedBytes == 0, "window move: total %llu",
          (unsigned long long) S.cpuTotalBytes);

    /* a client's pixmap copied into the root in two rects: between buffers, no overlap */
    BoxRec two[2] = { { 0, 0, 10, 10 }, { 20, 0, 40, 5 } };              /* 100 + 100 pixels */
    copy(LORIE_CORE_COPY_AREA, &clientPix, &rootPix, two, 2, 0, 0, 32, 40);
    CHECK(S.coreCopyBytes[LORIE_CORE_COPY_AREA] == 800 && S.coreCopyRootBytes[LORIE_CORE_COPY_AREA] == 800,
          "pixmap to root: %llu bytes", (unsigned long long) S.coreCopyBytes[LORIE_CORE_COPY_AREA]);
    CHECK(S.coreCopySameBytes[LORIE_CORE_COPY_AREA] == 0 && S.coreCopyOverlapBytes[LORIE_CORE_COPY_AREA] == 0,
          "pixmap to root: counted as within one buffer");

    /* within a pixmap that is not the root, moved further than its own size: one buffer, no overlap */
    BoxRec far = { 100, 0, 120, 10 };
    copy(LORIE_CORE_COPY_AREA, &otherPix, &otherPix, &far, 1, -100, 0, 32, 10);
    CHECK(S.coreCopySameBytes[LORIE_CORE_COPY_AREA] == 800 && S.coreCopyOverlapBytes[LORIE_CORE_COPY_AREA] == 0 &&
          S.coreCopyRootBytes[LORIE_CORE_COPY_AREA] == 800, "scroll far within a pixmap: same %llu, overlap %llu",
          (unsigned long long) S.coreCopySameBytes[LORIE_CORE_COPY_AREA],
          (unsigned long long) S.coreCopyOverlapBytes[LORIE_CORE_COPY_AREA]);
    CHECK(S.coreCopyMaxUs[LORIE_CORE_COPY_AREA] == 40 && S.coreCopyUs[LORIE_CORE_COPY_AREA] == 50, "longest %u of %u us",
          S.coreCopyMaxUs[LORIE_CORE_COPY_AREA], S.coreCopyUs[LORIE_CORE_COPY_AREA]);

    /* a bitmap: bytes at its depth */
    BoxRec bits = { 0, 0, 64, 8 };
    uint64_t before = S.coreCopyBytes[LORIE_CORE_COPY_AREA];
    copy(LORIE_CORE_COPY_AREA, &otherPix, &clientPix, &bits, 1, 0, 0, 1, 0);
    CHECK(S.coreCopyBytes[LORIE_CORE_COPY_AREA] - before == 64, "1 bpp 64 x 8: %llu bytes",
          (unsigned long long) (S.coreCopyBytes[LORIE_CORE_COPY_AREA] - before));

    /* made as part of something else: counted as that, once, and not as a client's CopyArea */
    uint32_t calls = S.coreCopyCalls[LORIE_CORE_COPY_AREA];
    uint64_t area = S.coreCopyBytes[LORIE_CORE_COPY_AREA];
    BoxRec full = { 0, 0, 100, 100 };
    lorieCopyContext(LORIE_COPY_CTX_PRESENT);
    copy(LORIE_CORE_COPY_AREA, &clientPix, &rootPix, &full, 1, 0, 0, 32, 5);
    lorieCopyContext(LORIE_COPY_CTX_RESIZE);
    copy(LORIE_CORE_COPY_AREA, &otherPix, &rootPix, &full, 1, 0, 0, 32, 5);
    lorieCopyContext(LORIE_COPY_CTX_UNFLIP);
    copy(LORIE_CORE_COPY_AREA, &clientPix, &rootPix, &full, 1, 0, 0, 32, 5);
    lorieCopyContext(LORIE_COPY_CTX_NONE);
    CHECK(S.cpuPresentBytes == 40000 && S.cpuResizeBytes == 40000 && S.cpuUnflipBytes == 40000,
          "contexts: present %llu, resize %llu, flip end %llu", (unsigned long long) S.cpuPresentBytes,
          (unsigned long long) S.cpuResizeBytes, (unsigned long long) S.cpuUnflipBytes);
    CHECK(S.coreCopyCalls[LORIE_CORE_COPY_AREA] == calls && S.coreCopyBytes[LORIE_CORE_COPY_AREA] == area,
          "contexts: counted as a client's CopyArea as well");
    copy(LORIE_CORE_COPY_AREA, &clientPix, &rootPix, &full, 1, 0, 0, 32, 5);
    CHECK(S.coreCopyBytes[LORIE_CORE_COPY_AREA] == area + 40000, "context left set after it ended");

    /* the total is every site once, with a renderer there */
    uint64_t sum = S.coreCopyBytes[0] + S.coreCopyBytes[1] + S.cpuPresentBytes + S.cpuResizeBytes + S.cpuUnflipBytes;
    CHECK(S.cpuTotalBytes == sum, "total %llu, the sites add up to %llu", (unsigned long long) S.cpuTotalBytes,
          (unsigned long long) sum);

    /* with no renderer, or the GPU path off, nothing else could have copied it: kept apart */
    uint64_t total = S.cpuTotalBytes;
    connected = FALSE;
    copy(LORIE_CORE_COPY_WINDOW, &rootPix, &rootPix, &moved, 1, -10, -5, 32, 1);
    connected = TRUE; surface = FALSE;
    copy(LORIE_CORE_COPY_WINDOW, &rootPix, &rootPix, &moved, 1, -10, -5, 32, 1);
    surface = TRUE; fakePvfb.gpuPresentDisabled = TRUE;
    copy(LORIE_CORE_COPY_WINDOW, &rootPix, &rootPix, &moved, 1, -10, -5, 32, 1);
    fakePvfb.gpuPresentDisabled = FALSE;
    CHECK(S.cpuTotalBytes == total && S.cpuDegradedBytes == 3 * 20000, "degraded: total moved by %llu, degraded %llu",
          (unsigned long long) (S.cpuTotalBytes - total), (unsigned long long) S.cpuDegradedBytes);

    printf("core copy accounting: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
