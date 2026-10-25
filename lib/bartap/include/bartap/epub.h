// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_EPUB_H
#define BARTAP_EPUB_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * EPUB: the book-level API, and with byte_source.h and text.h the whole of
 * Bartap's public surface (D9). The OCF container, DEFLATE and XML are
 * internal layers beneath it.
 *
 * Shape (Q18 is open; these are the current choices):
 * - State structs are opaque storage of a published size (Q2 option B),
 *   checked at build time against the real state.
 * - Opening is begin/step calls, each making at most one byte-source read
 *   (D14), through a transient opener that holds everything needed only while
 *   opening. The book keeps about 1 KiB: the archive's directory, the
 *   metadata, and where the table of contents and cover are. Opening reads
 *   META-INF/container.xml (the first rootfile whose media-type is the
 *   OPF's), finds the OPF, reads it twice (metadata and spine, then the
 *   manifest) and walks the directory once to find every spine item, the
 *   table of contents and the cover. Two lighter modes stop early.
 * - The spine is an array the caller supplies. Manifest and spine are matched
 *   through 32-bit FNV-1a hashes of their resolved paths and the paths'
 *   lengths, so no path strings are kept; a collision between two of one
 *   book's paths would misplace an item.
 * - Markup in container.xml, the OPF and the table of contents is read
 *   leniently by default: errors are recovered from and reported as
 *   BARTAP_EPUB_WARNING_MARKUP_RECOVERED (D19's reasoning, applied to XML).
 *   BARTAP_EPUB_OPEN_STRICT_MARKUP makes them CORRUPT.
 * - Content documents are read as streams of their bytes, one read per call.
 *   What reads them as text, and resolves CFIs in them, is fed those bytes by
 *   the caller (text.h, and bartap_epub_cfi_resolve_next below), so the same
 *   calls work over a stream, a decompressed copy in PSRAM or on SD, or any
 *   other bytes.
 * - Any entry can be found by its archive path and read as a stream:
 *   stylesheets, images, the cover.
 * - The table of contents comes from the EPUB 3 navigation document (the nav
 *   whose epub:type holds "toc"), or else the EPUB 2 NCX, as a flat series of
 *   entries with a depth, a label and a target: a spine index and a fragment.
 * - Hrefs are resolved relative to the referencing document, with "." and
 *   ".." segments and percent-encoding resolved. Composed and decomposed
 *   Unicode forms are not matched (D20).
 * - EPUB CFIs (EPUB CFI 1.1): one indirection ("/6/N!..."), element and text
 *   steps, a character offset counted in UTF-16 code units after line-end
 *   normalization and entity decoding, as a DOM counts. Ranges, temporal and
 *   spatial offsets and further indirections are UNSUPPORTED; id assertions
 *   are skipped, not checked.
 *
 * Reading a book with PSRAM. A content document can be decompressed whole
 * into memory: open a stream, read bartap_epub_stream_size once the first
 * read has returned, allocate that much, and read until *end, which is when
 * the CRC-32 and size are checked. Feed text.h and the CFI resolver from that
 * copy at any offset. With text.h's snapshots, paging backwards is a restore
 * and a feed from the snapshot's offset, with no decompression. Without
 * PSRAM, feed them from a stream as it is read, and restore by reading the
 * entry again from its start and discarding the bytes before the offset.
 *
 * Bounded work. Every call, and every step of a call built on steps, makes at
 * most one byte-source read (D14). The calls without a read do work bounded
 * by their arguments or the book's spine. bartap_epub_open and
 * bartap_epub_spine_name run to completion, as convenience calls (D5). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"

/* Values are fixed and the list is append-only (D18). */
typedef enum {
    BARTAP_EPUB_RESULT_OK = 0,
    BARTAP_EPUB_RESULT_INVALID_ARGUMENT = 1,
    BARTAP_EPUB_RESULT_IO_ERROR = 2,            //the byte source failed
    BARTAP_EPUB_RESULT_CORRUPT = 3,
    BARTAP_EPUB_RESULT_UNSUPPORTED = 4,
    BARTAP_EPUB_RESULT_NOT_FOUND = 5,
    BARTAP_EPUB_RESULT_LIMIT_EXCEEDED = 6,      //a caller-set limit (D18)
    BARTAP_EPUB_RESULT_NOT_AN_EPUB = 7,
    BARTAP_EPUB_RESULT_NOT_A_ZIP = 8,
} bartap_epub_result_t;

/* Warning bits: the OCF layer's (D19, D20), and the book layer's own.
 * Append-only. */
#define BARTAP_EPUB_WARNING_MIMETYPE_MISSING     (1UL << 0)
#define BARTAP_EPUB_WARNING_MIMETYPE_NOT_FIRST   (1UL << 1)
#define BARTAP_EPUB_WARNING_MIMETYPE_COMPRESSED  (1UL << 2)
#define BARTAP_EPUB_WARNING_MIMETYPE_EXTRA_FIELD (1UL << 3)
#define BARTAP_EPUB_WARNING_NAME_NOT_UTF8        (1UL << 4)
#define BARTAP_EPUB_WARNING_SPINE_ITEM_MISSING   (1UL << 8)  //an itemref with no manifest item or no entry
#define BARTAP_EPUB_WARNING_NO_TOC               (1UL << 9)  //neither a navigation document nor an NCX
#define BARTAP_EPUB_WARNING_METADATA_TRUNCATED   (1UL << 10) //a field longer than BARTAP_EPUB_METADATA_MAX
#define BARTAP_EPUB_WARNING_MARKUP_RECOVERED     (1UL << 11) //container.xml or the OPF was not well-formed

#define BARTAP_EPUB_METADATA_MAX 255U           //bytes kept of each metadata field
#define BARTAP_EPUB_LABEL_MAX 127U              //bytes kept of a table-of-contents label
#define BARTAP_EPUB_FRAGMENT_MAX 63U            //bytes kept of a target's fragment
#define BARTAP_EPUB_PATH_MAX 256U               //longest archive path found or resolved
#define BARTAP_EPUB_CFI_STEPS_MAX 16U           //steps inside a content document
#define BARTAP_EPUB_SCRATCH_MIN 64U             //smallest scratch buffer for opening

/* Opaque storage sizes: at least the real state's on every supported
 * platform, checked at build time. */
#define BARTAP_EPUB_BOOK_SIZE 1280U
#define BARTAP_EPUB_OPENER_SIZE 5376U
#define BARTAP_EPUB_STREAM_SIZE 2560U
#define BARTAP_EPUB_TOC_SIZE 4864U
#define BARTAP_EPUB_FINDER_SIZE 448U

#define BARTAP_EPUB_OPAQUE(size) union { max_align_t align; unsigned char bytes[size]; }

/* An open book. Private. Its spine array and byte source must outlive it. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_EPUB_BOOK_SIZE) storage;
} bartap_epub_t;

/* What opening needs only while it runs. Private; free once open finishes. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_EPUB_OPENER_SIZE) storage;
} bartap_epub_opener_t;

/* An entry being read. Private. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_EPUB_STREAM_SIZE) storage;
} bartap_epub_stream_t;

/* The table of contents being read. Private. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_EPUB_TOC_SIZE) storage;
} bartap_epub_toc_t;

/* An entry being looked up by path. Private. */
typedef struct {
    BARTAP_EPUB_OPAQUE(BARTAP_EPUB_FINDER_SIZE) storage;
} bartap_epub_finder_t;

/* One spine item, in an array the caller supplies. Its fields are private. */
typedef struct {
    uint32_t key;                               //the itemref's idref hash while opening
    uint32_t href_hash;                         //its resolved path's hash
    bartap_offset_t entry_offset;               //where its directory entry is
    uint32_t entry_index;
    uint8_t flags;
    uint16_t href_length;
} bartap_epub_spine_item_t;

/* Where an entry is in the archive. Its fields are private. A location from
 * outside Bartap is not trusted: one that is stale, or from another book, is
 * CORRUPT when used, never an out-of-bounds read. */
typedef struct {
    bartap_offset_t entry_offset;
    uint32_t entry_index;
} bartap_epub_location_t;

typedef enum {
    BARTAP_EPUB_OPEN_FULL = 0,                  //everything
    BARTAP_EPUB_OPEN_METADATA = 1,              //metadata only, from the OPF up to </metadata>
    BARTAP_EPUB_OPEN_METADATA_COVER = 2,        //metadata and the cover, from the OPF up to </manifest>
} bartap_epub_open_mode_t;

#define BARTAP_EPUB_OPEN_STRICT_MARKUP (1UL << 2)   //bartap_epub_memory_t.flags

/* Memory for opening. scratch, window and input are used only until open
 * finishes, then free for the caller; spine is kept for the book's life.
 * scratch may be the same memory as window: it is used only before the
 * window is. */
typedef struct {
    uint8_t *scratch;                           //at least BARTAP_EPUB_SCRATCH_MIN bytes
    size_t scratch_size;
    uint8_t *window;                            //an inflate window (D11)
    size_t window_size;
    uint8_t *input;                             //compressed input (D16)
    size_t input_size;
    bartap_epub_spine_item_t *spine;            //may be NULL unless mode is FULL
    size_t spine_capacity;                      //more itemrefs than this is LIMIT_EXCEEDED
    bartap_offset_t max_document_size;          //container.xml, the OPF, the nav or NCX; larger is LIMIT_EXCEEDED
    uint8_t mode;                               //a bartap_epub_open_mode_t
    uint32_t flags;                             //BARTAP_EPUB_OPEN_* bits
} bartap_epub_memory_t;

typedef enum {
    BARTAP_EPUB_FIELD_TITLE = 0,
    BARTAP_EPUB_FIELD_CREATOR = 1,
    BARTAP_EPUB_FIELD_LANGUAGE = 2,
    BARTAP_EPUB_FIELD_IDENTIFIER = 3,
} bartap_epub_field_t;

/* ------------------------------------------------------------- opening */

/* Start opening the book in *source into *book. No read. */
bartap_epub_result_t bartap_epub_open_begin(bartap_epub_opener_t *opener, bartap_epub_t *book,
    const bartap_byte_source_t *source, const bartap_epub_memory_t *memory);

/* One step of opening: at most one read. *done is set once the book is open.
 * Any error is final, and leaves the book unusable. Once a step sets *done or
 * returns an error, the opener, scratch, window and input are free. */
bartap_epub_result_t bartap_epub_open_step(bartap_epub_opener_t *opener, bool *done);

/* begin, then steps until done or an error (D5). */
bartap_epub_result_t bartap_epub_open(bartap_epub_opener_t *opener, bartap_epub_t *book,
    const bartap_byte_source_t *source, const bartap_epub_memory_t *memory);

/* BARTAP_EPUB_WARNING_* bits, complete once open has finished. */
uint32_t bartap_epub_warnings(const bartap_epub_t *book);

/* Copy a metadata field into buffer, with a NUL; *length (if not NULL) gets
 * its length. NOT_FOUND if the book has none; LIMIT_EXCEEDED, with nothing
 * copied, if buffer is too small. No read. A book opened in a METADATA mode
 * has had its OPF read only partly, so not CRC-checked: its metadata is
 * unverified, and a duplicate OPF entry (D21) is not noticed. */
bartap_epub_result_t bartap_epub_metadata(const bartap_epub_t *book, bartap_epub_field_t field, char *buffer,
    size_t buffer_size, size_t *length);

/* ------------------------------------------------------------- spine

   These and every call below that reads the book's content are
   INVALID_ARGUMENT for a book opened in a METADATA mode, except the cover
   and the finder, which a METADATA_COVER book has. */

uint32_t bartap_epub_spine_count(const bartap_epub_t *book);

/* The spine index of the item whose archive path is name (NUL-terminated),
 * by hash. No read. */
bartap_epub_result_t bartap_epub_spine_index(const bartap_epub_t *book, const char *name, uint32_t *index);

/* Copy spine item index's archive path into buffer, with a NUL. Runs to
 * completion (D5): two reads or more. */
bartap_epub_result_t bartap_epub_spine_name(const bartap_epub_t *book, uint32_t index, uint8_t *buffer,
    size_t buffer_size);

/* Where spine item index is. NOT_FOUND for an item whose entry is missing. No
 * read. */
bartap_epub_result_t bartap_epub_spine_location(const bartap_epub_t *book, uint32_t index,
    bartap_epub_location_t *location);

/* ------------------------------------------------------------- any entry */

/* Start looking for the entry whose archive path is path (path_length bytes,
 * at most BARTAP_EPUB_PATH_MAX, as from bartap_epub_href_resolve). The path
 * is copied. No read. */
bartap_epub_result_t bartap_epub_find_begin(bartap_epub_finder_t *finder, const bartap_epub_t *book,
    const uint8_t *path, size_t path_length);

/* One step: one read. OK with *done set and *location the entry, the first in
 * the directory with that path (D21); NOT_FOUND at the end of the directory.
 * Only names whose length matches are read. Errors are final. */
bartap_epub_result_t bartap_epub_find_step(bartap_epub_finder_t *finder, bool *done,
    bartap_epub_location_t *location);

typedef enum {
    BARTAP_EPUB_IMAGE_OTHER = 0,
    BARTAP_EPUB_IMAGE_JPEG = 1,
    BARTAP_EPUB_IMAGE_PNG = 2,
    BARTAP_EPUB_IMAGE_GIF = 3,
    BARTAP_EPUB_IMAGE_SVG = 4,
    BARTAP_EPUB_IMAGE_WEBP = 5,
} bartap_epub_image_type_t;

/* The cover image, found at open: the first manifest item whose properties
 * hold "cover-image" (EPUB 3), or else the item named by
 * <meta name="cover" content="ID"> (EPUB 2). *type (if not NULL) is a
 * bartap_epub_image_type_t from its media-type. NOT_FOUND if the book
 * declares none, or its entry is missing. No read. */
bartap_epub_result_t bartap_epub_cover(const bartap_epub_t *book, bartap_epub_location_t *location,
    uint8_t *type);

/* ------------------------------------------------------------- streams */

/* Start reading spine item index. max_size caps the declared size
 * (LIMIT_EXCEEDED, D6); window (D11) and input (D16) are for a deflated entry
 * and are used until the stream is finished or abandoned. No read; NOT_FOUND
 * for an item whose entry is missing. */
bartap_epub_result_t bartap_epub_stream_open(bartap_epub_stream_t *stream, const bartap_epub_t *book,
    uint32_t index, bartap_offset_t max_size, uint8_t *window, size_t window_size, uint8_t *input,
    size_t input_size);

/* The same for any entry: a stylesheet, an image, a chapter. A location
 * that is stale or from another book is CORRUPT at the first read. */
bartap_epub_result_t bartap_epub_stream_open_at(bartap_epub_stream_t *stream, const bartap_epub_t *book,
    const bartap_epub_location_t *location, bartap_offset_t max_size, uint8_t *window, size_t window_size,
    uint8_t *input, size_t input_size);

/* Read up to size bytes: one read per call, so a call may return none (the
 * first calls read the entry's headers). *end is set by the read that
 * reaches the end, which is when the CRC-32 and size are checked: bytes
 * delivered before then are unverified. Errors are final. size may be 0. */
bartap_epub_result_t bartap_epub_stream_read(bartap_epub_stream_t *stream, void *buffer, size_t size,
    size_t *bytes_read, bool *end);

/* The entry's declared uncompressed size, which the stream will deliver
 * exactly or fail. Known once the first read has returned; before that, or
 * after an error, INVALID_ARGUMENT. */
bartap_epub_result_t bartap_epub_stream_size(const bartap_epub_stream_t *stream, bartap_offset_t *size);

/* ------------------------------------------------------------- hrefs and targets */

/* Where an href leads: a spine item and a fragment. */
typedef struct {
    uint32_t spine_index;                       //UINT32_MAX if the path is not a spine item
    uint8_t fragment[BARTAP_EPUB_FRAGMENT_MAX + 1U];    //percent-decoded, NUL-terminated; empty if none
    uint8_t fragment_length;
    bool fragment_truncated;
} bartap_epub_target_t;

/* Resolve href (href_length bytes) against the directory of base (an archive
 * path, as from bartap_epub_spine_name), into path (NUL-terminated) and
 * *target. A fragment that is a CFI ("epubcfi(...)") is kept as written, for
 * bartap_epub_cfi_parse. UNSUPPORTED for an absolute URL or path, or one
 * that climbs above the archive's root; LIMIT_EXCEEDED if the path does not
 * fit path_size; CORRUPT for a malformed percent-escape. No read. book may be
 * NULL, in which case target->spine_index is UINT32_MAX. */
bartap_epub_result_t bartap_epub_href_resolve(const bartap_epub_t *book, const uint8_t *base,
    size_t base_length, const uint8_t *href, size_t href_length, uint8_t *path, size_t path_size,
    size_t *path_length, bartap_epub_target_t *target);

/* ------------------------------------------------------------- table of contents */

typedef struct {
    uint32_t depth;                             //1 for the top level
    bartap_epub_target_t target;
    char label[BARTAP_EPUB_LABEL_MAX + 1U];     //NUL-terminated, whitespace collapsed
    size_t label_length;
    bool label_truncated;
} bartap_epub_toc_entry_t;

/* Start reading the table of contents. window and input as for a stream.
 * NOT_FOUND if the book has none. No read. */
bartap_epub_result_t bartap_epub_toc_open(bartap_epub_toc_t *toc, const bartap_epub_t *book, uint8_t *window,
    size_t window_size, uint8_t *input, size_t input_size);

/* One step: at most one read. OK with *ready set: *entry is the next entry;
 * OK with *end set: there are no more, and the document has been read to its
 * end and checked; OK with neither: call again. Errors are final. */
bartap_epub_result_t bartap_epub_toc_next(bartap_epub_toc_t *toc, bartap_epub_toc_entry_t *entry, bool *ready,
    bool *end);

/* What leniency recovered from in the navigation document or NCX:
 * BARTAP_TEXT_WARNING_* bits (text.h), the same values. */
uint32_t bartap_epub_toc_warnings(const bartap_epub_toc_t *toc);

#endif
