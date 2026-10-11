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

redraw = func(I, "static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {")
check(before_first_return(redraw, "lorieProcessOwnershipRequest();"),
      "lorieRedraw does not take up the ownership request before its first return")

# Vulkan: the tick's frame allowed before the vblanks queue their Present copies, so they go in that frame
early = "if (lorieAhbPerAccess() && !twinned)\n        pvfb->state->waitForNextFrame = false;"
check(redraw.find(early) >= 0 and redraw.find(early) < redraw.find("loriePerformVblanks();"),
      "lorieRedraw does not allow a Vulkan frame before its vblanks queue the Present copies")

connected = func(I, "void lorieActivityConnected(void) {")
check("lorieResetOwnership();" in connected, "a new renderer connection does not reset the ownership")

events = func(C, "void handleLorieEvents(int fd, __unused int ready, __unused void *ignored) {")
err = events[events.find("if (ready & X_NOTIFY_ERROR) {"):]
check(events.find("if (ready & X_NOTIFY_ERROR) {") >= 0 and before_first_return(err, "lorieRendererDisconnected();"),
      "a lost renderer connection does not hand the ownership back")

init = func(I, "void OsVendorInit(void) {")
check('lorieScreen.state->backend.requested = lorieRequestedBackend(getenv("TERMUX_X11_RENDERER_BACKEND"),' in init,
      "OsVendorInit does not read TERMUX_X11_RENDERER_BACKEND into the server state")

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
in_order("root double buffering", func(I, "static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {"),
         "loriePerformVblanks();", "lorieRootSwap(priv)", "rootDouble ? priv->twin : priv->buffer")

redraw = func(R, "static void rendererVkRedraw(bool *waitingForBuffers) {")
# the swapchain is made again when out of date (vk.c); sizes come with a new surface, not from querying the window
check("ANativeWindow_get" not in redraw, "a Vulkan frame compares the window's size with the swapchain's")
in_order("Vulkan frame", redraw, "lorieVkAcquire(", "lorie_mutex_lock(&state->lock", "rendererVkOwnedLocked()",
         "state->rootWindowTextureID != desc->id", "rendererVkCopiesLocked(", "lorieVkFrameQueue(", "rendererVkInFlight();", "lorie_mutex_unlock(&state->lock",
         "lorieVkFrameWait(", "rendererVkFrameDone();", "completedSerial", "lorieVkPresent(")
alone = func(R, "static void rendererVkApplyPendingGpuCopies(void) {")
in_order("Vulkan copies alone", alone, "lorie_mutex_lock(&state->lock", "rendererVkOwnedLocked()", "rendererVkCopiesLocked(",
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
# another session: the last one's imported images and ownership answer are dropped before anything else runs
in_order("session change", loop, "if (stateChanged) {", "state = pendingState;", "vk.DeviceWaitIdle(vk.device);",
         "lorieVkImagesClear(&vk, &vkImages);", "vkOwned = false;", "if (windowChanged)")
in_order("Vulkan teardown", func(R, "static void rendererVkTeardown(void) {"), "rendererVkIdle();", "rendererVkFrameDone();",
         "lorieVkImagesClear(&vk, &vkImages);", "lorieVkDestroyDevice(&vk);")

check("            if (rendererGlesEager())\n                rendererAttachToGL(buf);" in loop,
      "a buffer that comes gets a GLES texture whoever draws")
check("rendererAttachToGL" not in func(R, "static LorieBuffer *rendererFindBufferWithRetry(uint64_t id) {"),
      "a buffer found for a copy gets a GLES texture whoever copies")
in_order("GLES frame", func(R, "void rendererRedrawLocked(bool* waitingForBuffers) {"), "rendererGlBuffer(buffer);",
         "LorieBuffer_bindTexture(buffer);")

selftest = func(os.path.join(L, "vk.c"), "bool lorieVkSelftest(struct lorie_vk *vk, volatile struct lorie_shared_server_state *st, uint32_t iterations) {")
check(selftest.count("st->diag.vkSelftestUs = lorieVkElapsedUs(&t0, &t1);") == 2 and "tv_nsec" not in selftest,
      "the self-test's duration not lorieVkElapsedUs's on both ways out")

print("ownership wiring (lorieRedraw, connect, disconnect, OsVendorInit) and the renderer's Vulkan order: %s (%d failures)" % (
    "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
