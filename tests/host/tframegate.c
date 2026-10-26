#include <stdio.h>
#include <stdbool.h>
#include "../../app/src/main/cpp/lorie/frame_gate_policy.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Model both interleavings around Present's vblank callback:
 * - the GPU renderer may or may not consume the pre-vblank frame release
 * - the X server must still allow copy-only work even when the frame is gated
 * (the latter invariant is checked against rendererPass by twiring.py). */
int main(void) {
    for (int selected = 0; selected <= 1; selected++) {
        for (int perAccessSingleRoot = 0; perAccessSingleRoot <= 1; perAccessSingleRoot++) {
            for (int rendererConsumedEarly = 0; rendererConsumedEarly <= 1; rendererConsumedEarly++) {
                bool waitForNextFrame = true;
                unsigned visibleFramesAllowed = 0;
                if (perAccessSingleRoot)
                    waitForNextFrame = false;
                if (!waitForNextFrame && rendererConsumedEarly) {
                    visibleFramesAllowed++;
                    waitForNextFrame = true; /* rendererVkRedraw sets it after submit */
                }
                if (lorieFrameGateRearmAfterVblanks(selected, perAccessSingleRoot))
                    waitForNextFrame = false;
                if (!waitForNextFrame) {
                    visibleFramesAllowed++;
                    waitForNextFrame = true;
                }
                CHECK(waitForNextFrame);
                if (selected && perAccessSingleRoot)
                    CHECK(visibleFramesAllowed == 1);
                else if (perAccessSingleRoot && rendererConsumedEarly)
                    CHECK(visibleFramesAllowed == 2); /* legacy duplicates */
                else
                    CHECK(visibleFramesAllowed == 1);
            }
        }
    }
    printf("TFRAMEGATE Vulkan PER_ACCESS one-screen-frame-per-tick + fallback: %s (%d failures)\n",
           failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
