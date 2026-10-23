// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

//glue layer for FatFs
//
//implements the five disk_*() functions FatFs calls over the block-device interface. this file replaces the
//stub third_party/fatfs/source/diskio.c, which is deliberately not compiled.
//
//the adapter is dumb by design: no retries, no caching, no policy. it forwards sectors and counts, maps
//result codes, and remembers one thing per drive - whether the medium has reported itself gone. FatFs
//never tests STA_NODISK: it tests STA_NOINIT (mount_volume and validate in ff.c), and STA_PROTECT
//before a write, which this adapter never reports. so a vanished medium is reported as
//STA_NOINIT | STA_NODISK, and STA_NOINIT is the bit FatFs acts on: open objects then fail validation with
//FR_INVALID_OBJECT, and the next mount attempt re-runs disk_initialize, which keeps failing until the
//owner re-initializes the device. STA_NODISK only records why.
//
//result mapping:
//  BLOCK_DEVICE_RESULT_OK               -> RES_OK
//  BLOCK_DEVICE_RESULT_INVALID_DEVICE   -> RES_NOTRDY, and the drive is marked no-disk
//  BLOCK_DEVICE_RESULT_NOT_INITIALIZED  -> RES_NOTRDY, and the drive is marked not initialized (the owner
//                                          deinitialized the device under a mounted volume)
//  BLOCK_DEVICE_RESULT_OUT_OF_RANGE     -> RES_PARERR
//  BLOCK_DEVICE_RESULT_INVALID_ARGUMENT -> RES_PARERR
//  BLOCK_DEVICE_RESULT_IO_ERROR         -> RES_ERROR
//  BLOCK_DEVICE_RESULT_BUSY_TIMEOUT     -> RES_ERROR
//  BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED  -> RES_ERROR
//
//status bits:
//  STA_NOINIT  until disk_initialize has succeeded on a bound device, again after unbind or a
//              NOT_INITIALIZED result, and whenever STA_NODISK is set
//  STA_NODISK  once any operation has returned INVALID_DEVICE, until a disk_initialize succeeds (which
//              cannot happen before the owner re-initializes the device - see disk_initialize)
//  STA_PROTECT never: absent-or-protected is one unavailable state at the driver (architecture decision)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ff.h"
#include "diskio.h"

#include "storage/block_device.h"
#include "storage/filesystem_diskio.h"

//per-drive adapter state
typedef struct {
    block_device_t *device;
    bool initialized;   //disk_initialize succeeded since bind
    bool no_disk;       //the device reported INVALID_DEVICE; cleared by a successful disk_initialize
} diskio_drive_t;

static diskio_drive_t drives[FF_VOLUMES];

//map a block-device result onto a FatFs DRESULT, recording a vanished medium on the way
static DRESULT map_result(diskio_drive_t *drive, block_device_result_t result)
{
    //every value listed and no default, so -Wswitch flags any block_device_result_t added later
    switch (result) {
    case BLOCK_DEVICE_RESULT_OK:
        return RES_OK;
    case BLOCK_DEVICE_RESULT_INVALID_DEVICE:
        drive->no_disk = true;
        return RES_NOTRDY;
    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:
        drive->initialized = false;
        return RES_NOTRDY;
    case BLOCK_DEVICE_RESULT_OUT_OF_RANGE:
    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:
        return RES_PARERR;
    case BLOCK_DEVICE_RESULT_IO_ERROR:
    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:
    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:
        return RES_ERROR;
    }
    return RES_ERROR;
}

//the drive for a FatFs physical drive number, or NULL if there is no such drive
static diskio_drive_t *drive_at(BYTE pdrv)
{
    return pdrv < FF_VOLUMES ? &drives[pdrv] : NULL;
}

//bound, initialized and not reported gone: the only state in which i/o is forwarded
static bool drive_ready(const diskio_drive_t *drive)
{
    return drive->device != NULL && drive->initialized && !drive->no_disk;
}

/* ---------------------------------------------------- owner interface */

bool filesystem_diskio_bind(uint8_t drive, block_device_t *device)
{
    if (drive >= FF_VOLUMES || !block_device_is_valid(device) || drives[drive].device != NULL) {
        return false;
    }
    drives[drive] = (diskio_drive_t){ .device = device };
    return true;
}

void filesystem_diskio_unbind(uint8_t drive)
{
    if (drive < FF_VOLUMES) {
        drives[drive] = (diskio_drive_t){ .device = NULL };
    }
}

block_device_t *filesystem_diskio_device(uint8_t drive)
{
    return drive < FF_VOLUMES ? drives[drive].device : NULL;
}

