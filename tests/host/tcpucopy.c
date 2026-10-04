/* Presents the GPU path turned down are counted by why, with the bytes the CPU then copies
 * (lorieCpuPresent and LORIE_CPU_PRESENT_*, extracted by gen.py). */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int Bool;
#define FALSE 0
typedef struct { short x1, y1, x2, y2; } BoxRec, *BoxPtr;
typedef struct { int n; BoxRec *rects; } RegionRec, *RegionPtr;
static int RegionNumRects(RegionPtr r) { return r->n; }
static BoxPtr RegionRects(RegionPtr r) { return r->rects; }
typedef struct { struct { int width, height; } drawable; } PixmapRec, *PixmapPtr;
#include "tcpucopy_types.inc"
static struct { struct { uint64_t cpuPresentBytes; uint32_t cpuPresents[LORIE_CPU_PRESENT_REASONS]; } presentStats; } fakeState;
static struct { typeof(fakeState) *state; } fakePvfb = { &fakeState };
#define pvfb (&fakePvfb)
static int gpuCopyAttempts;
#include "tcpucopy_funcs.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    PixmapRec pix = { { 100, 50 } };
    BoxRec two[2] = { { 0, 0, 10, 10 }, { 20, 5, 30, 25 } };   /* 100 + 200 pixels */
    RegionRec update = { 2, two };

    CHECK(!lorieCpuPresent(LORIE_CPU_PRESENT_QUEUE_FULL, &pix, &update), "said the GPU took it");
    CHECK(fakeState.presentStats.cpuPresents[LORIE_CPU_PRESENT_QUEUE_FULL] == 1, "not counted under its reason");
    CHECK(fakeState.presentStats.cpuPresentBytes == 300 * 4, "%llu bytes for 300 pixels",
          (unsigned long long) fakeState.presentStats.cpuPresentBytes);
    lorieCpuPresent(LORIE_CPU_PRESENT_NO_RENDERER, &pix, NULL);   /* no update region: the whole pixmap */
    CHECK(fakeState.presentStats.cpuPresents[LORIE_CPU_PRESENT_NO_RENDERER] == 1, "not counted under its reason");
    CHECK(fakeState.presentStats.cpuPresentBytes == (300 + 5000) * 4, "%llu bytes after the whole 100 x 50 pixmap",
          (unsigned long long) fakeState.presentStats.cpuPresentBytes);
    CHECK(gpuCopyAttempts == 2, "%d attempts counted, 2 made", gpuCopyAttempts);
    uint32_t others = 0;
    for (int why = 0; why < LORIE_CPU_PRESENT_REASONS; why++)
        if (why != LORIE_CPU_PRESENT_QUEUE_FULL && why != LORIE_CPU_PRESENT_NO_RENDERER)
            others += fakeState.presentStats.cpuPresents[why];
    CHECK(others == 0, "%u counted under reasons that did not happen", others);

    printf("CPU present accounting: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
