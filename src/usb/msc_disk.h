/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

#ifndef TAVERNKEEP_USB_MSC_DISK_H
#define TAVERNKEEP_USB_MSC_DISK_H

/* The disk the USB mass-storage class presents to a host: SCSI semantics over the block device the
 * storage coordinator lends to the USB owner (storage_usb_device). It holds no device of its own and
 * changes no owner; it only answers the host's commands. It does not include TinyUSB, so the host tests
 * run it over the real coordinator and the fake card; usb_device.c adapts it to TinyUSB's callbacks.
 *
 * What the host sees (docs/architecture.md, "USB Mode Entry and Exit"):
 *   - no medium (NOT READY, MEDIUM NOT PRESENT) while storage_usb_device() is NULL: before a handover,
 *     with no card in the socket, while a card is brought up, with a faulty card, after the card has gone
 *     back to the local owner, and after the host has ejected it. A card a transfer finds removed counts
 *     as no medium at once, without waiting for the coordinator's next step to notice
 *   - UNIT ATTENTION, NOT READY TO READY CHANGE, on the first command that finds a medium the host has not
 *     been told about: the first time one is lent, and whenever storage_epoch() has moved on since (a
 *     removal and reinsertion, or a new handover), so that the host drops what it cached about the old one
 *   - then the medium itself, write-through: every write is synced to the card before it is reported done
 *
 * Every command that fails gives the reason in *sense, for the host's REQUEST SENSE; on success *sense is
 * left alone.
 *
 * Execution model: cooperative, from the USB stack's task in the main loop, the same loop that runs
 * storage_step. Never from an interrupt handler. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t key;        //sense key
    uint8_t code;       //additional sense code (ASC)
    uint8_t qualifier;  //additional sense code qualifier (ASCQ)
} msc_disk_sense_t;

/* TEST UNIT READY: true if the host may use the medium now. */
bool msc_disk_test_unit_ready(msc_disk_sense_t *sense);

/* READ CAPACITY and READ FORMAT CAPACITIES: the medium's block count and block size, or false with both
 * 0. READ CAPACITY (10) carries the last block's address in 32 bits, so a medium of more than UINT32_MAX
 * blocks is offered as UINT32_MAX of them; the largest SD card, 2 TB, is 2^32 blocks, and its last block
 * is the one left out. */
bool msc_disk_capacity(uint32_t *block_count, uint16_t *block_size, msc_disk_sense_t *sense);

/* READ (10) and WRITE (10) data: size bytes starting offset bytes into block lba, as TinyUSB hands a
 * command over one buffer at a time. offset and size must be whole blocks (CFG_TUD_MSC_EP_BUFSIZE is a
 * multiple of the block size), and size more than 0. Return size, or -1 with *sense set. A write returns
 * only once its data is on the card (block_device_sync). */
int32_t msc_disk_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t size, msc_disk_sense_t *sense);
int32_t msc_disk_write(uint32_t lba, uint32_t offset, const void *buffer, uint32_t size,
    msc_disk_sense_t *sense);

/* Whether the host may write, for MODE SENSE's write-protect flag and before every WRITE (10). True with
 * no medium, so that the write itself reports the missing medium rather than a protected one. Changes no
 * state. */
bool msc_disk_writable(void);

/* START STOP UNIT. An eject (load_eject set, start clear) withholds the medium from the host until
 * storage_epoch() moves on, as it does when the card goes back to the local owner, and is recorded for
 * msc_disk_take_eject. Anything else - a load, a power condition - is accepted and changes nothing: a card
 * has no power states, and loading is the USB-mode policy's business, not the host's. */
void msc_disk_start_stop(bool start, bool load_eject);

/* True once for each eject since the last call: the USB-mode policy's cue to return the card to the
 * local owner. */
bool msc_disk_take_eject(void);

#endif
