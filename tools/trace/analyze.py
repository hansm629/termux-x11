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
        7: "DRAIN", 8: "FENCE", 9: "DIRECT", 10: "GLSWAP", 11: "RELEASE", 12: "HOLD"}
HOLD = {1: "frame incomplete", 2: "no slot back from compositor", 3: "shared lock unusable"}

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

    # the longest output gaps, with what happened inside them
    worst = sorted(zip(outs, outs[1:]), key=lambda p: p[1][0] - p[0][0], reverse=True)[:args.stalls]
    for (t1, _), (t2, _) in worst:
        print("\n== output gap %.1f ms at %.1f ms (%s)" % ((t2 - t1) / 1000, rel(t1), "input arriving" if dragging(t2) else "no input"))
        for t, k, a, b in recs:
            if t1 <= t <= t2 and k != 6:
                extra = HOLD.get(a, "") if k == 12 else ""
                print("   %10.1f  %-8s a=%-6d b=%-8d %s" % (rel(t), KIND.get(k, k), a, b, extra))
        n = sum(1 for t in inputs if t1 <= t <= t2)
        if n:
            print("   (%d input events in this gap)" % n)

if __name__ == "__main__":
    main()
