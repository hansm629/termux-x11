/* Renderer session lifetime: when what a copy holds may be let go of after the connection that owed its
 * report has gone. The X server's session bookkeeping (lorieCopyResolve and around it) and the renderer's
 * rendererSayRetired are extracted verbatim by gen.py. Simulated: the queue's indexes, the renderer's
 * draining and reports, and the process table that pidfd / kill(pid, 0) look at.
 *
 * Builds against code from before the session table as well (no HAVE_SESSIONS), driving it through the
 * same calls - which is how the cases are shown to fail there. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <poll.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define min(a, b) (((a) < (b)) ? (a) : (b))

struct xorg_list { struct xorg_list *next, *prev; };
#define xorg_list_for_each_entry(pos, head, member) \
    for (pos = (typeof(pos)) ((char *) (head)->next - offsetof(typeof(*pos), member)); \
         &pos->member != (head); \
         pos = (typeof(pos)) ((char *) pos->member.next - offsetof(typeof(*pos), member)))
static void xorg_list_add(struct xorg_list *e, struct xorg_list *head) {
    e->next = head->next; e->prev = head; head->next->prev = e; head->next = e;
}
typedef struct { struct xorg_list link; uint32_t session; uint64_t serial; uint64_t settleByUs; } LorieAbandonedCopy;
static struct xorg_list lorieAbandonedCopies = { &lorieAbandonedCopies, &lorieAbandonedCopies };

#define CAP 8
struct lorie_shared_server_state {
    struct {
        volatile uint64_t completedSerial;
        volatile uint64_t failedSerials[16];
        volatile uint32_t failedCount;
        volatile uint64_t failedLostUpTo;
        volatile uint32_t readIndex, writeIndex;
        struct { uint64_t serial; } entries[CAP];
        volatile uint32_t entryState[CAP];
    } gpuCopyQueue;
    volatile uint32_t sessionTag;
    struct { volatile uint32_t seq, session; volatile int32_t pid; volatile uint64_t serial; } retired[4];
    volatile uint32_t retiredClaim;
    struct { uint32_t copySettledRetired, copySettledProcessDead, copyForcedSettle; } presentStats;
};
static struct lorie_shared_server_state shared;
static struct { struct lorie_shared_server_state *state; uint64_t gpuCopySerialCounter; } fakePvfb = { &shared, 0 };
#define pvfb (&fakePvfb)

static uint64_t fakeNow = 1000000;
static uint64_t lorieNowUs(void) { return fakeNow; }
#define log(prio, ...) ((void) 0)
static void lorieReapAbandonedCopies(void) {}
static Bool lorieCancelQueuedEntry(uint32_t slot) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&shared.gpuCopyQueue.entryState[slot], &expected, 2u, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static void lorieRootCopyCancelled(uint64_t serial) { (void) serial; }

/* The process table. pidfd: readable once the process has exited. kill(pid, 0): the renderer runs
 * under another uid, so a live process answers EPERM and a gone one ESRCH. */
