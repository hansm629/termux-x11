#!/usr/bin/env python3
"""Follows each ROOT_DIRECT frame in a trace from the X server's tick to the compositor's latch, and sets
the frames the compositor latched late against the ones it latched on time.

usage: frames.py trace.ltr [--period-ms MS] [--csv FILE]

A frame is one transaction the renderer applied (ZCAPPLY), joined to the rest by sequence numbers, not by
order: the publish it took (ZCCLAIM's publish seq, PUBSEQ), the renderer's frame (ZCBATCH, ZCFENCE), and
the transaction (SFDONE's latch time, SFPRESENT). Its tick is the last VSYNC the X server recorded before
the publish. All times are CLOCK_MONOTONIC; the latch time is the compositor's own, the present time the
present fence's own record (often not given at all - then there is none).

Late: the compositor latched it more than 1.5 refresh periods after the frame before it - a vsync with
nothing new from here in between. The question asked of the late ones is where their time went before the
transaction was applied, and whether they were applied later in their vsync than the on-time ones.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import load, pct

VSYNC, PUBSEQ, ZCCLAIM, ZCBATCH, ZCFENCE, ZCAPPLY, SFDONE, SFPRESENT = range(19, 27)
XLOCK, RLOCK = 15, 18

def join(recs):
    vsyncs, pub, claim, batch, fence, apply_, done, present, xlocks = [], {}, {}, {}, {}, {}, {}, {}, []
    for t, k, a, b in recs:
        if k == VSYNC:      vsyncs.append((t, b))
        elif k == PUBSEQ:   pub[a] = t
        elif k == ZCCLAIM:  claim[a] = (t, b & 0xffffffff, bool(b >> 32))
        elif k == ZCBATCH:  batch[a] = (b >> 32, b & 0xffffffff)
        elif k == ZCFENCE:  fence[a] = (t, b)
        elif k == ZCAPPLY:  apply_[a] = (t, b)
        elif k == SFDONE:   done[a] = (t, b // 1000 if b else None)
        elif k == SFPRESENT: present[a] = t
        elif k == XLOCK:    xlocks.append((t - a, t))
    frames = []
    vi = 0
    for f, (tApply, seq) in sorted(apply_.items(), key=lambda kv: kv[1][0]):
        if f not in claim:
            continue
        tClaim, pseq, already = claim[f]
        if already:
            continue
        tPub = pub.get(pseq)
        tick = None
        if tPub is not None:
            while vi + 1 < len(vsyncs) and vsyncs[vi + 1][0] <= tPub:
                vi += 1
            if vsyncs and vsyncs[vi][0] <= tPub:
                tick = vsyncs[vi][1]
        safe, other = batch.get(f, (0, 0))
        tFence, waitUs = fence.get(f, (None, 0))
        tDone, latch = done.get(seq, (None, None))
        frames.append(dict(frame=f, publish=pseq, apply_seq=seq, vsync=tick, published=tPub, claimed=tClaim,
                           safe_kpx=safe, other_kpx=other, fence_us=waitUs, applied=tApply, completed=tDone,
                           latch=latch, presented=present.get(seq),
                           xlock_us=sum(max(0, min(e, tApply) - max(s, tick)) for s, e in xlocks)
                                    if tick is not None else 0))
    # judged only against the frame just before it: across one whose callback never came, the gap
    # would take in its vsync too
    last = None
    for fr in frames:
        fr["latch_gap"] = fr["latch"] - last if fr["latch"] is not None and last is not None else None
        last = fr["latch"]
    return frames

def ms(v):
    return "%.1f" % (v / 1000.0)

def describe(name, vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "%s -" % name
    return "%s p50 %s p90 %s max %s" % (name, ms(pct(vals, 50)), ms(pct(vals, 90)), ms(max(vals)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--period-ms", type=float, default=0, help="refresh period; from the vsyncs if not given")
    ap.add_argument("--csv")
    args = ap.parse_args()
    start, recs = load(args.trace)
    frames = join(recs)
    if not frames:
        sys.exit("no ROOT_DIRECT frames in this trace")
    period = args.period_ms * 1000
    if not period:
        vs = sorted(set(b for t, k, a, b in recs if k == VSYNC))
        gaps = [y - x for x, y in zip(vs, vs[1:]) if 4000 <= y - x <= 40000]
        period = pct(gaps, 50) if gaps else 16667
    late = [f for f in frames if f["latch_gap"] is not None and f["latch_gap"] > 1.5 * period]
    ontime = [f for f in frames if f["latch_gap"] is not None and f["latch_gap"] <= 1.5 * period]
    latched = [f for f in frames if f["latch"] is not None]

    print("== %d ROOT_DIRECT frames applied, %d with a latch time, %d with a present time; refresh period %s ms"
          % (len(frames), len(latched), sum(f["presented"] is not None for f in frames), ms(period)))
    print("   latched late (over 1.5 periods after the frame before): %d, on time: %d" % (len(late), len(ontime)))
    for name, group in (("late", late), ("on time", ontime)):
        if not group:
            continue
        print("-- %s (%d):" % (name, len(group)))
        print("   " + describe("tick -> publish", [fr["published"] - fr["vsync"] if fr["vsync"] is not None and fr["published"] else None for fr in group]))
        print("   " + describe("publish -> claim", [fr["claimed"] - fr["published"] if fr["published"] else None for fr in group]))
        print("   " + describe("claim -> apply", [fr["applied"] - fr["claimed"] for fr in group]))
        print("   " + describe("of which fence wait", [fr["fence_us"] for fr in group]))
        print("   " + describe("apply, after its vsync", [fr["applied"] - fr["vsync"] if fr["vsync"] is not None else None for fr in group]))
        print("   " + describe("apply -> latch", [fr["latch"] - fr["applied"] if fr["latch"] else None for fr in group]))
        print("   " + describe("X server blocked between its vsync and the apply", [fr["xlock_us"] for fr in group]))
        print("   copied in the frame's drain: into the slot it applied p50 %d kpx, into anything else p50 %d kpx"
              % (pct([fr["safe_kpx"] for fr in group], 50), pct([fr["other_kpx"] for fr in group], 50)))
    phases_on = [f["applied"] - f["vsync"] for f in ontime if f["vsync"] is not None]
    phases_late = [f["applied"] - f["vsync"] for f in late if f["vsync"] is not None]
    if phases_on and phases_late:
        if max(phases_on) < min(phases_late):
            print("== every late frame was applied later in its vsync than every on-time one: on time up to %s ms, "
                  "late from %s ms" % (ms(max(phases_on)), ms(min(phases_late))))
        else:
            print("== late and on-time frames overlap in when they were applied: on time up to %s ms, late from %s "
                  "ms - lateness is not explained by the apply time alone" % (ms(max(phases_on)), ms(min(phases_late))))
    if args.csv:
        keys = ["frame", "publish", "apply_seq", "vsync", "published", "claimed", "safe_kpx", "other_kpx", "fence_us",
                "applied", "completed", "latch", "presented", "latch_gap", "xlock_us"]
        with open(args.csv, "w") as out:
            out.write(",".join(keys) + "\n")
            for fr in frames:
                out.write(",".join("" if fr[k] is None else str(fr[k]) for k in keys) + "\n")

if __name__ == "__main__":
    main()
