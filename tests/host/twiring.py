#!/usr/bin/env python3
"""Checks that the X server calls the AHB ownership code where the handshake needs it (townership.c tests
the code itself): every frame's lorieRedraw takes up the renderer's request before anything can return
early; a new renderer connection and a lost one both go back to 3c98ffa's ownership; the requested backend
is read from TERMUX_X11_RENDERER_BACKEND when the server state is made; under PER_ACCESS lorieRedraw allows the
tick's frame before its vblanks queue the Present copies (root double buffering: swaps after them instead),
and a Vulkan frame takes the root the X server publishes under the lock.

And the order of the renderer's Vulkan steps (renderer.c; tvkframe.c, tvkrenderer.c and tpresentcopy.c test the
steps): a frame acquires its swapchain image before taking the X server's lock, checks under the lock that the X
server's PER_ACCESS answer is still its own before touching a buffer, submits and lists the frame's buffers in
flight before letting the lock go, then waits for its fence, gives the buffers back, publishes the completed
copies, and presents; the copies alone likewise; teardown finds the device idle (or lost) before it clears a
frame of ours left not over; a failure is torn down before the X server is asked for its ownership back, and that before
GLES takes the window; a window's swapchain goes before the window; the backend is chosen after the window
change was acknowledged, before stateLock is let go; a buffer's Vulkan image goes before the buffer; another
X server session (or none) drops every imported image and the ownership answer of the last one first. The
self-test's duration is lorieVkElapsedUs's (tvkselftest.c tests it) on both ways out. A buffer that comes gets
its GLES texture only when rendererGlesEager says GLES draws, a buffer found for a copy none, and GLES's frame
makes the root's (rendererGlBuffer) before binding it.

And P11-A: the vsync tick and a GPU copy's completion are queued as urgent work (lorieQueueUrgentWorkProc, which
tdispatchyield.c tests), with TERMUX_X11_X_YIELD_ON_WAKE read when the server state is made, "1" only. P11-C2: a
root copy asked of the renderer is made first in its loop, which it wakes, and the X server waits for one before it
takes the shared lock. P11-C3: TERMUX_X11_RENDERER_VK_FRAME_DIAG read when the server state is made, "1" only, into the
renderer's flag and Dispatch's; the tick checks the last frame it asked for before it can return early, and stamps
its request before it wakes the renderer; a Vulkan frame is timed only once presented, and only with the flag; the
pacing intervals start over with another session, window or device, and a root copy counts as ahead of the frame only
in the loop pass that made it. P11-C3-B1: TERMUX_X11_EXA_LOCK_OPT read when the server state is made, "1" only; copies
left queued for a reservation are no reason for the renderer to run again until one is let go, its wait for that
bounded, and they are forgotten with the session. P11-C3-B2: TERMUX_X11_RENDERER_VK_SCHEDULE_OPT read when the server
state is made, "1" only; the root copy submitted ahead only for a frame this pass draws, settled at the end of the pass
should none follow; the frame waits first should the X server already wait for it, is submitted waiting for it on the
device, and waits for its fence only after that submission; no renderer path waits for this process's own lists;
teardown ends the second submission's list and settles a root copy left pending.

usage: twiring.py <lorie source dir>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import func

L = sys.argv[1]
I, C = os.path.join(L, "InitOutput.c"), os.path.join(L, "cmdentrypoint.c")
fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print("  FAIL " + what)

def before_first_return(body, call):
    i, r = body.find(call), body.find("return")
    return i >= 0 and (r < 0 or i < r)

redraw = func(I, "static Bool lorieRedraw(__unused ClientPtr pClient, void *closure) {")
check(before_first_return(redraw, "lorieProcessOwnershipRequest();"),
      "lorieRedraw does not take up the ownership request before its first return")

# Vulkan: the tick's frame allowed before the vblanks queue their Present copies, so they go in that frame
early = "if (lorieAhbPerAccess() && !twinned)\n        pvfb->state->waitForNextFrame = false;"
check(redraw.find(early) >= 0 and redraw.find(early) < redraw.find("loriePerformVblanks();"),
      "lorieRedraw does not allow a Vulkan frame before its vblanks queue the Present copies")
check("lorieFrameGateRearmAfterVblanks(pvfb->vkSingleFramePerTick," in redraw and
      "preVblankVulkanRelease" in redraw and
      redraw.find("lorieFrameGateRearmAfterVblanks(") > redraw.find("lorieRecheckGpuCopies();"),
      "one-frame-per-tick opt-in does not protect the second post-vblank rearm")
check("else if (gpuCopyPending)\n            rendererVkApplyPendingGpuCopies();" in
      open(os.path.join(L, "renderer.c"), encoding="utf-8").read(),
      "one-frame-per-tick must keep processing GPU Present copies without a screen frame")

# V8: a CPU reservation must be published while holding state->lock, before
# dropping it and before GPU-completion waiting. GPU-copy and root sampling
# both honor the reservation until the X server releases it.
reserve_src = func(I, "static Bool lorieCpuReserve(uint64_t id) {")
prepare_src = func(I, "Bool loriePrepareAccess(PixmapPtr pPix, int index) {")
renderer_src = open(os.path.join(L, "renderer.c"), encoding="utf-8").read()
check("pvfb->exaEarlyReserveRoot && lorieGpuInFlight(st, id)" in reserve_src,
      "V8 early root reservation must require an actual in-flight GPU frame")
check("pvfb->exaEarlyReserve && pvfb->accessLocks == 1" in prepare_src and
      prepare_src.find("priv->reserved = reservedBeforeWait = TRUE;") <
      prepare_src.find("lorieGpuWaitIdle(pvfb->state, false,") and
      prepare_src.find("lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);") <
      prepare_src.find("lorieGpuWaitIdle(pvfb->state, false,"),
      "V8 must reserve an AHB and drop the global lock before waiting for its prior GPU frame")
check("if (owned && lorieCpuReserved(state, desc->id))" in renderer_src and
      "vkr.held = true;" in renderer_src and
      "syscall(SYS_futex, &state->cpuAccess.releases" in renderer_src,
      "V8 root reservation requires the Vulkan renderer's safe held-swapchain retry")

connected = func(I, "void lorieActivityConnected(void) {")
check("lorieResetOwnership();" in connected, "a new renderer connection does not reset the ownership")

events = func(C, "void handleLorieEvents(int fd, __unused int ready, __unused void *ignored) {")
err = events[events.find("if (ready & X_NOTIFY_ERROR) {"):]
check(events.find("if (ready & X_NOTIFY_ERROR) {") >= 0 and before_first_return(err, "lorieRendererDisconnected();"),
      "a lost renderer connection does not hand the ownership back")

init = func(I, "void OsVendorInit(void) {")
check('lorieScreen.state->backend.requested = lorieRequestedBackend(getenv("TERMUX_X11_RENDERER_BACKEND"),' in init,
      "OsVendorInit does not read TERMUX_X11_RENDERER_BACKEND into the server state")

# P11-C3-E2: no new timing clocks or causal samples without the explicit diagnostic option AND the ordinary
# Vulkan frame diagnostics. The probe does not alter the Xorg or Android CPU scheduler.
check('lorieScreen.syncChainDiag = lorieScreen.frameDiag && getenv("TERMUX_X11_SYNC_CHAIN_DIAG")' in init and
      '!strcmp(getenv("TERMUX_X11_SYNC_CHAIN_DIAG"), "1")' in init,
      "sync-chain probe not gated by both the frame diagnostic and an explicit opt-in")

# P11-A: the tick and the completion as urgent work - and nowhere queued as any work
frame = func(I, "void lorieChoreographerFrameCallback(long t, AChoreographer* d) {")
check("lorieQueueUrgentWorkProc(lorieRedraw, " in frame and "QueueWorkProc(lorieRedraw" not in frame.replace("lorieQueueUrgentWorkProc(", ""),
      "the vsync tick not queued as urgent work")
check("lorieQueueUrgentWorkProc(handleGpuCopyDoneEvent, " in events and
      "QueueWorkProc(handleGpuCopyDoneEvent" not in events.replace("lorieQueueUrgentWorkProc(", ""),
      "a GPU copy's completion not queued as urgent work")
check('getenv("TERMUX_X11_X_YIELD_ON_WAKE") && !strcmp(getenv("TERMUX_X11_X_YIELD_ON_WAKE"), "1")' in init,
      "OsVendorInit does not read TERMUX_X11_X_YIELD_ON_WAKE=1")

R = os.path.join(L, "renderer.c")
def in_order(what, body, *steps):
    at = 0
    for s_ in steps:
        i = body.find(s_, at)
        if i < 0:
            check(False, "%s: %r missing, or not after %r" % (what, s_, steps[steps.index(s_) - 1] if steps.index(s_) else "the start"))
            return
        at = i + len(s_)

# root double buffering: swapped after the vblanks, before the frame is allowed and the front published
in_order("root double buffering", func(I, "static Bool lorieRedraw(__unused ClientPtr pClient, void *closure) {"),
         "loriePerformVblanks();", "lorieRootSwap(priv)", "rootDouble ? priv->twin : priv->buffer")

redraw = func(R, "static void rendererVkRedraw(bool *waitingForBuffers) {")
# the swapchain is made again when out of date (vk.c); sizes come with a new surface, not from querying the window
check("ANativeWindow_get" not in redraw, "a Vulkan frame compares the window's size with the swapchain's")
in_order("Vulkan frame", redraw, "lorieVkAcquire(", "rendererVkLock()", "rendererVkOwnedLocked()",
         "state->rootWindowTextureID != desc->id", "rendererVkCopiesLocked(", "lorieVkFrameQueue(", "rendererVkInFlight();", "lorie_mutex_unlock(&state->lock",
         "lorieVkFrameWait(", "rendererVkFrameDone();", "completedSerial", "lorieVkPresent(")
alone = func(R, "static void rendererVkApplyPendingGpuCopies(void) {")
in_order("Vulkan copies alone", alone, "rendererVkLock()", "rendererVkOwnedLocked()", "rendererVkCopiesLocked(",
         "lorieVkFrameQueue(", "rendererVkInFlight();", "lorie_mutex_unlock(&state->lock", "lorieVkFrameWait(",
         "rendererVkFrameDone();", "completedSerial")
select = func(R, "static void rendererSelectBackend(void) {")
in_order("backend choice", select, "rendererDropEgl();", "rendererVkStart()", "vkFailed = true;", "rendererVkTeardown();",
         "rendererVkOwnership(LORIE_OWNERSHIP_PERSISTENT)", "rendererUseEgl();")
fail = func(R, "static void rendererVkFail(void) {")
in_order("Vulkan failure", fail, "vkFailed = true;", "rendererVkTeardown();", "vkGiveBack = backendSelect = true;")
refresh = func(R, "void rendererRefreshContext(void) {")
in_order("window change", refresh, "lorieVkSwapchainDestroy(&vk, &vkr);", "releaseWinAndSurface(&win, &sfc);")
loop = func(R, "__noreturn static void* rendererThread(void) {")
in_order("main loop", loop, "pthread_cond_signal(&stateChangeFinishCond);", "rendererSelectBackend();", "pthread_mutex_unlock(&stateLock);",
         "lorieVkImageRemove(&vk, &vkImages", "LorieBuffer_release(buf);")

# P11-C2: a root copy the X server asked for is made before anything else the renderer draws, once stateLock is let
# go, and wakes the loop; the X server waits for one before it takes the shared lock (the renderer takes the request
# under it) - in its EXA access, its ownership answer and its own drawing
# the loop's pass of work (rendererPass, which tpresentcopy.c runs): once stateLock is let go, before the buffers go
rpass = func(R, "static void rendererPass(bool *waitingForBuffers) {")
in_order("the renderer's pass in its loop", loop, "pthread_mutex_unlock(&stateLock);", "rendererPass(&waitingForBuffers);",
         "while((buf = LorieBufferList_first(&removedBuffers)))")
in_order("root copy in the main loop", rpass, "rendererVkRootCopy();", "rendererVkRedraw(waitingForBuffers);")
check("rendererRootCopyPending()" in func(R, "static inline __always_inline bool rendererShouldWait(bool *waitingForBuffers) {"),
      "the renderer sleeps through a root copy asked for")
for what, sig in (("EXA access", "Bool loriePrepareAccess(PixmapPtr pPix, int index) {"),
                  ("ownership answer", "static void lorieApplyOwnership(uint8_t mode, uint32_t seq) {"),
                  ("drawSquares", "Bool drawSquares() {")):
    in_order("root copy waited for before the shared lock: " + what, func(I, sig), "lorieRootCopyWait(",
             "lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);")
# another session: the last one's imported images and ownership answer are dropped before anything else runs
in_order("session change", loop, "if (stateChanged) {", "state = pendingState;", "vk.DeviceWaitIdle(vk.device);",
         "lorieVkImagesClear(&vk, &vkImages);", "vkOwned = false;", "if (windowChanged)")
in_order("Vulkan teardown", func(R, "static void rendererVkTeardown(void) {"), "rendererVkIdle();", "rendererVkFrameDone();",
         "lorieVkImagesClear(&vk, &vkImages);", "lorieVkDestroyDevice(&vk);")

check("            if (rendererGlesEager())\n                rendererAttachToGL(buf);" in loop,
      "a buffer that comes gets a GLES texture whoever draws")
find = func(R, "static LorieBuffer *rendererFindBuffer(uint64_t id, bool *later) {")
check("rendererAttachToGL" not in find, "a buffer found for a copy gets a GLES texture whoever copies")
# P11-C3-C: the buffers a pass's copies use are waited for before the shared lock (rendererAwaitBuffers, first in the
# pass), never under it: the lookups under it neither sleep nor wait, and every copy path looks up that way
check("usleep" not in find and "futex" not in find and "rendererFindBufferWithRetry" not in open(R, encoding="utf-8").read(),
      "a copy's buffer is still waited for under the shared lock")
in_order("the pass waits for its buffers first", rpass, "rendererAwaitBuffers();", "if (state && rendererRootCopyPending()) {",
         "rendererVkRedraw(waitingForBuffers);", "rendererRedrawLocked(waitingForBuffers);")
check("lorie_mutex_lock" not in func(R, "static void rendererAwaitBuffers(void) {"), "the buffers waited for under the shared lock")
for what, sig in (("GLES copies", "static uint64_t rendererApplyPendingGpuCopiesLocked(void) {"),
                  ("Vulkan copies", "static bool rendererVkCopiesLocked(uint64_t *serial, uint32_t *readIndex) {")):
    in_order("a buffer that may still come leaves the copies queued: " + what, func(R, sig), "rendererFindBuffer(entry.srcBufferId, &later);",
             "rendererFindBuffer(entry.dstBufferId, &later);", "if ((!src || !dst) && later) {", "break;")
in_order("GLES frame", func(R, "void rendererRedrawLocked(bool* waitingForBuffers) {"), "rendererGlBuffer(buffer);",
         "LorieBuffer_bindTexture(buffer);")

# P11-C3: the frame diagnostics' wiring
check('lorieScreen.frameDiag = getenv("TERMUX_X11_RENDERER_VK_FRAME_DIAG") && !strcmp(getenv("TERMUX_X11_RENDERER_VK_FRAME_DIAG"), "1");' in init
      and "lorieScreen.state->backend.vkFrameDiag = lorieScreen.frameDiag;" in init and "dispatchRequestTimed = lorieScreen.frameDiag || lorieScreen.freezeDiag;" in init,
      "OsVendorInit does not read TERMUX_X11_RENDERER_VK_FRAME_DIAG=1 into the renderer's flag and Dispatch's")
xredraw = func(I, "static Bool lorieRedraw(__unused ClientPtr pClient, void *closure) {")
check(before_first_return(xredraw, "lorieTickTimed(tickUs, ") and "uint64_t tickUs = pvfb->frameDiag || pvfb->freezeDiag ? lorieMonotonicUs() : 0;" in xredraw,
      "lorieRedraw does not check the last frame asked for before its first return, or reads the clock with the option off")
check("        if (tickUs)\n            lorieTickAsked(tickUs);\n        lorieWakeRenderer(rendererWake);" in xredraw,
      "lorieRedraw does not stamp its request right before waking the renderer")
in_order("Vulkan frame timed", redraw, "bool diag = state->backend.vkFrameDiag;", "lorieVkFrameWait(", "ft.fenceUs = vkr.fenceWaitUs;",
         "lorieVkPresent(", "state->renderedFrames++;", "if (diag) {", "rendererVkFrameTimed(&ft);")
def guarded(body, line_index):
    """the statement on that line is under `if (diag)`: the nearest line above it indented less says so"""
    lines = body.split("\n")
    indent = len(lines[line_index]) - len(lines[line_index].lstrip())
    for k in range(line_index - 1, -1, -1):
        if lines[k].strip() and len(lines[k]) - len(lines[k].lstrip()) < indent:
            return lines[k].strip() in ("if (diag)", "if (diag) {")
    return False
stamps = [k for k, l in enumerate(redraw.split("\n")) if "lorieMonotonicUs()" in l and ("ft." in l or "acquireStartUs" in l)]
check(len(stamps) == 9 and all(guarded(redraw, k) or "diag ? lorieMonotonicUs() : 0" in redraw.split("\n")[k] for k in stamps) and
      "rendererVkFrameTimed" not in alone, "a Vulkan frame's stamps (%d) not all under the frame diagnostics' flag" % len(stamps))
in_order("pacing over with another session", loop, "if (stateChanged) {", "rendererVkPacingReset();", "if (windowChanged)")
in_order("pacing over with another window", refresh, "lorieVkSwapchainDestroy(&vk, &vkr);", "rendererVkPacingReset();",
         "releaseWinAndSurface(&win, &sfc);")
in_order("pacing over with another device", func(R, "static void rendererVkTeardown(void) {"), "lorieVkDestroyDevice(&vk);",
         "rendererVkPacingReset();")
in_order("root copy ahead of the frame, this pass only", rpass, "vkRootCopyBusyUs = 0;", "rendererVkRootCopy();",
         "rendererVkRedraw(waitingForBuffers);")

# P11-C3-B1: the reservations' wiring
check('lorieScreen.exaLockOpt = getenv("TERMUX_X11_EXA_LOCK_OPT") && !strcmp(getenv("TERMUX_X11_EXA_LOCK_OPT"), "1");' in init,
      "OsVendorInit does not read TERMUX_X11_EXA_LOCK_OPT=1")
should = func(R, "static inline __always_inline bool rendererShouldWait(bool *waitingForBuffers) {")
check("gpuCopyPending = state && state->gpuCopyQueue.readIndex != state->gpuCopyQueue.writeIndex && !rendererCopiesStillDeferred();" in should,
      "copies left queued for a reservation wake the renderer again and again")

# P11-C3-C: the freeze watchdog (TERMUX_X11_FRAME_FREEZE_DIAG=1) - read in OsVendorInit into the renderer's flag and
# Dispatch's; its thread made only then; off, no stage kept and no clock read on either side; the thread itself takes no
# lock and waits for nothing but its own sleep; the request's start handed to the DDX only while requests are timed
check('lorieScreen.freezeDiag = getenv("TERMUX_X11_FRAME_FREEZE_DIAG") && !strcmp(getenv("TERMUX_X11_FRAME_FREEZE_DIAG"), "1");' in init
      and "lorieScreen.state->backend.freezeDiag = lorieScreen.freezeDiag;" in init,
      "OsVendorInit does not read TERMUX_X11_FRAME_FREEZE_DIAG=1 into the renderer's flag")
in_order("the watchdog's thread only with the option", init, "if (lorieScreen.freezeDiag) {", "pthread_create(&watch, NULL, lorieFreezeWatch, NULL)")
check(open(I, encoding="utf-8").read().count("lorieFreezeWatch, NULL)") == 1, "the watchdog's thread made elsewhere too")
stage = func(R, "static void rendererStage(uint32_t stage) {")
check(stage.index("if (state && state->backend.freezeDiag) {") < stage.index("lorieMonotonicUs()"), "the renderer's stage kept with the option off")
check(func(R, "static bool rendererProgressOn(void) {").count("state->backend.vkFrameDiag || state->backend.freezeDiag") == 1,
      "the asks kept without either option")
xenter = func(I, "static struct lorieXMark lorieXEnter(uint32_t stage) {")
check(xenter.index("if (pvfb->freezeDiag) {") < xenter.index("lorieMonotonicUs()") and
      "if (pvfb->freezeDiag) {" in func(I, "static void lorieXLeave(struct lorieXMark *prev) {"),
      "the X server's stage kept with the option off")
fsrc = open(I, encoding="utf-8").read()
watch = fsrc[fsrc.index("/* ---- P11-C3-C, TERMUX_X11_FRAME_FREEZE_DIAG=1: the freeze watchdog ---- */"):fsrc.index("static Bool lorieCreateScreenResources(")]
for bad in ("lorie_mutex_lock", "lorie_mutex_trylock", "pthread_mutex_lock", "input_lock", "lorieGpuWaitIdle", "lorieRootCopyWait",
            "WaitForFences", "pthread_cond_wait", "FUTEX_WAIT"):
    check(bad not in watch, "the freeze watchdog calls %s" % bad)
# patches/xserver.patch - or, for a negative control, the changed copy of it (tcontrols.py)
dsp = open(os.environ.get("LORIE_XSERVER_PATCH") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "app", "src",
           "main", "cpp", "patches", "xserver.patch"), encoding="utf-8").read()
check("CARD64 timedUs = dispatchRequestTimed ? ddxRequestStart(client->index, client->majorOp, client->minorOp) : 0;" in dsp,
      "Dispatch does not hand the request's start to the DDX only while requests are timed")
for what, body, mark in (("tick", xredraw, "struct lorieXMark tickMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_TICK);"),
                         ("root copy wait", func(I, "static void lorieRootCopyWait(Bool takeBack) {"),
                          "struct lorieXMark waitMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_ROOT_COPY_WAIT);"),
                         ("report", func(I, "static void lorieReport(void) {"),
                          "struct lorieXMark reportMark __attribute__((cleanup(lorieXLeave))) = lorieXEnter(LORIE_X_REPORT);")):
    check(mark in body, "the X server's thread not marked in its %s" % what)
in_order("the renderer marked asleep only as it sleeps", func(R, "static void rendererWaitForWork(bool *waitingForBuffers) {"),
         "rendererStage(LORIE_R_SLEEP);", "rendererSleep(seen, -1);", "rendererStage(LORIE_R_PASS);")
in_order("the Vulkan frame's stages", redraw, "rendererStage(LORIE_R_FRAME);", "rendererStage(LORIE_R_ACQUIRE);", "lorieVkAcquire(",
         "rendererStage(LORIE_R_LOCK_WAIT);", "rendererVkLock()", "rendererStage(LORIE_R_FENCE_WAIT);", "lorieVkFrameWait(",
         "rendererStage(LORIE_R_PRESENT);", "lorieVkPresent(", "state->progress.takenSeq = askSeq;")

# P11-C3-C: the frame gaps (TERMUX_X11_RENDERER_VK_FRAME_DIAG): each frame's gap classified before the frame starts'
# samples drop a long one as idle; the asks read at the frame's start, under the option
in_order("the gap classified before the long interval is dropped", func(R, "static void rendererVkFrameTimed(const struct rendererVkFrameTimes *t) {"),
         "lastStartUs = vkPacingLastStartUs;", "rendererVkFrameGap(t, lastStartUs, vkPacingLast.ticksRun);",
         "rendererVkInterval(&d->vkFrameStart, &vkPacingLastStartUs, t->startUs, &interval);", "vkPacingLast.ticksRun = t->ticksRun;")
in_order("the asks read as the frame begins", redraw, "if (diag) {", "ft.startUs = lorieMonotonicUs();",
         "ft.firstAskUs = askSeq != ft.takenSeq ? lorieAskUs(&state->progress, ft.takenSeq + 1, askSeq) : 0;", "lorieVkAcquire(")
in_order("the renderer's sleep stamped for the gaps", func(R, "static void rendererWaitForWork(bool *waitingForBuffers) {"),
         "if (rendererProgressOn())", "state->progress.sleepUs = lorieMonotonicUs();", "rendererSleep(seen, -1);")

# P11-C3-C: the 5 second report made as work on the X server's thread, not in the timer DoTimers runs under the input lock
counter = func(I, "static CARD32 lorieFramecounter(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {")
check("QueueWorkProc(lorieReportWorkProc, NULL, NULL);" in counter and "log(" not in counter and "lorieLogRendererDiag" not in counter
      and "lorieReport();" in func(I, "static Bool lorieReportWorkProc(__unused ClientPtr client, __unused void *closure) {"),
      "the 5 second report made in the timer, under the input lock")

# P11-C3-D0: client content (TERMUX_X11_CONTENT_FRAME_DIAG): the option read into its flags; the patch hands each step to
# the hooks where it happens; the accesses carry content only with the option on, a stale read looked for under the lock
# before the wait for a frame; the screen pixmap published only where its damage is taken up (swapped or drawn: never
# at a tick for the cursor alone); the renderer takes the content under the lock, counts it once presented
check('lorieScreen.contentDiag = getenv("TERMUX_X11_CONTENT_FRAME_DIAG") && !strcmp(getenv("TERMUX_X11_CONTENT_FRAME_DIAG"), "1");' in init
      and "lorieScreen.state->backend.contentDiag = lorieScreen.contentDiag;" in init,
      "OsVendorInit does not read TERMUX_X11_CONTENT_FRAME_DIAG=1 into the renderer's flag")
xp = dsp
for what, first, then in (
        ("the request numbered", "     vblank->event_id = ++present_scmd_event_id;",
         "+    vblank->lorie_content_seq = lorieContentRequest(pixmap, &vblank->lorie_content_us, &vblank->lorie_content_client);"),
        ("its GPU copy done", "+        lorieGpuCopyAck(vblank->pixmap, vblank->gpu_copy_dst_buffer);\n+        vblank->gpu_copy_pending = FALSE;\n",
         "+        lorieContentCopyDone(vblank->lorie_content_seq, vblank->lorie_content_us, vblank->lorie_copy_us, gpuCopyDst);"),
        ("its GPU copy queued", "+        vblank->gpu_copy_pending = TRUE;\n",
         "+        vblank->lorie_copy_us = lorieContentCopied(vblank->lorie_content_seq, vblank->lorie_content_us, gpuCopyDst, TRUE);"),
        ("its CPU copy", "+        present_copy_region(&window->drawable, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off);",
         "+        lorieContentCopied(vblank->lorie_content_seq, vblank->lorie_content_us, gpuCopyDst, FALSE);"),
        ("its CompleteNotify", "+    lorieContentComplete(vblank->lorie_content_seq, mode);     /* lorie: P11-C3-D0 */",
         "present_vblank_notify(vblank, vblank->kind, mode, ust, crtc_msc);"),
        ("skipped", "present_vblank_scrap(present_vblank_ptr vblank)\n {\n",
         "+    lorieContentSkipped(vblank->lorie_content_seq, vblank->lorie_content_client);     /* lorie: P11-C3-D0 */")):
    i, j = xp.find(first), xp.find(then)
    check(i >= 0 and j > i and j - i < 400, "patches/xserver.patch: the content hook for %s not where it happens" % what)
prep = func(I, "Bool loriePrepareAccess(PixmapPtr pPix, int index) {")
# ... clients gone told of only with the option on, every server generation (dix main deletes the callbacks at a reset)
check("    if (pvfb->contentDiag)\n        AddCallback(&ClientStateCallback, lorieContentClientGone, NULL);" in
      func(I, "static Bool lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {") and
      open(I, encoding="utf-8").read().count("AddCallback(&ClientStateCallback, lorieContentClientGone") == 1,
      "clients gone not told of with the content option on, or told of without it")
check("    if (pvfb->contentDiag && priv)\n        lorieContentAccess(pPix, priv, index, LORIE_CONTENT_OPEN);" in prep and
      "    if (pvfb->contentDiag && priv)\n        lorieContentAccess(pPix, priv, index, LORIE_CONTENT_DONE);" in
      func(I, "void lorieFinishAccess(PixmapPtr pPix, int index) {"), "the accesses do not carry content only with the option on")
# ... and an access whose PrepareAccess fails (EXA calls no FinishAccess for it: ExaDoPrepareAccess) is undone before it
# returns, its lock and reservation let go first
in_order("a failed access undone for the content", prep, 'Failed to lock buffer, err %d',
         "lorie_mutex_unlock(&pvfb->state->lock", "lorieContentAccess(pPix, priv, index, LORIE_CONTENT_FAILED);", "return FALSE;")
check(prep.count("return FALSE;") == 1, "loriePrepareAccess fails other than where the content is undone")
in_order("a stale read looked for under the lock, before the frame's wait", prep, "lorie_mutex_lock(&pvfb->state->lock",
         "lorieContentStaleRead(priv, index);", "lorieGpuWaitIdle(")
in_order("published where the damage is taken up", xredraw,
         "if (rootDouble && pvfb->rootGpuCopyPending == 0 && (nonEmpty || pvfb->rootTwinStale)) {", "lorieRootSwap(priv)",
         "DamageEmpty(pvfb->damage);", "pvfb->state->drawRequested = TRUE;", "lorieContentPublish(priv);",
         "if (pvfb->state->drawRequested || pvfb->state->cursor.moved || pvfb->state->cursor.updated) {")
check(xredraw.count("lorieContentPublish(") == 1 and
      "    if (nonEmpty && priv->buffer && (!rootDouble || pvfb->rootGpuCopyPending == 0)) {" in xredraw and
      xredraw.split("    if (nonEmpty && priv->buffer && (!rootDouble || pvfb->rootGpuCopyPending == 0)) {", 1)[1].split("\n    }\n", 1)[0].count("\n        lorieContentPublish(priv);") == 1,
      "the screen pixmap published at a tick for the cursor too (outside the block that takes its damage up)")
in_order("the content taken under the lock, counted once presented", redraw, "rendererVkLock()", "rendererContentRead(&content);",
         "lorie_mutex_unlock(&state->lock", "lorieVkPresent(", "if (presented == LORIE_VK_FRAME_OK)", "rendererContentFrame(&content, lorieMonotonicUs());")

# P11-C3-D1: the RENDER composites timed (TERMUX_X11_RENDERER_VK_FRAME_DIAG; texacomposite.c runs the timing) - wrapped
# over EXA's once the screen's pictures and EXA are set up, and only with the option on; the EXA accesses counted for
# them only then
sinit = func(I, "static Bool lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {")
in_order("the composites timed over EXA's", sinit, "!exaDriverInit(pScreen, &lorieExa)", "!present_screen_init(pScreen, &loriePresentInfo)",
         "    if (pvfb->frameDiag || pvfb->xrenderGpu) {\n        PictureScreenPtr ps = GetPictureScreen(pScreen);\n        pvfb->composite = ps->Composite;\n"
         "        ps->Composite = pvfb->frameDiag ? lorieCompositeTimed : lorieCompositeGpuOnly;\n    }")
check(sinit.count("lorieCompositeTimed") == 1, "the composites timed other than with the frame diagnostics on")
check("    if (pvfb->frameDiag)\n        pvfb->xAccessesOpened++;" in prep and open(I, encoding="utf-8").read().count("xAccessesOpened++") == 1,
      "the EXA accesses counted for the composites without the frame diagnostics, or not counted")

# P11-C3-D1-A: TERMUX_X11_EXA_COMPOSITE_OPT read when the server state is made, "1" only; the copies' damage put off only
# with it (tpresentsafety.c runs the patch's present_damage_gpu_copy and its callers)
check('lorieScreen.exaCompositeOpt = getenv("TERMUX_X11_EXA_COMPOSITE_OPT") && !strcmp(getenv("TERMUX_X11_EXA_COMPOSITE_OPT"), "1");'
      in init, "OsVendorInit does not read TERMUX_X11_EXA_COMPOSITE_OPT=1")
check("    if (!pvfb->exaCompositeOpt)\n        return FALSE;" in func(I, "Bool lorieDeferCopyDamage(void) {"),
      "a copy's damage put off with TERMUX_X11_EXA_COMPOSITE_OPT off")

# P11-C3-D: the freeze watchdog looks for the input thread at each look until it has run (dix main starts it as
# Dispatch begins, which can be after the watchdog's first look); tfreeze.c runs lorieInputTid
watch = func(I, "static void *lorieFreezeWatch(__unused void *arg) {")
in_order("the watchdog looks for the input thread until it has run", watch, "for (;;) {",
         "if (!tids[LORIE_THREAD_INPUT])\n            tids[LORIE_THREAD_INPUT] = lorieInputTid(&why);", "lorieFreezeSample(&s);")

# P11-C3-D2: TERMUX_X11_ROOT_COPY_SCHED_OPT read when the server state is made, "1" only, into the renderer's flag; the
# frame and the copies alone take the X server's lock through rendererVkLock (which tpresentcopy.c runs) - tries while
# a root copy submitted ahead is pending, then as always; the frame's root copy settled early once its image is
# acquired, before the lock - and settled, early, only through rendererVkAuxFinish (its fence waited for)
check('lorieScreen.state->backend.rootCopySchedOpt = getenv("TERMUX_X11_ROOT_COPY_SCHED_OPT") &&' in init and
      '!strcmp(getenv("TERMUX_X11_ROOT_COPY_SCHED_OPT"), "1");' in init, "OsVendorInit does not read TERMUX_X11_ROOT_COPY_SCHED_OPT=1")
vklock = func(R, "static bool rendererVkLock(void) {")
in_order("the frame's lock, a root copy ahead pending", vklock, "while (vkAux.pending && state->backend.rootCopySchedOpt) {",
         "if (lorie_mutex_trylock(&state->lock, &state->lockingPid))", "rendererVkAuxEarly(LORIE_ROOT_EARLY_LOCK);",
         "    lorie_mutex_lock(&state->lock, &state->lockingPid);\n    return true;")
for what, body in (("frame", redraw), ("copies alone", alone)):
    check(body.count("rendererVkLock()") == 1 and "lorie_mutex_lock(&state->lock" not in body,
          "the Vulkan %s takes the X server's lock other than through rendererVkLock" % what)
in_order("the frame's root copy settled early once its image is acquired", redraw, "lorieVkAcquire(",
         "rendererVkAuxEarly(LORIE_ROOT_EARLY_ACQUIRED);", "if (!vkActive)", "if (!rendererVkLock())")
early = func(R, "static void rendererVkAuxEarly(int at) {")
check("if (!vkAux.pending || !state->backend.rootCopySchedOpt)\n        return;" in early and "rendererVkAuxFinish();" in early and
      "rendererRootCopySettle(" not in early, "a root copy ahead settled early without the option, or without its fence waited for")

# P11-C3-C: the renderer's wake (lorie.h struct lorie_renderer_wake). The loop sleeps only in rendererWaitForWork (which
# twake.c runs), nothing in the renderer waits on a condition variable the X server would signal, and each waker
# publishes its work before it moves the count.
check("rendererWaitForWork(&waitingForBuffers);" in loop and "pthread_cond_wait(" not in loop and "rendererShouldWait(" not in loop,
      "the renderer's loop does not wait for work in rendererWaitForWork")
rsrc = open(R, encoding="utf-8").read()
check("stateCond" not in rsrc and "pthread_cond_timedwait(" not in rsrc and "pthread_cond_signal(stateCond" not in rsrc,
      "the renderer still waits on or signals the old condition variable")
check("rendererCond" not in open(I, encoding="utf-8").read(), "the X server still signals the old condition variable")
for what, body, publish in (
        ("tick", xredraw, "pvfb->state->rootWindowTextureID = LorieBuffer_description(rootDouble"),
        ("GPU copy queued", func(I, "static Bool lorieQueueGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,"),
         "__atomic_store_n(&pvfb->state->gpuCopyQueue.writeIndex, writeIndex + 1, __ATOMIC_RELEASE);"),
        ("root copy asked for", func(I, "static Bool lorieRootGpuCopyAsk(LorieBuffer *back, LorieBuffer *front, BoxPtr box, int n) {"),
         "__atomic_store_n(&st->rootCopy.seq, pvfb->rootCopySeq, __ATOMIC_RELEASE);"),
        ("ownership answered", func(I, "static void lorieProcessOwnershipRequest(void) {"), "lorieApplyOwnership(mode, LORIE_OWNERSHIP_SEQ(req));"),
        ("cursor moved", func(I, "static void lorieMoveCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, int x, int y) {"),
         "pvfb->state->cursor.moved = TRUE;"),
        ("reservation let go", func(I, "static void lorieCpuReleasedWake(void) {"), "FUTEX_WAKE"),
        ("buffer added", func(R, "void rendererAddBuffer(LorieBuffer* buf) {"), "pthread_spin_unlock(&bufferLock);"),
        ("window changed", func(R, "void rendererSetWindow(JNIEnv *env, __unused jobject thiz, jobject jsfc) {"), "windowChanged = TRUE;"),
        ("session changed", func(R, "__unused void rendererSetSharedState(struct lorie_shared_server_state* newState) {"), "stateChanged = true;")):
    in_order("published, then the renderer woken: " + what, body, publish, "lorieWakeRenderer(")
in_order("the wake region swapped under the input lock", func(I, "static Bool lorieSetRendererWakeupCondWorkProc(__unused ClientPtr client, void* closure) {"),
         "input_lock();", "rendererWake = newWake;", "input_unlock();", "lorieWakeRenderer(newWake);", "munmap(old")
in_order("copies left queued forgotten with the session", loop, "if (stateChanged) {", "deferredCopies = false;", "if (windowChanged)")
check("&& !rendererCopiesStillDeferred();\n    if (vkActive) {" in rpass, "the loop runs copies left queued for a reservation alone, again and again")

# P11-C3-B2: the scheduling's wiring
check('lorieScreen.state->backend.vkScheduleOpt = getenv("TERMUX_X11_RENDERER_VK_SCHEDULE_OPT") &&' in init and
      '!strcmp(getenv("TERMUX_X11_RENDERER_VK_SCHEDULE_OPT"), "1");' in init, "OsVendorInit does not read TERMUX_X11_RENDERER_VK_SCHEDULE_OPT=1")
in_order("root copy ahead of a frame only", rpass, "if (state && rendererRootCopyPending()) {",
         "else if (state->backend.vkScheduleOpt && rendererVkFrameWanted())", "rendererVkRootCopyAhead();", "rendererVkRootCopy();",
         "if (state && rendererVkFrameWanted())", "rendererVkRedraw(waitingForBuffers);", "if (vkAux.pending || vkAux.chained)",
         "rendererVkAuxSettle();")
in_order("the frame after a root copy ahead", redraw, "if (vkAux.pending && __atomic_load_n(&state->rootCopy.waiting, __ATOMIC_ACQUIRE) == vkAux.work.seq) {",
         "rendererVkAuxFinish();", "lorieVkAcquire(", "vkr.chainWait = vkAux.chained;", "lorieVkFrameQueue(", "vkAux.chained = false;",
         "lorie_mutex_unlock(&state->lock", "rendererVkAuxFinish();", "lorieVkFrameWait(")
for what, body in (("frame", redraw), ("copies alone", alone), ("root copy", func(R, "static bool rendererVkRootCopySubmit(struct rendererVkRootWork *w, bool ahead) {"))):
    check("lorieGpuWaitIdle(" not in body and "rendererVkWaitOthers();" in body,
          "the %s may wait for this process's own list (a root copy ahead)" % what)
in_order("teardown and the second submission", func(R, "static void rendererVkTeardown(void) {"), "rendererVkIdle();", "rendererVkFrameDone();",
         "rendererVkAuxDone();", "lorieVkRendererDestroy(&vk, &vkr);", "rendererVkAuxForget();")

selftest = func(os.path.join(L, "vk.c"), "bool lorieVkSelftest(struct lorie_vk *vk, volatile struct lorie_shared_server_state *st, uint32_t iterations) {")
check(selftest.count("st->diag.vkSelftestUs = lorieVkElapsedUs(&t0, &t1);") == 2 and "tv_nsec" not in selftest,
      "the self-test's duration not lorieVkElapsedUs's on both ways out")

print("ownership wiring (lorieRedraw, connect, disconnect, OsVendorInit), the urgent work and the renderer's Vulkan order: %s (%d failures)" % (
    "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
