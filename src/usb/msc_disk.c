/* USB mass-storage disk (msc_disk.h).
 *
 * State, all private to this file:
 *   presented, presented_epoch   the host has been told about the medium lent at that epoch
 *   withheld, withheld_epoch     the medium lent at that epoch is not offered: a transfer found the card
 *                                removed, or the host ejected it. a new lend has a new epoch
 *   eject_pending                an eject msc_disk_take_eject has not reported yet
 *
 * The epoch is what ties a medium to a lend: storage.h bumps it on every owner change and whenever the
 * medium stops being READY, so a medium seen at an epoch the host was told about is the same card in the
 * same hands, and any other is new.
 *
 * Sense keys and codes are SPC-4's (sense key table, and the ASC/ASCQ assignments in its annex). */

#include "usb/msc_disk.h"

#include <stddef.h>

#include "storage/block_device.h"
#include "storage/storage.h"

enum {
    SENSE_NOT_READY = 0x02,
    SENSE_MEDIUM_ERROR = 0x03,
    SENSE_HARDWARE_ERROR = 0x04,
    SENSE_ILLEGAL_REQUEST = 0x05,
    SENSE_UNIT_ATTENTION = 0x06,
};

enum {
    ASC_WRITE_ERROR = 0x0C,
    ASC_UNRECOVERED_READ_ERROR = 0x11,
    ASC_LBA_OUT_OF_RANGE = 0x21,
    ASC_INVALID_FIELD_IN_CDB = 0x24,
    ASC_NOT_READY_TO_READY_CHANGE = 0x28,
    ASC_MEDIUM_NOT_PRESENT = 0x3A,
    ASC_INTERNAL_TARGET_FAILURE = 0x44,
};

static bool presented;
static uint32_t presented_epoch;
static bool withheld;
static uint32_t withheld_epoch;
static bool eject_pending;

/* ----------------------------------------------------------- helpers */

static void set_sense(msc_disk_sense_t *sense, uint8_t key, uint8_t code)
{
    sense->key = key;
    sense->code = code;
    sense->qualifier = 0;
}

//the host is told there is no medium, so the next one it sees, whatever its epoch, is news to it
static void no_medium(msc_disk_sense_t *sense)
{
    presented = false;
    set_sense(sense, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
}

//stop offering the medium of the current lend
static void withhold(void)
{
    withheld = true;
    withheld_epoch = storage_epoch();
}

//the lent device, if the host may use it for this command; otherwise NULL with *sense set. the first
//command to find a medium the host has not been told about is failed with UNIT ATTENTION, and the host
//retries it
static block_device_t *admit(msc_disk_sense_t *sense)
{
    block_device_t *const device = storage_usb_device();
    const uint32_t epoch = storage_epoch();
    if (device == NULL || (withheld && epoch == withheld_epoch)) {
        no_medium(sense);
        return NULL;
    }
    if (!presented || epoch != presented_epoch) {
        presented = true;
        presented_epoch = epoch;
        set_sense(sense, SENSE_UNIT_ATTENTION, ASC_NOT_READY_TO_READY_CHANGE);
        return NULL;
    }
    return device;
}

//a block-device call failed: say why. medium_code is the sense for a failure of the medium itself,
//UNRECOVERED READ ERROR or WRITE ERROR. a device that has gone (removed: the driver's latch, or
//deinitialized under us) is no medium from now on, for every command, until the coordinator has dealt
//with it and the epoch has moved on
static void report_failure(block_device_result_t result, uint8_t medium_code, msc_disk_sense_t *sense)
{
    switch (result) {
    case BLOCK_DEVICE_RESULT_INVALID_DEVICE:
    case BLOCK_DEVICE_RESULT_NOT_INITIALIZED:
        withhold();
        no_medium(sense);
        return;
    case BLOCK_DEVICE_RESULT_OUT_OF_RANGE:
        set_sense(sense, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE);
        return;
    case BLOCK_DEVICE_RESULT_IO_ERROR:
    case BLOCK_DEVICE_RESULT_BUSY_TIMEOUT:
        set_sense(sense, SENSE_MEDIUM_ERROR, medium_code);
        return;
    case BLOCK_DEVICE_RESULT_OK:
    case BLOCK_DEVICE_RESULT_INVALID_ARGUMENT:
    case BLOCK_DEVICE_RESULT_NOT_IMPLEMENTED:
        break;
    }
    set_sense(sense, SENSE_HARDWARE_ERROR, ASC_INTERNAL_TARGET_FAILURE);
}

//the medium's geometry, or false with *sense set. a block size SCSI cannot carry is a fault of ours, not
//of the medium
static bool geometry(block_device_t *device, block_device_info_t *info, uint8_t medium_code,
    msc_disk_sense_t *sense)
{
    const block_device_result_t result = block_device_get_info(device, info);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        report_failure(result, medium_code, sense);
        return false;
    }
    if (info->block_size_bytes == 0U || info->block_size_bytes > UINT16_MAX || info->block_count == 0U) {
        set_sense(sense, SENSE_HARDWARE_ERROR, ASC_INTERNAL_TARGET_FAILURE);
        return false;
    }
    return true;
}

