#!/usr/bin/env python3
"""Checks tools/trace/frames.py against a synthetic trace whose frames are known: 20 ticks, 16.667 ms apart;
each present published 2 ms after its vsync, claimed 0.5 ms later, applied after a fence wait of 5 ms -
or 12 ms for every fourth frame, which then misses the compositor's deadline (11 ms after the vsync) and is
latched a vsync late. The compositor's callbacks arrive out of order, one frame has none, and a frame that
found nothing newer is in between - none of which may change who is joined to what.

usage: tframes.py <frames.py> <scratch dir>
"""
import os, struct, subprocess, sys

FRAMES, OUT = sys.argv[1], sys.argv[2]
P = 16667
VSYNC, PUBSEQ, ZCCLAIM, ZCBATCH, ZCFENCE, ZCAPPLY, SFDONE, SFPRESENT = range(19, 27)
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
path = os.path.join(OUT, "tframes.trace")
with open(path, "wb") as f:
    f.write(b"LTR1" + struct.pack("<IQ", 24, T0))
    for t, k, a, b in sorted(recs):
        f.write(struct.pack("<QIIQ", t, k, a, b))

out = subprocess.run([sys.executable, FRAMES, path], capture_output=True, text=True)
text = out.stdout + out.stderr
want = [
    "== 20 ROOT_DIRECT frames applied, 19 with a latch time, 0 with a present time; refresh period 16.7 ms",
    # judged: frames 1-19 but 7 (no callback) and 8 (nothing to judge it against); late: 3, 11, 15, 19
    "latched late (over 1.5 periods after the frame before): 4, on time: 13",
    "of which fence wait p50 12.0 p90 12.0 max 12.0",
    "of which fence wait p50 5.0 p90 5.0 max 5.0",
    "every late frame was applied later in its vsync than every on-time one: on time up to 7.9 ms, late from 14.9 ms",
    "into anything else p50 2025 kpx",
]
fails = 0
for w in want:
    if w not in text:
        fails += 1
        print("  FAIL frames.py output lacks: " + w)
if out.returncode != 0:
    fails += 1
    print("  FAIL frames.py exited %d" % out.returncode)
if fails:
    print(text)
print("trace analysis (ROOT_DIRECT frames, tick to latch): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
