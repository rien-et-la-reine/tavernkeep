// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Inflate, against zlib's raw inflate (windowBits -15) as the oracle.
 *
 * Input: one byte choosing the input piece size, one the output piece size (both 1 to 256, 1 included),
 * then a raw DEFLATE stream. Bartap's decoder is fed in those pieces; zlib gets the whole stream at once.
 * Both must agree on the verdict - decodes, truncated, or corrupt - since inflate.h accepts exactly what
 * zlib accepts, and on the output when both decode. Output is capped (a small stream may expand a
 * thousandfold); a stream that reaches the cap is compared only up to it. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "inflate.h"

#define OUTPUT_CAP (1U << 20)
#define STALL_MAX 4U

enum { DECODES, TRUNCATED, CORRUPT, CAPPED };

static const char *verdict_name(int v)
{
    return v == DECODES ? "decodes" : v == TRUNCATED ? "truncated" : v == CORRUPT ? "corrupt" : "capped";
}

static int zlib_inflate(const uint8_t *in, size_t size, uint8_t *out, size_t *produced)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) {
        abort();
    }
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)size;
    z.next_out = out;
    z.avail_out = OUTPUT_CAP;
    const int r = inflate(&z, Z_FINISH);
    *produced = OUTPUT_CAP - z.avail_out;
    inflateEnd(&z);
    if (r == Z_STREAM_END) {
        return DECODES;
    }
    if (r == Z_DATA_ERROR) {
        return CORRUPT;
    }
    return z.avail_out == 0U ? CAPPED : TRUNCATED;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2U) {
        return 0;
    }
    const size_t in_piece = 1U + data[0];
    const size_t out_piece = 1U + data[1];
    const uint8_t *in = data + 2;
    const size_t in_size = size - 2U;

    uint8_t *expected = malloc(OUTPUT_CAP);
    size_t expected_size;
    const int oracle = zlib_inflate(in, in_size, expected, &expected_size);

    uint8_t *window = malloc(BARTAP_INFLATE_WINDOW_SIZE);
    uint8_t *out = malloc(OUTPUT_CAP);
    bartap_inflate_t state;
    if (bartap_inflate_init(&state, window, BARTAP_INFLATE_WINDOW_SIZE) != BARTAP_INFLATE_RESULT_OK) {
        abort();
    }
    size_t taken = 0;
    size_t produced = 0;
    int verdict = CAPPED;
    bartap_inflate_t before = state;
    unsigned stalls = 0;
    //every call takes input or gives output: at most this many calls, with room to report the end
    const size_t cap = 2U * (in_size + OUTPUT_CAP) + 16U;
    for (size_t calls = 0;; calls++) {
        if (calls > cap) {
            fprintf(stderr, "more than %zu calls\n", cap);
            abort();
        }
        const size_t n = in_size - taken < in_piece ? in_size - taken : in_piece;
        const size_t m = OUTPUT_CAP - produced < out_piece ? OUTPUT_CAP - produced : out_piece;
        if (m == 0U) {
            break;                                      //capped
        }
        size_t used;
        size_t got;
        const bartap_inflate_result_t r = bartap_inflate_run(&state, in + taken, n, &used, taken + n == in_size,
            out + produced, m, &got);
        if (used > n || got > m) {
            abort();
        }
        taken += used;
        produced += got;
        if (r == BARTAP_INFLATE_RESULT_DONE) {
            verdict = DECODES;
            break;
        }
        if (r == BARTAP_INFLATE_RESULT_TRUNCATED) {
            verdict = TRUNCATED;
            break;
        }
        if (r != BARTAP_INFLATE_RESULT_OK) {
            verdict = CORRUPT;
            break;
        }
        if (used == 0U && got == 0U && memcmp(&before, &state, sizeof state) == 0 && ++stalls > STALL_MAX) {
            fprintf(stderr, "no progress at input byte %zu, output byte %zu\n", taken, produced);
            abort();
        } else if (used > 0U || got > 0U) {
            stalls = 0;
        }
        before = state;
    }
    if (verdict != CAPPED && oracle != CAPPED && verdict != oracle) {
        fprintf(stderr, "Bartap: %s; zlib: %s\n", verdict_name(verdict), verdict_name(oracle));
        abort();
    }
    //what both produced agrees as far as both went; in full when both decode
    const size_t common = produced < expected_size ? produced : expected_size;
    if (memcmp(out, expected, common) != 0 || (verdict == DECODES && oracle == DECODES && produced != expected_size)) {
        fprintf(stderr, "output differs: Bartap %zu bytes, zlib %zu\n", produced, expected_size);
        abort();
    }
    free(out);
    free(window);
    free(expected);
    return 0;
}