static uint8_t alive[512];
static uint32_t tagOf[512];          /* each renderer process's own rendererSessionTag */
static int pidfdAllowed = 1;
static int fakeRendererPid;
#define PROP_VALUE_MAX 92
#define __system_property_get(name, value) (strcpy(value, pidfdAllowed ? "35" : "29"), 2)
#define syscall(nr, pid, flags) (pidfdAllowed ? 1000 + (int) (pid) : -1)
#define poll(p, n, t) (alive[(p)->fd - 1000] ? 0 : 1)
#define kill(pid, sig) (errno = alive[pid] ? EPERM : ESRCH, -1)
#define close(fd) ((void) 0)
#define getpid() fakeRendererPid
#include "session_src.inc"
#undef poll
#undef kill
#undef close

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void) {
    LorieAbandonedCopy *c = NULL;
    (void) c;
    memset(&shared, 0, sizeof shared);
    lorieAbandonedCopies.next = lorieAbandonedCopies.prev = &lorieAbandonedCopies;
    fakePvfb.gpuCopySerialCounter = 0;
    lorieRendererSession = 0;
#ifdef HAVE_SESSIONS
    memset(lorieSessions, 0, sizeof lorieSessions);
    lorieRetiredSeen = 0;
#endif
    memset(alive, 0, sizeof alive);
    memset(tagOf, 0, sizeof tagOf);
    pidfdAllowed = 1;
}
/* a renderer process connects, and takes the state up */
static void connectRenderer(int pid) {
    alive[pid] = 1;
#ifdef HAVE_SESSIONS
    lorieNoteRendererConnected(pid);
    tagOf[pid] = shared.sessionTag;
#else
    lorieNoteRendererConnected();
#endif
}
/* the X server queues a copy; the renderer takes some from the queue; it reports a watermark */
static uint64_t enqueue(void) {
    uint64_t serial = ++fakePvfb.gpuCopySerialCounter;
    uint32_t i = shared.gpuCopyQueue.writeIndex;
    shared.gpuCopyQueue.entries[i % CAP].serial = serial;
    shared.gpuCopyQueue.entryState[i % CAP] = 0;
    shared.gpuCopyQueue.writeIndex = i + 1;
    return serial;
}
static void take(int n) { shared.gpuCopyQueue.readIndex += n; }
static void report(uint64_t serial) { shared.gpuCopyQueue.completedSerial = serial; }
static void reportFailed(uint64_t serial) {
    shared.gpuCopyQueue.failedSerials[shared.gpuCopyQueue.failedCount % 16] = serial;
    shared.gpuCopyQueue.failedCount++;
}
/* the renderer letting go of the state, its fences waited out (rendererSetSharedState) */
static void retire(int pid) {
#ifdef HAVE_SESSIONS
    fakeRendererPid = pid;
    rendererSessionTag = tagOf[pid];
    rendererSayRetired(&shared);
#else
    (void) pid;
#endif
}
/* what a cancelled present leaves behind while its copy may still run */
static LorieAbandonedCopy *hold(uint64_t serial) {
    LorieAbandonedCopy *c = calloc(1, sizeof *c);
    c->serial = serial;
    c->session = lorieRendererSession;
    xorg_list_add(&c->link, &lorieAbandonedCopies);
    return c;
}
static uint32_t forced(void) { return shared.presentStats.copyForcedSettle; }

