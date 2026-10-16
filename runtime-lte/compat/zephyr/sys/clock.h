/*
 * Zephyr 4.1 (NCS v3.1) shim for <zephyr/sys/clock.h> (added in Zephyr 4.2),
 * used by runtime-zephyr main when the blob is built from source here.
 * Needs CONFIG_POSIX_TIMERS. Error returns differ (-1 + errno instead of
 * -errno); the monitor only checks for != 0.
 * ponytail: drop once the workshop moves to an NCS on Zephyr >= 4.2.
 */
#pragma once
#include <time.h>

#define SYS_CLOCK_REALTIME CLOCK_REALTIME
#define sys_clock_settime  clock_settime
#define sys_clock_gettime  clock_gettime
