// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_OCF_H
#define BARTAP_OCF_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * OCF: the entries of an EPUB's zip container. Internal to Bartap (D9); the
 * public API is book-shaped (bartap/epub.h).
 *
 * Not a general zip reader: it enforces the OCF rules itself (D10). Only the
 * stored and deflated methods, no zip encryption, names compared as raw bytes
 * with UTF-8 checked only where flag bit 11 says so (D20), mimetype at most
 * once and META-INF/container.xml exactly once (D21). The mimetype packaging
 * rules are lenient and reported as warnings (D19).
 *
 * The central directory is the authority. Opening finds the end record (the
 * last 22 bytes first, then a backward scan over the longest comment, one
 * scratch-sized chunk per step, chunks overlapping by 3 bytes, every
 * signature confirmed by its record reaching exactly to the end of the
 * archive), the ZIP64 records if any (values must fit in 32 bits, D13), then
 * walks the whole directory once to check what the archive must satisfy, so
 * the warnings are complete once open has finished. A local header is read
 * only to find where its entry's data starts, and checked against the
 * directory for its signature, method and name length, not its name bytes
 * (D17). The directory is never loaded: lookups read it through the byte
 * source.
 *
 * Opening is split in two objects. The archive is what stays: where the
 * directory is and what the walk found, about 30 bytes, read-only once open
 * has finished and shareable by any number of cursors and streams (D22). The
 * opener holds the open steps' progress and the caller's scratch buffer, and
 * may be discarded or reused as soon as open has finished.
 *
 * Every offset and size from the file is checked before use (D6). Values are
 * decoded byte by byte as little-endian, with no struct overlaid on the data
 * (D7). Positions are bartap_offset_t, counts and indexes uint32_t (D13).
 *
 * Bounded work. Every call, and every step of a call built on steps, makes at
 * most one byte-source read, before changing any state (D14):
 * - open_begin, cursor_start, cursor_at, find_begin: no read.
 * - open_step: one read and bounded work on it.
 * - next: one read; an entry with a ZIP64 extra field to resolve takes a call
 *   per extra-field record, each returning OK with *ready unset.
 * - find_step: one read: an entry, or a piece of a name whose length matches.
 * - entry_name: one read. entry_open: one read, of the local header.
 * - entry_read: one read, then decoding bounded by the input buffer plus the
 *   caller's buffer (D12).
 * - bartap_ocf_open: a convenience call that loops open_step (D5). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"
#include "inflate.h"

/* Values are fixed and the list is append-only (D18). */
typedef enum {
    BARTAP_OCF_RESULT_OK = 0,
    BARTAP_OCF_RESULT_INVALID_ARGUMENT = 1,
    BARTAP_OCF_RESULT_NOT_A_ZIP = 2,            //no end of central directory record
    BARTAP_OCF_RESULT_CORRUPT = 3,              //the archive contradicts itself or the format
    BARTAP_OCF_RESULT_UNSUPPORTED = 4,          //ZIP64 over 32 bits, encryption, split archives, other methods
    BARTAP_OCF_RESULT_NOT_FOUND = 5,
    BARTAP_OCF_RESULT_IO_ERROR = 6,             //the byte source failed
    BARTAP_OCF_RESULT_LIMIT_EXCEEDED = 7,       //a caller-set limit (D18)
    BARTAP_OCF_RESULT_NOT_AN_EPUB = 8,          //another declared format, or no META-INF/container.xml
} bartap_ocf_result_t;

typedef enum {
    BARTAP_OCF_METHOD_STORED = 0,
    BARTAP_OCF_METHOD_DEFLATED = 8,
} bartap_ocf_method_t;

/* Warning bits (D19, D20), append-only. */
#define BARTAP_OCF_WARNING_MIMETYPE_MISSING     (1UL << 0)  //no mimetype entry, or an empty one
#define BARTAP_OCF_WARNING_MIMETYPE_NOT_FIRST   (1UL << 1)
#define BARTAP_OCF_WARNING_MIMETYPE_COMPRESSED  (1UL << 2)  //its content is then not checked
#define BARTAP_OCF_WARNING_MIMETYPE_EXTRA_FIELD (1UL << 3)  //in its local header
#define BARTAP_OCF_WARNING_NAME_NOT_UTF8        (1UL << 4)  //a name flagged UTF-8 (bit 11) that is not

