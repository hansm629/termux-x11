/* TPRESENTSAFETY (P10-S0, P10-S1): how a presentation whose copy was offloaded to the renderer's GPU ends, run on the
 * patched xserver's own code (patches/xserver.patch applied to a copy of present/, gen.py "presentsafety"):
 * present_execute, present_execute_copy, present_execute_post, present_re_execute, present_vblank_scrap,
 * present_vblank_destroy, present_free_window_vblank, lorieRecheckGpuCopies, the retired copies
 * (present_retired_copy_reserve, present_retire_copy, present_retired_copies_check, _forget_window, _drain) and
 * present_scmd_pixmap's loop that
 * scraps a queued presentation a new one overwrites in the same frame. The rest of the server is stubbed; the lorie
 * hooks are recorders. lorieGpuCopySettle answers as the real one does for the state a case sets (that one is run for
 * real against the shared queue in tpresentcopy.c): DONE once the copy's completion is published, RUNNING while the
 * renderer has it (recorded, in flight) or the shared lock is busy, otherwise CANCELLED - marked, so the renderer
 * never copies it.
 *
 * Every case logs, in order, what reaches the client and what the X server lets go: its references to the source
 * and destination (lorieGpuCopyAck), the client told its pixmap is idle (present_pixmap_idle: "idle", or
 * "idle(fence)" with the window gone - the fence alone is triggered), the presentation completed (with its mode) -
 * each marked with whether the GPU may still use the copy's buffers then ("+" it may not: done or cancelled, "!" it
 * may). No reference let go, no client told idle or presented while it may ("ack!", "idle!", "complete(copy)!"
 * never appear; a skipped presentation's completion may come first - it tells nothing about the buffer) - but at the
 * screen's close ("ack(close)": then the pixmaps go with the screen, no one is left to tell, and the renderer's own
 * references, not these, keep the buffers its GPU may still use - present_retired_copies_drain); a copy that
 * never ran completes as skipped; each reference, idle fence and notification goes exactly once; nothing is left
 * behind - every vblank and retired copy freed once (their allocations counted, and failed at will: out of memory, no
 * GPU copy is scheduled without its retired copy, and retiring one never allocates).
 *
 * P11-C3-D1-A (TERMUX_X11_EXA_COMPOSITE_OPT=1): the copy's damage, logged apart ("+"/"!" as above: the GPU may no longer
 * or may still write the window's pixmap then) with its region (regions here are one box): off, reported as the copy
 * is queued; on, once it is done - the window's clip, or the update region moved into the window (x_off/y_off, the
 * window where it is then) and clipped - before the client is told the pixmap is idle; a presentation scrapped meanwhile
 * reports it then (copy done: "+"; still running, as off: "!"), never when its copy never runs (cancelled, or its
 * renderer stalled) or its window goes; the update region destroyed once whatever happens; a CPU copy never puts it
 * off. Each counted by how it went. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the server, as far as these functions use it ---- */
typedef int Bool;
typedef uint32_t XID;
typedef uint16_t CARD16;
#define TRUE 1
#define FALSE 0
#define Success 0
#define DebugPresent(x)
#include "xorglist_src.inc"
typedef struct { int n; int x1, y1, x2, y2; } RegionRec, *RegionPtr;     /* one box: n 0 (empty) or 1 */
typedef struct { short x1, y1, x2, y2; } BoxRec;
typedef struct _Screen *ScreenPtr;
typedef struct { ScreenPtr pScreen; int16_t x, y; XID id; uint16_t width, height; } DrawableRec;
typedef struct _Window { DrawableRec drawable; RegionRec clipList; struct present_window_priv *priv; } WindowRec, *WindowPtr;
typedef struct _Pixmap { DrawableRec drawable; int16_t screen_x, screen_y; int refs; uint64_t copySerial; } PixmapRec, *PixmapPtr;
typedef struct _RRCrtc { ScreenPtr pScreen; } *RRCrtcPtr;
typedef struct _Screen { WindowPtr root; PixmapPtr (*GetWindowPixmap)(WindowPtr); PixmapPtr (*GetScreenPixmap)(ScreenPtr); } ScreenRec;
typedef struct present_fence { int triggered, destroyed; } *present_fence_ptr;
typedef struct present_notify *present_notify_ptr;
typedef enum { PRESENT_FLIP_REASON_UNKNOWN, PRESENT_FLIP_REASON_BUFFER_FORMAT } PresentFlipReason;
enum { PresentCompleteKindPixmap, PresentCompleteKindNotifyMSC };
enum { PresentCompleteModeCopy, PresentCompleteModeFlip, PresentCompleteModeSkip, PresentCompleteModeSuboptimalCopy };
struct present_info { void (*after_flip)(RRCrtcPtr, uint64_t, uint64_t, uint64_t, PixmapPtr); };
typedef struct present_screen_priv {
    int (*queue_vblank)(ScreenPtr, WindowPtr, RRCrtcPtr, uint64_t, uint64_t);
    void (*flush)(WindowPtr);
    void (*abort_vblank)(ScreenPtr, WindowPtr, RRCrtcPtr, uint64_t, uint64_t);
    struct present_vblank *flip_pending;
    uint64_t unflip_event_id;
    WindowPtr flip_window;
    PixmapPtr flip_pixmap;
    struct present_info *info;
} present_screen_priv_rec, *present_screen_priv_ptr;
typedef struct present_window_priv { struct xorg_list vblank; } present_window_priv_rec, *present_window_priv_ptr;
typedef struct present_vblank *present_vblank_ptr;

#include "presentsafety_struct_src.inc"
#include <time.h>
#include "diagtypes_src.inc"
/* the kinds the patch counts are lorie.h's */
_Static_assert(LORIE_GPU_COPY_EARLY_SCRAP == LORIE_X_EARLY_SCRAP && LORIE_GPU_COPY_EARLY_DESTROY == LORIE_X_EARLY_DESTROY &&
               LORIE_GPU_COPY_EARLY_POLL_FIRST == LORIE_X_EARLY_POLL_FIRST && LORIE_GPU_COPY_EARLY_POLL_LATER == LORIE_X_EARLY_POLL_LATER &&
               LORIE_GPU_COPY_EARLY_STALLED == LORIE_X_EARLY_STALLED, "patches/xserver.patch's kinds are not lorie.h's");

