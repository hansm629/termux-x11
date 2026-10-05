#!/usr/bin/env python3
"""A deterministic model of the ROOT_DIRECT root-slot lifecycle against the compositor, to choose how frames
are paced before anything in the app changes.

usage: zcslots.py [--hz 60 120] [--policy A B ...] [--scenario NAME ...] [--lifecycle] [--delays]

Policies (renderer side), all with the X server and renderer rules as they are in the sources:
  A        5 slots, backpressure off, transactions applied at once            (what the app does now)
  B        5 slots, backpressure on, applied at once
  C-apply  5 slots, backpressure off, OnCommit-paced: transaction N+1 is applied only once N's OnCommit has
           come, waiting right before txApply (claim, room check and copies as now)
  C-late   C-apply, with the room check moved from the start of the frame to right before txApply (after
           the wait): retired slots are given back at both points
  C-claim  the same as C-apply, but the frame - claim, room check, copies - does not start before it
  D        6 slots, backpressure on, no frame starts while submitted - committed >= 2
           (uncommittedDepth <= 1 before a submit)
  D-wait   D, but a frame that would exceed the depth waits for the OnCommit instead of being retried
           at the next vsync
  D-nocap  6 slots, backpressure on, no depth bound
  E        6 slots, backpressure off, C-apply

Rules taken from the sources:
  X server (InitOutput.c lorieRootHandover): publishes the slot it drew into at a tick with new content
    and moves on to the first slot that is neither that one nor held; with none, it keeps drawing into
    the same one (rootPublishNoSlot). Pool replacement (resize): a new generation, every held bit cleared.
  renderer (renderer.c): a frame claims the newest slot (held bit), rootZcDrainRetiring gives back every
    retiring slot of the current generation whose completion came and whose release fence has signalled
    (entries of an older generation are dropped without touching held bits), and the frame goes on only
    if kept + 2 <= slots - 2; else the claim goes back unless it is held anyway, and the frame is retried
    at the next vsync. A claim of the slot submitted last is "nothing new". Otherwise, once its copies are
    done (waitForNextFrame shut there, rootZcFrameDrained), the slot submitted before joins the retiring
    list under the new transaction's number, and the transaction is applied. Completions are matched by
    that number (rootZcOnComplete). rootZcStopPresenting hides the layer and retires the slot submitted
    last under the hide transaction's number. Off the root layer, a frame samples the newest slot and
    gives it back (the GL path).

The compositor, from AOSP main (frameworks/native):
  - a transaction is ready for the flush of a latch if applied MARGIN before it; the flush is the start
    of SurfaceFlinger::commit, the latch inside it (latchBuffers);
  - backpressure off: every ready transaction is applied; the last buffer is latched, the ones before it
    released at once. Only the frame's first buffer callback gets a previous release fence - for the
    buffer that was on screen (Layer.cpp releasePreviousBuffer, findCallbackHandle, updateTexImage);
  - backpressure on (automatic timestamps): with a buffer already ready in this flush the next one stays
    queued (SurfaceFlinger.cpp transactionReadyBufferCheck);
  - a transaction without a buffer change gets no previous release fence (releasePreviousBuffer is reset
    after every handle, Layer::setTransactionCompletedListeners);
  - OnCommit (NDK: "applied and the updates are ready to be presented ... any new transactions applied
    will not overwrite the transaction ... and instead will be included in the next frame"; no release or
    present fence) is sent from commit, after latchBuffers: here the latch + commitAt (scenarios move it
    to just before), reaching the client commitDelay later. It is "committed / overwrite-safe", not
    latched, presented or released;
  - OnComplete comes CALLBACK after the latch, with the previous release fence (or none); the latched
    buffer goes on screen at the next vsync, the one it replaced is read until then and its release fence
    signals then (+ the scenario's delay). A hidden layer is no longer scanned out from the next vsync.

Checked all the time: the held bits of the current pool are exactly the claim, the slot submitted last
and the retiring slots; no slot goes back to the X server while the compositor still reads or holds it;
the X server never draws into or moves on to such a slot.
"""
import argparse, heapq, sys

