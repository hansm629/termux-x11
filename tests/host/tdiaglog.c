/* TDIAGLOG: the X server's renderer diagnostics lines, from the real code - struct lorie_renderer_diag from
 * lorie.h and lorieLogRendererDiag from InitOutput.c (gen.py "diagtypes", "diaglog") - written to logs that tdiag.py then
 * judges with tools/diag/renderer-proof.sh, so the script and the format it parses cannot drift apart.
 *
 * Two runs, the counters moved as each backend's successful calls would move them: $OUT/diag-vulkan.log
 * (active VULKAN, imports, draws, submits, presents) and $OUT/diag-gles.log (active GLES, EGLImages, GLES
 * frames, swaps). Here it checks only that every report came out with its keys and the deltas right; and the
 * TERMUX_X11_RENDERER_VK_TRANSFER_COPY line: none while it is off, its verdicts and last copy while it is on; and
 * P11-C3's TERMUX_X11_RENDERER_VK_FRAME_DIAG lines: none while it is off, while it is on each timing's count, mean,
 * percentiles, maximum and frames over 16.667 and 33.333 ms from the samples given, and the period's worst; and the
 * ticks' frame bookkeeping (lorieTickTimed, lorieTickAsked): a frame asked for and none presented by the next tick
 * counted as missed - only then. */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static FILE *logf;
#define log(prio, fmt, ...) fprintf(logf, "10-07 00:00:00.000  1  1 I LorieNative: " fmt "\n", ##__VA_ARGS__)
#include "diagtypes_src.inc"
#include "own_consts_src.inc"
#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1     /* the NDK's values */
#define AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM 2
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
typedef int Bool;
#define TRUE 1
#define FALSE 0
static struct { struct lorie_renderer_diag diag; struct lorie_backend_control backend; volatile uint64_t frameWantedUs;
                struct lorie_progress progress; } fakeState;
