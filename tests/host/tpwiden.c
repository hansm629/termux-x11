/* A present's region with more rects than one GPU copy takes, made fewer (lorieWidenPresentRegion)
 * instead of left to the CPU: its bounding box, kept to the pixmap's valid content and to the window's
 * clip. What has to hold: the result covers everything the update did, nothing outside the clip or the
 * valid area, and fits one copy - or the region is left exactly as it was. */
#include "rootharness.h"
#include "pwiden_src.inc"

static uint32_t rng = 1;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }
static void add(RegionPtr r, int x1, int y1, int x2, int y2) {
    BoxRec b = { (short) x1, (short) y1, (short) x2, (short) y2 };
    RegionRec one;
    RegionInit(&one, &b, 1);
    RegionUnion(r, r, &one);
    RegionUninit(&one);
}
static int within(RegionPtr a, RegionPtr b) {   /* a is inside b */
    RegionRec d;
    RegionNull(&d);
    RegionSubtract(&d, a, b);
    int ok = !RegionNotEmpty(&d);
    RegionUninit(&d);
    return ok;
}
static int same(RegionPtr a, RegionPtr b) { return within(a, b) && within(b, a); }

int main(void) {
    RegionRec r, clip, valid, before;

    /* few rects: left alone */
    RegionNull(&r); RegionNull(&clip);
    add(&r, 0, 0, 10, 10); add(&r, 20, 20, 30, 30);
    add(&clip, 0, 0, 200, 200);
    RegionNull(&before); RegionCopy(&before, &r);
    CHECK(lorieWidenPresentRegion(&r, NULL, &clip) && same(&r, &before) && fakeState.presentStats.presentsWidened == 0,
          "two rects: changed");

    /* 100 isolated pixels in an unobscured window: one rect, their bounding box */
    RegionEmpty(&r);
    for (int k = 0; k < 100; k++)
        add(&r, 5 + k % 10 * 3, 7 + k / 10 * 3, 6 + k % 10 * 3, 8 + k / 10 * 3);
    RegionCopy(&before, &r);
    CHECK(lorieWidenPresentRegion(&r, NULL, &clip) && RegionNumRects(&r) == 1 && within(&before, &r) &&
          RegionExtents(&r)->x1 == 5 && RegionExtents(&r)->y2 == 8 + 27 && fakeState.presentStats.presentsWidened == 1,
          "100 pixels: not their bounding box");

    /* ... with only part of the pixmap valid, and a window over part of this one: kept off both */
    RegionCopy(&r, &before);
    RegionNull(&valid);
    add(&valid, 0, 0, 200, 30); add(&valid, 0, 30, 12, 200);   /* an L: its bounding box is not */
    for (int k = 0; k < 40; k++)
        add(&before, 2 + k % 4 * 2, 40 + k / 4 * 3, 3 + k % 4 * 2, 41 + k / 4 * 3);   /* down the L's foot */
    RegionCopy(&r, &before);
    RegionEmpty(&clip);
    add(&clip, 0, 0, 25, 200); add(&clip, 25, 20, 200, 200);    /* a window over [25,0 200,20] */
    RegionIntersect(&r, &r, &clip);
    RegionIntersect(&r, &r, &valid);
    RegionCopy(&before, &r);
    CHECK(lorieWidenPresentRegion(&r, &valid, &clip) && within(&before, &r) && within(&r, &clip) && within(&r, &valid) &&
          RegionNumRects(&r) <= LORIE_GPU_COPY_MAX_RECTS, "valid and clip: not kept to both");

    /* a clip in pieces finer than one copy takes: left as it was */
    RegionEmpty(&clip);
    for (int y = 0; y < 40; y++)
        for (int x = y & 1; x < 40; x += 2)
            add(&clip, x, y, x + 1, y + 1);
    RegionEmpty(&r);
    add(&r, 0, 0, 40, 40);
    RegionIntersect(&r, &r, &clip);
    RegionCopy(&before, &r);
    uint32_t widened = fakeState.presentStats.presentsWidened;
    CHECK(!lorieWidenPresentRegion(&r, NULL, &clip) && same(&r, &before) && fakeState.presentStats.presentsWidened == widened,
          "checkerboard clip: changed, or said it fits");

    /* random updates, clips and valid areas */
    int fitted = 0, left = 0;
    for (int t = 0; t < 3000; t++) {
        RegionEmpty(&r); RegionEmpty(&clip); RegionEmpty(&valid);
        /* a window under a few others, or under many small ones */
        int nu = 1 + rnd(150), pieces = rnd(4) == 0, nc = pieces ? 80 + rnd(80) : 1 + rnd(6);
        if (pieces && rnd(2))
            add(&r, 0, 0, 128, 128);                      /* the whole window updated */
        else
            for (int k = 0; k < nu; k++) { int x = rnd(120), y = rnd(120); add(&r, x, y, x + 1 + rnd(4), y + 1 + rnd(4)); }
        for (int k = 0; k < nc; k++) {
            int x = rnd(100), y = rnd(100), s = pieces ? 2 + rnd(3) : 10 + rnd(60);
            add(&clip, x, y, x + s, y + s);
        }
        int useValid = rnd(2);
        if (useValid) {
            add(&valid, 0, 0, 128, 40 + rnd(40));
            add(&valid, 0, 0, 20 + rnd(40), 128);          /* an L, so its bounding box is more than it */
            RegionIntersect(&r, &r, &valid);
        }
        RegionIntersect(&r, &r, &clip);
        RegionCopy(&before, &r);
        Bool fits = lorieWidenPresentRegion(&r, useValid ? &valid : NULL, &clip);
        if (fits) {
            fitted++;
            CHECK(within(&before, &r) && within(&r, &clip) && (!useValid || within(&r, &valid)) &&
                  RegionNumRects(&r) <= LORIE_GPU_COPY_MAX_RECTS, "random %d: widened past the clip or the valid area, "
                  "lost part of the update, or still too many rects", t);
        } else {
            left++;
            CHECK(same(&r, &before) && RegionNumRects(&r) > LORIE_GPU_COPY_MAX_RECTS, "random %d: turned down but changed", t);
        }
        if (fails > 3) break;
    }
    printf("present regions widened: %d fit one copy, %d left to the CPU\n", fitted, left);
    CHECK(left > 0 && fitted > 1000, "random: did not exercise both outcomes");
    printf("present regions widened to one copy: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
