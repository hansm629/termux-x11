#!/usr/bin/env python3
"""C1: what the GL output path puts on screen, tick by tick, when the renderer may run at any point of the
X server's vsync tick - every interleaving, to a bound, applied to three structures at once.

usage: glphase.py [--ticks N] [--warmup W] [--witness]

The X server's tick (InitOutput.c lorieRedraw, one per Choreographer callback) is a fixed sequence of
steps; the renderer is another process and may act between any two of them:

  V+P  vsync taken up; the client's present executed - its GPU copy queued, the renderer signalled
       (loriePerformVblanks -> present_execute_copy -> lorieTryScheduleGpuCopy)
  G    waitForNextFrame = false - the vsync gate opens
  H    3c98ffa: drawRequested if the root was damaged (no publish - the renderer draws the live root)
       current: the slot drawn into is published (lorieRootHandover) and drawRequested set, if damaged
  S    rootWindowTextureID, renderer signalled

  A  3c98ffa             single live root                         X order V+P, G, H, S
  B  current GL output   multi-root, the renderer draws a publish X order V+P, G, H, S
  C  multi-root          the gate opened after the publish        X order V+P, H, G, S

R, the renderer acting, is renderer.c's rendererThread with rendererShouldWait: a pending copy wakes it
whatever the gate says (gpuCopyPending is checked before waitForNextFrame); with the gate open and a
draw requested or a copy pending it runs a GL frame (rendererRedrawLocked) - drains every queued copy,
draws (A: the live root as it is now; B, C: the newest publish, rendererClaimRootBuffer), clears
drawRequested and closes the gate (waitForNextFrame = true, unconditionally); with the gate closed and a
copy pending it drains alone (rendererApplyPendingGpuCopies). With nothing to do it sleeps.

XC is the compositor (xfwm4, XRender, an EXA fallback on the X server's thread) putting the window's
newest drained content into the root - A: the live root, B and C: the slot being drawn into. It can only
run once the copy it shows has been drained, and only between two ticks (the tick is one work proc).

A GL frame is shown at the vsync after it; the last one between two vsyncs wins; none means the vsync
shows the same content again. The client presents a new frame every tick, so content id = the tick it
was presented in.

Counted per structure, over the ticks after the warm-up:
  repeat   a vsync showing the same content as the one before it
  skip     a client frame never shown (the displayed content jumped over it)
  unshown  B, C: a publish replaced before any GL frame drew it; A: a composite overwritten undrawn
  latency  for each newly shown content, the vsync it was shown at minus the tick it was presented at

Variants (for negative controls; nothing in the sources): D = B, but a GL frame that finds nothing new
published does not take the vsync (what b911041 did for ROOT_DIRECT).
"""
import argparse, collections, functools, itertools, sys

STRUCTURES = ("A", "B", "C", "D")
ORDER = {"A": ("VP", "G", "H", "S"), "B": ("VP", "G", "H", "S"), "C": ("VP", "H", "G", "S"),
         "D": ("VP", "G", "H", "S")}


# --- one structure's state -------------------------------------------------------------------------
# (gate_closed, draw_req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, displayed, comp_drawn)
#   queue      frame ids whose copy is queued
#   W          newest frame drained into the window's pixmap; wdirty: not composited since
#   target     A: the live root's content; B-D: the drawing slot's;  tdmg: damaged since the last tick (A)
#              or the last publish (B-D)
#   pub        B-D: the newest publish's content; pub_drawn: a GL frame has drawn it
#   frame      content of the last GL frame since the last vsync, or None
#   comp_drawn A: the live root's current composite has been read by a GL frame

def initial(s):
    return (True, False, (), 0, False, 0, False, 0, True, None, 0, True)


def x_step(s, st, step, k, m):
    gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn = st
    if step == "VP":
        # the vsync: what the last GL frame drew is shown now
        new = frame if frame is not None else disp
        if m is not None:
            if new == disp:
                m["repeat"] += 1
            else:
                m["skip"] += max(0, new - disp - 1)
                m["lat%d" % min(k - new, 3)] += 1
        disp, frame = new, None
        queue = queue + (k,)                 # the client's frame k: its copy queued
    elif step == "G":
        gate = False
    elif step == "H":
        if s == "A":
            if tdmg:
                req, tdmg = True, False
        elif tdmg:
            if not pub_drawn and m is not None:
                m["unshown"] += 1
            pub, pub_drawn, tdmg, req = target, False, False, True
    return (gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn)


def r_step(s, st, m, variant):
    gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn = st
    if not gate and (req or queue):
        if queue:
            W, wdirty, queue = max(queue), True, ()
        if s == "A":
            frame, comp_drawn = target, True
        else:
            nothing_new = pub_drawn
            frame, pub_drawn = pub, True
        req = False
        if not (s == "D" and nothing_new):
            gate = True
    elif queue:
        W, wdirty, queue = max(queue), True, ()
    return (gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn)


def c_step(s, st, m):
    gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn = st
    if wdirty:
        if s == "A" and not comp_drawn and target != W and m is not None:
            m["unshown"] += 1
        target, tdmg, wdirty, comp_drawn = W, True, False, False
    return (gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn)


# --- one interval's interleaving: R tokens after each X step, XC among those after the last ----------
def interval_orders(max_r):
    out = []
    for r in itertools.product(range(max_r + 1), repeat=4):
        if sum(r) > max_r:
            continue
        for xc in [None] + list(range(r[3] + 1)):
            out.append((r, xc))
    return out


# --- interleavings --------------------------------------------------------------------------------
# A renderer that answers every signal within the tick: the copy's signal (from V+P) at w1 - before G
# (slot 0), between G and H (1), between H and S (2) or after S (3) - and the signal after S, after S.
# The compositor runs after the GL frame, before it (only possible when the copy was drained alone
# before G, so the GL frame waits for the signal after S), or not this tick ("late": the next tick's
# composite shows whatever was drained by then). Slots are positions in time, the same for every
# structure: in C, whose X server publishes before it opens the gate, slot 1 lies between H and G.
#
#   K1  w1 in {before G, H..S, after S}, composite after the frame
#   K2  K1, and w1 between G and H                         <- the race C1 is about
#   K3  K2, and the composite before the frame
#   K4  K3, and the composite a tick late
#   K5  no assumption at all: up to three renderer actions anywhere in the tick, the compositor
#       anywhere after the signal, or neither (a renderer that sleeps through a signal, and so on)
def responsive_orders(cls):
    out = []
    for w1 in range(4):
        for comp in ("after", "before", "late"):
            if comp == "before" and w1 != 0:
                continue
            r = [0, 0, 0, 0]
            r[w1] += 1
            if w1 != 3:
                r[3] += 1                            # the answer to the signal after S
            xc = {"after": r[3], "before": 0, "late": None}[comp]
            if cls == "K1" and (w1 == 1 or comp != "after"):
                continue
            if cls == "K2" and comp != "after":
                continue
            if cls == "K3" and comp == "late":
                continue
            out.append(((tuple(r), xc), w1, comp))
    return out


CLASS_TEXT = {
    "K1": "renderer never first between G and H; composite after the GL frame",
    "K2": "K1 plus the renderer first between G and H",
    "K3": "K2 plus the composite before the GL frame",
    "K4": "K3 plus the composite a tick late",
    "K5": "no assumption: renderer and compositor anywhere, or not at all",
}


def class_orders(cls, max_r=3):
    if cls == "K5":
        return interval_orders(max_r)
    return [o for o, w1, comp in responsive_orders(cls)]


def run_interval(s, st, k, order, m, variant):
    r, xc = order
    for i, step in enumerate(ORDER[s]):
        st = x_step(s, st, step, k, m)
        tokens = ["R"] * r[i]
        if i == 3 and xc is not None:
            tokens.insert(xc, "XC")
        for t in tokens:
            st = r_step(s, st, m, variant) if t == "R" else c_step(s, st, m)
    return st


KEYS = ("repeat", "skip", "unshown", "lat1", "lat2", "lat3")


def norm(st, k):
    """Content ids as ages relative to tick k, so equal situations at different ticks memoise together."""
    gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn = st
    a = lambda f: None if f is None else k - f
    return (gate, req, tuple(k - q for q in queue), a(W), wdirty, a(target), tdmg, a(pub), pub_drawn, a(frame),
            a(disp), comp_drawn)


def denorm(n, k):
    gate, req, queue, W, wdirty, target, tdmg, pub, pub_drawn, frame, disp, comp_drawn = n
    a = lambda f: None if f is None else k - f
    return (gate, req, tuple(k - q for q in queue), a(W), wdirty, a(target), tdmg, a(pub), pub_drawn, a(frame),
            a(disp), comp_drawn)


def explore(ticks, warmup, structures=("A", "B", "C"), max_r=3, variant=None, cls="K5"):
    """Counter: (outcome per structure) -> number of interleavings, over ticks 1..ticks, every tick's
    interleaving drawn from class cls. An outcome is the tuple of KEYS counted from tick warmup+1 on (the
    vsync at tick k shows what interval k-1 drew)."""
    variant = variant or {}
    orders = class_orders(cls, max_r)

    @functools.lru_cache(maxsize=None)
    def go(k, nstates):
        if k > ticks:
            return {tuple((0,) * len(KEYS) for _ in structures): 1}
        # Interleavings that leave every structure in the same state with the same counts are the same
        # future: grouped, and the future explored once for each group.
        groups = collections.Counter()
        for order in orders:
            nxt, inc = [], []
            for s, n in zip(structures, nstates):
                st = denorm(n, k - 1)
                m = collections.Counter() if k > warmup else None
                st = run_interval(s, st, k, order, m, variant)
                nxt.append(norm(st, k))
                inc.append(tuple((m or {}).get(key, 0) for key in KEYS))
            groups[(tuple(nxt), tuple(inc))] += 1
        result = collections.Counter()
        for (nxt, inc), mult in groups.items():
            for fut, cnt in go(k + 1, nxt).items():
                result[tuple(tuple(a + b for a, b in zip(i, f)) for i, f in zip(inc, fut))] += cnt * mult
        return dict(result)

    start = tuple(norm(initial(s), 0) for s in structures)
    return collections.Counter(go(1, start))


