/* TDIAGLOG: the X server's renderer diagnostics lines, from the real code - struct lorie_renderer_diag from
 * lorie.h and lorieLogRendererDiag from InitOutput.c (gen.py "diagtypes", "diaglog") - written to logs that tdiag.py then
 * judges with tools/diag/renderer-proof.sh, so the script and the format it parses cannot drift apart.
 *
 * Two runs, the counters moved as each backend's successful calls would move them: $OUT/diag-vulkan.log
 * (active VULKAN, imports, draws, submits, presents) and $OUT/diag-gles.log (active GLES, EGLImages, GLES
 * frames, swaps). Here it checks only that every report came out with its keys and the deltas right; and the
 * TERMUX_X11_RENDERER_VK_TRANSFER_COPY line: none while it is off, its verdicts and last copy while it is on. */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *logf;
#define log(prio, fmt, ...) fprintf(logf, "10-07 00:00:00.000  1  1 I LorieNative: " fmt "\n", ##__VA_ARGS__)
#include "diagtypes_src.inc"
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1     /* the NDK's values */
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
static struct { struct lorie_renderer_diag diag; struct lorie_backend_control backend; } fakeState;
static struct { typeof(&fakeState) state; } fakePvfb = { &fakeState }, *pvfb = &fakePvfb;
#include "diaglog_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *out = getenv("OUT");
    char path[512], line[4096];
    int i, reports = 0, deltaOk = 0, backendOk = 0, selftestLines = 0;

    snprintf(path, sizeof path, "%s/diag-vulkan.log", out ? out : ".");
    logf = fopen(path, "w");
    fakeState.diag.active = LORIE_ACTIVE_VULKAN;
    fakeState.backend.requested = LORIE_BACKEND_VULKAN;
    fakeState.diag.xOwnership = LORIE_OWNERSHIP_PER_ACCESS;
    fakeState.diag.xOwnershipTransitions = 1;
    fakeState.diag.vkInstanceCreated = fakeState.diag.vkDeviceCreated = 1;
    fakeState.diag.vkSelftest = 1;              /* passed: 3 formats x 1000 iterations x 3 compares */
    fakeState.diag.vkSelftestIterations = 1000;
    fakeState.diag.vkSelftestCompares = 9000;
    fakeState.diag.vkSelftestFormatsPassed = 7;
    fakeState.diag.vkSelftestUs = 2100000;
    fakeState.diag.vkSurfaceCreated = fakeState.diag.vkSwapchainCreated = 1;
    for (i = 0; i < 4; i++) {
        fakeState.diag.vkAhbImportAttempts += 2; fakeState.diag.vkAhbImportSuccess += 2;
        fakeState.diag.vkImportedSourceDraws += 300; fakeState.diag.vkImportedDestinationDraws += 50;
        fakeState.diag.vkRenderCommands += 350; fakeState.diag.vkQueueSubmits += 300;
        fakeState.diag.vkQueuePresentSuccess += 300; fakeState.diag.vkFramesPresented += 300;
        fakeState.diag.vkAcquireSuccess += 300; fakeState.diag.vkFenceWaitUs += 900000;
        fakeState.diag.xAhbCpuAcquires += 120; fakeState.diag.xStateLockWaitUs += 4000;
        fakeState.diag.xGpuWaitUs += 3000; fakeState.diag.xAhbCpuLockUs += 500;
        lorieLogRendererDiag();
    }
    fclose(logf);
    logf = fopen(path, "r");
    while (fgets(line, sizeof line, logf)) {
        if (strstr(line, "XlorieVkSelftest:")) {
            CHECK(strstr(line, "XlorieVkSelftest: result=pass compares=9000(+") && strstr(line, " iterations=1000 formats_passed=0x7 ") &&
                  strstr(line, " fail_step=NONE "), "self-test line: %s", line);
            selftestLines++;
        }
        if (strstr(line, "XlorieBackend:") && strstr(line, "x_ahb_cpu_acquires=480(+120)")) {
            CHECK(strstr(line, "XlorieBackend: requested=vulkan vk_fail_at=none vk_selftest=quick active=VULKAN ownership=per_access "
                               "ownership_transitions=1 ")
                  && strstr(line, "x_state_lock_wait_us=16000(+4000) x_gpu_wait_us=12000(+3000) x_ahb_cpu_lock_us=2000(+500)"),
                  "vulkan backend line: %s", line);
            backendOk++;
        }
    }
    fclose(logf);
    CHECK(selftestLines == 4, "vulkan log: %d self-test lines", selftestLines);
    {
        int transferLines = 0;
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf))
            transferLines += strstr(line, "XlorieVkTransfer:") != NULL;
        fclose(logf);
        CHECK(transferLines == 0, "vulkan log: %d transfer-copy lines while it is off", transferLines);
    }
    CHECK(backendOk == 1, "vulkan log: %d backend lines with the expected X ownership counters", backendOk);

    snprintf(path, sizeof path, "%s/diag-gles.log", out ? out : ".");
    logf = fopen(path, "w");
    fakeState.diag.active = LORIE_ACTIVE_GLES;
    fakeState.diag.vkSelftest = 2;              /* failed: the fallback below */
    fakeState.diag.vkSelftestFailStep = LORIE_VK_SELFTEST_STEP_COMPARE;
    fakeState.diag.xOwnership = LORIE_OWNERSHIP_PERSISTENT;     /* given back on the fallback */
    fakeState.diag.xOwnershipTransitions = 2;
    fakeState.diag.fallbackTotal++;                    /* one fallback, before the GLES reports */
    fakeState.diag.fallbackByClass[LORIE_FALLBACK_CLASS_FATAL_BACKEND]++;
    fakeState.diag.fallbackByReason[LORIE_FALLBACK_VK_DEVICE_LOST]++;
    fakeState.diag.lastFallbackClass = LORIE_FALLBACK_CLASS_FATAL_BACKEND;
    fakeState.diag.lastFallbackReason = LORIE_FALLBACK_VK_DEVICE_LOST;
    lorieLogRendererDiag();
    for (i = 0; i < 3; i++) {
        fakeState.diag.eglImageImports += 1; fakeState.diag.glesCopyFrames += 300; fakeState.diag.eglSwapBuffers += 300;
        lorieLogRendererDiag();
    }
    fclose(logf);

    logf = fopen(path, "r");
    while (fgets(line, sizeof line, logf)) {
        if (strstr(line, "XlorieGles:")) {
            reports++;
            if (strstr(line, "gles_copy_frames=900(+300)"))
                deltaOk++;
        }
        if (strstr(line, "XlorieBackend:"))
            CHECK(strstr(line, "XlorieBackend: requested=vulkan vk_fail_at=none vk_selftest=quick active=GLES ownership=persistent "
                               "ownership_transitions=2 "),
                  "gles backend line: %s", line);
        if (strstr(line, "XlorieFallback:"))
            CHECK(strstr(line, "last_reason=VK_DEVICE_LOST") && strstr(line, "reasons=VK_DEVICE_LOST:1"),
                  "fallback line without its reason: %s", line);
    }
    fclose(logf);
    CHECK(reports == 4 && deltaOk == 1, "gles log: %d reports, %d with the expected total and delta", reports, deltaOk);

    /* P9-B0: a latency's report - 100 samples, 90 of 400 us, 5 of 3 ms, 5 of 70 ms: mean 4010, p95 in the 3 ms bucket,
     * the maximum 70 ms, taken back to 0; the next report, without samples, says so; all of 100 past the last bound.
     * A submission's part as a share of the entries' time: 10 submissions of 4 entries waiting 1 ms each, the 40
     * entries 10 ms each in all - 40 ms of 400 ms, 10.0% */
    {
        int latencyLines = 0, firstOk = 0, emptyOk = 0, pastOk = 0, shareOk = 0;
        snprintf(path, sizeof path, "%s/diag-latency.log", out ? out : ".");
        logf = fopen(path, "w");
        lorieLogRendererDiag();
        for (i = 0; i < 100; i++)
            lorieLatencyAdd(&fakeState.diag.copyQueue, i < 90 ? 400 : i < 95 ? 3000 : 70000);
        fakeState.diag.copyStandaloneEntries += 100;
        lorieLogRendererDiag();
        lorieLogRendererDiag();
        for (i = 0; i < 100; i++)
            lorieLatencyAdd(&fakeState.diag.copyGpuWait, 70000);
        lorieLogRendererDiag();
        for (i = 0; i < 10; i++)
            lorieLatencyAddWeighted(&fakeState.diag.copyLock, 1000, 4);
        for (i = 0; i < 40; i++)
            lorieLatencyAdd(&fakeState.diag.copyTotal, 10000);
        lorieLogRendererDiag();
        fclose(logf);
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf))
            if (strstr(line, "XlorieCopyLatency:")) {
                latencyLines++;
                if (latencyLines == 2)
                    firstOk = strstr(line, "XlorieCopyLatency: copy_standalone_entries=100(+100) ") &&
                              strstr(line, " copy_queue_us=n:100,avg:4010,p95:3000,max:70000 ") && strstr(line, " copy_lock_us=n:0 ");
                if (latencyLines == 3)
                    emptyOk = strstr(line, " copy_standalone_entries=100(+0) ") && strstr(line, " copy_queue_us=n:0 ");
                if (latencyLines == 4)
                    pastOk = strstr(line, " copy_gpu_wait_us=n:100,avg:70000,p95:>66000,max:70000 ") != NULL;
                if (latencyLines == 5)
                    shareOk = strstr(line, " copy_lock_us=n:10,avg:1000,p95:1000,max:1000,share:10.0% ") &&
                              strstr(line, " copy_total_us=n:40,avg:10000,p95:12000,max:10000 ") && strstr(line, " copy_queue_us=n:0 ");
            }
        fclose(logf);
        CHECK(latencyLines == 5 && firstOk && emptyOk && pastOk && shareOk, "latency log: %d lines (want 5); with samples %s, "
              "without %s, past the last bound %s, share %s", latencyLines, firstOk ? "right" : "wrong", emptyOk ? "right" : "wrong",
              pastOk ? "right" : "wrong", shareOk ? "right" : "wrong");
        CHECK(fakeState.diag.copyQueue.maxUs == 0 && fakeState.diag.copyGpuWait.maxUs == 0, "latency: a maximum not taken back by its report");
    }

    /* P10-B0: the X server's ticks and completions - 120 ticks posted, 118 run (2 waiting, 3 the most behind one),
     * their waits; 50 completions all run; 2 stamps out of order; one presentation scrapped early */
    {
        int tickLines = 0, tickOk = 0;
        snprintf(path, sizeof path, "%s/diag-tick.log", out ? out : ".");
        logf = fopen(path, "w");
        lorieLogRendererDiag();
        fakeState.diag.xTicksPosted += 120, fakeState.diag.xTicksRun += 118, fakeState.diag.xTicksWaitingMax = 3;
        for (i = 0; i < 118; i++)
            lorieLatencyAdd(&fakeState.diag.xCallbackToTick, 700);
        fakeState.diag.xCopyDonePosted += 50, fakeState.diag.xCopyDoneRun += 50;
        for (i = 0; i < 50; i++)
            lorieLatencyAdd(&fakeState.diag.xCopyRecheck, 1200), lorieLatencyAdd(&fakeState.diag.xCopyNotify, 100);
        fakeState.diag.xStampsInvalid += 2;
        fakeState.diag.xCopyEarly[LORIE_X_EARLY_SCRAP] += 1;
        lorieLogRendererDiag();
        fclose(logf);
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf))
            if (strstr(line, "XlorieTick:") && ++tickLines == 2)
                tickOk = strstr(line, "XlorieTick: vsync_to_callback_us=n:0 callback_to_tick_us=n:118,avg:700,p95:750,max:700 "
                                      "ticks=120(+120) ticks_waiting=2 ticks_waiting_max=3 copy_notify_us=n:50,avg:100,p95:250,") &&
                         strstr(line, " copy_recheck_us=n:50,avg:1200,p95:1500,") &&
                         strstr(line, " copy_done_events=50(+50) copy_done_waiting=0 copy_done_waiting_max=0 stamps_invalid=2(+2) "
                                      "copy_early=scrap:1(+1),destroy:0(+0),first_poll:0(+0),later_poll:0(+0),stalled:0(+0)") != NULL;
        fclose(logf);
        CHECK(tickLines == 2 && tickOk, "tick log: %d lines (want 2), the second %s", tickLines, tickOk ? "right" : "wrong");
        CHECK(fakeState.diag.xTicksWaitingMax == 0, "tick log: the most waiting not taken back by its report");
    }

    /* TERMUX_X11_RENDERER_VK_TRANSFER_COPY on: copies kept from vkCmdCopyImage by a BGRA source and an image whose
     * usage was refused; the last one BGRA into RGBX of depth 24 */
    snprintf(path, sizeof path, "%s/diag-transfer.log", out ? out : ".");
    logf = fopen(path, "w");
    fakeState.backend.vkTransferCopy = 1;
    lorieLogRendererDiag();
    fakeState.diag.vkXferChecked += 12;
    fakeState.diag.vkCopyCommands += 2;
    fakeState.diag.vkXferNot[LORIE_VK_XFER_NOT_FORMAT] += 9;
    fakeState.diag.vkXferNot[LORIE_VK_XFER_NOT_SRC_USAGE] += 1;
    fakeState.diag.vkXferUsageMissing[LORIE_VK_XFER_USAGE_REFUSED] += 1;
    fakeState.diag.vkXferImports[LORIE_VK_XFER_USAGE_GRANTED] += 3;
    fakeState.diag.vkXferImports[LORIE_VK_XFER_USAGE_REFUSED] += 1;
    fakeState.diag.vkXferLastNot = 1u << LORIE_VK_XFER_NOT_FORMAT | 1u << LORIE_VK_XFER_NOT_RECT_SOURCE;
    fakeState.diag.vkXferLastSrcFormat = AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;
    fakeState.diag.vkXferLastDstFormat = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
    fakeState.diag.vkXferLastSrcWidth = 800, fakeState.diag.vkXferLastSrcHeight = 600;
    fakeState.diag.vkXferLastDstWidth = 802, fakeState.diag.vkXferLastDstHeight = 630;
    fakeState.diag.vkXferLastDstDepth = 24;
    fakeState.diag.vkXferLastSrcAhbUsage = 0x300, fakeState.diag.vkXferLastSrcUsage = 0x17;
    fakeState.diag.vkXferLastRects = 1;
    fakeState.diag.vkXferLastRect[2] = 800, fakeState.diag.vkXferLastRect[3] = 600;
    fakeState.diag.vkXferLastOff[0] = 1, fakeState.diag.vkXferLastOff[1] = 29;
    fakeState.diag.vkXferCheck = LORIE_VK_XFER_CHECK_FAIL;
    fakeState.diag.vkXferCheckStep = LORIE_VK_SELFTEST_STEP_COMPARE;
    fakeState.diag.vkXferCheckOffset = 286, fakeState.diag.vkXferCheckExpected = 0x12, fakeState.diag.vkXferCheckActual = 0x56;
    lorieLogRendererDiag();
    fclose(logf);
    {
        int transferLines = 0, lastOk = 0;
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf))
            if (strstr(line, "XlorieVkTransfer:")) {
                transferLines++;
                lastOk += strstr(line, "XlorieVkTransfer: checked=12(+12) copied=") && strstr(line, " not_format=9(+9) ") &&
                          strstr(line, " not_src_usage=1(+1) ") && strstr(line, " usage_missing_refused=1(+1) ") &&
                          strstr(line, " imports_granted=3(+3) ") && strstr(line, " imports_refused=1(+1) ") &&
                          strstr(line, " last_not=FORMAT+RECT_SOURCE last_src=BGRA(5)/800x600/ahb_usage=0x300/vk_usage=0x17 "
                                       "last_dst=RGBX(2)/802x630/depth=24/") &&
                          strstr(line, " last_rects=1 last_first=0,0-800,600 last_off=1,29 bgra_rgbx_check=FAIL check_step=COMPARE "
                                       "check_byte=286 check_expected=0x12 check_actual=0x56 check_vkresult=0") != NULL;
            }
        fclose(logf);
        CHECK(transferLines == 2 && lastOk == 1, "transfer log: %d lines (want 2), %d with the verdicts and last copy (want 1)",
              transferLines, lastOk);
    }
    /* every message within __android_log_print's 1024-byte buffer (longer ones are cut): the copy latency line with
     * every part at its widest, then every line written above */
    {
        static const char *const logs[] = { "diag-vulkan.log", "diag-gles.log", "diag-latency.log", "diag-tick.log", "diag-transfer.log",
                                            "diag-wide.log" };
        volatile struct lorie_latency *parts[] = { &fakeState.diag.copyQueue, &fakeState.diag.copyLock, &fakeState.diag.copyInflightWait,
                                                   &fakeState.diag.copyRecord, &fakeState.diag.copyGpuWait, &fakeState.diag.copyFrameGpuWait,
                                                   &fakeState.diag.copyTotal, &fakeState.diag.copyAck, &fakeState.diag.xVsyncToCallback,
                                                   &fakeState.diag.xCallbackToTick, &fakeState.diag.xCopyNotify, &fakeState.diag.xCopyRecheck };
        size_t longest = 0, prefix = strlen("10-07 00:00:00.000  1  1 I LorieNative: ");
        snprintf(path, sizeof path, "%s/diag-wide.log", out ? out : ".");
        logf = fopen(path, "w");
        lorieLogRendererDiag();
        fakeState.diag.copyStandaloneEntries += 999999999, fakeState.diag.copyFrameEntries += 999999999;
        fakeState.diag.copyStandaloneSubmits += 999999999, fakeState.diag.copyFrameSubmits += 999999999;
        fakeState.diag.xTicksPosted += 999999999, fakeState.diag.xCopyDonePosted += 999999999, fakeState.diag.xStampsInvalid += 999999999;
        fakeState.diag.xTicksWaitingMax = fakeState.diag.xCopyDoneWaitingMax = 999999999;
        for (i = 0; i < LORIE_X_EARLY_COUNT; i++)
            fakeState.diag.xCopyEarly[i] += 999999999;
        for (unsigned k = 0; k < sizeof parts / sizeof *parts; k++)
            for (i = 0; i < 1000; i++)
                lorieLatencyAddWeighted(parts[k], 4294967295u, 1);
        lorieLogRendererDiag();
        fclose(logf);
        for (unsigned k = 0; k < sizeof logs / sizeof *logs; k++) {
            snprintf(path, sizeof path, "%s/%s", out ? out : ".", logs[k]);
            if (!(logf = fopen(path, "r")))
                continue;
            while (fgets(line, sizeof line, logf))
                if (strlen(line) > prefix + 1 && strlen(line) - prefix - 1 > longest)
                    longest = strlen(line) - prefix - 1;
            fclose(logf);
        }
        CHECK(longest <= 1023, "a diagnostics message of %zu bytes: __android_log_print cuts it at 1023", longest);
        printf("  longest diagnostics message: %zu bytes\n", longest);
    }
    printf("TDIAGLOG renderer diagnostics written by the real formatter: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
