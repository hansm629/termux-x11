/* TPRESENTSAFETY (P10-S0): how a presentation whose copy was offloaded to the renderer's GPU ends, run on the
 * patched xserver's own code (patches/xserver.patch applied to a copy of present/, gen.py "presentsafety"):
 * present_execute, present_execute_copy, present_execute_post, present_re_execute, present_vblank_scrap,
 * present_vblank_destroy, present_free_window_vblank, lorieRecheckGpuCopies and present_scmd_pixmap's loop that
 * scraps a queued presentation a new one overwrites in the same frame. The rest of the server is stubbed; the lorie
 * hooks (lorieGpuCopyIsDone, lorieGpuCopyAck, ...) are recorders, with the GPU copy done or not as each case sets.
 *
 * Every case logs, in order, what reaches the client and the X server's references: the X server's references to
 * the source and destination dropped (lorieGpuCopyAck), the client told its pixmap is idle (present_pixmap_idle),
 * the presentation completed (present_vblank_notify, with its mode) - each marked with whether the GPU copy was
 * done at that moment ("+" done, "!" not yet).
 *
 * The reference case is the one the patch is built for: the copy done, then the completion recheck - every step
 * after the copy. The others are the paths P10-S0 found that end the presentation while the copy may still read
 * the source: a new presentation for the same window and frame overwriting it (scrap), the window destroyed
 * (destroy), the poll re-queue failing (first and later), the renderer without a surface or disconnected (stalled).
 * Today each of them drops the references, and all but the window's destruction tell the client its buffer is
 * idle, before the copy is done - reproduced here (REPRODUCED). The test checks each sequence as it is now, so it
 * fails if any changes: the fix for P10-S0 has to turn these expectations around. Each early end is counted once,
 * by its kind, through lorieCountGpuCopyEarly (P10-B0); the reference case counts none. */
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
typedef struct { int n; } RegionRec, *RegionPtr;
typedef struct _Screen *ScreenPtr;
typedef struct { ScreenPtr pScreen; int16_t x, y; XID id; } DrawableRec;
typedef struct _Window { DrawableRec drawable; RegionRec clipList; struct present_window_priv *priv; } WindowRec, *WindowPtr;
typedef struct _Pixmap { DrawableRec drawable; int16_t screen_x, screen_y; int refs; uint64_t copySerial; } PixmapRec, *PixmapPtr;
typedef struct _RRCrtc { ScreenPtr pScreen; } *RRCrtcPtr;
typedef struct _Screen { WindowPtr root; PixmapPtr (*GetWindowPixmap)(WindowPtr); PixmapPtr (*GetScreenPixmap)(ScreenPtr); } ScreenRec;
typedef struct present_fence { int destroyed; } *present_fence_ptr;
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

#include "presentsafety_struct_src.inc"
typedef struct present_vblank *present_vblank_ptr;
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

/* ---- what happened, in order ---- */
static uint64_t completed;          /* the renderer's completedSerial */
static int queueVblankFails;        /* the next n queue_vblank calls fail */
static bool connectionAlive = true, rendererAvailable = true;
static char trail[512];
static void step(const char *what, bool done) {
    snprintf(trail + strlen(trail), sizeof trail - strlen(trail), "%s%s%s", trail[0] ? " " : "", what, done ? "+" : "!");
}
static uint64_t nextSerial;
static PixmapPtr current;           /* the presentation's pixmap, for the steps that no longer have it */
static bool doneFor(PixmapPtr p) { return p && completed >= p->copySerial; }

static Bool lorieTryScheduleGpuCopy(PixmapPtr pixmap, PixmapPtr dst, RegionPtr update, int16_t x_off, int16_t y_off,
                                    uint64_t *out_serial, void **out_dst_buffer) {
    (void) dst; (void) update; (void) x_off; (void) y_off;
    pixmap->copySerial = *out_serial = ++nextSerial;
    *out_dst_buffer = &dstPixmap;
    pixmap->refs++;                 /* the X server's extra reference (LorieBuffer_acquire), until the ack */
    return TRUE;
}
static Bool lorieGpuCopyIsDone(uint64_t serial) { return completed >= serial; }
static void lorieGpuCopyAck(PixmapPtr pixmap, void *dst_buffer) {
    (void) dst_buffer;
    pixmap->refs--;
    step("ack", doneFor(pixmap));
}
static bool lorieConnectionAlive(void) { return connectionAlive; }
/* P10-B0: the patch counts each early end, by kind (lorie.h LORIE_X_EARLY_*) */
static int early[LORIE_X_EARLY_COUNT];
static void lorieCountGpuCopyEarly(int how) { early[how]++; }
static bool lorieRendererAvailable(void) { return rendererAvailable; }

