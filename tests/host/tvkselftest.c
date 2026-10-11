/* TVKSELFTEST: the ownership self-test (vk.c lorieVkSelftest, gen.py "vk") run for real on fakevk.h's small
 * device, which moves a buffer's contents between the CPU and the device only through acquires from and releases
 * to FOREIGN in GENERAL, and flags the CPU touching a buffer the renderer owns or before a fence was waited for.
 *
 *   Passing: RGBA, RGBX, BGRA, LORIE_VK_SELFTEST_ITERATIONS each way, every compare counted (3 a format and
 *   iteration), no violation, no acquire from UNDEFINED, every download made visible to the host, nothing left
 *   behind; the pattern changes every iteration (every CPU write changes the buffer) and the CPU always writes
 *   something other than it read.
 *   BGRA not allocatable, or of an external format: unavailable, not passed. RGBX not allocatable: failed.
 *   Afterwards only the formats that passed may be imported. The quick run: every pattern kind once each way.
 *   One wrong byte - in a copy either way, RGBA's alpha byte included - fails it at that iteration, with the
 *   format, direction, byte, expected and actual value, the compares before it counted and no more; RGBX's
 *   X byte, undefined to Vulkan after an external write, is not compared and fails nothing. A failed
 *   lock, submission or fence wait fails it at its step. Every failure records one fallback (FATAL_BACKEND,
 *   VK_SELFTEST) and leaves nothing behind. Non-coherent memory is flushed and invalidated.
 *   Its duration (vkSelftestUs, lorieVkElapsedUs): microseconds across a second boundary, where tv_nsec is
 *   smaller at the end, as within one; never a wrapped value. */
#include "fakevk.h"

#define N LORIE_VK_SELFTEST_ITERATIONS
#define PIXELS (LORIE_VK_SELFTEST_WIDTH * LORIE_VK_SELFTEST_HEIGHT * 4)

static struct lorie_shared_server_state st;
static struct lorie_vk vk;

static void device(void) {
    if (vk.device)
        lorieVkDestroyDevice(&vk);
    memset(&st, 0, sizeof st);
    good();
    CHECK(lorieVkCreateDevice(&vk, fGetInstanceProcAddr, &st), "no device to test on");
    memset(&imp, 0, sizeof imp);
}

static void clean(const char *what) {
    CHECK(emu.ahbLive == 0 && emu.pools == 0 && emu.fences == 0 && emu.buffers == 0 && emu.mapped == 0 &&
          imp.images == 0 && imp.memories == 0 && imp.views == 0,
          "%s: left %d buffers, %d pools, %d fences, %d buffers, %d mappings, %d images, %d memories, %d views", what,
          emu.ahbLive, emu.pools, emu.fences, emu.buffers, emu.mapped, imp.images, imp.memories, imp.views);
}

static void passed(const char *what, bool ok, uint32_t iterations, uint8_t formats, uint8_t unavailable) {
    uint32_t n = 0;
    for (int b = 0; b < 3; b++)
        n += (formats >> b) & 1;
    CHECK(ok && st.diag.vkSelftest == 1, "%s: failed (step %u format %u direction %u iteration %u)", what,
          st.diag.vkSelftestFailStep, st.diag.vkSelftestFailFormat, st.diag.vkSelftestFailDirection, st.diag.vkSelftestFailIteration);
    CHECK(st.diag.vkSelftestCompares == n * iterations * 3 && st.diag.vkSelftestIterations == iterations,
          "%s: %u compares for %u iterations", what, st.diag.vkSelftestCompares, st.diag.vkSelftestIterations);
    CHECK(st.diag.vkSelftestFormatsPassed == formats && st.diag.vkSelftestFormatsUnavailable == unavailable,
          "%s: formats passed %#x unavailable %#x", what, st.diag.vkSelftestFormatsPassed, st.diag.vkSelftestFormatsUnavailable);
    CHECK(emu.violations == 0 && emu.acquiresFromUndefined == 0 && emu.acquires == emu.releases &&
          emu.acquires == (int) (n * iterations * 4) && emu.downloads == (int) (n * iterations) && emu.hostBarriers == emu.downloads,
          "%s: %d violations, %d acquires (%d from UNDEFINED), %d releases, %d downloads, %d made visible", what, emu.violations,
          emu.acquires, emu.acquiresFromUndefined, emu.releases, emu.downloads, emu.hostBarriers);
    CHECK(emu.cpuWrites == (int) (n * iterations * 2), "%s: the CPU changed the buffers %d times, not every time it wrote (%u)",
          what, emu.cpuWrites, n * iterations * 2);
    CHECK(st.diag.fallbackTotal == 0, "%s: a fallback recorded", what);
    clean(what);
}

