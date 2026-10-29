// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* The book: bartap_open reads META-INF/container.xml for the OPF, then the OPF in two passes (1: the
 * metadata and the spine's idrefs, with linear="no"; 2: the manifest, matched to the spine by the idref's
 * hash and length), then walks the central directory once to place each spine item, and finds the TOC
 * (EPUB 3 nav, else the NCX) and the cover. Also the resource streams and the TOC iterator. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

#define BARTAP_SPINE_MANIFEST_MATCHED (1U << 14)
#define BARTAP_SPINE_ZIP_RESOLVED     (1U << 15)

static bool bartap_prop_has_token(const char *props, const char *token)
{
    const size_t tlen = strlen(token);
    const char *p = props;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            p++;
        }
        if ((size_t)(p - start) == tlen && memcmp(start, token, tlen) == 0) {
            return true;
        }
    }
    return false;
}

bartap_err_t bartap_entry_base_dir(const bartap_source_t *source, uint32_t header_offset,
    char base_dir[BARTAP_PATH_MAX + 1])
{
    uint8_t lfh[30];
    bartap_err_t err = bartap_source_read(source, header_offset, lfh, sizeof lfh);
    if (err != BARTAP_OK) {
        return err;
    }
    if (memcmp(lfh, "PK\3\4", 4U) != 0) {
        return BARTAP_ERR_CORRUPT;
    }
    const uint16_t fn_len = (uint16_t)((uint16_t)lfh[26] | ((uint16_t)lfh[27] << 8U));
    if (fn_len > BARTAP_PATH_MAX) {
        return BARTAP_ERR_LIMIT;
    }
    char path[BARTAP_PATH_MAX + 1];
    err = bartap_source_read(source, header_offset + 30U, path, fn_len);
    if (err != BARTAP_OK) {
        return err;
    }
    path[fn_len] = '\0';
    bartap_path_dir(path, base_dir);
    return BARTAP_OK;
}

/* ------------------------------------------------------------ streams */

