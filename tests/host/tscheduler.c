/* P11-C3-E: real parser from InitOutput.c (generated into xscheduler_src.inc).  Must not alter Xorg's defaults or
 * command-line choices unless the user explicitly supplies a valid experimental interval. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int Bool;
#define TRUE 1
#define FALSE 0

long SmartScheduleInterval = 5;
long SmartScheduleSlice = 5;
long SmartScheduleMaxSlice = 15;

#include "xscheduler_src.inc"

static int failures;
static void check(int yes, const char *what) {
    if (!yes) { fprintf(stderr, "FAIL %s\n", what); failures++; }
}

static void reset(void) {
    SmartScheduleInterval = 5;
    SmartScheduleSlice = 5;
    SmartScheduleMaxSlice = 15;
}

int main(void) {
    static const char *bad[] = { "", "0", "11", "-2", "+2", " 2", "2 ", "2ms", "123456789012345678901" };
    size_t i;

    reset();
    check(!lorieTuneXRequestScheduler(NULL) && SmartScheduleInterval == 5 && SmartScheduleMaxSlice == 15,
          "unset must keep Xorg defaults");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset();
        check(!lorieTuneXRequestScheduler(bad[i]) && SmartScheduleInterval == 5 && SmartScheduleSlice == 5 &&
              SmartScheduleMaxSlice == 15, "invalid value must leave all Xorg scheduler settings untouched");
    }
    reset();
    check(lorieTuneXRequestScheduler("2") && SmartScheduleInterval == 2 && SmartScheduleSlice == 2 &&
          SmartScheduleMaxSlice == 6, "2ms interval uses 6ms bounded maximum");
    reset();
    check(lorieTuneXRequestScheduler("1") && SmartScheduleInterval == 1 && SmartScheduleMaxSlice == 3,
          "1ms lower bound");
    reset();
    check(lorieTuneXRequestScheduler("10") && SmartScheduleInterval == 10 && SmartScheduleMaxSlice == 30,
          "10ms upper bound");
    SmartScheduleInterval = 3; SmartScheduleSlice = 3; SmartScheduleMaxSlice = 9;
    check(!lorieTuneXRequestScheduler("invalid") && SmartScheduleInterval == 3 && SmartScheduleSlice == 3 &&
          SmartScheduleMaxSlice == 9, "invalid input preserves Xorg command-line override");
    printf("TXSCHEDULER Xorg dispatch-interval opt-in: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
