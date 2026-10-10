package com.termux.x11.input;

import java.util.ArrayList;
import java.util.List;

/**
 * Plain JVM test of the held-button drag motion (HeldDragMotion, as TouchInputHandler wires it), run by
 * tests/input/run.sh. It drives a model of one finger on the trackpad strategy: GestureDetector's
 * ACTION_MOVE rules as in AOSP (nothing until the focus leaves the touch slop around the down point, then
 * onScroll whenever it moved a whole pixel; every move while double tapping goes to onDoubleTapEvent,
 * which TouchInputHandler turns into onScroll), and TouchInputHandler's handleTouchEvent / onScroll /
 * moveHeldDrag. Each scenario runs with and without the held-drag path, so the test also shows what
 * GestureDetector alone did.
 */
public class HeldDragMotionTest {
    static int failures;

    static void check(boolean ok, String what) {
        if (!ok) {
            System.out.println("  FAIL " + what);
            failures++;
        }
    }

    static final int DOWN = 0, UP = 1, MOVE = 2, CANCEL = 3, POINTER_DOWN = 5, POINTER_UP = 6;
    static final float TOUCH_SLOP = 24; // 8 dp at 3x

    /** A finger event: action, pointer count, and the first pointer's id and position. */
    static final class Ev {
        final int action, pointers, id;
        final float x, y;
        Ev(int action, int pointers, int id, float x, float y) {
            this.action = action; this.pointers = pointers; this.id = id; this.x = x; this.y = y;
        }
    }

    static final class Model {
        final boolean heldDragPath;
        final HeldDragMotion heldDrag = new HeldDragMotion();
        boolean heldDragOwnsMotion, buttonHeld, suppressCursorMovement;
        // GestureDetector
        float downX, downY, lastFocusX, lastFocusY;
        boolean alwaysInTapRegion, doubleTapping;
        // TouchInputHandler.GestureListener.onDoubleTapEvent
        float dtLastX, dtLastY;
        /** Relative moves sent to the X server, and how many were sent for each event. */
        final List<float[]> sent = new ArrayList<>();
        final List<Integer> sentPerEvent = new ArrayList<>();

        Model(boolean heldDragPath) { this.heldDragPath = heldDragPath; }

        void moveCursorByOffset(float dx, float dy) { sent.add(new float[] {-dx, -dy}); }

        /** TouchInputHandler.moveHeldDrag. */
        boolean moveHeldDrag(Ev e) {
            switch (e.action) {
                case DOWN: heldDrag.delivered(e.id, e.x, e.y); return false;
                case POINTER_DOWN: case POINTER_UP: case UP: case CANCEL: heldDrag.reset(); return false;
                case MOVE: break;
                default: return false;
            }
            if (e.pointers != 1 || suppressCursorMovement || !buttonHeld)
                return false;
            if (heldDrag.move(e.id, e.x, e.y))
                moveCursorByOffset(heldDrag.distanceX, heldDrag.distanceY);
            return true;
        }

        /** TouchInputHandler.GestureListener.onScroll, single pointer, trackpad. */
        boolean onScroll(Ev e2, float dx, float dy) {
            if (e2.pointers != 1 || suppressCursorMovement)
                return false;
            if (heldDragOwnsMotion)
                return true;
            moveCursorByOffset(dx, dy);
            heldDrag.delivered(e2.id, e2.x, e2.y);
            return true;
        }

        /** GestureDetector.onTouchEvent, the parts that decide onScroll / onDoubleTapEvent. */
        void gestureDetector(Ev e) {
            switch (e.action) {
                case DOWN:
                    downX = lastFocusX = e.x; downY = lastFocusY = e.y;
                    alwaysInTapRegion = true;
                    if (doubleTapping) { dtLastX = e.x; dtLastY = e.y; }
                    break;
                case POINTER_DOWN: case POINTER_UP:
                    downX = lastFocusX = e.x; downY = lastFocusY = e.y;
                    break;
                case MOVE: {
                    float scrollX = lastFocusX - e.x, scrollY = lastFocusY - e.y;
                    if (doubleTapping) {
                        // onDoubleTapEvent(ACTION_MOVE) -> onScroll(null, e, mLastFocusX - e.getX(), ...)
                        if (e.pointers == 1) {
                            onScroll(e, dtLastX - e.x, dtLastY - e.y);
                            dtLastX = e.x; dtLastY = e.y;
                        }
                    } else if (alwaysInTapRegion) {
                        int deltaX = (int) (e.x - downX), deltaY = (int) (e.y - downY);
                        if (deltaX * deltaX + deltaY * deltaY > TOUCH_SLOP * TOUCH_SLOP) {
                            onScroll(e, scrollX, scrollY);
                            lastFocusX = e.x; lastFocusY = e.y;
                            alwaysInTapRegion = false;
                        }
                    } else if (Math.abs(scrollX) >= 1 || Math.abs(scrollY) >= 1) {
                        onScroll(e, scrollX, scrollY);
                        lastFocusX = e.x; lastFocusY = e.y;
                    }
                    break;
                }
                case UP: case CANCEL:
                    doubleTapping = false;
                    break;
            }
        }

