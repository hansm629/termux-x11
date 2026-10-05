#!/usr/bin/env python3
"""Every interleaving, to a bound, of the X server publishing and the renderer claiming, holding and giving
back root slots - to decide what "published since this claim" (rootZcPublishedSinceClaim) may rest on.

usage: handover.py [--slots N] [--bits K] [--depth D]

The handover word as the sources use it (lorie.h rootHandover, InitOutput.c lorieRootHandover,
renderer.c rendererClaimRootBuffer): held bits, the newest slot, a K-bit publish count, the pool
generation. The X server publishes the slot it draws into, writes rootPublishSeq[slot] (full) before the
swap that publishes it, and moves on to any slot that is neither that one nor held (every choice is
explored); it may also replace the pool. The renderer claims the newest slot (one swap that sets its held
bit), may hold or give back other slots it does not draw from, and at some point asks whether anything
was published since the claim, reading the word and then, in a second step, rootPublishSeq of the
newest slot the word named - with the X server free to act in between.

Each way of answering is checked against what actually happened up to the word read (the point the
answer is about; a publish after it sets drawRequested after the frame cleared it and is not lost):
  count    the K-bit count or the generation differs from the claim's      (what the code does, 8 bits)
  newest   the newest slot or the generation differs from the claim's
  fullseq  the generation differs, or rootPublishSeq[newest read from the word] != the claimed slot's
"""
import argparse, sys

def explore(slots, bits, depth):
    mask = (1 << bits) - 1
    found = {"count": [], "newest": [], "fullseq": []}      # said "nothing new" when there was: a lost publish
    spurious = {"count": 0, "newest": 0, "fullseq": 0}     # said "new" when there was not: one extra frame
    states = 0
    # state: (held, newest, drawn, count, gen, seqs, pubs, claim, check)
    #   claim = None or (slot, count, gen, claimedSeq, pubsAtClaim)
    #   check = None or (newestRead, genRead, countRead, pubsAtRead) - between the two reads
    start = (frozenset(), None, 0, 0, 0, tuple([0] * slots), 0, None, None)
    seen = set()
    stack = [(start, 0, ())]
    while stack:
        st, d, path = stack.pop()
        key = (st, d)
        if key in seen:
            continue
        seen.add(key)
        states += 1
        held, newest, drawn, count, gen, seqs, pubs, claim, check = st
        if d >= depth:
            continue
        nxt = []
        # X server: publish, moving on to each slot it may choose
        for i in range(slots):
            if i != drawn and i not in held:
                s = list(seqs); s[drawn] = pubs + 1
                nxt.append(((held, drawn, i, (count + 1) & mask, gen, tuple(s), pubs + 1, claim, check),
                            "publish %d->%d" % (drawn, i)))
        # X server: replace the pool (held bits cleared, nothing published in the new one yet)
        if gen < 1:
            nxt.append(((frozenset(), None, 0, count, gen + 1, tuple([0] * slots), pubs, claim, check), "replace pool"))
        # renderer: hold / give back a slot other than the claim, within slots - 2 in all
        for i in range(slots):
            if claim is not None and i == claim[0]:
                continue
            if i in held:
                nxt.append(((held - {i}, newest, drawn, count, gen, seqs, pubs, claim, check), "give back %d" % i))
            elif i != drawn and i != newest and len(held) < slots - 2:
                nxt.append(((held | {i}, newest, drawn, count, gen, seqs, pubs, claim, check), "hold %d" % i))
        if claim is None and newest is not None:
            nxt.append(((held | {newest}, newest, drawn, count, gen, seqs, pubs,
                         (newest, count, gen, seqs[newest], pubs), None), "claim %d" % newest))
        if claim is not None and check is None:
            nxt.append(((held, newest, drawn, count, gen, seqs, pubs, claim, (newest, gen, count, pubs)), "read word"))
        if check is not None:
            n, g, c, pubsAtRead = check
            truth = g != claim[2] or pubsAtRead != claim[4]
            answers = {
                "count": c != claim[1] or g != claim[2],
                "newest": n != claim[0] or g != claim[2],
                # the second read is of whatever pool is there by then
                "fullseq": True if g != claim[2] else (n is None or gen != g or seqs[n] != claim[3]),
            }
            for name, a in answers.items():
                if truth and not a and len(found[name]) < 3:
                    found[name].append((truth, a, path + ("read rootPublishSeq",)))
                elif a and not truth:
                    spurious[name] += 1
            # the claim ends: given back (its held bit, if it is still this pool's)
            h = held - {claim[0]} if claim[2] == gen else held
            nxt.append(((h, newest, drawn, count, gen, seqs, pubs, None, None), "check, give back the claim"))
        for ns, what in nxt:
            stack.append((ns, d + 1, path + (what,)))
    return found, spurious, states

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slots", type=int, nargs="*", default=[5, 6])
    ap.add_argument("--bits", type=int, nargs="*", default=[2, 3])
    ap.add_argument("--depth", type=int, default=14)
    args = ap.parse_args()
    worst = 0
    for slots in args.slots:
        for bits in args.bits:
            found, spurious, states = explore(slots, bits, args.depth)
            print("== %d slots, a %d-bit count, every interleaving to depth %d (%d states)" % (slots, bits, args.depth, states))
            for name in ("count", "newest", "fullseq"):
                bad = found[name]
                print("  %-8s %s; spurious 'new' answers %d" % (name, "never misses a publish" if not bad else
                      "MISSES A PUBLISH, e.g. after: %s" % ", ".join(bad[0][2]), spurious[name]))
                if bad and name != "count":
                    worst = 1
    return worst

if __name__ == "__main__":
    sys.exit(main())
