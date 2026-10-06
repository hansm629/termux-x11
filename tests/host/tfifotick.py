#!/usr/bin/env python3
"""Checks tools/model/fifotick.py (C2): a FIFO-paced client's frame against the vsync, by dependency graph,
over symbolic cost ranges, at 60 and 120 Hz.

  - the current multi-root with a carry (B) is never better than 3c98ffa (A) and is worse somewhere at
    both rates, far more at 120 Hz; the most it adds is bounded by what the carry puts on the path (the
    carry and root draw fence the X server ends up waiting for, the handover's tick work, a take-back);
  - carrying in the same batch (C) does not remove it;
  - multi-root without a carry (D) is never worse than A.

What it comes from, step by step: with the carry free on the GPU and the handover free, B is still worse
- where the X server gets the lock first and takes the carry back on the CPU; with that free as well, B is
A. And the EXA preflight is not it: without it B is not better (it only trades waiting for the GPU's carry
for making it with the CPU).

usage: tfifotick.py <fifotick.py>
"""
import importlib.util, sys

spec = importlib.util.spec_from_file_location("fifotick", sys.argv[1])
F = importlib.util.module_from_spec(spec)
sys.modules["fifotick"] = F
spec.loader.exec_module(F)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

res = {}
for hz in (60, 120):
    period = 1000.0 / hz
    rows = F.sweep(period)
    s = F.summarise(rows)
    res[hz] = s
    check(s["B_better_than_A"] == 0 and s["B_worse_than_A"] > 0, "%d Hz B: worse %d, better %d" % (hz, s["B_worse_than_A"], s["B_better_than_A"]))
    bound = max(F.GRID["gk"]) + max(F.GRID["gr"]) + max(F.GRID["h"]) - min(F.GRID["hA"]) + max(F.GRID["tb"])
    check(s["B_extra_ms_max"] <= bound + 1e-9, "%d Hz B up to %.2f ms later, beyond %.2f" % (hz, s["B_extra_ms_max"], bound))
    check(s["C_worse_than_A"] > 0, "%d Hz C: never worse than A" % hz)
    check(s["D_worse_than_A"] == 0, "%d Hz D: worse than A in %d" % (hz, s["D_worse_than_A"]))
check(res[120]["B_worse_than_A"] > 5 * res[60]["B_worse_than_A"],
      "B worse than A: 120 Hz %d vs 60 Hz %d" % (res[120]["B_worse_than_A"], res[60]["B_worse_than_A"]))

# controls: the carry's costs taken away one at a time
gpu_free = dict(F.GRID, gk=(0.0,), gr=(0.0,), h=(0.05,))
s0 = F.summarise(F.sweep(1000.0 / 120, grid=gpu_free))
check(s0["B_worse_than_A"] > 0, "control: with the carry free on the GPU B is no longer worse - the take-back is not modelled")
all_free = dict(gpu_free, tb=(0.0,))
s1 = F.summarise(F.sweep(1000.0 / 120, grid=all_free))
check(s1["B_worse_than_A"] == 0 and s1["B_better_than_A"] == 0,
      "control: with the carry free everywhere B differs from A: worse %d, better %d" % (s1["B_worse_than_A"], s1["B_better_than_A"]))
saved = F.PREFLIGHT_CARRY_MS
F.PREFLIGHT_CARRY_MS = 0.0                    # no waiting for a queue of carries
s2 = F.summarise(F.sweep(1000.0 / 120))
F.PREFLIGHT_CARRY_MS = saved
check(s2["B_worse_than_A"] >= res[120]["B_worse_than_A"],
      "control: without the preflight B is worse in %d, with it %d - the preflight would be the cause" % (
          s2["B_worse_than_A"], res[120]["B_worse_than_A"]))

print("C2 FIFO frame against the vsync (fifotick): %s (%d failures; B worse than A in %d at 60 Hz, %d at 120 Hz of %d; D never)" % (
    "FAIL" if fails else "PASS", fails, res[60]["B_worse_than_A"], res[120]["B_worse_than_A"], res[120]["points"]))
sys.exit(1 if fails else 0)
