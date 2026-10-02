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
}
for name, make in recipes.items():
    with open(os.path.join(OUT, name + "_src.inc"), "w") as f:
        f.write(make())
