package com.termux.x11.input;

/**
 * Follows the finger of a one-finger drag while a mouse button is held down in the X server, so that the
 * cursor (and the window or selection being dragged) moves with every real change of the finger's
 * position. GestureDetector only reports motion through onScroll once the finger has left the touch slop
 * around where it went down, and then only after the focus moved a whole pixel; a drag started by a long
 * press would otherwise sit still and then jump.
 *
 * It also records what onScroll delivered, so that motion given to the X server by one path is never sent
 * again, or skipped, by the other. Positions are in view pixels; distances have GestureDetector's sign,
 * last position minus current one.
 */
final class HeldDragMotion {
    /** Smaller position changes than this are no movement. */
    static final float EPSILON = 0.001f;

    private float mLastX, mLastY;
    private int mPointerId = -1;
    private boolean mAnchored;

    /** The distance of the last {@link #move} that returned true. */
    float distanceX, distanceY;

    /** The motion of finger {@code pointerId} up to ({@code x}, {@code y}) has been given to the X server. */
    void delivered(int pointerId, float x, float y) {
        mPointerId = pointerId;
        mLastX = x;
        mLastY = y;
        mAnchored = true;
    }

    /**
     * A finger went down or up, or the gesture ended: the next motion starts from wherever the remaining
     * finger is then (GestureDetector, too, measures from the new focus after a pointer change).
     */
    void reset() {
        mAnchored = false;
    }

    /**
     * For an ACTION_MOVE of a held-button drag with finger {@code pointerId} at ({@code x}, {@code y}):
     * whether the cursor is to be moved, by {@link #distanceX}, {@link #distanceY}. Not when the finger
     * did not move, nor for the first position of a finger it was not following yet.
     */
    boolean move(int pointerId, float x, float y) {
        if (!mAnchored || pointerId != mPointerId) {
            delivered(pointerId, x, y);
            return false;
        }

        float dx = mLastX - x, dy = mLastY - y;
        if (Math.abs(dx) < EPSILON && Math.abs(dy) < EPSILON)
            return false;

        distanceX = dx;
        distanceY = dy;
        delivered(pointerId, x, y);
        return true;
    }
}
