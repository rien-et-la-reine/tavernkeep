// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef TAVERNKEEP_PLATFORM_DEBUG_H
#define TAVERNKEEP_PLATFORM_DEBUG_H

#include <stdbool.h>

#ifndef TAVERNKEEP_LOGGING_ENABLED
#define TAVERNKEEP_LOGGING_ENABLED 1
#endif

typedef enum {
    DEBUG_LEVEL_INFO = 0,
    DEBUG_LEVEL_WARN,
    DEBUG_LEVEL_ERROR,
} debug_level_t;

bool debug_init(void);

#if defined(__GNUC__) || defined(__clang__)
void debug_log(debug_level_t level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#else
void debug_log(debug_level_t level, const char *format, ...);
#endif

#if TAVERNKEEP_LOGGING_ENABLED
#define DEBUG_INFO(...) debug_log(DEBUG_LEVEL_INFO, __VA_ARGS__)
#define DEBUG_WARN(...) debug_log(DEBUG_LEVEL_WARN, __VA_ARGS__)
#define DEBUG_ERROR(...) debug_log(DEBUG_LEVEL_ERROR, __VA_ARGS__)
#else
#define DEBUG_INFO(...) do { } while (0)
#define DEBUG_WARN(...) do { } while (0)
#define DEBUG_ERROR(...) do { } while (0)
#endif

#endif