class Timing:
    def __init__(self, hz):
        self.hz = hz
        self.P = int(round(1e6 / hz))
        self.TICK, self.WAKE = 300, 200
        self.LATCH = int(self.P * 0.6516)      # 10.86 ms at 60 Hz (829b4ec trace p50); the same phase at 120
        self.CALLBACK = 2240                   # OnComplete after the latch (trace p50)
        self.MARGIN = 300                      # applied this long before a latch to make it (trace replay)
        self.WORK = 2500                       # claim -> copies done, an ordinary frame
        self.SLOW = self.LATCH + (self.P - self.LATCH) // 2   # done after the latch, before the next tick
        self.SLOWER = self.P + 1500            # done after the next tick as well

COMMIT_AT = 0          # internal OnCommit, relative to the latch
COMMIT_DELAY = 500     # and on to the client

class Policy:
    def __init__(self, name, slots, bp, pace=None, depth=None):
        self.name, self.slots, self.bp, self.pace, self.depth = name, slots, bp, pace, depth
        self.lateRoom = name == "C-late"

POLICIES = {
    "A": Policy("A", 5, False),
    "B": Policy("B", 5, True),
    "C-apply": Policy("C-apply", 5, False, pace="apply"),
    "C-late": Policy("C-late", 5, False, pace="apply"),
    "C-claim": Policy("C-claim", 5, False, pace="claim"),
    "D": Policy("D", 6, True, depth=1),
    "D-wait": Policy("D-wait", 6, True, pace="depth", depth=1),
    "D-nocap": Policy("D-nocap", 6, True),
    "E": Policy("E", 6, False, pace="apply"),
}

class Scenario:
    def __init__(self, name, ticks=240, **kw):
        self.name, self.ticks = name, ticks
        none = lambda k: 0
        self.work = kw.get("work")                       # (timing, k) -> claim -> copies done
        self.cbLate = kw.get("cbLate", none)             # extra OnComplete delay, latch in vsync k
        self.relLate = kw.get("relLate", none)           # extra release delay, switch at vsync k
        self.commitAt = kw.get("commitAt", lambda k: COMMIT_AT)
        self.commitDelay = kw.get("commitDelay", lambda k: COMMIT_DELAY)
        self.content = kw.get("content", lambda k: True)
        self.extra = kw.get("extra", lambda k: False)    # a second publish and gate opening mid-vsync
        self.sfSkip = kw.get("sfSkip", lambda k: False)
        self.stopAt = kw.get("stopAt")                   # leave the root layer (rootZcStopPresenting)
        self.resumeAt = kw.get("resumeAt")               # come back to it
        self.resizeAt = kw.get("resizeAt")               # the X server replaces the pool
        self.teardownAt = kw.get("teardownAt")           # the surface goes: hide, reparent, a new SurfaceControl
        self.oldCallbacks = kw.get("oldCallbacks", "arrive")   # for the old layer's transactions: arrive / never
        self.midPoll = kw.get("midPoll", lambda k: False)      # a renderer frame (cursor, GL) between a
                                                               # completion and the next vsync

