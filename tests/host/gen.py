#!/usr/bin/env python3
"""Writes each test's <name>_src.inc from the sources as they are now, so the tests exercise the real code.

usage: gen.py <lorie source dir> <output dir> <3c98ffa lorie source dir>

The third directory holds InitOutput.c as it is at 3c98ffa (run.sh takes it from git): the code that tests
comparing against 3c98ffa's behaviour run side by side with the current code.
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from extract import func, macro, span

L, OUT, BASE = sys.argv[1], sys.argv[2], sys.argv[3]
I, R, H, B = (os.path.join(L, f) for f in ("InitOutput.c", "renderer.c", "lorie.h", "buffer.c"))
BI = os.path.join(BASE, "InitOutput.c")
XS = os.path.join(HERE, "..", "..", "app", "src", "main", "cpp", "xserver")

def opt(make, marker, path):
    """What make() extracts, if `path` has `marker` - so a recipe also builds against code from before the
    function existed, which is how a test is shown to fail there."""
    return make() if marker in open(path, encoding="utf-8").read() else ""

def own(path, ownership):
    """The pixmap private and the pixmap paths that take or drop a CPU lock, in the order InitOutput.c has them,
    with the ownership helpers (`ownership`, absent at 3c98ffa) ahead of them."""
    return (span(path, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + macro(path, "LORIE_PIXMAP_PRIV_FROM_PIXMAP")
        + "".join(func(path, f) for f in ownership)
        + "".join(func(path, f) for f in (
            "static LorieBuffer *lorieEnsureGpuSampleable(PixmapPtr pixmap, int8_t type) {",
            "static LoriePixmapPriv* lorieRootWindowPixmapPriv(void) {",
            "void drawSquare(int x, int y, int l, uint32_t color, uint32_t stride, uint32_t* pixels) {",
            "Bool drawSquares() {",
            "void lorieSetWindowPixmap(WindowPtr pWindow, PixmapPtr newPixmap) {",
            "void *lorieCreatePixmap(",
            "void lorieExaDestroyPixmap(",
            "static inline __always_inline Bool lorieNeedsGpuLock(",
            "Bool loriePrepareAccess(PixmapPtr pPix, int index) {",
            "void lorieFinishAccess(PixmapPtr pPix, int index) {")))

recipes = {
    # the renderer diagnostics, as lorie.h declares them and the X server logs them - tdiaglog.c
    "diagtypes": lambda: span(H, "enum { LORIE_ACTIVE_NONE = 0", "__ATOMIC_RELAXED); } while (0)"),
    "diaglog": lambda: span(I, "static const char *lorieActiveName(uint8_t a) {", "#undef LORIE_NAME")
        + func(I, "static void lorieLogRendererDiag(void) {"),
    # AHB CPU ownership - townership.c: xserver's list, EXA's access indices, then every pixmap path that takes
    # or drops a CPU lock, as 3c98ffa has them ("own_base") and as they are now, with the handshake ("own_new")
    "xorglist": lambda: open(os.path.join(XS, "include", "list.h"), encoding="utf-8").read(),
    "own_consts": lambda: macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_DEST")
        + macro(os.path.join(XS, "exa", "exa.h"), "EXA_PREPARE_SRC")
        + macro(I, "CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED"),
    "own_base": lambda: own(BI, []),
    "own_new": lambda: own(I, [
        "static Bool lorieAhbPerAccess(void) {",
        "static Bool lorieIsAhb(LoriePixmapPriv *priv) {",
        "static void lorieHoldPersistent(LoriePixmapPriv *priv) {",
        "static void lorieDropPersistent(LoriePixmapPriv *priv) {",
        "static void lorieApplyOwnership(uint8_t mode, uint32_t seq) {",
        "static void lorieProcessOwnershipRequest(void) {",
        "static void lorieResetOwnership(void) {",
        "static Bool lorieRendererGoneWorkProc(",
        "void lorieRendererDisconnected(void) {",
        "static uint8_t lorieRequestedBackend(const char *value, volatile uint8_t *invalid) {",
    ]),
}

for name, make in recipes.items():
    with open(os.path.join(OUT, name + "_src.inc"), "w") as f:
        f.write(make())