static bartap_err_t bartap_stream_init_entry(bartap_stream_t *stream, const bartap_source_t *source,
    const bartap_entry_t *entry, uint8_t *window)
{
    if (stream == NULL || source == NULL || entry == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (entry->method == 8U && window == NULL) {
        return BARTAP_ERR_ARG;
    }
    memset(stream, 0, sizeof *stream);
    stream->source = source;
    stream->entry = *entry;
    stream->crc32 = BARTAP_CRC32_INIT;
    if (entry->method == 8U) {
        bartap_inflate_init(&stream->inflate, window);
    }
    return BARTAP_OK;
}

bartap_err_t bartap_stream_open(bartap_stream_t *stream, const bartap_book_t *book, uint16_t spine_index,
    uint8_t *window)
{
    if (stream == NULL || book == NULL || spine_index >= book->spine_count) {
        return BARTAP_ERR_ARG;
    }
    const bartap_spine_item_t *const item = &book->spine[spine_index];
    bartap_entry_t entry;
    entry.header_offset = item->entry_offset;
    entry.data_offset = 0U;
    entry.compressed_size = item->compressed_size;
    entry.size = item->size;
    entry.crc32 = item->crc32;
    entry.method = item->method;

    if (entry.header_offset > book->zip.cd_offset
            || book->zip.cd_offset - entry.header_offset < 30U) {
        return BARTAP_ERR_CORRUPT;
    }
    uint8_t lfh[30];
    const bartap_err_t err = bartap_source_read(book->source, entry.header_offset, lfh, sizeof lfh);
    if (err != BARTAP_OK) {
        return err;
    }
    const uint16_t fn_len = (uint16_t)((uint16_t)lfh[26] | ((uint16_t)lfh[27] << 8U));
    const uint16_t extra_len = (uint16_t)((uint16_t)lfh[28] | ((uint16_t)lfh[29] << 8U));
    const uint32_t hdr_size = 30U + (uint32_t)fn_len + (uint32_t)extra_len;
    if (book->zip.cd_offset - entry.header_offset < hdr_size) {
        return BARTAP_ERR_CORRUPT;
    }
    entry.data_offset = entry.header_offset + hdr_size;
    if (entry.compressed_size > book->zip.cd_offset - entry.data_offset) {
        return BARTAP_ERR_CORRUPT;
    }
    return bartap_stream_init_entry(stream, book->source, &entry, window);
}

bartap_err_t bartap_stream_open_path(bartap_stream_t *stream, const bartap_book_t *book, const char *path,
    uint8_t *window)
{
    if (stream == NULL || book == NULL || path == NULL) {
        return BARTAP_ERR_ARG;
    }
    bartap_entry_t entry;
    const bartap_err_t err = bartap_zip_find(&book->zip, book->source, path, strlen(path), &entry);
    if (err != BARTAP_OK) {
        return err;
    }
    return bartap_stream_init_entry(stream, book->source, &entry, window);
}

bartap_err_t bartap_stream_open_cover(bartap_stream_t *stream, const bartap_book_t *book, uint8_t *window)
{
    if (stream == NULL || book == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (!book->has_cover) {
        return BARTAP_ERR_NOT_FOUND;
    }
    return bartap_stream_init_entry(stream, book->source, &book->cover, window);
}

bartap_err_t bartap_stream_read(bartap_stream_t *stream, void *buf, size_t len, size_t *got)
{
    if (stream == NULL || stream->source == NULL || got == NULL || (buf == NULL && len > 0U)) {
        return BARTAP_ERR_ARG;
    }
    *got = 0U;

    if (stream->entry.method == 0U) {
        const uint32_t rem = stream->entry.size - stream->out_offset;
        if (rem == 0U) {
            if (stream->crc32 != stream->entry.crc32) {
                return BARTAP_ERR_CORRUPT;
            }
            return BARTAP_END;
        }
        if (len == 0U) {
            return BARTAP_OK;
        }
        const size_t to_read = len < rem ? len : (size_t)rem;
        const bartap_err_t err = bartap_source_read(stream->source,
            stream->entry.data_offset + stream->in_offset, buf, to_read);
        if (err != BARTAP_OK) {
            return err;
        }
        stream->in_offset += (uint32_t)to_read;
        stream->out_offset += (uint32_t)to_read;
        stream->crc32 = bartap_crc32(stream->crc32, buf, to_read);
        *got = to_read;
        return BARTAP_OK;
    }

    uint8_t *const out = buf;
    size_t total_made = 0U;

    while (total_made < len || len == 0U) {
        if (stream->in_pos == stream->in_len && stream->in_offset < stream->entry.compressed_size) {
            const uint32_t comp_rem = stream->entry.compressed_size - stream->in_offset;
            const size_t chunk = comp_rem < BARTAP_STREAM_IN_SIZE ? (size_t)comp_rem : BARTAP_STREAM_IN_SIZE;
            const bartap_err_t rerr = bartap_source_read(stream->source,
                stream->entry.data_offset + stream->in_offset, stream->in, chunk);
            if (rerr != BARTAP_OK) {
                return rerr;
            }
            stream->in_offset += (uint32_t)chunk;
            stream->in_pos = 0U;
            stream->in_len = (uint16_t)chunk;
        }

        size_t used = 0U;
        size_t made = 0U;
        const bartap_err_t ierr = bartap_inflate(&stream->inflate,
            stream->in + stream->in_pos, (size_t)(stream->in_len - stream->in_pos), &used,
            out != NULL ? out + total_made : NULL, len - total_made, &made);
        stream->in_pos = (uint16_t)(stream->in_pos + (uint16_t)used);

        if (made > 0U) {
            if (made > stream->entry.size - stream->out_offset) {
                return BARTAP_ERR_CORRUPT;
            }
            stream->crc32 = bartap_crc32(stream->crc32, out + total_made, made);
            stream->out_offset += (uint32_t)made;
            total_made += made;
        }

        if (ierr == BARTAP_END) {
            if (total_made > 0U) {
                *got = total_made;
                return BARTAP_OK;
            }
            if (stream->out_offset != stream->entry.size || stream->crc32 != stream->entry.crc32) {
                return BARTAP_ERR_CORRUPT;
            }
            return BARTAP_END;
        }
        if (ierr != BARTAP_OK) {
            return ierr;
        }
        if (len == 0U) {
            return BARTAP_OK;
        }
        if (used == 0U && made == 0U && stream->in_pos == stream->in_len
                && stream->in_offset == stream->entry.compressed_size) {
            return BARTAP_ERR_CORRUPT;
        }
    }

    *got = total_made;
    return BARTAP_OK;
}

uint32_t bartap_stream_size(const bartap_stream_t *stream)
{
    return stream != NULL ? stream->entry.size : 0U;
}

uint32_t bartap_stream_tell(const bartap_stream_t *stream)
{
    return stream != NULL ? stream->out_offset : 0U;
}

bartap_err_t bartap_stream_rewind(bartap_stream_t *stream)
{
    if (stream == NULL || stream->source == NULL) {
        return BARTAP_ERR_ARG;
    }
    stream->in_offset = 0U;
    stream->out_offset = 0U;
    stream->crc32 = BARTAP_CRC32_INIT;
    stream->in_pos = 0U;
    stream->in_len = 0U;
    if (stream->entry.method == 8U) {
        bartap_inflate_init(&stream->inflate, stream->inflate.window);
    }
    return BARTAP_OK;
}

static bartap_err_t bartap_xml_fill_stream(void *user, uint8_t *buf, size_t len, size_t *got)
{
    bartap_stream_t *const stream = user;
    const bartap_err_t err = bartap_stream_read(stream, buf, len, got);
    if (err == BARTAP_END) {
        *got = 0U;
        return BARTAP_OK;
    }
    return err;
}

static bartap_err_t bartap_stream_verify_end(bartap_stream_t *stream)
{
    uint8_t sink[64];
    size_t got = 0U;
    for (;;) {
        const bartap_err_t err = bartap_stream_read(stream, sink, sizeof sink, &got);
        if (err == BARTAP_END) {
            return BARTAP_OK;
        }
        if (err != BARTAP_OK) {
            return err;
        }
    }
}

/* ------------------------------------------------------------ book open */

/* Input chunks end on UTF-8 boundaries. Append whole code points and stop permanently at the
 * first character that does not fit, so a later short character cannot sneak past a dropped one. */
static void bartap_append_display(char *dst, size_t capacity, const char *src, size_t len, bool *cut)
{
    if (*cut) {
        return;
    }
    size_t used = strlen(dst);
    for (size_t i = 0U; i < len;) {
        const bool space = src[i] == ' ' || src[i] == '\t' || src[i] == '\r' || src[i] == '\n';
        size_t n = 1U;
        while (i + n < len && ((uint8_t)src[i + n] & 0xC0U) == 0x80U) {
            n++;
        }
        if (space && (used == 0U || dst[used - 1U] == ' ')) {
            i += n;
            continue;
        }
        if (n >= capacity - used) {
            *cut = true;
            return;
        }
        if (space) {
            dst[used++] = ' ';
        } else {
            memcpy(dst + used, src + i, n);
            used += n;
        }
        dst[used] = '\0';
        i += n;
    }
}

static void bartap_rtrim_meta(char *s)
{
    size_t len = strlen(s);
    while (len > 0U && s[len - 1U] == ' ') {
        s[--len] = '\0';
    }
}

/* The entry named first, else the one named second (*used_second says which). An empty name is not
 * looked up: every lookup is a central directory walk. */
static bartap_err_t bartap_find_first(const bartap_zip_t *zip, const bartap_source_t *source,
    const char *first, const char *second, bartap_entry_t *entry, bool *used_second)
{
    *used_second = false;
    if (first[0] != '\0') {
        const bartap_err_t err = bartap_zip_find(zip, source, first, strlen(first), entry);
        if (err != BARTAP_ERR_NOT_FOUND) {
            return err;
        }
    }
    if (second[0] == '\0') {
        return BARTAP_ERR_NOT_FOUND;
    }
    *used_second = true;
    return bartap_zip_find(zip, source, second, strlen(second), entry);
}

bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source, bartap_spine_item_t *spine,
    uint16_t spine_capacity, bartap_work_t *work)
{
    if (book == NULL || source == NULL || source->read == NULL || work == NULL
            || (spine == NULL && spine_capacity > 0U)) {
        return BARTAP_ERR_ARG;
    }

    memset(book, 0, sizeof *book);
    book->source = source;
    book->spine = spine;

    bartap_err_t err = bartap_zip_open(&book->zip, source);
    if (err != BARTAP_OK) {
        return err;
    }
    book->warnings |= book->zip.warnings;

    /* 1. Find META-INF/container.xml and read the OPF full-path. */
    bartap_entry_t container_entry;
    err = bartap_zip_find(&book->zip, source, "META-INF/container.xml", 22U, &container_entry);
    if (err == BARTAP_ERR_NOT_FOUND) {
        return BARTAP_ERR_NOT_EPUB;
    }
    if (err != BARTAP_OK) {
        return err;
    }

    err = bartap_stream_init_entry(&work->stream, source, &container_entry, work->window);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_xml_init(&work->xml, bartap_xml_fill_stream, &work->stream);

    work->path[0] = '\0';
    bartap_xml_token_t tok;
    while ((err = bartap_xml_next(&work->xml, &tok)) == BARTAP_OK && tok.type != BARTAP_XML_END) {
        if (tok.type == BARTAP_XML_START_TAG && strcmp(work->xml.name, "rootfile") == 0
                && work->path[0] == '\0') {
            size_t len = 0U;
            const bartap_err_t attr_err = bartap_xml_attr(&work->xml, "full-path", work->path,
                sizeof(work->path), &len);
            if (attr_err == BARTAP_ERR_LIMIT) {
                return attr_err;
            }
        }
    }
    if (err != BARTAP_OK) {
        return err;
    }
    err = bartap_stream_verify_end(&work->stream);
    if (err != BARTAP_OK) {
        return err;
    }
    book->warnings |= work->xml.warnings;

    if (work->path[0] == '\0') {
        return BARTAP_ERR_NOT_EPUB;
    }

    bartap_entry_t opf_entry;
    err = bartap_zip_find(&book->zip, source, work->path, strlen(work->path), &opf_entry);
    if (err == BARTAP_ERR_NOT_FOUND) {
        return BARTAP_ERR_NOT_EPUB;
    }
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_path_dir(work->path, book->opf_dir);

    /* 2. Pass 1 over the OPF: metadata and <spine><itemref idref="..."/></spine>. */
    err = bartap_stream_init_entry(&work->stream, source, &opf_entry, work->window);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_xml_init(&work->xml, bartap_xml_fill_stream, &work->stream);
    work->unique_id[0] = '\0';

    work->cover_id[0] = '\0';
    work->ncx_id[0] = '\0';
    bool meta_cut[BARTAP_META_COUNT_] = { false };
    bool unique_id_unavailable = false;

    bool in_metadata = false;
    int active_meta = -1;
    bool have_unique_id_match = false;
    uint16_t raw_spine_count = 0U;

    while ((err = bartap_xml_next(&work->xml, &tok)) == BARTAP_OK && tok.type != BARTAP_XML_END) {
        if (tok.type == BARTAP_XML_START_TAG) {
            if (strcmp(work->xml.name, "package") == 0) {
                size_t ulen = 0U;
                unique_id_unavailable = bartap_xml_attr(&work->xml, "unique-identifier", work->unique_id,
                    sizeof(work->unique_id), &ulen) == BARTAP_ERR_LIMIT;
            } else if (strcmp(work->xml.name, "metadata") == 0) {
                in_metadata = true;
            } else if (in_metadata) {
                if (strcmp(work->xml.name, "title") == 0 && book->meta[BARTAP_META_TITLE][0] == '\0') {
                    active_meta = BARTAP_META_TITLE;
                } else if (strcmp(work->xml.name, "creator") == 0
                        && book->meta[BARTAP_META_AUTHOR][0] == '\0') {
                    active_meta = BARTAP_META_AUTHOR;
                } else if (strcmp(work->xml.name, "language") == 0
                        && book->meta[BARTAP_META_LANGUAGE][0] == '\0') {
                    active_meta = BARTAP_META_LANGUAGE;
                } else if (strcmp(work->xml.name, "identifier") == 0) {
                    char id_val[BARTAP_META_MAX + 1];
                    size_t id_len = 0U;
                    if (bartap_xml_attr(&work->xml, "id", id_val, sizeof(id_val), &id_len) == BARTAP_OK
                            && work->unique_id[0] != '\0'
                            && strcmp(id_val, work->unique_id) == 0) {
                        have_unique_id_match = true;
                        book->meta[BARTAP_META_IDENTIFIER][0] = '\0';
                        meta_cut[BARTAP_META_IDENTIFIER] = false;
                        active_meta = BARTAP_META_IDENTIFIER;
                    } else if (!have_unique_id_match && book->meta[BARTAP_META_IDENTIFIER][0] == '\0') {
                        /* Until the named one appears, keep the first non-empty identifier: a dangling or
                         * truncated unique-identifier still leaves the book an identity (WARN_MARKUP). */
                        meta_cut[BARTAP_META_IDENTIFIER] = false;
                        active_meta = BARTAP_META_IDENTIFIER;
                    }
                } else if (strcmp(work->xml.name, "meta") == 0) {
                    char mname[32];
                    size_t mlen = 0U;
                    if (bartap_xml_attr(&work->xml, "name", mname, sizeof(mname), &mlen) == BARTAP_OK
                            && strcmp(mname, "cover") == 0) {
                        size_t clen = 0U;
                        if (bartap_xml_attr(&work->xml, "content", work->path, sizeof(work->path), &clen)
                                == BARTAP_OK && clen > 0U && clen <= BARTAP_PATH_MAX) {
                            memcpy(work->cover_id, work->path, clen + 1U);
                        }
                    }
                }
            } else if (strcmp(work->xml.name, "spine") == 0) {
                size_t tlen = 0U;
                if (bartap_xml_attr(&work->xml, "toc", work->path, sizeof(work->path), &tlen) == BARTAP_OK
                        && tlen > 0U && tlen <= BARTAP_PATH_MAX) {
                    memcpy(work->ncx_id, work->path, tlen + 1U);
                }
            } else if (strcmp(work->xml.name, "itemref") == 0) {
                size_t iref_len = 0U;
                if (bartap_xml_attr(&work->xml, "idref", work->path, sizeof(work->path), &iref_len)
                        == BARTAP_OK && iref_len > 0U && iref_len <= BARTAP_PATH_MAX) {
                    if (raw_spine_count < spine_capacity) {
                        char lin[8];
                        size_t llen = 0U;
                        uint16_t flags = 0U;
                        if (bartap_xml_attr(&work->xml, "linear", lin, sizeof(lin), &llen) == BARTAP_OK
                                && strcmp(lin, "no") == 0) {
                            flags |= BARTAP_SPINE_NONLINEAR;
                        }
                        spine[raw_spine_count].entry_offset = 0U;
                        spine[raw_spine_count].compressed_size = 0U;
                        spine[raw_spine_count].size = 0U;
                        spine[raw_spine_count].crc32 = 0U;
                        spine[raw_spine_count].path_hash = bartap_hash(work->path, iref_len);
                        spine[raw_spine_count].path_len = (uint16_t)iref_len;
                        spine[raw_spine_count].method = 0U;
                        spine[raw_spine_count].flags = flags;
                        raw_spine_count++;
                    } else {
                        book->warnings |= BARTAP_WARN_SPINE_TRUNCATED;
                    }
                }
            }
        } else if (tok.type == BARTAP_XML_END_TAG) {
            if (strcmp(work->xml.name, "metadata") == 0) {
                in_metadata = false;
                active_meta = -1;
            } else if (active_meta >= 0) {
                bartap_rtrim_meta(book->meta[active_meta]);
                active_meta = -1;
            }
        } else if (tok.type == BARTAP_XML_TEXT && active_meta >= 0) {
            bartap_append_display(book->meta[active_meta], BARTAP_META_MAX + 1U,
                tok.text, tok.text_len, &meta_cut[active_meta]);
        }
    }
    if (err != BARTAP_OK) {
        return err;
    }
    /* Checked after the pass: a fallback identifier cut short may since have been replaced. */
    for (size_t i = 0U; i < BARTAP_META_COUNT_; i++) {
        if (meta_cut[i]) {
            book->warnings |= BARTAP_WARN_META_TRUNCATED;
        }
    }
    err = bartap_stream_verify_end(&work->stream);
    if (err != BARTAP_OK) {
        return err;
    }
    book->warnings |= work->xml.warnings;

    if (unique_id_unavailable || (work->unique_id[0] != '\0' && !have_unique_id_match)) {
        book->warnings |= BARTAP_WARN_MARKUP;
    }

    /* 3. Pass 2 over the OPF: match <manifest> items to spine idrefs in RAM (zero ZIP CD reads
     * while streaming the OPF!). */
    err = bartap_stream_rewind(&work->stream);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_xml_init(&work->xml, bartap_xml_fill_stream, &work->stream);

    work->nav_path[0] = '\0';
    work->ncx_path[0] = '\0';
    work->cover_path[0] = '\0';
    work->cover_meta_path[0] = '\0';

    while ((err = bartap_xml_next(&work->xml, &tok)) == BARTAP_OK && tok.type != BARTAP_XML_END) {
        if (tok.type != BARTAP_XML_START_TAG || strcmp(work->xml.name, "item") != 0) {
            continue;
        }
        char item_id[BARTAP_PATH_MAX + 1];
        size_t id_len = 0U;
        if (bartap_xml_attr(&work->xml, "id", item_id, sizeof(item_id), &id_len) != BARTAP_OK
                || id_len == 0U || id_len > BARTAP_PATH_MAX) {
            continue;
        }
        char raw_href[BARTAP_PATH_MAX + 1];
        size_t href_len = 0U;
        if (bartap_xml_attr(&work->xml, "href", raw_href, sizeof(raw_href), &href_len) != BARTAP_OK
                || href_len == 0U || href_len > BARTAP_PATH_MAX) {
            continue;
        }
        if (bartap_resolve_href(book->opf_dir, raw_href, work->path, NULL, 0U) != BARTAP_OK) {
            continue;
        }
        const size_t resolved_len = strlen(work->path);
        const uint32_t id_hash = bartap_hash(item_id, id_len);
        const uint32_t path_hash = bartap_hash(work->path, resolved_len);

        /* Spine slots hold the idref's hash until their manifest item is found, then the path's. */
        for (uint16_t i = 0; i < raw_spine_count; i++) {
            if ((spine[i].flags & BARTAP_SPINE_MANIFEST_MATCHED) == 0U
                    && spine[i].path_len == id_len && spine[i].path_hash == id_hash) {
                spine[i].path_hash = path_hash;
                spine[i].path_len = (uint16_t)resolved_len;
                spine[i].flags |= BARTAP_SPINE_MANIFEST_MATCHED;
            }
        }

        char props[128];
        size_t plen = 0U;
        props[0] = '\0';
        (void)bartap_xml_attr(&work->xml, "properties", props, sizeof(props), &plen);

        char mtype[64];
        size_t mtlen = 0U;
        mtype[0] = '\0';
        (void)bartap_xml_attr(&work->xml, "media-type", mtype, sizeof(mtype), &mtlen);

        if (bartap_prop_has_token(props, "nav") && work->nav_path[0] == '\0') {
            memcpy(work->nav_path, work->path, resolved_len + 1U);
        }
        if ((strcmp(mtype, "application/x-dtbncx+xml") == 0 || strcmp(work->ncx_id, item_id) == 0)
                && work->ncx_path[0] == '\0') {
            memcpy(work->ncx_path, work->path, resolved_len + 1U);
        }
        if (bartap_prop_has_token(props, "cover-image") && work->cover_path[0] == '\0') {
            memcpy(work->cover_path, work->path, resolved_len + 1U);
        }
        if (strcmp(work->cover_id, item_id) == 0 && work->cover_meta_path[0] == '\0') {
            memcpy(work->cover_meta_path, work->path, resolved_len + 1U);
        }
    }
    if (err != BARTAP_OK) {
        return err;
    }
    book->warnings |= work->xml.warnings;

    /* 4. One directory walk, matching names to spine paths by hash and length in RAM (D10). */
    uint32_t cd_pos = book->zip.cd_offset;
    for (uint16_t n = 0U; n < book->zip.entry_count; n++) {
        bartap_entry_t entry;
        uint16_t flags;
        err = bartap_zip_next(&book->zip, source, &cd_pos, &entry, work->path, &flags);
        if (err != BARTAP_OK) {
            return err;
        }
        const size_t name_len = strlen(work->path);
        if (name_len == 0U) {
            continue;
        }
        const uint32_t name_hash = bartap_hash(work->path, name_len);
        bool resolved = false;
        for (uint16_t i = 0U; i < raw_spine_count; i++) {
            if ((spine[i].flags & BARTAP_SPINE_MANIFEST_MATCHED) == 0U
                    || (spine[i].flags & BARTAP_SPINE_ZIP_RESOLVED) != 0U
                    || spine[i].path_len != name_len || spine[i].path_hash != name_hash) {
                continue;
            }
            if (!resolved) {
                err = bartap_zip_resolve_local_header(&book->zip, source, flags, &entry, NULL);
                if (err != BARTAP_OK) {
                    return err;
                }
                resolved = true;
            }
            spine[i].entry_offset = entry.header_offset;
            spine[i].compressed_size = entry.compressed_size;
            spine[i].size = entry.size;
            spine[i].crc32 = entry.crc32;
            spine[i].method = entry.method;
            spine[i].flags |= BARTAP_SPINE_ZIP_RESOLVED;
        }
    }

    uint16_t valid_spine = 0U;
    for (uint16_t i = 0; i < raw_spine_count; i++) {
        if ((spine[i].flags & BARTAP_SPINE_ZIP_RESOLVED) != 0U) {
            spine[i].flags &= (uint16_t)~(BARTAP_SPINE_MANIFEST_MATCHED | BARTAP_SPINE_ZIP_RESOLVED);
            if (valid_spine != i) {
                spine[valid_spine] = spine[i];
            }
            valid_spine++;
        } else {
            book->warnings |= BARTAP_WARN_SPINE_MISSING;
        }
    }
    book->spine_count = valid_spine;

    /* At most four additional lookups, independent of the number of spine items; none for a path the
     * OPF did not name. */
    err = bartap_find_first(&book->zip, source, work->nav_path, work->ncx_path, &book->toc,
        &book->toc_is_ncx);
    if (err != BARTAP_OK && err != BARTAP_ERR_NOT_FOUND) {
        return err;
    }
    book->has_toc = err == BARTAP_OK;
    if (!book->has_toc) {
        book->warnings |= BARTAP_WARN_NO_TOC;
    }
    bool cover_from_meta;
    err = bartap_find_first(&book->zip, source, work->cover_path, work->cover_meta_path, &book->cover,
        &cover_from_meta);
    if (err != BARTAP_OK && err != BARTAP_ERR_NOT_FOUND) {
        return err;
    }
    book->has_cover = err == BARTAP_OK;

    return BARTAP_OK;
}

uint32_t bartap_warnings(const bartap_book_t *book)
{
    return book != NULL ? book->warnings : 0U;
}

uint16_t bartap_spine_count(const bartap_book_t *book)
{
    return book != NULL ? book->spine_count : 0U;
}

bool bartap_spine_linear(const bartap_book_t *book, uint16_t spine_index)
{
    if (book == NULL || spine_index >= book->spine_count) {
        return false;
    }
    return (book->spine[spine_index].flags & BARTAP_SPINE_NONLINEAR) == 0U;
}

bartap_err_t bartap_metadata(const bartap_book_t *book, bartap_meta_t field, char *buf, size_t size)
{
    if (book == NULL || (unsigned)field >= BARTAP_META_COUNT_
            || buf == NULL || size == 0U) {
        return BARTAP_ERR_ARG;
    }
    const char *const src = book->meta[field];
    (void)bartap_copy_utf8(buf, size, src, strlen(src));
    return BARTAP_OK;
}

/* ------------------------------------------------------------ table of contents */

static bool bartap_find_spine_by_path(const bartap_book_t *book, const char *path, uint16_t *spine_index)
{
    const size_t len = strlen(path);
    const uint32_t hash = bartap_hash(path, len);
    for (uint16_t i = 0; i < book->spine_count; i++) {
        if (book->spine[i].path_len == len && book->spine[i].path_hash == hash) {
            *spine_index = i;
            return true;
        }
    }
    return false;
}

static void bartap_append_label(bartap_toc_t *toc, char *label, const char *text, size_t len)
{
    bartap_append_display(label, BARTAP_LABEL_MAX + 1U, text, len, &toc->label_truncated);
    if (toc->label_truncated) {
        toc->warnings |= BARTAP_WARN_TEXT_TRUNCATED;
    }
}

uint32_t bartap_toc_warnings(const bartap_toc_t *toc)
{
    return toc != NULL ? toc->warnings | toc->xml.warnings : 0U;
}

bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window)
{
    if (toc == NULL || book == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (!book->has_toc) {
        return BARTAP_ERR_NOT_FOUND;
    }
    memset(toc, 0, sizeof *toc);
    toc->book = book;
    bartap_err_t err = bartap_entry_base_dir(book->source, book->toc.header_offset, toc->base_dir);
    if (err != BARTAP_OK) {
        return err;
    }
    err = bartap_stream_init_entry(&toc->stream, book->source, &book->toc, window);
    if (err != BARTAP_OK) {
        return err;
    }
    bartap_xml_init(&toc->xml, bartap_xml_fill_stream, &toc->stream);
    return BARTAP_OK;
}

bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_toc_entry_t *entry)
{
    if (toc == NULL || toc->book == NULL || entry == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (toc->depth == 0xFFU) {
        return BARTAP_END;
    }

    toc->label_truncated = false;
    bartap_err_t err = BARTAP_OK;
    bartap_xml_token_t tok;

    if (!toc->book->toc_is_ncx) {
        for (;;) {
            while ((err = bartap_xml_next(&toc->xml, &tok)) == BARTAP_OK && tok.type != BARTAP_XML_END) {
                if (tok.type == BARTAP_XML_START_TAG) {
                    if (strcmp(toc->xml.name, "nav") == 0 && toc->depth == 0U) {
                        char ntype[64];
                        size_t nlen = 0U;
                        const bartap_err_t type_err = bartap_xml_attr(&toc->xml, "type", ntype, sizeof(ntype),
                            &nlen);
                        if (type_err == BARTAP_OK && bartap_prop_has_token(ntype, "toc")) {
                            toc->depth = 1U;
                        } else if (type_err == BARTAP_ERR_NOT_FOUND) {
                            /* An untyped nav is a fallback, taken only once no toc nav has been found. */
                            if (toc->accept_untyped_nav) {
                                toc->depth = 1U;
                            } else {
                                toc->saw_untyped_nav = true;
                            }
                        }
                    } else if (toc->depth >= 1U && strcmp(toc->xml.name, "ol") == 0) {
                        if (toc->depth < 250U) {
                            toc->depth++;
                        }
                    } else if (toc->depth >= 2U && strcmp(toc->xml.name, "a") == 0) {
                        char href[BARTAP_PATH_MAX + 1];
                        size_t hlen = 0U;
                        const bool has_href = (bartap_xml_attr(&toc->xml, "href", href, sizeof(href), &hlen)
                            == BARTAP_OK && hlen > 0U && hlen <= BARTAP_PATH_MAX);

                        memset(entry, 0, sizeof *entry);
                        toc->label_truncated = false;
                        entry->depth = (uint8_t)(toc->depth - 2U);

                        int a_depth = 1;
                        while (a_depth > 0 && (err = bartap_xml_next(&toc->xml, &tok)) == BARTAP_OK
                                && tok.type != BARTAP_XML_END) {
                            if (tok.type == BARTAP_XML_START_TAG && strcmp(toc->xml.name, "a") == 0) {
                                a_depth++;
                            } else if (tok.type == BARTAP_XML_END_TAG && strcmp(toc->xml.name, "a") == 0) {
                                a_depth--;
                            } else if (tok.type == BARTAP_XML_TEXT) {
                                bartap_append_label(toc, entry->label, tok.text, tok.text_len);
                            }
                        }
                        if (err != BARTAP_OK) {
                            return err;
                        }
                        bartap_rtrim_meta(entry->label);

                        if (has_href) {
                            char resolved[BARTAP_PATH_MAX + 1];
                            if (bartap_resolve_href(toc->base_dir, href, resolved, entry->fragment,
                                    sizeof(entry->fragment)) == BARTAP_OK
                                    && bartap_find_spine_by_path(toc->book, resolved, &entry->spine_index)) {
                                return BARTAP_OK;
                            }
                            toc->warnings |= BARTAP_WARN_MARKUP;
                        }
                    }
                } else if (tok.type == BARTAP_XML_END_TAG) {
                    if (toc->depth >= 2U && strcmp(toc->xml.name, "ol") == 0) {
                        toc->depth--;
                    } else if (toc->depth >= 1U && strcmp(toc->xml.name, "nav") == 0) {
                        toc->depth = 0xFFU;
                        break;
                    }
                }
            }
            if (err != BARTAP_OK || toc->depth != 0U || !toc->saw_untyped_nav || toc->accept_untyped_nav) {
                break;
            }
            /* The whole document had no toc nav: read it again, taking the first untyped nav. */
            err = bartap_stream_rewind(&toc->stream);
            if (err != BARTAP_OK) {
                return err;
            }
            const uint32_t xml_warnings = toc->xml.warnings;
            bartap_xml_init(&toc->xml, bartap_xml_fill_stream, &toc->stream);
            toc->xml.warnings = xml_warnings;
            toc->accept_untyped_nav = true;
        }
    } else {
        bool in_label = false;
        bool in_text = false;
        bool have_pending = false;
        memset(entry, 0, sizeof *entry);

        while ((err = bartap_xml_next(&toc->xml, &tok)) == BARTAP_OK && tok.type != BARTAP_XML_END) {
            if (tok.type == BARTAP_XML_START_TAG) {
                if (strcmp(toc->xml.name, "navpoint") == 0) {
                    memset(entry, 0, sizeof *entry);
                    toc->label_truncated = false;
                    entry->depth = toc->depth;
                    if (toc->depth < 250U) {
                        toc->depth++;
                    }
                    have_pending = true;
                } else if (have_pending && strcmp(toc->xml.name, "navlabel") == 0) {
                    in_label = true;
                } else if (in_label && strcmp(toc->xml.name, "text") == 0) {
                    in_text = true;
                } else if (have_pending && strcmp(toc->xml.name, "content") == 0) {
                    char src[BARTAP_PATH_MAX + 1];
                    size_t slen = 0U;
                    have_pending = false;
                    bartap_rtrim_meta(entry->label);
                    if (bartap_xml_attr(&toc->xml, "src", src, sizeof(src), &slen) == BARTAP_OK
                            && slen > 0U && slen <= BARTAP_PATH_MAX) {
                        char resolved[BARTAP_PATH_MAX + 1];
                        if (bartap_resolve_href(toc->base_dir, src, resolved, entry->fragment,
                                sizeof(entry->fragment)) == BARTAP_OK
                                && bartap_find_spine_by_path(toc->book, resolved, &entry->spine_index)) {
                            return BARTAP_OK;
                        }
                        toc->warnings |= BARTAP_WARN_MARKUP;
                    }
                }
            } else if (tok.type == BARTAP_XML_END_TAG) {
                if (strcmp(toc->xml.name, "text") == 0) {
                    in_text = false;
                } else if (strcmp(toc->xml.name, "navlabel") == 0) {
                    in_label = false;
                } else if (strcmp(toc->xml.name, "navpoint") == 0 && toc->depth > 0U) {
                    toc->depth--;
                    have_pending = false;
                }
            } else if (tok.type == BARTAP_XML_TEXT && in_text) {
                bartap_append_label(toc, entry->label, tok.text, tok.text_len);
            }
        }
    }

    if (err != BARTAP_OK) {
        return err;
    }
    toc->depth = 0xFFU;
    err = bartap_stream_verify_end(&toc->stream);
    if (err != BARTAP_OK) {
        return err;
    }
    return BARTAP_END;
}
