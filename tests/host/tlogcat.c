/* The X server's forwarder of the app's logcat (TERMUX_X11_DEBUG=1, logcatThread in cmdentrypoint.c,
 * extracted by gen.py): it copies what comes through the pipe to stderr, and once the app's end is
 * closed - its logcat exits with the app process, every time the app goes away - it has to stop. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

static void put(int fd, const char *s) { (void) !write(fd, s, strlen(s)); }

static char out[65536];
static size_t outLen;
static long fakeWrite(int fd, const void *b, size_t n) {
    if (fd != 2 || n > sizeof out - outLen) {
        printf("  FAIL: a write of %zu bytes to fd %d\nlogcat forwarder: FAIL\n", n, fd);
        exit(1);
    }
    memcpy(out + outLen, b, n);
    outLen += n;
    return (long) n;
}
#define write(fd, b, n) fakeWrite(fd, b, n)
#include "tlogcat_src.inc"
#undef write

static void timeout(int s) {
    (void) s;
    static const char m[] = "  FAIL: still running 2 s after the app's end was closed (spinning on end of file)\n"
                            "logcat forwarder: FAIL\n";
    (void) !write(1, m, sizeof m - 1);
    _exit(1);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    int p[2];

    if (pipe(p))
        return 1;
    put(p[1], "line one\n");
    put(p[1], "line two\n");
    close(p[1]);                                   /* the app's logcat has exited */
    fflush(stdout);
    signal(SIGALRM, timeout);
    alarm(2);
    logcatThread((void *) (intptr_t) p[0]);
    alarm(0);
    CHECK(outLen == 18 && !memcmp(out, "line one\nline two\n", 18), "forwarded %zu bytes, not the 18 written", outLen);
    CHECK(fcntl(p[0], F_GETFD) == -1, "its end of the pipe left open");

    printf("logcat forwarder: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
