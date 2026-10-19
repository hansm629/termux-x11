/* TBUFFERLOCK (P11-B): buffer.c's CPU locks as they are (gen.py "bufferlock"), against a fake AHardwareBuffer_lock
 * that records what it is asked. LorieBuffer_lockRect asks for the one access given - CPU_READ_OFTEN alone, or
 * CPU_WRITE_OFTEN alone - with the rect given (NULL passed on as NULL), hands back the address the lock gave, and marks
 * the buffer locked only when that lock succeeded: refused, nothing is left to unlock; a lock that "succeeded" without
 * an address is unlocked again and refused. A buffer already locked is refused without asking for another lock. A
 * shared-memory buffer needs no lock. LorieBuffer_lock (READ|WRITE, the whole buffer, marked locked even when refused)
 * stays as it was; and each lock that succeeded is unlocked once. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define __LIBC_HIDDEN__ static
#define __unused __attribute__((unused))
#define _Nullable
#define _Nonnull
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct { int32_t left, top, right, bottom; } ARect;
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN 3ULL
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN (3ULL << 4)
typedef unsigned int GLuint;
typedef void *EGLImage;
struct xorg_list { struct xorg_list *next, *prev; };

/* the AHB's own CPU lock */
static struct { int locks, unlocks, held, fail, noAddress; uint64_t usage; bool rectGiven; ARect rect; } ahb;
static uint32_t pixels[64];
static int AHardwareBuffer_lock(AHardwareBuffer *b, uint64_t usage, int32_t fence, const ARect *rect, void **out) {
    (void) b; (void) fence;
    ahb.locks++;
    ahb.usage = usage;
    ahb.rectGiven = rect != NULL;
    if (rect)
        ahb.rect = *rect;
    if (ahb.fail)
        return -EINVAL;
    ahb.held++;
    *out = ahb.noAddress ? NULL : pixels;
    return 0;
}
static int AHardwareBuffer_unlock(AHardwareBuffer *b, int32_t *fence) {
    (void) b; (void) fence;
    ahb.unlocks++;
    if (!ahb.held)
        return -EINVAL;
    ahb.held--;
    return 0;
}

#include "bufferlock_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    LorieBuffer b = { .desc = { .width = 8, .height = 8, .stride = 8, .type = LORIEBUFFER_AHARDWAREBUFFER, .buffer = (AHardwareBuffer *) 1 } };
    ARect r = { 1, 2, 5, 7 };
    void *p = NULL;
    int ret;

    /* for reading, within a rect */
    ret = LorieBuffer_lockRect(&b, false, &r, &p);
    CHECK(!ret && p == pixels && b.locked && ahb.usage == AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN && ahb.rectGiven &&
          !memcmp(&ahb.rect, &r, sizeof r), "read lock: ret %d, usage %llx, rect %s", ret, (unsigned long long) ahb.usage,
          ahb.rectGiven ? "given" : "not given");
    CHECK(LorieBuffer_lockRect(&b, true, &r, &p) == EEXIST && ahb.locks == 1, "locked twice: asked the AHB again");
    CHECK(!LorieBuffer_unlock(&b) && !b.locked && ahb.held == 0, "read lock: not unlocked");

    /* for writing, the whole buffer */
    ret = LorieBuffer_lockRect(&b, true, NULL, &p);
    CHECK(!ret && ahb.usage == AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN && !ahb.rectGiven, "write lock: ret %d, usage %llx, rect %s", ret,
          (unsigned long long) ahb.usage, ahb.rectGiven ? "given" : "NULL");
    LorieBuffer_unlock(&b);

    /* refused: not marked locked, nothing to unlock - and LorieBuffer_unlock then asks the AHB for nothing */
    ahb.fail = 1;
    p = NULL;
    ret = LorieBuffer_lockRect(&b, false, &r, &p);
    CHECK(ret && !b.locked && !p, "refused: ret %d, locked %d", ret, b.locked);
    {
        int unlocks = ahb.unlocks;
        CHECK(LorieBuffer_unlock(&b) == ENOENT && ahb.unlocks == unlocks, "refused, then unlocked: the AHB asked to unlock");
    }
    ahb.fail = 0;

    /* locked without an address: unlocked again, refused */
    ahb.noAddress = 1;
    ret = LorieBuffer_lockRect(&b, true, &r, &p);
    CHECK(ret && !b.locked && ahb.held == 0, "no address: ret %d, locked %d, AHB still locked %d", ret, b.locked, ahb.held);
    ahb.noAddress = 0;

    /* shared memory: its own address, no AHB lock */
    {
        LorieBuffer m = { .desc = { .width = 8, .height = 8, .stride = 8, .type = LORIEBUFFER_FD, .data = pixels + 1 } };
        int locks = ahb.locks;
        ret = LorieBuffer_lockRect(&m, true, &r, &p);
        CHECK(!ret && p == pixels + 1 && m.locked && ahb.locks == locks, "shared memory: ret %d", ret);
        LorieBuffer_unlock(&m);
    }

    /* LorieBuffer_lock as it was: READ|WRITE, the whole buffer, locked even when refused */
    ret = LorieBuffer_lock(&b, &p);
    CHECK(!ret && ahb.usage == (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN) && !ahb.rectGiven,
          "LorieBuffer_lock: usage %llx", (unsigned long long) ahb.usage);
    LorieBuffer_unlock(&b);
    ahb.fail = 1;
    LorieBuffer_lock(&b, &p);
    CHECK(b.locked, "LorieBuffer_lock refused: not marked locked any more");
    LorieBuffer_unlock(&b);
    ahb.fail = 0;

    CHECK(ahb.held == 0, "the AHB left locked %d times", ahb.held);
    printf("TBUFFERLOCK buffer.c's CPU locks (LorieBuffer_lockRect, LorieBuffer_lock): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