        /** TouchInputHandler.handleTouchEvent, finger path, trackpad strategy. */
        void event(Ev e) {
            int before = sent.size();
            if ((e.action == UP || e.action == CANCEL) && buttonHeld)
                buttonHeld = false; // TrackpadInputStrategy.onMotionEvent
            heldDragOwnsMotion = heldDragPath && moveHeldDrag(e);
            gestureDetector(e);
            heldDragOwnsMotion = false;
            sentPerEvent.add(sent.size() - before);
        }

        float sumX() { float s = 0; for (float[] d : sent) s += d[0]; return s; }
        float sumY() { float s = 0; for (float[] d : sent) s += d[1]; return s; }
        int eventsSendingTwice() { int n = 0; for (int c : sentPerEvent) if (c > 1) n++; return n; }
    }

    static boolean near(float a, float b) { return Math.abs(a - b) < 0.01f; }

    /** Long press (button down), then the finger moves 2 px per event to the right, 30 times. */
    static Model longPressDrag(boolean path) {
        Model m = new Model(path);
        m.event(new Ev(DOWN, 1, 0, 100, 100));
        m.buttonHeld = true; // TapGestureDetector -> onLongPress -> onPressAndHold
        for (int i = 1; i <= 30; i++)
            m.event(new Ev(MOVE, 1, 0, 100 + 2 * i, 100));
        m.event(new Ev(UP, 1, 0, 160, 100));
        return m;
    }

    static void testLongPressDragFollowsFinger() {
        System.out.println("long press drag: every move of the finger moves the cursor, from the first one");
        Model before = longPressDrag(false), after = longPressDrag(true);
        int firstSendBefore = before.sentPerEvent.indexOf(1);
        System.out.println("    GestureDetector alone: first move sent with event " + firstSendBefore
                + " (" + before.sent.get(0)[0] + " px at once)");
        check(firstSendBefore == 13 && near(before.sent.get(0)[0], 26), "model: GestureDetector holds the slop back");
        check(after.sent.size() == 30, "one send per move, got " + after.sent.size());
        for (float[] d : after.sent)
            check(near(d[0], 2) && near(d[1], 0), "each send is the finger's own 2 px");
        check(near(after.sumX(), 60) && near(before.sumX(), 60), "same total: nothing lost, nothing added");
        check(after.eventsSendingTwice() == 0, "never two sends for one event");
    }

    static void testSlowDragIsNotLost() {
        System.out.println("slow drag: motion below the slop and below a pixel per event still moves the cursor");
        Model before = new Model(false), after = new Model(true);
        for (Model m : new Model[] {before, after}) {
            m.event(new Ev(DOWN, 1, 0, 50, 50));
            m.buttonHeld = true;
            for (int i = 1; i <= 40; i++)
                m.event(new Ev(MOVE, 1, 0, 50 + 0.25f * i, 50));
            m.event(new Ev(UP, 1, 0, 60, 50));
        }
        check(before.sent.isEmpty(), "model: GestureDetector sends nothing for a 10 px drag");
        check(after.sent.size() == 40 && near(after.sumX(), 10), "all 10 px sent, in 40 moves");
    }

    static void testStillFingerSendsNothing() {
        System.out.println("still finger: moves that do not change the position send nothing");
        Model m = new Model(true);
        m.event(new Ev(DOWN, 1, 0, 10, 10));
        m.buttonHeld = true;
        for (int i = 0; i < 20; i++)
            m.event(new Ev(MOVE, 1, 0, 10, 10));
        m.event(new Ev(MOVE, 1, 0, 13, 10));
        m.event(new Ev(MOVE, 1, 0, 13, 10));
        check(m.sent.size() == 1 && near(m.sumX(), 3), "only the real 3 px change, got " + m.sent.size());
    }