def defective(o):
    return o[0] > 0 or o[1] > 0 or o[2] > 0


def latencies(o):
    return [i + 1 for i, v in enumerate(o[3:]) if v]


def summarise(res, structures=("A", "B", "C")):
    total = sum(res.values())
    s = {"total": total}
    for i, name in enumerate(structures):
        s[name + "_defective"] = sum(c for o, c in res.items() if defective(o[i]))
        s[name + "_latencies"] = sorted({l for o in res for l in latencies(o[i])})
        s[name + "_nonconstant_latency"] = sum(c for o, c in res.items() if len(latencies(o[i])) > 1)
        for j, key in enumerate(KEYS):
            s[name + "_" + key] = sum(o[i][j] * c for o, c in res.items())
    if "A" in structures:
        ia = structures.index("A")
        for i, name in enumerate(structures):
            if name != "A":
                s[name + "_defective_where_A_clean"] = sum(c for o, c in res.items()
                                                           if defective(o[i]) and not defective(o[ia]))
                s["A_defective_where_" + name + "_clean"] = sum(c for o, c in res.items()
                                                                if defective(o[ia]) and not defective(o[i]))
    return s


def witness(ticks, want, structures=("A", "B", "C"), max_r=3, cls="K5", warmup=2):
    """The first interleaving (fewest tokens first, tick by tick) whose per-structure outcomes, counted over
    every tick, satisfy want(). Breadth first over the joint state, keeping one path per state and set of
    counts."""
    orders = sorted(class_orders(cls, max_r), key=lambda o: (sum(o[0]) + (o[1] is not None), o))
    frontier = {(tuple(initial(s) for s in structures), tuple((0,) * len(KEYS) for _ in structures)): []}
    for k in range(1, ticks + 1):
        nxt_frontier = {}
        for (sts, acc), path in frontier.items():
            for order in orders:
                nsts, nacc = [], []
                for s, st, a in zip(structures, sts, acc):
                    m = collections.Counter()
                    nsts.append(run_interval(s, st, k, order, m if k > warmup else None, {}))
                    nacc.append(tuple(a[j] + m.get(key, 0) for j, key in enumerate(KEYS)))
                key = (tuple(norm(x, k) for x in nsts), tuple(nacc))
                if key not in nxt_frontier:
                    nxt_frontier[key] = (tuple(nsts), tuple(nacc), path + [order])
        frontier = {}
        for key, (sts, acc, path) in nxt_frontier.items():
            if want(acc):
                return path
            frontier[(sts, acc)] = path
    return None


def describe(path, s):
    """The tick-by-tick event sequence of an interleaving, as structure s orders its X steps."""
    out = []
    for k, (r, xc) in enumerate(path, 1):
        ev = []
        for i, step in enumerate(ORDER[s]):
            ev.append(step.replace("VP", "V%d+P%d" % (k, k)) if step == "VP" else step + str(k))
            toks = ["R"] * r[i]
            if i == 3 and xc is not None:
                toks.insert(xc, "XC")
            ev.extend(toks)
        out.append(" ".join(ev))
    return " | ".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ticks", type=int, default=7)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--witness", action="store_true")
    args = ap.parse_args()
    names = ("A", "B", "C", "D")
    for cls in ("K1", "K2", "K3", "K4", "K5"):
        res = explore(args.ticks, args.warmup, names, cls=cls)
        s = summarise(res, names)
        print("== %s (%s): %d interleavings, ticks %d, counted from tick %d" % (
            cls, CLASS_TEXT[cls], s["total"], args.ticks, args.warmup + 1))
        for name in names:
            print("   %s  defective %12d  repeat %12d  skip %12d  unshown %12d  latencies %-9s non-constant %d" % (
                name, s[name + "_defective"], s[name + "_repeat"], s[name + "_skip"], s[name + "_unshown"],
                s[name + "_latencies"], s[name + "_nonconstant_latency"]))
        for name in ("B", "C", "D"):
            print("   %s defective where A is clean %12d   A defective where %s is clean %12d" % (
                name, s[name + "_defective_where_A_clean"], name, s["A_defective_where_" + name + "_clean"]))
    if args.witness:
        w = witness(4, lambda acc: defective(acc[1]) and not defective(acc[0]) and not defective(acc[2]), cls="K2")
        if w:
            print("witness (K2): B repeats or skips, A and C do not")
            for name in ("A", "B", "C"):
                print("   %s: %s" % (name, describe(w, name)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
