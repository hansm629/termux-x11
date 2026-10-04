#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma ide diagnostic ignored "bugprone-reserved-identifier"
#pragma ide diagnostic ignored "OCUnusedMacroInspection"
#pragma ide diagnostic ignored "EndlessLoop"
#define __USE_GNU
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <libgen.h>
#include <globals.h>
#include <xkbsrv.h>
#include <errno.h>
#include <inpututils.h>
#include <randrstr.h>
#include <linux/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "lorie.h"
extern void lorieSetMonitorResolution(int dpi);

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

static int argc = 0;
static char** argv = NULL;
__LIBC_HIDDEN__ volatile int conn_fd = -1; // The only variable shared with activity code.
extern DeviceIntPtr lorieMouse, lorieTouch, lorieKeyboard, loriePen, lorieEraser;
extern ScreenPtr pScreenPtr;
extern int ucs2keysym(long ucs);
void lorieKeysymKeyboardEvent(KeySym keysym, int down);

char *xtrans_unix_path_x11 = NULL;
char *xtrans_unix_dir_x11 = NULL;

/* The buffers whose handles have been sent to the renderer on the current connection - which a renderer
 * connecting afresh has none of. Emptied whenever a connection is made or the current one is lost. */
struct xorg_list registeredBuffers;

/*
 * Renderer connections, X server thread only. A connection is its socket and the session it was opened
 * as, and it is closed exactly once, by whichever comes first: its own hangup or a newer connection
 * replacing it.
 *
 * The hangup used to be handled with whatever conn_fd and session were current when it got to run.
 * The input thread set conn_fd to -1 and named lorieRendererSessionId() as the session lost - so the
 * old renderer's socket going away after a new renderer had connected cut the new one off and marked
 * its session over, leaving it with the shared state and none of the buffers.
 */
static struct LorieConnection {
    uint32_t session;
    int fd;
    Bool open;
} lorieConnections[8];

static void* startServer(__unused void* cookie) {
    char* envp[] = { NULL };
    exit(dix_main(argc, (char**) argv, envp));
}

static Bool detectTracer(void)
{
    FILE *fp;
    char  line[256];
    int pid = 0;

    fp = fopen("/proc/self/status", "r");
    if (!fp)
        return TRUE;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            sscanf(line+10, "%d", &pid);
            break;
        }
    }

    if (pid != 0)
        log(INFO, "Tracer detected");

    fclose(fp);
    return pid != 0;
}

