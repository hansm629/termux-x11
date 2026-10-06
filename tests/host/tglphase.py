#!/usr/bin/env python3
"""Checks tools/model/glphase.py (C1): with the renderer answering every signal within the tick and the
compositor after the GL frame,

  - with the renderer never first between G and H (K1), every structure shows every frame once, at a
    constant latency;
  - letting it be (K2), the current multi-root GL output (B) repeats, skips and leaves publishes unshown,
    while the single live root (A) keeps one constant latency and the multi-root with the gate opened
    after the publish (C) shows nothing of it; there is a concrete interleaving that does it;
  - the composite landing before the GL frame (K3) is A's own race - and C has none of it either;
  - a composite a tick late (K4) costs every structure, but C never where A does not.

And that this is not vacuous: C with its gate back before the publish, and A drawing a published snapshot
instead of the live root, both fail where B fails; without the G..H wake, B does not.

usage: tglphase.py <glphase.py>
"""
import importlib.util, sys

spec = importlib.util.spec_from_file_location("glphase", sys.argv[1])
G = importlib.util.module_from_spec(spec)
sys.modules["glphase"] = G
spec.loader.exec_module(G)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

TICKS, WARM = 8, 3
S = ("A", "B", "C")
res = {c: G.summarise(G.explore(TICKS, WARM, S, cls=c), S) for c in ("K1", "K2", "K3")}
k4 = G.summarise(G.explore(7, WARM, S, cls="K4"), S)

for name in S:
    check(res["K1"][name + "_defective"] == 0 and res["K1"][name + "_latencies"] == [2],
          "K1 %s: %d defective, latencies %s" % (name, res["K1"][name + "_defective"], res["K1"][name + "_latencies"]))
k2 = res["K2"]
check(k2["A_defective"] == 0 and k2["A_latencies"] == [2], "K2 A: %d defective, latencies %s" % (k2["A_defective"], k2["A_latencies"]))
check(k2["B_defective"] > 0 and k2["B_repeat"] > 0 and k2["B_skip"] > 0 and k2["B_unshown"] > 0,
      "K2 B: no repeat/skip/unshown (%d/%d/%d)" % (k2["B_repeat"], k2["B_skip"], k2["B_unshown"]))
check(k2["C_defective"] == 0 and k2["C_latencies"] == [2], "K2 C: %d defective, latencies %s" % (k2["C_defective"], k2["C_latencies"]))
w = G.witness(4, lambda acc: G.defective(acc[1]) and not G.defective(acc[0]) and not G.defective(acc[2]), cls="K2")
check(w is not None and any(r[1] for r, xc in w), "K2: no interleaving with B defective and A, C clean that wakes between G and H")
k3 = res["K3"]
check(k3["A_defective"] > 0 and k3["C_defective"] == 0, "K3: A %d, C %d defective" % (k3["A_defective"], k3["C_defective"]))
check(k4["C_defective_where_A_clean"] == 0, "K4: C defective where A is clean in %d" % k4["C_defective_where_A_clean"])

# --- negative controls ---------------------------------------------------------------------------------
# C with the gate opened before the publish again is B
saved = dict(G.ORDER)
G.ORDER["C"] = ("VP", "G", "H", "S")
nc1 = G.summarise(G.explore(TICKS, WARM, S, cls="K2"), S)
G.ORDER.clear(); G.ORDER.update(saved)
check(nc1["C_defective"] == nc1["B_defective"] > 0, "control: C with the gate before the publish has %d defective (B %d)" % (
    nc1["C_defective"], nc1["B_defective"]))
# A drawing a published snapshot, not the live root (structure "A" run through the multi-root rules)
saved_r, saved_x, saved_c = G.r_step, G.x_step, G.c_step
G.r_step = lambda s, st, m, v: saved_r("B" if s == "A" else s, st, m, v)
G.x_step = lambda s, st, step, k, m: saved_x("B" if s == "A" else s, st, step, k, m)
G.c_step = lambda s, st, m: saved_c("B" if s == "A" else s, st, m)
nc2 = G.summarise(G.explore(TICKS, WARM, S, cls="K2"), S)
G.r_step, G.x_step, G.c_step = saved_r, saved_x, saved_c
check(nc2["A_defective"] > 0, "control: A drawing a snapshot has no defect")
# The race is the wake moving between ticks: always between G and H, B shows every frame a tick later and
# nothing else; mixed with waking after S, it repeats and skips - and A and C do neither
early, late = ((0, 1, 0, 1), 1), ((0, 0, 0, 1), 1)
saved_orders = G.class_orders
G.class_orders = lambda cls, max_r=3: [early]
nc3 = G.summarise(G.explore(TICKS, WARM, S, cls="K2"), S)
G.class_orders = lambda cls, max_r=3: [early, late]
nc4 = G.summarise(G.explore(TICKS, WARM, S, cls="K2"), S)
G.class_orders = saved_orders
check(nc3["B_defective"] == 0 and nc3["B_latencies"] == [3] and nc3["A_latencies"] == [2] and nc3["C_latencies"] == [2],
      "control: always waking between G and H: B %d defective, latencies B %s A %s C %s" % (
          nc3["B_defective"], nc3["B_latencies"], nc3["A_latencies"], nc3["C_latencies"]))
check(nc4["B_defective"] > 0 and nc4["A_defective"] == 0 and nc4["C_defective"] == 0,
      "control: waking between G and H or after S: B %d, A %d, C %d defective of %d" % (
          nc4["B_defective"], nc4["A_defective"], nc4["C_defective"], nc4["total"]))

print("C1 GL-path publish race (glphase, %d ticks): %s (%d failures; K2: B defective in %d of %d, A and C in 0)" % (
    TICKS, "FAIL" if fails else "PASS", fails, k2["B_defective"], k2["total"]))
sys.exit(1 if fails else 0)