static struct {
    typeof(&fakeState) state;
    int yieldOnWake, rootCopyOpt, rootCopyOptFailed, rootGpuCopy;
    struct { uint32_t seq; } rootCopy;
    Bool frameDiag, frameAsked;
    uint32_t framesAtAsk;
    struct { uint32_t us; int client, major, minor; char cmd[24], ext[24]; } worstRequest;
    struct { uint32_t us; int width, height, index; Bool reserved; } worstAccess;
    Bool exaLockOpt;
    Bool freezeDiag;
} fakePvfb = { &fakeState }, *pvfb = &fakePvfb;
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
        fakeState.diag.xCopyRetired += 3, fakeState.diag.xCopyRetiredReleased += 2, fakeState.diag.xCopyCancelled += 1;
        lorieLogRendererDiag();
        fclose(logf);
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf))
            if (strstr(line, "XlorieTick:") && ++tickLines == 2)
                tickOk = strstr(line, "XlorieTick: vsync_to_callback_us=n:0 callback_to_tick_us=n:118,avg:700,p95:750,max:700 "
                                      "ticks=120(+120) ticks_waiting=2 ticks_waiting_max=3 copy_notify_us=n:50,avg:100,p95:250,") &&
                         strstr(line, " copy_recheck_us=n:50,avg:1200,p95:1500,") &&
                         strstr(line, " copy_done_events=50(+50) copy_done_waiting=0 copy_done_waiting_max=0 stamps_invalid=2(+2) "
                                      "copy_early=scrap:1(+1),destroy:0(+0),first_poll:0(+0),later_poll:0(+0),stalled:0(+0) "
                                      "copy_retired=3(+3) copy_retired_live=1 copy_cancelled=1(+1)") != NULL;
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
    /* P11-C3, TERMUX_X11_RENDERER_VK_FRAME_DIAG. Off (every log above): none of its lines, the backend line says so.
     * On: frame starts 8, 17 and 35 ms apart - 90, 6 and 4 of them: mean 9620, p95 in the 20 ms bucket, p99 in the
     * 50 ms one, maximum 35000, 10 over 16.667 ms, 4 over 33.333; the bounds themselves not over them, a microsecond
     * more over (16667, 16668, 33333, 33334); past the last bound ">100000"; each fence its own; the period's worst
     * frame, root copy and request. The next report: "0" for each, the maxima and the worst taken back. */
    {
        static const char *const offLogs[] = { "diag-vulkan.log", "diag-gles.log", "diag-latency.log", "diag-tick.log", "diag-transfer.log" };
        int offLines = 0, offBackend = 0, backendLines = 0, onBackend = 0, n[4] = { 0 }, pacingOk = 0, pipelineOk = 0, rootOk = 0, xOk = 0, emptyOk = 0;
        for (unsigned k = 0; k < sizeof offLogs / sizeof *offLogs; k++) {
            snprintf(path, sizeof path, "%s/%s", out ? out : ".", offLogs[k]);
            logf = fopen(path, "r");
            while (logf && fgets(line, sizeof line, logf)) {
                offLines += strstr(line, "XlorieVkFramePacing:") || strstr(line, "XlorieVkPipeline:") ||
                            strstr(line, "XlorieRootCopyTiming:") || strstr(line, "XlorieXLatency:");
                offBackend += strstr(line, "XlorieBackend:") && strstr(line, " vk_frame_diag=off exa_lock_opt=off vk_schedule_opt=off freeze_diag=off\n");
                backendLines += strstr(line, "XlorieBackend:") != NULL;
            }
            if (logf)
                fclose(logf);
        }
        CHECK(offLines == 0 && backendLines == 17 && offBackend == backendLines, "frame diag off: %d of its lines (want 0), "
              "%d of %d backend lines saying off (want 17)", offLines, offBackend, backendLines);

        snprintf(path, sizeof path, "%s/diag-frame.log", out ? out : ".");
        logf = fopen(path, "w");
        fakePvfb.frameDiag = 1;
        lorieLogRendererDiag();
        for (i = 0; i < 100; i++)
            lorieHistAdd(&fakeState.diag.vkFrameStart, i < 90 ? 8000 : i < 96 ? 17000 : 35000);
        lorieHistAdd(&fakeState.diag.vkPresentDelta, 16667), lorieHistAdd(&fakeState.diag.vkPresentDelta, 16668);
        lorieHistAdd(&fakeState.diag.vkPresentDelta, 33333), lorieHistAdd(&fakeState.diag.vkPresentDelta, 33334);
        lorieHistAdd(&fakeState.diag.vkFrameWake, 200000);
        fakeState.diag.vkFrameIdleGaps += 2, fakeState.diag.vkFrameStampsInvalid += 1;
        fakeState.diag.xTicksFrameWanted += 120, fakeState.diag.xTicksFrameMissed += 7;
        fakeState.diag.vkWorstStartUs = 35000, fakeState.diag.vkWorstWakeUs = 500, fakeState.diag.vkWorstAcquireUs = 21000;
        fakeState.diag.vkWorstLockUs = 300, fakeState.diag.vkWorstFenceUs = 9000, fakeState.diag.vkWorstPresentUs = 1200;
        fakeState.diag.vkWorstAfterRootUs = 4000;
        fakeState.diag.vkWorstPrevCpuUs = 21926, fakeState.diag.vkWorstPrevLockUs = 19345, fakeState.diag.vkWorstPrevFenceUs = 1598;
        fakeState.diag.vkWorstPrevAfterRootUs = 573, fakeState.diag.vkWorstGapUs = 5, fakeState.diag.vkWorstUnaskedUs = 0;
        lorieHistAdd(&fakeState.diag.vkFrameFence, 3000), lorieHistAdd(&fakeState.diag.vkFenceRoot, 1500);
        lorieHistAdd(&fakeState.diag.vkFenceCopy, 700);
        lorieHistAdd(&fakeState.diag.rootTake, 400), lorieHistAdd(&fakeState.diag.rootRecord, 250);
        lorieHistAdd(&fakeState.diag.rootResume, 90), lorieHistAdd(&fakeState.diag.rootBlock, 2000);
        fakeState.diag.rootWorstUs = 5000, fakeState.diag.rootWorstTakeUs = 400, fakeState.diag.rootWorstRecordUs = 250;
        fakeState.diag.rootWorstFenceUs = 4000;
        for (i = 0; i < 3; i++)
            lorieHistAdd(&fakeState.diag.xRequest, 120);
        lorieHistAdd(&fakeState.diag.xRequest, 25000);
        fakePvfb.worstRequest.us = 25000, fakePvfb.worstRequest.client = 7, fakePvfb.worstRequest.major = 130, fakePvfb.worstRequest.minor = 3;
        strcpy(fakePvfb.worstRequest.cmd, "glmark2-es2"), strcpy(fakePvfb.worstRequest.ext, "MIT-SHM");
        lorieHistAdd(&fakeState.diag.xAccess, 800), lorieHistAdd(&fakeState.diag.xAccessLock, 50);
        lorieHistAdd(&fakeState.diag.xAccessHold, 120);
        fakePvfb.worstAccess.us = 19379, fakePvfb.worstAccess.width = 800, fakePvfb.worstAccess.height = 600;
        fakePvfb.worstAccess.index = EXA_PREPARE_SRC, fakePvfb.worstAccess.reserved = TRUE;
        lorieHistAdd(&fakeState.diag.xAccessGpu, 6000), lorieHistAdd(&fakeState.diag.xTickDelay, 700);
        lorieHistAdd(&fakeState.diag.xTickWork, 300);
        lorieLogRendererDiag();
        lorieLogRendererDiag();
        fclose(logf);
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf)) {
            onBackend += strstr(line, "XlorieBackend:") && strstr(line, " vk_frame_diag=on exa_lock_opt=off vk_schedule_opt=off freeze_diag=off\n");
            if (strstr(line, "XlorieVkFramePacing:") && ++n[0] == 2)
                pacingOk = strstr(line, "XlorieVkFramePacing: frame_start_us=100/9620/20000/50000/35000/10/4 "
                                        "present_delta_us=4/25000/50000/50000/33334/3/1 wake_us=1/200000/>100000/>100000/200000/1/1 "
                                        "idle_gaps=2(+2) stamps_invalid=1(+1) ticks_frame_wanted=120(+120) ticks_frame_missed=7(+7) "
                                        "worst_start_us=35000 worst_wake_us=500 worst_acquire_us=21000 worst_lock_us=300 "
                                        "worst_fence_us=9000 worst_present_us=1200 worst_after_root_us=4000 worst_prev_cpu_us=21926 "
                                        "worst_prev_lock_us=19345 worst_prev_fence_us=1598 worst_prev_after_root_us=573 worst_gap_us=5 "
                                        "worst_unasked_us=0\n") != NULL;
            if (strstr(line, "XlorieVkFramePacing:") && n[0] == 3)
                emptyOk += strstr(line, "XlorieVkFramePacing: frame_start_us=0 present_delta_us=0 wake_us=0 idle_gaps=2(+0) "
                                        "stamps_invalid=1(+0) ticks_frame_wanted=120(+0) ticks_frame_missed=7(+0) worst_start_us=0 ") != NULL;
            if (strstr(line, "XlorieVkPipeline:") && ++n[1] == 2)
                pipelineOk = strstr(line, "XlorieVkPipeline: cpu_us=0 acquire_us=0 lock_us=0 record_us=0 submit_us=0 "
                                          "fence_frame_us=1/3000/3000/3000/3000/0/0 fence_root_us=1/1500/1500/1500/1500/0/0 "
                                          "fence_copy_us=1/700/750/750/700/0/0 present_us=0 after_root_us=0\n") != NULL;
            if (strstr(line, "XlorieVkPipeline:") && n[1] == 3)
                emptyOk += strstr(line, "fence_frame_us=0 fence_root_us=0 fence_copy_us=0 ") != NULL;
            if (strstr(line, "XlorieRootCopyTiming:") && ++n[2] == 2)
                rootOk = strstr(line, "XlorieRootCopyTiming: take_us=1/400/500/500/400/0/0 record_us=1/250/300/300/250/0/0 "
                                      "resume_us=1/90/100/100/90/0/0 x_block_us=1/2000/2000/2000/2000/0/0 worst_total_us=5000 "
                                      "worst_take_us=400 worst_record_us=250 worst_fence_us=4000\n") != NULL;
            if (strstr(line, "XlorieRootCopyTiming:") && n[2] == 3)
                emptyOk += strstr(line, "take_us=0 record_us=0 resume_us=0 x_block_us=0 worst_total_us=0 ") != NULL;
            if (strstr(line, "XlorieXLatency:") && ++n[3] == 2)
                xOk = strstr(line, "XlorieXLatency: request_us=4/6340/25000/25000/25000/1/0 access_us=1/800/1000/1000/800/0/0 "
                                   "access_lock_us=1/50/50/50/50/0/0 access_gpu_us=1/6000/6000/6000/6000/0/0 "
                                   "tick_delay_us=1/700/750/750/700/0/0 tick_work_us=1/300/300/300/300/0/0 "
                                   "worst_request_us=25000 worst_request=7:glmark2-es2:MIT-SHM:130.3 access_hold_us=1/120/200/200/120/0/0 "
                                   "worst_access_us=19379 worst_access=800x600:src:reserved\n") != NULL;
            if (strstr(line, "XlorieXLatency:") && n[3] == 3)
                emptyOk += strstr(line, "request_us=0 access_us=0 access_lock_us=0 access_gpu_us=0 tick_delay_us=0 tick_work_us=0 "
                                        "worst_request_us=0 worst_request=-1:none:none:0.0 access_hold_us=0 worst_access_us=0 "
                                        "worst_access=0x0:none:none\n") != NULL;
        }
        fclose(logf);
        CHECK(onBackend == 3 && n[0] == 3 && n[1] == 3 && n[2] == 3 && n[3] == 3, "frame diag on: %d backend lines saying on, "
              "lines %d/%d/%d/%d (want 3 each)", onBackend, n[0], n[1], n[2], n[3]);
        CHECK(pacingOk, "frame diag: the pacing line's timings or worst frame wrong");
        CHECK(pipelineOk, "frame diag: the pipeline line's fences not each their own");
        CHECK(rootOk, "frame diag: the root copy timing line wrong");
        CHECK(xOk, "frame diag: the X latency line wrong");
        CHECK(emptyOk == 4, "frame diag: %d of 4 lines without samples say so and took the worst back", emptyOk);
        CHECK(!fakeState.diag.vkFrameStart.maxUs && !fakeState.diag.vkPresentDelta.maxUs && !fakeState.diag.xRequest.maxUs &&
              !fakeState.diag.vkWorstStartUs && !fakeState.diag.rootWorstUs && !fakePvfb.worstRequest.us && !fakePvfb.worstAccess.us,
              "frame diag: a maximum or worst not taken back by its report");
    }

    /* P11-C3: the ticks' frame bookkeeping. The first tick: nothing asked before, nothing missed. A tick asking for a
     * frame, the next one with none presented since: missed (once - the one after it asked for none); with one presented
     * (Vulkan's or GLES's): not; a tick that asked for none: nothing missed after it. Each tick's wait from its callback (the stamps' low 32 bits, across their
     * wrap too); none without a callback or over 10 s. The request's stamp and its work. */
    {
        volatile struct lorie_renderer_diag *dg = &fakeState.diag;
        uint32_t missed = dg->xTicksFrameMissed, wanted = dg->xTicksFrameWanted, delays = dg->xTickDelay.count;
        uint64_t sum = dg->xTickDelay.sumUs, now = lorieMonotonicUs(), work = dg->xTickWork.count;
        fakePvfb.frameAsked = FALSE;
        lorieTickTimed(now, (uint32_t) now - 700, TRUE);
        CHECK(dg->xTicksFrameMissed == missed && dg->xTickDelay.count == delays + 1 && dg->xTickDelay.sumUs == sum + 700,
              "ticks: the first one missed %u, its wait %llu us", dg->xTicksFrameMissed - missed,
              (unsigned long long) (dg->xTickDelay.sumUs - sum));
        lorieTickAsked(now);
        CHECK(fakePvfb.frameAsked && dg->xTicksFrameWanted == wanted + 1 && dg->xTickWork.count == work + 1 &&
              fakeState.frameWantedUs >= now, "ticks: asking for a frame not recorded");
        lorieTickTimed(now + 8333, 0, FALSE);
        CHECK(dg->xTicksFrameMissed == missed + 1 && dg->xTickDelay.count == delays + 1, "ticks: none presented, %u missed (want 1), "
              "%u waits without a callback", dg->xTicksFrameMissed - missed, dg->xTickDelay.count - delays - 1);
        lorieTickTimed(now + 16666, 0, FALSE);    /* the tick before asked for none: still none presented, nothing more missed */
        CHECK(dg->xTicksFrameMissed == missed + 1, "ticks: a tick after one that asked for none, %u more missed",
              dg->xTicksFrameMissed - missed - 1);
        lorieTickAsked(now + 8333);
        dg->vkFramesPresented++;
        lorieTickTimed(now + 16666, 0, FALSE);
        lorieTickAsked(now + 16666);
        dg->eglSwapBuffers++;
        lorieTickTimed(now + 25000, 0, FALSE);
        CHECK(dg->xTicksFrameMissed == missed + 1, "ticks: a frame presented, still %u missed", dg->xTicksFrameMissed - missed - 1);
        lorieTickTimed(now + 33333, 0, FALSE);    /* the tick before asked for none */
        CHECK(dg->xTicksFrameMissed == missed + 1, "ticks: none asked for, %u missed", dg->xTicksFrameMissed - missed - 1);
        sum = dg->xTickDelay.sumUs, delays = dg->xTickDelay.count;
        lorieTickTimed(0x100000100ull, 0xffffff00u, TRUE);
        lorieTickTimed(now, (uint32_t) now - 20000000, TRUE);
        CHECK(dg->xTickDelay.count == delays + 1 && dg->xTickDelay.sumUs == sum + 0x200, "ticks: across the wrap %llu us (want 512), "
              "%u waits (want 1: none over 10 s)", (unsigned long long) (dg->xTickDelay.sumUs - sum), dg->xTickDelay.count - delays);
    }

    /* P11-C3-B1: XlorieSchedule in every report - TERMUX_X11_EXA_LOCK_OPT off, its counters as they moved; on. P11-C3-C:
     * the buffers waited for, given up, and the copies left for them, always */
    {
        int lines = 0, offOk = 0, onOk = 0;
        snprintf(path, sizeof path, "%s/diag-schedule.log", out ? out : ".");
        logf = fopen(path, "w");
        lorieLogRendererDiag();
        fakePvfb.exaLockOpt = TRUE;
        fakeState.backend.vkScheduleOpt = 1;
        fakeState.diag.xAccessReserved += 1201, fakeState.diag.xAccessReserveFull += 2;
        fakeState.diag.copiesDeferred += 7, fakeState.diag.copiesDeferredWakes += 6;
        fakeState.diag.rootAhead += 596, fakeState.diag.rootAheadXWaiting += 5, fakeState.diag.rootChainConsumed += 3;
        fakeState.diag.rootAheadUnavailable += 1;
        fakeState.diag.buffersAwaited += 4, fakeState.diag.bufferAwaitUs += 123456, fakeState.diag.buffersGivenUp += 1;
        fakeState.diag.copiesLeftForBuffers += 2;      /* P11-C3-C */
        lorieLogRendererDiag();
        fakePvfb.exaLockOpt = FALSE;
        fakeState.backend.vkScheduleOpt = 0;
        fclose(logf);
        logf = fopen(path, "r");
        while (fgets(line, sizeof line, logf)) {
            if (!strstr(line, "XlorieSchedule:"))
                continue;
            lines++;
            offOk += lines == 1 && strstr(line, "XlorieSchedule: exa_lock_opt=off x_access_reserved=0(+0) x_access_reserve_full=0(+0) "
                                                "copies_deferred=0(+0) copies_deferred_wakes=0(+0) vk_schedule_opt=off root_ahead=0(+0) "
                                                "root_ahead_x_waiting=0(+0) root_chain_consumed=0(+0) root_ahead_unavailable=0(+0) "
                                                "buffers_awaited=0(+0) buffer_await_us=0(+0) buffers_given_up=0(+0) "
                                                "copies_left_for_buffers=0(+0)\n") != NULL;
            onOk += lines == 2 && strstr(line, "XlorieSchedule: exa_lock_opt=on x_access_reserved=1201(+1201) "
                                               "x_access_reserve_full=2(+2) copies_deferred=7(+7) copies_deferred_wakes=6(+6) "
                                               "vk_schedule_opt=on root_ahead=596(+596) root_ahead_x_waiting=5(+5) "
                                               "root_chain_consumed=3(+3) root_ahead_unavailable=1(+1) buffers_awaited=4(+4) "
                                               "buffer_await_us=123456(+123456) buffers_given_up=1(+1) copies_left_for_buffers=2(+2)\n") != NULL;
        }
        fclose(logf);
        CHECK(lines == 2 && offOk && onOk, "schedule log: %d lines (want 2), off %s, on %s", lines, offOk ? "right" : "wrong",
              onOk ? "right" : "wrong");
    }

    /* every message within __android_log_print's 1024-byte buffer (longer ones are cut): the copy latency line with
     * every part at its widest, and P11-C3's lines with every timing and worst at its widest, then every line written
     * above */
    {
        static const char *const logs[] = { "diag-vulkan.log", "diag-gles.log", "diag-latency.log", "diag-tick.log", "diag-transfer.log",
                                            "diag-frame.log", "diag-schedule.log", "diag-wide.log" };
        volatile struct lorie_hist *hists[] = {
            &fakeState.diag.vkFrameStart, &fakeState.diag.vkPresentDelta, &fakeState.diag.vkFrameWake, &fakeState.diag.vkFrameCpu,
            &fakeState.diag.vkFrameAcquire, &fakeState.diag.vkFrameLock, &fakeState.diag.vkFrameRecord, &fakeState.diag.vkFrameSubmit,
            &fakeState.diag.vkFrameFence, &fakeState.diag.vkFramePresent, &fakeState.diag.vkFrameAfterRoot, &fakeState.diag.vkFenceRoot,
            &fakeState.diag.vkFenceCopy, &fakeState.diag.rootTake, &fakeState.diag.rootRecord, &fakeState.diag.rootResume,
            &fakeState.diag.rootBlock, &fakeState.diag.xRequest, &fakeState.diag.xAccess, &fakeState.diag.xAccessLock,
            &fakeState.diag.xAccessGpu, &fakeState.diag.xTickDelay, &fakeState.diag.xTickWork, &fakeState.diag.xAccessHold };
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
        fakeState.diag.xCopyRetired += 999999999, fakeState.diag.xCopyCancelled += 999999999;
        for (unsigned k = 0; k < sizeof parts / sizeof *parts; k++)
            for (i = 0; i < 1000; i++)
                lorieLatencyAddWeighted(parts[k], 4294967295u, 1);
        for (unsigned k = 0; k < sizeof hists / sizeof *hists; k++)
            for (i = 0; i < 1000; i++)
                lorieHistAdd(hists[k], 4294967295u);
        fakeState.diag.vkFrameIdleGaps += 999999999, fakeState.diag.vkFrameStampsInvalid += 999999999;
        fakeState.diag.xTicksFrameWanted += 999999999, fakeState.diag.xTicksFrameMissed += 999999999;
        fakeState.diag.vkWorstStartUs = fakeState.diag.vkWorstWakeUs = fakeState.diag.vkWorstAcquireUs = 4294967295u;
        fakeState.diag.vkWorstLockUs = fakeState.diag.vkWorstFenceUs = fakeState.diag.vkWorstPresentUs = 4294967295u;
        fakeState.diag.vkWorstAfterRootUs = fakeState.diag.rootWorstUs = fakeState.diag.rootWorstTakeUs = 4294967295u;
        fakeState.diag.rootWorstRecordUs = fakeState.diag.rootWorstFenceUs = 4294967295u;
        fakeState.diag.vkWorstPrevCpuUs = fakeState.diag.vkWorstPrevLockUs = fakeState.diag.vkWorstPrevFenceUs = 4294967295u;
        fakeState.diag.vkWorstPrevAfterRootUs = fakeState.diag.vkWorstGapUs = fakeState.diag.vkWorstUnaskedUs = 4294967295u;
        fakePvfb.worstRequest.us = 4294967295u, fakePvfb.worstRequest.client = 2047, fakePvfb.worstRequest.major = 255;
        fakePvfb.worstRequest.minor = 255;
        memset(fakePvfb.worstRequest.cmd, 'c', 23), memset(fakePvfb.worstRequest.ext, 'e', 23);
        fakePvfb.worstAccess.us = 4294967295u, fakePvfb.worstAccess.width = fakePvfb.worstAccess.height = 32767;
        fakePvfb.worstAccess.index = EXA_PREPARE_MASK, fakePvfb.worstAccess.reserved = TRUE;
        fakeState.diag.xAccessReserved += 999999999, fakeState.diag.xAccessReserveFull += 999999999;
        fakeState.diag.copiesDeferred += 999999999, fakeState.diag.copiesDeferredWakes += 999999999;
        fakeState.diag.rootAhead += 999999999, fakeState.diag.rootAheadXWaiting += 999999999;
        fakeState.diag.rootChainConsumed += 999999999, fakeState.diag.rootAheadUnavailable += 999999999;
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
