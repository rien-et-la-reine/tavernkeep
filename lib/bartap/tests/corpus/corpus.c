// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The corpus runner: the whole-book flow (../common/flow.h) over each EPUB named on the command line,
 * printing one tab-separated line per book for run_corpus.py to summarize:
 *
 *   BOOK path open=.. metadata=.. toc=.. streams=.. text=.. cfi=.. warnings=0x.. text_warnings=0x..
 *        toc_warnings=0x.. spine=N items=N toc_entries=N resolves=N mib=X ms_per_mib=X snapshot_max=N failure=..
 *
 * The time per MiB is from a first run that only reads (open, table of contents, streams, text); the
 * checks (positions, resolving, snapshots) are a second run, whose results are the ones printed.
 *
 * Options, before the paths: --strict (strict markup everywhere), --elements (element and attribute items
 * too), --feed N (bytes fed to the text layer per call; 0 mixes sizes), --resolve-every N, --resolve-max N
 * (per spine item; 0 for no cap), --no-snapshot. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flow.h"

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    uint8_t *data = NULL;
    size_t length = 0;
    size_t capacity = 0;
    for (;;) {
        if (length == capacity) {
            capacity = capacity * 2U + (1U << 20);
            data = realloc(data, capacity);
        }
        const size_t got = fread(data + length, 1, capacity - length, f);
        length += got;
        if (got == 0U) {
            break;
        }
    }
    fclose(f);
    if (length > UINT32_MAX) {
        free(data);
        errno = EFBIG;
        return NULL;
    }
    *size = length;
    return data;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    flow_options_t options;
    flow_options_default(&options);
    options.resolve_max = 64;
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            options.text_flags |= BARTAP_TEXT_STRICT;
            options.open_flags |= BARTAP_EPUB_OPEN_STRICT_MARKUP;
        } else if (strcmp(argv[i], "--feed") == 0 && i + 1 < argc) {
            options.feed = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--resolve-every") == 0 && i + 1 < argc) {
            options.resolve_every = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--resolve-max") == 0 && i + 1 < argc) {
            options.resolve_max = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--elements") == 0) {
            options.text_flags |= BARTAP_TEXT_DELIVER_ELEMENTS | BARTAP_TEXT_DELIVER_ATTRIBUTES;
        } else if (strcmp(argv[i], "--no-snapshot") == 0) {
            options.snapshot_check = false;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i == argc) {
        fprintf(stderr, "usage: %s [options] book.epub...\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (; i < argc; i++) {
        size_t size;
        uint8_t *data = load(argv[i], &size);
        if (data == NULL) {
            printf("BOOK\t%s\tload=%s\n", argv[i], strerror(errno));
            continue;
        }
        flow_options_t light = options;
        light.resolve_every = 0;
        light.snapshot_check = false;
        flow_report_t report;
        const double start = now();
        flow_run(data, size, &light, &report);
        const double seconds = now() - start;
        const double mib = (double)report.content_bytes / (1024.0 * 1024.0);
        flow_run(data, size, &options, &report);
        printf("BOOK\t%s\topen=%s\tmetadata=%s\ttoc=%s\tstreams=%s\ttext=%s\tcfi=%s\twarnings=0x%x\t"
            "text_warnings=0x%x\ttoc_warnings=0x%x\tspine=%u\titems=%llu\ttoc_entries=%llu\tresolves=%llu\t"
            "mib=%.3f\tms_per_mib=%.1f\tsnapshot_max=%zu\tfailure=%s\n",
            argv[i], flow_result_name(report.open), flow_result_name(report.metadata), flow_result_name(report.toc),
            flow_result_name(report.streams), flow_result_name(report.text), flow_result_name(report.cfi),
            report.warnings, report.text_warnings, report.toc_warnings, report.spine_count,
            (unsigned long long)report.items, (unsigned long long)report.toc_entries,
            (unsigned long long)report.resolves, mib, mib > 0.0 ? seconds * 1000.0 / mib : 0.0,
            report.snapshot_largest, report.failure);
        fflush(stdout);
        failures += report.failure[0] != '\0';
        free(data);
    }
    return failures == 0 ? 0 : 1;
}
