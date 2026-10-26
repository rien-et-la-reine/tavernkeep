// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//runner for Bartap's host tests, adapted from the tavernkeep firmware's storage-layer tests
//
//usage: bartap_tests [--list] [--shuffle SEED] [--no-fork] [--selftest] [NAME_SUBSTRING...]
//  --list       print the case names and exit
//  --shuffle    run the cases in an order derived from SEED, to expose hidden order dependencies
//  --no-fork    run the selected cases in this process, for a debugger
//  --selftest   check the harness itself: run one case that fails a check, one that executes no
//               checks and one that crashes, and exit 0 only if all three were reported as failures
//  names        run only the cases whose name contains one of the substrings
//
//by default every case runs in its own child process, so no case can see state an earlier one left
//behind and a case that crashes is reported as a failure rather than ending the run. the decoders are
//meant to keep all their state in caller-owned structs, but this costs nothing and catches the case
//where one does not.
//
//exit status is 0 only if every selected case ran at least one check and none failed.

#define _POSIX_C_SOURCE 200809L     //fork, pipe, waitpid

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"

//every case table; add a new test file's here, and declare it in harness.h
static const struct {
    const test_case_t *cases;
    const size_t *count;
} suites[] = {
    { headers_tests, &headers_test_count },
};

static unsigned case_checks;
static unsigned case_failures;
static unsigned total_checks;

void harness_record(bool ok, const char *file, int line, const char *expression, const char *detail)
{
    case_checks++;
    total_checks++;
    if (!ok) {
        case_failures++;
        printf("    FAIL %s:%d: %s%s%s\n", file, line, expression, detail != NULL ? " -- " : "",
            detail != NULL ? detail : "");
    }
}

//returns true if the case passed
static bool run_case(const test_case_t *test)
{
    printf("%s\n", test->name);
    case_checks = 0;
    case_failures = 0;
    test->run();

    if (case_checks == 0) {
        printf("    FAIL: the case executed no checks\n");
        case_failures++;
    }
    printf("    %s (%u checks)\n", case_failures == 0 ? "ok" : "FAILED", case_checks);
    return case_failures == 0;
}

//run one case in a child process. the child reports its check count through a pipe; a child that dies
//is a failure
static bool run_case_isolated(const test_case_t *test)
{
    int channel[2];
    if (pipe(channel) != 0) {
        perror("pipe");
        exit(2);
    }
    fflush(stdout);
    const pid_t child = fork();
    if (child < 0) {
        perror("fork");
        exit(2);
    }
    if (child == 0) {
        close(channel[0]);
        const bool passed = run_case(test);
        const unsigned checks = case_checks;
        if (write(channel[1], &checks, sizeof checks) != (ssize_t)sizeof checks) {
            _exit(3);
        }
        close(channel[1]);
        fflush(stdout);
        exit(passed ? 0 : 1);
    }

    close(channel[1]);
    unsigned checks = 0;
    const bool reported = read(channel[0], &checks, sizeof checks) == (ssize_t)sizeof checks;
    close(channel[0]);
    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        perror("waitpid");
        exit(2);
    }
    total_checks += checks;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && reported) {
        return true;
    }
    if (WIFSIGNALED(status)) {
        printf("    FAIL: the case crashed (signal %d)\n", WTERMSIG(status));
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 1 || !reported) {
        printf("    FAIL: the case ended abnormally (exit status %d)\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return false;
}

/* ------------------------------------------------------------ selftest */

static void selftest_failing_case(void)
{
    EXPECT(1 + 1 == 3);
}

static void selftest_vacuous_case(void)
{
}

static void selftest_crashing_case(void)
{
    EXPECT(true);
    volatile int *volatile nowhere = NULL;
    *nowhere = 1;
}

static int run_selftest(void)
{
    static const test_case_t failing = TEST_CASE(selftest_failing_case);
    static const test_case_t vacuous = TEST_CASE(selftest_vacuous_case);
    static const test_case_t crashing = TEST_CASE(selftest_crashing_case);
    printf("harness selftest: every case below must be reported FAILED\n");
    const bool failing_detected = !run_case(&failing) && !run_case_isolated(&failing);
    const bool vacuous_detected = !run_case(&vacuous) && !run_case_isolated(&vacuous);
    const bool crash_detected = !run_case_isolated(&crashing);
    const bool ok = failing_detected && vacuous_detected && crash_detected;
    printf("harness selftest %s\n", ok ? "passed" : "FAILED: the harness can pass a broken test");
    return ok ? 0 : 1;
}

/* --------------------------------------------------------------- main */

static bool selected(const char *name, int argc, char **argv, int first_name)
{
    if (first_name >= argc) {
        return true;
    }
    for (int i = first_name; i < argc; i++) {
        if (strstr(name, argv[i]) != NULL) {
            return true;
        }
    }
    return false;
}

//xorshift32: a fixed, portable sequence for --shuffle, independent of the C library's rand()
static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    bool list = false;
    bool shuffle = false;
    bool isolate = true;
    uint32_t seed = 0;
    int first_name = 1;
    while (first_name < argc && strncmp(argv[first_name], "--", 2) == 0) {
        if (strcmp(argv[first_name], "--list") == 0) {
            list = true;
        } else if (strcmp(argv[first_name], "--no-fork") == 0) {
            isolate = false;
        } else if (strcmp(argv[first_name], "--selftest") == 0) {
            return run_selftest();
        } else if (strcmp(argv[first_name], "--shuffle") == 0 && first_name + 1 < argc) {
            shuffle = true;
            seed = (uint32_t)strtoul(argv[++first_name], NULL, 0);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[first_name]);
            return 2;
        }
        first_name++;
    }

    size_t total = 0;
    for (size_t s = 0; s < sizeof suites / sizeof suites[0]; s++) {
        total += *suites[s].count;
    }
    const test_case_t **order = calloc(total, sizeof *order);
    if (order == NULL) {
        return 2;
    }
    size_t count = 0;
    for (size_t s = 0; s < sizeof suites / sizeof suites[0]; s++) {
        for (size_t i = 0; i < *suites[s].count; i++) {
            if (selected(suites[s].cases[i].name, argc, argv, first_name)) {
                order[count++] = &suites[s].cases[i];
            }
        }
    }

    if (list) {
        for (size_t i = 0; i < count; i++) {
            printf("%s\n", order[i]->name);
        }
        free(order);
        return 0;
    }
    if (count == 0) {
        fprintf(stderr, "no test case matches\n");
        free(order);
        return 2;
    }

    if (shuffle) {
        uint32_t state = seed != 0 ? seed : 1U;
        printf("shuffled with seed %lu\n", (unsigned long)seed);
        for (size_t i = count - 1; i > 0; i--) {
            const size_t j = next_random(&state) % (i + 1);
            const test_case_t *const t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
    }

    size_t failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (!(isolate ? run_case_isolated(order[i]) : run_case(order[i]))) {
            failed++;
        }
    }
    free(order);

    printf("\n%zu of %zu cases passed, %u checks executed\n", count - failed, count, total_checks);
    return failed == 0 ? 0 : 1;
}
