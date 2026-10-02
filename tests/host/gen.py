#!/usr/bin/env python3
"""Writes each test's *_src.inc from the sources as they are now, so the tests exercise the real code.

usage: gen.py <lorie source dir> <output dir>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import func, macro, span

L, OUT = sys.argv[1], sys.argv[2]
I, R, H = L + "/InitOutput.c", L + "/renderer.c", L + "/lorie.h"

recipes = {
    "t07": lambda: macro(H, "LORIE_GPU_COPY_FAILED_SLOTS")
        + func(R, "static void rendererPublishFailedSerial(uint64_t serial) {")
        + func(I, "static Bool lorieGpuCopyKnownNotMade(uint64_t serial) {")
        + func(I, "Bool lorieGpuCopyMade(uint64_t serial) {")
        + func(I, "Bool lorieGpuCopyResolved(uint64_t serial) {"),
    "t19": lambda: macro(H, "LORIE_ROOT_SLOTS") + macro(H, "LORIE_ROOT_NEWEST_SHIFT") + macro(H, "LORIE_ROOT_NEWEST_MASK")
        + "static int rendererRootSlot = -1;\nstatic uint64_t rendererRootSlotId = 0;\n"
        + func(R, "static uint64_t rendererClaimRootBuffer(void) {")
        + func(R, "static void rendererReleaseRootSlot(int slot, uint64_t bufferId) {")
        + func(R, "static void rendererReleaseRootBuffer(void) {"),
    "t21": lambda: span(I, "#define LORIE_VSYNC_RECORDS 16", "static uint64_t lorieVsyncPeriodUs = 16667;")
        + func(I, "static uint32_t lorieAdvanceVsyncClock(void) {"),
    "tstat": lambda: macro(H, "LORIE_STAT_MAX"),
    "t05": lambda: macro(H, "LORIE_ROOT_SLOTS") + macro(H, "LORIE_ROOT_HELD_MASK") + macro(H, "LORIE_ROOT_NEWEST_SHIFT")
        + macro(H, "LORIE_ROOT_NEWEST_MASK") + macro(H, "LORIE_ROOT_COUNT_STEP")
        + span(I, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + func(I, "static void lorieCopyRootRegion(LoriePixmapPriv *priv, int from, int to, RegionPtr region) {")
        + func(I, "static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region) {")
        + func(I, "static Bool lorieRepairRootOwed(LoriePixmapPriv *priv) {")
        + func(I, "static RegionPtr lorieRootPendingGpuRegion(LoriePixmapPriv *priv, int slot) {")
        + func(I, "static Bool lorieRootHandover(LoriePixmapPriv *priv) {"),
    "ttrace_src_types": lambda: span(H, "typedef struct {\n    volatile uint64_t seq;", "#define LORIE_TRACE_RECORDS 4096")
        + "struct lorie_shared_server_state { volatile uint8_t traceEnabled; volatile uint64_t traceHead;"
          " volatile uint64_t traceTail; volatile uint32_t traceDropped; LorieTraceRecord trace[LORIE_TRACE_RECORDS]; };\n"
        + func(H, "static inline __always_inline uint64_t lorieTraceNowUs(void) {")
        + func(H, "static inline __always_inline void lorieTraceAt(struct lorie_shared_server_state *st, uint32_t kind,\n"
                  "                                                 uint32_t a, uint64_t b, uint64_t tUs) {"),
    "ttrace_src_funcs": lambda: "static FILE *lorieTraceFile = NULL;\n"
        + func(I, "static void lorieTraceFlush(Bool force) {"),
}
for name, make in recipes.items():
    with open(os.path.join(OUT, (name if name.endswith("_types") or name.endswith("_funcs") else name + "_src") + ".inc"), "w") as f:
        f.write(make())