def scenarios(T):
    P = T.P
    deep = lambda T_, k: T_.SLOW if k == 90 else T_.WORK
    every10 = lambda T_, k: T_.SLOW if k % 10 == 5 else T_.WORK
    spread = lambda k, lo, hi: lo + (k * 7919) % (hi - lo + 1)
    late = lambda k: P if 100 <= k < 160 else 0
    return [
        Scenario("steady"),
        Scenario("one slow frame", work=lambda T_, k: T_.SLOW if k == 100 else T_.WORK),
        Scenario("five slow frames in a row", work=lambda T_, k: T_.SLOW if 100 <= k < 105 else T_.WORK),
        Scenario("one frame slower than a whole period", work=lambda T_, k: T_.SLOWER if k == 100 else T_.WORK),
        Scenario("a slow frame every 10 ticks", work=every10),
        Scenario("OnComplete a frame late", work=deep, cbLate=late),
        Scenario("release fences a frame late", work=deep, relLate=late),
        Scenario("OnComplete and release fences a frame late", work=deep, cbLate=late, relLate=late),
        Scenario("release fences 0-2 ms after the vsync", work=deep, relLate=lambda k: spread(k, 0, 2000)),
        Scenario("release fence after the claim in 6% of frames", work=deep,
                 relLate=lambda k: 1500 if (k * 7919) % 100 < 6 else 0),
        Scenario("slow frames every 10 ticks, release fences 0-2 ms late", work=every10,
                 relLate=lambda k: spread(k, 0, 2000)),
        Scenario("client faster than the display", extra=lambda k: 100 <= k < 130),
        Scenario("compositor skips a latch every 20 vsyncs", sfSkip=lambda k: k % 20 == 7),
        Scenario("client stops after one queued frame, then resumes",
                 work=lambda T_, k: T_.SLOW if k == 119 else T_.WORK, content=lambda k: not 120 <= k < 180),
        Scenario("OnCommit internally just before the latch", work=every10, commitAt=lambda k: -T.MARGIN),
        Scenario("OnCommit 3 ms late to the client", work=every10, commitDelay=lambda k: 3000),
        Scenario("OnCommit a frame late, ticks 100-159", work=deep,
                 commitDelay=lambda k: P if 100 <= k < 160 else COMMIT_DELAY),
        Scenario("OnCommit 0.2-6 ms late, slow frames every 10 ticks", work=every10,
                 commitDelay=lambda k: spread(k, 200, 6000)),
    ]

def lifecycle(T):
    deep = lambda T_, k: T_.SLOW if k == 90 else T_.WORK
    return [
        Scenario("stop presenting, then resume", work=deep, stopAt=100, resumeAt=130),
        Scenario("stop presenting, a renderer frame between the hide's callback and the vsync", work=deep,
                 stopAt=100, resumeAt=130, midPoll=lambda k: 100 <= k < 104),
        Scenario("pool replaced (resize) with callbacks in flight", work=deep, resizeAt=100,
                 cbLate=lambda k: T.P if 95 <= k < 105 else 0),
        Scenario("surface torn down with a transaction queued; old callbacks arrive late", work=deep,
                 teardownAt=100, resumeAt=104, cbLate=lambda k: 2 * T.P if 95 <= k < 105 else 0,
                 commitDelay=lambda k: 2 * T.P if 95 <= k < 105 else COMMIT_DELAY),
        Scenario("surface torn down; old callbacks never arrive, then the pool is replaced", work=deep,
                 teardownAt=100, resumeAt=104, oldCallbacks="never", resizeAt=110),
    ]

class Violation(Exception):
    pass

SF_STATE = {"queued": "has it queued", "latched": "has it latched", "on screen": "shows it",
            "being released": "still reads it", "hidden": "still scans it out"}

