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
 * Implemented in src/byte_source.c, but still a draft interface: the choices are
 * explained beside each part so that they are easy to revisit.
 *
 * Choices:
 * - Positional reads (read at an offset), not seek-then-read. The OCF layer
 *   jumps between the end record, the central directory and an entry's data,
 *   and an entry being streamed and a lookup in the central directory can be
 *   interleaved. With positional reads there is no shared cursor for those to
 *   disturb, and a source never needs a "position" in its state.
 * - Positions and sizes in the source are bartap_offset_t, 32 bits (D13 in
 *   DECISIONS.md): ZIP64 records are read only where their values fit in 32
 *   bits, so nothing larger is ever addressed (and FAT, the filesystem this
 *   was first written for, has no file over 4 GiB - 1). Lengths of memory
 *   buffers stay size_t. Where one meets the other - a buffer's length
 *   against what is left of the source - Bartap checks that the value fits
 *   before converting it, so a size_t wider than 32 bits never truncates.
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
 * - Reads block, and deliver everything asked for or fail (D14): there are no
 *   short reads from the callback and no "not ready" result. Every Bartap
 *   call (every step, for the calls built on steps) makes at most one read,
 *   and makes it before changing any state, so that a "not ready, call
 *   again" result could be added later without redesigning the callers.
 * - Platform error detail stays in the callback's context: a source that
 *   wants to keep the platform's own error records it there, and Bartap
 *   reports only BARTAP_BYTE_SOURCE_RESULT_IO_ERROR.
 *
 * Execution model: Bartap calls a source's read callback synchronously, from
 * inside whichever Bartap call needs the bytes, and from nowhere else. Where
 * and when that may happen (which core, foreground only) is the platform's
 * rule for its own source to keep; the memory-backed source has none. Bartap
 * keeps no shared state (D14): if one source is used from several cores at
 * once, through several Bartap objects, making that safe is the caller's
 * responsibility.
 *
 * Bounded work:
 * - bartap_byte_source_read: bounded. One call of the read callback, for at
 *   most size bytes; how long that takes is the callback's.
 * - bartap_byte_source_memory_init: constant work. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A position or size in a byte source, in bytes (D13). */
typedef uint32_t bartap_offset_t;

/* Values are fixed and the list is append-only: never renumbered or reused
 * (D18). */
typedef enum {
    BARTAP_BYTE_SOURCE_RESULT_OK = 0,
    BARTAP_BYTE_SOURCE_RESULT_INVALID_ARGUMENT = 1,
    /* The bytes could not be read: the medium failed or went away. This says
     * nothing about whether the archive is valid, which is what keeps "the
     * storage is gone" apart from "the book is damaged" in the layers above.
     * It is not for the caller to recover from: abandon the work. Any detail
     * stays in the source's context (D14). */
    BARTAP_BYTE_SOURCE_RESULT_IO_ERROR = 2,
} bartap_byte_source_result_t;

/* Read size bytes starting at offset into buffer. Called only through
 * bartap_byte_source_read, which has already checked the arguments and
 * clipped the request to the source's size, so an implementation can assume
 * offset + size <= the source's size. Blocks until it has read all of it, or
 * fails (D14). */
typedef bartap_byte_source_result_t (*bartap_byte_source_read_fn)(
    void *context,
    bartap_offset_t offset,
    void *buffer,
    size_t size);

typedef struct {
    bartap_byte_source_read_fn read;
    void *context;
    bartap_offset_t size;
} bartap_byte_source_t;

/* Read up to size bytes from offset. Fewer than size bytes are read only at
 * the end of the source, and none when offset is at or past the end; neither
 * is an error. A caller that needs exactly size bytes (a fixed-size header)
 * treats a short read as a truncated archive. */
bartap_byte_source_result_t bartap_byte_source_read(
    const bartap_byte_source_t *source,
    bartap_offset_t offset,
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
    bartap_offset_t size,
    bartap_byte_source_t *source);

#endif
