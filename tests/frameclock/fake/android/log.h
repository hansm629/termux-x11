/* Test double for <android/log.h>: frameclock.c's lines go to fake_log (tframeclock.c). */
#pragma once
enum { ANDROID_LOG_DEBUG = 3, ANDROID_LOG_INFO = 4, ANDROID_LOG_WARN = 5, ANDROID_LOG_ERROR = 6 };
int fake_log(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define __android_log_print fake_log
