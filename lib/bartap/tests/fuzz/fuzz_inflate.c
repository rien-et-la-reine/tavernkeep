// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The first two bytes select input/output chunk sizes. zlib is a host-only oracle; cap its
 * output at 1 MiB so fuzzing a small compression bomb cannot consume unbounded test memory. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "bartap_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t window[BARTAP_WINDOW_SIZE];
    static uint8_t reference[1024U * 1024U];
    if (size < 2U || size > 1024U * 1024U) return 0;
    const size_t in_piece = (size_t)data[0] + 1U;
    const size_t out_piece = (size_t)data[1] + 1U;
    data += 2;
    size -= 2;
    z_stream z = {0};
    z.next_in = (Bytef *)(uintptr_t)data;
    z.avail_in = (uInt)size;
    z.next_out = reference;
    z.avail_out = sizeof reference;
    if (inflateInit2(&z, -15) != Z_OK) abort();
    const int verdict = inflate(&z, Z_FINISH);
    const size_t expected = z.total_out;
    (void)inflateEnd(&z);

    bartap_inflate_t inf;
    bartap_inflate_init(&inf, window);
    size_t pos = 0U, total = 0U;
    bartap_err_t err = BARTAP_OK;
    uint8_t out[256];
    while (err == BARTAP_OK && total < sizeof reference) {
        const size_t piece = size - pos < in_piece ? size - pos : in_piece;
        size_t used, made;
        err = bartap_inflate(&inf, data + pos, piece, &used, out, out_piece, &made);
        if (used > piece || made > out_piece) abort();
        pos += used;
        if (verdict == Z_STREAM_END && (total + made > expected
                || memcmp(reference + total, out, made) != 0)) abort();
        total += made;
        if (used == 0U && made == 0U) break;
    }
    if (total < sizeof reference) {
        if (verdict == Z_STREAM_END && (err != BARTAP_END || total != expected)) abort();
        if (verdict == Z_DATA_ERROR && err == BARTAP_END) abort();
    }
    return 0;
}
