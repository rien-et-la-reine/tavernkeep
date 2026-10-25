// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_SNAPSHOT_H
#define BARTAP_SNAPSHOT_H

/* Snapshot encoding (internal): fields written one after another as little-endian bytes, whatever the
 * host's byte order and alignment (D7), and read back with every access bounds-checked. A writer that runs
 * out of room, or a reader that runs out of input, remembers it and does nothing more; the caller checks
 * once at the end. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t length;
    bool overflow;
} bartap_writer_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t at;
    bool short_input;
} bartap_reader_t;

static inline void bartap_put_bytes(bartap_writer_t *w, const void *data, size_t size)
{
    if (w->overflow || size > w->size - w->length) {
        w->overflow = true;
        return;
    }
    if (size > 0U) {
        memcpy(w->data + w->length, data, size);
    }
    w->length += size;
}

static inline void bartap_put8(bartap_writer_t *w, uint32_t value)
{
    const uint8_t b = (uint8_t)value;
    bartap_put_bytes(w, &b, 1);
}

static inline void bartap_put16(bartap_writer_t *w, uint32_t value)
{
    const uint8_t b[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    bartap_put_bytes(w, b, sizeof b);
}

static inline void bartap_put32(bartap_writer_t *w, uint32_t value)
{
    const uint8_t b[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    bartap_put_bytes(w, b, sizeof b);
}

//the next size bytes, or NULL (and short_input set) if there are not that many
static inline const uint8_t *bartap_get_bytes(bartap_reader_t *r, size_t size)
{
    if (r->short_input || size > r->length - r->at) {
        r->short_input = true;
        return NULL;
    }
    const uint8_t *p = r->data + r->at;
    r->at += size;
    return p;
}

static inline uint32_t bartap_get8(bartap_reader_t *r)
{
    const uint8_t *p = bartap_get_bytes(r, 1);
    return p == NULL ? 0U : p[0];
}

static inline uint32_t bartap_get16(bartap_reader_t *r)
{
    const uint8_t *p = bartap_get_bytes(r, 2);
    return p == NULL ? 0U : (uint32_t)(p[0] | ((uint32_t)p[1] << 8));
}

static inline uint32_t bartap_get32(bartap_reader_t *r)
{
    const uint8_t *p = bartap_get_bytes(r, 4);
    return p == NULL ? 0U : p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif
