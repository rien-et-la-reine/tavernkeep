// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//libFuzzer harness for xml.c: tokenize a document fed in pieces whose size comes from the first byte.
//TODO(rewrite): check offsets never decrease and every byte is consumed once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    size_t piece;
} feed_t;

static bartap_err_t feed(void *user, uint8_t *buf, size_t len, size_t *got)
{
    feed_t *f = user;
    size_t n = f->size - f->pos;
    if (n > len) {
        n = len;
    }
    if (n > f->piece) {
        n = f->piece;
    }
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    *got = n;
    return BARTAP_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) {
        return 0;
    }
    feed_t f = { data + 1, size - 1, 0, (size_t)data[0] + 1 };
    bartap_xml_t xml;
    bartap_xml_init(&xml, feed, &f);
    bartap_xml_token_t token;
    for (size_t steps = 0; steps < 4 * size + 64; steps++) {
        if (bartap_xml_next(&xml, &token) != BARTAP_OK || token.type == BARTAP_XML_END) {
            break;
        }
    }
    return 0;
}