class Sim:
    def __init__(self, T, pol, sc, bugs=(), fixHide=False):
        self.T, self.pol, self.sc, self.bugs, self.fixHide = T, pol, sc, set(bugs), fixHide
        self.n, self.maxHeld = pol.slots, pol.slots - 2
        self.gen = 0
        self.held = set()                 # held bits of the current pool
        self.displayed = None             # rootZcDisplayedSlot: (gen, idx) submitted last
        self.retiring = []                # dict(seq, slot, arrived, fenceAt)
        self.claimed = None
        self.claimNewest = None           # (gen, newest) at the claim
        self.drawn, self.newest = 0, None
        self.pending = self.drawRequested = self.retry = False
        self.gateOpen = self.busy = self.waitingCommit = False
        self.zc = True
        self.epoch = 0                    # SurfaceControl
        self.seq = 0
        self.submitted, self.committed = {}, {}           # per SurfaceControl
        self.sfQueue = []
        self.onScreen = {}
        self.sfHas = {}                   # (gen, idx) -> what the compositor does with it
        self.ev, self.order = [], 0
        self.publishTick = {}
        self.txs = []
        self.presented, self.dropped, self.publishStalls, self.frameStalls = [], 0, 0, 0
        self.heldMax = self.queuedMax = self.uncommittedMax = 0
        self.xAvailMin = self.n
        self.publishedAt, self.contentTicks = [], []
        self.sfSkips = self.extraPubs = self.overruns = 0
        self.shownVsyncs = []

    def at(self, t, kind, *data):
        self.order += 1
        heapq.heappush(self.ev, (t, self.order, kind, data))

    def fail(self, t, what):
        raise Violation("%.1f ms: %s" % (t / 1000.0, what))

    def cur(self, s):
        return s is not None and s[0] == self.gen

    def check(self, t):
        owned = {s[1] for s in [self.claimed, self.displayed] + [e["slot"] for e in self.retiring] if self.cur(s)}
        if self.held != owned:
            self.fail(t, "held bits %s, the renderer accounts for %s" % (sorted(self.held), sorted(owned)))
        d = (self.gen, self.drawn)
        if self.drawn in self.held or d in self.sfHas:
            self.fail(t, "the X server draws into slot %d, which is %s" % (
                self.drawn, "held" if self.drawn in self.held else self.sfHas[d]))
        n = len({s for s in [self.displayed] + [e["slot"] for e in self.retiring] if self.cur(s)})
        self.heldMax = max(self.heldMax, n)
        if n > self.maxHeld:
            self.fail(t, "the renderer holds %d after a submit, over %d" % (n, self.maxHeld))

    # ---- X server
    def tick(self, t, k):
        self.gateOpen = True
        if self.sc.resizeAt == k:
            self.gen += 1                                 # a new pool: new buffers, every held bit cleared
            self.held = set()
            self.newest = None
            self.drawn = 0
        if self.sc.content(k) or self.sc.resizeAt == k:
            self.pending = True
            self.contentTicks.append(k)
        if self.pending:
            self.publish(t, k)
        self.wake(t)

    def publish(self, t, k):
        self.xAvailMin = min(self.xAvailMin, sum(1 for i in range(self.n) if i not in self.held))
        nxt = next((i for i in range(self.n) if i != self.drawn and
                    (i not in self.held or "x-ignores-held" in self.bugs)), None)
        if nxt is None:
            self.publishStalls += 1
            return
        if (self.gen, nxt) in self.sfHas:
            self.fail(t, "the X server moves on to slot %d, which the compositor %s" % (nxt, SF_STATE[self.sfHas[(self.gen, nxt)]]))
        self.newest = self.drawn
        self.publishTick[(self.gen, self.drawn)] = k
        self.publishedAt.append(k)
        self.drawn = nxt
        self.pending = False
        self.drawRequested = True

    # ---- renderer
    def uncommitted(self):
        return self.submitted.get(self.epoch, 0) - self.committed.get(self.epoch, 0)

    def wake(self, t):
        if self.busy or not self.gateOpen or not (self.drawRequested or self.retry) or self.newest is None:
            return
        if self.zc and ((self.pol.pace == "claim" and self.uncommitted() > 0) or
                        (self.pol.pace == "depth" and self.uncommitted() > self.pol.depth)):
            self.waitingCommit = True                     # started when the OnCommit comes
            return
        self.busy = True
        self.at(t + self.T.WAKE, "frame")

    def drainRetiring(self, t):
        kept = []
        for e in self.retiring:
            if not self.cur(e["slot"]):
                continue                                  # an older pool: its held bit went with it
            done = e["arrived"] and (e["fenceAt"] is None or e["fenceAt"] <= t or "no-fence-wait" in self.bugs)
            if done:
                if e["slot"] in self.sfHas:
                    self.fail(t, "slot %d given back while the compositor %s" % (e["slot"][1], SF_STATE[self.sfHas[e["slot"]]]))
                self.held.discard(e["slot"][1])
            else:
                kept.append(e)
        self.retiring = kept
        return len(kept) + (1 if "room-plus-one" in self.bugs else 2) <= self.maxHeld

    def frame(self, t):
        self.drawRequested = False
        self.claimed = (self.gen, self.newest)
        self.claimNewest = (self.gen, self.newest)
        self.held.add(self.newest)
        self.check(t)
        if not self.zc:                                   # the GL path samples the slot and gives it back
            self.drainRetiring(t)
            self.held.discard(self.claimed[1])
            self.claimed = None
            self.busy = False
            self.gateOpen = False
            self.check(t)
            return
        capped = self.pol.depth is not None and self.pol.pace != "depth" and self.uncommitted() > self.pol.depth
        room = self.drainRetiring(t)
        if capped or (not room and not self.pol.lateRoom):
            self.frameStalls += 1
            mine = self.claimed == self.displayed or any(e["slot"] == self.claimed for e in self.retiring)
            if not mine:
                self.held.discard(self.claimed[1])
            self.claimed = None
            self.retry = True
            self.gateOpen = False
            self.busy = False
            self.check(t)
            return
        if self.claimed == self.displayed:
            self.claimed = None
            self.retry = False
            self.busy = False
            if self.publishedSinceClaim():
                self.drawRequested = True
            self.check(t)
            self.wake(t)
            return
        if self.claimed in self.sfHas:
            self.fail(t, "the frame copies into slot %d, which the compositor %s" % (self.claimed[1], SF_STATE[self.sfHas[self.claimed]]))
        work = self.sc.work(self.T, self.publishTick[self.claimed]) if self.sc.work else self.T.WORK
        if work > self.T.P - self.T.WAKE:
            self.overruns += 1
        self.at(t + work, "drained")

    def publishedSinceClaim(self):
        # by the newest slot and the pool, not by a wrapping counter (the 6-slot counter analysis)
        return (self.gen, self.newest) != self.claimNewest

    def drained(self, t):
        self.gateOpen = False                             # rootZcFrameDrained: waitForNextFrame
        if self.pol.pace == "apply" and self.uncommitted() > 0:
            self.waitingCommit = True
            return
        self.apply(t)

    def stall(self, t):
        self.frameStalls += 1
        mine = self.claimed == self.displayed or any(e["slot"] == self.claimed for e in self.retiring)
        if not mine:
            self.held.discard(self.claimed[1])
        self.claimed = None
        self.retry = True
        self.busy = False
        self.check(t)

    def apply(self, t):
        self.waitingCommit = False
        if self.claimed is not None and self.cur(self.claimed) and not self.zc:
            # left the root layer while this frame ran: nothing is submitted, the claim goes back
            if self.claimed != self.displayed and all(e["slot"] != self.claimed for e in self.retiring):
                self.held.discard(self.claimed[1])
            self.claimed = None
            self.busy = False
            self.check(t)
            return
        if self.pol.lateRoom and self.claimed is not None and self.cur(self.claimed) and not self.drainRetiring(t):
            self.stall(t)
            return
        if self.claimed is None or not self.cur(self.claimed):
            self.claimed = None                           # the pool went while the frame ran
            self.busy = False
            self.retry = True
            self.check(t)
            return
        self.seq += 1
        if self.displayed is not None:
            self.retiring.append(dict(seq=self.seq, slot=self.displayed, arrived=False, fenceAt=None))
        self.displayed = self.claimed
        self.claimed = None
        tx = dict(seq=self.seq, slot=self.displayed, applied=t, pub=self.publishTick[self.displayed],
                  epoch=self.epoch, buffer=True)
        self.txs.append(tx)
        self.sfQueue.append(tx)
        self.sfHas[tx["slot"]] = "queued"
        self.submitted[self.epoch] = self.seq
        self.queuedMax = max(self.queuedMax, sum(1 for x in self.sfQueue if x["buffer"] and x["epoch"] == self.epoch))
        self.uncommittedMax = max(self.uncommittedMax, self.uncommitted())
        self.retry = self.publishedSinceClaim()
        self.busy = False
        self.check(t)
        self.wake(t)

    def stopPresenting(self, t):
        self.zc = False
        self.seq += 1
        if self.displayed is not None:
            self.retiring.append(dict(seq=self.seq, slot=self.displayed, arrived=False, fenceAt=None))
            self.displayed = None
        self.sfQueue.append(dict(seq=self.seq, slot=None, applied=t, epoch=self.epoch, buffer=False, hide=True))
        self.submitted[self.epoch] = self.seq
        if not self.busy:
            self.drainRetiring(t)

    def teardown(self, t):
        self.stopPresenting(t)
        self.epoch += 1                                   # reparented and released; a new SurfaceControl next
        if "seq-restart" in self.bugs:
            self.seq = 0                                  # numbering per SurfaceControl
        # pacing starts again on the new SurfaceControl: nothing of it is uncommitted yet
        self.submitted[self.epoch] = self.committed[self.epoch] = self.seq

    # ---- callbacks
    def commitcb(self, t, epoch, seq):
        if "seq-restart" in self.bugs:
            epoch = self.epoch                            # and OnCommit not told apart by SurfaceControl
        self.committed[epoch] = max(self.committed.get(epoch, 0), seq)
        limit = self.pol.depth if self.pol.pace == "depth" else 0
        if self.waitingCommit and self.uncommitted() <= limit:
            if self.busy:
                self.at(t + 100, "apply")
            else:
                self.waitingCommit = False
                self.wake(t)

    def complete(self, t, seq, fenceAt, slot):
        for e in self.retiring:
            if e["arrived"]:
                continue
            if e["seq"] == seq or ("match-by-index" in self.bugs and slot is not None and e["slot"][1] == slot[1]):
                e["arrived"], e["fenceAt"] = True, fenceAt
                return

    # ---- compositor
    def latch(self, t, k):
        if self.sc.sfSkip(k):
            self.sfSkips += 1
            return
        T = self.T
        ready = [tx for tx in self.sfQueue if tx["applied"] <= t - T.MARGIN]
        if not ready:
            return
        if self.pol.bp:
            flush, seen = [], set()
            for tx in self.sfQueue:
                if tx not in ready or (tx["buffer"] and tx["epoch"] in seen):
                    break
                flush.append(tx)
                if tx["buffer"]:
                    seen.add(tx["epoch"])
        else:
            flush = ready
        for tx in flush:
            self.sfQueue.remove(tx)
        switch = (k + 1) * T.P
        release = switch + self.sc.relLate(k + 1)
        byEpoch = {}
        for tx in flush:
            byEpoch.setdefault(tx["epoch"], []).append(tx)
        for epoch, txs in byEpoch.items():
            silent = epoch != self.epoch and self.sc.oldCallbacks == "never"
            bufs = [tx for tx in txs if tx["buffer"]]
            latched = bufs[-1] if bufs else None
            shown = self.onScreen.get(epoch)
            for tx in txs:
                if silent:
                    continue
                first = tx is (latched if "fence-to-last" in self.bugs else (bufs[0] if bufs else None))
                if tx["buffer"]:
                    fence = (release if shown is not None else None) if first else None
                elif self.fixHide:
                    fence = switch                        # the present fence of the frame that hides it
                else:
                    fence = None                          # no buffer change: no previous release fence
                retired = next((e["slot"] for e in self.retiring if e["seq"] == tx["seq"]), None)
                self.at(t + T.CALLBACK + self.sc.cbLate(k), "complete", tx["seq"], fence, retired)
                self.at(t + self.sc.commitAt(k) + self.sc.commitDelay(k), "commitcb", epoch, tx["seq"])
            for tx in bufs[:-1]:
                del self.sfHas[tx["slot"]]
                self.dropped += 1
            if latched is not None:
                self.sfHas[latched["slot"]] = "latched"
                self.at(switch, "switch", epoch, latched, release)
            if any(tx.get("hide") for tx in txs):
                self.at(switch, "hidden", epoch)

    def switch(self, t, epoch, tx, release):
        old = self.onScreen.get(epoch)
        self.onScreen[epoch] = tx
        self.sfHas[tx["slot"]] = "on screen"
        self.presented.append((tx["seq"], t // self.T.P - tx["pub"], epoch))
        self.shownVsyncs.append(t // self.T.P)
        if old is not None:
            self.sfHas[old["slot"]] = "being released"
            self.at(release, "released", old["slot"])

    def hidden(self, t, epoch):
        tx = self.onScreen.pop(epoch, None)
        if tx is not None and self.sfHas.get(tx["slot"]) == "on screen":
            del self.sfHas[tx["slot"]]                    # no longer scanned out from this vsync

    def released(self, t, slot):
        if self.sfHas.get(slot) == "being released":
            del self.sfHas[slot]

    def run(self):
        T, sc = self.T, self.sc
        end = sc.ticks + 30
        for k in range(end + 10):
            v = k * T.P
            if k < end:
                self.at(v + T.TICK - 1, "events", k)
                self.at(v + T.TICK, "tick", k)
                if sc.extra(k):
                    self.at(v + T.P // 2, "extra", k)
                if sc.midPoll(k):
                    # half way between a completion for this vsync's latch and the next vsync
                    self.at(v + (T.LATCH + T.CALLBACK + T.P) // 2, "poll", k)
            self.at(v + T.LATCH, "latch", k)
        while self.ev:
            t, _, kind, data = heapq.heappop(self.ev)
            if kind == "events":
                k = data[0]
                if sc.stopAt == k: self.stopPresenting(t)
                if sc.teardownAt == k: self.teardown(t)
                if sc.resumeAt == k: self.zc = True
            elif kind == "tick": self.tick(t, *data)
            elif kind == "extra":
                self.extraPubs += 1
                self.pending = True
                self.publish(t, data[0])
                self.gateOpen = True
                self.wake(t)
            elif kind == "poll":
                if not self.busy:
                    self.drainRetiring(t)
            elif kind == "frame": self.frame(t)
            elif kind == "drained": self.drained(t)
            elif kind == "apply": self.apply(t)
            elif kind == "latch": self.latch(t, *data)
            elif kind == "complete": self.complete(t, *data)
            elif kind == "commitcb": self.commitcb(t, *data)
            elif kind == "switch": self.switch(t, *data)
            elif kind == "hidden": self.hidden(t, *data)
            elif kind == "released": self.released(t, *data)
            self.check(t)
        return self.result()

    def result(self):
        seqs = [s for s, _, e in self.presented if e == self.epoch]
        lat = sorted(l for _, l, e in self.presented)
        pubs, content, episodes, run = set(self.publishedAt), set(self.contentTicks), 0, False
        for k in range(self.sc.ticks + 30 - 8):
            window = range(k, k + 8)
            half = all(i in content for i in window) and sum(i in pubs for i in window) <= 4
            if half and not run:
                episodes += 1
            run = half
        shownAt = {}
        for s_, l, e in self.presented:
            pass
        stopped = set()
        if self.sc.stopAt is not None:
            stopped |= set(range(self.sc.stopAt, self.sc.resumeAt or self.sc.stopAt))
        if self.sc.teardownAt is not None:
            stopped |= set(range(self.sc.teardownAt, self.sc.resumeAt or self.sc.teardownAt))
        offRoot = stopped | {k + i for k in stopped for i in range(1, 4)}   # shown through GL, not modelled
        shownVsyncs = set(self.shownVsyncs)
        presentedHalf, run = 0, False
        for k in range(10, self.sc.ticks + 30 - 8):
            window = range(k, k + 8)
            half = not any(i in offRoot for i in window) and all(i - 1 in content for i in window) \
                and sum(i in shownVsyncs for i in window) <= 4
            if half and not run:
                presentedHalf += 1
            run = half
        submitted = {tx["pub"] for tx in self.txs}
        skipped = sum(1 for k in set(self.publishedAt) if k not in submitted and k not in stopped)
        live = [tx for tx in self.txs if tx["epoch"] == self.epoch]
        lost = [tx["seq"] for tx in live if tx["seq"] not in set(seqs)]
        return dict(presented=len(self.presented), dropped=self.dropped, publishStalls=self.publishStalls,
                    frameStalls=self.frameStalls, heldMax=self.heldMax, queuedMax=self.queuedMax,
                    uncommittedMax=self.uncommittedMax, xAvailMin=self.xAvailMin,
                    inOrder=seqs == sorted(seqs), latency=(lat[len(lat) // 2], lat[-1]) if lat else (None, None),
                    halfRate=episodes + presentedHalf, notPresented=len(lost), skipped=skipped,
                    unavoidable=self.sfSkips + self.extraPubs + self.overruns + (1 if self.sc.resizeAt is not None else 0),
                    stuck=self.waitingCommit,
                    leftHeld=len([e for e in self.retiring if self.cur(e["slot"])]))

def verdict(r, strict=True):
    """strict: what a policy meant to stop dropping frames has to meet as well"""
    why = []
    if r["xAvailMin"] < 2: why.append("X server down to %d slot(s)" % r["xAvailMin"])
    if r["halfRate"]: why.append("%d half-rate episode(s)" % r["halfRate"])
    if not r["inOrder"]: why.append("presented out of order")
    if r["stuck"]: why.append("waiting for an OnCommit that never comes")
    if strict:
        lost = r["skipped"] + r["dropped"]
        if lost > r["unavoidable"]:
            why.append("%d frames lost, %d unavoidable" % (lost, r["unavoidable"]))
        if r["latency"][0] is not None and r["latency"][0] > 2:
            why.append("latency grew to %d frames" % r["latency"][0])
    return why

def runAll(T, pol, scs, bugs=(), fixHide=False, strict=True):
    out = {}
    for sc in scs:
        try:
            r = Sim(T, pol, sc, bugs, fixHide).run()
            out[sc.name] = (None, r, verdict(r, strict))
        except Violation as v:
            out[sc.name] = (str(v), None, None)
    return out

DELAYS = (200, 1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 16700)

def sweep(T, pol, bugs=()):
    rows = []
    for d in DELAYS:
        a = Sim(T, pol, Scenario("one slow frame", work=lambda T_, k: T_.SLOW if k == 90 else T_.WORK,
                                 commitDelay=lambda k, d=d: d), bugs).run()
        b = Sim(T, pol, Scenario("slow every 10", work=lambda T_, k: T_.SLOW if k % 10 == 5 else T_.WORK,
                                 commitDelay=lambda k, d=d: d), bugs).run()
        rows.append((d, a, b, verdict(a) + verdict(b)))
    return rows

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", type=int, nargs="*", default=[60, 120])
    ap.add_argument("--policy", nargs="*", default=list(POLICIES))
    ap.add_argument("--scenario", nargs="*")
    ap.add_argument("--lifecycle", action="store_true")
    ap.add_argument("--fix-hide", action="store_true", help="retire a hidden slot on the hide frame's present fence")
    ap.add_argument("--delays", action="store_true", help="sweep the OnCommit delay for each policy")
    ap.add_argument("--bug", nargs="*", default=[])
    args = ap.parse_args()
    for hz in args.hz:
        T = Timing(hz)
        if args.delays:
            print("== %d Hz: OnCommit delay to the client, a slow frame at tick 90 / a slow frame every 10 ticks" % hz)
            for name in args.policy:
                cells = []
                for d, a, b, why in sweep(T, POLICIES[name], args.bug):
                    cells.append("%4.1f:%s" % (d / 1000.0, "ok" if not why else "lat%d lost%d half%d" % (
                        max(a["latency"][0], b["latency"][0]), a["skipped"] + a["dropped"] + b["skipped"] + b["dropped"]
                        - a["unavoidable"] - b["unavoidable"], a["halfRate"] + b["halfRate"])))
                print("  %-8s %s" % (name, " | ".join(cells)))
            continue
        scs = lifecycle(T) if args.lifecycle else scenarios(T)
        if args.scenario:
            scs = [s for s in scs if s.name in args.scenario]
        for name in args.policy:
            pol = POLICIES[name]
            print("== %d Hz, %s: %d slots, backpressure %s%s%s" % (hz, name, pol.slots, "ON" if pol.bp else "OFF",
                  ", OnCommit-paced before %s" % ("txApply" if pol.pace == "apply" else "the claim") if pol.pace else "",
                  ", uncommittedDepth <= %d" % pol.depth if pol.depth is not None else ""))
            out = runAll(T, pol, scs, args.bug, args.fix_hide, strict=name != "A")
            for sc in scs:
                v, r, why = out[sc.name]
                if v:
                    print("  %-70s VIOLATION %s" % (sc.name, v))
                    continue
                print("  %-70s %s presented %d dropped %d skipped %d stalls %d/%d held %d SF-queued %d uncommitted %d "
                      "X-min %d latency %s/%s half %d%s" % (
                          sc.name, "PASS" if not why else "FAIL", r["presented"], r["dropped"], r["skipped"],
                          r["publishStalls"], r["frameStalls"], r["heldMax"], r["queuedMax"], r["uncommittedMax"],
                          r["xAvailMin"], r["latency"][0], r["latency"][1], r["halfRate"],
                          (" - " + "; ".join(why)) if why else ""))

if __name__ == "__main__":
    main()