/* ------------------------------------------------------ FatFs interface */

DSTATUS disk_status(BYTE pdrv)
{
    const diskio_drive_t *const drive = drive_at(pdrv);
    if (drive == NULL || drive->device == NULL) {
        return STA_NOINIT;
    }
    if (drive->no_disk) {
        return STA_NOINIT | STA_NODISK;
    }
    return drive->initialized ? 0 : STA_NOINIT;
}

//the owner has already called block_device_init before mounting, so this never calls it: the sd driver
//refuses a second init, and FatFs re-calls disk_initialize on every remount while STA_NOINIT is set.
//instead it confirms the device is usable through block_device_get_info, which touches no bus and checks
//the driver's removal latch first - so after a removal it keeps returning INVALID_DEVICE (and no_disk
//stays set) until the owner has re-run block_device_init
DSTATUS disk_initialize(BYTE pdrv)
{
    diskio_drive_t *const drive = drive_at(pdrv);
    if (drive == NULL || drive->device == NULL) {
        return STA_NOINIT;
    }

    block_device_info_t info;
    if (map_result(drive, block_device_get_info(drive->device, &info)) != RES_OK) {
        drive->initialized = false;
        return disk_status(pdrv);
    }

    //FatFs is built for 512-byte sectors only (FF_MIN_SS == FF_MAX_SS) and 32-bit sector numbers
    //(FF_LBA64 0). a read-only device is refused like an absent one: read-only operation is not a Tome
    //mode, which is also why STA_PROTECT is never reported
    if (info.block_size_bytes != FF_MAX_SS || info.block_count == 0U
            || info.block_count > UINT32_MAX || !info.writable) {
        drive->initialized = false;
        return STA_NOINIT;
    }

    drive->initialized = true;
    drive->no_disk = false;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    diskio_drive_t *const drive = drive_at(pdrv);
    if (drive == NULL || buff == NULL || count == 0U) {
        return RES_PARERR;
    }
    if (!drive_ready(drive)) {
        return RES_NOTRDY;
    }
    //sector and count forwarded unchanged: a multi-block request is the driver's CMD18 path
    return map_result(drive, block_device_read_blocks(drive->device, sector, buff, count));
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    diskio_drive_t *const drive = drive_at(pdrv);
    if (drive == NULL || buff == NULL || count == 0U) {
        return RES_PARERR;
    }
    if (!drive_ready(drive)) {
        return RES_NOTRDY;
    }
    //CMD24 for one block, CMD25 for several
    return map_result(drive, block_device_write_blocks(drive->device, sector, buff, count));
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
#if !FF_USE_MKFS
    (void)buff;     //only the commands f_mkfs asks take a buffer
#endif
    diskio_drive_t *const drive = drive_at(pdrv);
    if (drive == NULL) {
        return RES_PARERR;
    }
    //gated like disk_read, so that a sync after a removal is not reported as success
    if (!drive_ready(drive)) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        //FatFs has already written everything it buffers; this waits for the device to finish with it
        //(block_device_sync). a write the device accepted but failed later shows up here, so the
        //f_close or f_sync behind it fails instead of reporting data that may not be on the card
        return map_result(drive, block_device_sync(drive->device));

#if FF_USE_MKFS
    //only f_mkfs asks for these, and the firmware does not build it (ffconf.h); the host tests do, to
    //create the volumes they run on
    case GET_SECTOR_COUNT: {
        if (buff == NULL) {
            return RES_PARERR;
        }
        block_device_info_t info;
        const DRESULT result = map_result(drive, block_device_get_info(drive->device, &info));
        if (result != RES_OK) {
            return result;
        }
        //checked by disk_initialize; a different card needs a fresh initialize, but stay safe regardless
        if (info.block_count > UINT32_MAX) {
            return RES_ERROR;
        }
        *(LBA_t *)buff = (LBA_t)info.block_count;
        return RES_OK;
    }

    case GET_BLOCK_SIZE:
        if (buff == NULL) {
            return RES_PARERR;
        }
        //erase block size in sectors; unknown here, and 1 is FatFs's documented "unknown" value
        *(DWORD *)buff = 1U;
        return RES_OK;
#endif

    default:
        //GET_SECTOR_SIZE is never asked while FF_MIN_SS == FF_MAX_SS, CTRL_TRIM needs FF_USE_TRIM, and
        //GET_SECTOR_COUNT and GET_BLOCK_SIZE need FF_USE_MKFS
        return RES_PARERR;
    }
}
