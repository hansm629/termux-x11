/* TOUTPUT: which output path the renderer takes - rootZeroCopyUsable, the real code (gen.py "output"), over
 * every combination of what it looks at: the backend asked for (auto, gpu-copy, root-direct), the root
 * layer there or not, the root one of our slots or a client's, the buffer BGRA or not (or none), a
 * viewport or not, slots stuck or not, linear or nearest filtering.
 *
 * In a build with LORIE_ROOT_DIRECT_ALLOWED 0 (the comparison build) ROOT_DIRECT is never taken, whatever
 * is asked for, and what is published for the X server says GPU_COPY - with the comparison as the reason
 * where nothing else would have kept it off. Anywhere else, the path is taken when everything allows it
 * and not when gpu-copy is asked for: which is also what shows this test can tell the two apart. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM 5
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
typedef struct { int format; } LorieBuffer_Desc;
typedef struct ASurfaceControl { int id; } ASurfaceControl;
struct lorie_shared_server_state {
    volatile uint8_t outputBackend, rootDoubleBuffered, outputFilterNearest, rootBackpressure, rootCommitTracked;
    volatile uint8_t outputBackendActive;
    volatile char outputBackendReason[96];
};
static struct lorie_shared_server_state fakeState, *state = &fakeState;
static ASurfaceControl layer, *rootSurfaceControl;
static int viewportW, viewportH, rootZcUnusableCount, filtering;
static bool rootZcBackpressureOn;
static struct { void *txSetOnCommit; } scApi;
#define log(...) ((void) 0)
#include "output_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 5) { printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main(void) {
    static const uint8_t asked[] = { LORIE_OUTPUT_AUTO, LORIE_OUTPUT_GPU_COPY, LORIE_OUTPUT_ROOT_DIRECT };
    LorieBuffer_Desc bgra = { AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM }, rgba = { AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM };
    const LorieBuffer_Desc *descs[] = { &bgra, &rgba, NULL };
    int combos = 0, taken = 0;
#if defined(LORIE_ROOT_DIRECT_ALLOWED) && !LORIE_ROOT_DIRECT_ALLOWED
    const bool comparison = true;
#else
    const bool comparison = false;
#endif

    for (int a = 0; a < 3; a++)
    for (int l = 0; l < 2; l++)
    for (int d = 0; d < 2; d++)
    for (int f = 0; f < 3; f++)
    for (int v = 0; v < 2; v++)
    for (int u = 0; u < 2; u++)
    for (int n = 0; n < 2; n++) {
        /* only the inputs: what was published stays as the function left it, as in the shared state, since it
         * publishes again only when its answer changes */
        fakeState.outputBackend = asked[a];
        rootSurfaceControl = l ? &layer : NULL;
        fakeState.rootDoubleBuffered = (uint8_t) d;
        viewportW = viewportH = v ? 1920 : 0;
        rootZcUnusableCount = u ? 3 : 0;
        filtering = n ? GL_NEAREST : GL_LINEAR;
        bool everythingAllows = l && d && f == 0 && v && !u && (n == 0 || asked[a] == LORIE_OUTPUT_ROOT_DIRECT) &&
                                asked[a] != LORIE_OUTPUT_GPU_COPY;
        bool usable = rootZeroCopyUsable(descs[f]);
        combos++;
        taken += usable;
        CHECK((fakeState.outputBackendActive == LORIE_OUTPUT_ROOT_DIRECT) == usable,
              "asked %d: returned %d but published %d", asked[a], usable, fakeState.outputBackendActive);
        if (comparison) {
            CHECK(!usable, "comparison build: ROOT_DIRECT taken (asked %d, layer %d, ours %d, format %d, viewport %d, "
                  "stuck %d, nearest %d)", asked[a], l, d, f, v, u, n);
            if (everythingAllows || asked[a] == LORIE_OUTPUT_GPU_COPY)
                CHECK(strstr((const char *) fakeState.outputBackendReason, "comparison") != NULL,
                      "comparison build: the reason given is \"%s\"", (const char *) fakeState.outputBackendReason);
        } else
            CHECK(usable == everythingAllows, "asked %d, layer %d, ours %d, format %d, viewport %d, stuck %d, nearest %d: "
                  "ROOT_DIRECT %s", asked[a], l, d, f, v, u, n, usable ? "taken" : "not taken");
    }
    printf("TOUTPUT output path, %s: %s (%d failures; %d combinations, ROOT_DIRECT taken in %d)\n",
           comparison ? "comparison build, ROOT_DIRECT never taken" : "ROOT_DIRECT taken exactly when everything allows it",
           fails ? "FAIL" : "PASS", fails, combos, taken);
    return fails != 0;
}
