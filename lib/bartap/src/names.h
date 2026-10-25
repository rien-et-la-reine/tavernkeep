// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_NAMES_H
#define BARTAP_NAMES_H

/* Small helpers for names and attribute values (internal), shared by the book, CFI and text layers. Lengths
 * are always explicit: no str* function is used, and a loop that looks for a terminator could be compiled
 * into a call to one. Literals are passed with BARTAP_LITERAL, which supplies their length. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xml.h"

#define BARTAP_LITERAL(s) (s), (sizeof(s) - 1U)

static inline bool bartap_bytes_are(const uint8_t *data, size_t length, const char *text, size_t text_length)
{
    return length == text_length && memcmp(data, text, text_length) == 0;
}

//where the local part of a qualified name starts: after its last ':'
static inline size_t bartap_local_start(const uint8_t *name, size_t length)
{
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        if (name[i] == ':') {
            start = i + 1U;
        }
    }
    return start;
}

static inline bool bartap_local_is(const uint8_t *name, size_t length, const char *local, size_t local_length)
{
    const size_t start = bartap_local_start(name, length);
    return bartap_bytes_are(name + start, length - start, local, local_length);
}

//whether a whitespace-separated list holds token. A list cut short (truncated) does not count its last token,
//which may be only part of one
static inline bool bartap_has_token(const uint8_t *list, size_t length, bool truncated, const char *token,
    size_t token_length)
{
    size_t i = 0;
    while (i < length) {
        while (i < length && list[i] == ' ') {
            i++;
        }
        size_t j = i;
        while (j < length && list[j] != ' ') {
            j++;
        }
        if (j > i && !(truncated && j == length) && bartap_bytes_are(list + i, j - i, token, token_length)) {
            return true;
        }
        i = j;
    }
    return false;
}

static inline uint32_t bartap_hash_bytes(const uint8_t *data, size_t size)
{
    uint32_t h = BARTAP_XML_HASH_INIT;
    for (size_t i = 0; i < size; i++) {
        h = bartap_xml_hash_byte(h, data[i]);
    }
    return h;
}

#endif
