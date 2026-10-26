// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The book: bartap_open reads META-INF/container.xml for the OPF, then the OPF in passes (1: metadata,
 * the identifier <package unique-identifier> names; 2: the spine, with linear="no"; 3: the manifest,
 * matched to the spine by hash), then finds the TOC (EPUB 3 nav, else the NCX) and the cover. Also the
 * resource streams and the TOC iterator. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source, bartap_spine_item_t *spine,
    uint16_t spine_capacity, bartap_work_t *work)
{
    // TODO(rewrite)
    (void)book;
    (void)source;
    (void)spine;
    (void)spine_capacity;
    (void)work;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_warnings(const bartap_book_t *book)
{
    // TODO(rewrite)
    (void)book;
    return 0;
}

uint16_t bartap_spine_count(const bartap_book_t *book)
{
    // TODO(rewrite)
    (void)book;
    return 0;
}

bool bartap_spine_linear(const bartap_book_t *book, uint16_t spine_index)
{
    // TODO(rewrite)
    (void)book;
    (void)spine_index;
    return false;
}

bartap_err_t bartap_metadata(const bartap_book_t *book, bartap_meta_t field, char *buf, size_t size)
{
    // TODO(rewrite)
    (void)book;
    (void)field;
    (void)buf;
    (void)size;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------ streams */

bartap_err_t bartap_stream_open(bartap_stream_t *stream, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window)
{
    // TODO(rewrite)
    (void)stream;
    (void)book;
    (void)spine_index;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_stream_open_path(bartap_stream_t *stream, const bartap_book_t *book, const char *path,
    uint8_t *window)
{
    // TODO(rewrite)
    (void)stream;
    (void)book;
    (void)path;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_stream_open_cover(bartap_stream_t *stream, const bartap_book_t *book, uint8_t *window)
{
    // TODO(rewrite)
    (void)stream;
    (void)book;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_stream_read(bartap_stream_t *stream, void *buf, size_t len, size_t *got)
{
    // TODO(rewrite): fold every byte delivered into stream->crc32 with bartap_crc32; at the entry's end,
    // BARTAP_END only if it equals stream->entry.crc32 and out_offset equals entry.size, else CORRUPT
    (void)stream;
    (void)buf;
    (void)len;
    (void)got;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_stream_size(const bartap_stream_t *stream)
{
    // TODO(rewrite)
    (void)stream;
    return 0;
}

uint32_t bartap_stream_tell(const bartap_stream_t *stream)
{
    // TODO(rewrite)
    (void)stream;
    return 0;
}

bartap_err_t bartap_stream_rewind(bartap_stream_t *stream)
{
    // TODO(rewrite): crc32 back to BARTAP_CRC32_INIT with the rest of the stream's progress
    (void)stream;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------ table of contents */

bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window)
{
    // TODO(rewrite)
    (void)toc;
    (void)book;
    (void)window;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_toc_entry_t *entry)
{
    // TODO(rewrite)
    (void)toc;
    (void)entry;
    return BARTAP_ERR_UNSUPPORTED;
}
