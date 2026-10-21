#ifndef TAVERNKEEP_TESTS_FILESYSTEM_FAKE_BLOCK_DEVICE_H
#define TAVERNKEEP_TESTS_FILESYSTEM_FAKE_BLOCK_DEVICE_H

//RAM-backed block device for the filesystem-layer tests
//
//its state machine mirrors the SPI SD driver's contract (src/storage/sd_spi.c), because the adapter's
//correctness depends on exactly that contract:
//  - every operation checks the removal latch first and returns INVALID_DEVICE while it is set, then
//    returns NOT_INITIALIZED unless initialized (sd_spi_require_usable)
//  - init refuses an already-initialized device with INVALID_ARGUMENT, fails with INVALID_DEVICE (and
//    latches) when no card is present, and otherwise clears the latch
//  - a removal latches immediately but leaves the device initialized: the coordinator must deinit and
//    re-init after reinsertion
//  - is_present reports the socket (`present`), not the latch, and works whether or not the device is
//    initialized
//
//  - writes land before write_blocks returns, so sync has nothing to wait for and only checks the device
//    is usable, as the driver's does today
//
//on top of that it counts every call and can inject a result into the Nth read, write, get_info, sync,
//init or deinit.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "storage/block_device.h"

#define FAKE_BLOCK_SIZE 512U

typedef struct {
    int calls_until;                    //-1: disarmed; 0: the next call fails; n: n calls succeed first
    int repeat;                         //further calls in a row that fail after the first; 0 by default
    block_device_result_t result;
} fake_fault_t;

typedef struct {
    uint8_t *data;                      //backing store, backed_blocks * FAKE_BLOCK_SIZE bytes
    uint64_t backed_blocks;

    //what get_info reports; normally the backed geometry, overridden by the geometry tests
    uint32_t reported_block_size;
    uint64_t reported_block_count;
    bool reported_writable;

    bool present;                       //a card is in the socket
    bool latched;                       //the removal latch
    bool initialized;

    unsigned init_calls;
    unsigned deinit_calls;
    unsigned read_calls;
    unsigned write_calls;
    unsigned info_calls;
    unsigned sync_calls;
    unsigned presence_calls;

    fake_fault_t read_fault;
    fake_fault_t write_fault;
    fake_fault_t info_fault;
    fake_fault_t sync_fault;
    fake_fault_t init_fault;            //the init refuses with the result and changes nothing
    fake_fault_t deinit_fault;          //the deinit fails and stays initialized (never once latched)
    int remove_at_info;                 //-1: disarmed; n: the card is pulled during the (n+1)th get_info
    int remove_at_read;                 //-1: disarmed; n: the card is pulled during the (n+1)th read
    //-1: none. otherwise a read that includes this block fails with IO_ERROR, as an uncorrectable sector
    //does, until a write covers it: a card remaps a sector it rewrites
    int64_t bad_block;
    //0: bad_block fails every read until written. n: only the next n reads of it fail (a passing error)
    int bad_block_reads;

    block_device_t device;
} fake_disk_t;

//a present, uninitialized card of the given size, zero-filled. returns false if allocation fails
bool fake_disk_create(fake_disk_t *disk, uint64_t blocks);
void fake_disk_destroy(fake_disk_t *disk);

//the card is pulled: the latch is set as the card-detect interrupt would set it
void fake_disk_remove(fake_disk_t *disk);

//the card is pushed back in; the latch stays set until the coordinator re-initializes
void fake_disk_insert(fake_disk_t *disk);

//what the storage coordinator does to make a present card usable: deinit if still initialized (after a
//removal), then init. returns the init result
block_device_result_t fake_disk_bring_up(fake_disk_t *disk);

//the next `after` calls of that kind succeed, then one returns `result` and the fault disarms; set the
//fault's repeat afterwards to make that many more calls in a row fail too
void fake_disk_fail_read(fake_disk_t *disk, int after, block_device_result_t result);
void fake_disk_fail_write(fake_disk_t *disk, int after, block_device_result_t result);
void fake_disk_fail_info(fake_disk_t *disk, int after, block_device_result_t result);
void fake_disk_fail_sync(fake_disk_t *disk, int after, block_device_result_t result);
void fake_disk_fail_init(fake_disk_t *disk, int after, block_device_result_t result);
void fake_disk_fail_deinit(fake_disk_t *disk, int after, block_device_result_t result);

//pull the card at an exact moment: `after` more get_info calls see it, the next one finds it removed
void fake_disk_remove_at_info(fake_disk_t *disk, int after);
//the same at a read: `after` more reads succeed, the next one finds the card removed
void fake_disk_remove_at_read(fake_disk_t *disk, int after);

//total calls of any kind that could touch the medium (read, write, get_info, sync). is_present is not one:
//it samples the socket only
unsigned fake_disk_io_calls(const fake_disk_t *disk);

#endif
