// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//libFuzzer harness for inflate.c. input: a byte for the input piece size, one for the output piece size
//(1 to 256), then raw DEFLATE. TODO(rewrite): differential against zlib's raw inflate (windowBits -15)

#include <stddef.h>
#include <stdint.h>

#include "bartap_internal.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t window[BARTAP_WINDOW_SIZE];
    uint8_t out[256];
    if (size < 2) {
        return 0;
    }
    const size_t in_piece = (size_t)data[0] + 1;
    const size_t out_piece = (size_t)data[1] + 1;
    data += 2;
    size -= 2;

    bartap_inflate_t inflate;
    bartap_inflate_init(&inflate, window);
    size_t pos = 0;
    for (size_t steps = 0; steps < 16 * size + 64; steps++) {
        const size_t piece = size - pos < in_piece ? size - pos : in_piece;
        size_t used = 0;
        size_t made = 0;
        if (bartap_inflate(&inflate, data + pos, piece, &used, out, out_piece, &made) != BARTAP_OK) {
            break;
        }
        pos += used;
    }
    return 0;
}
