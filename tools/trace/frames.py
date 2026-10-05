#!/usr/bin/env python3
"""Follows each ROOT_DIRECT frame in a trace from the vsync to the compositor's latch and present, and sets
the frames the compositor took late against the ones it took on time - with what the frame before each
one did, since a frame can be late because of the one before it.

usage: frames.py trace.ltr [--period-ms MS] [--csv FILE]

A frame is one transaction the renderer applied (ZCAPPLY), joined to the rest by sequence numbers, not by
order: the publish it took (ZCCLAIM's publish seq, PUBSEQ), the renderer's frame (ZCBATCH, ZCFENCE), and
the transaction (SFDONE's latch time, SFPRESENT). All times are CLOCK_MONOTONIC.

Its vsync is the last VSYNC the X server recorded before the publish. That record has two times: when the
X server took the tick up (the record's own time) and the Choreographer's frame time it carries; they are
kept apart - the gap between them is not the X server's processing.

Late: taken more than 1.5 refresh periods after the frame before it - by latch time (the compositor's own
record) or by present time (the present fence's own record). A frame whose predecessor has no such time is
not judged; one with none of its own is counted apart, with why.

The renderer is one thread: while it waits on a fence - of a frame it applies, of one that found nothing
newer, or of a drain on its own (DRAIN to the FENCE of the same serial) - it claims nothing. That busy time
inside a frame's publish -> claim is reported, as is the frame before it.

Where a late frame lost its vsync is read off the compositor's latch grid: the first latch after its publish
taken means the publish itself came a vsync late; otherwise the time went in the claim, the apply, or after
an apply that was in time. A publish can be late because its content was: a client presenting into a
redirected window (ENQUEUE a = 0) gets to the root only once the compositor has read that window back, which
the X server lets it do only when the copy into it is done (XLOCK on that buffer). For such frames the window
copy queued at the tick of the publish before is followed to when the X server saw it finished.

Present times are used only if they differ from one another: some drivers' present fences all carry the
same timestamp.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import load, pct

DRAIN, FENCE, XLOCK, RLOCK = 7, 8, 15, 18
VSYNC, PUBSEQ, ZCCLAIM, ZCBATCH, ZCFENCE, ZCAPPLY, SFDONE, SFPRESENT = range(19, 27)

def overlap(intervals, t1, t2):
    if t1 is None or t2 is None or t2 <= t1:
        return 0
    return sum(max(0, min(e, t2) - max(s, t1)) for s, e in intervals if e > t1 and s < t2)

def gaps(seqs):
    seqs = sorted(set(seqs))
    return sum(b - a - 1 for a, b in zip(seqs, seqs[1:]) if b - a > 1)

def join(recs):
    vsyncs, pub, claim, batch, fence, apply_, done, present = [], {}, {}, {}, {}, {}, {}, {}
    xlocks, drains, enq, fenced, requests, preflights = [], {}, [], [], [], []
    ticks, resolved, rootIds = [], {}, set()
    busyBy = {"standalone drain": [], "nothing-new frame": [], "applied frame": []}
    # a drain inside a ROOT_DIRECT frame is followed at once by the frame's ZCBATCH, and its fence wait is
    # the frame's ZCFENCE; any other drain is one on its own, whose wait runs from it to its FENCE
    batchTimes = sorted(t for t, k, a, b in recs if k == ZCBATCH)
    import bisect
    def inFrame(t):
        i = bisect.bisect_left(batchTimes, t)
        return i < len(batchTimes) and batchTimes[i] - t <= 1000
    for t, k, a, b in recs:
        if k == VSYNC:       vsyncs.append((t, b))
        elif k == PUBSEQ:    pub[a] = t
        elif k == ZCCLAIM:   claim[a] = (t, b & 0xffffffff, bool(b >> 32))
        elif k == ZCBATCH:   batch[a] = (b >> 32, b & 0xffffffff)
        elif k == ZCFENCE:   fence[a] = (t, b)
        elif k == ZCAPPLY:   apply_[a] = (t, b)
        elif k == SFDONE:    done[a] = (t, b)
        elif k == SFPRESENT: present[a] = t
        elif k == XLOCK:     xlocks.append((t - a, t, b))
        elif k == 5:         ticks.append(t)                # TICK
        elif k == 3:         resolved[b] = t                # RESOLVED: the X server saw copy b finished
        elif k == 4:         rootIds.add(b)                 # PUBLISH: b is a root slot's buffer
        elif k == 1:         requests.append(t)             # REQUEST: a client's present arrived
        elif k == 14:        preflights.append((t - b, t))  # PREFLIGHT: an EXA fallback waited b us
        elif k == 2:         enq.append((t, a, b))          # ENQUEUE: a = 1 present into the root, 0 into
                                                            # anything else (a redirected window), 2 carry, 3 core
        elif k == DRAIN:
            if not inFrame(t):
                drains[b] = t
        elif k == FENCE:
            fenced.append((t, b))
            if b in drains:
                s = drains.pop(b)
                if t > s:
                    busyBy["standalone drain"].append((s, t))
    for f, (t, b) in fence.items():
        if b and f in claim:
            busyBy["nothing-new frame" if claim[f][2] else "applied frame"].append((t - b, t))
    busy = [iv for v in busyBy.values() for iv in v]
    # each copy's completion: the first fence (any path) that covers its serial
    fenced.sort()
    fencedSerials, fencedTimes, best = [], [], 0
    for t, b in fenced:
        if b > best:
            best = b
            fencedSerials.append(b); fencedTimes.append(t)
    def completedAt(serial):
        i = bisect.bisect_left(fencedSerials, serial)
        return fencedTimes[i] if i < len(fencedSerials) else None
    presentsIn = [(t, b) for t, a, b in enq if a == 1]
    # Presents into a redirected window: the root gets that content only once a compositor has read it
    # back, which it can do only when the copy is done - the X server waits for it (XLOCK on that buffer).
    windowCopies = sorted((t, b) for t, a, b in enq if a == 0)
    ticks.sort()
    def tickBefore(t):
        i = bisect.bisect_right(ticks, t)
        return ticks[i - 1] if i else None
    def tickAfter(t):
        i = bisect.bisect_right(ticks, t)
        return ticks[i] if i < len(ticks) else None
    pubTimes = sorted(pub.values())
    xlockSpans = sorted((a, b) for a, b, buf in xlocks)
    windowXlockSpans = sorted((a, b) for a, b, buf in xlocks if buf not in rootIds)
    claimsByTime = sorted((t, f, already) for f, (t, p, already) in claim.items())
    frames = []
    vi = 0
    order = sorted(apply_.items(), key=lambda kv: kv[1][0])
    for f, (tApply, seq) in order:
        if f not in claim:
            continue
        tClaim, pseq, already = claim[f]
        if already:
            continue
        tPub = pub.get(pseq)
        consumed = frameTime = None
        if tPub is not None:
            while vi + 1 < len(vsyncs) and vsyncs[vi + 1][0] <= tPub:
                vi += 1
            if vsyncs and vsyncs[vi][0] <= tPub:
                consumed, frameTime = vsyncs[vi]
        safe, other = batch.get(f, (0, 0))
        tFence, waitUs = fence.get(f, (None, 0))
        rec = done.get(seq)
        # the present copy whose content this slot carries: the last one queued into the root before the publish
        pc = None
        if tPub is not None:
            i = bisect.bisect_right(presentsIn, (tPub, float("inf"))) - 1
            if i >= 0:
                pc = presentsIn[i]
        frames.append(dict(
            present_queued=pc[0] if pc else None, present_done=completedAt(pc[1]) if pc else None,
            busy_drain=overlap(busyBy["standalone drain"], tPub, tClaim),
            busy_nothing_new=overlap(busyBy["nothing-new frame"], tPub, tClaim),
            busy_applied=overlap(busyBy["applied frame"], tPub, tClaim),
            frame=f, publish=pseq, apply_seq=seq, frame_time=frameTime, consumed=consumed, published=tPub,
            claimed=tClaim, safe_kpx=safe, other_kpx=other, fence_us=waitUs, applied=tApply,
            completed=rec[0] if rec else None, sfdone=rec is not None,
            latch=rec[1] // 1000 if rec and rec[1] else None, presented=present.get(seq),
            xlock_us=overlap(xlockSpans, consumed, tApply),
            busy_before_claim=overlap(busy, tPub, tClaim)))
        # the window copy queued at the tick of the publish before this one: the content the next tick's
        # publish could have carried, had the compositor been able to read it back in time
        fr = frames[-1]
        fr.update(prev_published=None, next_tick=None, window_copy=None, window_copy_done=None,
                  window_copy_resolved=None, x_blocked_window=0)
        if tPub is not None:
            i = bisect.bisect_left(pubTimes, tPub)
            p0 = pubTimes[i - 1] if i else None
            fr["prev_published"] = p0
            if p0 is not None:
                t0, t1 = tickBefore(p0), tickAfter(p0)
                fr["next_tick"] = t1
                if t0 is not None and t1 is not None:
                    j = bisect.bisect_left(windowCopies, (t1, -1)) - 1
                    if j >= 0 and windowCopies[j][0] >= t0:
                        tq, serial = windowCopies[j]
                        fr.update(window_copy=tq, window_copy_done=completedAt(serial),
                                  window_copy_resolved=resolved.get(serial))
                fr["x_blocked_window"] = overlap(windowXlockSpans, p0, tPub)
        fr["window_copy_done_in_claim_wait"] = any(tPub is not None and tPub < (completedAt(s) or 0) <= tClaim
                                                   for t, s in windowCopies[max(0, bisect.bisect_left(windowCopies, ((tPub or 0) - 100000, -1))):
                                                                            bisect.bisect_left(windowCopies, (tClaim, -1))])
        fr["window_copy_done_in_apply_wait"] = any(tClaim < (completedAt(s) or 0) <= tApply
                                                   for t, s in windowCopies[max(0, bisect.bisect_left(windowCopies, (tClaim - 100000, -1))):
                                                                            bisect.bisect_left(windowCopies, (tApply, -1))])
    prev = None
    ci = 0
    newest = None
    for fr in frames:
        # the same publish applied again (back on screen after leaving the compositor): not a new frame
        fr["reapplied"] = newest is not None and fr["publish"] <= newest
        newest = fr["publish"] if newest is None else max(newest, fr["publish"])
        # nothing-new frames the renderer ran between the frame before and this one's claim
        n = 0
        if prev is not None:
            while ci < len(claimsByTime) and claimsByTime[ci][0] <= prev["applied"]:
                ci += 1
            j = ci
            while j < len(claimsByTime) and claimsByTime[j][0] < fr["claimed"]:
                n += claimsByTime[j][2]
                j += 1
        fr["nothing_new_between"] = n
        for key in ("fence_us", "safe_kpx", "other_kpx", "xlock_us"):
            fr["prev_" + key] = prev[key] if prev else None
        fr["prev_claim_to_apply"] = prev["applied"] - prev["claimed"] if prev else None
        fr["prev_apply_to_latch"] = prev["latch"] - prev["applied"] if prev and prev["latch"] else None
        fr["prev_apply_to_claim"] = fr["claimed"] - prev["applied"] if prev else None
        fr["latch_gap"] = fr["latch"] - prev["latch"] if prev and fr["latch"] and prev["latch"] else None
        fr["present_gap"] = fr["presented"] - prev["presented"] if prev and fr["presented"] and prev["presented"] else None
        prev = fr
    checks = dict(requests=sorted(requests), preflights=preflights, xlocks=xlocks, publish_times=pubTimes,
                  window_copies=windowCopies, root_ids=rootIds,
                  publish_gaps=gaps(pub.keys()), frame_gaps=gaps(claim.keys()), apply_gaps=gaps(a for t, a in apply_.values()),
                  claims=len(claim), batches=len(batch), fences=len(fence),
                  sfdone_without_apply=sum(1 for s in done if s not in set(a for t, a in apply_.values())))
    return frames, checks

def ms(v):
    return "%.1f" % (v / 1000.0)

def describe(name, vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return "%-44s -" % name
    return "%-44s p50 %6s p90 %6s max %7s" % (name, ms(pct(vals, 50)), ms(pct(vals, 90)), ms(max(vals)))

def stages(group, presentsUsable):
    d = lambda a, b: [fr[b] - fr[a] if fr[a] is not None and fr[b] is not None else None for fr in group]
    print("   " + describe("frame time -> X took the tick up", d("frame_time", "consumed")))
    print("   " + describe("X took the tick up -> publish", d("consumed", "published")))
    print("   " + describe("publish -> claim", d("published", "claimed")))
    print("   " + describe("  of which the renderer busy on fences", [fr["busy_before_claim"] for fr in group]))
    print("   " + describe("    a drain on its own", [fr["busy_drain"] for fr in group]))
    print("   " + describe("    a frame that found nothing newer", [fr["busy_nothing_new"] for fr in group]))
    print("   " + describe("    the frame applied before it", [fr["busy_applied"] for fr in group]))
    print("   " + describe("present copy queued -> done on the GPU", d("present_queued", "present_done")))
    print("   " + describe("present copy done -> claim", d("present_done", "claimed")))
    print("   " + describe("claim -> apply", d("claimed", "applied")))
    print("   " + describe("  of which fence wait", [fr["fence_us"] for fr in group]))
    print("   " + describe("X took the tick up -> apply", d("consumed", "applied")))
    print("   " + describe("apply -> latch", d("applied", "latch")))
    if presentsUsable:
        print("   " + describe("apply -> present", d("applied", "presented")))
        print("   " + describe("latch -> present", d("latch", "presented")))
    print("   " + describe("X blocked, tick taken up -> apply", [fr["xlock_us"] for fr in group]))
    print("   drained: into its own slot p50 %d kpx, into anything else p50 %d kpx" %
          (pct([fr["safe_kpx"] for fr in group], 50), pct([fr["other_kpx"] for fr in group], 50)))

def predecessors(group):
    print("   the frame before it:")
    print("   " + describe("  fence wait", [fr["prev_fence_us"] for fr in group]))
    print("   " + describe("  claim -> apply", [fr["prev_claim_to_apply"] for fr in group]))
    print("   " + describe("  apply -> latch", [fr["prev_apply_to_latch"] for fr in group]))
    print("   " + describe("  X blocked", [fr["prev_xlock_us"] for fr in group]))
    print("   " + describe("  its apply -> this claim", [fr["prev_apply_to_claim"] for fr in group]))
    vals = [fr["prev_safe_kpx"] for fr in group if fr["prev_safe_kpx"] is not None]
    other = [fr["prev_other_kpx"] for fr in group if fr["prev_other_kpx"] is not None]
    if vals:
        print("     drained: into its own slot p50 %d kpx, into anything else p50 %d kpx" % (pct(vals, 50), pct(other, 50)))
    nn = [fr["nothing_new_between"] for fr in group]
    print("     nothing-new frames in between: %d of %d frames had one or more" % (sum(1 for v in nn if v), len(nn)))

def share(group, cond):
    return "%d of %d (%.0f%%)" % (sum(1 for fr in group if cond(fr)), len(group), 100.0 * sum(1 for fr in group if cond(fr)) / max(1, len(group)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--period-ms", type=float, default=0, help="refresh period; from the vsyncs if not given")
    ap.add_argument("--csv")
    args = ap.parse_args()
    start, recs = load(args.trace)
    frames, checks = join(recs)
    if not frames:
        sys.exit("no ROOT_DIRECT frames in this trace")
    period = args.period_ms * 1000
    if not period:
        vs = sorted(set(b for t, k, a, b in recs if k == VSYNC))
        g = [y - x for x, y in zip(vs, vs[1:]) if 4000 <= y - x <= 40000]
        period = pct(g, 50) if g else 16667
    late_at = 1.5 * period

    reapplied = [f for f in frames if f["reapplied"]]
    frames = [f for f in frames if not f["reapplied"]]
    groups = {
        "on time (latch)": [f for f in frames if f["latch_gap"] is not None and f["latch_gap"] <= late_at],
        "late (latch)": [f for f in frames if f["latch_gap"] is not None and f["latch_gap"] > late_at],
        "latched, not judged": [f for f in frames if f["latch"] is not None and f["latch_gap"] is None],
        "no latch": [f for f in frames if f["latch"] is None],
    }
    print("== %d ROOT_DIRECT frames applied, %d with a latch time, %d with a present time; refresh period %s ms"
          % (len(frames), sum(f["latch"] is not None for f in frames), sum(f["presented"] is not None for f in frames),
             ms(period)))
    print("   %d of them the same publish applied again, left out of everything below" % len(reapplied))
    print("   latched late (over 1.5 periods after the frame before): %d, on time: %d"
          % (len(groups["late (latch)"]), len(groups["on time (latch)"])))
    print("   latched but the frame before has none (not judged): %d; no latch: %d"
          % (len(groups["latched, not judged"]), len(groups["no latch"])))
    nl = groups["no latch"]
    print("   no latch: completion callback never recorded %d, recorded with no latch time %d; of them with a "
          "present time %d" % (sum(not f["sfdone"] for f in nl), sum(f["sfdone"] for f in nl),
                               sum(f["presented"] is not None for f in nl)))
    print("   trace completeness: %d publish seqs missing, %d frame seqs missing, %d transaction seqs missing; "
          "claims %d, drains %d, fences %d; %d completions for transactions not in the trace"
          % (checks["publish_gaps"], checks["frame_gaps"], checks["apply_gaps"], checks["claims"], checks["batches"],
             checks["fences"], checks["sfdone_without_apply"]))

    # some drivers' present fences all carry one timestamp, which says nothing about when a frame was shown
    presented = [f["presented"] for f in frames if f["presented"] is not None]
    presentsUsable = bool(presented) and len(set(presented)) > max(1, len(presented) // 100)
    for name in ("late (latch)", "on time (latch)", "no latch"):
        group = groups[name]
        if not group:
            continue
        print("-- %s (%d):" % (name, len(group)))
        stages(group, presentsUsable)
        predecessors(group)

    if presented and not presentsUsable:
        print("== present times: %d, but only %d distinct value(s) - the fences do not record when the frame was "
              "shown; nothing here is judged by them" % (len(presented), len(set(presented))))
    elif presented:
        pgroups = {
            "late (present)": [f for f in frames if f["present_gap"] is not None and f["present_gap"] > late_at],
            "on time (present)": [f for f in frames if f["present_gap"] is not None and f["present_gap"] <= late_at],
        }
        print("== by present time: late %d, on time %d (%s)"
              % (len(pgroups["late (present)"]), len(pgroups["on time (present)"]),
                 describe("present gaps", [f["present_gap"] for f in frames if f["present_gap"] is not None]).strip()))
        for name in ("late (present)", "on time (present)"):
            group = pgroups[name]
            if not group:
                continue
            print("-- %s (%d):" % (name, len(group)))
            stages(group, True)
            predecessors(group)
        both = [f for f in frames if f["latch_gap"] is not None and f["present_gap"] is not None]
        print("== late by latch and by present: both %d, latch only %d, present only %d, neither %d"
              % (sum(f["latch_gap"] > late_at and f["present_gap"] > late_at for f in both),
                 sum(f["latch_gap"] > late_at and f["present_gap"] <= late_at for f in both),
                 sum(f["latch_gap"] <= late_at and f["present_gap"] > late_at for f in both),
                 sum(f["latch_gap"] <= late_at and f["present_gap"] <= late_at for f in both)))

    # The compositor latches on its vsync: its latch times lie on a grid. For each frame, the first grid
    # point after its apply is its first chance; latched there, it made it, at a later one, it missed.
    # How long before that first chance it was applied (its slack) says whether a deadline explains it.
    import bisect
    latches = sorted(set(f["latch"] for f in frames if f["latch"] is not None))
    P = period
    if len(latches) > 10:
        lg = [y - x for x, y in zip(latches, latches[1:]) if 0.5 * period < y - x < 1.5 * period]
        P = pct(lg, 50) if lg else period
        made, missed = [], []
        for fr in frames:
            if fr["latch"] is None:
                continue
            i = bisect.bisect_left(latches, fr["applied"])
            ref = latches[min(i, len(latches) - 1)]
            k = -((ref - fr["applied"]) // P) if ref < fr["applied"] else -(-(ref - fr["applied"]) // P) * 0
            first = ref + P * (((fr["applied"] - ref) // P) + 1) if ref < fr["applied"] else ref - P * ((ref - fr["applied"]) // P)
            if first <= fr["applied"]:
                first += P
            slack = first - fr["applied"]
            chance = round((fr["latch"] - first) / P)
            (made if chance <= 0 else missed).append((slack, fr))
        print("== against the compositor's latch grid (period %s ms from its latches): %d latched at the first chance "
              "after their apply, %d at a later one" % (ms(P), len(made), len(missed)))
        print("   " + describe("slack, first chance taken", [s for s, f in made]))
        print("   " + describe("slack, first chance missed", [s for s, f in missed]))
        if made and missed:
            lo = min(s for s, f in made)
            print("   missed ones with more slack than the least any first chance taken had (%s ms): %d of %d"
                  % (ms(lo), sum(1 for s, f in missed if s > lo), len(missed)))
            taken = sorted(s for s, f in made)
            miss = sorted(s for s, f in missed)
            for d in (2000, 3000, 4000, 5000, 6000):
                print("   slack under %s ms: first chance taken %d, missed %d" %
                      (ms(d), bisect.bisect_left(taken, d), bisect.bisect_left(miss, d)))

    # Where each late frame lost its vsync. Against the latch grid: the first latch after its publish
    # taken means its content came late - no new root slot for a vsync; missed, the time went after the
    # publish - in the claim, in the apply, or after a timely apply.
    late = groups["late (latch)"]
    if late and len(latches) > 10:
        reqs, pubs = checks["requests"], checks["publish_times"]
        def first_after(t):
            i = bisect.bisect_left(latches, t)
            ref = latches[min(i, len(latches) - 1)]
            g = ref + P * ((t - ref) // P + 1) if ref < t else ref - P * ((ref - t) // P)
            while g <= t:
                g += P
            return g
        def prev_of(seq, t):
            i = bisect.bisect_left(seq, t)
            return seq[i - 1] if i > 0 else None
        content, claimLate, applyLate, sfLate = [], [], [], []
        for fr in late:
            if fr["published"] is None:
                continue
            g = first_after(fr["published"])
            if round((fr["latch"] - g) / P) <= 0:
                content.append(fr)
            elif fr["claimed"] > g:
                claimLate.append((fr, g))
            elif fr["applied"] > g:
                applyLate.append((fr, g))
            else:
                sfLate.append(fr)
        print("== where the %d late frames lost their vsync:" % len(late))
        # content late: followed back to the window copy that should have fed the publish at the tick after
        # the one before - none queued, or queued and not read back by the compositor in time
        wc = checks["window_copies"]
        span = (wc[0][0], wc[-1][0]) if wc else None
        def path(fr):
            if fr["window_copy"] is None:
                if span and span[0] <= fr["prev_published"] <= span[1]:
                    return "no window copy queued at the tick of the publish before"
                return "outside the span the client presented into a window in"
            if fr["window_copy_resolved"] is None or fr["window_copy_resolved"] > fr["next_tick"]:
                return "its window copy seen finished by the X server only after the next tick"
            return "window copy seen finished before the next tick, root not published at it"
        print("   new content came late (first latch after its publish taken, the publish a vsync late): %d" % len(content))
        paths = {}
        for fr in content:
            paths.setdefault(path(fr), []).append(fr)
        for name, g in sorted(paths.items(), key=lambda kv: -len(kv[1])):
            print("     %s: %d" % (name, len(g)))
            w = [fr for fr in g if fr["window_copy"] is not None and fr["window_copy_done"]]
            if w:
                print("   " + describe("      window copy queued -> done", [fr["window_copy_done"] - fr["window_copy"] for fr in w]))
                print("   " + describe("      done -> seen by the X server", [fr["window_copy_resolved"] - fr["window_copy_done"]
                                                                             for fr in w if fr["window_copy_resolved"]]))
            if g and g[0]["prev_published"] is not None:
                print("   " + describe("      X blocked on a non-root buffer between the publishes", [fr["x_blocked_window"] for fr in g]))
        okw = [fr for fr in groups["on time (latch)"] if fr["window_copy"] is not None and fr["window_copy_done"]]
        if okw:
            print("     on time, for comparison: %d with a window copy at the tick before" % len(okw))
            print("   " + describe("      window copy queued -> done", [fr["window_copy_done"] - fr["window_copy"] for fr in okw]))
            print("   " + describe("      X blocked on a non-root buffer between the publishes", [fr["x_blocked_window"] for fr in okw]))
        print("   published in time, claimed after its first latch: %d" % len(claimLate))
        if claimLate:
            g0 = [fr for fr, g in claimLate]
            print("   " + describe("    publish -> claim", [fr["claimed"] - fr["published"] for fr in g0]))
            print("   " + describe("    the renderer on a drain on its own then", [fr["busy_drain"] for fr in g0]))
            print("   " + describe("    on a frame that found nothing newer", [fr["busy_nothing_new"] for fr in g0]))
            print("   " + describe("    on the frame applied before it", [fr["busy_applied"] for fr in g0]))
            print("     a window copy finished while it waited to be claimed: %d of %d"
                  % (sum(fr["window_copy_done_in_claim_wait"] for fr in g0), len(g0)))
        print("   claimed in time, applied after its first latch: %d" % len(applyLate))
        if applyLate:
            g1 = [fr for fr, g in applyLate]
            print("   " + describe("    claim -> apply", [fr["applied"] - fr["claimed"] for fr in g1]))
            print("   " + describe("    of which its fence wait", [fr["fence_us"] for fr in g1]))
            print("     drained then: into anything but its own slot p50 %d kpx" % pct([fr["other_kpx"] for fr in g1], 50))
            print("     a window copy finished in its own fence wait: %d of %d"
                  % (sum(fr["window_copy_done_in_apply_wait"] for fr in g1), len(g1)))
        print("   applied before its first latch and latched later all the same: %d" % len(sfLate))
        if sfLate:
            print("   " + describe("    how long before that latch it was applied", [first_after(fr["published"]) - fr["applied"] for fr in sfLate]))

    ontime = groups["on time (latch)"]
    if late and ontime:
        slow = lambda f: f["claimed"] - f["published"] > period / 3 if f["published"] else False
        print("== the chain, late (latch) vs on time:")
        print("   publish -> claim over a third of a period: late %s, on time %s" % (share(late, slow), share(ontime, slow)))
        busy = lambda f: slow(f) and f["busy_before_claim"] >= (f["claimed"] - f["published"]) / 2
        print("   ... with the renderer busy on fences for half of it or more: late %s, on time %s"
              % (share(late, busy), share(ontime, busy)))
        pf = lambda f: f["prev_fence_us"] is not None and f["prev_fence_us"] > period / 3
        print("   the frame before waited on its fence over a third of a period: late %s, on time %s"
              % (share(late, pf), share(ontime, pf)))
        xb = lambda f: f["xlock_us"] > period / 3
        print("   X blocked over a third of a period between the tick and the apply: late %s, on time %s"
              % (share(late, xb), share(ontime, xb)))

    if args.csv:
        keys = ["frame", "publish", "apply_seq", "frame_time", "consumed", "published", "claimed", "safe_kpx", "other_kpx",
                "fence_us", "applied", "completed", "sfdone", "latch", "presented", "latch_gap", "present_gap",
                "xlock_us", "busy_before_claim", "nothing_new_between", "prev_fence_us", "prev_claim_to_apply",
                "prev_apply_to_latch", "prev_safe_kpx", "prev_other_kpx", "prev_xlock_us", "prev_apply_to_claim",
                "busy_drain", "busy_nothing_new", "busy_applied", "reapplied", "prev_published", "next_tick",
                "window_copy", "window_copy_done", "window_copy_resolved", "x_blocked_window",
                "window_copy_done_in_claim_wait", "window_copy_done_in_apply_wait"]
        with open(args.csv, "w") as out:
            out.write(",".join(keys) + "\n")
            for fr in frames:
                out.write(",".join("" if fr[k] is None else str(int(fr[k])) for k in keys) + "\n")

if __name__ == "__main__":
    main()
