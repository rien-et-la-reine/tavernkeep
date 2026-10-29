// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Reads through a bartap_source_t, the memory source, and the helpers every module shares. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

const char *bartap_strerror(bartap_err_t err)
{
    switch (err) {
    case BARTAP_OK:
        return "ok";
    case BARTAP_END:
        return "end of stream";
    case BARTAP_ERR_ARG:
        return "invalid argument";
    case BARTAP_ERR_IO:
        return "input/output error";
    case BARTAP_ERR_NOT_EPUB:
        return "not an EPUB archive";
    case BARTAP_ERR_CORRUPT:
        return "corrupt archive or stream";
    case BARTAP_ERR_UNSUPPORTED:
        return "unsupported feature";
    case BARTAP_ERR_NOT_FOUND:
        return "not found";
    case BARTAP_ERR_LIMIT:
        return "fixed limit exceeded";
    }
    return "unknown error";
}

static int bartap_memory_read(void *user, uint32_t offset, void *buf, size_t len)
{
    const uint8_t *const base = (const uint8_t *)(uintptr_t)user;
    if (len > 0U) {
        if (base == NULL || buf == NULL) {
            return -1;
        }
        memcpy(buf, base + offset, len);
    }
    return 0;
}

void bartap_source_memory(bartap_source_t *source, const void *data, uint32_t size)
{
    if (source == NULL) {
        return;
    }
    source->read = bartap_memory_read;
    source->user = (void *)(uintptr_t)data;
    source->size = data != NULL ? size : 0U;
}

bartap_err_t bartap_source_read(const bartap_source_t *source, uint32_t offset, void *buf, size_t len)
{
    if (source == NULL || source->read == NULL || (buf == NULL && len > 0U)) {
        return BARTAP_ERR_ARG;
    }
    if (len > source->size || offset > source->size - (uint32_t)len) {
        return BARTAP_ERR_CORRUPT;
    }
    if (len == 0U) {
        return BARTAP_OK;
    }
    if (source->read(source->user, offset, buf, len) != 0) {
        return BARTAP_ERR_IO;
    }
    return BARTAP_OK;
}

uint32_t bartap_hash(const void *data, size_t len)
{
    const uint8_t *const bytes = data;
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

static size_t bartap_utf8_lead_len(uint8_t lead)
{
    if ((lead & 0x80U) == 0x00U) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 1U;
}

bool bartap_copy_utf8(char *dst, size_t size, const char *src, size_t len)
{
    if (dst == NULL || size == 0U) {
        return len > 0U;
    }
    if (src == NULL || len == 0U) {
        dst[0] = '\0';
        return false;
    }
    if (len < size) {
        memcpy(dst, src, len);
        dst[len] = '\0';
        return false;
    }

    size_t cut = size - 1U;
    if (cut > 0U && ((uint8_t)src[cut] & 0xC0U) == 0x80U) {
        size_t start = cut;
        while (start > 0U && (cut - start) < 3U && ((uint8_t)src[start] & 0xC0U) == 0x80U) {
            start--;
        }
        if (((uint8_t)src[start] & 0xC0U) == 0xC0U) {
            const size_t seq_len = bartap_utf8_lead_len((uint8_t)src[start]);
            if (start + seq_len > cut) {
                cut = start;
            }
        }
    }

    if (cut > 0U) {
        memcpy(dst, src, cut);
    }
    dst[cut] = '\0';
    return true;
}

void bartap_path_dir(const char *path, char dir[BARTAP_PATH_MAX + 1])
{
    const char *last_slash = NULL;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            last_slash = p;
        }
    }
    if (last_slash == NULL) {
        dir[0] = '\0';
        return;
    }
    size_t len = (size_t)(last_slash - path + 1);
    if (len > BARTAP_PATH_MAX) {
        len = BARTAP_PATH_MAX;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
}

static int bartap_hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode URI bytes exactly once. A truncated or NUL-containing name cannot identify a resource. In a
 * path, an escaped '/' (%2F) is data within a segment (RFC 3986 2.2), which no archive name can hold, so
 * it is refused rather than decoded into a separator. */
static bartap_err_t bartap_decode_uri(const char *src, size_t len, char *out, size_t capacity,
    bool is_path)
{
    size_t used = 0U;
    for (size_t i = 0U; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%' && len - i >= 3U) {
            const int hi = bartap_hex_digit(src[i + 1U]);
            const int lo = bartap_hex_digit(src[i + 2U]);
            if (hi >= 0 && lo >= 0) {
                c = (unsigned char)((hi << 4) | lo);
                i += 2U;
                if (is_path && c == '/') {
                    out[0] = '\0';
                    return BARTAP_ERR_CORRUPT;
                }
            }
        }
        if (c == 0U) {
            out[0] = '\0';
            return BARTAP_ERR_CORRUPT;
        }
        if (used + 1U >= capacity) {
            out[0] = '\0';
            return BARTAP_ERR_LIMIT;
        }
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return BARTAP_OK;
}

bartap_err_t bartap_resolve_href(const char *base_dir, const char *href,
    char out[BARTAP_PATH_MAX + 1], char *frag, size_t frag_size)
{
    if (href == NULL || out == NULL || (frag != NULL && frag_size == 0U)) {
        return BARTAP_ERR_ARG;
    }
    if (frag != NULL) {
        frag[0] = '\0';
    }
    const char *hash = strchr(href, '#');
    const char *query = strchr(href, '?');
    const char *end = href + strlen(href);
    if (hash != NULL) {
        end = hash;
        if (frag != NULL) {
            const bartap_err_t err = bartap_decode_uri(hash + 1, strlen(hash + 1), frag, frag_size, false);
            if (err != BARTAP_OK) {
                return err;
            }
        }
    }
    if (query != NULL && query < end) {
        end = query;
    }
    /* External URIs are not archive paths. */
    for (const char *p = href; p < end && *p != '/'; p++) {
        if (*p == ':') {
            return BARTAP_ERR_UNSUPPORTED;
        }
    }
    size_t prefix = href[0] != '/' && base_dir != NULL ? strlen(base_dir) : 0U;
    if (prefix > BARTAP_PATH_MAX) {
        return BARTAP_ERR_LIMIT;
    }
    if (prefix != 0U) {
        memcpy(out, base_dir, prefix);
    }
    bartap_err_t err = bartap_decode_uri(href, (size_t)(end - href), out + prefix,
        BARTAP_PATH_MAX + 1U - prefix, true);
    if (err != BARTAP_OK) {
        return err;
    }
    /* Normalize decoded segments in place; write never overtakes read. */
    size_t read = 0U, write = 0U;
    while (out[read] != '\0') {
        while (out[read] == '/') {
            read++;
        }
        const size_t start = read;
        while (out[read] != '\0' && out[read] != '/') {
            read++;
        }
        const size_t n = read - start;
        if (n == 0U || (n == 1U && out[start] == '.')) {
            continue;
        }
        if (n == 2U && out[start] == '.' && out[start + 1U] == '.') {
            if (write == 0U) {
                return BARTAP_ERR_CORRUPT;
            }
            while (write > 0U && out[write - 1U] != '/') {
                write--;
            }
            if (write > 0U) {
                write--;
            }
        } else {
            if (write > 0U) {
                out[write++] = '/';
            }
            memmove(out + write, out + start, n);
            write += n;
        }
    }
    out[write] = '\0';
    return BARTAP_OK;
}