//admit a transfer of size bytes from offset bytes into block lba and turn it into whole blocks: the
//device, the first block and the block count, or NULL with *sense set
static block_device_t *locate(uint32_t lba, uint32_t offset, uint32_t size, uint8_t medium_code,
    uint64_t *first, size_t *count, msc_disk_sense_t *sense)
{
    block_device_t *const device = admit(sense);
    block_device_info_t info;
    if (device == NULL || !geometry(device, &info, medium_code, sense)) {
        return NULL;
    }
    const uint32_t block_size = info.block_size_bytes;
    if (size == 0U || size > INT32_MAX || offset % block_size != 0U || size % block_size != 0U) {
        set_sense(sense, SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB);
        return NULL;
    }
    *first = (uint64_t)lba + offset / block_size;
    *count = size / block_size;
    if (*first >= info.block_count || *count > info.block_count - *first) {
        set_sense(sense, SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE);
        return NULL;
    }
    return device;
}

/* ------------------------------------------------------------ interface */

bool msc_disk_test_unit_ready(msc_disk_sense_t *sense)
{
    return admit(sense) != NULL;
}

bool msc_disk_capacity(uint32_t *block_count, uint16_t *block_size, msc_disk_sense_t *sense)
{
    *block_count = 0;
    *block_size = 0;
    block_device_t *const device = admit(sense);
    block_device_info_t info;
    if (device == NULL || !geometry(device, &info, ASC_UNRECOVERED_READ_ERROR, sense)) {
        return false;
    }
    *block_count = info.block_count > UINT32_MAX ? UINT32_MAX : (uint32_t)info.block_count;
    *block_size = (uint16_t)info.block_size_bytes;
    return true;
}

int32_t msc_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t size, msc_disk_sense_t *sense)
{
    uint64_t first = 0;
    size_t count = 0;
    block_device_t *const device = locate(lba, offset, size, ASC_UNRECOVERED_READ_ERROR, &first, &count,
        sense);
    if (device == NULL) {
        return -1;
    }
    const block_device_result_t result = block_device_read_blocks(device, first, buffer, count);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        report_failure(result, ASC_UNRECOVERED_READ_ERROR, sense);
        return -1;
    }
    return (int32_t)size;
}

int32_t msc_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t size,
    msc_disk_sense_t *sense)
{
    uint64_t first = 0;
    size_t count = 0;
    block_device_t *const device = locate(lba, offset, size, ASC_WRITE_ERROR, &first, &count, sense);
    if (device == NULL) {
        return -1;
    }
    //write-through: the host is told the data is written only once it is on the card, so a host's
    //SYNCHRONIZE CACHE has nothing left to flush and a write that fails late fails this command
    block_device_result_t result = block_device_write_blocks(device, first, buffer, count);
    if (result == BLOCK_DEVICE_RESULT_OK) {
        result = block_device_sync(device);
    }
    if (result != BLOCK_DEVICE_RESULT_OK) {
        report_failure(result, ASC_WRITE_ERROR, sense);
        return -1;
    }
    return (int32_t)size;
}

bool msc_disk_writable(void)
{
    block_device_t *const device = storage_usb_device();
    block_device_info_t info;
    return device == NULL || block_device_get_info(device, &info) != BLOCK_DEVICE_RESULT_OK || info.writable;
}

void msc_disk_start_stop(bool start, bool load_eject)
{
    if (!load_eject || start) {
        return;
    }
    withhold();
    presented = false;
    eject_pending = true;
}

bool msc_disk_take_eject(void)
{
    const bool ejected = eject_pending;
    eject_pending = false;
    return ejected;
}
