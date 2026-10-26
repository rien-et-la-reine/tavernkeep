// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Reads through a bartap_source_t, the memory source, and the helpers every module shares. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

const char *bartap_strerror(bartap_err_t err)
{
    // TODO(rewrite): one string per bartap_err_t
    (void)err;
    return "unknown";
}

void bartap_source_memory(bartap_source_t *source, const void *data, uint32_t size)
{
    // TODO(rewrite)
    (void)source;
    (void)data;
    (void)size;
}

bartap_err_t bartap_source_read(const bartap_source_t *source, uint32_t offset, void *buf, size_t len)
{
    // TODO(rewrite): bounds-check offset + len against source->size before calling read
    (void)source;
    (void)offset;
    (void)buf;
    (void)len;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_hash(const void *data, size_t len)
{
    // TODO(rewrite): FNV-1a, offset basis 2166136261, prime 16777619
    (void)data;
    (void)len;
    return 0;
}

bool bartap_copy_utf8(char *dst, size_t size, const char *src, size_t len)
{
    // TODO(rewrite): walk the cut back over continuation bytes (10xxxxxx)
    (void)dst;
    (void)size;
    (void)src;
    (void)len;
    return false;
}