static present_screen_priv_ptr present_screen_priv(ScreenPtr s) { (void) s; return &screenPriv; }
static present_window_priv_ptr present_window_priv(WindowPtr w) { return w->priv; }
static present_window_priv_ptr present_get_window_priv(WindowPtr w, Bool create) { (void) create; return w->priv; }
static Bool present_execute_wait(struct present_vblank *v, uint64_t msc) { (void) v; (void) msc; return FALSE; }
static int present_get_ust_msc(ScreenPtr s, RRCrtcPtr c, uint64_t *ust, uint64_t *msc) { (void) s; (void) c; *ust = 0; *msc = 10; return Success; }
static Bool present_flip(RRCrtcPtr c, uint64_t id, uint64_t msc, PixmapPtr p, Bool sync) { (void) c; (void) id; (void) msc; (void) p; (void) sync; return FALSE; }
static void present_set_tree_pixmap(WindowPtr w, PixmapPtr e, PixmapPtr p) { (void) w; (void) e; (void) p; }
static void present_set_abort_flip(ScreenPtr s) { (void) s; }
static void present_unflip(ScreenPtr s) { (void) s; }
static void present_copy_region(DrawableRec *d, PixmapPtr p, RegionPtr r, int16_t x, int16_t y) { (void) d; (void) p; (void) r; (void) x; (void) y; step("cpu-copy", true); }
static void present_pixmap_idle(PixmapPtr p, WindowPtr w, uint32_t serial, present_fence_ptr f) { (void) w; (void) serial; (void) f; step("idle", doneFor(p)); }
static void present_fence_destroy(present_fence_ptr f) { if (f) f->destroyed = 1; }
static void dixDestroyPixmap(PixmapPtr p, XID id) { (void) id; p->refs--; }
static void present_destroy_notifies(present_notify_ptr n, int count) { (void) n; (void) count; }
static void present_vblank_notify(struct present_vblank *v, CARD16 kind, CARD16 mode, uint64_t ust, uint64_t msc) {
    (void) v; (void) kind; (void) ust; (void) msc;
    step(mode == PresentCompleteModeSkip ? "complete(skip)" : "complete(copy)", doneFor(current));
}
static void RegionDestroy(RegionPtr r) { (void) r; }
static void RegionIntersect(RegionPtr d, RegionPtr a, RegionPtr b) { (void) d; (void) a; (void) b; }
static void DamageDamageRegion(DrawableRec *d, RegionPtr r) { (void) d; (void) r; }
static int queueVblank(ScreenPtr s, WindowPtr w, RRCrtcPtr c, uint64_t id, uint64_t msc) {
    (void) s; (void) w; (void) c; (void) id; (void) msc;
    if (queueVblankFails > 0) {
        queueVblankFails--;
        return 1;
    }
    return Success;
}
static void flush(WindowPtr w) { (void) w; }
static void abortVblank(ScreenPtr s, WindowPtr w, RRCrtcPtr c, uint64_t id, uint64_t msc) { (void) s; (void) w; (void) c; (void) id; (void) msc; }
static PixmapPtr getWindowPixmap(WindowPtr w) { (void) w; return &dstPixmap; }

void present_execute_copy(struct present_vblank *vblank, uint64_t crtc_msc);
void present_execute_post(struct present_vblank *vblank, uint64_t ust, uint64_t crtc_msc);
void present_vblank_scrap(struct present_vblank *vblank);
void present_vblank_destroy(struct present_vblank *vblank);
static void present_execute(struct present_vblank *vblank, uint64_t ust, uint64_t crtc_msc);
#include "presentsafety_src.inc"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL " __VA_ARGS__); printf("\n"); } } while (0)

/* a client's presentation of `pixmap` to the window at msc 10, as present_scmd_pixmap queues and executes it */
static struct present_vblank *presentation(PixmapPtr pixmap) {
    struct present_vblank *v = calloc(1, sizeof *v);
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
    current = pixmap;
    present_execute(v, 0, 10);
    return v;
}

static void reset(void) {
    memset(early, 0, sizeof early);
    trail[0] = 0;
    completed = 0;
    queueVblankFails = 0;
    connectionAlive = rendererAvailable = true;
}

/* one case: what it logged, what P10-S0 found, and whether that is the client told before the copy was done; and
 * what the patch counted of it (P10-B0): that one kind once, or nothing */
