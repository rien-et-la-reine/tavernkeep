// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "fake_platform.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "platform/debug.h"

static bool clock_valid;
static clock_datetime_t clock_now;

void fake_clock_set(clock_datetime_t now)
{
    clock_now = now;
    clock_valid = true;
}

void fake_clock_invalidate(void)
{
    clock_valid = false;
}

bool clock_is_valid(void)
{
    return clock_valid;
}

bool clock_get_datetime(clock_datetime_t *now)
{
    if (now == NULL || !clock_valid) {
        return false;
    }
    *now = clock_now;
    return true;
}

static unsigned log_total;
static unsigned log_warnings;

void fake_log_reset(void)
{
    log_total = 0;
    log_warnings = 0;
}

unsigned fake_log_warnings(void)
{
    return log_warnings;
}

unsigned fake_log_count(void)
{
    return log_total;
}

void debug_log(debug_level_t level, const char *format, ...)
{
    log_total++;
    if (level == DEBUG_LEVEL_WARN) {
        log_warnings++;
    }
    va_list args;
    va_start(args, format);
    printf("    [log] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}