    static void testNoButtonUnchanged() {
        System.out.println("no button held: the cursor moves exactly as GestureDetector says, as before");
        Model before = new Model(false), after = new Model(true);
        for (Model m : new Model[] {before, after}) {
            m.event(new Ev(DOWN, 1, 0, 0, 0));
            for (int i = 1; i <= 50; i++)
                m.event(new Ev(MOVE, 1, 0, 1.5f * i, 0.5f * i));
            m.event(new Ev(UP, 1, 0, 75, 25));
        }
        check(before.sent.size() == after.sent.size(), "same number of sends");
        for (int i = 0; i < before.sent.size(); i++)
            check(near(before.sent.get(i)[0], after.sent.get(i)[0]) && near(before.sent.get(i)[1], after.sent.get(i)[1]),
                  "same send " + i);
    }

    static void testDoubleTapDragNoDuplicates() {
        System.out.println("double tap drag: onDoubleTapEvent's moves and the held-drag path never both send");
        Model m = new Model(true);
        m.doubleTapping = true; // second tap of a double tap, tap-to-move: onPressAndHold(force) at its DOWN
        m.event(new Ev(DOWN, 1, 0, 200, 200));
        m.buttonHeld = true;
        for (int i = 1; i <= 25; i++)
            m.event(new Ev(MOVE, 1, 0, 200 - 3 * i, 200 + i));
        m.event(new Ev(UP, 1, 0, 125, 225));
        check(m.sent.size() == 25 && m.eventsSendingTwice() == 0, "one send per move, got " + m.sent.size());
        check(near(m.sumX(), -75) && near(m.sumY(), 25), "total equals the finger's displacement");
    }

    static void testSecondFingerNoJump() {
        System.out.println("second finger: no single-finger motion while two are down, no jump when one lifts");
        Model m = new Model(true);
        m.event(new Ev(DOWN, 1, 0, 100, 100));
        m.buttonHeld = true;
        m.event(new Ev(MOVE, 1, 0, 110, 100));
        m.event(new Ev(POINTER_DOWN, 2, 0, 110, 100));
        m.event(new Ev(MOVE, 2, 0, 120, 100));
        m.event(new Ev(POINTER_UP, 2, 1, 400, 300));       // finger 0 lifts, finger 1 (far away) stays
        m.event(new Ev(MOVE, 1, 1, 400, 300));
        m.event(new Ev(MOVE, 1, 1, 405, 300));
        check(m.sent.size() == 2, "the 10 px before and the 5 px after, got " + m.sent.size());
        check(near(m.sumX(), 15), "no jump to the other finger, total " + m.sumX());
    }

    static void testCancelReleases() {
        System.out.println("cancel: the drag ends, the next gesture is a plain one again");
        Model m = new Model(true);
        m.event(new Ev(DOWN, 1, 0, 0, 0));
        m.buttonHeld = true;
        m.event(new Ev(MOVE, 1, 0, 5, 0));
        m.event(new Ev(CANCEL, 1, 0, 5, 0));
        check(!m.buttonHeld, "the button is released on cancel");
        m.event(new Ev(DOWN, 1, 0, 50, 50));
        m.event(new Ev(MOVE, 1, 0, 52, 50));                 // within the slop again, no button: nothing
        check(m.sent.size() == 1, "only the held drag's move was sent, got " + m.sent.size());
    }

    static void testSuppressedSendsNothing() {
        System.out.println("suppressed cursor movement (keyboard, swipe): no held-drag motion either");
        Model m = new Model(true);
        m.event(new Ev(DOWN, 1, 0, 0, 0));
        m.buttonHeld = true;
        m.suppressCursorMovement = true;
        for (int i = 1; i <= 20; i++)
            m.event(new Ev(MOVE, 1, 0, 5 * i, 0));
        check(m.sent.isEmpty(), "nothing sent");
    }

    static void testTrackerDirectly() {
        System.out.println("HeldDragMotion: anchors, distances, no-move and finger change");
        HeldDragMotion t = new HeldDragMotion();
        check(!t.move(0, 10, 10), "first position only anchors");
        check(t.move(0, 13, 6) && near(t.distanceX, -3) && near(t.distanceY, 4), "distance is last - current");
        check(!t.move(0, 13, 6), "same position: no move");
        check(!t.move(0, 13.0005f, 6), "below EPSILON: no move");
        check(!t.move(1, 50, 50), "another finger re-anchors");
        check(t.move(1, 51, 50) && near(t.distanceX, -1), "then follows it");
        t.reset();
        check(!t.move(1, 80, 80), "after reset the next position anchors");
    }

    public static void main(String[] args) {
        testTrackerDirectly();
        testLongPressDragFollowsFinger();
        testSlowDragIsNotLost();
        testStillFingerSendsNothing();
        testNoButtonUnchanged();
        testDoubleTapDragNoDuplicates();
        testSecondFingerNoJump();
        testCancelReleases();
        testSuppressedSendsNothing();
        System.out.println(failures == 0 ? "held drag: PASS" : "held drag: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
