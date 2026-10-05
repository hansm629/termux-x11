#!/usr/bin/env python3
"""Static check of the xserver.patch hooks.

Applies the patch's exa/ and present/ sections to the tree's own sources and checks:
  - every way EXA writes a pixmap reaches lorieExaAccess() as a write, before the access is prepared. A
    write that never does is one a queued GPU copy can land on top of afterwards (see lorieExaAccess in
    lorie/InitOutput.c);
  - every copy an EXA fallback makes with the CPU is timed from before it and counted after it
    (lorieNoteCoreCopy), and the present and flip-end copies are counted as theirs (lorieCopyContext),
    with the constants the patch repeats matching lorie/lorie.h;
  - CopyArea's and CopyWindow's fallbacks offer the copy to the GPU first (lorieCoreCopyOnGpu), before
    any access is opened, and copy nothing with the CPU when it was made.

usage: tpatch.py <xserver source dir> <xserver.patch> <scratch dir>
"""
import os, re, shutil, subprocess, sys

XS, PATCH, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
work = os.path.join(OUT, "tpatch")
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
shutil.copytree(os.path.join(XS, "exa"), os.path.join(work, "exa"))
shutil.copytree(os.path.join(XS, "present"), os.path.join(work, "present"))

# Only the exa/ and present/ sections; the patch has no "---" lines, each file starts at its "+++ ./path".
sections = re.split(r"(?m)^(?=\+\+\+ )", open(PATCH).read())
exa = "".join(s for s in sections if s.startswith("+++ ./exa/") or s.startswith("+++ ./present/"))
r = subprocess.run(["patch", "-p1", "-N", "-s", "-d", work], input=exa, text=True, capture_output=True)
if r.returncode != 0 or "fuzz" in r.stdout + r.stderr:
    print("patch hooks: FAIL (exa sections do not apply cleanly)\n" + r.stdout + r.stderr)
    sys.exit(1)

def body(path, signature):
    src = open(os.path.join(work, path if "/" in path else os.path.join("exa", path))).read()
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

# Every CPU copy an EXA fallback makes: timed from before the copy, counted after it.
def counted(path, signature, call, kind):
    f = body(path, signature)
    check(before(f, "lorieCoreCopyBegin()", call) and before(f, call, "lorieNoteCoreCopy(" + kind),
          signature.strip().split("(")[0] + ": its CPU copy is not timed from before it and counted after it")
counted("exa_unaccel.c", "\nExaCheckCopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,", "pGC->ops->CopyArea(pSrc, pDst",
        "LORIE_CORE_COPY_AREA")
counted("exa_unaccel.c", "\nExaCheckCopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,", "ret = pGC->ops->CopyArea(",
        "LORIE_CORE_COPY_AREA")
counted("exa_unaccel.c", "\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)",
        "pScreen->CopyWindow(pWin, ptOldOrg, prgnSrc);", "LORIE_CORE_COPY_WINDOW")
f = body("exa_unaccel.c", "\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)")
check(before(f, "RegionCopy(&lorieDst, prgnSrc);", "pScreen->CopyWindow(pWin, ptOldOrg, prgnSrc);"),
      "ExaCheckCopyWindow: what it writes is taken from prgnSrc after fbCopyWindow has moved it")

# The core copies offered to the GPU first (lorieCoreCopyOnGpu): before any access is opened - it waits
# for the renderer, which must not happen with the shared lock held - and, made, ending the fallback
# with nothing copied by the CPU.
def gpu_first(path, signature, call, firsts, post):
    f = body(path, signature)
    name = signature.strip().split("(")[0]
    check(all(before(f, call, x) for x in firsts), name + ": the GPU is offered the copy after an access is opened")
    tail = f[f.find(call):]
    check(re.search(r"if \(lorieDone\) \{\s*" + re.escape(post) + r";\s*return;\s*\}", tail) is not None and
          before(tail, "if (lorieDone)", "lorieCoreCopyBegin()"),
          name + ": made by the GPU, it does not end the fallback and return before the CPU's copy")
gpu_first("exa_unaccel.c", "\nExaCheckCopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,",
          "lorieCoreCopyOnGpu(LORIE_CORE_COPY_AREA,", ["prepare_access_reg", "exaPrepareAccess(", "lorieCoreCopyBegin()"],
          "EXA_POST_FALLBACK_GC(pGC)")
gpu_first("exa_unaccel.c", "\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)",
          "lorieCoreCopyOnGpu(LORIE_CORE_COPY_WINDOW,", ["lorieExaAccess(", "EXA_PREPARE_SRC", "lorieCoreCopyBegin()"],
          "EXA_POST_FALLBACK(pScreen)")
