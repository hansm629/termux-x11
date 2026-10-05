#!/usr/bin/env python3
"""Checks tools/trace/frames.py against a synthetic trace whose frames are known: 20 ticks, 16.667 ms apart;
each present published 2 ms after its vsync, claimed 0.5 ms later, applied after a fence wait of 5 ms -
or 12 ms for every fourth frame, which then misses the compositor's deadline (11 ms after the vsync) and is
latched a vsync late. The compositor's callbacks arrive out of order, one frame has none, and a frame that
found nothing newer is in between - none of which may change who is joined to what.

A second trace has a client presenting into a redirected window: each tick's copy into it finishes 2 ms after
it is drained, but tick 5's takes 16 ms. The X server waits on that window's buffer until it is done, no root
publish is made at tick 6, and the frame published at tick 7 is latched a vsync late although every stage
from its own publish on was on time - which has to be put down to that window copy.

usage: tframes.py <frames.py> <scratch dir>
"""
import os, re, struct, subprocess, sys

FRAMES, OUT = sys.argv[1], sys.argv[2]
P = 16667
VSYNC, PUBSEQ, ZCCLAIM, ZCBATCH, ZCFENCE, ZCAPPLY, SFDONE, SFPRESENT = range(19, 27)
ENQUEUE, RESOLVED, PUBLISH, TICK, DRAIN, FENCE, XLOCK = 2, 3, 4, 5, 7, 8, 15
T0 = 10_000_000
recs = []
late = 0
for i in range(20):
    v = T0 + i * P
    recs.append((v + 100, VSYNC, 1, v))
    pub = v + 2000
    recs.append((pub, PUBSEQ, 100 + i, i % 5))
    claim = pub + 500
    frame, seq = 1000 + 2 * i, 500 + i
    recs.append((claim, ZCCLAIM, frame, 100 + i))
    wait = 12000 if i % 4 == 3 else 5000
    recs.append((claim + 200, ZCBATCH, frame, (2025 << 32) | (2025 if i % 4 == 3 else 0)))
    recs.append((claim + 300 + wait, ZCFENCE, frame, wait))
    applied = claim + 400 + wait
    recs.append((applied, ZCAPPLY, frame, seq))
    deadline = v + 11000
    latch = (v + P if applied <= deadline else v + 2 * P) + 500
    late += applied > deadline
    if i != 7:                                   # one frame whose callback never came
        cb = latch + 1000 + (3000 if i % 2 else 0)  # out of order with the next one's
        recs.append((cb, SFDONE, seq, latch * 1000))
    recs.append((claim + 100, ZCCLAIM, frame + 1, (100 + i) | (1 << 32)))   # a nothing-new frame
def run(name, recs):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(b"LTR1" + struct.pack("<IQ", 24, T0))
        for t, k, a, b in sorted(recs):
            f.write(struct.pack("<QIIQ", t, k, a, b))
    out = subprocess.run([sys.executable, FRAMES, path], capture_output=True, text=True)
    return out.returncode, out.stdout + out.stderr

def check(code, text, want, unwanted=()):
    flat = re.sub(r"\s+", " ", text)
    fails = 0
    for w in want:
        if re.sub(r"\s+", " ", w) not in flat:
            fails += 1
            print("  FAIL frames.py output lacks: " + w)
    for w in unwanted:
        if re.sub(r"\s+", " ", w) in flat:
            fails += 1
            print("  FAIL frames.py output has: " + w)
    if code != 0:
        fails += 1
        print("  FAIL frames.py exited %d" % code)
    if fails:
        print(text)
    return fails

fails = check(*run("tframes.trace", recs), [
    "== 20 ROOT_DIRECT frames applied, 19 with a latch time, 0 with a present time; refresh period 16.7 ms",
    # judged: frames 1-19 but 7 (no callback) and 8 (nothing to judge it against); late: 3, 11, 15, 19
    "latched late (over 1.5 periods after the frame before): 4, on time: 13",
    "of which fence wait p50 12.0 p90 12.0 max 12.0",
    "of which fence wait p50 5.0 p90 5.0 max 5.0",
    "into anything else p50 2025 kpx",
    # the late ones were applied 2.3 ms before the next latch, past this compositor's 5.7 ms deadline
    "15 latched at the first chance after their apply, 4 at a later one",
    "applied before its first latch and latched later all the same: 4",
    "how long before that latch it was applied p50 2.3 p90 2.3 max 2.3",
])

