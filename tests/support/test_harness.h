/*
 * Small test harness shared by the adversarial SD suites.
 *
 * Two things it does that a bare assert does not:
 *   - failures print the offending values and the current case context, so a
 *     table-driven row says which row it was;
 *   - failures dump the SD protocol trace, so "the driver returned IO_ERROR"
 *     comes with the byte-level story of how it got there.
 *
 * Checks stay active under NDEBUG deliberately: the suite is run in Release.
 */
#ifndef TAVERNKEEP_TEST_HARNESS_H
#define TAVERNKEEP_TEST_HARNESS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sd_card_model.h"
#include "storage/block_device.h"

extern int t_failures;
extern int t_checks;
extern int t_cases;
extern char t_case_context[256];
extern const char *t_current_case;
extern bool t_case_failed;
extern bool t_dump_trace_on_failure;

void t_context(const char *format, ...);
void t_clear_context(void);
void t_run(void (*test)(void), const char *name);
int t_summary(const char *suite);
void t_report_failure(const char *file, int line, const char *text);
const char *t_result_name(block_device_result_t result);

#define T_CHECK(condition)                                                     \
    do {                                                                       \
        t_checks++;                                                            \
        if (!(condition)) {                                                    \
            t_report_failure(__FILE__, __LINE__, #condition);                   \
            return;                                                            \
        }                                                                      \
    } while (false)

#define T_EQ_U(expected, actual)                                               \
    do {                                                                       \
        t_checks++;                                                            \
        const unsigned long long t_e_ = (unsigned long long)(expected);        \
        const unsigned long long t_a_ = (unsigned long long)(actual);          \
        if (t_e_ != t_a_) {                                                    \
            char t_buf_[192];                                                  \
            (void)snprintf(t_buf_, sizeof(t_buf_),                             \
                "%s == %s (expected %llu, got %llu)",                          \
                #expected, #actual, t_e_, t_a_);                               \
            t_report_failure(__FILE__, __LINE__, t_buf_);                       \
            return;                                                            \
        }                                                                      \
    } while (false)

#define T_EQ_RESULT(expected, actual)                                          \
    do {                                                                       \
        t_checks++;                                                            \
        const block_device_result_t t_e_ = (expected);                         \
        const block_device_result_t t_a_ = (actual);                           \
        if (t_e_ != t_a_) {                                                    \
            char t_buf_[192];                                                  \
            (void)snprintf(t_buf_, sizeof(t_buf_),                             \
                "expected %s, got %s",                                         \
                t_result_name(t_e_), t_result_name(t_a_));                     \
            t_report_failure(__FILE__, __LINE__, t_buf_);                       \
            return;                                                            \
        }                                                                      \
    } while (false)

#endif
