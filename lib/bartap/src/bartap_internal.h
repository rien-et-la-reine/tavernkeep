// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_INTERNAL_H
#define BARTAP_INTERNAL_H

/* The interfaces between the modules. The state layouts they work on are in bartap.h.
 *
 *   byte_source.c   reads through bartap_source_t; the memory source; shared helpers
 *   crc.c           CRC-32 of entries' uncompressed bytes
 *   ocf.c           ZIP end record (tolerating trailing bytes), central directory walk, entry lookup
 *   inflate.c       streaming DEFLATE over a caller window
 *   xml.c           lenient pull tokenizer, entities, Windows-1252 fallback
 *   epub.c          bartap_open (container.xml, OPF passes), metadata, streams, TOC
 *   text.c          word/block token iterator over a chapter */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap.h"

/* ------------------------------------------------------------ byte_source.c */

/* 32-bit FNV-1a over len bytes, for matching paths and tag names without keeping the strings. */
uint32_t bartap_hash(const void *data, size_t len);

/* Copy src (len bytes of UTF-8) into dst as a NUL-terminated string, cut back to a character boundary
 * if it does not fit in size bytes. Returns true if it was cut. */
bool bartap_copy_utf8(char *dst, size_t size, const char *src, size_t len);

/* Read exactly len bytes at offset; a short source is BARTAP_ERR_CORRUPT. */
bartap_err_t bartap_source_read(const bartap_source_t *source, uint32_t offset, void *buf, size_t len);

/* Extract the directory prefix of path (including trailing '/', or empty if at root) into dir. */
void bartap_path_dir(const char *path, char dir[BARTAP_PATH_MAX + 1]);

/* Resolve href (stopping at '#' or '?') relative to base_dir in-place into out, normalizing '.' and '..'
 * and decoding %XX escapes. Optionally copies the fragment after '#' into frag[0..frag_size-1]. */
bartap_err_t bartap_resolve_href(const char *base_dir, const char *href,
    char out[BARTAP_PATH_MAX + 1], char *frag, size_t frag_size);

/* ------------------------------------------------------------ crc.c */

#define BARTAP_CRC32_INIT 0U

/* The CRC-32 (ZIP's) of crc's data followed by len more bytes at data. Start from BARTAP_CRC32_INIT;
 * the result after the last byte is the entry's CRC-32, with nothing more to apply. */
uint32_t bartap_crc32(uint32_t crc, const void *data, size_t len);

/* ------------------------------------------------------------ ocf.c */

bartap_err_t bartap_zip_open(bartap_zip_t *zip, const bartap_source_t *source);
/* The entry named path (len bytes), compared byte for byte; the first of duplicates. */
bartap_err_t bartap_zip_find(const bartap_zip_t *zip, const bartap_source_t *source, const char *path,
    size_t len, bartap_entry_t *entry);
/* Walk one directory entry, advancing pos. An overlong name is returned as an empty path. */
bartap_err_t bartap_zip_next(const bartap_zip_t *zip, const bartap_source_t *source,
    uint32_t *pos, bartap_entry_t *entry, char path[BARTAP_PATH_MAX + 1], uint16_t *flags);
bartap_err_t bartap_zip_resolve_local_header(const bartap_zip_t *zip, const bartap_source_t *source,
    uint16_t cd_flags, bartap_entry_t *entry, uint16_t *lfh_extra_len);

/* ------------------------------------------------------------ inflate.c */

void bartap_inflate_init(bartap_inflate_t *inflate, uint8_t *window);
/* Decode from in (in_len bytes) into out (out_len bytes); report what was used. BARTAP_END once the
 * final block has ended. */
bartap_err_t bartap_inflate(bartap_inflate_t *inflate, const uint8_t *in, size_t in_len, size_t *in_used,
    uint8_t *out, size_t out_len, size_t *out_made);

/* ------------------------------------------------------------ xml.c */

typedef enum {
    BARTAP_XML_START_TAG,       // name; attributes through bartap_xml_attr
    BARTAP_XML_END_TAG,         // also sent after a self-closing start tag
    BARTAP_XML_TEXT,            // decoded UTF-8: entities expanded, Windows-1252 bytes mapped
    BARTAP_XML_END,
} bartap_xml_type_t;

typedef struct {
    bartap_xml_type_t type;
    uint32_t name_hash;         // bartap_hash of the local name, lower-cased
    uint32_t offset;            // of the token's first byte in the document
    const char *text;           // TEXT: into the tokenizer's buffer, valid until the next call
    size_t text_len;
} bartap_xml_token_t;

void bartap_xml_init(bartap_xml_t *xml, bartap_xml_fill_fn fill, void *user);
bartap_err_t bartap_xml_next(bartap_xml_t *xml, bartap_xml_token_t *token);
/* The value of the current start tag's attribute name (local name), decoded into buf. */
bartap_err_t bartap_xml_attr(const bartap_xml_t *xml, const char *name, char *buf, size_t size,
    size_t *len);

#endif
