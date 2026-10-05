#!/usr/bin/env python3
"""Checks what tools/model/zcslots.py says about pacing the root layer, and that its checks are not vacuous.

  - no policy, at 60 or 120 Hz, in any scenario, gives back a slot the compositor still has, leaks a held
    bit, or lets the X server draw into or move on to such a slot; nor across stopping, pool replacement
    and surface teardown once a hidden slot waits for the hide frame's present fence;
  - without that, a renderer frame between the hide's callback and the vsync gives back a slot still on
    screen - under every policy, the current one included;
  - 5 slots with OnCommit pacing (C) cannot hold a frame-deep pipeline through a completion or release
    fence a frame late, and with the room check moved to the apply the X server is left one slot;
  - 6 slots with backpressure loses nothing beyond the unavoidable; bounded by OnCommit (D) it fails only
    when OnCommit is late (from 4 ms at 120 Hz, 8 ms at 60 Hz), unbounded (D-nocap) only by latency
    growing when the compositor skips a latch or the client outruns it;
  - each seeded rule change is caught: no wait for the release fence, room for one fewer, an X server
    ignoring held bits, the release fence given to the frame's last callback, completions matched by slot
    index, transaction numbers restarting with each SurfaceControl.

usage: tzcslots.py <zcslots.py>
"""
import importlib.util, sys

spec = importlib.util.spec_from_file_location("zcslots", sys.argv[1])
Z = importlib.util.module_from_spec(spec)
sys.modules["zcslots"] = Z
spec.loader.exec_module(Z)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

def failing(out):
    return sorted(n for n, (v, r, why) in out.items() if v or why)

results = {}
for hz in (60, 120):
    T = Z.Timing(hz)
    for name, pol in Z.POLICIES.items():
        out = Z.runAll(T, pol, Z.scenarios(T), strict=name != "A")
        results[(hz, name)] = out
        viol = [(n, v) for n, (v, r, w) in out.items() if v]
        check(not viol, "%d Hz %s: violations %s" % (hz, name, viol))
        life = Z.runAll(T, pol, Z.lifecycle(T), fixHide=True)
        check(not [n for n, (v, r, w) in life.items() if v], "%d Hz %s lifecycle: violations %s" % (
            hz, name, [(n, v) for n, (v, r, w) in life.items() if v]))
        bare = Z.runAll(T, pol, Z.lifecycle(T))
        mid = "stop presenting, a renderer frame between the hide's callback and the vsync"
        check(bare[mid][0] is not None, "%d Hz %s: a hidden slot given back on a callback with no fence went unnoticed" % (hz, name))

LATE = ["OnComplete a frame late", "release fences a frame late", "OnComplete and release fences a frame late"]
for hz in (60, 120):
    c = failing(results[(hz, "C-apply")])
    check(all(n in c for n in LATE), "%d Hz C-apply: passes a frame-late completion or fence (%s)" % (hz, c))
    late = results[(hz, "C-late")]
    check(all(n in failing(late) for n in LATE), "%d Hz C-late: passes a frame-late completion or fence" % hz)
    check(min(r["xAvailMin"] for v, r, w in late.values() if r) == 1, "%d Hz C-late: the X server never down to 1 slot" % hz)
    nocap = results[(hz, "D-nocap")]
    check(failing(nocap) == ["client faster than the display", "compositor skips a latch every 20 vsyncs"],
          "%d Hz D-nocap: fails %s" % (hz, failing(nocap)))
    check(all(r["dropped"] + r["skipped"] <= r["unavoidable"] for v, r, w in nocap.values() if r),
          "%d Hz D-nocap: loses frames beyond the unavoidable" % hz)
want = {60: ["OnCommit a frame late, ticks 100-159"],
        120: ["OnCommit 0.2-6 ms late, slow frames every 10 ticks", "OnCommit a frame late, ticks 100-159"]}
for hz in (60, 120):
    check(failing(results[(hz, "D")]) == want[hz], "%d Hz D: fails %s, want %s" % (hz, failing(results[(hz, "D")]), want[hz]))

# where OnCommit lateness starts to cost frames: the last delay that is still fine
for hz, policy, last in ((60, "C-apply", 8000), (120, "C-apply", 4000), (60, "D", 6000), (120, "D", 3000),
                         (60, "D-nocap", 16700), (120, "D-nocap", 16700)):
    rows = Z.sweep(Z.Timing(hz), Z.POLICIES[policy])
    okUpTo = max([d for d, a, b, why in rows if not why] or [0])
    firstBad = min([d for d, a, b, why in rows if why] or [99999])
    check(okUpTo == last and firstBad > last, "%d Hz %s: fine up to %d us, first failing at %d; want fine up to %d" % (
        hz, policy, okUpTo, firstBad, last))

# seeded rule changes
T = Z.Timing(60)
for bug in ("no-fence-wait", "room-plus-one", "x-ignores-held"):
    for name in ("A", "D-nocap"):
        out = Z.runAll(T, Z.POLICIES[name], Z.scenarios(T), (bug,))
        check(any(v for v, r, w in out.values()), "seeded %s in %s: no violation" % (bug, name))
mixed = [s for s in Z.scenarios(T) if s.name == "slow frames every 10 ticks, release fences 0-2 ms late"]
check(Z.runAll(T, Z.POLICIES["A"], mixed, ("fence-to-last",))[mixed[0].name][0] is not None,
      "seeded fence-to-last in A: no violation")
def resize(r):
    return Z.Scenario("resize", work=lambda T_, k: T_.SLOW if k % 7 == 3 else T_.WORK, resizeAt=r,
                      cbLate=lambda k: 3 * T.P if r - 3 <= k < r else 0)
def violations(policy, scs, bugs=(), fixHide=False):
    n = 0
    for sc in scs:
        try:
            Z.Sim(T, Z.POLICIES[policy], sc, bugs, fixHide).run()
        except Z.Violation:
            n += 1
    return n
check(violations("D-nocap", [resize(r) for r in range(95, 125)]) == 0, "pool replacement: a violation as designed")
check(violations("D-nocap", [resize(r) for r in range(95, 125)], ("match-by-index",)) > 0,
      "seeded match-by-index: no violation")
teardown = Z.Scenario("teardown", work=lambda T_, k: T_.SLOW if (k == 90 or (k > 104 and k % 7 == 3)) else T_.WORK,
                      teardownAt=100, resumeAt=104, cbLate=lambda k: 3 * T.P if 95 <= k < 105 else 0,
                      commitDelay=lambda k: 3 * T.P if 95 <= k < 105 else Z.COMMIT_DELAY)
a = Z.Sim(T, Z.POLICIES["C-apply"], teardown, (), True).run()
b = Z.Sim(T, Z.POLICIES["C-apply"], teardown, ("seq-restart",), True).run()
check(a["dropped"] == 0 and b["dropped"] > 0, "seeded seq-restart: dropped %d, as designed %d" % (b["dropped"], a["dropped"]))

print("root slot model (5 policies, 60/120 Hz, lifecycle): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
