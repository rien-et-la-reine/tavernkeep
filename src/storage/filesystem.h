#ifndef TAVERNKEEP_STORAGE_FILESYSTEM_H
#define TAVERNKEEP_STORAGE_FILESYSTEM_H

/* File-level access to removable storage.
 *
 * The filesystem layer is a single module, not an object: there is one card,
 * FF_VOLUMES is 1, and storage has one owner at a time (docs/architecture.md,
 * "Operating Modes and Storage Ownership"). FatFs is an implementation detail
 * of src/storage/filesystem.c; no FatFs type, header or result code appears in
 * this interface, and only the filesystem layer is built with the FatFs
 * include path (CMakeLists.txt).
 *
 * Execution model: cooperative and single-threaded. Every function here is
 * called from one core only - the one running the storage coordinator - and
 * from foreground code only: never from an interrupt handler, in particular
 * not from the card-detect handler. There is no locking, so a consumer on the
 * other core (such as an audio decoder) reaches files through the coordinator,
 * never by calling this layer itself.
 *
 * Removal before the coordinator has unmounted shows up in three ways: a call
 * already transferring sectors fails with FILESYSTEM_RESULT_STORAGE_ERROR
 * (FatFs reports every failed sector transfer alike), later calls on handles
 * opened before the removal fail with FILESYSTEM_RESULT_STALE_HANDLE, and
 * calls naming a path fail with FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE. None is
 * for the caller to recover from: abandon the work and let the coordinator's
 * ownership transition run.
 *
 * Paths and names are UTF-8 (FF_LFN_UNICODE 2). Sizes and offsets are 32-bit
 * because exFAT is not supported (FF_FS_EXFAT 0), so no file exceeds 4 GiB.
 *
 * Open entries. Any number of handles may read a file at once, but these fail
 * with FILESYSTEM_RESULT_IN_USE and change nothing, because each would corrupt
 * the volume if it went ahead:
 *   - removing or renaming a file that is open, or removing a directory being
 *     listed;
 *   - opening a file with FILESYSTEM_OPEN_REPLACE while it is open through any
 *     other handle, or opening a file at all while it is open for
 *     FILESYSTEM_OPEN_REPLACE.
 * Renaming a directory being listed is allowed: the listing follows the
 * directory, not its name. FatFs enforces the rules for files with its table
 * of open entries (FF_FS_LOCK), which identifies a file by its place in its
 * directory, so two paths naming the same file are the same entry; the layer
 * checks a listed directory itself, because that table does not cover it.
 * Closing a handle releases its entry even when the close fails, and an
 * unmount releases every entry. The entries inside a directory being listed
 * are not held by the listing. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/block_device.h"

typedef enum {
    FILESYSTEM_RESULT_OK = 0,
    FILESYSTEM_RESULT_INVALID_ARGUMENT,
    FILESYSTEM_RESULT_NOT_MOUNTED,
    FILESYSTEM_RESULT_ALREADY_MOUNTED,
    /* The card is absent, was removed, or was never initialized; mount again
     * once the coordinator has re-initialized it. */
    FILESYSTEM_RESULT_MEDIUM_UNAVAILABLE,
    /* The card is readable but holds no FAT volume (e.g. exFAT out of the
     * box); the UI can offer to format it (filesystem_format). */
    FILESYSTEM_RESULT_NO_FILESYSTEM,
    FILESYSTEM_RESULT_NOT_FOUND,
    FILESYSTEM_RESULT_EXISTS,
    /* Access refused, including a full volume or directory. */
    FILESYSTEM_RESULT_DENIED,
    /* The handle was closed, or the volume was unmounted (removal, USB mode)
     * or the card removed after the handle was opened. The handle must be
     * discarded. */
    FILESYSTEM_RESULT_STALE_HANDLE,
    FILESYSTEM_RESULT_TOO_MANY_OPEN,
    FILESYSTEM_RESULT_STORAGE_ERROR,
    FILESYSTEM_RESULT_NOT_IMPLEMENTED,
    /* The entry is open through another handle in a way the call would
     * conflict with; see "Open entries" above. Nothing was changed: close the
     * other handle and try again. Last in the list so that the numbers of the
     * results before it, which logs and validation records quote, stay the
     * same. */
    FILESYSTEM_RESULT_IN_USE,
} filesystem_result_t;

typedef enum {
    FILESYSTEM_OPEN_READ = 0,
    /* Create the file, or truncate it if it exists; write-only. */
    FILESYSTEM_OPEN_REPLACE,
} filesystem_open_mode_t;

/* Handles are small values naming a slot in the filesystem layer's own pool,
 * plus the generation that slot was given when it was opened. A slot's
 * generation advances when it is opened, when it is closed and on every
 * unmount, so an old copy of a handle
 * is detected (FILESYSTEM_RESULT_STALE_HANDLE) rather than used. Generation 0
 * is never issued, so a zero-initialized handle is always stale. */
typedef struct {
    uint8_t slot;
    uint16_t generation;
} filesystem_file_t;

typedef struct {
    uint8_t slot;
    uint16_t generation;
} filesystem_dir_t;

/* Longest entry name in bytes, excluding the terminator. Equal to FF_LFN_BUF;
 * filesystem.c checks this at compile time. */
#define FILESYSTEM_NAME_MAX 255

