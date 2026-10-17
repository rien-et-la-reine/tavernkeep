#ifndef TAVERNKEEP_STORAGE_FILESYSTEM_DISKIO_H
#define TAVERNKEEP_STORAGE_FILESYSTEM_DISKIO_H

/* Private to the filesystem layer: included by src/storage/filesystem.c and
 * src/storage/diskio.c only. Nothing above the filesystem layer may include
 * this header, and nothing above it should hold the block device at all
 * (docs/architecture.md, "Operating Modes and Storage Ownership").
 *
 * The FatFs disk I/O adapter (src/storage/diskio.c) is part of the filesystem
 * layer. FatFs only ever passes a physical drive number to disk_*(), so
 * filesystem_mount() binds the block device to that number before calling
 * f_mount and filesystem_unmount() unbinds it afterwards; filesystem_format()
 * binds it around f_mkfs in the same way. */

#include <stdbool.h>
#include <stdint.h>

#include "storage/block_device.h"

/* Physical drive numbers FatFs knows about; FF_VOLUMES in ffconf.h is 1, so
 * only drive 0 exists. */
enum {
    FILESYSTEM_DRIVE_SD = 0,
};

/* Bind a block device to a physical drive number. The device must already be
 * initialized by the storage coordinator (see disk_initialize in diskio.c).
 * Returns false for an invalid drive number, a device that fails
 * block_device_is_valid(), or a drive that is already bound. */
bool filesystem_diskio_bind(uint8_t drive, block_device_t *device);

/* Unbind the device from a drive number. Safe to call when nothing is bound.
 * Performs no I/O, so it is safe after a card removal. */
void filesystem_diskio_unbind(uint8_t drive);

/* The device currently bound to a drive number, or NULL. */
block_device_t *filesystem_diskio_device(uint8_t drive);

#endif
