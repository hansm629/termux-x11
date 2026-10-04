/* T32: renderer reconnect ordering. A renderer (the Termux:X11 app) goes away and a new one connects
 * while the X server keeps running; the old socket's hangup and the new connection arrive in either
 * order. The connection bookkeeping (cmdentrypoint.c: addFd, the hangup path, buffer registration) and
 * lorieActivityConnected / lorieRegisterQueuedCopyBuffers (InitOutput.c) are extracted verbatim by
 * gen.py, and so is what gives back a copy's buffers (lorieReleaseCopyResources). Simulated: the input
 * thread's watch list, the X server's work queue - run step by step so every order can be forced - the
 * sockets, buffer references, and what each renderer has been sent. The renderer
 * session bookkeeping is a stand-in here (lorieNoteRendererConnected and friends), as tsession tests
 * the real one. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
typedef int Bool;
#define TRUE 1
#define FALSE 0
#define __unused __attribute__((unused))
typedef void *ClientPtr;
typedef uint32_t CARD32;
struct xorg_list { struct xorg_list *next, *prev; };
struct present_fence;
typedef struct _Window { int dummy; } *WindowPtr;
typedef struct { int32_t width, stride, height, format, type; uint64_t id; void *buffer; } LorieBuffer_Desc;
typedef struct LorieBuffer { LorieBuffer_Desc desc; struct LorieBuffer *nextInList; int listed, refs, freed; } LorieBuffer;
typedef struct { LorieBuffer *buffer; Bool rootDouble; LorieBuffer *rootBuf[5]; } LoriePixmapPriv;
typedef struct _Pixmap { LoriePixmapPriv *priv; } *PixmapPtr;
#define LORIE_ROOT_SLOTS 5
#define LORIE_GPU_COPY_QUEUE_CAPACITY 8
#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(p) ((p) ? ((PixmapPtr) (p))->priv : NULL)
#define LORIE_BUFFER_FROM_PIXMAP(p) ((p) ? ((PixmapPtr) (p))->priv->buffer : NULL)
static void logSink(const char *fmt, ...) { (void) fmt; }
#define log(prio, ...) logSink(__VA_ARGS__)
#define FatalError(...) abort()
#define X_NOTIFY_ERROR 4

/* the list of buffers sent on the current connection */
static LorieBuffer *registeredHead;
struct xorg_list registeredBuffers;
static const LorieBuffer_Desc *LorieBuffer_description(LorieBuffer *b) { return &b->desc; }
static LorieBuffer *LorieBufferList_findById(struct xorg_list *l, uint64_t id) {
    (void) l;
    for (LorieBuffer *b = registeredHead; b; b = b->nextInList) if (b->desc.id == id) return b;
    return NULL;
}
static LorieBuffer *LorieBufferList_first(struct xorg_list *l) { (void) l; return registeredHead; }
static void LorieBuffer_addToList(LorieBuffer *b, struct xorg_list *l) {
    (void) l; if (b->listed) return; b->listed = 1; b->nextInList = registeredHead; registeredHead = b;
}
static void LorieBuffer_removeFromList(LorieBuffer *b) {
    for (LorieBuffer **p = &registeredHead; *p; p = &(*p)->nextInList)
        if (*p == b) { *p = b->nextInList; b->listed = 0; return; }
}
/* references: freeing takes a buffer off whatever list it is on, as __LorieBuffer_free does */
static void __attribute__((unused)) LorieBuffer_release(LorieBuffer *b) {
    if (b && --b->refs == 0) { b->freed = 1; LorieBuffer_removeFromList(b); }
}
static bool __attribute__((unused)) LorieBuffer_isLastReference(LorieBuffer *b) { return b && b->refs == 1; }
static void __attribute__((unused)) LorieBuffer_gpuCopyPendingDec(LorieBuffer *b) { (void) b; }

