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
 * - entry names are compared as raw bytes; a name whose entry has
 *   general-purpose flag bit 11 set is checked as UTF-8 at open, and an
 *   invalid one sets BARTAP_OCF_WARNING_NAME_NOT_UTF8 rather than being
 *   refused (D20);
 * - mimetype may appear at most once and META-INF/container.xml exactly once
 *   (D21).
 * The packaging rules for mimetype are applied leniently (D19): a mimetype
 * that is missing, empty, compressed, not the first entry or has an extra
 * field opens anyway and sets a warning bit; one that declares another format
 * is refused as BARTAP_OCF_RESULT_NOT_AN_EPUB.
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
 *   find where its entry's data starts, checking its signature, method and
 *   name length against the directory's but not its name bytes (D17). Data
 *   descriptors are never read at all.
 * - Opening is a series of bounded steps (D15), each making at most one read
 *   (D14) into a scratch buffer the caller supplies. The first step reads the
 *   last 22 bytes, where the end record of an archive with no comment is.
 *   Failing that, later steps scan backwards over at most the last 22 + 65535
 *   bytes (the record and the longest comment), one scratch-sized chunk per
 *   step; the first chunk ends at the end of the archive, and each next one
 *   ends 3 bytes after the start of the one before, so a 4-byte signature
 *   always lies wholly in one chunk. A signature found is confirmed by
 *   reading its 22-byte record in the next step: it counts only if its
 *   comment length takes it exactly to the end of the archive, so a signature
 *   inside a comment is not mistaken for one. Then come the ZIP64 records, if
 *   any, and a walk over the whole central directory (below).
 * - ZIP64 is read where its values fit in 32 bits (D13). If a ZIP64 end of
 *   central directory locator lies just before the end record, its record's
 *   values replace the end record's; a 0xFFFF or 0xFFFFFFFF field in the end
 *   record with no locator is BARTAP_OCF_RESULT_CORRUPT. In the directory, a
 *   size or offset of 0xFFFFFFFF is taken from the entry's ZIP64 extra field.
 *   Every 8-byte value is decoded as two 32-bit halves, and one whose upper
 *   half is not zero is BARTAP_OCF_RESULT_UNSUPPORTED. No 64-bit arithmetic
 *   is used. A marker with no ZIP64 field to resolve it is CORRUPT.
 * - The walk at open visits every directory entry once, one read per step,
 *   to check what the whole archive must satisfy: each entry's signature;
 *   mimetype and META-INF/container.xml each appearing as D21 requires; the
 *   mimetype's position, method, extra field and, if it is stored, content
 *   (D19); and the UTF-8 validity of every name flagged as UTF-8 (D20). So
 *   the warnings are complete once open finishes, and the open archive is
 *   read-only from then on.
 * - A stored mimetype's content is accepted if it is "application/epub+zip",
 *   optionally preceded by a UTF-8 byte order mark and followed by ASCII
 *   spaces, tabs, CRs and LFs, none of which sets a warning. An empty one is
 *   treated as missing. Anything else declares another format:
 *   BARTAP_OCF_RESULT_NOT_AN_EPUB (D19). A compressed mimetype's content is
 *   not checked.
 * - The directory is not loaded into memory. Lookups and listings read it
 *   through the byte source each time: an EPUB has tens to hundreds of
 *   entries, which is too much RAM to keep and too little time to matter.
 *   The layer above caches what it looks up often (the spine's entries).
 * - An entry holds where its name is in the central directory and how long
 *   it is, not the name itself (D17), so entries stay small enough to cache
 *   and names have no length limit. bartap_ocf_entry_name copies a name out
 *   when it is wanted.
 * - Names are compared byte for byte, case-sensitively, as OCF requires, with
 *   memcmp, never strcmp (no str* function is used). Nothing is converted or
 *   normalized (D20).
 * - Bytes read from the archive, entry names included, are uint8_t: Bartap's
 *   own code inspects them (UTF-8 validation, path parsing), where plain
 *   char's implementation-defined signedness matters. A string the caller
 *   passes in only to be matched (bartap_ocf_find's name) is const char *, so
 *   a string literal needs no cast; memcmp compares bytes as unsigned char, so
 *   its signedness never matters.
 * - Only what an EPUB needs is supported. Anything else is refused as
 *   BARTAP_OCF_RESULT_UNSUPPORTED rather than misread: ZIP64 values over 32
 *   bits, encryption (flag bit 0), split archives (a disk number other than
 *   0), and methods other than stored (0) and deflated (8).
 * - Every offset and size read from the archive is checked against the
 *   archive before it is used (D6 in DECISIONS.md). An entry may not
 *   decompress to more than its directory entry says (CORRUPT), nor be opened
 *   if that is more than the max_size its caller accepts
 *   (BARTAP_OCF_RESULT_LIMIT_EXCEEDED, D18). A stored entry whose compressed
 *   and uncompressed sizes differ is CORRUPT; a deflated entry larger
 *   compressed than uncompressed is legal (D22). A damaged or hostile file is
 *   an error result, never an overrun.
 * - Every field is decoded from the archive's bytes explicitly, as the
 *   little-endian value the format defines, one byte at a time: no struct is
 *   overlaid on the data, so neither the host's byte order nor its alignment
 *   rules matter.
 * - Positions and sizes in the archive are bartap_offset_t; lengths of memory
 *   buffers are size_t; counts and indexes are uint32_t (D13). A size_t
 *   length is checked to fit before it is compared with or added to a
 *   bartap_offset_t.
 * - CRC-32 is computed here (it is zip's checksum, not DEFLATE's), privately
 *   in ocf.c. The table-driven form costs 1 KiB of read-only data (Q12).
 *
 * Execution model: that of the byte source it reads (byte_source.h). Once
 * open has finished, a bartap_ocf_t is read-only: any number of cursors and
 * streams may use one at once (D22), each keeping its own progress.
 *
 * Bounded work. Every call, and every step of a call built on steps, makes at
 * most one byte-source read, before changing any state (D14):
 * - bartap_ocf_open_begin, bartap_ocf_cursor_start: constant work, no read.
 * - bartap_ocf_open_step: one read and a bounded amount of work on it.
 * - bartap_ocf_next: one read. An entry with no ZIP64 field to resolve
 *   completes in one call; one that has needs a call per extra-field record
 *   read, each returning OK with *ready unset until the entry is complete.
 * - bartap_ocf_entry_name: one read, of the name.
 * - bartap_ocf_entry_open: one read, of the local header.
 * - bartap_ocf_entry_read: bounded (D12). It makes at most one read - for a
 *   deflated entry a refill of the input buffer, for a stored one a read
 *   straight into the caller's buffer - and decodes from what it has, so its
 *   work is bounded by the input buffer's size plus size, as
 *   bartap_inflate_run's is by its buffers. It may return fewer than size
 *   bytes, even none, and reports the end of the entry separately.
 * - bartap_ocf_open: runs to completion, as a convenience call that loops
 *   bartap_ocf_open_step (D5, D15). Its work grows with the size of the
 *   comment scanned and the number of entries walked.
 * - bartap_ocf_find: runs to completion, as a convenience call built on
 *   bartap_ocf_next, which D5 allows. It walks the directory until it finds
 *   the name, comparing name lengths first and reading only the names whose
 *   length matches, so its work grows with the number of entries (Q8). A
 *   caller that needs bounded steps lists with bartap_ocf_next itself. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bartap/byte_source.h"
#include "inflate.h"

/* Values are fixed and the list is append-only: never renumbered or reused
 * (D18). */
typedef enum {
    BARTAP_OCF_RESULT_OK = 0,
    BARTAP_OCF_RESULT_INVALID_ARGUMENT = 1,
    /* No end of central directory record: not a zip archive, or one cut short
     * at the end. */
    BARTAP_OCF_RESULT_NOT_A_ZIP = 2,
    /* The archive contradicts itself or the format: an offset or size outside
     * the archive, a missing signature, a local header that disagrees with
     * the directory, a CRC-32 or size mismatch, an invalid DEFLATE stream, a
     * duplicate mimetype or META-INF/container.xml. Which of these, and
     * where, is not reported: codes only (D18). */
    BARTAP_OCF_RESULT_CORRUPT = 3,
    BARTAP_OCF_RESULT_UNSUPPORTED = 4,
    BARTAP_OCF_RESULT_NOT_FOUND = 5,
    /* The byte source failed (byte_source.h); nothing is known about the
     * archive. */
    BARTAP_OCF_RESULT_IO_ERROR = 6,
    /* A limit the caller set was exceeded: entry_open's max_size, or a buffer
     * the caller supplied too small for what it must hold. The same code, with
     * the same meaning, appears in each layer that takes caller-set limits
     * (D18). */
    BARTAP_OCF_RESULT_LIMIT_EXCEEDED = 7,
    /* A valid zip archive that is not an EPUB: its mimetype declares another
     * format, or it has no META-INF/container.xml (D19, D21). */
    BARTAP_OCF_RESULT_NOT_AN_EPUB = 8,
} bartap_ocf_result_t;

typedef enum {
    BARTAP_OCF_METHOD_STORED = 0,
    BARTAP_OCF_METHOD_DEFLATED = 8,
} bartap_ocf_method_t;

/* Bits of bartap_ocf_t's warnings: rules the archive breaks that do not stop
 * it opening (D19, D20). Append-only, as the result codes are (D18). */
#define BARTAP_OCF_WARNING_MIMETYPE_MISSING     (1UL << 0)  //no mimetype entry, or an empty one
#define BARTAP_OCF_WARNING_MIMETYPE_NOT_FIRST   (1UL << 1)
#define BARTAP_OCF_WARNING_MIMETYPE_COMPRESSED  (1UL << 2)  //its content is then not checked
#define BARTAP_OCF_WARNING_MIMETYPE_EXTRA_FIELD (1UL << 3)  //in its local header
#define BARTAP_OCF_WARNING_NAME_NOT_UTF8        (1UL << 4)  //a name flagged UTF-8 (bit 11) that is not

/* Smallest scratch buffer bartap_ocf_open_begin accepts: room for the largest
 * fixed-size record a step reads, the 56-byte ZIP64 end of central directory
 * record. A larger one makes the backward scan take fewer, larger steps. */
#define BARTAP_OCF_OPEN_SCRATCH_MIN 64U

/* An open archive: what its end records say, checked against the source, and
 * what the walk at open found. Read-only once open has finished.
 *
 * TODO: the open steps' progress (which stage, the scan position, a candidate
 * end record, the walk's position and what it has seen of mimetype and
 * META-INF/container.xml) goes here too, once the implementation settles it;
 * it is private, and unused after open. */
typedef struct {
    const bartap_byte_source_t *source;
    uint8_t *scratch;                   //from the caller; NULL once open has finished
    size_t scratch_size;                //0 once open has finished
    bartap_offset_t directory_offset;
    bartap_offset_t directory_size;
    uint32_t entry_count;
    uint32_t warnings;                  //BARTAP_OCF_WARNING_* bits
    bartap_ocf_result_t status;         //of the open steps; sticky once an error
} bartap_ocf_t;

typedef struct {
    bartap_offset_t name_offset;        //where the name is, in the central directory
    uint16_t name_length;               //bytes, with no terminator; there is no other limit
    uint32_t index;                     //0 for the first entry in the directory
    uint16_t method;                    //a bartap_ocf_method_t, or what the archive said if unsupported
    uint16_t flags;                     //general-purpose bit flags, from the directory
    uint32_t crc32;
    bartap_offset_t compressed_size;    //ZIP64 values resolved
    bartap_offset_t uncompressed_size;
    bartap_offset_t local_header_offset;
} bartap_ocf_entry_t;

/* A place in the central directory, for listing it, and the progress of an
 * entry that needs more than one call of bartap_ocf_next (its ZIP64 extra
 * field). Private.
 *
 * TODO: the extra-field progress (where in the extra field, which values are
 * still to resolve) goes here once the implementation settles it. */
typedef struct {
    bartap_offset_t offset;
    uint32_t index;
    bartap_ocf_entry_t pending;         //the entry being completed
} bartap_ocf_cursor_t;

/* An entry being read. Private, and allocated by the caller (Q2). */
typedef struct {
    const bartap_ocf_t *ocf;
    bartap_ocf_entry_t entry;
    bartap_offset_t data_offset;        //where the entry's data starts in the archive
    bartap_offset_t compressed_read;    //of entry.compressed_size, taken from the source
    bartap_offset_t uncompressed_produced; //of entry.uncompressed_size, handed to the caller
    uint32_t crc32;                     //running, over what was handed to the caller
    bartap_inflate_t inflate;           //deflated entries only
    uint8_t *input;                     //from the caller (D16); deflated entries only
    size_t input_size;
    size_t input_start;
    size_t input_end;
    bartap_ocf_result_t status;         //sticky once an error (D22)
    bool ended;
} bartap_ocf_stream_t;

/* ------------------------------------------------------------- opening */

/* Start opening the archive in *source (D15). scratch is scratch_size bytes,
 * at least BARTAP_OCF_OPEN_SCRATCH_MIN, used only until open has finished.
 * The source must outlive *ocf. Makes no read.
 * BARTAP_OCF_RESULT_INVALID_ARGUMENT for a NULL pointer or a scratch buffer
 * that is too small, in which case no scratch pointer is kept. */
bartap_ocf_result_t bartap_ocf_open_begin(
    bartap_ocf_t *ocf,
    const bartap_byte_source_t *source,
    uint8_t *scratch,
    size_t scratch_size);

/* Take one step of opening: one read, and the checks on it. Returns OK with
 * *done unset while there is more to do, and OK with *done set once the
 * archive is open and its warnings complete. Any error is final: every later
 * step returns it again.
 *
 * Open has finished when a step sets *done or returns an error. Either way,
 * that step clears *ocf's scratch pointer and size to NULL and 0, so no stale
 * pointer to the scratch buffer remains: the caller may reuse that memory at
 * once, as an inflate window for instance (D15). */
bartap_ocf_result_t bartap_ocf_open_step(bartap_ocf_t *ocf, bool *done);

/* bartap_ocf_open_begin, then bartap_ocf_open_step until done or an error. A
 * convenience call that runs to completion (D5). */
bartap_ocf_result_t bartap_ocf_open(
    bartap_ocf_t *ocf,
    const bartap_byte_source_t *source,
    uint8_t *scratch,
    size_t scratch_size);

/* --------------------------------------------------------- directory */

void bartap_ocf_cursor_start(const bartap_ocf_t *ocf, bartap_ocf_cursor_t *cursor);

/* Read the next entry at *cursor (D14, D15). Each call makes one read:
 * - OK with *ready set: *entry is complete, and the cursor has moved past it;
 * - OK with neither set: the entry needs another call (its ZIP64 extra field
 *   is being read); call again with the same cursor;
 * - OK with *end set: there are no more entries, and *entry is unspecified.
 * Any error is final for this cursor. */
bartap_ocf_result_t bartap_ocf_next(
    const bartap_ocf_t *ocf,
    bartap_ocf_cursor_t *cursor,
    bartap_ocf_entry_t *entry,
    bool *ready,
    bool *end);

/* Find the entry named name (NUL-terminated, compared as raw bytes, not
 * terminated by '/' unless it names a directory entry).
 * BARTAP_OCF_RESULT_NOT_FOUND if there is none. If several entries have the
 * name, the first in the directory is returned (D21). Built on
 * bartap_ocf_next. name's length is found by Bartap's own scan for the
 * terminator, not strlen; only entries whose name has that length are read
 * from the source, a fixed-size piece at a time on the stack, and compared
 * with memcmp. */
bartap_ocf_result_t bartap_ocf_find(const bartap_ocf_t *ocf, const char *name, bartap_ocf_entry_t *entry);

/* Copy *entry's name into buffer, followed by a NUL (D17). buffer_size must be
 * at least entry->name_length + 1, or the result is
 * BARTAP_OCF_RESULT_LIMIT_EXCEEDED and nothing is read. One read. The name is
 * raw bytes: valid UTF-8 only if the archive's is (D20). */
bartap_ocf_result_t bartap_ocf_entry_name(
    const bartap_ocf_t *ocf,
    const bartap_ocf_entry_t *entry,
    uint8_t *buffer,
    size_t buffer_size);

/* ------------------------------------------------------------ entries */

/* Start reading *entry, as returned by bartap_ocf_next or bartap_ocf_find.
 *
 * Checks first, without reading: a stored entry whose compressed and
 * uncompressed sizes differ is BARTAP_OCF_RESULT_CORRUPT (D22); an encrypted
 * entry, or one with an unsupported method, is BARTAP_OCF_RESULT_UNSUPPORTED;
 * and one whose directory says it is larger than max_size, the most the
 * caller will accept, is BARTAP_OCF_RESULT_LIMIT_EXCEEDED, so a decompression
 * bomb fails cleanly (D6, D18). Then reads the local header and checks its
 * signature, method and name length against the directory (D17).
 *
 * For a deflated entry, window and window_size are passed to
 * bartap_inflate_init, which refuses a window smaller than
 * BARTAP_INFLATE_WINDOW_SIZE (D11), and input is input_size bytes, not zero,
 * for compressed data read from the source (D16). For a stored entry all four
 * may be NULL and 0: its data is read straight into the caller's buffer. The
 * window and input buffer are used until the stream is finished or abandoned. */
bartap_ocf_result_t bartap_ocf_entry_open(
    bartap_ocf_stream_t *stream,
    const bartap_ocf_t *ocf,
    const bartap_ocf_entry_t *entry,
    bartap_offset_t max_size,
    uint8_t *window,
    size_t window_size,
    uint8_t *input,
    size_t input_size);

/* Read up to size bytes of the entry's contents. Bounded (D12 in
 * DECISIONS.md): a call makes at most one read and decodes from what it has,
 * so it may return OK with fewer than size bytes, possibly none (after a run
 * of empty stored blocks, for instance), and a short read does not mean the
 * end. *end is set by the read that reaches the end of the entry; keep calling
 * until it is. A read after that returns OK with no bytes and *end set again.
 *
 * Bytes delivered before *end are unverified. The CRC-32 and size are checked
 * only by the read that reaches the end, and that read returns
 * BARTAP_OCF_RESULT_CORRUPT if either does not match; so does any read that
 * would take the entry past its uncompressed size. If it does, the caller
 * must discard, or re-read, anything it used from this entry: it was not the
 * entry's content. Only a caller that reads to the end and gets OK has had
 * every byte checked. Whether a whole-entry convenience call should be built
 * on this is Q5.
 *
 * Every error is final for the stream, BARTAP_OCF_RESULT_IO_ERROR included
 * (D22): later reads return it again. To try again after an I/O error, open
 * the entry again. */
bartap_ocf_result_t bartap_ocf_entry_read(
    bartap_ocf_stream_t *stream,
    void *buffer,
    size_t size,
    size_t *bytes_read,
    bool *end);

#endif
