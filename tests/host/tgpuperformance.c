#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "../../app/src/main/cpp/lorie/gpu_performance_policy.h"

static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #x); fails++; } } while (0)

int main(void) {
    const uint64_t t = UINT64_C(10000000000);
    CHECK(!gpuPerfShouldWork(GPU_PERF_OFF, true, 0, t, t));
    CHECK(!gpuPerfShouldWork(GPU_PERF_BALANCED, false, 0, t, t));
    CHECK(!gpuPerfShouldWork(GPU_PERF_BALANCED, true, 4, t, t));
    CHECK(!gpuPerfShouldWork(GPU_PERF_BALANCED, true, 0, t, 0));
    CHECK(!gpuPerfShouldWork(GPU_PERF_BALANCED, true, 0, t, t-1100000000));
    CHECK(!gpuPerfShouldWork(GPU_PERF_BALANCED, true, 0, t-1, t));
    CHECK(gpuPerfShouldWork(GPU_PERF_BALANCED, true, 0, t, t-1000000000));
    CHECK(gpuPerfShouldWork(GPU_PERF_PERFORMANCE, true, 3, t, t-200000000));
    CHECK(gpuPerfShouldWork(GPU_PERF_MAXIMUM, true, 0, t, t-1000000));

    struct gpu_perf_limits balanced = gpuPerfLimits(GPU_PERF_BALANCED);
    struct gpu_perf_limits performance = gpuPerfLimits(GPU_PERF_PERFORMANCE);
    CHECK(balanced.min_sleep_us > performance.min_sleep_us);
    CHECK(balanced.cpu_budget_percent < performance.cpu_budget_percent);
    CHECK(gpuPerfLimits(GPU_PERF_MAXIMUM).min_sleep_us == 0);
    CHECK(gpuPerfNextSleep(GPU_PERF_MAXIMUM, 2000, 99) == 0);
    CHECK(gpuPerfNextSleep(GPU_PERF_OFF, 500, 20) == 0);
    CHECK(gpuPerfNextSleep(GPU_PERF_BALANCED, 250, 90) > 250);
    CHECK(gpuPerfNextSleep(GPU_PERF_PERFORMANCE, 1200, 0) < 1200);
    CHECK(gpuPerfNextSleep(GPU_PERF_BALANCED, balanced.max_sleep_us, 99) ==
          balanced.max_sleep_us);
    CHECK(gpuPerfNextSleep(GPU_PERF_PERFORMANCE, 50, 0) == 50);
    CHECK(gpuPerfNextSleep(GPU_PERF_BALANCED, 0, 0) >= balanced.min_sleep_us);
    CHECK(gpuPerfNextSleep(GPU_PERF_BALANCED, 10000, 90) <= balanced.max_sleep_us);
    for (int mode = GPU_PERF_BALANCED; mode <= GPU_PERF_MAXIMUM; mode++) {
        struct gpu_perf_limits limits = gpuPerfLimits(mode);
        unsigned sleep = limits.min_sleep_us;
        for (int i = 0; i < 400; ++i) {
            sleep = gpuPerfNextSleep(mode, sleep, (i % 2) ? 90 : 0);
            CHECK(sleep >= limits.min_sleep_us && sleep <= limits.max_sleep_us);
        }
    }
    printf("TGPUPERFORMANCE idle/foreground/thermal and CPU duty regulator: %s (%d failures)\n",
           fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
