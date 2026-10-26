/* GPU Performance Controller policy. No Android, GPU vendor or graphics-API dependencies.
 *
 * This policy intentionally does NOT write governor/cpuset/sysfs state. It only schedules
 * an optional, tiny EGL pbuffer workload in the *app's* own process, based on visible
 * frames and the helper thread's measured CPU time.
 *
 * Shared by the runtime controller and the native host regression tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
    GPU_PERF_OFF = 0,
    GPU_PERF_BALANCED = 1,
    GPU_PERF_PERFORMANCE = 2,
    GPU_PERF_MAXIMUM = 3
};

struct gpu_perf_limits {
    unsigned min_sleep_us;
    unsigned max_sleep_us;
    unsigned cpu_budget_percent;
};

static inline struct gpu_perf_limits gpuPerfLimits(int mode) {
    switch (mode) {
        case GPU_PERF_BALANCED:    return (struct gpu_perf_limits) {250, 3000, 12};
        case GPU_PERF_PERFORMANCE: return (struct gpu_perf_limits) {50, 2000, 28};
        case GPU_PERF_MAXIMUM:     return (struct gpu_perf_limits) {0, 0, 100};
        default:                   return (struct gpu_perf_limits) {0, 0, 0};
    }
}

/* A 120 Hz on-screen compositor wakes more frequently than a 60 Hz DeX
 * session. Allow a modestly tighter GPU-warmup budget at high refresh without
 * spinning an entire CPU core. Never use vendor or device-model heuristics. */
static inline struct gpu_perf_limits gpuPerfLimitsForRefresh(int mode, unsigned refresh_hz) {
    struct gpu_perf_limits limits = gpuPerfLimits(mode);
    if (refresh_hz >= 90) {
        if (mode == GPU_PERF_BALANCED)
            return (struct gpu_perf_limits) {160, 2500, 16};
        if (mode == GPU_PERF_PERFORMANCE)
            return (struct gpu_perf_limits) {25, 1600, 36};
    }
    return limits;
}

/* Severe / critical / emergency / shutdown: stop synthetic work immediately, without
 * attempting to alter the device's own thermal power policy. Android's thermal constants
 * are monotonic and THERMAL_STATUS_SEVERE is 4 on supported SDKs. */
static inline bool gpuPerfThermalAllows(int android_thermal_status) {
    return android_thermal_status < 4;
}

static inline bool gpuPerfShouldWork(int mode, bool foreground, int thermal,
                                     uint64_t now_ns, uint64_t last_frame_ns) {
    if (mode <= GPU_PERF_OFF || mode > GPU_PERF_MAXIMUM || !foreground ||
        !gpuPerfThermalAllows(thermal) || !last_frame_ns || now_ns < last_frame_ns)
        return false;
    /* A headless, invisible or idle desktop must not keep its GPU awake. */
    return now_ns - last_frame_ns < UINT64_C(1100000000);
}

/* Bounded feedback regulator. sleep_us is the delay AFTER an EGL clear + glFinish,
 * not a busy-loop wait. The CPU budget measures only the helper's CPU time;
 * the driver's internal GPU work/power cannot be inferred from this metric. */
static inline unsigned gpuPerfNextSleepForRefresh(int mode, unsigned refresh_hz,
                                                  unsigned sleep_us,
                                                  unsigned cpu_percent) {
    struct gpu_perf_limits limits = gpuPerfLimitsForRefresh(mode, refresh_hz);
    unsigned step;
    if (!limits.cpu_budget_percent || mode == GPU_PERF_MAXIMUM)
        return limits.min_sleep_us;
    if (sleep_us < limits.min_sleep_us)
        sleep_us = limits.min_sleep_us;
    if (sleep_us > limits.max_sleep_us)
        sleep_us = limits.max_sleep_us;

    if (cpu_percent > limits.cpu_budget_percent + 3) {
        step = sleep_us / 3 + 50;
        return step > limits.max_sleep_us - sleep_us ?
               limits.max_sleep_us : sleep_us + step;
    }
    if (cpu_percent + 6 < limits.cpu_budget_percent &&
        sleep_us > limits.min_sleep_us) {
        step = sleep_us / 4 + 25;
        return step > sleep_us - limits.min_sleep_us ?
               limits.min_sleep_us : sleep_us - step;
    }
    return sleep_us;
}

static inline unsigned gpuPerfNextSleep(int mode, unsigned sleep_us,
                                        unsigned cpu_percent) {
    return gpuPerfNextSleepForRefresh(mode, 60, sleep_us, cpu_percent);
}
