/* Carries and owed areas of more rects than the queue entries for one carry hold (lorieRootCarryOnGpu,
 * with the real handover and repair code in rootharness.h) - on a 256 x 64 root, where they can be.
 *
 * A handover's carry is widened to its extents, less what is left behind: the slot going out holds the
 * same as the next one there, or newer. An owed area is widened too, but only over what the drawing slot
 * holds the same as its donor: never over what has been drawn into it since - that would put the older
 * content back on top. What widening cannot bring down goes as far as the entries take it, and the rest
 * on the next round. Every case ends with the slots matching what was drawn and no CPU copy at all. */
#define W 256
#define H 64
#include "rootharness.h"

typedef struct { uint64_t serial; int from, to, n, cancelled; BoxRec boxes[LORIE_GPU_COPY_MAX_RECTS]; } Job;
static Job jobs[4096];
static int jobHead, jobTail;
static uint64_t serial;
static uint64_t queueCarry(int from, int to, BoxPtr box, int n) {
    Job *j = &jobs[jobTail++];
    memset(j, 0, sizeof *j);
    j->serial = ++serial; j->from = from; j->to = to; j->n = n;
    memcpy(j->boxes, box, (size_t) n * sizeof *box);
    return j->serial;
}
static void renderAll(void) {
    while (jobHead < jobTail) {
        Job *j = &jobs[jobHead++];
        if (j->cancelled) { gpuFails(j->serial); continue; }
        for (int k = 0; k < j->n; k++)
            for (int y = j->boxes[k].y1; y < j->boxes[k].y2; y++)
                for (int x = j->boxes[k].x1; x < j->boxes[k].x2; x++)
                    pixels[j->to][y * W + x] = pixels[j->from][y * W + x];
        fakeCompleted = j->serial;
    }
}
static int cancelJob(uint64_t s) {
    for (int j = jobHead; j < jobTail; j++)
        if (jobs[j].serial == s)
            return jobs[j].cancelled ? 0 : (jobs[j].cancelled = 1);
    return 0;
}

static uint32_t ref[W * H];
static LoriePixmapPriv priv;
static void reset(void) {
    init(&priv);
    memset(ref, 0, sizeof ref);
    memset(&fakeState.presentStats, 0, sizeof fakeState.presentStats);
    jobHead = jobTail = 0; serial = 0;
    harnessCarryOn = 1; harnessQueueCarry = queueCarry; harnessCancel = cancelJob; harnessLockWait = NULL;
}
static void draw(BoxRec b, uint32_t v) {
    xDraw(&priv, b, v);
    for (int y = b.y1; y < b.y2; y++) for (int x = b.x1; x < b.x2; x++) ref[y * W + x] = v;
}
static void pixel(int x, int y, uint32_t v) { draw((BoxRec) { (short) x, (short) y, (short) (x + 1), (short) (y + 1) }, v); }
static int matches(int slot) { return !memcmp(pixels[slot], ref, sizeof ref); }
static uint64_t cpu(void) { return fakeState.presentStats.cpuCarryBytes + fakeState.presentStats.cpuOwedFetchBytes; }
/* rounds of repair on the GPU and the renderer running them, as EXA fallbacks begin, until nothing new */
static int repairRounds(void) {
    int rounds = 0;
    for (;;) {
        int before = jobTail;
        lorieRootRepairOnGpu(&priv);
        if (jobTail == before)
            return rounds;
        rounds++;
        renderAll();
    }
}

int main(void) {
    const int limit = LORIE_ROOT_CARRY_ENTRIES * LORIE_GPU_COPY_MAX_RECTS;

    /* a handover's carry of 2048 isolated pixels: widened to one rect, one copy */
    reset();
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 4)
            pixel(x, y, 1000 + y * W + x);
    int A = priv.rootWrite;
    CHECK(handover(&priv), "carry: no publish");
    int B = priv.rootWrite;
    CHECK(fakeState.presentStats.gpuCarryJobs == 1 && fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS] == 0,
          "carry: %u copies for %d rects (limit %d), kept %u", fakeState.presentStats.gpuCarryJobs, 2048, limit,
          fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS]);
    renderAll();
    CHECK(matches(A) && matches(B), "carry: the slots do not match what was drawn");
    CHECK(cpu() == 0, "carry: the CPU copied %llu bytes", (unsigned long long) cpu());

    /*
     * An owed area of 2048 rects that becomes fetchable only after the drawing slot has been drawn into:
     * left behind by a handover because the copy carrying it into the slot going out had not landed, the
     * X server draws in between, then that copy lands. Repaired on the GPU, widened around what was drawn
     * meanwhile - which it must not cover.
     */
    reset();
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 4)
            pixel(x, y, 1000 + y * W + x);
    CHECK(handover(&priv), "repair: first publish");      /* carried on the GPU into A, not landed */
    A = priv.rootWrite;
    CHECK(handover(&priv), "repair: A not published with its carry in flight");
    B = priv.rootWrite;                                  /* owes the pixels from A, once A has them */
    uint64_t before = fakeState.presentStats.gpuOwedRepairs;
    draw((BoxRec) { 10, 10, 50, 30 }, 7);                /* drawn into B meanwhile, over a part of them */
    draw((BoxRec) { 100, 0, 101, 64 }, 8);
    draw((BoxRec) { 200, 40, 256, 41 }, 9);
    CHECK(cpu() == 0, "repair: the CPU fetched before the area could be had");
    renderAll();                                         /* A's carry lands */
    int rounds = repairRounds();
    CHECK(rounds >= 1 && fakeState.presentStats.gpuOwedRepairs > before, "repair: not given to the GPU");
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS] == 0, "repair: not widened (%u kept)",
          fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS]);
    xDraw(&priv, (BoxRec) { 0, 0, 1, 1 }, ref[0]);       /* a PrepareAccess: anything left would be fetched */
    CHECK(matches(B), "repair: the drawing slot does not match - the widened copy covered what was drawn since?");
    CHECK(cpu() == 0, "repair: the CPU copied %llu bytes", (unsigned long long) cpu());
    CHECK(handover(&priv), "repair: B not published");
    renderAll();
    CHECK(matches(B), "repair: B went out wrong");

    /* the same with what is drawn meanwhile a checkerboard between the owed pixels: nothing to widen over;
     * the entries take it as far as they go each round, and the rounds take the rest */
    reset();
    for (int y = 0; y < H; y++)
        for (int x = (y & 1); x < W; x += 2)
            pixel(x, y, 2000 + y * W + x);
    CHECK(handover(&priv), "checkerboard: first publish");
    CHECK(handover(&priv), "checkerboard: second publish");
    B = priv.rootWrite;
    for (int y = 0; y < H; y++)
        for (int x = 1 - (y & 1); x < W; x += 2)
            pixel(x, y, 9);
    renderAll();
    rounds = repairRounds();
    CHECK(rounds >= (W * H / 2) / limit, "checkerboard: %d rounds for %d rects at %d a round", rounds, W * H / 2, limit);
    CHECK(fakeState.presentStats.cpuCarryKept[LORIE_CARRY_KEPT_RECTS] >= 1, "checkerboard: never more than fitted");
    xDraw(&priv, (BoxRec) { 0, 0, 1, 1 }, ref[0]);
    CHECK(matches(B), "checkerboard: the drawing slot does not match");
    CHECK(cpu() == 0, "checkerboard: the CPU copied %llu bytes", (unsigned long long) cpu());
    CHECK(handover(&priv), "checkerboard: B not published");
    renderAll();
    CHECK(matches(B), "checkerboard: B went out wrong");

    printf("carries of many rects (widened, in rounds): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
