// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "platform/debug.h"

#include <stdarg.h>
#include <stdio.h>

#include "pico/stdlib.h"

#if TAVERNKEEP_LOGGING_ENABLED
static const char *debug_level_name(debug_level_t level)
{
    switch (level) {
    case DEBUG_LEVEL_INFO:
        return "INFO";
    case DEBUG_LEVEL_WARN:
        return "WARN";
    case DEBUG_LEVEL_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}
#endif

bool debug_init(void)
{
#if TAVERNKEEP_LOGGING_ENABLED
    return stdio_init_all();
#else
    return true;
#endif
}

void debug_log(debug_level_t level, const char *format, ...)
{
#if TAVERNKEEP_LOGGING_ENABLED
    if (format == NULL) {
        return;
    }

    (void)printf("[%s] ", debug_level_name(level));

    va_list arguments;
    va_start(arguments, format);
    (void)vprintf(format, arguments);
    va_end(arguments);

    (void)putchar('\n');
#else
    (void)level;
    (void)format;
#endif
}