# The redirected window. Each tick: a root publish at +0.2 ms, its frame claimed at +0.25 and applied after a
# 1 ms fence for the carry; the client's copy into its window queued at +1.79 and drained on its own at +1.8,
# done 2 ms later, seen by the X server 1.2 ms after that; the X server waits for it on the window's buffer
# (900, the root slots being 317-321). The compositor latches 12 ms into each vsync.
recs = []
LATCH_AT = 12000
pubSeq, frameSeq, applySeq, serial = 100, 1000, 500, 0
for i in range(12):
    v = T0 + i * P
    recs.append((v, TICK, 1, i))
    recs.append((v + 100, VSYNC, 1, v))
    if i != 6:                                  # nothing new to publish: tick 5's window copy not read back yet
        pub = v + 200
        recs.append((pub, PUBSEQ, pubSeq, i % 5))
        recs.append((pub, PUBLISH, i % 5, 317 + i % 5))
        serial += 1
        recs.append((pub + 10, ENQUEUE, 2, serial))     # the carry
        claim = pub + 50
        recs.append((claim, ZCCLAIM, frameSeq, pubSeq))
        recs.append((claim + 10, DRAIN, 1, serial))
        recs.append((claim + 20, ZCBATCH, frameSeq, 2025))
        recs.append((claim + 1010, ZCFENCE, frameSeq, 1000))
        recs.append((claim + 1010, FENCE, 0, serial))
        applied = claim + 1500
        recs.append((applied, ZCAPPLY, frameSeq, applySeq))
        # the first latch at least 1 ms after the apply
        k = (applied + 1000 - T0 - LATCH_AT + P - 1) // P
        recs.append((applied + 20000, SFDONE, applySeq, (T0 + LATCH_AT + k * P) * 1000))
        recs.append((T0, SFPRESENT, applySeq, 0))       # a driver whose present fences all carry one time
        pubSeq += 1; frameSeq += 1; applySeq += 1
    if i != 6:                                  # the client's next present came only after tick 5's copy was seen done
        serial += 1
        q = v + 1790
        recs.append((q, ENQUEUE, 0, serial))
        recs.append((q + 10, DRAIN, 1, serial))
        done = q + 10 + (16000 if i == 5 else 2000)
        recs.append((done, FENCE, 0, serial))
        recs.append((done + 1200, RESOLVED, 1, serial))
        start = v + 5000 if i == 5 else done - 800
        recs.append((done, XLOCK, done - start, 900))
    if i == 5:                                  # a wait on a root slot in between, not the window's
        recs.append((v + 11000, XLOCK, 3000, 318))

fails += check(*run("tframes-window.trace", recs), [
    # frames at ticks 1-5 and 8-11 on time, the one at tick 0 has nothing before it
    "latched late (over 1.5 periods after the frame before): 1, on time: 9",
    "new content came late (first latch after its publish taken, the publish a vsync late): 1",
    "its window copy seen finished by the X server only after the next tick: 1",
    "window copy queued -> done p50 16.0 p90 16.0 max 16.0",
    "done -> seen by the X server p50 1.2 p90 1.2 max 1.2",
    "X blocked on a non-root buffer between the publishes p50 12.8 p90 12.8 max 12.8",
    "on time, for comparison: 9 with a window copy at the tick before",
    "window copy queued -> done p50 2.0 p90 2.0 max 2.0",
    "published in time, claimed after its first latch: 0",
    "claimed in time, applied after its first latch: 0",
    "== present times: 11, but only 1 distinct value(s)",
], unwanted=["apply -> present", "latch -> present"])
print("trace analysis (ROOT_DIRECT frames, tick to latch): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
