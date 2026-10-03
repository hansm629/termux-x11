#!/usr/bin/env python3
"""Reads a TERMUX_X11_TRACE file and says where frames waited.

usage: analyze.py trace.bin [--stalls N] [--window MS]

All times are CLOCK_MONOTONIC microseconds from both the X server and the renderer, so steps from the
two processes line up. Nothing here is a measurement of what reached the glass: the last step recorded
is the renderer handing a buffer to the compositor (or swapping a GL frame).
"""
import struct, sys, argparse
from collections import defaultdict

KIND = {1: "REQUEST", 2: "ENQUEUE", 3: "RESOLVED", 4: "PUBLISH", 5: "TICK", 6: "INPUT",
        7: "DRAIN", 8: "FENCE", 9: "DIRECT", 10: "GLSWAP", 11: "RELEASE", 12: "HOLD",
        13: "CANCEL", 14: "PREFLIGHT", 15: "XLOCK", 16: "ROOTCOPY", 17: "REMAP", 18: "RLOCK"}
HOLD = {1: "frame incomplete", 2: "no slot back from compositor", 3: "shared lock unusable"}
PREFLIGHT = {1: "drained", 2: "timed out", 3: "skipped, lock already held", 4: "skipped, head stuck",
             5: "skipped, no renderer"}
MIN_WAIT_MS = 0.1   # LORIE_TRACE_MIN_WAIT_US: shorter lock waits are not recorded

def spans(recs):
    """Each timed step as (start, end, kind, record): what each record's duration covers."""
    out = []
    for t, k, a, b in recs:
        if k == 14:   out.append((t - b, t, "preflight", (t, k, a, b)))    # b = wait us, recorded at its end
        elif k == 15: out.append((t - a, t, "xlock", (t, k, a, b)))        # a = wait us, recorded once taken
        elif k == 16: out.append((t - a, t, "rootcopy", (t, k, a, b)))     # a = us, recorded at its end
        elif k == 17: out.append((t - a, t, "remap", (t, k, a, b)))
        elif k == 8:  out.append((t - a, t, "fence", (t, k, a, b)))        # a = wait us, recorded at its end
        elif k == 18:                                                       # dated when the lock was taken
            out.append((t - a, t, "rlockwait", (t, k, a, b)))
            out.append((t, t + b, "rlockheld", (t, k, a, b)))
    return out

def overlap(s, e, t1, t2):
    return max(0, min(e, t2) - max(s, t1))

def breakdown(sp, recs, t1, t2):
    """Where the time inside [t1, t2] went, per step, and what happened in it."""
    ms = defaultdict(float); n = defaultdict(int); extra = defaultdict(int)
    for s, e, what, r in sp:
        o = overlap(s, e, t1, t2)
        if o > 0 or (s == e and t1 <= s <= t2):
            ms[what] += o / 1000.0
            n[what] += 1
            if what == "preflight" and r[2] == 2:
                extra["preflight timeouts"] += 1
            if what == "rootcopy":
                extra["rootcopy bytes"] += r[3]
    ev = defaultdict(int); holds = defaultdict(int)
    for t, k, a, b in recs:
        if t1 <= t <= t2:
            ev[k] += 1
            if k == 12: holds[HOLD.get(a, a)] += 1
    return ms, n, extra, ev, holds

def load(path):
    data = open(path, "rb").read()
    if data[:4] != b"LTR1":
        sys.exit("not a trace file")
    size, start = struct.unpack_from("<IQ", data, 4)
    recs = []
    for off in range(16, len(data) - size + 1, size):
        t, k, a, b = struct.unpack_from("<QIIQ", data, off)
        recs.append((t, k, a, b))
    recs.sort(key=lambda r: r[0])
    return start, recs

def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]