static struct xorg_list present_exec_queue, present_flip_queue;
static ScreenRec screen;
static WindowRec window;
static PixmapRec dstPixmap;
static struct _RRCrtc crtcRec = { &screen };
static present_window_priv_rec windowPriv;
static present_screen_priv_rec screenPriv;
static struct present_info info;

/* ---- the renderer's side of a copy, as a case sets it ---- */
static uint64_t completed;          /* the renderer's completedSerial */
static uint64_t recorded;           /* copies up to this one the renderer recorded (in flight until completed) */
static bool lockBusy;               /* the shared lock held by a renderer that exists */
static bool submitterGone;          /* the renderer process that recorded them no longer exists */
static uint64_t cancelled[16];      /* serials lorieGpuCopySettle marked cancelled */
static int ncancelled;
static bool isCancelled(uint64_t s) { for (int i = 0; i < ncancelled; i++) if (cancelled[i] == s) return true; return false; }
/* whether the GPU may still use copy `s`'s buffers */
static bool gpuMay(uint64_t s) { return s && completed < s && !isCancelled(s); }

static int queueVblankFails;        /* the next n queue_vblank calls fail */
static bool connectionAlive = true, rendererAvailable = true;
static char trail[512];
static PixmapPtr current;           /* the presentation's pixmap, for the steps that no longer have it */
static void step(const char *what, PixmapPtr p) {
    snprintf(trail + strlen(trail), sizeof trail - strlen(trail), "%s%s%s", trail[0] ? " " : "", what,
             gpuMay(p ? p->copySerial : 0) ? "!" : "+");
}
static uint64_t nextSerial;

static Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,
                                    uint64_t *out_serial, void **out_dst_buffer) {
    (void) dst; (void) update; (void) x_off; (void) y_off;
    pixmap->copySerial = *out_serial = ++nextSerial;
    *out_dst_buffer = &dstPixmap;
    pixmap->refs++;                 /* the X server's references (LorieBuffer_acquire), until the ack */
    dstPixmap.refs++;
    return TRUE;
}
static Bool lorieGpuCopyIsDone(uint64_t serial) { return completed >= serial; }
static bool closing;                /* the screen closing (present_close_screen) */
static void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer) {
    pixmap->refs--;
    ((PixmapPtr) dst_buffer)->refs--;
    step(closing ? "ack(close)" : "ack", pixmap);
}
int lorieGpuCopySettle(uint64_t serial) {
    if (completed >= serial)
        return LORIE_GPU_COPY_DONE;
    if (lockBusy)
        return LORIE_GPU_COPY_RUNNING;
    if (serial <= recorded && !submitterGone)
        return LORIE_GPU_COPY_RUNNING;
    if (!isCancelled(serial))
        cancelled[ncancelled++] = serial;
    return LORIE_GPU_COPY_CANCELLED;
}
static bool lorieConnectionAlive(void) { return connectionAlive; }
/* P10-B0: the patch counts each early end, by kind (lorie.h LORIE_X_EARLY_*) */
static int early[LORIE_X_EARLY_COUNT];
static void lorieCountGpuCopyEarly(int how) { early[how]++; }
static bool lorieRendererAvailable(void) { return rendererAvailable; }
/* P10-S1: retired copies made and let go, as the patch counts them */
static int retiredMade, retiredReleased;
void lorieCountGpuCopyRetired(int made) { if (made) retiredMade++; else retiredReleased++; }

static present_screen_priv_ptr present_screen_priv(ScreenPtr s) { (void) s; return &screenPriv; }
static present_window_priv_ptr present_window_priv(WindowPtr w) { return w->priv; }
static present_window_priv_ptr present_get_window_priv(WindowPtr w, Bool create) { (void) create; return w->priv; }
static Bool present_execute_wait(struct present_vblank *v, uint64_t msc) { (void) v; (void) msc; return FALSE; }
static int present_get_ust_msc(ScreenPtr s, RRCrtcPtr c, uint64_t *ust, uint64_t *msc) { (void) s; (void) c; *ust = 0; *msc = 10; return Success; }
static Bool present_flip(RRCrtcPtr c, uint64_t id, uint64_t msc, PixmapPtr p, Bool sync) { (void) c; (void) id; (void) msc; (void) p; (void) sync; return FALSE; }
static void present_set_tree_pixmap(WindowPtr w, PixmapPtr e, PixmapPtr p) { (void) w; (void) e; (void) p; }
static void present_set_abort_flip(ScreenPtr s) { (void) s; }
static void present_unflip(ScreenPtr s) { (void) s; }
static void present_copy_region(DrawableRec *d, PixmapPtr p, RegionPtr r, int16_t x, int16_t y) { (void) d; (void) r; (void) x; (void) y; step("cpu-copy", p); }
static int idleFences, fencesDestroyed;
static void present_pixmap_idle(PixmapPtr p, WindowPtr w, uint32_t serial, present_fence_ptr f) {
    (void) serial;
    if (f)
        f->triggered++, idleFences++;
    step(w ? "idle" : "idle(fence)", p);
}
static void present_fence_destroy(present_fence_ptr f) { if (f) f->destroyed++, fencesDestroyed++; }
static void dixDestroyPixmap(PixmapPtr p, XID id) { (void) id; p->refs--; }
static void present_destroy_notifies(present_notify_ptr n, int count) { (void) n; (void) count; }
static void present_vblank_notify(struct present_vblank *v, CARD16 kind, CARD16 mode, uint64_t ust, uint64_t msc) {
    (void) v; (void) kind; (void) ust; (void) msc;
    step(mode == PresentCompleteModeSkip ? "complete(skip)" : "complete(copy)", current);
}
static int regionsDestroyed, regionsLive;
static bool regionCopyFails;
static void RegionDestroy(RegionPtr r) { if (r) regionsDestroyed++; }
static void RegionNull(RegionPtr r) { *r = (RegionRec) { 0 }; regionsLive++; }
static void RegionInit(RegionPtr r, BoxRec *b, int n) { *r = b && n ? (RegionRec) { 1, b->x1, b->y1, b->x2, b->y2 } : (RegionRec) { 0 }; regionsLive++; }
static void RegionUninit(RegionPtr r) { (void) r; regionsLive--; }
static Bool RegionCopy(RegionPtr d, RegionPtr s) { if (regionCopyFails) return FALSE; *d = *s; return TRUE; }
static void RegionTranslate(RegionPtr r, int dx, int dy) { r->x1 += dx, r->y1 += dy, r->x2 += dx, r->y2 += dy; }
static void RegionIntersect(RegionPtr d, RegionPtr a, RegionPtr b) {
    RegionRec r = { 1, a->x1 > b->x1 ? a->x1 : b->x1, a->y1 > b->y1 ? a->y1 : b->y1, a->x2 < b->x2 ? a->x2 : b->x2,
                    a->y2 < b->y2 ? a->y2 : b->y2 };
    if (!a->n || !b->n || r.x1 >= r.x2 || r.y1 >= r.y2)
        r = (RegionRec) { 0 };
    *d = r;
}
/* P11-C3-D1-A: the damage reported, logged apart (dtrail) - its box, and whether the GPU may still write then */
static char dtrail[256];
static bool damageBeforeIdle;
static void DamageDamageRegion(DrawableRec *d, RegionPtr r) {
    (void) d;
    snprintf(dtrail + strlen(dtrail), sizeof dtrail - strlen(dtrail), "%sdamage(%d,%d,%d,%d)%s", dtrail[0] ? " " : "",
             r->n ? r->x1 : 0, r->n ? r->y1 : 0, r->n ? r->x2 : 0, r->n ? r->y2 : 0, gpuMay(current ? current->copySerial : 0) ? "!" : "+");
    damageBeforeIdle = !strstr(trail, "idle");
}
/* P11-C3-D0: the content diagnostics told a copy's damage was reported - which copy, and whether the damage was, just
 * before (dtrail's last entry is that copy's) */
