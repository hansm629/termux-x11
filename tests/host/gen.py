#!/usr/bin/env python3
"""Writes each test's *_src.inc from the sources as they are now, so the tests exercise the real code.

usage: gen.py <lorie source dir> <output dir>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract import func, macro, span

L, OUT = sys.argv[1], sys.argv[2]
I, R, H, C = L + "/InitOutput.c", L + "/renderer.c", L + "/lorie.h", L + "/cmdentrypoint.c"

def opt(make, marker, path=I):
    """What make() extracts, if the sources have `marker` - so a recipe also builds against code from
    before a function existed, which is how a test is shown to fail there."""
    return make() if marker in open(path, encoding="utf-8").read() else ""

recipes = {
    "t07": lambda: macro(H, "LORIE_GPU_COPY_FAILED_SLOTS") + macro(H, "LORIE_GPU_COPY_QUEUE_CAPACITY")
        + func(R, "static void rendererPublishFailedSerial(uint64_t serial) {")
        + opt(lambda: "#define HAVE_SESSION_LIST 1\n", "static void lorieSessionsReclaim")
        + opt(lambda: macro(H, "LORIE_RETIRE_RECORDS") + "#define HAVE_SESSIONS 1\n"
            + span(I, "static uint32_t lorieRendererSession;", "    lorieRetiredSeen = n;\n}"),
            "LorieRendererSessionRec")
        + func(I, "static Bool lorieGpuCopyKnownNotMade(uint64_t serial) {")
        + opt(lambda: span(I, "static void lorieSnapshotDeadSession(LorieRendererSessionRec *s) {", "} LorieCopyResolution;")
            + func(I, "static LorieCopyResolution lorieCopyResolve(uint64_t serial, Bool *made) {"),
            "LorieRendererSessionRec")
        + func(I, "Bool lorieGpuCopyMade(uint64_t serial) {")
        + func(I, "Bool lorieGpuCopyResolved(uint64_t serial) {"),
    "slots": lambda: macro(H, "LORIE_ROOT_SLOTS") + macro(H, "LORIE_ROOT_NEWEST_SHIFT") + macro(H, "LORIE_ROOT_NEWEST_MASK")
        + opt(lambda: macro(H, "LORIE_ROOT_GEN_SHIFT") + macro(H, "LORIE_ROOT_GEN"), "#define LORIE_ROOT_GEN_SHIFT", H)
        + "static int rendererRootSlot = -1;\nstatic uint64_t rendererRootSlotId = 0;\n"
        + "static uint32_t rendererRootSlotGen __attribute__((unused)) = 0;\n"
        + func(R, "static uint64_t rendererClaimRootBuffer(void) {")
        + func(R, "static void rendererReleaseRootSlot(int slot, uint64_t bufferId")
        + func(R, "static void rendererReleaseRootBuffer(void) {"),
    "t21": lambda: span(I, "#define LORIE_VSYNC_RECORDS 16", "static uint64_t lorieVsyncPeriodUs = 16667;")
        + opt(lambda: "#define HAVE_VSYNC_SEQ 1\n" + func(I, "static Bool lorieReadVsyncRecord(uint32_t idx, uint64_t *us) {"),
              "static Bool lorieReadVsyncRecord")
        + func(I, "static uint32_t lorieAdvanceVsyncClock(void) {"),
    # the same, with a ring of 4 so the producer laps it all the time
    "t29": lambda: (span(I, "#define LORIE_VSYNC_RECORDS 16", "static uint64_t lorieVsyncPeriodUs = 16667;")
        + opt(lambda: "#define HAVE_VSYNC_SEQ 1\n" + func(I, "static Bool lorieReadVsyncRecord(uint32_t idx, uint64_t *us) {"),
              "static Bool lorieReadVsyncRecord")
        + func(I, "static uint32_t lorieAdvanceVsyncClock(void) {")).replace(
            "#define LORIE_VSYNC_RECORDS 16", "#define LORIE_VSYNC_RECORDS 4"),
    "tstat": lambda: macro(H, "LORIE_STAT_MAX"),
    "root": lambda: macro(H, "LORIE_ROOT_SLOTS") + macro(H, "LORIE_ROOT_HELD_MASK") + macro(H, "LORIE_ROOT_NEWEST_SHIFT")
        + macro(H, "LORIE_ROOT_NEWEST_MASK") + macro(H, "LORIE_ROOT_COUNT_STEP") + macro(H, "LORIE_GPU_COPY_QUEUE_CAPACITY")
        + opt(lambda: macro(H, "LORIE_ROOT_COUNT_MASK"), "#define LORIE_ROOT_COUNT_MASK", H)
        + opt(lambda: macro(I, "LORIE_ROOT_REPLACEMENTS") + "#define HAVE_REPLACING 1\n"
            + span(I, "typedef struct {\n    uint64_t serial;\n    RegionRec region;", "} LorieRootCopyMark;"),
            "#define LORIE_ROOT_REPLACEMENTS")
        + span(I, "typedef struct {\n    LorieBuffer *buffer;", "} LoriePixmapPriv;")
        + "static Bool lorieRepairRootOwed(LoriePixmapPriv *priv);\n"
        + opt(lambda: "#define HAVE_PINS 1\n", "static uint32_t lorieRootPinnedSlots")
        + func(I, "static void lorieCopyRootRegion(LoriePixmapPriv *priv, int from, int to, RegionPtr region) {")
        + func(I, "static void lorieMarkRootStale(LoriePixmapPriv *priv, RegionPtr region) {")
        + opt(lambda: func(I, "static void lorieRootCpuDrawn(LoriePixmapPriv *priv, RegionPtr region) {")
            + func(I, "static void lorieRootSettleReplacements(LoriePixmapPriv *priv) {")
            + func(I, "static Bool lorieRootCanQueueCopy(LoriePixmapPriv *priv) {")
            + func(I, "static void lorieRootNoteGpuCopy(LoriePixmapPriv *priv, RegionPtr r, uint64_t serial) {")
            + func(I, "static void lorieRootFetchOwed(LoriePixmapPriv *priv, RegionPtr area, int slot, uint32_t epoch) {"),
            "#define LORIE_ROOT_REPLACEMENTS")
        + func(I, "static Bool lorieRepairRootOwed(LoriePixmapPriv *priv) {")
        + opt(lambda: func(I, "static uint32_t lorieRootPinnedSlots(LoriePixmapPriv *priv) {"),
            "static uint32_t lorieRootPinnedSlots")
        + opt(lambda: func(I, "static void lorieRootKeepConditional(LoriePixmapPriv *priv, int slot) {")
            + func(I, "static void lorieRootReuseSlot(LoriePixmapPriv *priv, int slot) {")
            + func(I, "static void lorieRootCopyCancelled(uint64_t serial) {"),
            "#define LORIE_ROOT_REPLACEMENTS")
        + func(I, "static RegionPtr lorieRootPendingGpuRegion(LoriePixmapPriv *priv, int slot) {")
        + func(I, "static Bool lorieRootHandover(LoriePixmapPriv *priv) {")
        + func(I, "static void lorieNoteRootPublished(LoriePixmapPriv *priv) {"),
    "t10": lambda: macro(H, "LORIE_GPU_COPY_QUEUE_CAPACITY")
        + span(I, "typedef struct {\n    struct xorg_list link;    /* only while waiting to be reaped */", "} LorieAbandonedCopy;")
        + span(I, "#define LORIE_COPY_RECORDS", "static LorieAbandonedCopy lorieCopyRecords[LORIE_COPY_RECORDS];")
        + func(I, "static LorieAbandonedCopy *lorieTakeCopyRecord(void) {")
        + func(I, "static void lorieGiveBackCopyRecord(LorieAbandonedCopy *c) {"),
    "t25_src_types": lambda: macro(H, "LORIE_GPU_COPY_MAX_RECTS") + macro(H, "LORIE_GPU_COPY_QUEUE_CAPACITY")
        + span(H, "typedef struct { int16_t x1, y1, x2, y2; } LorieGpuCopyRect;", "} LorieGpuCopyRect;")
        + span(H, "typedef struct {\n    uint64_t serial;\n    uint64_t srcBufferId;", "} LorieGpuCopyEntry;")
        + span(H, "enum { LORIE_JOB_QUEUED = 0", "};"),
    "t25_src_funcs": lambda: func(I, "static Bool lorieCancelQueuedEntry(uint32_t slot) {")
        + func(I, "static Bool lorieEntryTouches(const LorieGpuCopyEntry *e, int16_t dx, int16_t dy, RegionPtr region) {")
        + func(I, "static void lorieCancelConflictingCopies(uint64_t bufferId, RegionPtr region) {")
        + func(R, "static bool rendererClaimEntry(uint32_t slot) {"),
    "session": lambda: macro(H, "LORIE_GPU_COPY_FAILED_SLOTS") + macro(H, "LORIE_GPU_COPY_QUEUE_CAPACITY")
        + opt(lambda: macro(H, "LORIE_RETIRE_RECORDS") + "#define HAVE_SESSIONS 1\n"
            + span(I, "static uint32_t lorieRendererSession;", "    lorieRetiredSeen = n;\n}"),
            "LorieRendererSessionRec")
        + opt(lambda: span(I, "static uint32_t lorieRendererSession;", "#define LORIE_LOST_SESSION_SETTLE_US (2 * 1000 * 1000ULL)"),
            "uint64_t settleByUs;")
        + func(I, "static Bool lorieGpuCopyKnownNotMade(uint64_t serial) {")
        + opt(lambda: span(I, "static void lorieSnapshotDeadSession(LorieRendererSessionRec *s) {",
                           "} LorieCopyResolution;")
            + func(I, "static LorieCopyResolution lorieCopyResolve(uint64_t serial, Bool *made) {"),
            "LorieRendererSessionRec")
        + func(I, "Bool lorieGpuCopyMade(uint64_t serial) {")
        + func(I, "Bool lorieGpuCopyResolved(uint64_t serial) {")
        + func(I, "static Bool lorieCopySettled(LorieAbandonedCopy *c) {")
        + opt(lambda: func(I, "static void lorieEndSession(LorieRendererSessionRec *s, const char *why) {"),
            "static void lorieEndSession")
        + opt(lambda: "#define HAVE_SESSION_LIST 1\n" + func(I, "static Bool lorieRememberUnmade(uint64_t from, uint64_t to) {")
            + func(I, "static void lorieSessionsReclaim(void) {")
            + func(I, "static void lorieGiveBackCopyRecord(LorieAbandonedCopy *c) {"),
            "static void lorieSessionsReclaim")
        + opt(lambda: func(I, "static void lorieMarkSessionOver(uint32_t session, uint64_t settleByUs, const char *why) {"),
            "uint64_t settleByUs;")
        + func(I, "void lorieNoteRendererLost(void) {")
        + func(I, "void lorieNoteRendererConnected(")
        + opt(lambda: "static uint32_t rendererSessionTag = 0;\n"
            + func(R, "static void rendererSayRetired(struct lorie_shared_server_state *st) {"),
            "static void rendererSayRetired", R),
    "cursor": lambda: opt(lambda: span(R, "#define LORIE_CURSOR_BUFFERS 4", "static bool cursorOverlayRenderPending = false;")
        + span(R, "typedef enum { LORIE_ZC_FENCE_DONE, LORIE_ZC_FENCE_WAITING, LORIE_ZC_FENCE_UNUSABLE } LorieZcFence;",
               "} LorieZcFence;")
        + func(R, "static LorieZcFence rootZcFenceState(int fd) {")
        + func(R, "static int cursorPoolTake(void) {")
        + func(R, "static void cursorPoolHand(int slot) {")
        + func(R, "static uint32_t cursorPoolSetOnLayer(int slot) {")
        + func(R, "static void cursorPoolReleaseReported(uint32_t seq, int fd, bool reportCarriedLayer) {")
        + func(R, "static void cursorPoolDrain(void) {")
        + func(R, "static void cursorPoolOrphan(void) {"), "#define LORIE_CURSOR_BUFFERS", R),
    "t32_types": lambda: span(H, "typedef enum {\n    EVENT_UNKNOWN", "} lorieEvent;"),
    "t32": lambda: opt(lambda: span(C, "static struct LorieConnection {", "} lorieConnections[8];")
        + func(C, "static struct LorieConnection *lorieConnectionOf(uint32_t session) {")
        + func(C, "static void lorieForgetRegisteredBuffers(void) {")
        + func(C, "static void lorieConnectionClose(struct LorieConnection *c) {")
        + func(C, "static Bool handleRendererLostEvent(__unused ClientPtr pClient, void *closure) {")
        + func(C, "static void lorieConnectionHungUp(int fd, void *session) {")
        + span(C, "static struct { int fd; int32_t pid; } lorieNewConnections[4];", "static uint32_t lorieNewConnectionNext;")
        + func(C, "void lorieSendSharedServerState(int memfd) {")
        + func(C, "void lorieRegisterBuffer(LorieBuffer* buffer) {")
        + func(C, "void lorieUnregisterBuffer(LorieBuffer* buffer) {")
        + span(I, "typedef struct {\n    struct xorg_list link;    /* only while waiting to be reaped */", "} LorieAbandonedCopy;")
        + span(I, "#define LORIE_COPY_RECORDS", "static LorieAbandonedCopy lorieCopyRecords[LORIE_COPY_RECORDS];")
        + func(I, "static void lorieRegisterQueuedCopyBuffers(void) {")
        + func(I, "void lorieActivityConnected(void) {")
        + func(C, "static Bool addFd(__unused ClientPtr pClient, void *closure) {"),
        "static struct LorieConnection", C)
        # code from before the connection table: the hangup was handled inline in handleLorieEvents
        + opt(lambda: "#define HAVE_OLD_CONNECTIONS 1\n"
            + func(C, "static Bool handleRendererLostEvent(__unused ClientPtr pClient, void *closure) {")
            + "static void lorieConnectionHungUp(int fd, void *data) {\n    int ready = X_NOTIFY_ERROR;\n    (void) data;\n"
            + span(C, "    if (ready & X_NOTIFY_ERROR) {", "        return;\n    }") + "}\n"
            + span(C, "static struct { int fd; int32_t pid; } lorieNewConnections[4];", "static uint32_t lorieNewConnectionNext;")
            + func(C, "void lorieSendSharedServerState(int memfd) {")
            + func(C, "void lorieRegisterBuffer(LorieBuffer* buffer) {")
            + func(C, "void lorieUnregisterBuffer(LorieBuffer* buffer) {")
            + span(I, "typedef struct {\n    struct xorg_list link;    /* only while waiting to be reaped */", "} LorieAbandonedCopy;")
            + span(I, "#define LORIE_COPY_RECORDS", "static LorieAbandonedCopy lorieCopyRecords[LORIE_COPY_RECORDS];")
            + func(I, "void lorieActivityConnected(void) {")
            + func(C, "static Bool addFd(__unused ClientPtr pClient, void *closure) {")
            if "static struct LorieConnection" not in open(C, encoding="utf-8").read() else "",
            "lorieNewConnections", C)
        + opt(lambda: func(I, "static void lorieReleaseCopyBuffer(LorieBuffer *buffer) {"), "static void lorieReleaseCopyBuffer")
        + func(I, "static void lorieReleaseCopyResources(LorieBuffer *src, LorieBuffer *dst) {"),
    "tlogcat": lambda: func(C, "void* logcatThread(void *arg) {"),
    "ttrace_src_types": lambda: span(H, "typedef struct {\n    volatile uint64_t seq;", "#define LORIE_TRACE_RECORDS 4096")
        + "struct lorie_shared_server_state { volatile uint8_t traceEnabled; volatile uint64_t traceHead;"
          " volatile uint64_t traceTail; volatile uint32_t traceDropped; LorieTraceRecord trace[LORIE_TRACE_RECORDS]; };\n"
        + func(H, "static inline __always_inline uint64_t lorieTraceNowUs(void) {")
        + func(H, "static inline __always_inline void lorieTraceAt(struct lorie_shared_server_state *st, uint32_t kind,\n"
                  "                                                 uint32_t a, uint64_t b, uint64_t tUs) {"),
    "ttrace_src_funcs": lambda: "static FILE *lorieTraceFile = NULL;\n"
        + func(I, "static void lorieTraceFlush(Bool force) {"),
}
for name, make in recipes.items():
    with open(os.path.join(OUT, (name if name.endswith("_types") or name.endswith("_funcs") else name + "_src") + ".inc"), "w") as f:
        f.write(make())
