/* T10: the copy record reserve. A record is taken before a copy is enqueued; with none left the copy must
 * not be offered to the GPU at all (the caller falls back to the CPU), and a record given back is fully
 * reset before it is reused, so nothing from the previous job survives into the next. LorieAbandonedCopy
 * and the take/give-back functions are extracted from InitOutput.c by gen.py. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
typedef uint32_t CARD32;
struct xorg_list { struct xorg_list *next, *prev; };
typedef struct _LorieBuffer LorieBuffer;
typedef struct _Pixmap *PixmapPtr;
typedef struct _Window *WindowPtr;
struct present_fence;
/* giving a record back also lets its connection's record count one copy fewer (T31's subject) */
static void lorieSessionCopyEnded(uint32_t id) { (void) id; }
#include "t10_src.inc"
int main(void) {
    int fails = 0;
    LorieAbandonedCopy *taken[LORIE_COPY_RECORDS];
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
    for (int i = 0; i < LORIE_COPY_RECORDS; i++) {
        taken[i] = lorieTakeCopyRecord();
        CHECK(taken[i] != NULL, "record %d of %d not available", i, LORIE_COPY_RECORDS);
        for (int j = 0; j < i; j++) CHECK(taken[i] != taken[j], "record %d handed out twice", i);
        taken[i]->serial = 1000 + i; taken[i]->session = 7; taken[i]->src = (LorieBuffer *) 1;
    }
    CHECK(lorieTakeCopyRecord() == NULL, "a record handed out with the reserve exhausted - the copy would be untracked");
    lorieGiveBackCopyRecord(taken[5]);
    LorieAbandonedCopy *again = lorieTakeCopyRecord();
    CHECK(again == taken[5], "freed record not reused");
    CHECK(again && again->serial == 0 && again->session == 0 && again->src == NULL && again->inUse,
          "reused record still carries the previous job's state");
    CHECK(lorieTakeCopyRecord() == NULL, "reserve should be exhausted again");
    printf("T10 copy record reserve: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
