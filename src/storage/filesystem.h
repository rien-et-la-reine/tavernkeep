#ifndef TAVERNKEEP_STORAGE_FILESYSTEM_H
#define TAVERNKEEP_STORAGE_FILESYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#include "storage/block_device.h"

typedef enum {
    FILESYSTEM_RESULT_OK = 0,
    FILESYSTEM_RESULT_INVALID_ARGUMENT,
    FILESYSTEM_RESULT_STORAGE_ERROR,
    FILESYSTEM_RESULT_NOT_IMPLEMENTED,
} filesystem_result_t;

typedef struct {
    block_device_t *block_device;
    bool mounted;
} filesystem_t;

filesystem_result_t filesystem_prepare(
    filesystem_t *filesystem,
    block_device_t *block_device);

filesystem_result_t filesystem_mount(filesystem_t *filesystem);
filesystem_result_t filesystem_unmount(filesystem_t *filesystem);
bool filesystem_is_mounted(const filesystem_t *filesystem);

/* ---------------------------------------------------- adapter internals */

/* The FatFs disk I/O adapter (src/storage/diskio.c) is part of this layer.
 * FatFs only ever passes a physical drive number to disk_*(), so the owner of
 * the block device binds it to that number before mounting and unbinds it
 * after unmounting or when the medium is taken away. These are called from
 * filesystem_mount()/filesystem_unmount() only; nothing above the filesystem
 * layer should call them, and nothing above it should hold the block device
 * at all (docs/architecture.md, "Operating Modes and Storage Ownership"). */

/* Physical drive numbers FatFs knows about; FF_VOLUMES in ffconf.h is 1, so
 * only drive 0 exists. */
enum {
    FILESYSTEM_DRIVE_SD = 0,
};

/* Bind a block device to a physical drive number. The device must already be
 * configured; whether it is also initialized is the owner's business (see
 * disk_initialize in diskio.c). Returns false for an invalid drive number, a
 * NULL device, or a drive that is already bound. */
bool filesystem_diskio_bind(uint8_t drive, block_device_t *device);

/* Unbind the device from a drive number. Safe to call when nothing is bound.
 * Performs no I/O, so it is safe after a card removal. */
void filesystem_diskio_unbind(uint8_t drive);

/* The device currently bound to a drive number, or NULL. */
block_device_t *filesystem_diskio_device(uint8_t drive);

#endif