JNIEXPORT jboolean JNICALL
Java_com_termux_x11_CmdEntryPoint_start(JNIEnv *env, __unused jclass cls, jobjectArray args) {
    pthread_t t;
    JavaVM* vm = NULL;
    // execv's argv array is a bit incompatible with Java's String[], so we do some converting here...
    argc = (*env)->GetArrayLength(env, args) + 1; // Leading executable path
    argv = (char**) calloc(argc, sizeof(char*));

    argv[0] = (char*) "Xlorie";
    for(int i=1; i<argc; i++) {
        jstring js = (jstring)((*env)->GetObjectArrayElement(env, args, i - 1));
        const char *pjc = (*env)->GetStringUTFChars(env, js, JNI_FALSE);
        argv[i] = (char *) calloc(strlen(pjc) + 1, sizeof(char)); //Extra char for the terminating NULL
        strcpy((char *) argv[i], pjc);
        (*env)->ReleaseStringUTFChars(env, js, pjc);
    }

    {
        cpu_set_t mask;
        long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

        // Without this the mask starts out as whatever was on the stack, so the X server ends up
        // allowed on "the upper half of the cores, plus a random set of the others" - a different
        // set on every launch, which can include the little cores.
        CPU_ZERO(&mask);

        for (int i = num_cpus/2; i < num_cpus; i++)
            CPU_SET(i, &mask);

        if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1)
            log(ERROR, "Failed to set process affinity: %s", strerror(errno));
        else {
            char list[128] = {0};
            int n = 0;
            for (int i = 0; i < num_cpus && n < (int) sizeof(list) - 4; i++)
                if (CPU_ISSET(i, &mask))
                    n += snprintf(list + n, sizeof(list) - n, "%s%d", n ? "," : "", i);
            log(INFO, "X server pinned to CPUs %s of %ld", list, num_cpus);
        }
    }

    // Which build this actually is. Telling a fresh APK from the one already installed has cost
    // more test rounds here than any single bug, and the stats lines below cannot do it: they look
    // identical whichever build produced them.
    log(INFO, "XlorieBuild: compiled %s %s", __DATE__, __TIME__);

    if (getenv("TERMUX_X11_DEBUG") && !fork()) {
        // Printing logs of local logcat.
        char pid[32] = {0};
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        sprintf(pid, "%d", getppid());
        execlp("logcat", "logcat", "--pid", pid, NULL);
    }

    // No matter what tracer is attached.
    // In the case of gdb or lldb LD_PRELOAD is already set.
    // In the case of proot or proot-distro libtermux-exec in LD_PRELOAD will break linking.
    if (access("/data/data/com.termux/files/usr/lib/libtermux-exec.so", F_OK) == 0 && !detectTracer()
            && !getenv("XSTARTUP_LD_PRELOAD"))
        setenv("LD_PRELOAD", "/data/data/com.termux/files/usr/lib/libtermux-exec.so", 1);

    // adb sets TMPDIR to /data/local/tmp which is pretty useless.
    if (!strcmp("/data/local/tmp", getenv("TMPDIR") ?: ""))
        unsetenv("TMPDIR");

    if (!getenv("TMPDIR")) {
        if (access("/tmp", F_OK) == 0)
            setenv("TMPDIR", "/tmp", 1);
        else if (access("/data/data/com.termux/files/usr/tmp", F_OK) == 0)
            setenv("TMPDIR", "/data/data/com.termux/files/usr/tmp", 1);
    }

    if (!getenv("TMPDIR")) {
        char* error = (char*) "$TMPDIR is not set. Normally it is pointing to /tmp of a container.";
        log(ERROR, "%s", error);
        dprintf(2, "%s\n", error);
        return JNI_FALSE;
    }

    {
        char* tmp = getenv("TMPDIR");
        char cwd[1024] = {0};

        if (!getcwd(cwd, sizeof(cwd)) || access(cwd, F_OK) != 0)
            chdir(tmp);
        asprintf(&xtrans_unix_path_x11, "%s/.X11-unix/X", tmp);
        asprintf(&xtrans_unix_dir_x11, "%s/.X11-unix/", tmp);
    }

    log(VERBOSE, "Using TMPDIR=\"%s\"", getenv("TMPDIR"));

    {
        const char *root_dir = dirname(getenv("TMPDIR"));
        const char* pathes[] = {
                "/etc/X11/fonts", "/usr/share/fonts/X11", "/share/fonts", NULL
        };
        for (int i=0; pathes[i]; i++) {
            char current_path[1024] = {0};
            snprintf(current_path, sizeof(current_path), "%s%s", root_dir, pathes[i]);
            if (access(current_path, F_OK) == 0) {
                char default_font_path[4096] = {0};
                snprintf(default_font_path, sizeof(default_font_path),
                         "%s/misc,%s/TTF,%s/OTF,%s/Type1,%s/100dpi,%s/75dpi",
                         current_path, current_path, current_path, current_path, current_path, current_path);
                defaultFontPath = strdup(default_font_path);
                break;
            }
        }
    }

    if (!getenv("XKB_CONFIG_ROOT")) {
        // chroot case
        const char *root_dir = dirname(getenv("TMPDIR"));
        char current_path[1024] = {0};
        snprintf(current_path, sizeof(current_path), "%s/usr/share/X11/xkb", root_dir);
        if (access(current_path, F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", current_path, 1);
    }

    if (!getenv("XKB_CONFIG_ROOT")) {
        // proot case
        if (access("/usr/share/xkeyboard-config-2", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/usr/share/xkeyboard-config-2", 1);
        else if (access("/usr/share/X11/xkb", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 1);
        // Termux case
        else if (access("/data/data/com.termux/files/usr/share/xkeyboard-config-2", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/data/data/com.termux/files/usr/share/xkeyboard-config-2", 1);
        else if (access("/data/data/com.termux/files/usr/share/X11/xkb", F_OK) == 0)
            setenv("XKB_CONFIG_ROOT", "/data/data/com.termux/files/usr/share/X11/xkb", 1);
    }

    if (!getenv("XKB_CONFIG_ROOT")) {
        char* error = (char*) "$XKB_CONFIG_ROOT is not set. Normally it is pointing to /usr/share/X11/xkb of a container.";
        log(ERROR, "%s", error);
        dprintf(2, "%s\n", error);
        return JNI_FALSE;
    }

    XkbBaseDirectory = getenv("XKB_CONFIG_ROOT");
    if (access(XkbBaseDirectory, F_OK) != 0) {
        log(ERROR, "%s is unaccessible: %s\n", XkbBaseDirectory, strerror(errno));
        printf("%s is unaccessible: %s\n", XkbBaseDirectory, strerror(errno));
        return JNI_FALSE;
    }

    (*env)->GetJavaVM(env, &vm);

    AChoreographer *choreographer = AChoreographer_getInstance();
    // Trigger it first time
    lorieChoreographerStart(choreographer);

    xorg_list_init(&registeredBuffers);
    pthread_create(&t, NULL, startServer, vm);
    return JNI_TRUE;
}

static Bool sendConfigureNotify(__unused ClientPtr pClient, void *closure) {
    // This must be done only on X server thread.
    lorieEvent* e = closure;
    __android_log_print(ANDROID_LOG_ERROR, "tx11-request", "window changed: %d %d %s", e->screenSize.width, e->screenSize.height, e->screenSize.name);
    lorieSetMonitorResolution(e->screenSize.dpi > 0 ? e->screenSize.dpi : 96); lorieConfigureNotify(e->screenSize.width, e->screenSize.height, e->screenSize.framerate, e->screenSize.name_size, e->screenSize.name);
    free(e);
    return TRUE;
}

static Bool handleClipboardAnnounce(__unused ClientPtr pClient, __unused void *closure) {
    // This must be done only on X server thread.
    lorieHandleClipboardAnnounce();
    return TRUE;
}

static struct LorieConnection *lorieConnectionOf(uint32_t session) {
    size_t i;

    for (i = 0; i < sizeof(lorieConnections) / sizeof(lorieConnections[0]); i++)
        if (lorieConnections[i].session == session && session)
            return &lorieConnections[i];
    return NULL;
}

static void lorieForgetRegisteredBuffers(void) {
    LorieBuffer* buf;

    while ((buf = LorieBufferList_first(&registeredBuffers)))
        LorieBuffer_removeFromList(buf);
}

// Closes one connection, once. The input thread may already have stopped watching it on hangup;
// asking again is harmless.
static void lorieConnectionClose(struct LorieConnection *c) {
    if (!c || !c->open)
        return;
    c->open = FALSE;
    InputThreadUnregisterDev(c->fd);
    if (conn_fd == c->fd)
        conn_fd = -1;
    close(c->fd);
}

/*
 * A connection hung up. Runs on the X server thread with the session the connection was opened as,
 * which the input thread kept with the socket. Only if that is the current connection is anything of
 * the current state touched; an older one that a newer connection has since replaced was closed then
 * and its session ended with it, so all that can be left is its socket, if not closed already.
 */
static Bool handleRendererLostEvent(__unused ClientPtr pClient, void *closure) {
    uint32_t session = (uint32_t) (uintptr_t) closure;
    struct LorieConnection *c = lorieConnectionOf(session);
    Bool current = c && c->open && lorieRendererSessionIsCurrent(session) && conn_fd == c->fd;

    lorieConnectionClose(c);
    if (!current) {
        log(INFO, "renderer connection of session %u hung up after being replaced; the current one is kept", session);
        return TRUE;
    }

    log(INFO, "renderer connection of session %u hung up", session);
    lorieEnableClipboardSync(FALSE);
    lorieNoteRendererLost();
    lorieForgetRegisteredBuffers();
    return TRUE;
}

static Bool handleGpuCopyDoneEvent(__unused ClientPtr pClient, __unused void *closure) {
    // This must be done only on X server thread (touches present's internal vblank queue).
    lorieRecheckGpuCopies();
    // Same thread, and the same news: a serial the renderer has finished with may be one an
    // abandoned copy is waiting on, and waiting for the next redraw instead would mean waiting for
    // one that need not come.
    lorieReapAbandonedCopies();
    return TRUE;
}

static Bool handleClipboardData(__unused ClientPtr pClient, void *closure) {
    // This must be done only on X server thread.
    lorieHandleClipboardData(closure);
    return TRUE;
}

static Bool handleTouchEvent(__unused ClientPtr pClient, void *closure) {
    ValuatorMask mask;
    lorieEvent *e = closure;
    double x = max(min((float) e->touch.x, pScreenPtr->width), 0);
    double y = max(min((float) e->touch.y, pScreenPtr->height), 0);
    valuator_mask_zero(&mask);
    DDXTouchPointInfoPtr touch = TouchFindByDDXID(lorieTouch, e->touch.id, FALSE);

    // Avoid duplicating events
    if (touch && touch->active) {
        double oldx = 0, oldy = 0;
        if (e->touch.type == XI_TouchUpdate &&
            valuator_mask_fetch_double(touch->valuators, 0, &oldx) &&
            valuator_mask_fetch_double(touch->valuators, 1, &oldy) &&
            oldx == x && oldy == y)
            goto end;
    }

    // Sometimes activity part does not send XI_TouchBegin and sends only XI_TouchUpdate.
    if (e->touch.type == XI_TouchUpdate && (!touch || !touch->active))
        e->touch.type = XI_TouchBegin;

    if (e->touch.type == XI_TouchEnd && (!touch || !touch->active))
        goto end;

    valuator_mask_set_double(&mask, 0, x * 0xFFFF / (float) pScreenPtr->width);
    valuator_mask_set_double(&mask, 1, y * 0xFFFF / (float) pScreenPtr->height);
    QueueTouchEvents(lorieTouch, e->touch.type, e->touch.id, 0, &mask);

    end:
    free(e);
    return TRUE;
}

/*
 * Input thread: the connection on `fd`, opened as `session`, hung up. It stops being watched, and
 * nothing else happens here: the socket, conn_fd, the session and the buffer list are the X server
 * thread's, and which connection this was - by the session it was opened as, not whichever is current
 * by the time that thread gets to it - is all it needs to sort out the rest.
 */
static void lorieConnectionHungUp(int fd, void *session) {
    InputThreadUnregisterDev(fd);
    QueueWorkProc(handleRendererLostEvent, NULL, session);
    lorieWakeServer();
}

// Input thread. `data` is the session the connection on `fd` was opened as (addFd).
void handleLorieEvents(int fd, __unused int ready, void *data) {
    ValuatorMask mask;
    lorieEvent e = {0};
    valuator_mask_zero(&mask);

    if (ready & X_NOTIFY_ERROR) {
        lorieConnectionHungUp(fd, data);
        return;
    }

    again:
    if (read(fd, &e, sizeof(e)) == sizeof(e)) {
        switch(e.type) {
            case EVENT_SCREEN_SIZE: {
                lorieEvent *copy = calloc(1, sizeof(lorieEvent) + e.screenSize.name_size + 1);
                memcpy(copy, &e, sizeof(e));
                copy->screenSize.name = copy->screenSize.name_size ? (char*) (copy + 1) : NULL;
                if (copy->screenSize.name_size)
                    read(fd, copy->screenSize.name, copy->screenSize.name_size);
                QueueWorkProc(sendConfigureNotify, NULL, copy);
                lorieWakeServer();
                break;
            }
            case EVENT_TOUCH: {
                lorieTraceInput(e.type);
                lorieEvent *copy = calloc(1, sizeof(lorieEvent));
                memcpy(copy, &e, sizeof(e));
                QueueWorkProc(handleTouchEvent, NULL, copy);
                lorieWakeServer();
                break;
            }
            case EVENT_STYLUS: {
                static int buttons_prev = 0;
                uint32_t released, pressed, diff;
                DeviceIntPtr device = e.stylus.mouse ? lorieMouse : (e.stylus.eraser ? lorieEraser : loriePen);
                if (!device) {
                    __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "got stylus event but device is not requested\n");
                    break;
                }
                __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "got stylus event %f %f %d %d %d %d %s\n", e.stylus.x, e.stylus.y, e.stylus.pressure, e.stylus.tilt_x, e.stylus.tilt_y, e.stylus.orientation,
                                    device == lorieMouse ? "lorieMouse" : (device == loriePen ? "loriePen" : "lorieEraser"));

                valuator_mask_set_double(&mask, 0, max(min(e.stylus.x, pScreenPtr->width), 0));
                valuator_mask_set_double(&mask, 1, max(min(e.stylus.y, pScreenPtr->height), 0));
                if (device != lorieMouse) {
                    valuator_mask_set_double(&mask, 2, e.stylus.pressure);
                    valuator_mask_set_double(&mask, 3, e.stylus.tilt_x);
                    valuator_mask_set_double(&mask, 4, e.stylus.tilt_y);
                    valuator_mask_set_double(&mask, 5, e.stylus.orientation);
                }
                QueuePointerEvents(device, MotionNotify, 0, POINTER_ABSOLUTE | POINTER_DESKTOP | (device == lorieMouse ? POINTER_NORAW : 0), &mask);

                diff = buttons_prev ^ e.stylus.buttons;
                released = diff & ~e.stylus.buttons;
                pressed = diff & e.stylus.buttons;

                for (int i=0; i<3; i++) {
                    if (released & 0x1) {
                        QueuePointerEvents(device, ButtonRelease, i + 1, POINTER_RELATIVE, NULL);
                        __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "sending %d press", i+1);
                    }
                    if (pressed & 0x1) {
                        QueuePointerEvents(device, ButtonPress, i + 1, POINTER_RELATIVE, NULL);
                        __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "sending %d release", i+1);
                    }
                    released >>= 1;
                    pressed >>= 1;
                }
                buttons_prev = e.stylus.buttons;

                break;
            }
            case EVENT_STYLUS_ENABLE: {
                lorieSetStylusEnabled(e.stylusEnable.enable);
                break;
            }
            case EVENT_MOUSE: {
                lorieTraceInput(e.type);
                int flags;
                switch(e.mouse.detail) {
                    case 0: // BUTTON_UNDEFINED
                        flags = (e.mouse.relative) ? POINTER_RELATIVE | POINTER_ACCELERATE : POINTER_ABSOLUTE | POINTER_SCREEN | POINTER_NORAW;
                        if (!e.mouse.relative) {
                            e.mouse.x = max(0, min(e.mouse.x, pScreenPtr->width));
                            e.mouse.y = max(0, min(e.mouse.y, pScreenPtr->height));
                        }
                        valuator_mask_set_double(&mask, 0, (double) e.mouse.x);
                        valuator_mask_set_double(&mask, 1, (double) e.mouse.y);
                        QueuePointerEvents(lorieMouse, MotionNotify, 0, flags, &mask);
                        break;
                    case 1: // BUTTON_LEFT
                    case 2: // BUTTON_MIDDLE
                    case 3: // BUTTON_RIGHT
                        QueuePointerEvents(lorieMouse, e.mouse.down ? ButtonPress : ButtonRelease, e.mouse.detail, POINTER_RELATIVE, NULL);
                        break;
                    case 4: // BUTTON_SCROLL
                        if (e.mouse.x) {
                            valuator_mask_zero(&mask);
                            valuator_mask_set_double(&mask, 2, (double) e.mouse.x / 120);
                            QueuePointerEvents(lorieMouse, MotionNotify, 0, POINTER_RELATIVE, &mask);
                        }
                        if (e.mouse.y) {
                            valuator_mask_zero(&mask);
                            valuator_mask_set_double(&mask, 3, (double) e.mouse.y / 120);
                            QueuePointerEvents(lorieMouse, MotionNotify, 0, POINTER_RELATIVE, &mask);
                        }
                        break;
                }
                break;
            }
            case EVENT_KEY:
                QueueKeyboardEvents(lorieKeyboard, e.key.state ? KeyPress : KeyRelease, e.key.key);
                break;
            case EVENT_UNICODE: {
                int ks = ucs2keysym((long) e.unicode.code);
                __android_log_print(ANDROID_LOG_DEBUG, "LorieNative", "Trying to input keysym %d\n", ks);
                lorieKeysymKeyboardEvent(ks, TRUE);
                lorieKeysymKeyboardEvent(ks, FALSE);
                break;
            }
            case EVENT_CLIPBOARD_ENABLE:
                lorieEnableClipboardSync(e.clipboardEnable.enable);
                break;
            case EVENT_CLIPBOARD_ANNOUNCE:
                QueueWorkProc(handleClipboardAnnounce, NULL, NULL);
                lorieWakeServer();
                break;
            case EVENT_CLIPBOARD_SEND: {
                char *data = calloc(1, e.clipboardSend.count + 1);
                read(fd, data, e.clipboardSend.count);
                data[e.clipboardSend.count] = 0;
                QueueWorkProc(handleClipboardData, NULL, data);
                lorieWakeServer();
                break;
            }
            case EVENT_RENDERER_WAKEUP_COND: {
                int wakeupFd = ancil_recv_fd(fd);
                if (wakeupFd >= 0)
                    lorieSetRendererWakeupCond(wakeupFd);
                break;
            }
            case EVENT_GPU_COPY_DONE:
                QueueWorkProc(handleGpuCopyDoneEvent, NULL, NULL);
                lorieWakeServer();
                break;
        }

        int n;
        if (ioctl(fd, FIONREAD, &n) >= 0 && n > sizeof(e))
            goto again;
    }
}

void lorieSendClipboardData(const char* data) {
    if (data && conn_fd != -1) {
        size_t len = strlen(data);
        lorieEvent e = { .clipboardSend = { .t = EVENT_CLIPBOARD_SEND, .count = len } };
        write(conn_fd, &e, sizeof(e));
        write(conn_fd, data, len);
    }
}

void lorieRequestClipboard(void) {
    if (conn_fd != -1) {
        lorieEvent e = { .type = EVENT_CLIPBOARD_REQUEST };
        write(conn_fd, &e, sizeof(e));
    }
}

bool lorieConnectionAlive(void) {
    if (conn_fd == -1)
        return false;

    // Check if socket is closed or has errors.
    struct pollfd p = { .fd = conn_fd, .events = POLLIN | POLLHUP | POLLERR | POLLRDHUP };
    return !(poll(&p, 1, 0) == 1 && (p.revents & (POLLERR | POLLNVAL | POLLRDHUP | POLLHUP)));
}

/* A new connection on its way from the Binder thread that made it to the X server thread. A few
 * slots, taken in turn: connections are made one at a time, each picked up right after. */
static struct { int fd; int32_t pid; } lorieNewConnections[4];
static uint32_t lorieNewConnectionNext;

static Bool addFd(__unused ClientPtr pClient, void *closure) {
    int fd = lorieNewConnections[(uintptr_t) closure].fd;
    int32_t pid = lorieNewConnections[(uintptr_t) closure].pid;
    struct LorieConnection *c = NULL;
    uint32_t session;
    size_t i;

    // A connection still open is being replaced: closed now, so the renderer at its other end lets go
    // of the state, and so its hangup, whenever that arrives, finds nothing left of it to act on.
    lorieConnectionClose(lorieConnectionOf(lorieRendererSessionId()));
    // What was sent on it is not something this renderer has.
    lorieForgetRegisteredBuffers();

    // Before anything is offered to it, so every copy carries the session that owes its report.
    lorieNoteRendererConnected(pid);
    session = lorieRendererSessionId();

    for (i = 0; i < sizeof(lorieConnections) / sizeof(lorieConnections[0]) && !c; i++)
        if (!lorieConnections[i].open)
            c = &lorieConnections[i];
    if (!c)
        FatalError("more renderer connections open at once than there is room for\n");
    c->session = session;
    c->fd = fd;
    c->open = TRUE;
    log(INFO, "renderer connection of session %u: fd %d", session, fd);

    InputThreadRegisterDev(fd, handleLorieEvents, (void *) (uintptr_t) session);
    conn_fd = fd;
    lorieActivityConnected();
    return TRUE;
}

void lorieSendSharedServerState(int memfd) {
    if (conn_fd != -1) {
        lorieEvent e = { .type = EVENT_SHARED_SERVER_STATE };
        write(conn_fd, &e, sizeof(e));
        ancil_send_fd(conn_fd, memfd);
    }
}

void lorieRegisterBuffer(LorieBuffer* buffer) {
    unsigned long id = LorieBuffer_description(buffer)->id;
    if (conn_fd == -1 || LorieBufferList_findById(&registeredBuffers, id))
        return; // Already registered

    if (conn_fd != -1 && buffer) {
        lorieEvent e = { .type = EVENT_ADD_BUFFER };
        write(conn_fd, &e, sizeof(e));
        LorieBuffer_sendHandleToUnixSocket(buffer, conn_fd);
        LorieBuffer_addToList(buffer, &registeredBuffers);
        const LorieBuffer_Desc* desc = LorieBuffer_description(buffer);
        log(INFO, "Sent shared buffer width %d stride %d height %d format %d type %d id %llu", desc->width, desc->stride, desc->height, desc->format, desc->type, desc->id);
    }
}

void lorieUnregisterBuffer(LorieBuffer* buffer) {
    unsigned long id;
    if (!buffer || (!LorieBufferList_findById(&registeredBuffers, (id = LorieBuffer_description(buffer)->id))))
        return;  // Not exist or not registered so no need to unregister

    if (conn_fd != -1 && buffer) {
        lorieEvent e = { .removeBuffer = { .t = EVENT_REMOVE_BUFFER, .id = id } };
        write(conn_fd, &e, sizeof(e));
        LorieBuffer_removeFromList(buffer);
    }
}

void DDXNotifyFocusChanged(void) {
    if (conn_fd != -1) {
        lorieEvent e = { .type = EVENT_WINDOW_FOCUS_CHANGED };
        write(conn_fd, &e, sizeof(e));
    }
}

JNIEXPORT jobject JNICALL
Java_com_termux_x11_CmdEntryPoint_getXConnection(JNIEnv *env, __unused jobject cls) {
    int client[2];
    jclass ParcelFileDescriptorClass = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    jmethodID adoptFd = (*env)->GetStaticMethodID(env, ParcelFileDescriptorClass, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
    /*
     * Which process is asking: this runs inside the Binder call the renderer's activity makes, so the
     * calling pid is the renderer's. The socket cannot say - it is a pair made here, and its peer
     * credentials are this process's own. The X server needs it to tell, once a connection has gone,
     * whether the process that may still be using its buffers has gone too (lorieCopyResolve).
     */
    jclass BinderClass = (*env)->FindClass(env, "android/os/Binder");
    jmethodID getCallingPid = BinderClass ? (*env)->GetStaticMethodID(env, BinderClass, "getCallingPid", "()I") : NULL;
    int32_t pid = getCallingPid ? (int32_t) (*env)->CallStaticIntMethod(env, BinderClass, getCallingPid) : 0;

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        pid = 0;
    }
    if (pid == (int32_t) getpid())
        pid = 0;   // not a call from another process after all: nothing to watch
    socketpair(AF_UNIX, SOCK_STREAM, 0, client);
    {
        uint32_t n = __atomic_fetch_add(&lorieNewConnectionNext, 1, __ATOMIC_RELAXED) % 4u;

        lorieNewConnections[n].fd = client[1];
        lorieNewConnections[n].pid = pid;
        // QueueWorkProc's own locking orders these stores before the X server thread reads them.
        QueueWorkProc(addFd, NULL, (void*) (uintptr_t) n);
    }
    lorieWakeServer();

    return (*env)->CallStaticObjectMethod(env, ParcelFileDescriptorClass, adoptFd, client[0]);
}

void* logcatThread(void *arg) {
    char buffer[4096];
    size_t len;
    while((len = read((int) (int64_t) arg, buffer, 4096)) >=0)
        write(2, buffer, len);
    close((int) (int64_t) arg);
    return NULL;
}

JNIEXPORT jobject JNICALL
Java_com_termux_x11_CmdEntryPoint_getLogcatOutput(JNIEnv *env, __unused jobject cls) {
    jclass ParcelFileDescriptorClass = (*env)->FindClass(env, "android/os/ParcelFileDescriptor");
    jmethodID adoptFd = (*env)->GetStaticMethodID(env, ParcelFileDescriptorClass, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
    const char *debug = getenv("TERMUX_X11_DEBUG");
    if (debug && !strcmp(debug, "1")) {
        pthread_t t;
        int p[2];
        pipe(p);
        fchmod(p[1], 0777);
        pthread_create(&t, NULL, logcatThread, (void*) (uint64_t) p[0]);
        return (*env)->CallStaticObjectMethod(env, ParcelFileDescriptorClass, adoptFd, p[1]);
    }
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_termux_x11_CmdEntryPoint_connected(__unused JNIEnv *env, __unused jclass clazz) {
    return conn_fd != -1;
}

JNIEXPORT void JNICALL
Java_com_termux_x11_CmdEntryPoint_listenForConnections(JNIEnv *env, jobject thiz) {
    int server_fd, client, count;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr = { .s_addr = INADDR_ANY }, .sin_port = htons(PORT) };
    int addrlen = sizeof(address);
    jmethodID sendBroadcast = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz), "sendBroadcast", "()V");
    uint8_t buffer[512] = {0};

    // Even in the case if it will fail for some reason everything will work fine
    // But connection will be delayed a bit

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        log(ERROR, "Socket creation failed: %s", strerror(errno));
        return;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        log(ERROR, "Socket bind failed: %s", strerror(errno));
        close(server_fd);
        return;
    }

    if (listen(server_fd, 5) < 0) {
        log(ERROR, "Socket listen failed: %s", strerror(errno));
        close(server_fd);
        return;
    }

    while(1) {
        if ((client = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
            log(ERROR, "Socket accept failed: %s", strerror(errno));
            continue;
        }

        if ((count = read(client, buffer, sizeof(buffer))) > 0) {
            if (!memcmp(buffer, MAGIC, min(count, sizeof(MAGIC)))) {
                log(DEBUG, "New client connection!\n");
                (*env)->CallVoidMethod(env, thiz, sendBroadcast);
            }
        }
        close(client);
    }
}

void abort(void) {
    _exit(134);
}

void exit(int code) {
    _exit(code);
}
