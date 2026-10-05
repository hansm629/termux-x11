#!/usr/bin/env python3
"""A deterministic model of the ROOT_DIRECT root-slot lifecycle, against the compositor with buffer
backpressure off (what the NDK gives a SurfaceControl by default) and on (what BLASTBufferQueue sets).

usage: zcslots.py [--slots N ...] [--scenario NAME ...]

The renderer and X server rules are the ones in the sources, in the same order:

  X server (InitOutput.c lorieRootHandover): at a tick with new content it publishes the slot it drew
    into and moves on to the first slot that is neither that one nor held; with none, it does not
    publish and keeps drawing into the same slot (rootPublishNoSlot).
  renderer (renderer.c rendererRedrawLocked -> rootZcPresent): a frame claims the newest published
    slot (its held bit), then rootZcDrainRetiring gives back every retiring slot whose completion
    has arrived and whose release fence has signalled (or came as -1), and goes on only if
    kept + 2 <= LORIE_ZC_MAX_HELD (= slots - 2); otherwise the frame is dropped, the claim given back
    unless it is a slot it holds anyway, and retried at the next vsync. A claim of the slot it last
    submitted is "nothing new" and submits nothing. Otherwise, after the frame's fence wait, the slot
    it submitted last joins the retiring list under this transaction's number, the new slot becomes
    rootZcDisplayedSlot, and the transaction is applied. waitForNextFrame shuts until the next tick.
  completion (rootZcOnComplete): transaction N's callback hands its previous release fence to the
    retiring entry with N's number.

The compositor follows AOSP (frameworks/native, main): SurfaceFlinger.cpp transactionReadyBufferCheck,
Layer.cpp setBuffer/releasePreviousBuffer/findCallbackHandle/updateTexImage.

  A transaction is ready for a latch if it was applied MARGIN before it.
  Backpressure off: every ready one is applied to the layer that frame; the last is latched, the ones
    before it are dropped and their buffers released at once. Of the frame's callbacks only the first
    carries a previous release fence - for the buffer that was on screen; the others carry none, the
    buffer before each having been dropped. Only the latched one gets a latch time.
  Backpressure on (and an automatic timestamp, as here): with a buffer already going to be presented
    from this flush, the next transaction stays queued - one buffer per latch, in order.
  Each latched buffer goes on screen at the next vsync; the one it replaced is released then
  (+ the release-fence delay). Callbacks for a frame come CALLBACK after its latch.

What is checked, all the time: the held bits are exactly the renderer's claim, its last submitted
slot and its retiring slots (no leak, nothing unprotected); the renderer never gives back a slot the
compositor still has; the X server never draws into or moves on to such a slot.
"""
import argparse, heapq, sys

P = 16667          # 60 Hz
TICK = 300         # the X server takes the vsync up this long after it
WAKE = 200         # and the renderer a publish
LATCH = 10860      # the compositor latches this long after the vsync (829b4ec trace, p50)
CALLBACK = 2240    # the completion callback comes this long after the latch (same trace, p50)
COMMIT = 500       # an OnCommit callback (API 31): sent when the transaction is latched
MARGIN = 300       # applied at least this long before a latch to make it (trace replay)
WORK = 2500        # an ordinary frame: claim -> apply
SLOW = 13000       # a frame applied after the latch but before the next tick (what superseding needs)
SLOWER = 18000     # one applied after the next tick as well

class Scenario:
    def __init__(self, name, ticks=240, work=None, cbLate=None, relLate=None, content=None, extra=None, sfSkip=None,
                 commitLate=None):
        self.name, self.ticks = name, ticks
        self.work = work or (lambda k: WORK)            # claim -> apply for the frame published at tick k
        self.cbLate = cbLate or (lambda k: 0)           # extra callback delay for a latch in vsync k
        self.relLate = relLate or (lambda k: 0)         # extra release-fence delay for a switch at vsync k
        self.content = content or (lambda k: True)      # whether the client has a new frame at tick k
        self.extra = extra or (lambda k: False)         # a second publish and gate opening mid-vsync
        self.sfSkip = sfSkip or (lambda k: False)       # the compositor latches nothing this vsync
        self.commitLate = commitLate or (lambda k: 0)   # extra OnCommit delay for a latch in vsync k

def deep(k):
    """one slow frame at tick 90 first: with backpressure on, the pipeline is a frame deeper from there"""
    return SLOW if k == 90 else WORK

