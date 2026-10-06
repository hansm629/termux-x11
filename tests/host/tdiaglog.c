/* TDIAGLOG: the X server's renderer diagnostics lines, from the real code - struct lorie_renderer_diag from
 * lorie.h and lorieLogRendererDiag from InitOutput.c (gen.py "diagtypes", "diaglog") - written to logs that tdiag.py then
 * judges with tools/diag/renderer-proof.sh, so the script and the format it parses cannot drift apart.
 *
 * Two runs, the counters moved as each backend's successful calls would move them: $OUT/diag-vulkan.log
 * (active VULKAN, imports, draws, submits, presents) and $OUT/diag-gles.log (active GLES, EGLImages, GLES
 * frames, swaps). Here it checks only that every report came out with its keys and the deltas right. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static FILE *logf;
#define log(prio, fmt, ...) fprintf(logf, "10-07 00:00:00.000  1  1 I LorieNative: " fmt "\n", ##__VA_ARGS__)
#include "diagtypes_src.inc"
static struct { struct lorie_renderer_diag diag; struct lorie_backend_control backend; } fakeState;
static struct { typeof(&fakeState) state; } fakePvfb = { &fakeState }, *pvfb = &fakePvfb;
#include "diaglog_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *out = getenv("OUT");
    char path[512], line[2048];
    int i, reports = 0, deltaOk = 0, backendOk = 0;

    snprintf(path, sizeof path, "%s/diag-vulkan.log", out ? out : ".");
    logf = fopen(path, "w");
    fakeState.diag.active = LORIE_ACTIVE_VULKAN;
    fakeState.backend.requested = LORIE_BACKEND_VULKAN;
    fakeState.diag.xOwnership = LORIE_OWNERSHIP_PER_ACCESS;
    fakeState.diag.xOwnershipTransitions = 1;
    fakeState.diag.vkInstanceCreated = fakeState.diag.vkDeviceCreated = 1;
    fakeState.diag.vkSurfaceCreated = fakeState.diag.vkSwapchainCreated = 1;
    for (i = 0; i < 4; i++) {
        fakeState.diag.vkAhbImportAttempts += 2; fakeState.diag.vkAhbImportSuccess += 2;
        fakeState.diag.vkImportedSourceDraws += 300; fakeState.diag.vkImportedDestinationDraws += 50;
        fakeState.diag.vkRenderCommands += 350; fakeState.diag.vkQueueSubmits += 300;
        fakeState.diag.vkQueuePresentSuccess += 300; fakeState.diag.vkFramesPresented += 300;
        fakeState.diag.vkAcquireSuccess += 300; fakeState.diag.vkFenceWaitUs += 900000;
        fakeState.diag.xAhbCpuAcquires += 120; fakeState.diag.xStateLockWaitUs += 4000;
        lorieLogRendererDiag();
    }
    fclose(logf);
    logf = fopen(path, "r");
    while (fgets(line, sizeof line, logf))
        if (strstr(line, "XlorieBackend:") && strstr(line, "x_ahb_cpu_acquires=480(+120)")) {
            CHECK(strstr(line, "XlorieBackend: requested=vulkan vk_fail_at=none active=VULKAN ownership=per_access "
                               "ownership_transitions=1 ")
                  && strstr(line, "x_state_lock_wait_us=16000(+4000)"), "vulkan backend line: %s", line);
            backendOk++;
        }
    fclose(logf);
    CHECK(backendOk == 1, "vulkan log: %d backend lines with the expected X ownership counters", backendOk);

    snprintf(path, sizeof path, "%s/diag-gles.log", out ? out : ".");
    logf = fopen(path, "w");
    fakeState.diag.active = LORIE_ACTIVE_GLES;
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
            CHECK(strstr(line, "XlorieBackend: requested=vulkan vk_fail_at=none active=GLES ownership=persistent "
                               "ownership_transitions=2 "),
                  "gles backend line: %s", line);
        if (strstr(line, "XlorieFallback:"))
            CHECK(strstr(line, "last_reason=VK_DEVICE_LOST") && strstr(line, "reasons=VK_DEVICE_LOST:1"),
                  "fallback line without its reason: %s", line);
    }
    fclose(logf);
    CHECK(reports == 4 && deltaOk == 1, "gles log: %d reports, %d with the expected total and delta", reports, deltaOk);
    printf("TDIAGLOG renderer diagnostics written by the real formatter: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
