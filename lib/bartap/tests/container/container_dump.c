// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The container dumper for check_container.py, which compares its output with make_fixtures.py's manifest:
 *
 *   bartap_container_dump deflate FILE         RESULT verdict length crc32
 *   bartap_container_dump zip FILE SCRATCH     OPEN result warnings
 *                                              ENTRY name-as-hex result length crc32 (one per entry)
 *                                              LIST result
 *
 * A DEFLATE stream is decoded twice, in one piece and a byte at a time (1-byte output too); a zip entry is
 * read twice, with 1-byte buffers and with large ones. Both ways must agree, or the dumper says so on a
 * MISMATCH line. Results are the internal layers' names; the CRC-32 is zlib's, over what was delivered. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc.h"
#include "inflate.h"
#include "ocf.h"

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(n > 0 ? (size_t)n : 1U);
    if (n < 0 || fread(data, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static const char *ocf_name(bartap_ocf_result_t r)
{
    static const char *const names[] = { "ok", "invalid argument", "not a zip", "corrupt", "unsupported",
        "not found", "io error", "limit exceeded", "not an epub" };
    return (unsigned)r < sizeof names / sizeof names[0] ? names[r] : "?";
}

//decode in pieces of in_piece and out_piece bytes; the verdict, and the output's length and CRC-32
static const char *inflate_all(const uint8_t *in, size_t size, size_t in_piece, size_t out_piece, size_t *length,
    uint32_t *crc)
{
    static uint8_t window[BARTAP_INFLATE_WINDOW_SIZE];
    bartap_inflate_t state;
    (void)bartap_inflate_init(&state, window, sizeof window);
    uint8_t out[4096];
    size_t taken = 0;
    *length = 0;
    *crc = BARTAP_CRC32_INIT;
    for (;;) {
        const size_t n = size - taken < in_piece ? size - taken : in_piece;
        size_t used;
        size_t got;
        const bartap_inflate_result_t r = bartap_inflate_run(&state, in + taken, n, &used, taken + n == size, out,
            out_piece < sizeof out ? out_piece : sizeof out, &got);
        taken += used;
        *length += got;
        *crc = bartap_crc32_update(*crc, out, got);
        if (r != BARTAP_INFLATE_RESULT_OK) {
            *crc = bartap_crc32_final(*crc);
            return r == BARTAP_INFLATE_RESULT_DONE ? "decodes" : r == BARTAP_INFLATE_RESULT_TRUNCATED ? "truncated"
                : "corrupt";
        }
    }
}

static int dump_deflate(const char *path)
{
    size_t size;
    uint8_t *data = load(path, &size);
    size_t a_length;
    size_t b_length;
    uint32_t a_crc;
    uint32_t b_crc;
    const char *a = inflate_all(data, size, size + 1U, 4096, &a_length, &a_crc);
    const char *b = inflate_all(data, size, 1, 1, &b_length, &b_crc);
    printf("RESULT\t%s\t%zu\t%u\n", a, a_length, a_crc);
    if (strcmp(a, b) != 0 || a_length != b_length || a_crc != b_crc) {
        printf("MISMATCH\ta byte at a time: %s %zu %u\n", b, b_length, b_crc);
    }
    free(data);
    return 0;
}

static bartap_ocf_result_t read_entry(const bartap_ocf_archive_t *archive, const bartap_ocf_entry_t *entry,
    size_t piece, size_t *length, uint32_t *crc)
{
    static uint8_t window[BARTAP_INFLATE_WINDOW_SIZE];
    uint8_t input[4096];
    uint8_t out[4096];
    bartap_ocf_stream_t stream;
    bartap_ocf_result_t r = bartap_ocf_entry_open(&stream, archive, entry, 1U << 26, window, sizeof window, input,
        piece);
    *length = 0;
    *crc = BARTAP_CRC32_INIT;
    bool end = false;
    while (r == BARTAP_OCF_RESULT_OK && !end) {
        size_t got;
        r = bartap_ocf_entry_read(&stream, out, piece, &got, &end);
        *length += got;
        *crc = bartap_crc32_update(*crc, out, got);
    }
    *crc = bartap_crc32_final(*crc);
    return r;
}

static int dump_zip(const char *path, size_t scratch_size)
{
    size_t size;
    uint8_t *data = load(path, &size);
    bartap_byte_source_memory_t memory;
    bartap_byte_source_t source;
    bartap_byte_source_memory_init(&memory, data, (bartap_offset_t)size, &source);
    uint8_t *scratch = malloc(scratch_size);
    bartap_ocf_opener_t opener;
    bartap_ocf_archive_t archive;
    bartap_ocf_result_t r = bartap_ocf_open(&opener, &archive, &source, scratch, scratch_size);
    printf("OPEN\t%s\t%u\n", ocf_name(r), r == BARTAP_OCF_RESULT_OK ? archive.warnings : 0U);
    if (r == BARTAP_OCF_RESULT_OK) {
        bartap_ocf_cursor_t cursor;
        bartap_ocf_cursor_start(&archive, &cursor);
        bool end = false;
        while (r == BARTAP_OCF_RESULT_OK && !end) {
            bartap_ocf_entry_t entry;
            bool ready;
            r = bartap_ocf_next(&archive, &cursor, &entry, &ready, &end);
            if (r != BARTAP_OCF_RESULT_OK || !ready) {
                continue;
            }
            uint8_t name[65536];
            if (bartap_ocf_entry_name(&archive, &entry, name, sizeof name) != BARTAP_OCF_RESULT_OK) {
                name[0] = 0;
                entry.name_length = 0;
            }
            size_t a_length;
            size_t b_length;
            uint32_t a_crc;
            uint32_t b_crc;
            const bartap_ocf_result_t a = read_entry(&archive, &entry, 4096, &a_length, &a_crc);
            const bartap_ocf_result_t b = read_entry(&archive, &entry, 1, &b_length, &b_crc);
            printf("ENTRY\t");
            for (size_t i = 0; i < entry.name_length; i++) {
                printf("%02x", name[i]);
            }
            printf("\t%s\t%zu\t%u\n", ocf_name(a), a_length, a_crc);
            if (a != b || (a == BARTAP_OCF_RESULT_OK && (a_length != b_length || a_crc != b_crc))) {
                printf("MISMATCH\ta byte at a time: %s %zu %u\n", ocf_name(b), b_length, b_crc);
            }
        }
        printf("LIST\t%s\n", ocf_name(r));
    }
    free(scratch);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "deflate") == 0) {
        return dump_deflate(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "zip") == 0) {
        return dump_zip(argv[2], strtoul(argv[3], NULL, 10));
    }
    fprintf(stderr, "usage: %s deflate FILE | zip FILE SCRATCH_SIZE\n", argv[0]);
    return 2;
}
