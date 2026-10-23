// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_ZIP_H
#define BARTAP_ZIP_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * Zip: reading the entries of a zip archive, as an EPUB container is one.
 *
 * Finds an entry by name or lists them all, and streams an entry's contents,
 * stored or deflated, with its CRC-32 and size checked. Knows nothing of EPUB
 * itself: the OCF rules (mimetype first, stored, no extra field) are checked
 * a layer above (where, within Bartap, is Q14), which is why an entry carries
 * its index and the reader reports each entry's method and flags.
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
 * - Names are compared byte for byte, case-sensitively, as OCF requires. They
 *   are UTF-8 in an EPUB; nothing is converted (Q15).
 * - Only what an EPUB needs is supported. Anything else is refused as
 *   BARTAP_ZIP_RESULT_UNSUPPORTED rather than misread: ZIP64 (Q13),
 *   encryption (flag bit 0), split archives, and methods other than stored
 *   (0) and deflated (8).
 * - Every offset and size read from the archive is checked against the
 *   archive before it is used, and an entry may not decompress to more than
 *   its directory entry says. A damaged or hostile file is
 *   BARTAP_ZIP_RESULT_CORRUPT, never an overrun.
 * - Every field is decoded from the archive's bytes explicitly, as the
 *   little-endian value the format defines, one byte at a time: no struct is
 *   overlaid on the data, so neither the host's byte order nor its alignment
 *   rules matter.
 * - CRC-32 is computed here (it is zip's checksum, not DEFLATE's), privately
 *   in zip.c. The table-driven form costs 1 KiB of read-only data (Q12).
 *
 * Execution model: that of the byte source it reads (byte_source.h).
 *
 * Bounded work:
 * - bartap_zip_cursor_start, bartap_zip_entry_open: bounded (constant work,
 *   and for entry_open one local header read).
 * - bartap_zip_next: bounded: one directory entry, read with a fixed number
 *   of source reads however long its name, extra field or comment.
 * - bartap_zip_open: runs to completion. Finding the end record may read up to
 *   22 + 65535 bytes, in up to about 128 sector-sized reads (Q7).
 * - bartap_zip_find: runs to completion. It walks the directory until it finds
 *   the name, so its work grows with the number of entries (Q8). A caller that
 *   needs bounded steps can list with bartap_zip_next instead.
 * - bartap_zip_entry_read: NOT bounded as drafted. "Fewer than size bytes only
 *   at the end" means a call must keep reading until it has produced size
 *   bytes, and a deflated entry can consume input without producing output
 *   (empty stored blocks, for instance) for as long as its compressed size
 *   allows. Q6 has the options. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"
#include "bartap/inflate.h"

typedef enum {
    BARTAP_ZIP_RESULT_OK = 0,
    BARTAP_ZIP_RESULT_INVALID_ARGUMENT,
    /* No end of central directory record: not a zip archive, or one cut short
     * at the end. */
    BARTAP_ZIP_RESULT_NOT_A_ZIP,
    /* The archive contradicts itself or the format: an offset or size outside
     * the archive, a missing signature, a local header that disagrees with
     * the directory, a CRC-32 or size mismatch, an invalid DEFLATE stream.
     * Which of these, and where, is Q4. */
    BARTAP_ZIP_RESULT_CORRUPT,
    BARTAP_ZIP_RESULT_UNSUPPORTED,
    BARTAP_ZIP_RESULT_NOT_FOUND,
    /* The byte source failed (byte_source.h); nothing is known about the
     * archive. */
    BARTAP_ZIP_RESULT_IO_ERROR,
} bartap_zip_result_t;

typedef enum {
    BARTAP_ZIP_METHOD_STORED = 0,
    BARTAP_ZIP_METHOD_DEFLATED = 8,
} bartap_zip_method_t;

/* Longest entry name kept in a bartap_zip_entry_t, in bytes, excluding the
 * terminator. 255 is the longest name FAT and most other filesystems allow,
 * so a name can always be copied into a filesystem's directory entry. Whether
 * it should be configurable is Q10. A longer name still has its entry listed,
 * with name_truncated set, and bartap_zip_find compares the whole name against
 * the directory, so it still finds it. */
