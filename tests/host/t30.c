/* T30: copies over the same root area failing for far longer than there are slots - every slot reused
 * many times over - with unrelated CPU drawing in between, failures over part of the area, and then
 * successes again. Against the real handover, repair and replacement bookkeeping (rootharness.h).
 *
 * The reference is what the screen should hold: each made copy and each CPU draw, in order. Every slot
 * the X server publishes is compared with it once the renderer has run the copies queued into it.
 *
 * One thing is allowed, once per run of failures: the first slot to go out while a copy into it was
 * still pending, that copy then failing. The slot went out before anything was known and shows its own
 * earlier pixels in that copy's area for that frame. Everything published after it, and everything
 * outside that area, must match - which is what losing the content's last copy, or giving up on it,
 * would break. */
#include "rootharness.h"

static uint32_t ref[W * H];
static void refFill(BoxRec b, uint32_t v) {
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) ref[y * W + x] = v;
}
static int inBox(BoxRec b, int x, int y) { return x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2; }

typedef struct { uint64_t serial; int slot; BoxRec b; uint32_t v; int cancelled; } Job;
static Job queued[64];
static int nQueued;
static uint64_t serial;
static uint32_t value = 100;
static int publishes, attempts, staleFrames, badFrames, runStale, inRun;

/* the renderer: runs everything queued, in order, making or failing each as told */
static void renderAll(int fail) {
    for (int i = 0; i < nQueued; i++) {
        if (fail || queued[i].cancelled) gpuFails(queued[i].serial);
        else { gpuLands(queued[i].slot, queued[i].b, queued[i].v, queued[i].serial); refFill(queued[i].b, queued[i].v); }
    }
    nQueued = 0;
}

/* one frame: a client presents over `b` (made or failing), maybe some CPU drawing elsewhere first, then
 * the block handler hands over and the renderer runs the queue before it shows what was published */
static void frame(LoriePixmapPriv *priv, BoxRec b, int fail, int cpu, BoxRec q) {
    if (cpu) { xDraw(priv, q, ++value); refFill(q, value); }
    if (enqueueRootCopy(priv, b, ++serial))
        queued[nQueued++] = (Job) { serial, priv->rootWrite, b, ++value, 0 };
    else { xDraw(priv, b, ++value); refFill(b, value); }    /* refused: drawn by the CPU */
    int drawn = priv->rootWrite;
    BoxRec pendingFailed = { 0, 0, 0, 0 };
    for (int i = 0; i < nQueued; i++)
        if (fail && queued[i].slot == drawn) pendingFailed = queued[i].b;
    attempts++;
    Bool ok = handover(priv);
    renderAll(fail);
    if (!ok)
        return;
    publishes++;
    int bad = 0, exemptOnly = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (pixels[drawn][y * W + x] != ref[y * W + x]) {
                bad++;
                if (!inBox(pendingFailed, x, y)) exemptOnly = 0;
            }
    if (!bad)
        return;
    if (exemptOnly && fail && !runStale) { runStale = 1; staleFrames++; return; }
    badFrames++;
}

/*
 * The same with the renderer behind: it reports each copy `lag` frames after it was queued, so the X
 * server keeps publishing slots before it knows whether their copies were made, and the chain of slots
 * that may hold the content gets as long as the lag. Each slot is checked once every copy queued before
 * it went out has resolved, against a replay of everything issued up to then, presents only where made.
 * Exempt per slot is only the area of copies into it still in flight when it went out that then failed
 * - nothing the X server can know at that point. No other slot may show old content, and nothing may
 * be lost track of.
 */