/* Smallest scratch buffer for opening: the 56-byte ZIP64 end record fits. */
#define BARTAP_OCF_OPEN_SCRATCH_MIN 64U

/* Size of a central directory header before its name. */
#define BARTAP_OCF_CENTRAL_HEADER_SIZE 46U

typedef struct {
    bartap_offset_t name_offset;        //in the central directory; the entry's header is just before it
    uint16_t name_length;
    uint32_t index;                     //0 for the first entry in the directory
    uint16_t method;                    //a bartap_ocf_method_t, or what the archive said if unsupported
    uint16_t flags;                     //general-purpose bit flags, from the directory
    uint32_t crc32;
    bartap_offset_t compressed_size;    //ZIP64 values resolved
    bartap_offset_t uncompressed_size;
    bartap_offset_t local_header_offset;
} bartap_ocf_entry_t;

/* A place in the central directory, and an entry being completed across calls
 * (its ZIP64 extra field). Private. */
typedef struct {
    bartap_offset_t offset;             //of the next directory entry
    uint32_t index;
    bartap_ocf_entry_t pending;
    bartap_offset_t extra_position;     //the next extra-field record to read
    bartap_offset_t extra_end;
    bartap_offset_t next_offset;        //of the entry after the pending one
    uint8_t unresolved;                 //bits in ocf.c
    bartap_ocf_result_t status;         //sticky once an error
} bartap_ocf_cursor_t;

/* An open archive. Read-only once open has finished. Private. */
typedef struct {
    const bartap_byte_source_t *source;
    bartap_offset_t directory_offset;
    bartap_offset_t directory_size;
    uint32_t entry_count;
    uint32_t warnings;                  //BARTAP_OCF_WARNING_* bits
    bool open;                          //open has finished, successfully
} bartap_ocf_archive_t;

/* The progress of opening an archive. Private; unused once open has finished. */
typedef struct {
    bartap_ocf_archive_t *archive;
    uint8_t *scratch;                   //from the caller; NULL once open has finished
    size_t scratch_size;
    bartap_ocf_result_t status;         //sticky once an error
    bool done;
    uint8_t stage;                      //an enum in ocf.c
    bartap_offset_t end_record;
    bartap_offset_t scan_end;           //the backward scan's next chunk ends here
    bartap_offset_t scan_limit;         //and goes no lower
    bartap_offset_t candidate;          //a signature waiting to be confirmed
    bartap_offset_t zip64_record;
    bool end_record_escaped;            //some end record field says "see ZIP64"
    bool split;                         //the end record names another disk
    bartap_ocf_cursor_t walk;
    bartap_ocf_entry_t walk_entry;
    bartap_offset_t name_read;          //bytes of walk_entry's name checked
    uint8_t name_target;                //which name it may still be; an enum in ocf.c
    uint8_t utf8_continuations;
    uint8_t utf8_low;
    uint8_t utf8_high;
    bool utf8_invalid;
    uint32_t mimetype_count;
    uint32_t container_count;
    bartap_ocf_entry_t mimetype;
    bartap_ocf_entry_t container;       //META-INF/container.xml, for the book layer
    bartap_offset_t mimetype_data;
    bartap_offset_t mimetype_read;
    uint8_t mimetype_bom;
    uint8_t mimetype_text;
} bartap_ocf_opener_t;

/* An entry being found by name, a step at a time. Private. */
typedef struct {
    const bartap_ocf_archive_t *archive;
    bartap_ocf_cursor_t cursor;
    bartap_ocf_entry_t entry;           //the candidate whose name is being compared
    const uint8_t *name;                //the caller's, kept until the find finishes
    uint16_t name_length;
    uint16_t compared;                  //bytes of the candidate's name that matched so far
    bool comparing;
    bool done;
    bartap_ocf_result_t status;
} bartap_ocf_finder_t;

/* An entry being read. Private. */
typedef struct {
    const bartap_ocf_archive_t *archive;
    bartap_ocf_entry_t entry;
    bartap_offset_t data_offset;
    bartap_offset_t compressed_read;
    bartap_offset_t uncompressed_produced;
    uint32_t crc32;                     //running
    bartap_inflate_t inflate;           //deflated entries only
    uint8_t *input;                     //from the caller (D16); deflated entries only
    size_t input_size;
    size_t input_start;
    size_t input_end;
    bartap_ocf_result_t status;         //sticky once an error (D22)
    bool ended;
} bartap_ocf_stream_t;

