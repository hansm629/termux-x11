/* Test double for <android/choreographer.h>, <android/looper.h> and the libandroid symbols frameclock.c
 * resolves at runtime. Included by frameclock.c after the system headers it uses, so the macros below
 * only redirect frameclock.c's own calls. See tframeclock.c. */
#pragma once
#include <stdint.h>
#include <time.h>

typedef struct AChoreographer AChoreographer;
typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void *data);

AChoreographer *AChoreographer_getInstance(void);
void AChoreographer_postFrameCallback(AChoreographer *choreographer, AChoreographer_frameCallback callback, void *data);

typedef struct ALooper ALooper;
typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);
enum { ALOOPER_POLL_CALLBACK = -2, ALOOPER_EVENT_INPUT = 1 << 0, ALOOPER_EVENT_ERROR = 1 << 2, ALOOPER_EVENT_HANGUP = 1 << 3 };
ALooper *ALooper_forThread(void);
int ALooper_addFd(ALooper *looper, int fd, int ident, int events, ALooper_callbackFunc callback, void *data);

int fake_clock_gettime(clockid_t clock, struct timespec *ts);
void *fake_dlopen(const char *name, int flags);
void *fake_dlsym(void *handle, const char *symbol);
#define clock_gettime fake_clock_gettime
#define dlopen fake_dlopen
#define dlsym fake_dlsym
