#!/usr/bin/env python3
"""Checks that the X server calls the AHB ownership code where the handshake needs it (townership.c tests
the code itself): every frame's lorieRedraw takes up the renderer's request before anything can return
early; a new renderer connection and a lost one both go back to 3c98ffa's ownership; the requested backend
is read from TERMUX_X11_RENDERER_BACKEND when the server state is made.

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

connected = func(I, "void lorieActivityConnected(void) {")
check("lorieResetOwnership();" in connected, "a new renderer connection does not reset the ownership")

events = func(C, "void handleLorieEvents(int fd, __unused int ready, __unused void *ignored) {")
err = events[events.find("if (ready & X_NOTIFY_ERROR) {"):]
check(events.find("if (ready & X_NOTIFY_ERROR) {") >= 0 and before_first_return(err, "lorieRendererDisconnected();"),
      "a lost renderer connection does not hand the ownership back")

init = func(I, "void OsVendorInit(void) {")
check('lorieScreen.state->backend.requested = lorieRequestedBackend(getenv("TERMUX_X11_RENDERER_BACKEND"),' in init,
      "OsVendorInit does not read TERMUX_X11_RENDERER_BACKEND into the server state")

print("ownership wiring (lorieRedraw, connect, disconnect, OsVendorInit): %s (%d failures)" % (
    "FAIL" if fails else "PASS", fails))
sys.exit(1 if fails else 0)