def spread(k, lo, hi):
    return lo + (k * 7919) % (hi - lo + 1)

SCENARIOS = [
    Scenario("steady 60Hz"),
    Scenario("one slow frame", work=lambda k: SLOW if k == 100 else WORK),
    Scenario("five slow frames in a row", work=lambda k: SLOW if 100 <= k < 105 else WORK),
    Scenario("one frame slower than a whole period", work=lambda k: SLOWER if k == 100 else WORK),
    Scenario("a slow frame every 10 ticks", work=lambda k: SLOW if k % 10 == 5 else WORK),
    Scenario("callbacks a frame late", work=deep, cbLate=lambda k: P if 100 <= k < 160 else 0),
    Scenario("release fences a frame late", work=deep, relLate=lambda k: P if 100 <= k < 160 else 0),
    Scenario("callbacks and release fences a frame late", work=deep,
             cbLate=lambda k: P if 100 <= k < 160 else 0, relLate=lambda k: P if 100 <= k < 160 else 0),
    Scenario("release fences 0-2 ms after the vsync", work=deep, relLate=lambda k: spread(k, 0, 2000)),
    Scenario("release fence after the claim in 6% of frames", work=deep,
             relLate=lambda k: 1500 if (k * 7919) % 100 < 6 else 0),
    Scenario("slow frames every 10 ticks, release fences 0-2 ms late",
             work=lambda k: SLOW if k % 10 == 5 else WORK, relLate=lambda k: spread(k, 0, 2000)),
    Scenario("client faster than the display", extra=lambda k: 100 <= k < 130),
    Scenario("compositor skips a latch every 20 vsyncs", sfSkip=lambda k: k % 20 == 7 and k < 240),
    Scenario("commit and complete callbacks a frame late", work=deep,
             cbLate=lambda k: P if 100 <= k < 160 else 0, commitLate=lambda k: P if 100 <= k < 160 else 0),
    Scenario("client stops after one queued frame, then resumes",
             work=lambda k: SLOW if k == 119 else WORK, content=lambda k: not 120 <= k < 180),
]

class Violation(Exception):
    pass

