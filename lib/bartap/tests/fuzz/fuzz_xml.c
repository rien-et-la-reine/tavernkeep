// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The XML tokenizer.
 *
 * Input: a byte choosing the flags (strict, XHTML entities), one the depth limit (1 to 64), one the token
 * buffer's size (32 to 543), one seeding the piece sizes, then the document, fed in pieces of 1 to 64 bytes.
 * Checked on every call: depth never exceeds the limit; every name and value lies inside the token buffer;
 * event positions never decrease and never pass the bytes taken; no call takes more than it was offered.
 * At DONE, every byte was taken exactly once. In lenient mode nothing is CORRUPT: malformed markup is
 * recovered from. A call that takes nothing, gives no event and changes nothing fails after a few in a
 * row (D5), and so does a run of more calls than the input could need. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xml.h"

#define STALL_MAX 4U

static void check_inside(const uint8_t *buffer, size_t size, const uint8_t *p, size_t length, const char *what)
{
    if (p == NULL) {
        if (length != 0U) {
            fprintf(stderr, "%s: NULL with length %zu\n", what, length);
            abort();
        }
        return;
    }
    if (p < buffer || p > buffer + size || length > (size_t)(buffer + size - p)) {
        fprintf(stderr, "%s lies outside the token buffer\n", what);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 4U) {
        return 0;
    }
    const uint32_t flags = data[0] & (BARTAP_XML_FLAG_STRICT | BARTAP_XML_FLAG_XHTML_ENTITIES);
    const uint32_t max_depth = 1U + data[1] % 64U;
    const size_t buffer_size = BARTAP_XML_BUFFER_MIN + data[2];
    uint32_t random = 0x9E3779B9U ^ data[3];
    const uint8_t *doc = data + 4;
    const size_t doc_size = size - 4U;

    uint8_t *buffer = malloc(buffer_size);
    uint32_t stack[64];
    bartap_xml_t xml;
    if (bartap_xml_init(&xml, buffer, buffer_size, stack, max_depth, flags) != BARTAP_XML_RESULT_OK) {
        abort();
    }
    size_t taken = 0;
    uint32_t last_position = 0;
    bartap_xml_t before = xml;
    unsigned stalls = 0;
    const size_t cap = 64U + 4U * doc_size + (size_t)max_depth * doc_size;
    for (size_t calls = 0;; calls++) {
        if (calls > cap) {
            fprintf(stderr, "more than %zu calls\n", cap);
            abort();
        }
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const size_t piece = 1U + random % 64U;
        const size_t n = doc_size - taken < piece ? doc_size - taken : piece;
        size_t used;
        bartap_xml_event_t e;
        const bartap_xml_result_t r = bartap_xml_next(&xml, doc + taken, n, &used, taken + n == doc_size, &e);
        if (used > n) {
            abort();
        }
        taken += used;
        if (r != BARTAP_XML_RESULT_OK) {
            if (r == BARTAP_XML_RESULT_CORRUPT && !(flags & BARTAP_XML_FLAG_STRICT)) {
                fprintf(stderr, "CORRUPT in lenient mode at byte %zu\n", taken);
                abort();
            }
            if (r == BARTAP_XML_RESULT_INVALID_ARGUMENT) {
                abort();
            }
            break;
        }
        if (xml.depth > max_depth || e.depth > max_depth) {
            fprintf(stderr, "depth %u past the limit %u\n", e.depth, max_depth);
            abort();
        }
        if (e.kind == BARTAP_XML_EVENT_DONE) {
            if (taken != doc_size) {
                fprintf(stderr, "DONE with %zu of %zu bytes taken\n", taken, doc_size);
                abort();
            }
            break;
        }
        if (e.kind != BARTAP_XML_EVENT_NONE) {
            check_inside(buffer, buffer_size, e.name, e.name_length, "name");
            check_inside(buffer, buffer_size, e.value, e.value_length, "value");
            if (e.position < last_position || e.position > taken) {
                fprintf(stderr, "position %u after %u, with %zu bytes taken\n", e.position, last_position, taken);
                abort();
            }
            last_position = e.position;
            if (e.kind == BARTAP_XML_EVENT_TEXT && e.value_length == 0U) {
                fprintf(stderr, "an empty TEXT event\n");
                abort();
            }
        }
        if (used == 0U && e.kind == BARTAP_XML_EVENT_NONE && memcmp(&before, &xml, sizeof xml) == 0) {
            if (++stalls > STALL_MAX) {
                fprintf(stderr, "no progress at byte %zu\n", taken);
                abort();
            }
        } else {
            stalls = 0;
        }
        before = xml;
    }
    free(buffer);
    return 0;
}
