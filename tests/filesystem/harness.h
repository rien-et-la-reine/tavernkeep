// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef TAVERNKEEP_TESTS_FILESYSTEM_HARNESS_H
#define TAVERNKEEP_TESTS_FILESYSTEM_HARNESS_H

//minimal test harness for the filesystem-layer host tests
//
//a test case is a void function registered in test_main.c. EXPECT* records a failure and carries on;
//REQUIRE* records a failure and returns from the test case, for checks whose failure would make the
//rest of the case meaningless or unsafe (a handle that was never opened, a NULL pointer). every case
//must execute at least one check: a case that executes none fails, so a test cannot pass vacuously
//because an early return or an empty loop skipped everything it was meant to verify.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "storage/filesystem.h"

void harness_record(bool ok, const char *file, int line, const char *expression, const char *detail);
const char *harness_result_name(filesystem_result_t result);

#define HARNESS_DETAIL_SIZE 160

#define EXPECT(cond) \
    harness_record((cond), __FILE__, __LINE__, #cond, NULL)

#define EXPECT_EQ_U(actual, expected) do { \
    const unsigned long long harness_a_ = (unsigned long long)(actual); \
    const unsigned long long harness_e_ = (unsigned long long)(expected); \
    char harness_d_[HARNESS_DETAIL_SIZE]; \
    snprintf(harness_d_, sizeof harness_d_, "got %llu (0x%llx), expected %llu (0x%llx)", \
        harness_a_, harness_a_, harness_e_, harness_e_); \
    harness_record(harness_a_ == harness_e_, __FILE__, __LINE__, #actual " == " #expected, harness_d_); \
} while (0)

#define EXPECT_RESULT(actual, expected) do { \
    const filesystem_result_t harness_a_ = (actual); \
    const filesystem_result_t harness_e_ = (expected); \
    char harness_d_[HARNESS_DETAIL_SIZE]; \
    snprintf(harness_d_, sizeof harness_d_, "got %s, expected %s", \
        harness_result_name(harness_a_), harness_result_name(harness_e_)); \
    harness_record(harness_a_ == harness_e_, __FILE__, __LINE__, #actual, harness_d_); \
} while (0)

#define REQUIRE(cond) do { \
    const bool harness_ok_ = (cond); \
    harness_record(harness_ok_, __FILE__, __LINE__, #cond, "(required; ending this case)"); \
    if (!harness_ok_) { \
        return; \
    } \
} while (0)

#define REQUIRE_RESULT(actual, expected) do { \
    const filesystem_result_t harness_a_ = (actual); \
    const filesystem_result_t harness_e_ = (expected); \
    char harness_d_[HARNESS_DETAIL_SIZE]; \
    snprintf(harness_d_, sizeof harness_d_, "got %s, expected %s (required; ending this case)", \
        harness_result_name(harness_a_), harness_result_name(harness_e_)); \
    harness_record(harness_a_ == harness_e_, __FILE__, __LINE__, #actual, harness_d_); \
    if (harness_a_ != harness_e_) { \
        return; \
    } \
} while (0)

typedef struct {
    const char *name;
    void (*run)(void);
} test_case_t;

#define TEST_CASE(fn) { #fn, fn }

#endif
