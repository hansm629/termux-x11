#!/usr/bin/env python3
"""Checks tools/trace/analyze.py's account of an output gap against a trace whose timings are known.

A synthetic trace in the format lorieTraceFlush writes: a 50 ms gap between two direct submits, with
timed steps inside it - one of them running past the gap's end, so only its part inside may count.

usage: tanalyze.py <analyze.py> <scratch dir>
"""
import os, struct, subprocess, sys

ANALYZE, OUT = sys.argv[1], sys.argv[2]
MS = 1000
T0 = 5_000_000
recs = [
    (0, 9, 0, 101),                     # DIRECT: the gap starts
    (5 * MS, 6, 1, 0), (6 * MS, 6, 1, 0), (7 * MS, 6, 1, 0),   # INPUT x3
    (8 * MS, 2, 1, 77),                 # ENQUEUE
    (10 * MS, 18, 2 * MS, 30 * MS),     # RLOCK taken at 10: waited 2 before, held 30 after
    (20 * MS, 14, 2, 18 * MS),          # PREFLIGHT timed out after waiting 18
    (35 * MS, 15, 12 * MS, 5),          # XLOCK: waited 12
    (40 * MS, 16, 4 * MS, 3_000_000),   # ROOTCOPY: 4 ms, 3,000,000 bytes
    (42 * MS, 17, 1 * MS, 0),           # REMAP: 1 ms
    (45 * MS, 8, 7 * MS, 77),           # FENCE: waited 7
    (46 * MS, 4, 1, 102),               # PUBLISH
    (50 * MS, 9, 1, 102),               # DIRECT: the gap ends at 50
    (55 * MS, 15, 10 * MS, 5),          # XLOCK 45..55: 5 ms of it inside the gap
    (66 * MS, 9, 2, 103),               # DIRECT: a 16 ms gap, not the longest
]
path = os.path.join(OUT, "tanalyze.trace")
with open(path, "wb") as f:
    f.write(b"LTR1" + struct.pack("<IQ", 24, T0))
    for t, k, a, b in recs:
        f.write(struct.pack("<QIIQ", T0 + t, k, a, b))

out = subprocess.run([sys.executable, ANALYZE, path, "--stalls", "1"], capture_output=True, text=True)
text = out.stdout + out.stderr
want = [
    "output gap 50.0 ms",
    "X server : preflight waits 18.0 ms (1, 1 timed out), shared lock waits 17.0 ms (2), "
    "root CPU copies 4.0 ms (1, 2.86 MB), root remaps 1.0 ms (1)",
    "renderer : shared lock waits 2.0 ms (1), holding the lock 30.0 ms (1), GPU fence waits 7.0 ms (1)",
    "events   : input 3, copies queued 1, root publishes 1, direct submits 2",
    "not at the glass",
]
fails = 0
for w in want:
    if w not in text:
        fails += 1
        print("  FAIL analyze.py output lacks: " + w)
if out.returncode != 0:
    fails += 1
    print("  FAIL analyze.py exited %d" % out.returncode)
if fails:
    print(text)
print("trace analysis (gap breakdown): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
