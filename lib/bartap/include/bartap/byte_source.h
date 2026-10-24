// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_BYTE_SOURCE_H
#define BARTAP_BYTE_SOURCE_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * Byte sources: read-only, random-access bytes for Bartap.
 *
 * A book is opened from a byte source (bartap/epub.h), and the OCF layer
 * (src/ocf.h, internal) reads the archive through this interface rather than
 * through any filesystem, so that it runs unchanged over a file on whatever
 * storage the platform has and over a buffer in memory (the host tests, and
 * an archive already in RAM if that is ever useful). A source over a
 * platform's files is an adapter that belongs to the platform, outside
 * Bartap; this header provides only the interface and a memory-backed source.
 *
 * TODO: nothing here is implemented yet. What follows is a suggested
 * interface, not a settled one; the choices are explained beside each part so
 * that they are easy to revisit.
 *
 * Choices:
 * - Positional reads (read at an offset), not seek-then-read. The OCF layer
 *   jumps between the end record, the central directory and an entry's data,
 *   and an entry being streamed and a lookup in the central directory can be
 *   interleaved. With positional reads there is no shared cursor for those to
 *   disturb, and a source never needs a "position" in its state.
 * - 32-bit offsets and sizes. ZIP64 is not supported (src/ocf.h), so nothing
 *   larger can be addressed (and FAT, the filesystem this was first written
 *   for, has no file over 4 GiB - 1). See Q13.
 * - The size is fixed when the source is made and kept in it, rather than
 *   asked for on every use: the archive is opened read-only, so it cannot
 *   change, and the OCF layer needs it first of all, to find the end record.
 * - One function pointer and a context, rather than an operations table: a
 *   source has one operation. Add a table if it grows more.
 * - No cache of its own. The OCF layer's small reads (headers of 22, 30 and
 *   46 bytes) are expected to be absorbed below the callback, by a
 *   filesystem's sector buffer for instance; any further caching belongs in
 *   the callback too. Add one here only if profiling says so.
 * - No allocation: the caller owns every structure.
 * - The callback's signature, and how a failed read is reported, are open:
 *   see Q3.
 *
 * Execution model: Bartap calls a source's read callback synchronously, from
 * inside whichever Bartap call needs the bytes, and from nowhere else. Where
 * and when that may happen (which core, foreground only) is the platform's
 * rule for its own source to keep; the memory-backed source has none.
 *
 * Bounded work:
 * - bartap_byte_source_read: bounded. One call of the read callback, for at
 *   most size bytes; how long that takes is the callback's.
 * - bartap_byte_source_memory_init: constant work. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BARTAP_BYTE_SOURCE_RESULT_OK = 0,
    BARTAP_BYTE_SOURCE_RESULT_INVALID_ARGUMENT,
    /* The bytes could not be read: the medium failed or went away. This says
     * nothing about whether the archive is valid, which is what keeps "the
     * storage is gone" apart from "the book is damaged" in the layers above.
     * It is not for the caller to recover from: abandon the work. Whether it
     * should carry more detail is Q3 and Q4. */
    BARTAP_BYTE_SOURCE_RESULT_IO_ERROR,
} bartap_byte_source_result_t;

/* Read up to size bytes starting at offset into buffer. Called only through
 * bartap_byte_source_read, which has already checked the arguments and
 * clipped the request to the source's size, so an implementation can assume
 * offset + size <= the source's size. Must then read all of it, or fail. */
typedef bartap_byte_source_result_t (*bartap_byte_source_read_fn)(
    void *context,
    uint32_t offset,
    void *buffer,
    size_t size);

typedef struct {
    bartap_byte_source_read_fn read;
    void *context;
    uint32_t size;
} bartap_byte_source_t;

/* Read up to size bytes from offset. Fewer than size bytes are read only at
 * the end of the source, and none when offset is at or past the end; neither
 * is an error. A caller that needs exactly size bytes (a fixed-size header)
 * treats a short read as a truncated archive. */
bartap_byte_source_result_t bartap_byte_source_read(
    const bartap_byte_source_t *source,
    uint32_t offset,
    void *buffer,
    size_t size,
    size_t *bytes_read);

/* ------------------------------------------------------------ memory */

typedef struct {
    const uint8_t *data;
} bartap_byte_source_memory_t;

/* Make *source read the size bytes at data. The bytes are not copied: they
 * and *memory must outlive the source. */
void bartap_byte_source_memory_init(
    bartap_byte_source_memory_t *memory,
    const void *data,
    uint32_t size,
    bartap_byte_source_t *source);

#endif
