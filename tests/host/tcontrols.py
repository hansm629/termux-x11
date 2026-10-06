#!/usr/bin/env python3
"""Negative controls: each breaks one rule of the code under test, in a copy of the sources, and the test
that guards that rule must then fail. A control whose text is no longer in the sources stops with an error
rather than passing, so a control cannot quietly stop testing anything.

usage: tcontrols.py <lorie source dir> <output dir> <3c98ffa lorie source dir>
"""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
L, OUT, BASE = sys.argv[1], sys.argv[2], sys.argv[3]
CC = os.environ.get("CC", "cc")

# (what breaks, file, text, replacement, test that must then fail)
CONTROLS = [
    ("the answer stored after the shared lock is let go", "InitOutput.c",
     """    __atomic_store_n(&pvfb->state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);
    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);""",
     """    lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    __atomic_store_n(&pvfb->state->backend.ownershipAck, LORIE_OWNERSHIP_WORD(seq, mode), __ATOMIC_RELEASE);""",
     "townership"),
    ("PERSISTENT does not lock the pixmaps again", "InitOutput.c",
     """            } else if (mode == LORIE_OWNERSHIP_PERSISTENT && !priv->locked)
                LorieBuffer_lock(priv->buffer, &priv->locked);""",
     """            }""",
     "townership"),
    ("PER_ACCESS keeps the pixmaps locked", "InitOutput.c",
     """            if (mode == LORIE_OWNERSHIP_PER_ACCESS && priv->locked) {""",
     """            if (0) {""",
     "townership"),
    ("PER_ACCESS FinishAccess lets the shared lock go before the CPU lock", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        if (!priv->wasLocked) {
            LorieBuffer_unlock(priv->buffer);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        return;
    }""",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (!priv->wasLocked) {
            LorieBuffer_unlock(priv->buffer);
            priv->locked = NULL;
            priv->wasLocked = FALSE;
        }
        return;
    }""",
     "townership"),
    ("the PER_ACCESS FinishAccess path taken under PERSISTENT too", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv)) {
        if (!priv->wasLocked) {""",
     """    if (lorieIsAhb(priv)) {
        if (!priv->wasLocked) {""",
     "townership"),
    ("a CPU access to an AHB under PER_ACCESS without the shared lock", "InitOutput.c",
     """    if (lorieAhbPerAccess() && lorieIsAhb(priv))
        return TRUE;
""",
     "",
     "townership"),
    ("a failed PrepareAccess under PER_ACCESS keeps the shared lock", "InitOutput.c",
     """            if (perAccessAhb)
                lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
            return FALSE;""",
     """            return FALSE;""",
     "townership"),
    ("PER_ACCESS CPU accesses not counted", "InitOutput.c",
     """        if (perAccessAhb)
            __atomic_fetch_add(&pvfb->state->diag.xAhbCpuAcquires, 1, __ATOMIC_RELAXED);""",
     "",
     "townership"),
    ("a session asked for gles granted PER_ACCESS", "InitOutput.c",
     """(mode != LORIE_OWNERSHIP_PER_ACCESS || pvfb->state->backend.requested == LORIE_BACKEND_GLES)""",
     """(mode != LORIE_OWNERSHIP_PER_ACCESS)""",
     "townership"),
    ("an answered request taken up again", "InitOutput.c",
     """    if (LORIE_OWNERSHIP_SEQ(req) == LORIE_OWNERSHIP_SEQ(ack))
        return;
""",
     "",
     "townership"),
    ("a new AHB pixmap locked whatever the mode", "InitOutput.c",
     """    if (!priv->locked && !(lorieIsAhb(priv) && lorieAhbPerAccess()))
        LorieBuffer_lock(priv->buffer, &priv->locked);""",
     """    if (!priv->locked)
        LorieBuffer_lock(priv->buffer, &priv->locked);""",
     "townership"),
    ("a pixmap made GPU-sampleable locked as 3c98ffa did, whatever the mode", "InitOutput.c",
     """            priv->locked = NULL;
            lorieHoldPersistent(priv);""",
     """            LorieBuffer_lock(priv->buffer, &priv->locked);""",
     "townership"),
    ("the root window's new pixmap not remembered", "InitOutput.c",
     """        if (new && new->buffer && !new->locked) {
            lorieHoldPersistent(new);""",
     """        if (new && new->buffer && !new->locked) {
            LorieBuffer_lock(new->buffer, &new->locked);""",
     "townership"),
    ("a destroyed pixmap left on the list", "InitOutput.c",
     """    LoriePixmapPriv *priv = driverPriv;
    if (priv->persistentWanted) {
        xorg_list_del(&priv->persistentLink);
        priv->persistentWanted = FALSE;
    }""",
     """    LoriePixmapPriv *priv = driverPriv;""",
     "townership"),
    ("drawSquares lets the shared lock go before the CPU lock", "InitOutput.c",
     """    if (perAccess) {
        if (pixels)
            LorieBuffer_unlock(priv->buffer);
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
    }""",
     """    if (perAccess) {
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);
        if (pixels)
            LorieBuffer_unlock(priv->buffer);
    }""",
     "townership"),
    ("a lost connection does not reset the ownership", "InitOutput.c",
     """static Bool lorieRendererGoneWorkProc(__unused ClientPtr client, __unused void *closure) {
    lorieResetOwnership();""",
     """static Bool lorieRendererGoneWorkProc(__unused ClientPtr client, __unused void *closure) {""",
     "townership"),
    ("TERMUX_X11_RENDERER_BACKEND matched without case", "InitOutput.c",
     """    if (value && !strcmp(value, "vulkan"))""",
     """    if (value && !strcasecmp(value, "vulkan"))""",
     "townership"),
    ("lorieRedraw without the ownership request", "InitOutput.c",
     """
    lorieProcessOwnershipRequest();
""",
     "\n",
     "twiring"),
    ("a new connection without the reset", "InitOutput.c",
     """    lorieResetOwnership();      // a new renderer starts on GLES, with 3c98ffa's ownership
""",
     "",
     "twiring"),
    ("a lost connection not reported", "cmdentrypoint.c",
     """        lorieRendererDisconnected();    // AHB ownership back to 3c98ffa's (InitOutput.c)
""",
     "",
     "twiring"),
]

def copy_sources(dst):
    os.makedirs(dst, exist_ok=True)
    for f in os.listdir(L):
        p = os.path.join(L, f)
        if os.path.isfile(p) and not os.path.islink(p) and f.endswith((".c", ".h")):
            shutil.copyfile(p, os.path.join(dst, f))

fails = 0
for n, (what, file, old, new, test) in enumerate(CONTROLS):
    work = os.path.join(OUT, "controls", str(n))
    src, gen = os.path.join(work, "lorie"), os.path.join(work, "gen")
    shutil.rmtree(work, ignore_errors=True)
    copy_sources(src)
    os.makedirs(gen)
    path = os.path.join(src, file)
    text = open(path, encoding="utf-8").read()
    if text.count(old) != 1:
        sys.exit("control %r: its text is in %s %d times, not once - update the control" % (what, file, text.count(old)))
    open(path, "w", encoding="utf-8").write(text.replace(old, new))

    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", OUT=gen)
    if test == "twiring":
        r = subprocess.run([sys.executable, os.path.join(HERE, "twiring.py"), src], capture_output=True, text=True, env=env)
    else:
        subprocess.run([sys.executable, os.path.join(HERE, "gen.py"), src, gen, BASE], check=True, env=env)
        exe = os.path.join(gen, test)
        b = subprocess.run([CC, "-Wall", "-Wno-unused-function", "-O1", "-pthread", "-I" + gen, "-I" + HERE,
                            os.path.join(HERE, test + ".c"), "-o", exe, "-lm"], capture_output=True, text=True)
        if b.returncode != 0:
            fails += 1
            print("  FAIL control %r: does not build, so it shows nothing\n%s" % (what, b.stderr[-2000:]))
            continue
        r = subprocess.run([exe], capture_output=True, text=True, env=env)
    if r.returncode == 0:
        fails += 1
        print("  FAIL control %r: %s still passes" % (what, test))
    else:
        first = next((l.strip() for l in r.stdout.splitlines() if l.strip().startswith("FAIL")), "(no FAIL line)")
        print("  control %d, %s: %s" % (n, what, first[:220]))
    shutil.rmtree(work, ignore_errors=True)

print("negative controls (%d, each must fail its test): %s (%d not detected)" % (
    len(CONTROLS), "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