static void failed(const char *what, bool ok, uint8_t step, uint32_t format, uint8_t direction, uint32_t iteration, uint32_t compares) {
    CHECK(!ok && st.diag.vkSelftest == 2, "%s: passed", what);
    CHECK(st.diag.vkSelftestFailStep == step && st.diag.vkSelftestFailFormat == format &&
          st.diag.vkSelftestFailDirection == direction && st.diag.vkSelftestFailIteration == iteration,
          "%s: failed at step %u format %u direction %u iteration %u (want %u %u %u %u)", what, st.diag.vkSelftestFailStep,
          st.diag.vkSelftestFailFormat, st.diag.vkSelftestFailDirection, st.diag.vkSelftestFailIteration, step, format, direction, iteration);
    CHECK(st.diag.vkSelftestCompares == compares, "%s: %u compares counted, %u passed", what, st.diag.vkSelftestCompares, compares);
    CHECK(st.diag.fallbackTotal == 1 && st.diag.fallbackByClass[LORIE_FALLBACK_CLASS_FATAL_BACKEND] == 1 &&
          st.diag.fallbackByReason[LORIE_FALLBACK_VK_SELFTEST] == 1, "%s: fallback not recorded once as VK_SELFTEST", what);
    CHECK(emu.violations == 0, "%s: %d violations", what, emu.violations);
    clean(what);
}