/* Longest wildcard pattern filesystem_opendir accepts, in bytes, excluding the
 * terminator. The layer keeps its own copy for the life of the listing. */
#define FILESYSTEM_PATTERN_MAX 31

/* Longest volume label in bytes, including the terminator (f_getlabel's
 * documented buffer size for FF_LFN_UNICODE 2). */
#define FILESYSTEM_LABEL_SIZE 34

typedef struct {
    char name[FILESYSTEM_NAME_MAX + 1];
    uint32_t size;
    bool is_directory;
} filesystem_dirent_t;

/* ------------------------------------------------------------ volume */

/* Mount the volume on an already-initialized block device. Called by the
 * storage coordinator only; from then until unmount, the filesystem layer is
 * the device's only holder. Assumes a changed medium: no state from any
 * earlier mount is reused. */
filesystem_result_t filesystem_mount(block_device_t *device);

/* Drop the volume and every open handle without any I/O, so it is safe after a
 * card removal and on entry to USB mode. Nothing is flushed: writers close
 * their files at their save points, before any transition. Idempotent, and
 * always succeeds. */
filesystem_result_t filesystem_unmount(void);

bool filesystem_is_mounted(void);

/* Create a new, empty FAT volume on an already-initialized block device,
 * erasing everything on it. The volume must not be mounted; mount afterwards to
 * use it. Called by the storage coordinator only, once the user has confirmed -
 * typically after filesystem_mount returned FILESYSTEM_RESULT_NO_FILESYSTEM.
 * FatFs picks FAT12/16 or FAT32 by card size; exFAT is never written. */
filesystem_result_t filesystem_format(block_device_t *device);

/* Copy the volume label (UTF-8, possibly empty) into a buffer of at least
 * FILESYSTEM_LABEL_SIZE bytes. */
filesystem_result_t filesystem_get_label(char *label, size_t label_size);

/* ------------------------------------------------------------- files */

filesystem_result_t filesystem_open(
    const char *path,
    filesystem_open_mode_t mode,
    filesystem_file_t *file);

/* For a file opened for writing, close is the point at which the data is
 * confirmed on the card; its result must be checked. */
filesystem_result_t filesystem_close(filesystem_file_t file);

/* Reads fewer than size bytes only at end of file. */
filesystem_result_t filesystem_read(
    filesystem_file_t file,
    void *buffer,
    size_t size,
    size_t *bytes_read);

/* Writes fewer than size bytes only when the volume is full or the file has
 * reached FAT's limit of 4 GiB - 1 bytes. */
filesystem_result_t filesystem_write(
    filesystem_file_t file,
    const void *buffer,
    size_t size,
    size_t *bytes_written);

/* Move the read/write position. On a file opened for reading, an offset past
 * the end stops at the end. On a file opened with FILESYSTEM_OPEN_REPLACE, it
 * extends the file to offset, and the added bytes are unspecified; if the
 * volume fills first, the position stops short and the result is still OK, so
 * check filesystem_tell when the exact position matters. */
filesystem_result_t filesystem_seek(filesystem_file_t file, uint32_t offset);
filesystem_result_t filesystem_tell(filesystem_file_t file, uint32_t *offset);
filesystem_result_t filesystem_size(filesystem_file_t file, uint32_t *size);

/* ------------------------------------------------------- directories */

/* Open a directory for listing. pattern is NULL to list every entry, or a
 * wildcard pattern ("*.epub"; '*' and '?' are wildcards) of at most
 * FILESYSTEM_PATTERN_MAX bytes to list only matching names. Only one listing
 * can be open at a time. */
filesystem_result_t filesystem_opendir(
    const char *path,
    const char *pattern,
    filesystem_dir_t *dir);

/* Read the next entry. "." and ".." are never returned. At the end of the
 * directory, returns OK with *end set and entry left unspecified. */
filesystem_result_t filesystem_readdir(
    filesystem_dir_t dir,
    filesystem_dirent_t *entry,
    bool *end);

filesystem_result_t filesystem_closedir(filesystem_dir_t dir);

/* ----------------------------------------------------------- entries */

/* Describe one entry. Fails with FILESYSTEM_RESULT_INVALID_ARGUMENT for the
 * root directory itself, which has no entry to describe. */
filesystem_result_t filesystem_stat(const char *path, filesystem_dirent_t *entry);

/* Rename an entry. Fails with FILESYSTEM_RESULT_EXISTS if new_path exists.
 *
 * A save point replaces a file without leaving it half-written in three steps:
 * write the new contents under a temporary name (FILESYSTEM_OPEN_REPLACE, then
 * a close whose result was OK), remove the old file, and rename the temporary
 * into place. An interruption can leave the pair in two states, and whoever
 * reads the file resolves them before using it:
 *   - the file and the temporary both exist: the interruption came before the
 *     remove, so the temporary may be incomplete; remove it and use the file.
 *   - only the temporary exists: the interruption came after the remove, and
 *     the temporary was confirmed complete before it; rename it into place.
 * (docs/architecture.md, "Operating Modes and Storage Ownership") */
filesystem_result_t filesystem_rename(const char *old_path, const char *new_path);

/* Remove a file or an empty directory. A directory that is not empty fails
 * with FILESYSTEM_RESULT_DENIED. */
filesystem_result_t filesystem_remove(const char *path);

#endif