static uint64_t damagedSerial;
static int damagedTold, damagedUntold;
static size_t damagedAt;
static void lorieContentDamaged(uint64_t serial) {
    damagedSerial = serial;
    damagedTold++;
    if (strlen(dtrail) == damagedAt)
        damagedUntold++;        /* no damage reported since the last time */
    damagedAt = strlen(dtrail);
}
static bool deferDamage;            /* TERMUX_X11_EXA_COMPOSITE_OPT */
static int damageDeferred, damageHow[3];
static Bool lorieDeferCopyDamage(void) { damageDeferred += deferDamage; return deferDamage; }
static void lorieCountCopyDamage(int how) { damageHow[how]++; }
static int queued;                  /* polls queued (the lorie vblank events), minus those aborted */
static int queueVblank(ScreenPtr s, WindowPtr w, RRCrtcPtr c, uint64_t id, uint64_t msc) {
    (void) s; (void) w; (void) c; (void) id; (void) msc;
    if (queueVblankFails > 0) {
        queueVblankFails--;
        return 1;
    }
    queued++;
    return Success;
}
static void flush(WindowPtr w) { (void) w; }
static void abortVblank(ScreenPtr s, WindowPtr w, RRCrtcPtr c, uint64_t id, uint64_t msc) { (void) s; (void) w; (void) c; (void) id; (void) msc; }
static PixmapPtr getWindowPixmap(WindowPtr w) { (void) w; return &dstPixmap; }

/* the patch's own allocations (vblanks, made here, counted as theirs): live, attempted, and failing while out of memory */
static int allocs, allocCalls;
static bool outOfMemory;
static void *testCalloc(size_t n, size_t size) {
    allocCalls++;
    if (outOfMemory)
        return NULL;
    allocs++;
    return calloc(n, size);
}
static void testFree(void *p) {
    if (p)
        allocs--;
    free(p);
}

/* P11-C3-D0: the content hooks the patch calls (InitOutput.c lorieContent*), as a log of their own - the
 * presentation's number, copied by the GPU or the CPU, its GPU copy done, its CompleteNotify, skipped */
static char ctrail[256];
static uint32_t contentSeqNext;     /* the number presentation() gives the next one (0: not followed) */
static void cstep(const char *what, uint32_t seq, const char *how) {
    snprintf(ctrail + strlen(ctrail), sizeof ctrail - strlen(ctrail), "%s%s(%u%s%s)", ctrail[0] ? " " : "", what, seq,
             how ? "," : "", how ? how : "");
}
static void lorieContentSkipped(uint32_t seq) { if (seq) cstep("skipped", seq, NULL); }
static uint64_t lorieContentCopied(uint32_t seq, uint64_t request_us, PixmapPtr dst, Bool gpu) {
    (void) request_us, (void) dst;
    if (seq)
        cstep("copied", seq, gpu ? "gpu" : "cpu");
    return seq ? 1000 + seq : 0;
}
static void lorieContentCopyDone(uint32_t seq, uint64_t request_us, uint64_t copy_us, PixmapPtr dst) {
    (void) request_us, (void) dst;
    if (seq)
        cstep(copy_us == 1000 + seq ? "done" : "done(no queue time)", seq, NULL);
}
static void lorieContentComplete(uint32_t seq, uint8_t mode) {
    if (seq)
        cstep("complete", seq, mode == PresentCompleteModeSkip ? "skip" : "copy");
}
void present_damage_gpu_copy(struct present_vblank *vblank, int how);
void present_execute_copy(struct present_vblank *vblank, uint64_t crtc_msc);
void present_execute_post(struct present_vblank *vblank, uint64_t ust, uint64_t crtc_msc);
void present_vblank_scrap(struct present_vblank *vblank);
void present_vblank_destroy(struct present_vblank *vblank);
Bool present_retired_copy_reserve(struct present_vblank *vblank);
static void present_execute(struct present_vblank *vblank, uint64_t ust, uint64_t crtc_msc);
#define calloc testCalloc
#define free testFree
#include "presentsafety_src.inc"
#undef calloc
#undef free

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* P11-C3-D1-A: the next presentation's update region (none: n 0) and offset into the window */
static RegionRec nextUpdate;
static int16_t nextXOff, nextYOff;

