#!/usr/bin/env python3
"""C2: whether a client paced by Present's FIFO feedback gets its next frame on the next vsync, by the
dependency graph one frame goes through - not the sum of GPU time.

usage: fifotick.py [--hz 60,120]

A client like SuperTuxKart (Vulkan, X11 WSI, FIFO) presents frame f+1 with target = the msc its frame f
completed at + 1. The X server reports a GPU-copied present complete only once the renderer's fence for
it has signalled and the X server's own thread has taken up the renderer's notification (EVENT_GPU_COPY_DONE
-> lorieRecheckGpuCopies). That notification is read from the socket only when the X thread is back in its
loop, so if the thread is still busy when the next vsync's lorieRedraw is queued (from the Choreographer
thread), the tick runs first, the completion carries the next msc, and the frame costs two ticks.

One frame, from the tick (t = 0) at which its present is executed, its window redirected and composited
by xfwm4 with XRender (an EXA fallback on the X thread), as the sources do it:

  renderer  woken by the copy's signal at w, before the gate opens: drains it alone (rendererApplyPending
            GpuCopies): lock, copy on the GPU - it cannot finish before the client's own rendering of the
            source does (r) - fence waited inside the lock, notification sent (done1). Then its GL frame,
            once the gate is open and the draw requested at s: lock, drain what is queued, draw the root,
            fence; the fence is waited inside the lock when the frame carried GPU work (current, 10fcb73)
            and always in 3c98ffa (root fence wait on in every mode).
  X server  its tick work up to s (the handover, if any), then the compositor's request, which arrived
            at dx: in the current code an EXA preflight first (lorieFallbackWait: wait for the queue to be
            drained - 4 ms when only carries are queued, 20 ms otherwise); then PrepareAccess on the window
            (a copy into it is pending until the completion is taken up, so the shared lock); then on the
            root's drawing slot (3c98ffa: the lock again; multi-root: a carry into the slot must have landed,
            and with the lock held and the carry still queued it is taken back and made by the CPU, tb);
            the composite on the CPU (cc); and only then the completion.
  lock      when the renderer lets go of the lock and both want it, `race` says who gets it.

Structures:
  A  3c98ffa          single live root; no carry; X locks the root itself
  B  current          multi-root; the handover queues a carry; separate batches; preflight
  C  multi-root       carry drained in the same batch as the copy (one lock, one fence, after the publish)
  D  multi-root       no carry (the drawing slot needs nothing to land); preflight

Costs are symbols swept over ranges, not the current trace's distributions.
"""
import argparse, itertools, sys

PREFLIGHT_CARRY_MS, PREFLIGHT_MS = 4.0, 20.0


def frame(s, p, race):
    """Time (ms after the tick) at which the X server takes up frame f's completion, and the steps."""
    w, r, gc, gk, gr, h, dx, cc, tb = (p[k] for k in ("w", "r", "gc", "gk", "gr", "h", "dx", "cc", "tb"))
    sx = h if s != "A" else p["hA"]          # end of the X tick work: the handover is not in 3c98ffa
    tx0 = max(dx, sx)                        # the compositor's request taken up
    path = []
    if s == "C":
        a = max(w, sx)                       # one batch, after the publish: copy, carry, root
        done = max(a, r) + gc + gk + gr
        event = done
        # preflight: the copy and the carry are queued until a; then the window lock until done
        start = max(tx0, a) if tx0 < a else tx0
        if start < done:
            start = done
        path.append("one batch to %.2f" % done)
        tcd = start + cc
    else:
        done1 = max(w, r) + gc               # the copy alone, fence inside the lock
        event = done1
        L0 = max(done1, sx)                  # the GL frame wants the lock
        carry = s == "B"
        holds = s in ("A", "B")              # GL frame waits for its fence inside the lock
        done2 = L0 + (gk if carry else 0.0) + gr
        gl_end = done2 if holds else L0      # D: no copies in the frame, fence deferred past the lock
        # X: preflight (not in 3c98ffa); only the carry can still be queued here (the copy went at w)
        t = tx0
        timed_out = False
        if s in ("B", "D") and carry and t < L0:
            if L0 - t > PREFLIGHT_CARRY_MS:
                t, timed_out = t + PREFLIGHT_CARRY_MS, True
            else:
                t = L0                       # drained when the GL frame takes the lock and drains it
        # X: the window's lock
        if t < done1:
            # waits for the copy batch; at done1 the GL frame wants the lock as well
            if race == "x" and (not carry or timed_out):
                start = done1
                if carry:                    # carry still queued: taken back by the CPU
                    start += tb
                    path.append("carry taken back")
            else:
                start = gl_end
        elif t < L0:
            start = t
            if carry and timed_out:
                start += tb
        elif t < gl_end:
            start = gl_end
        else:
            start = t
        tcd = start + cc
        path.append("copy %.2f, GL frame %.2f-%.2f" % (done1, L0, gl_end))
    # the completion is read once the X thread is back in its loop
    complete = max(event, tcd) if dx <= event else max(event, sx)
    return complete, path


