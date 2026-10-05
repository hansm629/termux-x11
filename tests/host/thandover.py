#!/usr/bin/env python3
"""Checks tools/model/handover.py: over every interleaving to depth 9 with 5 slots, a 2-bit publish count
misses a publish (a claim outlives a wrap - the X server alternates between two free slots for as long as
the claim lasts, so no width of count is safe), while comparing the newest slot and the pool, or the full
rootPublishSeq of the newest slot read after the word, never does.

usage: thandover.py <handover.py>
"""
import importlib.util, sys

spec = importlib.util.spec_from_file_location("handover", sys.argv[1])
H = importlib.util.module_from_spec(spec)
spec.loader.exec_module(H)

found, spurious, states = H.explore(5, 2, 9)
fails = 0
for cond, what in ((found["count"], "the 2-bit count never misses a publish"),
                   (not found["newest"], "newest slot + pool misses a publish: %s" % (found["newest"][:1],)),
                   (spurious["newest"] == 0, "newest slot + pool says 'new' %d times when nothing was" % spurious["newest"]),
                   (not found["fullseq"], "full rootPublishSeq misses a publish: %s" % (found["fullseq"][:1],)),
                   (states > 100000, "only %d states explored" % states)):
    if not cond:
        fails += 1
        print("  FAIL " + what)
print("publish-since-claim answers (%d states): %s (%d failures)" % (states, "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
