// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_OCF_H
#define BARTAP_OCF_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * OCF: reading the entries of an EPUB's OCF zip container. Internal to Bartap;
 * the public API is book-shaped (bartap/epub.h, D9 in DECISIONS.md).
 *
 * Finds an entry by name or lists them all, and streams an entry's contents,
 * stored or deflated, with its CRC-32 and size checked. This is not a general
 * zip reader: it is EPUB-specific and enforces the OCF rules itself (D10):
 * - only the stored and deflated methods;
 * - no zip encryption;
 * - entry names are treated as UTF-8 and compared case-sensitively; whether
 *   invalid UTF-8 is rejected is Q15.
 * The packaging rules for mimetype (present, the first entry, stored, no
 * extra field, the exact content "application/epub+zip") are checked here
 * too, but whether a violation is rejected or accepted, and whether it is
 * reported, is Q19.
 *
 * TODO: nothing here is implemented yet. What follows is a suggested
 * interface, not a settled one; the choices are explained beside each part so
 * that they are easy to revisit.
 *
 * Choices:
 * - The central directory is the authority, not the local headers. Entries
 *   written with a data descriptor (general-purpose flag bit 3) have zero
 *   sizes and CRC in their local header, and only the central directory says
 *   where every entry is. So the reader finds the end of central directory
 *   record, walks the directory from there, and reads a local header only to
 *   find where its entry's data starts (its name and extra field can differ
 *   in length from the directory's copy). Data descriptors are then never
 *   read at all.
 * - The end record is found by scanning backwards from the end of the
 *   archive over at most 22 + 65535 bytes (the record and the longest
 *   comment), a sector-sized piece at a time rather than all at once. A
 *   candidate counts only if its comment length takes it exactly to the end
 *   of the archive, so a signature inside a comment is not mistaken for one.
 *   Where that piece's memory comes from is Q7.
 * - The directory is not loaded into memory. Lookups and listings read it
 *   through the byte source each time: an EPUB has tens to hundreds of
 *   entries, which is too much RAM to keep and too little time to matter.
 *   The layer above caches what it looks up often (the spine's entries).
 * - Names are compared byte for byte, case-sensitively, as OCF requires, with
 *   memcmp, never strcmp (no str* function is used). They are UTF-8 in an
 *   EPUB; nothing is converted (Q15).
 * - Bytes read from the archive, entry names included, are uint8_t: Bartap's
 *   own code inspects them (UTF-8 validation, path parsing), where plain
 *   char's implementation-defined signedness matters. A string the caller
 *   passes in only to be matched (bartap_ocf_find's name) is const char *, so
 *   a string literal needs no cast; memcmp compares bytes as unsigned char, so
 *   its signedness never matters.
 * - Only what an EPUB needs is supported. Anything else is refused as
 *   BARTAP_OCF_RESULT_UNSUPPORTED rather than misread: ZIP64 (Q13),
 *   encryption (flag bit 0), split archives, and methods other than stored
 *   (0) and deflated (8).
 * - Every offset and size read from the archive is checked against the
 *   archive before it is used, and an entry may not decompress to more than
 *   its directory entry says, nor to more than the max_size its caller
 *   accepts (D6 in DECISIONS.md). A damaged or hostile file is
 *   BARTAP_OCF_RESULT_CORRUPT, never an overrun.
 * - Every field is decoded from the archive's bytes explicitly, as the
 *   little-endian value the format defines, one byte at a time: no struct is
 *   overlaid on the data, so neither the host's byte order nor its alignment
 *   rules matter.
 * - CRC-32 is computed here (it is zip's checksum, not DEFLATE's), privately
 *   in ocf.c. The table-driven form costs 1 KiB of read-only data (Q12).
 *
 * Execution model: that of the byte source it reads (byte_source.h).
 *
 * Bounded work:
 * - bartap_ocf_cursor_start, bartap_ocf_entry_open: bounded (constant work,
 *   and for entry_open one local header read).
 * - bartap_ocf_next: bounded: one directory entry, read with a fixed number
 *   of source reads however long its name, extra field or comment.
 * - bartap_ocf_open: runs to completion, as drafted. Finding the end record may
 *   read up to 22 + 65535 bytes, in up to about 128 sector-sized reads. D5 in
 *   DECISIONS.md allows that only for a convenience call built on bounded
 *   steps, and those steps are not drafted yet (Q7).
 * - bartap_ocf_find: runs to completion, as a convenience call built on
 *   bartap_ocf_next, which D5 allows. It walks the directory until it finds
 *   the name, so its work grows with the number of entries (Q8). A caller that
 *   needs bounded steps lists with bartap_ocf_next itself.
 * - bartap_ocf_entry_read: bounded (D5). It stops once its budget is spent,
 *   with fewer than size bytes if need be, even none, and reports the end of
 *   the entry separately. What sets the budget is Q6. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"
#include "inflate.h"

typedef enum {
    BARTAP_OCF_RESULT_OK = 0,
    BARTAP_OCF_RESULT_INVALID_ARGUMENT,
    /* No end of central directory record: not a zip archive, or one cut short
     * at the end. */
    BARTAP_OCF_RESULT_NOT_A_ZIP,
    /* The archive contradicts itself or the format: an offset or size outside
     * the archive, a missing signature, a local header that disagrees with
     * the directory, a CRC-32 or size mismatch, an invalid DEFLATE stream.
     * Which of these, and where, is Q4. */
    BARTAP_OCF_RESULT_CORRUPT,
    BARTAP_OCF_RESULT_UNSUPPORTED,
    BARTAP_OCF_RESULT_NOT_FOUND,
    /* The byte source failed (byte_source.h); nothing is known about the
     * archive. */
    BARTAP_OCF_RESULT_IO_ERROR,
} bartap_ocf_result_t;

typedef enum {
    BARTAP_OCF_METHOD_STORED = 0,
    BARTAP_OCF_METHOD_DEFLATED = 8,
} bartap_ocf_method_t;

/* Longest entry name kept in a bartap_ocf_entry_t, in bytes, excluding the
 * terminator. 255 is the longest name FAT and most other filesystems allow,
 * so a name can always be copied into a filesystem's directory entry. Whether
 * it should be configurable is Q10. A longer name is never truncated, since a
 * truncated name can match a different entry's; whether its entry is rejected
 * or skipped is Q10 too. */
#define BARTAP_OCF_NAME_MAX 255

/* An open archive: what its end record says, checked against the source. */
typedef struct {
    const bartap_byte_source_t *source;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint16_t entry_count;
} bartap_ocf_t;

typedef struct {
    uint8_t name[BARTAP_OCF_NAME_MAX + 1];  //UTF-8, NUL-terminated
    uint16_t name_length;               //bytes in name, excluding the terminator
    uint16_t index;                     //0 for the first entry in the directory
    uint16_t method;                    //a bartap_ocf_method_t, or what the archive said if unsupported
    uint16_t flags;                     //general-purpose bit flags, from the directory
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
} bartap_ocf_entry_t;

/* A place in the central directory, for listing it. */
typedef struct {
    uint32_t offset;
    uint16_t index;
} bartap_ocf_cursor_t;

/* Bytes of compressed input an entry stream reads from the source at a time:
 * one 512-byte sector, the SD card sector this was first written for, so that
 * each read stays within one sector of a filesystem's buffer. Whether the
 * caller should supply this buffer, or choose its size, is Q9. */
#define BARTAP_OCF_STREAM_INPUT_SIZE 512U

/* An entry being read. Private, as bartap_inflate_t is, and allocated by the
 * caller (Q2). */
typedef struct {
    const bartap_ocf_t *zip;
    bartap_ocf_entry_t entry;
    uint32_t data_offset;               //where the entry's data starts in the archive
    uint32_t compressed_read;           //of entry.compressed_size, taken from the source
    uint32_t uncompressed_produced;     //of entry.uncompressed_size, handed to the caller
    uint32_t crc32;                     //running, over what was handed to the caller
    bartap_inflate_t inflate;           //deflated entries only
    uint8_t input[BARTAP_OCF_STREAM_INPUT_SIZE];
    uint16_t input_start;
    uint16_t input_end;
} bartap_ocf_stream_t;

/* Open the archive in *source: find and check its end record. The source must
 * outlive *zip. */
bartap_ocf_result_t bartap_ocf_open(bartap_ocf_t *zip, const bartap_byte_source_t *source);

/* --------------------------------------------------------- directory */

void bartap_ocf_cursor_start(const bartap_ocf_t *zip, bartap_ocf_cursor_t *cursor);

/* Read the entry at *cursor and advance it. After the last entry, returns OK
 * with *end set and entry left unspecified. */
bartap_ocf_result_t bartap_ocf_next(
    const bartap_ocf_t *zip,
    bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry,
    bool *end);

/* Find the entry named name (UTF-8, NUL-terminated, not terminated by '/'
 * unless it names a directory entry). BARTAP_OCF_RESULT_NOT_FOUND if there is
 * none. Built on bartap_ocf_next, so it never matches an entry through a
 * truncated copy of its name (Q10). name's length is found by Bartap's own
 * scan for the terminator, not strlen, and it is compared with each entry's
 * name bytewise with memcmp. */
bartap_ocf_result_t bartap_ocf_find(const bartap_ocf_t *zip, const char *name, bartap_ocf_entry_t *entry);

/* ------------------------------------------------------------ entries */

/* Start reading *entry, as returned by bartap_ocf_next or bartap_ocf_find.
 * Reads and checks its local header. max_size is the most the caller will
 * accept: an entry whose directory says it is larger is refused before
 * anything else is read (with which result is a smaller decision in Q5), so a
 * decompression bomb fails cleanly (D6 in DECISIONS.md). window and
 * window_size are passed to bartap_inflate_init for a deflated entry, which
 * refuses a window smaller than BARTAP_INFLATE_WINDOW_SIZE (D11); for a
 * stored entry they may be NULL and 0. The window is used until the stream is
 * abandoned. */
bartap_ocf_result_t bartap_ocf_entry_open(
    bartap_ocf_stream_t *stream,
    const bartap_ocf_t *zip,
    const bartap_ocf_entry_t *entry,
    uint32_t max_size,
    uint8_t *window,
    size_t window_size);

/* Read up to size bytes of the entry's contents. Bounded (D5 in
 * DECISIONS.md): a call returns OK with fewer than size bytes, possibly none,
 * once its budget is spent (what sets the budget is Q6), so a short read does
 * not mean the end. *end is set by the read that reaches the end of the entry.
 * The CRC-32 and size are checked then, and that read returns
 * BARTAP_OCF_RESULT_CORRUPT if either does not match; so does any read that
 * would take the entry past its uncompressed size. A caller that reads to the
 * end and gets OK has had every byte checked; bytes it was given before then
 * are not checked yet. Whether a whole-entry convenience call should be built
 * on this is Q5. */
bartap_ocf_result_t bartap_ocf_entry_read(
    bartap_ocf_stream_t *stream,
    void *buffer,
    size_t size,
    size_t *bytes_read,
    bool *end);

#endif
