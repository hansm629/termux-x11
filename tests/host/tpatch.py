#!/usr/bin/env python3
"""Static check of the xserver.patch hooks the GPU copy ordering depends on.

Applies the patch's exa/ sections to the tree's own exa sources and checks that every way EXA writes a
pixmap reaches lorieExaAccess() as a write, before the access is prepared. A write that never does is
one a queued GPU copy can land on top of afterwards (see lorieExaAccess in lorie/InitOutput.c).

usage: tpatch.py <xserver source dir> <xserver.patch> <scratch dir>
"""
import os, re, shutil, subprocess, sys

XS, PATCH, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
work = os.path.join(OUT, "tpatch")
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
shutil.copytree(os.path.join(XS, "exa"), os.path.join(work, "exa"))

# Only the exa/ sections; the patch has no "---" lines, each file starts at its "+++ ./path".
sections = re.split(r"(?m)^(?=\+\+\+ )", open(PATCH).read())
exa = "".join(s for s in sections if s.startswith("+++ ./exa/"))
r = subprocess.run(["patch", "-p1", "-N", "-s", "-d", work], input=exa, text=True, capture_output=True)
if r.returncode != 0 or "fuzz" in r.stdout + r.stderr:
    print("patch hooks: FAIL (exa sections do not apply cleanly)\n" + r.stdout + r.stderr)
    sys.exit(1)

def body(path, signature):
    src = open(os.path.join(work, "exa", path)).read()
    start = src.index(signature)
    depth, i = 0, src.index("{", start)
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i:j]
    raise ValueError(signature)

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

def before(text, first, second):
    a, b = text.find(first), text.find(second)
    return a >= 0 and b >= 0 and a < b

# Every CPU access goes through here, with its drawable and the kind of access.
f = body("exa.c", "\nexaPrepareAccess(DrawablePtr pDrawable, int index)")
check(before(f, "lorieExaAccess(pDrawable, pPixmap, index);", "pExaScr->prepare_access_reg"),
      "exaPrepareAccess: lorieExaAccess is not called before the access is prepared")

# CopyWindow writes the window's new area but prepares the pixmap only as a source, so the hook above
# sees a read. It has to be told about the write itself, before that prepare.
f = body("exa_unaccel.c", "\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)")
check(before(f, "lorieExaAccess(pDrawable, pScreen->GetWindowPixmap(pWin), EXA_PREPARE_DEST);",
             "EXA_PREPARE_SRC"),
      "ExaCheckCopyWindow: its write is never reported as one before the source is prepared")

print("patch hooks (exa write paths reach lorieExaAccess): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
