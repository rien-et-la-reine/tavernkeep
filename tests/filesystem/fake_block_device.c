// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#include "fake_block_device.h"

#include <stdlib.h>
#include <string.h>

static bool fault_fires(fake_fault_t *fault, block_device_result_t *result)
{
    if (fault->calls_until < 0) {
        return false;
    }
    if (fault->calls_until > 0) {
        fault->calls_until--;
        return false;
    }
    if (fault->repeat > 0) {
        fault->repeat--;
    } else {
        fault->calls_until = -1;
    }
    *result = fault->result;
    return true;
}

//sd_spi_require_usable: latch first, then initialization
static block_device_result_t require_usable(const fake_disk_t *disk)
{
    if (disk->latched) {
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    if (!disk->initialized) {
        return BLOCK_DEVICE_RESULT_NOT_INITIALIZED;
    }
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_init(void *context)
{
    fake_disk_t *const disk = context;
    disk->init_calls++;
    if (disk->initialized) {
        return BLOCK_DEVICE_RESULT_INVALID_ARGUMENT;
    }
    block_device_result_t injected;
    if (fault_fires(&disk->init_fault, &injected)) {
        return injected;
    }
    if (!disk->present) {
        disk->latched = true;
        return BLOCK_DEVICE_RESULT_INVALID_DEVICE;
    }
    disk->latched = false;
    disk->initialized = true;
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_deinit(void *context)
{
    fake_disk_t *const disk = context;
    disk->deinit_calls++;
    block_device_result_t injected;
    //like the driver, a deinit after a removal cannot fail: it skips the bus entirely
    if (disk->initialized && !disk->latched && fault_fires(&disk->deinit_fault, &injected)) {
        return injected;
    }
    disk->initialized = false;
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_read(void *context, uint64_t lba, void *buffer, size_t count)
{
    fake_disk_t *const disk = context;
    disk->read_calls++;
    if (disk->remove_at_read >= 0 && disk->remove_at_read-- == 0) {
        fake_disk_remove(disk);
    }
    block_device_result_t result = require_usable(disk);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return result;
    }
    if (fault_fires(&disk->read_fault, &result)) {
        //the block-device contract: after a failed read the destination is unspecified. scribble on it
        //so that a caller wrongly trusting it is caught
        memset(buffer, 0xA5, count * FAKE_BLOCK_SIZE);
        return result;
    }
    if (lba >= disk->backed_blocks || count > disk->backed_blocks - lba) {
        return BLOCK_DEVICE_RESULT_OUT_OF_RANGE;
    }
    if (disk->bad_block >= 0 && (uint64_t)disk->bad_block >= lba && (uint64_t)disk->bad_block - lba < count) {
        if (disk->bad_block_reads > 0 && --disk->bad_block_reads == 0) {
            disk->bad_block = -1;
        }
        memset(buffer, 0xA5, count * FAKE_BLOCK_SIZE);
        return BLOCK_DEVICE_RESULT_IO_ERROR;
    }
    memcpy(buffer, disk->data + lba * FAKE_BLOCK_SIZE, count * FAKE_BLOCK_SIZE);
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_write(void *context, uint64_t lba, const void *buffer, size_t count)
{
    fake_disk_t *const disk = context;
    disk->write_calls++;
    block_device_result_t result = require_usable(disk);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return result;
    }
    if (fault_fires(&disk->write_fault, &result)) {
        return result;
    }
    if (lba >= disk->backed_blocks || count > disk->backed_blocks - lba) {
        return BLOCK_DEVICE_RESULT_OUT_OF_RANGE;
    }
    memcpy(disk->data + lba * FAKE_BLOCK_SIZE, buffer, count * FAKE_BLOCK_SIZE);
    if (disk->bad_block >= 0 && (uint64_t)disk->bad_block >= lba && (uint64_t)disk->bad_block - lba < count) {
        disk->bad_block = -1;
    }
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_get_info(void *context, block_device_info_t *info)
{
    fake_disk_t *const disk = context;
    disk->info_calls++;
    if (disk->remove_at_info >= 0 && disk->remove_at_info-- == 0) {
        fake_disk_remove(disk);
    }
    block_device_result_t result = require_usable(disk);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return result;
    }
    if (fault_fires(&disk->info_fault, &result)) {
        return result;
    }
    *info = (block_device_info_t){
        .block_size_bytes = disk->reported_block_size,
        .block_count = disk->reported_block_count,
        .writable = disk->reported_writable,
    };
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_sync(void *context)
{
    fake_disk_t *const disk = context;
    disk->sync_calls++;
    block_device_result_t result = require_usable(disk);
    if (result != BLOCK_DEVICE_RESULT_OK) {
        return result;
    }
    if (fault_fires(&disk->sync_fault, &result)) {
        return result;
    }
    return BLOCK_DEVICE_RESULT_OK;
}

static block_device_result_t fake_is_present(void *context, bool *present)
{
    fake_disk_t *const disk = context;
    disk->presence_calls++;
    *present = disk->present;
    return BLOCK_DEVICE_RESULT_OK;
}

static const block_device_operations_t fake_operations = {
    .init = fake_init,
    .deinit = fake_deinit,
    .read_blocks = fake_read,
    .write_blocks = fake_write,
    .get_info = fake_get_info,
    .sync = fake_sync,
    .is_present = fake_is_present,
};

bool fake_disk_create(fake_disk_t *disk, uint64_t blocks)
{
    *disk = (fake_disk_t){
        .data = calloc((size_t)blocks, FAKE_BLOCK_SIZE),
        .backed_blocks = blocks,
        .reported_block_size = FAKE_BLOCK_SIZE,
        .reported_block_count = blocks,
        .reported_writable = true,
        .present = true,
        .read_fault = { .calls_until = -1 },
        .write_fault = { .calls_until = -1 },
        .info_fault = { .calls_until = -1 },
        .sync_fault = { .calls_until = -1 },
        .init_fault = { .calls_until = -1 },
        .deinit_fault = { .calls_until = -1 },
        .remove_at_info = -1,
        .remove_at_read = -1,
        .bad_block = -1,
    };
    disk->device = (block_device_t){ .context = disk, .operations = &fake_operations };
    return disk->data != NULL;
}

void fake_disk_destroy(fake_disk_t *disk)
{
    free(disk->data);
    disk->data = NULL;
}

void fake_disk_remove(fake_disk_t *disk)
{
    disk->present = false;
    disk->latched = true;
}

void fake_disk_insert(fake_disk_t *disk)
{
    disk->present = true;
}

block_device_result_t fake_disk_bring_up(fake_disk_t *disk)
{
    if (disk->initialized) {
        (void)block_device_deinit(&disk->device);
    }
    return block_device_init(&disk->device);
}

void fake_disk_fail_read(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->read_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_fail_write(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->write_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_fail_info(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->info_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_fail_sync(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->sync_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_fail_init(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->init_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_fail_deinit(fake_disk_t *disk, int after, block_device_result_t result)
{
    disk->deinit_fault = (fake_fault_t){ .calls_until = after, .result = result };
}

void fake_disk_remove_at_info(fake_disk_t *disk, int after)
{
    disk->remove_at_info = after;
}

void fake_disk_remove_at_read(fake_disk_t *disk, int after)
{
    disk->remove_at_read = after;
}

unsigned fake_disk_io_calls(const fake_disk_t *disk)
{
    return disk->read_calls + disk->write_calls + disk->info_calls + disk->sync_calls;
}
