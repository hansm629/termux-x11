#!/usr/bin/env python3
"""Checks what tools/model/zcslots.py says about the root-slot lifecycle under compositor backpressure, and
that its checks are not vacuous.

  - no configuration ever gives back a slot the compositor still has, leaks a held bit, or lets the X
    server move on to a slot the compositor has;
  - 5 slots with backpressure on loses published frames it did not have to once the pipeline is a frame
    deep and a release fence or a callback is late (it has no room to submit), where 6 slots does not;
  - 6 slots with backpressure on meets every criterion in every scenario except that the queue, and the
    latency with it, grows a frame each time the compositor skips a latch or the client outruns it;
    a depth cap known from OnCommit holds that to one frame, and fails only when OnCommit itself is late;
  - each seeded rule change - no wait for the release fence, room for one fewer, an X server that ignores
    the held bits, the release fence given to the frame's last callback rather than its first - is
    caught as a violation.

usage: tzcslots.py <zcslots.py>
"""
import importlib.util, sys

spec = importlib.util.spec_from_file_location("zcslots", sys.argv[1])
M = importlib.util.module_from_spec(spec)
sys.modules["zcslots"] = M
spec.loader.exec_module(M)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

def run(slots, bp, cap=None, bugs=()):
    out = {}
    for sc in M.SCENARIOS:
        try:
            r = M.Sim(slots, bp, sc, bugs, cap).run()
            out[sc.name] = (None, r, M.verdict(r, bp))
        except M.Violation as v:
            out[sc.name] = (str(v), None, None)
    return out

def failing(out):
    return sorted(name for name, (v, r, why) in out.items() if v or why)

configs = {"A": (5, False, None), "B": (5, True, None), "C": (6, True, None), "C+commit": (6, True, "commit")}
results = {name: run(*cfg) for name, cfg in configs.items()}

for name, out in results.items():
    violations = [(n, v) for n, (v, r, why) in out.items() if v]
    check(not violations, "%s: violations %s" % (name, violations))
    for n, (v, r, why) in out.items():
        if r:
            check(r["xAvailMin"] >= 2, "%s / %s: the X server down to %d slots" % (name, n, r["xAvailMin"]))
            check(r["halfRate"] == 0, "%s / %s: %d half-rate episodes" % (name, n, r["halfRate"]))
            check(r["inOrder"], "%s / %s: presented out of order" % (name, n))

LATE = ["callbacks a frame late", "release fences a frame late", "callbacks and release fences a frame late",
        "release fences 0-2 ms after the vsync", "release fence after the claim in 6% of frames",
        "commit and complete callbacks a frame late", "slow frames every 10 ticks, release fences 0-2 ms late"]
check(failing(results["B"]) == sorted(LATE), "5 slots, backpressure on: fails %s, want %s" % (failing(results["B"]), sorted(LATE)))
mixed = "slow frames every 10 ticks, release fences 0-2 ms late"
a, b, c = (results[k][mixed][1] for k in ("A", "B", "C"))
check(a["dropped"] == 27 and b["skipped"] == 27 and c["dropped"] + c["skipped"] == 0,
      "%s: A drops %d, B skips %d, C loses %d; want 27, 27, 0" % (mixed, a["dropped"], b["skipped"], c["dropped"] + c["skipped"]))

GROWS = ["client faster than the display", "compositor skips a latch every 20 vsyncs"]
check(failing(results["C"]) == sorted(GROWS), "6 slots, backpressure on: fails %s, want %s" % (failing(results["C"]), sorted(GROWS)))
for n in GROWS:
    why = results["C"][n][2]
    check(why and all(w.startswith("latency grew") for w in why), "6 slots / %s: fails for %s, want latency only" % (n, why))
check(failing(results["C+commit"]) == ["commit and complete callbacks a frame late"],
      "6 slots with the OnCommit cap: fails %s" % failing(results["C+commit"]))
check(max(r["queuedMax"] for v, r, why in results["C+commit"].values() if r) == 2,
      "6 slots with the OnCommit cap: compositor queue deeper than 2")

# the checks are not vacuous: each seeded rule change is a violation somewhere
for bug, cfgs in (("no-fence-wait", ("A", "C")), ("room-plus-one", ("A", "C")),
                  ("x-ignores-held", ("A", "C")), ("fence-to-last", ("A",))):
    for name in cfgs:
        slots, bp, cap = configs[name]
        out = run(slots, bp, cap, (bug,))
        check(any(v for v, r, why in out.values()), "seeded %s in %s: no violation" % (bug, name))

print("root slot model (backpressure off/on, 5/6 slots): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