#define EV 2048
static struct { int cpu; BoxRec b; uint32_t v; uint64_t s; } ev[EV];
static int nev;
static uint8_t made[EV];          /* by serial: 1 made, 2 failed */
static struct { int slot, nev; uint64_t maxSerial; int nIn; uint64_t in[16]; } pend[64];
static int nPend, lagChecked, lagBad, lagPublished;
static Job lagged[256];
static int nLagged;
static uint64_t frameNo;
static void lagCheck(void) {
    for (int i = 0; i < nPend; i++) {
        if (pend[i].maxSerial > fakeCompleted)
            continue;
        uint32_t r[W * H];
        memset(r, 0, sizeof r);
        for (int e = 0; e < pend[i].nev; e++)
            if (ev[e].cpu || made[ev[e].s] == 1)
                for (int y = ev[e].b.y1; y < ev[e].b.y2; y++) for (int x = ev[e].b.x1; x < ev[e].b.x2; x++) r[y * W + x] = ev[e].v;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (pixels[pend[i].slot][y * W + x] == r[y * W + x])
                    continue;
                int exempt = 0;
                for (int k = 0; k < pend[i].nIn && !exempt; k++)
                    for (int e = 0; e < pend[i].nev; e++)
                        if (!ev[e].cpu && ev[e].s == pend[i].in[k] && made[ev[e].s] == 2 && inBox(ev[e].b, x, y))
                            exempt = 1;
                if (!exempt) { lagBad++; goto next; }
            }
        next:
        lagChecked++;
        pend[i--] = pend[--nPend];
    }
}
static void lagFrame(LoriePixmapPriv *priv, BoxRec b, int fail, int lag, int cpu, BoxRec q) {
    frameNo++;
    if (cpu) { xDraw(priv, q, ++value); ev[nev++] = (typeof(ev[0])) { 1, q, value, 0 }; }
    if (enqueueRootCopy(priv, b, ++serial)) {
        lagged[nLagged++] = (Job) { serial, priv->rootWrite, b, ++value, 0 };
        ev[nev++] = (typeof(ev[0])) { 0, b, value, serial };
        made[serial] = fail ? 2 : 1;          /* decided now, reported later */
        lagged[nLagged - 1].v = value;
    } else { xDraw(priv, b, ++value); ev[nev++] = (typeof(ev[0])) { 1, b, value, 0 }; }
    /* the renderer reports what was queued `lag` frames ago and before */
    while (nLagged && lagged[0].serial + (uint64_t) lag < serial + 1) {
        Job j = lagged[0];
        if (made[j.serial] == 2 || j.cancelled) { made[j.serial] = 2; gpuFails(j.serial); }
        else gpuLands(j.slot, j.b, j.v, j.serial);
        memmove(lagged, lagged + 1, --nLagged * sizeof *lagged);
    }
    lagCheck();                              /* checked before any slot that held them is drawn into again */
    int drawn = priv->rootWrite;
    attempts++;
    if (handover(priv)) {
        publishes++;
        lagPublished++;
        if (nPend < 64) {
            pend[nPend].slot = drawn; pend[nPend].nev = nev; pend[nPend].maxSerial = serial; pend[nPend].nIn = 0;
            for (int i = 0; i < nLagged && pend[nPend].nIn < 16; i++)
                if (lagged[i].slot == drawn) pend[nPend].in[pend[nPend].nIn++] = lagged[i].serial;
            nPend++;
        }
    }
}

/* the X server taking back a copy still waiting in the queue (lorieMarkQueuedCopySuperseded) */
static int cancelQueued(uint64_t s) {
    for (int i = 0; i < nQueued; i++)
        if (queued[i].serial == s && !queued[i].cancelled) return queued[i].cancelled = 1;
    for (int i = 0; i < nLagged; i++)
        if (lagged[i].serial == s && !lagged[i].cancelled) return lagged[i].cancelled = 1;
    return 0;
}