/* a client's presentation of `pixmap` to the window at msc 10, as present_scmd_pixmap queues and executes it */
static struct present_vblank *presentation(PixmapPtr pixmap) {
    struct present_vblank *v = calloc(1, sizeof *v);
    allocs++;                       /* present_vblank_destroy's to free */
    v->screen = &screen;
    v->window = &window;
    v->pixmap = pixmap;
    pixmap->refs++;                 /* the presentation's own reference */
    v->crtc = &crtcRec;
    v->kind = PresentCompleteKindPixmap;
    v->target_msc = v->exec_msc = 10;
    v->idle_fence = calloc(1, sizeof *v->idle_fence);
    xorg_list_init(&v->window_list);
    xorg_list_init(&v->event_queue);
    xorg_list_append(&v->window_list, &windowPriv.vblank);
    xorg_list_append(&v->event_queue, &present_exec_queue);
    v->queued = TRUE;
    v->lorie_content_seq = contentSeqNext;
    if (nextUpdate.n) {
        v->update = &nextUpdate;
        v->x_off = nextXOff, v->y_off = nextYOff;
    }
    current = pixmap;
    present_execute(v, 0, 10);
    return v;
}

/* P11-C3-D1-A: the damage logged and counted, and no update region for the next presentation */
static void dreset(void) {
    dtrail[0] = 0;
    damagedSerial = 0, damagedTold = damagedUntold = 0, damagedAt = 0;
    damageDeferred = regionsDestroyed = 0;
    memset(damageHow, 0, sizeof damageHow);
    nextUpdate = (RegionRec) { 0 }, nextXOff = nextYOff = 0;
}

static void reset(void) {
    memset(early, 0, sizeof early);
    trail[0] = 0;
    completed = recorded = nextSerial;      /* earlier cases' copies settled */
    ncancelled = 0;
    lockBusy = submitterGone = false;
    queueVblankFails = 0;
    connectionAlive = rendererAvailable = true;
    outOfMemory = false;
    xorg_list_init(&windowPriv.vblank);
    xorg_list_init(&present_exec_queue);
}

/* the copy, once the renderer recorded it, done: its completion published, then announced (EVENT_GPU_COPY_DONE) */
static void gpuDone(uint64_t s) {
    if (recorded < s)
        recorded = s;
    completed = s;
    lorieRecheckGpuCopies();
}

/* the server's end of a generation, a reset as an exit (dix_main): every window destroyed with its presentations
 * (FreeAllResources: present_destroy_window), then the screen closed (CloseScreen: present_close_screen) */
static void serverClose(void) {
    present_retired_copies_forget_window(&window);
    present_free_window_vblank(&window);
    closing = true;
    present_retired_copies_drain();
    closing = false;
}

/* one case's log and, of the early ends P10-B0 counts, which one (once), or none */
static void expect(const char *what, const char *want, int kind) {
    int counted = 0;
    for (int k = 0; k < LORIE_X_EARLY_COUNT; k++)
        counted += early[k];
    CHECK(!strcmp(trail, want), "%s: %s (want %s)", what, trail, want);
    CHECK(!strstr(trail, "ack!") && !strstr(trail, "idle!") && !strstr(trail, "idle(fence)!") && !strstr(trail, "complete(copy)!"),
          "%s: let go, told idle or told presented while the GPU may still use the copy's buffers: %s", what, trail);
    CHECK(kind < 0 ? counted == 0 : counted == 1 && early[kind] == 1, "%s: counted %d early ends (want %s)", what, counted,
          kind < 0 ? "none" : "one, of its kind");
    printf("  %-62s %s\n", what, trail);
}
/* nothing held after a case: every reference back, no retired copy left, every vblank and retired copy freed */
static void clean(const char *what, PixmapPtr p, int refs) {
    CHECK(p->refs == refs && dstPixmap.refs == 0 && xorg_list_is_empty(&present_retired_copies) && retiredMade == retiredReleased &&
          allocs == 0, "%s: references %d (want %d), destination %d, retired copies %s, %d made and %d let go, %d allocations left",
          what, p->refs, refs, dstPixmap.refs, xorg_list_is_empty(&present_retired_copies) ? "none" : "left", retiredMade,
          retiredReleased, allocs);
}