def fmt(v):
    return "n=%d p50 %.1f p95 %.1f p99 %.1f max %.1f ms" % (
        len(v), pct(v, 50) / 1000, pct(v, 95) / 1000, pct(v, 99) / 1000, (max(v) if v else 0) / 1000)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--stalls", type=int, default=5, help="how many of the longest output gaps to show")
    ap.add_argument("--window", type=float, default=100, help="input within this many ms counts as dragging")
    args = ap.parse_args()
    start, recs = load(args.trace)
    if not recs:
        sys.exit("trace is empty")
    t0 = recs[0][0]
    rel = lambda t: (t - t0) / 1000.0

    counts = defaultdict(int)
    for r in recs:
        counts[r[1]] += 1
    print("== %d records over %.1f s" % (len(recs), (recs[-1][0] - t0) / 1e6))
    print("   " + ", ".join("%s %d" % (KIND.get(k, k), n) for k, n in sorted(counts.items())))

    # copy lifecycle by serial
    enq, drain, fence, res = {}, [], [], {}
    for t, k, a, b in recs:
        if k == 2: enq[b] = t
        elif k == 7: drain.append((t, b))
        elif k == 8: fence.append((t, b))
        elif k == 3: res[b] = (t, a)
    def first_at_or_after(events, serial):
        for t, s in events:
            if s >= serial:
                return t
        return None
    toDrain, toFence, toResolve = [], [], []
    for s, t in enq.items():
        d = first_at_or_after(drain, s); f = first_at_or_after(fence, s)
        if d: toDrain.append(d - t)
        if f: toFence.append(f - t)
        if s in res: toResolve.append(res[s][0] - t)
    notMade = sum(1 for v in res.values() if v[1] == 0)
    print("\n== present copies (%d enqueued, %d resolved, %d not made)" % (len(enq), len(res), notMade))
    print("   enqueue -> renderer drained it : " + fmt(toDrain))
    print("   enqueue -> GPU finished        : " + fmt(toFence))
    print("   enqueue -> X server resolved   : " + fmt(toResolve))

    # output cadence, split by whether input was arriving
    inputs = [t for t, k, a, b in recs if k == 6]
    outs = [(t, k) for t, k, a, b in recs if k in (9, 10)]
    import bisect
    def dragging(t):
        i = bisect.bisect_right(inputs, t)
        return i > 0 and t - inputs[i - 1] <= args.window * 1000
    gapsDrag, gapsIdle = [], []
    for (t1, _), (t2, _) in zip(outs, outs[1:]):
        (gapsDrag if dragging(t2) else gapsIdle).append(t2 - t1)
    print("\n== gaps between frames handed to the screen (direct submits and GL swaps)")
    print("   while input arriving : " + fmt(gapsDrag))
    print("   otherwise            : " + fmt(gapsIdle))
    pubs = [t for t, k, a, b in recs if k == 4]
    pubDrag = [b - a for a, b in zip(pubs, pubs[1:]) if dragging(b)]
    pubIdle = [b - a for a, b in zip(pubs, pubs[1:]) if not dragging(b)]
    print("== gaps between root publishes by the X server")
    print("   while input arriving : " + fmt(pubDrag))
    print("   otherwise            : " + fmt(pubIdle))
    holds = defaultdict(int)
    for t, k, a, b in recs:
        if k == 12: holds[a] += 1
    if holds:
        print("== frames held back: " + ", ".join("%s %d" % (HOLD.get(a, a), n) for a, n in holds.items()))

    # waits and costs over the whole trace, by step
    sp = spans(recs)
    if sp:
        print("\n== timed steps over the whole trace (lock waits under %.1f ms are not recorded)" % MIN_WAIT_MS)
        names = [("preflight", "X: EXA preflight wait     "), ("xlock", "X: shared lock wait       "),
                 ("rootcopy", "X: root CPU copy          "), ("remap", "X: root remap             "),
                 ("rlockwait", "renderer: shared lock wait"), ("rlockheld", "renderer: holding the lock"),
                 ("fence", "renderer: GPU fence wait  ")]
        for what, label in names:
            v = [e - s for s, e, w, r in sp if w == what]
            if v:
                print("   %s: %s" % (label, fmt(v)))
        res = defaultdict(int)
        for t, k, a, b in recs:
            if k == 14: res[PREFLIGHT.get(a, a)] += 1
        if res:
            print("   preflight results: " + ", ".join("%s %d" % (w, c) for w, c in res.items()))
        cancels = sum(1 for r in recs if r[1] == 13)
        if cancels:
            print("   queued copies cancelled for an overlapping CPU write: %d" % cancels)

    # the longest output gaps, with what happened inside them
    worst = sorted(zip(outs, outs[1:]), key=lambda p: p[1][0] - p[0][0], reverse=True)[:args.stalls]
    for (t1, _), (t2, _) in worst:
        print("\n== output gap %.1f ms at %.1f ms (%s)" % ((t2 - t1) / 1000, rel(t1), "input arriving" if dragging(t2) else "no input"))
        ms, n, extra, ev, holds = breakdown(sp, recs, t1, t2)
        def part(what, label):
            if not n[what]:
                return None
            s = "%s %.1f ms (%d" % (label, ms[what], n[what])
            if what == "preflight" and extra["preflight timeouts"]:
                s += ", %d timed out" % extra["preflight timeouts"]
            if what == "rootcopy":
                s += ", %.2f MB" % (extra["rootcopy bytes"] / 1048576.0)
            return s + ")"
        xs = [p for p in (part("preflight", "preflight waits"), part("xlock", "shared lock waits"),
                          part("rootcopy", "root CPU copies"), part("remap", "root remaps")) if p]
        rs = [p for p in (part("rlockwait", "shared lock waits"), part("rlockheld", "holding the lock"),
                          part("fence", "GPU fence waits")) if p]
        print("   X server : " + (", ".join(xs) if xs else "no timed step recorded"))
        print("   renderer : " + (", ".join(rs) if rs else "no timed step recorded"))
        evs = [(6, "input"), (1, "requests"), (2, "copies queued"), (13, "cancelled"), (7, "drains"),
               (4, "root publishes"), (9, "direct submits"), (10, "GL swaps"), (11, "slots released"), (5, "ticks")]
        line = ", ".join("%s %d" % (name, ev[k]) for k, name in evs if ev[k])
        if holds:
            line += "; held back: " + ", ".join("%s %d" % (w, c) for w, c in holds.items())
        print("   events   : " + (line or "none"))
        print("   (the two threads overlap, and the gap ends at the next direct submit or GL swap - not at the glass)")
        for t, k, a, b in recs:
            if t1 <= t <= t2 and k != 6:
                extra = HOLD.get(a, "") if k == 12 else ""
                print("   %10.1f  %-8s a=%-6d b=%-8d %s" % (rel(t), KIND.get(k, k), a, b, extra))
        n = sum(1 for t in inputs if t1 <= t <= t2)
        if n:
            print("   (%d input events in this gap)" % n)

if __name__ == "__main__":
    main()