int main(void) {
    LorieAbandonedCopy *c1, *c2;
    uint64_t s1, s2;
    uint32_t before;

    /* 1. The socket goes, the process lives on and has not said it is done. Held however long it takes:
     *    time passing says nothing about its GPU work. */
    reset();
    connectRenderer(100);
    s1 = enqueue(); take(1);
    c1 = hold(s1);
    lorieNoteRendererLost();
    fakeNow += 10 * 1000 * 1000;
    CHECK(!lorieCopySettled(c1), "socket lost, process alive: let go of after a wait, with the process still able to read it");
    CHECK(!lorieGpuCopyResolved(s1), "socket lost, process alive: resolved");

    /* 2. The socket goes and the process is gone. Settled at once, and not as recovery. What it had taken
     *    and never reported was not made; what was still queued is cancelled so no later renderer runs it. */
    reset();
    connectRenderer(101);
    s1 = enqueue(); take(1);
    s2 = enqueue();
    c1 = hold(s1); c2 = hold(s2);
    lorieNoteRendererLost();
    alive[101] = 0;
    before = forced();
    CHECK(lorieCopySettled(c1) && lorieCopySettled(c2), "socket lost, process dead: still held");
    CHECK(forced() == before, "socket lost, process dead: counted as recovery rather than as the process having exited");
    CHECK(!lorieGpuCopyMade(s1) && !lorieGpuCopyMade(s2), "socket lost, process dead: a copy it never reported counted as made");
    CHECK(shared.gpuCopyQueue.entryState[(s2 - 1) % CAP] == 2, "process dead: its queued copy left for the next renderer to run");

    /* 3. A new renderer, a different process, connects while the old one is still alive, and its
     *    watermark passes the old copy's serial. That is the new renderer's report, not the old one's. */
    reset();
    connectRenderer(102);
    s1 = enqueue(); take(1);
    c1 = hold(s1);
    connectRenderer(103);
    s2 = enqueue(); take(1); report(s2);
    CHECK(s1 < shared.gpuCopyQueue.completedSerial, "setup");
    CHECK(!lorieCopySettled(c1), "new renderer, old alive: the old copy let go of on the new connection");
    CHECK(!lorieGpuCopyResolved(s1) && !lorieGpuCopyMade(s1), "old session serial < new completedSerial: taken as resolved by the new watermark");
    fakeNow += 10 * 1000 * 1000;
    CHECK(!lorieCopySettled(c1), "new renderer, old alive: let go of after a wait");
    /* ...until the old process says it finished, as it lets go of the state */
    retire(102);
    CHECK(lorieCopySettled(c1), "old process said it retired: still held");
    CHECK(lorieGpuCopyMade(s1), "old process said it retired: its finished copy not counted as made");

    /* 4. The same process connecting again. It lets go of the old state - fences waited - before it
     *    can use the new connection, and says so. Settled as retired, not as recovery. */
    reset();
    connectRenderer(104);
    s1 = enqueue(); take(1);
    c1 = hold(s1);
    report(s1);                                  /* its retire waits the last fence and publishes it */
    retire(104);
    before = forced();
    connectRenderer(104);
    CHECK(lorieCopySettled(c1), "same process reconnected after retiring: still held");
    CHECK(forced() == before, "same process reconnected: settled as recovery, not as a finished copy");
    CHECK(lorieGpuCopyMade(s1), "same process reconnected: finished copy not made");

    /* 5. Nothing can tell whether the process is alive (no pidfd; kill only says something has the
     *    number). Held for the bounded wait, then let go of as recovery - counted, and never made. */
    reset();
    pidfdAllowed = 0;
    connectRenderer(105);
    s1 = enqueue(); take(1);
    c1 = hold(s1);
    lorieNoteRendererLost();
    fakeNow += 1000 * 1000;
    CHECK(!lorieCopySettled(c1), "liveness unknown: let go of before the wait ran out");
    fakeNow += 1500 * 1000;
    before = forced();
    CHECK(lorieCopySettled(c1), "liveness unknown: held past the wait");
    CHECK(forced() == before + 1, "liveness unknown: the release not counted as recovery");
    CHECK(!lorieGpuCopyMade(s1), "liveness unknown: counted as made");

    /* 6. The live connection: its own watermark and failure reports, as before. */
    reset();
    connectRenderer(106);
    s1 = enqueue(); s2 = enqueue(); take(2);
    c1 = hold(s1);
    CHECK(!lorieCopySettled(c1), "live: settled before its report");
    reportFailed(s2); report(s2);
    CHECK(lorieCopySettled(c1) && lorieGpuCopyMade(s1), "live: reported copy not settled or not made");
    CHECK(lorieGpuCopyResolved(s2) && !lorieGpuCopyMade(s2), "live: failed copy counted as made");

    /* 7. The old process is found gone only after another renderer had connected: which copies it had
     *    taken is unknown, so a copy is settled once the watermark passes it, and is not made. */
    reset();
    connectRenderer(107);
    s1 = enqueue(); take(1);
    c1 = hold(s1);
    connectRenderer(108);
    alive[107] = 0;
    CHECK(!lorieCopySettled(c1), "dead after a new connection: settled before anything passed it");
    s2 = enqueue(); take(1); report(s2);
    CHECK(lorieCopySettled(c1), "dead after a new connection: still held once the watermark passed it");
    CHECK(!lorieGpuCopyMade(s1), "dead after a new connection: counted as made");

    /* 8. Still queued when its connection went, and the old process said it was done without having
     *    taken it: whichever renderer takes it answers for it. */
    reset();
    connectRenderer(109);
    s1 = enqueue();
    c1 = hold(s1);
    retire(109);
    connectRenderer(110);
    CHECK(!lorieCopySettled(c1), "never taken by the old renderer: settled before the new one ran it");
    take(1); report(s1);
    CHECK(lorieCopySettled(c1) && lorieGpuCopyMade(s1), "never taken by the old renderer: the new one's report not taken");

    printf("session lifetime (tsession): %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