def ticks(t, period):
    return 1 if t < period else 2 if t < 2 * period else 3


GRID = {
    "w": (0.05, 0.3),            # renderer woken by the copy's signal (before the gate opens)
    "r": tuple(x * 0.5 for x in range(0, 21)),   # client's own GPU rendering left at the tick, 0-10 ms
    "gc": (0.3, 0.8, 1.5),       # the present copy on the GPU
    "gk": (0.3, 0.8, 1.5),       # a full-screen carry on the GPU
    "gr": (0.2, 0.6, 1.2),       # the GL root draw
    "h": (0.1, 0.4, 1.0),        # X tick work up to the end of the handover (multi-root)
    "hA": (0.05,),               # X tick work in 3c98ffa (no handover)
    "dx": (0.5, 1.5, 3.0),       # the compositor's request arriving after the Damage
    "cc": (1.0, 2.0, 4.0),       # the composite on the CPU
    "tb": (1.0, 2.5),            # a carry taken back by the CPU
}


def sweep(period, structures=("A", "B", "C", "D"), grid=None, frame_fn=None):
    grid = grid or GRID
    frame_fn = frame_fn or frame
    keys = list(grid)
    out = []
    for vals in itertools.product(*(grid[k] for k in keys)):
        p = dict(zip(keys, vals))
        for race in ("renderer", "x"):
            t = {s: frame_fn(s, p, race)[0] for s in structures}
            out.append((p, race, t, {s: ticks(t[s], period) for s in structures}))
    return out


def summarise(rows, structures=("A", "B", "C", "D")):
    n = len(rows)
    s = {"points": n}
    for x in structures:
        s[x + "_two"] = sum(1 for p, race, t, k in rows if k[x] > 1)
    for x in structures:
        if x == "A":
            continue
        s[x + "_worse_than_A"] = sum(1 for p, race, t, k in rows if k[x] > k["A"])
        s[x + "_better_than_A"] = sum(1 for p, race, t, k in rows if k[x] < k["A"])
        s[x + "_extra_ms_max"] = max(t[x] - t["A"] for p, race, t, k in rows)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", default="60,120")
    args = ap.parse_args()
    for hz in (int(x) for x in args.hz.split(",")):
        period = 1000.0 / hz
        rows = sweep(period)
        s = summarise(rows)
        print("== %d Hz (%.2f ms): %d parameter points x lock race" % (hz, period, s["points"]))
        for x in ("A", "B", "C", "D"):
            extra = "" if x == "A" else "  worse than A %d, better %d, up to %.2f ms later" % (
                s[x + "_worse_than_A"], s[x + "_better_than_A"], s[x + "_extra_ms_max"])
            print("   %s  frames needing two ticks %6d%s" % (x, s[x + "_two"], extra))
        ex = next(((p, race, t) for p, race, t, k in rows if k["A"] == 1 and k["B"] == 2 and k["D"] == 1), None)
        if ex:
            p, race, t = ex
            print("   e.g. %s race=%s: A %.2f  B %.2f  C %.2f  D %.2f ms" % (
                " ".join("%s=%g" % kv for kv in p.items()), race, t["A"], t["B"], t["C"], t["D"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