class Sim:
    def __init__(self, slots, backpressure, sc, bugs=(), cap=None):
        self.n, self.maxHeld, self.bp, self.sc, self.bugs = slots, slots - 2, backpressure, sc, set(bugs)
        # a depth cap: no frame while two submitted transactions are not known to be latched yet, known
        # from OnCommit ("commit") or from OnComplete ("complete")
        self.cap = cap
        self.knownLatched = set()
        self.held = set()
        self.displayed = None           # rootZcDisplayedSlot: the slot submitted last
        self.retiring = []              # [seq, slot, arrived, fenceAt]
        self.claimed = None
        self.claimWord = 0
        self.drawn, self.newest, self.pubCount = 0, None, 0
        self.pending = False            # content the X server has not published yet
        self.drawRequested = self.retry = False
        self.gateOpen = False
        self.busy = False
        self.seq = 0
        self.sfQueue = []               # applied, not taken by a latch yet
        self.onScreen = None            # the tx on screen
        self.sfHas = {}                 # slot -> what the compositor is doing with it
        self.ev, self.order = [], 0
        self.publishTick = {}           # slot -> tick its content was published at
        self.txs = []
        # results
        self.presented, self.dropped, self.publishStalls, self.frameStalls = [], 0, 0, 0
        self.heldMax = self.heldWithClaimMax = self.queuedMax = 0
        self.xAvailMin = slots
        self.publishedAt = []
        self.sfSkips = self.extraPubs = self.overruns = 0
        self.contentTicks = []

    def at(self, t, kind, *data):
        self.order += 1
        heapq.heappush(self.ev, (t, self.order, kind, data))

    def fail(self, t, what):
        raise Violation("%.1f ms: %s" % (t / 1000.0, what))

    def check(self, t):
        owned = {s for s in [self.claimed, self.displayed] + [e[1] for e in self.retiring] if s is not None}
        if self.held != owned:
            self.fail(t, "held bits %s, renderer accounts for %s" % (sorted(self.held), sorted(owned)))
        if self.drawn in self.held or self.drawn in self.sfHas:
            self.fail(t, "the X server draws into slot %d, which is %s" % (
                self.drawn, "held" if self.drawn in self.held else self.sfHas[self.drawn]))
        n = len({s for s in [self.displayed] + [e[1] for e in self.retiring] if s is not None})
        self.heldMax = max(self.heldMax, n)
        self.heldWithClaimMax = max(self.heldWithClaimMax, len(self.held))
        if n > self.maxHeld:
            self.fail(t, "renderer holds %d after a submit, over %d" % (n, self.maxHeld))

    # ---- X server
    def tick(self, t, k):
        self.gateOpen = True                              # loriePerformVblanks; waitForNextFrame = false
        if self.sc.content(k):
            self.pending = True
            self.contentTicks.append(k)
        if self.pending:
            self.publish(t, k)
        self.wake(t)

    def publish(self, t, k):
        free = [i for i in range(self.n) if i not in self.held]
        self.xAvailMin = min(self.xAvailMin, len(free))
        nxt = None
        for i in range(self.n):
            if i != self.drawn and (i not in self.held or "x-ignores-held" in self.bugs):
                nxt = i
                break
        if nxt is None:
            self.publishStalls += 1
            return
        if nxt in self.sfHas:
            self.fail(t, "the X server moves on to slot %d, which the compositor has (%s)" % (nxt, self.sfHas[nxt]))
        self.newest = self.drawn
        self.publishTick[self.drawn] = k
        self.pubCount += 1
        self.publishedAt.append(k)
        self.drawn = nxt
        self.pending = False
        self.drawRequested = True

    # ---- renderer
    def wake(self, t):
        if not self.busy and self.gateOpen and (self.drawRequested or self.retry) and self.newest is not None:
            self.busy = True
            self.at(t + WAKE, "frame")

    def drainRetiring(self, t):
        kept = []
        for e in self.retiring:
            seq, slot, arrived, fenceAt = e
            done = arrived and (fenceAt is None or fenceAt <= t or "no-fence-wait" in self.bugs)
            if done:
                if slot in self.sfHas:
                    self.fail(t, "slot %d given back while the compositor has it (%s)" % (slot, self.sfHas[slot]))
                self.held.discard(slot)
            else:
                kept.append(e)
        self.retiring = kept
        need = 1 if "room-plus-one" in self.bugs else 2
        return len(kept) + need <= self.maxHeld

    def frame(self, t):
        self.drawRequested = False
        self.claimed = self.newest
        self.held.add(self.claimed)
        self.claimWord = self.pubCount
        self.check(t)
        unlatched = sum(1 for tx in self.txs if tx["seq"] not in self.knownLatched)
        capped = self.cap is not None and unlatched >= 2
        if capped or not self.drainRetiring(t):
            self.frameStalls += 1
            mine = self.claimed == self.displayed or any(e[1] == self.claimed for e in self.retiring)
            if not mine:
                self.held.discard(self.claimed)
            self.claimed = None
            self.retry = True
            self.gateOpen = False
            self.busy = False
            self.check(t)
            return
        if self.claimed == self.displayed:                # already submitted: nothing new
            self.claimed = None
            self.retry = False
            self.busy = False
            if self.pubCount != self.claimWord:
                self.drawRequested = True
            self.check(t)
            self.wake(t)
            return
        if self.claimed in self.sfHas:
            self.fail(t, "the frame copies into slot %d, which the compositor has" % self.claimed)
        work = self.sc.work(self.publishTick[self.claimed])
        if work > P - WAKE:
            self.overruns += 1                            # busy through the next tick: its content is lost anyway
        self.at(t + work, "apply")

    def apply(self, t):
        self.seq += 1
        if self.displayed is not None:
            self.retiring.append([self.seq, self.displayed, False, None])
        self.displayed = self.claimed
        self.claimed = None
        tx = {"seq": self.seq, "slot": self.displayed, "applied": t, "pub": self.publishTick[self.displayed]}
        self.txs.append(tx)
        self.sfQueue.append(tx)
        self.sfHas[tx["slot"]] = "queued"
        self.queuedMax = max(self.queuedMax, len(self.sfQueue))
        self.retry = self.pubCount != self.claimWord
        self.gateOpen = False
        self.busy = False
        self.check(t)
        self.wake(t)

    def known(self, t, seq):
        self.knownLatched.add(seq)

    def complete(self, t, seq, fenceAt):
        if self.cap == "complete":
            self.knownLatched.add(seq)
        for e in self.retiring:
            if e[0] == seq and not e[2]:
                e[2], e[3] = True, fenceAt
                return

    # ---- compositor
    def latch(self, t, k):
        if self.sc.sfSkip(k):
            self.sfSkips += 1
            return
        ready = [tx for tx in self.sfQueue if tx["applied"] <= t - MARGIN]
        if not ready:
            return
        flush = ready[:1] if self.bp else ready
        for tx in flush:
            self.sfQueue.remove(tx)
        latched, dropped = flush[-1], flush[:-1]
        switch = (k + 1) * P
        release = switch + self.sc.relLate(k + 1)
        cb = t + CALLBACK + self.sc.cbLate(k)
        for i, tx in enumerate(flush):
            # only the first callback of the frame says when the buffer on screen is released
            first = len(flush) - 1 if "fence-to-last" in self.bugs else 0
            fence = (release if self.onScreen is not None else None) if i == first else None
            self.at(cb, "complete", tx["seq"], fence)
            if self.cap == "commit":
                self.at(t + COMMIT + self.sc.commitLate(k), "known", tx["seq"])
        for tx in dropped:
            del self.sfHas[tx["slot"]]
            self.dropped += 1
        self.sfHas[latched["slot"]] = "latched"
        self.at(switch, "switch", latched, release)

    def switch(self, t, tx, release):
        old = self.onScreen
        self.onScreen = tx
        self.sfHas[tx["slot"]] = "on screen"
        self.presented.append((tx["seq"], t // P - tx["pub"]))
        if old is not None:
            self.sfHas[old["slot"]] = "being released"
            self.at(release, "released", old["slot"], old["seq"])

    def released(self, t, slot, seq):
        if self.sfHas.get(slot) == "being released":
            del self.sfHas[slot]

    def run(self):
        end = self.sc.ticks + 30                          # then 30 ticks of new content to settle
        for k in range(end + 10):                         # the compositor carries on after the last tick
            v = k * P
            if k < end:
                self.at(v + TICK, "tick", k)
            self.at(v + LATCH, "latch", k)
            if k < self.sc.ticks and self.sc.extra(k):
                self.at(v + 8000, "extra", k)
        content = self.sc.content
        self.sc.content = lambda k: True if k >= self.sc.ticks else content(k)
        while self.ev:
            t, _, kind, data = heapq.heappop(self.ev)
            if kind == "tick": self.tick(t, *data)
            elif kind == "extra":
                self.extraPubs += 1
                self.pending = True
                self.publish(t, data[0])
                self.gateOpen = True                      # a second redraw, or a retry, in the same vsync
                self.wake(t)
            elif kind == "frame": self.frame(t)
            elif kind == "apply": self.apply(t)
            elif kind == "latch": self.latch(t, *data)
            elif kind == "complete": self.complete(t, *data)
            elif kind == "known": self.known(t, *data)
            elif kind == "switch": self.switch(t, *data)
            elif kind == "released": self.released(t, *data)
            self.check(t)
        return self.result()

    def result(self):
        seqs = [s for s, _ in self.presented]
        inOrder = seqs == sorted(seqs)
        lat = sorted(l for _, l in self.presented)
        # half rate: 8 ticks in a row with new content but no more than 4 publishes among them
        pubs, content, episodes, run = set(self.publishedAt), set(self.contentTicks), 0, False
        for k in range(self.sc.ticks + 30 - 8):
            window = range(k, k + 8)
            half = all(i in content for i in window) and sum(i in pubs for i in window) <= 4
            if half and not run:
                episodes += 1
            run = half
        lost = [tx["seq"] for tx in self.txs if tx["seq"] not in set(seqs)]
        submitted = {tx["pub"] for tx in self.txs}
        skipped = sum(1 for k in set(self.publishedAt) if k not in submitted)
        return dict(presented=len(self.presented), dropped=self.dropped, publishStalls=self.publishStalls,
                    frameStalls=self.frameStalls, heldMax=self.heldMax, heldWithClaimMax=self.heldWithClaimMax,
                    queuedMax=self.queuedMax, xAvailMin=self.xAvailMin, inOrder=inOrder,
                    latency=(lat[len(lat) // 2], lat[-1]) if lat else (None, None),
                    halfRate=episodes, notPresented=len(lost), leftQueued=len(self.sfQueue), skipped=skipped,
                    unavoidable=self.sfSkips + self.extraPubs + self.overruns)

def verdict(r, bp):
    """bp: the criteria for backpressure on. Off, superseding is what it does - reported, not failed."""
    why = []
    if r["xAvailMin"] < 2: why.append("X server down to %d slot(s)" % r["xAvailMin"])
    if r["halfRate"]: why.append("%d half-rate episode(s)" % r["halfRate"])
    if bp and r["dropped"]: why.append("%d dropped with backpressure on" % r["dropped"])
    if bp and r["notPresented"]: why.append("%d never presented" % r["notPresented"])
    lost = r["skipped"] + r["dropped"]
    if bp and lost > r["unavoidable"]:
        why.append("%d published frames lost, %d of them unavoidable" % (lost, r["unavoidable"]))
    if bp and r["latency"][0] is not None and r["latency"][0] > 2:
        why.append("latency grew to %d frames" % r["latency"][0])
    if not r["inOrder"]: why.append("presented out of order")
    return why

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slots", type=int, nargs="*", default=[5, 6, 7])
    ap.add_argument("--scenario", nargs="*")
    ap.add_argument("--bug", nargs="*", default=[], help="seed a rule change, to show the checks catch it")
    ap.add_argument("--caps", action="store_true", help="also the depth caps")
    args = ap.parse_args()
    configs = [("A", 5, False, None)] + [(chr(ord("B") + i), n, True, None) for i, n in enumerate(args.slots)]
    if args.caps:
        configs += [("%s+cap(%s)" % (chr(ord("B") + i), c), n, True, c) for c in ("commit", "complete")
                    for i, n in enumerate(args.slots) if n <= 6]
    status = 0
    summary = []
    for name, slots, bp, cap in configs:
        if name != "A" and slots not in args.slots:
            continue
        print("== %s. %d slots, backpressure %s (LORIE_ZC_MAX_HELD %d)%s" % (
            name, slots, "ON" if bp else "OFF", slots - 2, ", depth cap known from On%s" % cap.capitalize() if cap else ""))
        agg = dict(presented=0, dropped=0, skipped=0, publishStalls=0, frameStalls=0, heldMax=0, heldWithClaimMax=0,
                   queuedMax=0, xAvailMin=slots, latency=0, halfRate=0, fails=[], violations=0)
        for sc in SCENARIOS:
            if args.scenario and sc.name not in args.scenario:
                continue
            try:
                r = Sim(slots, bp, sc, args.bug, cap).run()
            except Violation as v:
                print("  %-48s VIOLATION %s" % (sc.name, v))
                agg["violations"] += 1
                status = 1
                continue
            why = verdict(r, bp)
            for key in ("presented", "dropped", "skipped", "publishStalls", "frameStalls", "halfRate"):
                agg[key] += r[key]
            for key in ("heldMax", "heldWithClaimMax", "queuedMax"):
                agg[key] = max(agg[key], r[key])
            agg["xAvailMin"] = min(agg["xAvailMin"], r["xAvailMin"])
            agg["latency"] = max(agg["latency"], r["latency"][0] or 0)
            if why:
                agg["fails"].append(sc.name)
            print("  %-48s %s presented %d dropped %d skipped %d publish-stalls %d frame-stalls %d held %d (+claim %d) "
                  "SF-queued %d X-avail-min %d latency %s/%s half-rate %d%s" % (
                      sc.name, "PASS" if not why else "FAIL", r["presented"], r["dropped"], r["skipped"], r["publishStalls"],
                      r["frameStalls"], r["heldMax"], r["heldWithClaimMax"], r["queuedMax"], r["xAvailMin"],
                      r["latency"][0], r["latency"][1], r["halfRate"], (" - " + "; ".join(why)) if why else ""))
            if why and bp:
                status = 1
        summary.append((name, slots, bp, cap, agg))
    print("== summary over all scenarios")
    for name, slots, bp, cap, a in summary:
        print("  %-16s %s  presented %d dropped %d skipped %d publish-stalls %d frame-stalls %d renderer-held max %d "
              "(+claim %d) SF-queued max %d X-available min %d latency p50 up to %d half-rate %d violations %d%s" % (
                  name, "PASS" if not a["fails"] and not a["violations"] else "FAIL", a["presented"], a["dropped"],
                  a["skipped"], a["publishStalls"], a["frameStalls"], a["heldMax"], a["heldWithClaimMax"],
                  a["queuedMax"], a["xAvailMin"], a["latency"], a["halfRate"], a["violations"],
                  (" - fails: " + ", ".join(a["fails"])) if a["fails"] else ""))
    return status

if __name__ == "__main__":
    sys.exit(main())