f = body("exa_unaccel.c", "\nExaCheckCopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,")
check("!bitplane && pGC->alu == GXcopy && EXA_PM_IS_SOLID(pDst, pGC->planemask)" in f,
      "ExaCheckCopyNtoN: the GPU is offered copies that are not plain (a bit plane, a raster op, a plane mask)")
# with the depth, which is what says whether there is alpha the GPU's copy would not keep
for sig, arg in (("\nExaCheckCopyNtoN(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC,", "pDst->depth,"),
                 ("\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)", "pDrawable->depth,")):
    f = body("exa_unaccel.c", sig)
    call = f[f.find("lorieCoreCopyOnGpu("):]
    check(arg in call[:call.find(";")], sig.strip().split("(")[0] + ": the GPU is not told the copy's depth")
f = body("exa_unaccel.c", "\nExaCheckCopyWindow(WindowPtr pWin, DDXPointRec ptOldOrg, RegionPtr prgnSrc)")
check(before(f, "RegionIntersect(&lorieDst, &lorieDst, &pWin->borderClip);", "lorieCoreCopyOnGpu(") and
      before(f, "exaGetDrawableDeltas(&pWin->drawable, loriePix", "lorieCoreCopyOnGpu("),
      "ExaCheckCopyWindow: what the GPU is given to write is not fbCopyWindow's destination in the pixmap")
# The patch's declaration of it, as lorie/InitOutput.c defines it.
def params(text, name):
    m = re.search(r"\bBool\s+" + name + r"\s*\(([^)]*)\)", text)
    # the types, in order: the names may differ
    return m and [re.sub(r"\s*\w+$", "", re.sub(r"\s+", " ", p).strip()) for p in m.group(1).split(",")]
init_c = open(os.path.join(os.path.dirname(os.path.abspath(PATCH)), "..", "lorie", "InitOutput.c")).read()
check(params(open(os.path.join(work, "exa/exa_priv.h")).read(), "lorieCoreCopyOnGpu") is not None and
      params(open(os.path.join(work, "exa/exa_priv.h")).read(), "lorieCoreCopyOnGpu") == params(init_c, "lorieCoreCopyOnGpu"),
      "lorieCoreCopyOnGpu in exa_priv.h is not lorie/InitOutput.c's")

# The copies that are a framebuffer copy of their own, counted as that and the context ended after.
def context(path, signature, call, ctx):
    f = body(path, signature)
    check(before(f, "lorieCopyContext(" + ctx + ");", call) and
          before(f[f.find(call):], call, "lorieCopyContext(LORIE_COPY_CTX_NONE);"),
          signature.strip().split("(")[0] + ": its copy is not counted under " + ctx)
context("present/present_execute.c", "\npresent_execute_copy(present_vblank_ptr vblank, uint64_t crtc_msc)",
        "present_copy_region(&window->drawable, vblank->pixmap", "LORIE_COPY_CTX_PRESENT")
context("present/present_scmd.c", "\npresent_restore_screen_pixmap(ScreenPtr screen)",
        "present_copy_region(&screen_pixmap->drawable, flip_pixmap", "LORIE_COPY_CTX_UNFLIP")

# The constants the patch repeats, because it cannot include lorie.h.
lorie_h = open(os.path.join(os.path.dirname(os.path.abspath(PATCH)), "..", "lorie", "lorie.h")).read()
def defined(text, name):
    m = re.search(r"#define " + name + r"\s+(\d+)", text)
    return m and int(m.group(1))
enum = re.search(r"enum \{\s*LORIE_CORE_COPY_WINDOW,[^}]*\}", lorie_h).group(0)
order = re.findall(r"(LORIE_CORE_COPY_\w+)", enum)
for name, text in (("LORIE_CORE_COPY_WINDOW", open(os.path.join(work, "exa/exa_priv.h")).read()),
                   ("LORIE_CORE_COPY_AREA", open(os.path.join(work, "exa/exa_priv.h")).read())):
    check(defined(text, name) == order.index(name), "%s in exa_priv.h is not lorie.h's" % name)
priv = open(os.path.join(work, "present/present_priv.h")).read()
for name in ("LORIE_COPY_CTX_NONE", "LORIE_COPY_CTX_PRESENT", "LORIE_COPY_CTX_UNFLIP"):
    check(defined(priv, name) is not None and defined(priv, name) == defined(lorie_h, name),
          "%s in present_priv.h is not lorie.h's" % name)

print("patch hooks (exa write paths reach lorieExaAccess, CPU copies counted, core copies offered to the GPU): %s (%d failures)" % ("FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