static int reproduced;
static void expect(const char *what, const char *want, bool hazard, int kind) {
    int counted = 0;
    for (int k = 0; k < LORIE_X_EARLY_COUNT; k++)
        counted += early[k];
    CHECK(!strcmp(trail, want), "%s: %s (want %s)", what, trail, want);
    CHECK(kind < 0 ? counted == 0 : counted == 1 && early[kind] == 1, "%s: counted %d early ends (want %s)", what, counted,
          kind < 0 ? "none" : "one, of its kind");
    printf("  %-58s %s%s\n", what, trail, hazard ? "   REPRODUCED" : "");
    if (hazard && !strcmp(trail, want))
        reproduced++;
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

    /* the copy queued: the presentation waits for it, nothing told, nothing dropped */
    reset();
    v = presentation(&a);
    CHECK(v->gpu_copy_pending && v->queued && !trail[0] && a.refs == 2, "queued: pending %d queued %d, %s, %d references (want 2)",
          v->gpu_copy_pending, v->queued, trail, a.refs);

    /* the reference: the copy done, then the renderer's notification - everything after it */
    completed = a.copySerial;
    lorieRecheckGpuCopies();
    expect("copy done, then the completion recheck", "ack+ idle+ complete(copy)+", false, -1);
    CHECK(a.refs == 0, "copy done: %d references left", a.refs);

    /* a new presentation for the same window and frame, the copy still running: present_scmd_pixmap scraps it */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    presentOverwrite(&windowPriv, NULL, &b, &crtcRec, 10);
    expect("overwritten in the same frame (present_scmd_pixmap scrap)", "ack! idle!", true, LORIE_X_EARLY_SCRAP);
    CHECK(a.refs == 0, "overwritten: the X server holds %d references while the copy runs", a.refs);
    completed = a.copySerial;
    present_execute(v, 0, 11);      /* its msc+1 event: completes as skipped */
    xorg_list_init(&windowPriv.vblank);

    /* the window destroyed, the copy still running */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    present_free_window_vblank(&window);
    expect("window destroyed (present_free_window_vblank)", "ack!", true, LORIE_X_EARLY_DESTROY);
    CHECK(a.refs == 0, "window destroyed: %d references left", a.refs);
    xorg_list_init(&windowPriv.vblank);
    xorg_list_init(&present_exec_queue);

    /* the copy queued, but its poll cannot be queued */
    reset();
    queueVblankFails = 1;
    current = &a;
    {
        struct present_vblank *w = calloc(1, sizeof *w);
        w->screen = &screen; w->window = &window; w->pixmap = &a; a.refs++; w->crtc = &crtcRec;
        w->kind = PresentCompleteKindPixmap; w->target_msc = w->exec_msc = 10;
        w->idle_fence = calloc(1, sizeof *w->idle_fence);
        xorg_list_init(&w->window_list); xorg_list_init(&w->event_queue);
        xorg_list_append(&w->window_list, &windowPriv.vblank); xorg_list_append(&w->event_queue, &present_exec_queue);
        w->queued = TRUE;
        present_execute(w, 0, 10);
    }
    expect("its first poll not queued (present_execute_copy)", "ack! idle! complete(copy)!", true, LORIE_X_EARLY_POLL_FIRST);
    xorg_list_init(&windowPriv.vblank);
    xorg_list_init(&present_exec_queue);

    /* polled at the next frame, the copy still running, the poll cannot be queued again */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    queueVblankFails = 1;
    present_execute(v, 0, 11);
    expect("a later poll not queued (present_execute_copy)", "ack! idle! complete(copy)!", true, LORIE_X_EARLY_POLL_LATER);
    xorg_list_init(&windowPriv.vblank);
    xorg_list_init(&present_exec_queue);

    /* the renderer without a surface (the activity in the background) - it still runs queued copies - or
     * disconnected: the recheck gives the presentation up as skipped */
    reset();
    v = presentation(&a);
    trail[0] = 0;
    rendererAvailable = false;
    lorieRecheckGpuCopies();
    expect("renderer without a surface (stalled)", "ack! idle! complete(skip)!", true, LORIE_X_EARLY_STALLED);
    xorg_list_init(&windowPriv.vblank);
    xorg_list_init(&present_exec_queue);

    reset();
    v = presentation(&a);
    trail[0] = 0;
    connectionAlive = false;
    lorieRecheckGpuCopies();
    expect("renderer disconnected (stalled)", "ack! idle! complete(skip)!", true, LORIE_X_EARLY_STALLED);

    printf("TPRESENTSAFETY a GPU-copied presentation's end, on the patched xserver: %s (%d failures; %d of 6 known early "
           "releases reproduced, P10-S0)\n", fails ? "FAIL" : "PASS", fails, reproduced);
    return fails != 0;
}