/* ------------------------------------------------------------- opening */

/* Start opening *source into *archive (D15). scratch is at least
 * BARTAP_OCF_OPEN_SCRATCH_MIN bytes, used until open finishes. The source must
 * outlive the archive. No read. */
bartap_ocf_result_t bartap_ocf_open_begin(bartap_ocf_opener_t *opener, bartap_ocf_archive_t *archive,
    const bartap_byte_source_t *source, uint8_t *scratch, size_t scratch_size);

/* One step: one read. OK with *done set once the archive is open and its
 * warnings complete. Errors are final. When open finishes either way, the
 * opener's scratch pointer is cleared, so the caller may reuse it at once. */
bartap_ocf_result_t bartap_ocf_open_step(bartap_ocf_opener_t *opener, bool *done);

/* begin, then steps until done or an error (D5). */
bartap_ocf_result_t bartap_ocf_open(bartap_ocf_opener_t *opener, bartap_ocf_archive_t *archive,
    const bartap_byte_source_t *source, uint8_t *scratch, size_t scratch_size);

/* --------------------------------------------------------- directory */

void bartap_ocf_cursor_start(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor);

/* Place a cursor at the directory entry that starts at offset and is entry
 * index, as remembered from an earlier walk. Nothing is trusted: the entry is
 * read and checked afresh by bartap_ocf_next, and an offset outside the
 * directory or an index past the last entry is CORRUPT there. */
void bartap_ocf_cursor_at(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_offset_t offset, uint32_t index);

/* The next entry: OK with *ready set and *entry complete; OK with neither set
 * (call again); OK with *end set at the end of the directory. Errors are
 * final for the cursor. */
bartap_ocf_result_t bartap_ocf_next(const bartap_ocf_archive_t *archive, bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry, bool *ready, bool *end);

/* Where an entry's directory header starts, for bartap_ocf_cursor_at. */
static inline bartap_offset_t bartap_ocf_entry_header_offset(const bartap_ocf_entry_t *entry)
{
    return entry->name_offset - BARTAP_OCF_CENTRAL_HEADER_SIZE;
}

/* Find the first entry named name (name_length bytes, raw, D21), a step at a
 * time. name must stay valid until the find finishes. */
void bartap_ocf_find_begin(bartap_ocf_finder_t *finder, const bartap_ocf_archive_t *archive, const uint8_t *name,
    size_t name_length);

/* One step: one read. OK with *done set and *entry the match; NOT_FOUND at
 * the end of the directory. Errors are final. */
bartap_ocf_result_t bartap_ocf_find_step(bartap_ocf_finder_t *finder, bartap_ocf_entry_t *entry, bool *done);

/* Copy *entry's name into buffer with a NUL (D17). LIMIT_EXCEEDED, with
 * nothing read, unless buffer_size > name_length. One read. */
bartap_ocf_result_t bartap_ocf_entry_name(const bartap_ocf_archive_t *archive, const bartap_ocf_entry_t *entry,
    uint8_t *buffer, size_t buffer_size);

/* ------------------------------------------------------------ entries */

/* Start reading *entry. Checks first, without a read: stored sizes that
 * differ are CORRUPT (D22); encryption or another method UNSUPPORTED; a
 * declared size over max_size LIMIT_EXCEEDED (D6). Then reads the local
 * header. window (D11) and input (D16) are for a deflated entry and may be
 * NULL for a stored one; both are used until the stream is finished or
 * abandoned. */
bartap_ocf_result_t bartap_ocf_entry_open(bartap_ocf_stream_t *stream, const bartap_ocf_archive_t *archive,
    const bartap_ocf_entry_t *entry, bartap_offset_t max_size, uint8_t *window, size_t window_size,
    uint8_t *input, size_t input_size);

/* Read up to size bytes (D12): one read, so possibly fewer, even none. *end
 * is set by the read that reaches the end, which checks the CRC-32 and size;
 * bytes before it are unverified. Reads after the end return OK, nothing,
 * *end set. Errors are final (D22). size may be 0. */
bartap_ocf_result_t bartap_ocf_entry_read(bartap_ocf_stream_t *stream, void *buffer, size_t size,
    size_t *bytes_read, bool *end);

#endif
