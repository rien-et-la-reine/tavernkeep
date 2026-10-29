// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_TESTS_HARNESS_H
#define BARTAP_TESTS_HARNESS_H

//minimal test harness for Bartap's host tests, adapted from the tavernkeep firmware's storage-layer tests
//
//a test case is a void function listed in its test file's case table, and every table is registered in
//test_main.c. EXPECT* records a failure and carries on; REQUIRE* records a failure and returns from the
//test case, for checks whose failure would make the rest of the case meaningless or unsafe. every case
//must execute at least one check: a case that executes none fails, so a test cannot pass vacuously
//because an early return or an empty loop skipped everything it was meant to verify.
//
//the build supplies three directories as string literals:
//  BARTAP_TESTS_FIXTURE_DIR    tests/fixtures, the committed fixtures
//  BARTAP_TESTS_GENERATED_DIR  where make_fixtures.py wrote its inputs for this build (manifest.json
//                              lists them)
//  BARTAP_TESTS_BOOKS_DIR      where make_books.py wrote its books (books.json lists them)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void harness_record(bool ok, const char *file, int line, const char *expression, const char *detail);

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

#define REQUIRE(cond) do { \
    const bool harness_ok_ = (cond); \
    harness_record(harness_ok_, __FILE__, __LINE__, #cond, "(required; ending this case)"); \
    if (!harness_ok_) { \
        return; \
    } \
} while (0)

typedef struct {
    const char *name;
    void (*run)(void);
} test_case_t;

#define TEST_CASE(fn) { #fn, fn }

//case tables, one per test file; add each new one to the list in test_main.c too
extern const test_case_t headers_tests[];
extern const size_t headers_test_count;

extern const test_case_t text_tests[];
extern const size_t text_test_count;

extern const test_case_t fixtures_tests[];
extern const size_t fixtures_test_count;

extern const test_case_t xml_tests[];
extern const size_t xml_test_count;

#endif
