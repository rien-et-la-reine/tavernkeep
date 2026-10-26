// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Streaming DEFLATE (RFC 1951), puff-style canonical Huffman decoding over a caller-owned 32 KiB window,
 * with an optional 8-bit fast lookup table for literal/length codes (BARTAP_INFLATE_FAST_LUT). */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

void bartap_inflate_init(bartap_inflate_t *inflate, uint8_t *window)
{
    // TODO(rewrite)
    (void)inflate;
    (void)window;
}

bartap_err_t bartap_inflate(bartap_inflate_t *inflate, const uint8_t *in, size_t in_len, size_t *in_used,
    uint8_t *out, size_t out_len, size_t *out_made)
{
    // TODO(rewrite)
    (void)inflate;
    (void)in;
    (void)in_len;
    (void)in_used;
    (void)out;
    (void)out_len;
    (void)out_made;
    return BARTAP_ERR_UNSUPPORTED;
}