/* sockets: fd numbers are never reused here, so every check can name them */
#define FDS 256
static int fdOpen[FDS], fdClosed[FDS], stateSent[FDS];
static uint8_t received[FDS][64];             /* buffer ids each renderer has been sent (and not removed) */
static void *watched[FDS];                     /* input thread: fd -> the data it was registered with */
static int isWatched[FDS];
static int nextFd = 10;
static volatile int conn_fd = -1;
static void InputThreadRegisterDev(int fd, void (*cb)(int, int, void *), void *data) { (void) cb; isWatched[fd] = 1; watched[fd] = data; }
static int InputThreadUnregisterDev(int fd) { int was = isWatched[fd]; isWatched[fd] = 0; return was; }
static int doubleCloses;
static void fakeClose(int fd) { if (!fdOpen[fd]) doubleCloses++; fdOpen[fd] = 0; fdClosed[fd]++; }
#define close(fd) fakeClose(fd)
static int lastEventType;
#include "t32_types.inc"
static long fakeWrite(int fd, const void *buf, size_t len) {
    const lorieEvent *e = buf;
    (void) len;
    if (!fdOpen[fd]) return -1;
    lastEventType = e->type;
    if (e->type == EVENT_SHARED_SERVER_STATE) stateSent[fd] = 1;
    if (e->type == EVENT_REMOVE_BUFFER) received[fd][e->removeBuffer.id % 64] = 0;
    return (long) len;
}
#define write(fd, buf, len) fakeWrite(fd, buf, len)
static void LorieBuffer_sendHandleToUnixSocket(LorieBuffer *b, int fd) { if (fdOpen[fd]) received[fd][b->desc.id % 64] = 1; }
static int ancil_send_fd(int sock, int fd) { (void) sock; (void) fd; return 0; }
static void lorieEnableClipboardSync(Bool on) { (void) on; }
static void lorieWakeServer(void) {}

/* the X server's work queue, run one item at a time so the order can be chosen */
typedef Bool (*WorkProc)(ClientPtr, void *);
static struct { WorkProc fn; void *closure; } work[256];
static int workHead, workTail;
static void QueueWorkProc(WorkProc fn, ClientPtr c, void *closure) { (void) c; work[workTail].fn = fn; work[workTail++].closure = closure; }
static int runOne(void) { if (workHead == workTail) return 0; work[workHead].fn(NULL, work[workHead].closure); workHead++; return 1; }
static void runAll(void) { while (runOne()); }

/* stand-in for the renderer session bookkeeping */
static uint32_t session;
static int sessionLost[256];
static uint32_t lorieRendererSessionId(void) { return session; }
static Bool lorieRendererSessionIsCurrent(uint32_t s) { return s == session; }
static void lorieNoteRendererConnected(int32_t pid) { (void) pid; session++; }
static void lorieNoteRendererLost(void) { sessionLost[session]++; }

/* the screen */
static LorieBuffer bufs[64];
static LoriePixmapPriv rootPrivRec, flipPrivRec;
static struct _Pixmap rootPixmap = { &rootPrivRec }, flipPixmap = { &flipPrivRec };
static struct _Window rootWindow;
static PixmapPtr shownPixmap = &rootPixmap;
static PixmapPtr GetWindowPixmap(WindowPtr w) { (void) w; return shownPixmap; }
static struct { void *devPrivate; WindowPtr root; PixmapPtr (*GetWindowPixmap)(WindowPtr); } screenRec = { &rootPixmap, &rootWindow, GetWindowPixmap };
static typeof(screenRec) *pScreenPtr = &screenRec;
static struct { bool drawRequested; struct { bool updated; } cursor; } fakeStateRec;
static struct { typeof(fakeStateRec) *state; int stateFd; } fakePvfb = { &fakeStateRec, 3 };
#define pvfb (&fakePvfb)

void handleLorieEvents(int fd, int ready, void *data);
#include "t32_src.inc"
void handleLorieEvents(int fd, int ready, void *data) { (void) fd; (void) ready; (void) data; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void) {
    memset(fdOpen, 0, sizeof fdOpen); memset(fdClosed, 0, sizeof fdClosed); memset(stateSent, 0, sizeof stateSent);
    memset(received, 0, sizeof received); memset(isWatched, 0, sizeof isWatched); memset(sessionLost, 0, sizeof sessionLost);
#ifndef HAVE_OLD_CONNECTIONS
    memset(lorieConnections, 0, sizeof lorieConnections);
#endif
    memset(lorieCopyRecords, 0, sizeof lorieCopyRecords);
    memset(bufs, 0, sizeof bufs);
    for (int i = 0; i < 64; i++) { bufs[i].desc.id = i; bufs[i].refs = 1; }
    registeredHead = NULL; conn_fd = -1; session = 0; workHead = workTail = 0; doubleCloses = 0;
    rootPrivRec = (LoriePixmapPriv) { &bufs[1], TRUE, { &bufs[2], &bufs[3], &bufs[4], &bufs[5], &bufs[6] } };
    flipPrivRec = (LoriePixmapPriv) { &bufs[20], FALSE, { 0 } };
    shownPixmap = &rootPixmap;
}
/* the app asks for a connection (getXConnection): a socket pair, one end queued for the X server */
static int connectApp(int pid) {
    int fd = nextFd++;
    uint32_t n = lorieNewConnectionNext++ % 4u;
    fdOpen[fd] = 1;
    lorieNewConnections[n].fd = fd;
    lorieNewConnections[n].pid = pid;
    QueueWorkProc(addFd, NULL, (void *) (uintptr_t) n);
    return fd;
}
/* the input thread sees the app's end of `fd` go away, if it is still watching it */
static void hangUp(int fd) { if (isWatched[fd]) lorieConnectionHungUp(fd, watched[fd]); }
/* everything the renderer on `fd` needs to put the root on screen and run the queued copies */
static int missing(int fd) {
    int n = 0;
    LorieBuffer *need[16] = { rootPrivRec.buffer, shownPixmap->priv->buffer };
    int k = 2;
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++) need[k++] = rootPrivRec.rootBuf[i];
    for (int i = 0; i < LORIE_COPY_RECORDS && k < 16; i++)
        if (lorieCopyRecords[i].inUse) { if (lorieCopyRecords[i].src) need[k++] = lorieCopyRecords[i].src;
                                        if (lorieCopyRecords[i].dst && k < 16) need[k++] = lorieCopyRecords[i].dst; }
    for (int i = 0; i < k; i++) if (need[i] && !received[fd][need[i]->desc.id % 64]) n++;
    return n;
}
/* the state after a reconnect to `fd`: it is the current connection, open, watched, sent the state and
 * every buffer it needs, and its session was never treated as lost */
