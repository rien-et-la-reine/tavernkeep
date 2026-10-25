// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The OCF layer, over a memory byte source holding the input.
 *
 * Opens the archive (with a 64-byte scratch buffer or a 4 KiB one, by a bit of the input's length), then
 * reads every entry to its end twice: with 1-byte input and output buffers, and with large ones. Both reads
 * must end the same way and, where they get that far, deliver the same bytes; a read that reaches *end has
 * its CRC-32 and size checked by the layer itself. Entries are capped at 1 MiB (LIMIT_EXCEEDED past it).
 * Every step loop has a cap and a no-progress check (D5). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ocf.h"

#define ENTRY_CAP (1U << 20)
#define STALL_MAX 4U

typedef struct {
    bartap_ocf_result_t result;
    uint8_t *data;
    size_t length;
} read_t;

static void read_entry(const bartap_ocf_archive_t *archive, const bartap_ocf_entry_t *entry, uint8_t *window,
    size_t input_size, size_t output_size, size_t source_size, read_t *out)
{
    out->data = malloc(ENTRY_CAP);
    out->length = 0;
    uint8_t *input = malloc(input_size);
    uint8_t *buffer = malloc(output_size);
    bartap_ocf_stream_t stream;
    out->result = bartap_ocf_entry_open(&stream, archive, entry, ENTRY_CAP, window, BARTAP_INFLATE_WINDOW_SIZE,
        input, input_size);
    bool end = false;
    bartap_ocf_stream_t before = stream;
    unsigned stalls = 0;
    //each call refills the input or decodes from it: compressed bytes plus output bytes bound the calls
    const size_t cap = 64U + 2U * (source_size + ENTRY_CAP);
    for (size_t calls = 0; out->result == BARTAP_OCF_RESULT_OK && !end; calls++) {
        if (calls > cap) {
            fprintf(stderr, "entry %u: more than %zu reads\n", entry->index, cap);
            abort();
        }
        size_t got;
        out->result = bartap_ocf_entry_read(&stream, buffer, output_size, &got, &end);
        if (got > output_size || out->length + got > ENTRY_CAP) {
            abort();
        }
        memcpy(out->data + out->length, buffer, got);
        out->length += got;
        if (got == 0U && !end && memcmp(&before, &stream, sizeof stream) == 0) {
            if (++stalls > STALL_MAX) {
                fprintf(stderr, "entry %u: no progress\n", entry->index);
                abort();
            }
        } else {
            stalls = 0;
        }
        before = stream;
    }
    free(buffer);
    free(input);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > UINT32_MAX) {
        return 0;
    }
    bartap_byte_source_memory_t memory;
    bartap_byte_source_t source;
    bartap_byte_source_memory_init(&memory, data, (bartap_offset_t)size, &source);
    const size_t scratch_size = (size & 1U) ? BARTAP_OCF_OPEN_SCRATCH_MIN : 4096U;
    uint8_t *scratch = malloc(scratch_size);
    bartap_ocf_opener_t opener;
    bartap_ocf_archive_t archive;
    bartap_ocf_result_t r = bartap_ocf_open_begin(&opener, &archive, &source, scratch, scratch_size);
    bool done = false;
    bartap_ocf_opener_t before = opener;
    unsigned stalls = 0;
    const size_t cap = 1024U + 4U * size;
    for (size_t steps = 0; r == BARTAP_OCF_RESULT_OK && !done; steps++) {
        if (steps > cap) {
            fprintf(stderr, "open: more than %zu steps\n", cap);
            abort();
        }
        r = bartap_ocf_open_step(&opener, &done);
        if (r == BARTAP_OCF_RESULT_OK && !done && memcmp(&before, &opener, sizeof opener) == 0 && ++stalls > STALL_MAX) {
            fprintf(stderr, "open: no progress\n");
            abort();
        }
        before = opener;
    }
    if (!done || opener.scratch != NULL) {
        if (opener.scratch != NULL) {
            fprintf(stderr, "open finished but kept the scratch buffer\n");
            abort();
        }
        free(scratch);
        return 0;
    }
    free(scratch);

    uint8_t *window = malloc(BARTAP_INFLATE_WINDOW_SIZE);
    bartap_ocf_cursor_t cursor;
    bartap_ocf_cursor_start(&archive, &cursor);
    bool end = false;
    for (size_t steps = 0; !end; steps++) {
        if (steps > cap) {
            fprintf(stderr, "listing: more than %zu steps\n", cap);
            abort();
        }
        bartap_ocf_entry_t entry;
        bool ready;
        if (bartap_ocf_next(&archive, &cursor, &entry, &ready, &end) != BARTAP_OCF_RESULT_OK) {
            break;
        }
        if (!ready) {
            continue;
        }
        read_t small;
        read_t large;
        read_entry(&archive, &entry, window, 1, 1, size, &small);
        read_entry(&archive, &entry, window, 4096, 4096, size, &large);
        //a deflated entry read a byte at a time may stop at a different byte than one read in large pieces,
        //but it must stop with the same verdict, and what both delivered agrees
        const size_t common = small.length < large.length ? small.length : large.length;
        if (small.result != large.result || memcmp(small.data, large.data, common) != 0
                || (small.result == BARTAP_OCF_RESULT_OK && small.length != large.length)) {
            fprintf(stderr, "entry %u: small reads %d (%zu bytes), large %d (%zu bytes)\n", entry.index,
                small.result, small.length, large.result, large.length);
            abort();
        }
        free(small.data);
        free(large.data);
    }
    free(window);
    return 0;
}