int main(void) {
    static uint8_t a[PIXELS], b[PIXELS], c[PIXELS];
    bool ok;
    int kinds = 0;

    /* the patterns: every kind, changing every iteration, the CPU never writing back what it read */
    for (uint32_t it = 0; it < N; it++) {
        lorieVkSelftestPattern(a, it % 5, it);
        lorieVkSelftestPattern(b, (it + 1) % 5, it + 1);
        CHECK(memcmp(a, b, PIXELS), "iterations %u and %u have the same pattern", it, it + 1);
        lorieVkSelftestPattern(a, it % 5, it ^ 0x5a5a5a5au);
        lorieVkSelftestPattern(c, (it + 2) % 5, it + 1);
        CHECK(memcmp(a, c, PIXELS), "iteration %u: the CPU writes back what it read", it);
        kinds |= 1 << (it % 5);
    }
    CHECK(kinds == 0x1f, "pattern kinds used: %#x", kinds);

    device();
    ok = lorieVkSelftest(&vk, &st, N);
    passed("all formats", ok, N, 7, 0);

    CHECK(vk.verifiedFormats == 7, "all formats: %#x verified", vk.verifiedFormats);
    CHECK(st.diag.vkSelftestUs < 600000000ULL, "all formats: took %llu us", (unsigned long long) st.diag.vkSelftestUs);

    device();
    ok = lorieVkSelftest(&vk, &st, 256);
    passed("256 iterations", ok, 256, 7, 0);

    device();
    ok = lorieVkSelftest(&vk, &st, LORIE_VK_SELFTEST_QUICK_ITERATIONS);
    passed("the quick run", ok, LORIE_VK_SELFTEST_QUICK_ITERATIONS, 7, 0);
    CHECK(LORIE_VK_SELFTEST_QUICK_ITERATIONS >= 5, "the quick run does not use every pattern kind");

    device();
    vk.memoryProperties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    ok = lorieVkSelftest(&vk, &st, 16);
    passed("non-coherent memory", ok, 16, 7, 0);
    CHECK(emu.flushes == 3 * 16 && emu.invalidates == 3 * 16, "non-coherent memory: %d flushes, %d invalidates", emu.flushes, emu.invalidates);

    device();
    emu.allocFail[AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM] = true;
    ok = lorieVkSelftest(&vk, &st, N);
    passed("BGRA not allocatable", ok, N, 3, 4);
    CHECK(vk.verifiedFormats == 3, "BGRA not allocatable: %#x verified", vk.verifiedFormats);

    device();
    emu.bgraExternal = true;
    ok = lorieVkSelftest(&vk, &st, N);
    passed("BGRA of an external format", ok, N, 3, 4);

    device();
    emu.allocFail[AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM] = true;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("RGBX not allocatable", ok, LORIE_VK_SELFTEST_STEP_ALLOCATE, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 0, 0, 3 * N);
    CHECK(st.diag.vkSelftestFormatsPassed == 1 && vk.verifiedFormats == 0, "RGBX not allocatable: formats passed %#x, %#x verified",
          st.diag.vkSelftestFormatsPassed, vk.verifiedFormats);

    /* RGBA, CPU -> Vulkan -> CPU, iteration 500 (an all-zero pattern): its copy is the 501st */
    device();
    emu.corruptCopy = 501;
    emu.corruptByte = 4 * 67 * 10 + 2;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a wrong byte copied, CPU->Vulkan->CPU", ok, LORIE_VK_SELFTEST_STEP_COMPARE, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, 500, 500);
    CHECK(st.diag.vkSelftestFailOffset == emu.corruptByte && st.diag.vkSelftestFailExpected == 0x00 && st.diag.vkSelftestFailActual == 0x01,
          "a wrong byte copied: byte %u expected %#x actual %#x", st.diag.vkSelftestFailOffset, st.diag.vkSelftestFailExpected,
          st.diag.vkSelftestFailActual);

    /* RGBA, Vulkan -> CPU -> Vulkan, iteration 3: the upload (copy 1000 + 2*3 + 1) is wrong, the CPU sees it */
    device();
    emu.corruptCopy = 1000 + 2 * 3 + 1;
    emu.corruptByte = 17;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a wrong byte uploaded", ok, LORIE_VK_SELFTEST_STEP_COMPARE, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 2, 3, N + 2 * 3);
    lorieVkSelftestPattern(a, 3 % 5, 3 ^ 0x5a5a5a5au);
    CHECK(st.diag.vkSelftestFailOffset == 17 && st.diag.vkSelftestFailExpected == a[17] && st.diag.vkSelftestFailActual == (a[17] ^ 1),
          "a wrong byte uploaded: byte %u expected %#x actual %#x", st.diag.vkSelftestFailOffset, st.diag.vkSelftestFailExpected,
          st.diag.vkSelftestFailActual);

    /* RGBX, Vulkan -> CPU -> Vulkan, iteration 7: the download of what the CPU wrote has a wrong red byte */
    device();
    emu.corruptCopy = 3 * N + N + 2 * 7 + 2;
    emu.corruptByte = 4;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a wrong RGBX red byte read back", ok, LORIE_VK_SELFTEST_STEP_COMPARE, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, 2, 7,
           3 * N + N + 2 * 7 + 1);
    lorieVkSelftestPattern(a, (7 + 2) % 5, 7 + 1);
    CHECK(st.diag.vkSelftestFailOffset == 4 && st.diag.vkSelftestFailExpected == a[4] && st.diag.vkSelftestFailActual == (a[4] ^ 1),
          "a wrong RGBX red byte read back: byte %u expected %#x actual %#x", st.diag.vkSelftestFailOffset,
          st.diag.vkSelftestFailExpected, st.diag.vkSelftestFailActual);

    /* the same in RGBX's X byte: undefined to Vulkan, not compared, not a failure */
    device();
    emu.corruptCopy = 3 * N + N + 2 * 7 + 2;
    emu.corruptByte = 7;
    ok = lorieVkSelftest(&vk, &st, N);
    passed("a different RGBX X byte", ok, N, 7, 0);

    /* RGBA's alpha byte is compared: CPU -> Vulkan -> CPU, iteration 2 */
    device();
    emu.corruptCopy = 3;
    emu.corruptByte = 4 * 67 * 3 + 7;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a wrong RGBA alpha byte", ok, LORIE_VK_SELFTEST_STEP_COMPARE, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, 2, 2);
    lorieVkSelftestPattern(a, 2, 2);
    CHECK(st.diag.vkSelftestFailOffset == 4 * 67 * 3 + 7 && st.diag.vkSelftestFailExpected == a[4 * 67 * 3 + 7],
          "a wrong RGBA alpha byte: byte %u expected %#x", st.diag.vkSelftestFailOffset, st.diag.vkSelftestFailExpected);

    /* a lock, a submission, a fence wait */
    device();
    emu.lockFail = 10;      /* iteration 4's second lock: reading the copy */
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a lock fails", ok, LORIE_VK_SELFTEST_STEP_CPU_LOCK, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, 4, 4);
    device();
    emu.submitResult = VK_ERROR_DEVICE_LOST;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a submission fails", ok, LORIE_VK_SELFTEST_STEP_SUBMIT, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, 0, 0);
    CHECK(st.diag.vkSelftestFailResult == VK_ERROR_DEVICE_LOST, "a submission fails: VkResult %d", st.diag.vkSelftestFailResult);
    device();
    emu.fenceResult = VK_TIMEOUT;
    ok = lorieVkSelftest(&vk, &st, N);
    failed("a fence wait times out", ok, LORIE_VK_SELFTEST_STEP_FENCE, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, 1, 0, 0);
    CHECK(st.diag.vkSelftestFailResult == VK_TIMEOUT, "a fence wait times out: VkResult %d", st.diag.vkSelftestFailResult);

    /* its duration */
    {
        static const struct { struct timespec t0, t1; uint64_t us; } t[] = {
            { { 5, 500 }, { 5, 2500 }, 2 },
            { { 1, 999999000 }, { 2, 1000 }, 2 },           /* across a second: tv_nsec smaller at the end */
            { { 10, 999999999 }, { 12, 0 }, 1000000 },
            { { 100, 0 }, { 100, 0 }, 0 },
            { { 7, 300 }, { 6, 999999000 }, 0 },            /* the end before the start: nothing, not a wrapped value */
        };
        for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
            uint64_t us = lorieVkElapsedUs(&t[k].t0, &t[k].t1);
            CHECK(us == t[k].us, "duration %lld.%09ld to %lld.%09ld: %llu us, not %llu", (long long) t[k].t0.tv_sec, t[k].t0.tv_nsec,
                  (long long) t[k].t1.tv_sec, t[k].t1.tv_nsec, (unsigned long long) us, (unsigned long long) t[k].us);
        }
    }

    lorieVkDestroyDevice(&vk);
    printf("TVKSELFTEST ownership self-test on a small device: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
