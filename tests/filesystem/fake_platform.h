// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef TAVERNKEEP_TESTS_FILESYSTEM_FAKE_PLATFORM_H
#define TAVERNKEEP_TESTS_FILESYSTEM_FAKE_PLATFORM_H

//test doubles for the platform functions the filesystem layer calls: the wall clock (platform/clock.h)
//and the logger (platform/debug.h). the real clock.c is not linked; this one is settable.

#include <stdbool.h>

#include "platform/clock.h"

//the clock reports `now` from now on; fake_clock_invalidate() makes it report "not valid" again
void fake_clock_set(clock_datetime_t now);
void fake_clock_invalidate(void);

//log lines captured since the last reset, by level
void fake_log_reset(void);
unsigned fake_log_warnings(void);
unsigned fake_log_count(void);

#endif