#define BARTAP_ZIP_NAME_MAX 255

/* An open archive: what its end record says, checked against the source. */
typedef struct {
    const bartap_byte_source_t *source;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint16_t entry_count;
} bartap_zip_t;

typedef struct {
    char name[BARTAP_ZIP_NAME_MAX + 1];
    uint16_t name_length;               //the whole name's, even if truncated
    bool name_truncated;
    uint16_t index;                     //0 for the first entry in the directory
    uint16_t method;                    //a bartap_zip_method_t, or what the archive said if unsupported
    uint16_t flags;                     //general-purpose bit flags, from the directory
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
} bartap_zip_entry_t;

/* A place in the central directory, for listing it. */
typedef struct {
    uint32_t offset;
    uint16_t index;
} bartap_zip_cursor_t;

/* Bytes of compressed input an entry stream reads from the source at a time:
 * one 512-byte sector, the SD card sector this was first written for, so that
 * each read stays within one sector of a filesystem's buffer. Whether the
 * caller should supply this buffer, or choose its size, is Q9. */
#define BARTAP_ZIP_STREAM_INPUT_SIZE 512U

/* An entry being read. Private, as bartap_inflate_t is, and allocated by the
 * caller (Q2). */
typedef struct {
    const bartap_zip_t *zip;
    bartap_zip_entry_t entry;
    uint32_t data_offset;               //where the entry's data starts in the archive
    uint32_t compressed_read;           //of entry.compressed_size, taken from the source
    uint32_t uncompressed_produced;     //of entry.uncompressed_size, handed to the caller
    uint32_t crc32;                     //running, over what was handed to the caller
    bartap_inflate_t inflate;           //deflated entries only
    uint8_t input[BARTAP_ZIP_STREAM_INPUT_SIZE];
    uint16_t input_start;
    uint16_t input_end;
} bartap_zip_stream_t;

/* Open the archive in *source: find and check its end record. The source must
 * outlive *zip. */
bartap_zip_result_t bartap_zip_open(bartap_zip_t *zip, const bartap_byte_source_t *source);

/* --------------------------------------------------------- directory */

void bartap_zip_cursor_start(const bartap_zip_t *zip, bartap_zip_cursor_t *cursor);

/* Read the entry at *cursor and advance it. After the last entry, returns OK
 * with *end set and entry left unspecified. */
bartap_zip_result_t bartap_zip_next(
    const bartap_zip_t *zip,
    bartap_zip_cursor_t *cursor,
    bartap_zip_entry_t *entry,
    bool *end);

/* Find the entry named name (UTF-8, not terminated by '/' unless it names a
 * directory entry). BARTAP_ZIP_RESULT_NOT_FOUND if there is none. */
bartap_zip_result_t bartap_zip_find(const bartap_zip_t *zip, const char *name, bartap_zip_entry_t *entry);

/* ------------------------------------------------------------ entries */

/* Start reading *entry, as returned by bartap_zip_next or bartap_zip_find.
 * Reads and checks its local header. window is BARTAP_INFLATE_WINDOW_SIZE
 * bytes for a deflated entry and may be NULL for a stored one; it is used
 * until the stream is abandoned. */
bartap_zip_result_t bartap_zip_entry_open(
    bartap_zip_stream_t *stream,
    const bartap_zip_t *zip,
    const bartap_zip_entry_t *entry,
    uint8_t *window);

/* Read up to size bytes of the entry's contents. Fewer than size bytes are
 * read only at the end of the entry. The CRC-32 and size are checked when the
 * end is reached, and the read that reaches it returns
 * BARTAP_ZIP_RESULT_CORRUPT if either does not match; so does any read that
 * would take the entry past its uncompressed size. A caller that reads to the
 * end and gets OK has had every byte checked; bytes it was given before then
 * are not checked yet. Whether a whole-entry convenience call should be built
 * on this is Q5. */
bartap_zip_result_t bartap_zip_entry_read(
    bartap_zip_stream_t *stream,
    void *buffer,
    size_t size,
    size_t *bytes_read);

#endif