static void healthy(int fd, uint32_t s, const char *what) {
    CHECK(conn_fd == fd, "%s: conn_fd %d, the new connection is %d", what, conn_fd, fd);
    CHECK(fdOpen[fd] && isWatched[fd], "%s: the new connection closed or no longer watched", what);
    CHECK(stateSent[fd], "%s: the new renderer was never sent the shared state", what);
    CHECK(missing(fd) == 0, "%s: the new renderer lacks %d buffers it needs (Buffer N not found)", what, missing(fd));
    CHECK(session == s && sessionLost[s] == 0, "%s: the new session %u was treated as lost", what, s);
    CHECK(doubleCloses == 0, "%s: a socket closed twice", what);
}

int main(void) {
    int a, b;

    /* A. the old renderer's hangup is dealt with, then the new one connects */
    reset();
    a = connectApp(100); runAll();
    hangUp(a); runAll();
    CHECK(sessionLost[1] == 1 && fdClosed[a] == 1 && conn_fd == -1, "A: the old connection not torn down");
    b = connectApp(200); runAll();
    healthy(b, 2, "A");

    /* B. the hangup is queued, the new connection comes and is handled, then the hangup is run */
    reset();
    a = connectApp(100); runAll();
    hangUp(a);
    b = connectApp(200);
    runAll();                                       /* lost(1) runs first, then addFd */
    healthy(b, 2, "B (hangup first in the queue)");
    reset();
    a = connectApp(100); runAll();
    b = connectApp(200); runOne();                  /* the new connection is made ... */
    hangUp(a); runAll();                            /* ... then the old hangup is queued and run */
    healthy(b, 2, "B (hangup run after the new connection)");
    CHECK(fdClosed[a] == 1, "B: the old socket closed %d times", fdClosed[a]);

    /* C. the review's counterexample: the new renderer is connected (session 2) and the old socket's
     *    hangup only then arrives. It is session 1's, and session 2 must not notice it. */
    reset();
    a = connectApp(100); runAll();
    b = connectApp(200); runAll();
    hangUp(a); runAll();
    healthy(b, 2, "C");

    /* D. every order of: the old hangup, its handling, the new connection queued, and handled */
    {
        const char *orders[] = { "HQRR", "HRQR", "QHRR", "QRHR", "QRRH" };
        for (int o = 0; o < 5; o++) {
            reset();
            a = connectApp(100); runAll();
            for (const char *s = orders[o]; *s; s++) {
                if (*s == 'H') hangUp(a);
                else if (*s == 'Q') b = connectApp(200);
                else runOne();
            }
            runAll();
            char what[32]; snprintf(what, sizeof what, "D order %s", orders[o]);
            healthy(b, 2, what);
            CHECK(fdClosed[a] == 1, "%s: the old socket closed %d times", what, fdClosed[a]);
        }
    }

    /* E. copies still out when the renderer goes: their buffers - a client's pixmap sent on the old
     *    connection, a slot as destination - go to the new renderer too, and so does a client's pixmap
     *    the root window is showing (flip). */
    reset();
    a = connectApp(100); runAll();
    lorieRegisterBuffer(&bufs[30]);                 /* a client's pixmap, sent when its copy was queued */
    bufs[30].refs = bufs[4].refs = 2;               /* the pixmap's or the slot's reference, and the copy's */
    lorieCopyRecords[0].inUse = TRUE; lorieCopyRecords[0].src = &bufs[30]; lorieCopyRecords[0].dst = &bufs[4];
    lorieRegisterBuffer(&bufs[20]);
    shownPixmap = &flipPixmap;
    b = connectApp(200); runOne();
    hangUp(a); runAll();
    healthy(b, 2, "E");
    CHECK(received[b][30] && received[b][20], "E: the queued copy's source or the flipped pixmap not sent again");
    /* the copy done: its buffers still belong to their pixmap and to the root, and stay with the renderer */
    lorieCopyRecords[0].inUse = FALSE;
    lorieReleaseCopyResources(lorieCopyRecords[0].src, lorieCopyRecords[0].dst);
    CHECK(received[b][30] && received[b][4], "E: a buffer still in use taken from the new renderer when the copy was done");

    /* the old regression: every slot id the renderer can be told to sample must be one it was sent */
    for (int i = 0; i < LORIE_ROOT_SLOTS; i++)
        CHECK(received[b][rootPrivRec.rootBuf[i]->desc.id], "slot %d's buffer never sent to the new renderer", i);

    /* G. a copy outliving its source's pixmap across the reconnect. The pixmap was destroyed while the copy
     *    was out, and the old renderer told then to let go of the buffer; the new one is sent it for the
     *    copy, and has to be told to let go of it when the copy gives up the last reference - or it keeps
     *    it for as long as it stays connected. */
    reset();
    a = connectApp(100); runAll();
    lorieRegisterBuffer(&bufs[31]);
    bufs[31].refs = bufs[5].refs = 2;
    lorieCopyRecords[1].inUse = TRUE; lorieCopyRecords[1].src = &bufs[31]; lorieCopyRecords[1].dst = &bufs[5];
    lorieUnregisterBuffer(&bufs[31]); LorieBuffer_release(&bufs[31]);       /* lorieDestroyPixmap */
    CHECK(!received[a][31], "G: the old renderer not told to let go of the destroyed pixmap's buffer");
    b = connectApp(200); runAll();
    hangUp(a); runAll();
    healthy(b, 2, "G");
    CHECK(received[b][31], "G: the copy's source, its pixmap gone, not sent to the new renderer");
    lorieCopyRecords[1].inUse = FALSE;
    lorieReleaseCopyResources(lorieCopyRecords[1].src, lorieCopyRecords[1].dst);
    CHECK(bufs[31].freed && !received[b][31], "G: the copy's source freed with the new renderer never told (kept there)");
    CHECK(!bufs[5].freed && received[b][5], "G: the slot the copy wrote into taken from the new renderer");
    CHECK(!bufs[31].listed, "G: a freed buffer left on the registered list");

    /* F. twenty reconnects in a row, the old hangup each time handled before the new connection, queued
     *    before it, arriving after it, or not until all of them are done */
    reset();
    int fds[32];
    fds[0] = connectApp(100); runAll();
    for (int k = 1; k <= 20; k++) {
        switch (k % 4) {
        case 0: hangUp(fds[k - 1]); runAll(); fds[k] = connectApp(100 + k); runAll(); break;
        case 1: hangUp(fds[k - 1]); fds[k] = connectApp(100 + k); runAll(); break;
        case 2: fds[k] = connectApp(100 + k); runAll(); hangUp(fds[k - 1]); runAll(); break;
        case 3: fds[k] = connectApp(100 + k); runAll(); break;
        }
        char what[32]; snprintf(what, sizeof what, "F reconnect %d", k);
        healthy(fds[k], (uint32_t) k + 1, what);
    }
    runAll();
    for (int k = 0; k < 20; k++) hangUp(fds[k]);                    /* the stragglers */
    runAll();
    healthy(fds[20], 21, "F");
    int open = 0;
    for (int k = 0; k < 20; k++) { CHECK(fdClosed[fds[k]] == 1, "F: socket %d closed %d times", k, fdClosed[fds[k]]); open += fdOpen[fds[k]]; }
    int tableOpen = 1;
#ifndef HAVE_OLD_CONNECTIONS
    tableOpen = 0;
    for (size_t i = 0; i < sizeof lorieConnections / sizeof lorieConnections[0]; i++) tableOpen += lorieConnections[i].open;
#endif
    CHECK(open == 0 && tableOpen == 1, "F: %d old sockets left open, %d connections open in the table", open, tableOpen);

    printf("T32 renderer reconnect ordering: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
