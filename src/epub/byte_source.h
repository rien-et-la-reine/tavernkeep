#ifndef TAVERNKEEP_EPUB_BYTE_SOURCE_H
#define TAVERNKEEP_EPUB_BYTE_SOURCE_H

/* Byte sources: read-only, random-access bytes for the EPUB container layer.
 *
 * The zip reader (zip.h) reads an archive through this interface rather than
 * through filesystem.h directly, so that it runs unchanged over an open file
 * on the card (the firmware) and over a buffer in memory (the host tests, and
 * an archive already in RAM if that is ever useful).
 *
 * TODO: nothing here is implemented yet. What follows is a suggested
 * interface, not a settled one; the choices are explained beside each part so
 * that they are easy to revisit.
 *
 * Choices:
 * - Positional reads (read at an offset), not seek-then-read. A zip reader
 *   jumps between the end record, the central directory and an entry's data,
 *   and an entry being streamed and a lookup in the central directory can be
 *   interleaved. With positional reads there is no shared cursor for those to
 *   disturb, and a source never needs a "position" in its state.
 * - 32-bit offsets and sizes. FAT has no file over 4 GiB - 1 (filesystem.h),
 *   and ZIP64 is not supported (zip.h), so nothing larger can be addressed.
 * - The size is fixed when the source is made and kept in it, rather than
 *   asked for on every use: the archive is opened read-only, so it cannot
 *   change, and the zip reader needs it first of all, to find the end record.
 * - One function pointer and a context, rather than an operations table as in
 *   block_device.h: a source has one operation. Add a table if it grows more.
 * - No cache of its own. FatFs keeps a sector buffer per open file
 *   (FF_FS_TINY 0), which already absorbs the small reads a zip reader makes
 *   (headers of 22, 30 and 46 bytes) as long as they fall in one sector. Add
 *   one only if profiling on the card says so.
 * - No allocation: the caller owns every structure, as everywhere else in the
 *   firmware.
 *
 * Execution model: a file-backed source calls the filesystem layer, so it is
 * used on the storage coordinator's core, from foreground code only
 * (filesystem.h). A memory-backed source has no such limit. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/filesystem.h"

typedef enum {
    BYTE_SOURCE_RESULT_OK = 0,
    BYTE_SOURCE_RESULT_INVALID_ARGUMENT,
    /* The bytes could not be read: the card failed, was removed, or the file
     * handle went stale. This says nothing about whether the archive is
     * valid, which is what keeps "the card is gone" apart from "the book is
     * damaged" in the layers above. As with the filesystem layer, it is not
     * for the caller to recover from: abandon the work. */
    BYTE_SOURCE_RESULT_IO_ERROR,
} byte_source_result_t;

/* Read up to size bytes starting at offset into buffer. Called only through
 * byte_source_read, which has already checked the arguments and clipped the
 * request to the source's size, so an implementation can assume
 * offset + size <= the source's size. Must then read all of it, or fail. */
typedef byte_source_result_t (*byte_source_read_fn)(
    void *context,
    uint32_t offset,
    void *buffer,
    size_t size);

typedef struct {
    byte_source_read_fn read;
    void *context;
    uint32_t size;
} byte_source_t;

/* Read up to size bytes from offset. Fewer than size bytes are read only at
 * the end of the source, and none when offset is at or past the end; neither
 * is an error, as with filesystem_read. A caller that needs exactly size
 * bytes (a fixed-size header) treats a short read as a truncated archive. */
byte_source_result_t byte_source_read(
    const byte_source_t *source,
    uint32_t offset,
    void *buffer,
    size_t size,
    size_t *bytes_read);

/* ------------------------------------------------------------ memory */

typedef struct {
    const uint8_t *data;
} byte_source_memory_t;

/* Make *source read the size bytes at data. The bytes are not copied: they
 * and *memory must outlive the source. */
void byte_source_memory_init(
    byte_source_memory_t *memory,
    const void *data,
    uint32_t size,
    byte_source_t *source);

/* -------------------------------------------------------------- file */

typedef struct {
    filesystem_file_t file;
    /* The filesystem layer's result for the last read that failed, kept for
     * the log: BYTE_SOURCE_RESULT_IO_ERROR deliberately hides which it was. */
    filesystem_result_t last_error;
} byte_source_file_t;

/* Make *source read a file already opened with FILESYSTEM_OPEN_READ. Reads
 * the file's size once, here. The source does not own the file: the caller
 * closes it, after it has finished with the source.
 *
 * If the host tests should not have to link the filesystem layer and FatFs,
 * implement this half in a file of its own (byte_source_file.c) and leave it
 * out of the host build. */
byte_source_result_t byte_source_file_init(
    byte_source_file_t *file_source,
    filesystem_file_t file,
    byte_source_t *source);

#endif