int main(void) {
    LoriePixmapPriv priv;
    harnessCancel = cancelQueued;
    BoxRec R = { 8, 2, 24, 10 }, R1 = { 8, 2, 16, 10 }, Q = { 40, 2, 56, 10 }, none = { 0, 0, 0, 0 };
    int runs = 0;

    init(&priv);
    memset(ref, 0, sizeof ref);
    for (int i = 0; i < 3; i++) frame(&priv, R, 0, 0, none);           /* the area holds a made frame */

    for (int pass = 0; pass < 2; pass++) {
        runs++; runStale = 0; inRun = 1;
        for (int k = 0; k < 4 * LORIE_ROOT_SLOTS; k++)                     /* far longer than the slots last */
            frame(&priv, (k % 3 == 2) ? R1 : R, 1, k % 2, Q);
        inRun = 0;
        frame(&priv, pass ? R1 : R, 0, 0, none);                           /* a success again */
        for (int i = 0; i < 4; i++) frame(&priv, R, 0, i % 2, Q);
    }

    CHECK(badFrames == 0, "%d published slots differed from the reference beyond the one allowed per run", badFrames);
    CHECK(staleFrames <= runs, "%d runs, %d stale first frames", runs, staleFrames);
    CHECK(fakeState.presentStats.rootOwedLost == 0, "%u areas lost track of", fakeState.presentStats.rootOwedLost);
    CHECK(publishes >= attempts - 2 * runs, "%d of %d frames published - held back for good?", publishes, attempts);
    /* and once nothing is pending, the next slot goes out and matches exactly */
    renderAll(0);
    int last = priv.rootWrite;
    CHECK(handover(&priv), "no publish once everything had resolved");
    CHECK(memcmp(pixels[last], ref, sizeof ref) == 0, "the slot published at the end differs from the reference");

    /* the renderer behind by 2, then 4 frames: failures far longer than the slots last, CPU drawing
     * elsewhere, part of the area failing too, then successes */
    for (int lag = 2; lag <= 4; lag += 2) {
        init(&priv);
        serial = 0; nev = 0; nLagged = 0; nPend = 0; memset(made, 0, sizeof made);
        fakeState.presentStats.rootOwedLost = 0;
        int a0 = attempts, p0 = publishes, a1, p1, a2, p2;
        for (int i = 0; i < 3; i++) lagFrame(&priv, R, 0, lag, 0, none);
        a1 = attempts; p1 = publishes;
        for (int k = 0; k < 6 * LORIE_ROOT_SLOTS; k++)
            lagFrame(&priv, (k % 3 == 2) ? R1 : R, 1, lag, k % 2, Q);
        printf("T30 renderer %d frames behind: %d of %d frames published while failing, ", lag,
               publishes - p1, attempts - a1);
        a2 = attempts; p2 = publishes;
        for (int k = 0; k < 3 * LORIE_ROOT_SLOTS; k++)
            lagFrame(&priv, (k % 4 == 3) ? R1 : R, 0, lag, k % 3 == 0, Q);
        printf("%d of %d once made again\n", publishes - p2, attempts - a2);
        CHECK(publishes - p2 >= attempts - a2 - lag - 2, "lag %d: publishing did not come back once copies were made", lag);
        for (int k = 0; k < lag + 2; k++) lagFrame(&priv, R, 0, lag, 0, none);   /* let it all resolve */
        while (nLagged) {                       /* and the renderer catch up with the rest */
            Job j = lagged[0];
            if (made[j.serial] == 2 || j.cancelled) { made[j.serial] = 2; gpuFails(j.serial); }
            else gpuLands(j.slot, j.b, j.v, j.serial);
            memmove(lagged, lagged + 1, --nLagged * sizeof *lagged);
        }
        lagCheck();
        CHECK(fakeState.presentStats.rootOwedLost == 0, "lag %d: %u areas lost track of", lag,
              fakeState.presentStats.rootOwedLost);
        CHECK(publishes - p0 >= (attempts - a0) / 2, "lag %d: %d of %d frames published", lag, publishes - p0, attempts - a0);
    }
    CHECK(lagBad == 0, "renderer behind: %d of %d published slots showed old content beyond their own failed copies",
          lagBad, lagChecked);
    CHECK(lagChecked == lagPublished, "renderer behind: %d of %d published slots checked", lagChecked, lagPublished);

    printf("T30 root owed area, donor exhaustion: %s (%d failures; in step: %d frames, %d published, %d first-frame stale, "
           "%d bad; renderer behind: %d slots checked, %d bad)\n",
           fails ? "FAIL" : "PASS", fails, attempts, publishes, staleFrames, badFrames, lagChecked, lagBad);
    return fails != 0;
}