int main(void) {
    PixmapRec a = { .drawable.id = 0x200001 }, b = { .drawable.id = 0x200002 };
    struct present_vblank *v;

    xorg_list_init(&present_exec_queue);
    xorg_list_init(&present_flip_queue);
    xorg_list_init(&windowPriv.vblank);
    screen = (ScreenRec) { .root = &window, .GetWindowPixmap = getWindowPixmap };
    window = (WindowRec) { .drawable = { .pScreen = &screen, .id = 0x400001 }, .priv = &windowPriv };
    screenPriv = (present_screen_priv_rec) { .queue_vblank = queueVblank, .flush = flush, .abort_vblank = abortVblank, .info = &info };

    /* the copy queued: the presentation waits for it, nothing told, nothing let go */
    reset();
    v = presentation(&a);
    CHECK(v->gpu_copy_pending && v->queued && !trail[0] && a.refs == 2, "queued: pending %d queued %d, %s, %d references (want 2)",
          v->gpu_copy_pending, v->queued, trail, a.refs);
    /* the reference: done, then the completion recheck - every step after it */
    gpuDone(a.copySerial);
    expect("copy done, then the completion recheck", "ack+ idle+ complete(copy)+", -1);
    clean("copy done", &a, 0);

    /* overwritten in the same frame while the copy runs: retired, let go once it is done */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    CHECK(!trail[0] && a.refs == 2 && !xorg_list_is_empty(&present_retired_copies) && retiredMade == retiredReleased + 1,
          "overwritten, running: %s, %d references, %d retired copies held", trail, a.refs, retiredMade - retiredReleased);
    present_execute(v, 0, 11);              /* its msc+1 event: the presentation completes as skipped */
    gpuDone(a.copySerial);
    lorieRecheckGpuCopies();                /* a second completion event: nothing more */
    expect("overwritten in the same frame while copying (scrap)", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    clean("overwritten, running", &a, 0);

    /* overwritten before the renderer recorded it: cancelled - never copied - let go at once */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    CHECK(isCancelled(a.copySerial), "overwritten, not recorded: not cancelled");
    present_execute(v, 0, 11);
    expect("overwritten in the same frame before it was recorded (scrap)", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_SCRAP);
    clean("overwritten, not recorded", &a, 0);

    /* the window destroyed while the copy runs: retired with no notification owed; its fence destroyed, untriggered */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    present_free_window_vblank(&window);
    CHECK(!trail[0] && a.refs == 2, "window destroyed, running: %s, %d references", trail, a.refs);
    gpuDone(a.copySerial);
    expect("window destroyed while copying (destroy)", "ack+", LORIE_X_EARLY_DESTROY);
    CHECK(idleFences == 0 && fencesDestroyed == 1, "window destroyed: %d fences triggered, %d destroyed (want 0, 1)", idleFences, fencesDestroyed);
    clean("window destroyed, running", &a, 0);

    /* overwritten while copying, then its window destroyed: the idle owed goes to the fence alone */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_retired_copies_forget_window(&window);
    present_free_window_vblank(&window);    /* the scrapped presentation goes with the window */
    gpuDone(a.copySerial);
    expect("overwritten while copying, then its window destroyed", "ack+ idle(fence)+", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 1 && fencesDestroyed == 1, "then destroyed: %d fences triggered, %d destroyed (want 1, 1)", idleFences, fencesDestroyed);
    clean("overwritten, then destroyed", &a, 0);

    /* the same buffer presented again only once it is idle: overwritten while copying, A's buffer stays busy until its
     * copy is done; then B's copy, queued after it, runs */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    {
        struct present_vblank *w = presentation(&b);
        CHECK(!strstr(trail, "idle") && b.copySerial > a.copySerial, "the next: A told idle before its copy (%s)", trail);
        present_execute(v, 0, 11);
        gpuDone(a.copySerial);
        CHECK(strstr(trail, "idle+") && w->gpu_copy_pending, "the next: A not idle after its copy, or B done with it (%s)", trail);
        current = &b;
        gpuDone(b.copySerial);
    }
    expect("the next presentation queued behind one still copying", "complete(skip)! ack+ idle+ ack+ idle+ complete(copy)+", LORIE_X_EARLY_SCRAP);
    clean("the next", &a, 0);
    CHECK(b.refs == 0, "the next: B holds %d references", b.refs);

    /* the server closing (reset or exit) while the copy runs, overwritten or still queued: the X server's references
     * go with the screen, no one told, its fence destroyed untriggered - and the copy is not taken as done, nor
     * cancelled: the renderer runs it to its end, which then lets nothing more go */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    serverClose();
    CHECK(!isCancelled(a.copySerial) && completed < a.copySerial, "closed, overwritten while copying: the copy taken as %s",
          isCancelled(a.copySerial) ? "cancelled" : "done");
    gpuDone(a.copySerial);
    expect("the server closed, overwritten while copying", "complete(skip)! ack(close)!", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 0 && fencesDestroyed == 1, "closed, overwritten: %d fences triggered, %d destroyed (want 0, 1)", idleFences,
          fencesDestroyed);
    clean("closed, overwritten while copying", &a, 0);

    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    serverClose();
    CHECK(!isCancelled(a.copySerial) && completed < a.copySerial, "closed, queued while copying: the copy taken as %s",
          isCancelled(a.copySerial) ? "cancelled" : "done");
    gpuDone(a.copySerial);
    expect("the server closed, its presentation queued while copying", "ack(close)!", LORIE_X_EARLY_DESTROY);
    CHECK(idleFences == 0 && fencesDestroyed == 1, "closed, queued: %d fences triggered, %d destroyed (want 0, 1)", idleFences,
          fencesDestroyed);
    clean("closed, queued while copying", &a, 0);

    /* the copy done before the server closes: let go then, the client told idle - the close finds nothing */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    gpuDone(a.copySerial);
    serverClose();
    expect("the copy done, then the server closed", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 1 && fencesDestroyed == 1, "done, then closed: %d fences triggered, %d destroyed (want 1, 1)", idleFences,
          fencesDestroyed);
    clean("done, then closed", &a, 0);

    /* cancelled before the server closes (not recorded, known once the shared lock is free): let go then */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    lockBusy = true;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    lockBusy = false;
    lorieRecheckGpuCopies();
    CHECK(isCancelled(a.copySerial), "cancelled, then closed: not cancelled");
    serverClose();
    gpuDone(a.copySerial);                  /* the renderer skips the entry */
    expect("the copy cancelled, then the server closed", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 1 && fencesDestroyed == 1, "cancelled, then closed: %d fences triggered, %d destroyed (want 1, 1)", idleFences,
          fencesDestroyed);
    clean("cancelled, then closed", &a, 0);

    /* the renderer gone (its process too) before the server closes: settled at its disconnection, let go then */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    connectionAlive = false;
    submitterGone = true;
    lorieRecheckGpuCopies();                /* lorieRendererGoneWorkProc */
    serverClose();
    expect("the renderer gone, then the server closed", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 1 && fencesDestroyed == 1, "renderer gone, then closed: %d fences triggered, %d destroyed (want 1, 1)",
          idleFences, fencesDestroyed);
    clean("renderer gone, then closed", &a, 0);

    /* the server closed, then the renderer gone: let go once, at the close - its disconnection finds nothing */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    serverClose();
    connectionAlive = false;
    submitterGone = true;
    lorieRecheckGpuCopies();
    expect("the server closed, then the renderer gone", "complete(skip)! ack(close)!", LORIE_X_EARLY_SCRAP);
    CHECK(idleFences == 0 && fencesDestroyed == 1, "closed, then renderer gone: %d fences triggered, %d destroyed (want 0, 1)",
          idleFences, fencesDestroyed);
    clean("closed, then renderer gone", &a, 0);
    completed = a.copySerial;

    /* out of memory reserving its retired copy: no GPU copy scheduled - copied as without one */
    reset();
    outOfMemory = true;
    {
        uint64_t serials = nextSerial;
        v = presentation(&a);
        CHECK(nextSerial == serials, "out of memory reserving: %d GPU copies scheduled", (int) (nextSerial - serials));
    }
    expect("out of memory reserving its retired copy", "cpu-copy+ idle+ complete(copy)+", -1);
    clean("out of memory reserving", &a, 0);

    /* out of memory once the copy is scheduled: overwritten while copying, or its window destroyed - retired all the
     * same, into the copy reserved before (nothing allocated), let go once it is done */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    outOfMemory = true;
    {
        int calls = allocCalls;
        presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
        present_execute(v, 0, 11);
        CHECK(allocCalls == calls && a.refs == 2 && retiredMade == retiredReleased + 1, "out of memory, overwritten: %d allocations, "
              "%d references, %d retired copies held", allocCalls - calls, a.refs, retiredMade - retiredReleased);
        gpuDone(a.copySerial);
        CHECK(allocCalls == calls, "out of memory, overwritten: %d allocations letting go", allocCalls - calls);
    }
    expect("out of memory, overwritten while copying (scrap)", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    clean("out of memory, overwritten", &a, 0);

    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    idleFences = fencesDestroyed = 0;
    outOfMemory = true;
    {
        int calls = allocCalls;
        present_free_window_vblank(&window);
        CHECK(allocCalls == calls && !trail[0] && a.refs == 2 && retiredMade == retiredReleased + 1, "out of memory, window destroyed: "
              "%d allocations, %s, %d references, %d retired copies held", allocCalls - calls, trail, a.refs, retiredMade - retiredReleased);
        gpuDone(a.copySerial);
    }
    expect("out of memory, its window destroyed while copying (destroy)", "ack+", LORIE_X_EARLY_DESTROY);
    CHECK(idleFences == 0 && fencesDestroyed == 1, "out of memory, window destroyed: %d fences triggered, %d destroyed (want 0, 1)",
          idleFences, fencesDestroyed);
    clean("out of memory, window destroyed", &a, 0);

    /* its first poll not queued: still waited for - the completion recheck (every event and tick) finds it done */
    reset();
    queueVblankFails = 1;
    current = &a;
    {
        int polls = queued;
        struct present_vblank *w = calloc(1, sizeof *w);
        allocs++;
        w->screen = &screen; w->window = &window; w->pixmap = &a; a.refs++; w->crtc = &crtcRec;
        w->kind = PresentCompleteKindPixmap; w->target_msc = w->exec_msc = 10;
        w->idle_fence = calloc(1, sizeof *w->idle_fence);
        xorg_list_init(&w->window_list); xorg_list_init(&w->event_queue);
        xorg_list_append(&w->window_list, &windowPriv.vblank); xorg_list_append(&w->event_queue, &present_exec_queue);
        w->queued = TRUE;
        present_execute(w, 0, 10);
        CHECK(!trail[0] && w->gpu_copy_pending && w->queued && queued == polls, "first poll not queued: %s, pending %d, queued %d, %d polls",
              trail, w->gpu_copy_pending, w->queued, queued - polls);
        lorieRecheckGpuCopies();            /* a tick, still copying: nothing */
        gpuDone(a.copySerial);
    }
    expect("its first poll not queued", "ack+ idle+ complete(copy)+", LORIE_X_EARLY_POLL_FIRST);
    clean("first poll not queued", &a, 0);

    /* a later poll not queued, still copying: still waited for */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    queueVblankFails = 1;
    present_execute(v, 0, 11);              /* its poll, the copy still running, the next not queued */
    CHECK(!trail[0] && v->gpu_copy_pending, "later poll not queued: %s", trail);
    gpuDone(a.copySerial);
    expect("a later poll not queued", "ack+ idle+ complete(copy)+", LORIE_X_EARLY_POLL_LATER);
    clean("later poll not queued", &a, 0);

    /* the renderer without a surface, its copy running (it still runs the queued ones): waited for - one poll at a
     * time, the recheck queuing none - and presented once done */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    rendererAvailable = false;
    {
        int polls = queued;
        lorieRecheckGpuCopies();            /* its surface lost (notified) */
        present_execute(v, 0, 11);          /* its poll */
        lorieRecheckGpuCopies();            /* a tick */
        CHECK(!trail[0] && v->gpu_copy_pending && queued - polls == 1, "no surface, running: %s, %d polls queued (want 1)", trail,
              queued - polls);
    }
    gpuDone(a.copySerial);
    expect("renderer without a surface, its copy running", "ack+ idle+ complete(copy)+", LORIE_X_EARLY_STALLED);
    clean("no surface, running", &a, 0);

    /* without a surface, not recorded: cancelled, the presentation skipped, its buffer idle; the renderer then skipping
     * the entry (its serial completed) tells nothing more */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    rendererAvailable = false;
    lorieRecheckGpuCopies();
    CHECK(isCancelled(a.copySerial), "no surface, not recorded: not cancelled");
    gpuDone(a.copySerial);                  /* the renderer consumes the cancelled entry */
    expect("renderer without a surface, its copy not recorded", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_STALLED);
    clean("no surface, not recorded", &a, 0);

    /* disconnected: running with a renderer process that exists - waited for; recorded by one that no longer
     * exists - settled, skipped; not recorded - cancelled, skipped */
    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    connectionAlive = false;
    lorieRecheckGpuCopies();
    CHECK(!trail[0], "disconnected, running: %s", trail);
    connectionAlive = true;
    gpuDone(a.copySerial);
    expect("renderer disconnected, its copy running with the process alive", "ack+ idle+ complete(copy)+", LORIE_X_EARLY_STALLED);
    clean("disconnected, running", &a, 0);

    reset();
    v = presentation(&a);
    recorded = a.copySerial;
    trail[0] = 0;
    connectionAlive = false;
    submitterGone = true;
    lorieRecheckGpuCopies();
    expect("renderer disconnected, its copy recorded by a process gone", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_STALLED);
    clean("disconnected, renderer gone", &a, 0);

    reset();
    v = presentation(&a);
    trail[0] = 0;
    connectionAlive = false;
    lorieRecheckGpuCopies();
    expect("renderer disconnected, its copy not recorded", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_STALLED);
    clean("disconnected, not recorded", &a, 0);

    /* stalled with the shared lock held by the renderer: not known yet - asked again at the next tick, settled then */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    rendererAvailable = false;
    lockBusy = true;
    lorieRecheckGpuCopies();
    CHECK(!trail[0] && v->gpu_copy_pending, "lock busy: %s", trail);
    lockBusy = false;
    lorieRecheckGpuCopies();
    expect("renderer without a surface, the shared lock busy, then free", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_STALLED);
    clean("lock busy", &a, 0);

    /* P11-C3-D1-A: the copy's damage - what it draws: the presented pixmap (800x600; or its update region, in its own
     * coordinates) at x_off/y_off in the window, which is at (100,50) inside a redirected frame whose pixmap (804x604,
     * a 2 pixel border) is at (98,48) on the screen; the region the copy draws there, within that pixmap, on the screen.
     * Off: reported as the copy is queued (the GPU may still write), once. */
    window.drawable.x = 100, window.drawable.y = 50;
    window.clipList = (RegionRec) { 1, 100, 50, 900, 650 };
    a.drawable.width = b.drawable.width = 800, a.drawable.height = b.drawable.height = 600;
    dstPixmap.screen_x = 98, dstPixmap.screen_y = 48, dstPixmap.drawable.width = 804, dstPixmap.drawable.height = 604;
    reset();
    dreset();
    v = presentation(&a);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)!") && !v->lorie_damage_deferred, "off: not damaged as the copy is queued: %s", dtrail);
    CHECK(damagedTold == 1 && !damagedUntold && damagedSerial == a.copySerial, "off: the content diagnostics told of the damage "
          "%d times (%d without it), serial %llu", damagedTold, damagedUntold, (unsigned long long) damagedSerial);
    gpuDone(a.copySerial);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)!") && !damageDeferred && !damageHow[0] && !damageHow[1] && !damageHow[2] &&
          !regionsLive, "off: damaged again, or counted, or a region left (%d): %s", regionsLive, dtrail);
    expect("damage, off: as the copy is queued", "ack+ idle+ complete(copy)+", -1);
    clean("damage, off", &a, 0);

    /* off, an update region: in the presented pixmap's coordinates - moved into the window by x_off/y_off, where the copy
     * draws it, not taken as the screen's (which put it outside the window: no damage) - within the frame's pixmap; the
     * update region destroyed once; out of memory copying it, the window's whole clip */
    reset();
    dreset();
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 10 }, nextXOff = 5, nextYOff = 6;
    v = presentation(&a);
    CHECK(!strcmp(dtrail, "damage(105,56,115,66)!") && regionsDestroyed == 1 && !regionsLive, "off, update: %s, its region destroyed "
          "%d times, %d left (want damage(105,56,115,66)!, once, none)", dtrail, regionsDestroyed, regionsLive);
    gpuDone(a.copySerial);
    expect("damage, off, an update region: where the copy draws it", "ack+ idle+ complete(copy)+", -1);
    clean("damage, off, update", &a, 0);
    reset();
    dreset();
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 700 };
    v = presentation(&a);
    CHECK(!strcmp(dtrail, "damage(100,50,110,652)!"), "off, update past the frame's pixmap: %s (want damage(100,50,110,652)!)", dtrail);
    gpuDone(a.copySerial);
    clean("damage, off, update past the pixmap", &a, 0);
    reset();
    dreset();
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 10 };
    regionCopyFails = true;
    v = presentation(&a);
    regionCopyFails = false;
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)!") && !regionsLive, "off, out of memory: %s (want the clip)", dtrail);
    gpuDone(a.copySerial);
    clean("damage, off, out of memory", &a, 0);

    /* on: none as it is queued, nor while it runs; once it is done, before the client is told its pixmap is idle */
    deferDamage = true;
    reset();
    dreset();
    v = presentation(&a);
    recorded = a.copySerial;
    lorieRecheckGpuCopies();                /* a tick, still copying */
    present_execute(v, 0, 11);              /* its poll, still copying */
    CHECK(!dtrail[0] && v->lorie_damage_deferred && damageDeferred == 1 && !damagedTold, "on: damaged before the copy is done: %s "
          "(told %d)", dtrail, damagedTold);
    gpuDone(a.copySerial);
    CHECK(damagedTold == 1 && !damagedUntold && damagedSerial == a.copySerial, "on: the content diagnostics told of the damage "
          "%d times (%d without it)", damagedTold, damagedUntold);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)+") && damageBeforeIdle && damageHow[LORIE_COPY_DAMAGE_DONE] == 1 &&
          !damageHow[LORIE_COPY_DAMAGE_EARLY] && !damageHow[LORIE_COPY_DAMAGE_DROPPED], "on: not damaged once the copy is done, "
          "before idle (%d): %s", damageBeforeIdle, dtrail);
    expect("damage, on: once the copy is done", "ack+ idle+ complete(copy)+", -1);
    clean("damage, on", &a, 0);

    /* on, an update region, the frame moved 100 to the right meanwhile (the window and its pixmap with it): where the
     * copy drew it, which moved with them - within the pixmap; destroyed once */
    reset();
    dreset();
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 700 }, nextXOff = 5, nextYOff = 6;
    v = presentation(&a);
    CHECK(!dtrail[0] && v->update == &nextUpdate && !regionsDestroyed, "on, update: damaged, or its region gone, as it is queued: %s",
          dtrail);
    window.drawable.x = 200, window.clipList = (RegionRec) { 1, 200, 50, 1000, 650 }, dstPixmap.screen_x = 198;
    gpuDone(a.copySerial);
    CHECK(!strcmp(dtrail, "damage(205,56,215,652)+") && regionsDestroyed == 1 && !regionsLive, "on, update, the frame moved: %s, its "
          "region destroyed %d times (want damage(205,56,215,652)+, once)", dtrail, regionsDestroyed);
    expect("damage, on, an update region, the frame moved meanwhile", "ack+ idle+ complete(copy)+", -1);
    clean("damage, on, update", &a, 0);
    /* on, the window moved within its frame meanwhile (its pixmap did not move): where the copy drew it, in that pixmap -
     * not where the window is now */
    reset();
    dreset();
    window.drawable.x = 100, window.clipList = (RegionRec) { 1, 100, 50, 900, 650 }, dstPixmap.screen_x = 98;
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 10 }, nextXOff = 5, nextYOff = 6;
    v = presentation(&a);
    window.drawable.x = 150, window.clipList = (RegionRec) { 1, 150, 50, 850, 650 };
    gpuDone(a.copySerial);
    CHECK(!strcmp(dtrail, "damage(105,56,115,66)+"), "on, moved within its frame: %s (want damage(105,56,115,66)+)", dtrail);
    clean("damage, on, moved within its frame", &a, 0);
    /* on, a window drawn into the screen pixmap (not redirected, at 0,0, 1920x1080), moved meanwhile: where the copy drew
     * it - the screen pixmap does not move */
    reset();
    dreset();
    window.drawable.x = 100;
    dstPixmap.screen_x = dstPixmap.screen_y = 0, dstPixmap.drawable.width = 1920, dstPixmap.drawable.height = 1080;
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 10 }, nextXOff = 5, nextYOff = 6;
    v = presentation(&a);
    window.drawable.x = 300, window.clipList = (RegionRec) { 1, 300, 50, 1100, 650 };
    gpuDone(a.copySerial);
    CHECK(!strcmp(dtrail, "damage(105,56,115,66)+"), "on, into the screen pixmap, moved: %s (want damage(105,56,115,66)+)", dtrail);
    clean("damage, on, screen pixmap", &a, 0);
    window.drawable.x = 100, window.clipList = (RegionRec) { 1, 100, 50, 900, 650 };
    dstPixmap.screen_x = 98, dstPixmap.screen_y = 48, dstPixmap.drawable.width = 804, dstPixmap.drawable.height = 604;

    /* on, overwritten in its frame while the copy runs: damaged then, as off (the GPU may still write), not again */
    reset();
    dreset();
    v = presentation(&a);
    recorded = a.copySerial;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)!") && damageHow[LORIE_COPY_DAMAGE_EARLY] == 1 && damagedTold == 1 &&
          damagedSerial == a.copySerial, "on, overwritten while copying: %s (told %d)", dtrail, damagedTold);
    present_execute(v, 0, 11);
    gpuDone(a.copySerial);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)!") && damageHow[LORIE_COPY_DAMAGE_EARLY] == 1 && !damageHow[LORIE_COPY_DAMAGE_DONE],
          "on, overwritten while copying: damaged again: %s", dtrail);
    expect("damage, on, overwritten while copying: then (scrap)", "complete(skip)! ack+ idle+", LORIE_X_EARLY_SCRAP);
    clean("damage, on, overwritten while copying", &a, 0);

    /* on, overwritten once the copy is done, not taken up yet: damaged then, done */
    reset();
    dreset();
    v = presentation(&a);
    recorded = completed = a.copySerial;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    CHECK(!strcmp(dtrail, "damage(100,50,900,650)+") && damageHow[LORIE_COPY_DAMAGE_DONE] == 1, "on, overwritten, copy done: %s", dtrail);
    expect("damage, on, overwritten once the copy is done", "ack+ idle+ complete(skip)+", -1);
    clean("damage, on, overwritten, done", &a, 0);

    /* on, overwritten before the renderer recorded it (cancelled), or its renderer stalled before it did: never damaged */
    reset();
    dreset();
    v = presentation(&a);
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    CHECK(!dtrail[0] && damageHow[LORIE_COPY_DAMAGE_DROPPED] == 1 && !damagedTold, "on, cancelled: %s (told %d)", dtrail, damagedTold);
    expect("damage, on, overwritten before it was recorded: none (scrap)", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_SCRAP);
    clean("damage, on, cancelled", &a, 0);
    reset();
    dreset();
    v = presentation(&a);
    rendererAvailable = false;
    lorieRecheckGpuCopies();
    gpuDone(a.copySerial);
    CHECK(!dtrail[0] && damageHow[LORIE_COPY_DAMAGE_DROPPED] == 1 && !v->lorie_damage_deferred, "on, stalled, not recorded: %s", dtrail);
    expect("damage, on, its renderer stalled before recording it: none", "ack+ idle+ complete(skip)+", LORIE_X_EARLY_STALLED);
    clean("damage, on, stalled", &a, 0);

    /* on, its window destroyed while the copy runs: never damaged, its update region destroyed once */
    reset();
    dreset();
    nextUpdate = (RegionRec) { 1, 0, 0, 10, 10 };
    v = presentation(&a);
    recorded = a.copySerial;
    present_free_window_vblank(&window);
    gpuDone(a.copySerial);
    CHECK(!dtrail[0] && damageHow[LORIE_COPY_DAMAGE_DROPPED] == 1 && regionsDestroyed == 1, "on, window destroyed: %s, its region "
          "destroyed %d times", dtrail, regionsDestroyed);
    expect("damage, on, its window destroyed while copying: none (destroy)", "ack+", LORIE_X_EARLY_DESTROY);
    clean("damage, on, window destroyed", &a, 0);

    /* on, copied by the CPU (out of memory reserving the retired copy): nothing put off */
    reset();
    dreset();
    outOfMemory = true;
    v = presentation(&a);
    outOfMemory = false;
    CHECK(!dtrail[0] && !damageDeferred, "on, CPU copy: %s, %d put off", dtrail, damageDeferred);
    expect("damage, on, copied by the CPU: nothing put off", "cpu-copy+ idle+ complete(copy)+", -1);
    clean("damage, on, CPU copy", &a, 0);
    deferDamage = false;
    window.drawable.x = window.drawable.y = 0;
    window.clipList = (RegionRec) { 0 };

    /* P11-C3-D0: a followed presentation's content - its GPU copy queued, done, then its CompleteNotify; copied by the CPU;
     * overwritten in its frame by the next one, which is then copied; nothing for one not followed */
    reset();
    ctrail[0] = 0;
    contentSeqNext = 5;
    v = presentation(&a);
    gpuDone(a.copySerial);
    CHECK(!strcmp(ctrail, "copied(5,gpu) done(5) complete(5,copy)"), "content, GPU copy: %s", ctrail);
    clean("content, GPU copy", &a, 0);
    reset();
    ctrail[0] = 0;
    contentSeqNext = 6;
    outOfMemory = true;
    v = presentation(&a);
    outOfMemory = false;
    CHECK(!strcmp(ctrail, "copied(6,cpu) complete(6,copy)"), "content, CPU copy: %s", ctrail);
    clean("content, CPU copy", &a, 0);
    reset();
    ctrail[0] = 0;
    contentSeqNext = 7;
    v = presentation(&a);
    recorded = a.copySerial;
    contentSeqNext = 8;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    present_execute(v, 0, 11);
    gpuDone(a.copySerial);
    CHECK(!strncmp(ctrail, "copied(7,gpu) skipped(7)", strlen("copied(7,gpu) skipped(7)")) && !strstr(ctrail, "done(7)") &&
          strstr(ctrail, "complete(7,skip)"), "content, overwritten while its copy runs: %s", ctrail);
    clean("content, overwritten", &a, 0);
    reset();
    ctrail[0] = 0;
    contentSeqNext = 0;
    v = presentation(&a);
    gpuDone(a.copySerial);
    CHECK(!ctrail[0], "content not followed: %s", ctrail);
    clean("content not followed", &a, 0);

    printf("TPRESENTSAFETY a GPU-copied presentation's end, on the patched xserver: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
