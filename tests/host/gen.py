#!/usr/bin/env python3
"""Writes each test's <name>_src.inc from the sources as they are now, so the tests exercise the real code.

usage: gen.py <lorie source dir> <output dir>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import func, macro, span

L, OUT = sys.argv[1], sys.argv[2]
I, R, H, B = (os.path.join(L, f) for f in ("InitOutput.c", "renderer.c", "lorie.h", "buffer.c"))

def opt(make, marker, path):
    """What make() extracts, if `path` has `marker` - so a recipe also builds against code from before the
    function existed, which is how a test is shown to fail there."""
    return make() if marker in open(path, encoding="utf-8").read() else ""

recipes = {
    # the renderer diagnostics, as lorie.h declares them and the X server logs them - tdiaglog.c
    "diagtypes": lambda: span(H, "enum { LORIE_ACTIVE_NONE = 0", "__ATOMIC_RELAXED); } while (0)"),
    "diaglog": lambda: span(I, "static const char *lorieActiveName(uint8_t a) {", "#undef LORIE_NAME")
        + func(I, "static void lorieLogRendererDiag(void) {"),
}

for name, make in recipes.items():
    with open(os.path.join(OUT, name + "_src.inc"), "w") as f:
        f.write(make())
