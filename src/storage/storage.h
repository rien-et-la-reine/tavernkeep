#ifndef TAVERNKEEP_STORAGE_STORAGE_H
#define TAVERNKEEP_STORAGE_STORAGE_H

/* The storage coordinator: the owner of the removable card.
 *
 * It holds the block device for the life of the firmware and is the only code
 * that initializes or deinitializes it, mounts, unmounts or formats the
 * filesystem, or lends the device to the USB mass-storage class. Everything
 * else learns what it may do from the two values below and the epoch
 * (docs/architecture.md, "Operating Modes and Storage Ownership").
 *
 *   owner   who may use the card: the filesystem layer (reader and audio
 *           modes) or the USB mass-storage class (USB mode)
 *   medium  the card's condition, as far as that owner is concerned
 *
 * Every change of ownership and every loss of the medium goes through
 * storage_step, never through an interrupt handler: the card-detect interrupt
 * only latches the removal inside the driver, and the next step acts on it.
 * Requests (an owner change, a format) are therefore asynchronous: a request
 * is recorded, and a later step carries it out. Callers watch storage_owner,
 * storage_medium and storage_request_pending for the outcome. This is also
 * the seam where consumers will be given time to reach a safe point before a
 * transition - persistent state written before a USB handover, the audio
 * stream stopped - without the interface changing.
 *
 * Execution model: cooperative. storage_step and the requests are called from
 * the core that runs the storage coordinator, which is also the only core that
 * calls the filesystem layer. storage_epoch alone may be read from any core.
 */

#include <stdbool.h>
#include <stdint.h>

#include "storage/block_device.h"

typedef enum {
    /* The filesystem layer, through filesystem.h. */
    STORAGE_OWNER_LOCAL = 0,
    /* The USB mass-storage class, through the bare block device
     * (storage_usb_device). No filesystem is mounted for the duration. */
    STORAGE_OWNER_USB,
} storage_owner_t;

typedef enum {
    /* No card in the socket (or an absent-or-write-protected one, which the
     * hardware cannot tell apart). Insertion is watched for. */
    STORAGE_MEDIUM_ABSENT = 0,
    /* A card is in the socket and is being brought up (initialized, and for
     * the local owner mounted), including the settle time after it appears
     * and the wait between attempts. */
    STORAGE_MEDIUM_PREPARING,
    /* A card is in the socket but could not be brought up (or, for the local
     * owner, mounted) after STORAGE_BRING_UP_ATTEMPTS tries. Left alone until
     * it is removed; reinserting it tries again. */
    STORAGE_MEDIUM_FAULTY,
    /* Local owner only: the card works but holds no FAT volume (an SDXC card
     * arrives formatted as exFAT). storage_request_format can fix that. */
    STORAGE_MEDIUM_UNFORMATTED,
    /* Usable by the current owner. Local: mounted, and filesystem.h works.
     * USB: initialized and lent through storage_usb_device. */
    STORAGE_MEDIUM_READY,
} storage_medium_t;

/* Bring-up attempts (init, then mount) on an inserted card before it is
 * declared faulty, and the interval between them. A refusal because the card
 * turned out not to be seated (INVALID_DEVICE) is not counted: it is retried at
 * the same interval for as long as the socket reports a card. */
#define STORAGE_BRING_UP_ATTEMPTS 3
#define STORAGE_RETRY_INTERVAL_MS 250U

/* How long the socket must report a card, sampled on every step, before the
 * first bring-up attempt; any sample reading empty starts the wait again. The
 * detect switch closes before the card's contacts have seated and before the
 * card has powered up (the SD specification allows up to 250 ms for the supply
 * ramp), so an attempt made at once fails on a good card - always on a
 * deliberately slow insertion (VAL-H02). The driver's own 10 ms debounce
 * rejects contact bounce; this covers seating and power-up. It applies to
 * every new card, including one present at boot, and not to retries, which
 * already follow a settled card. */
#define STORAGE_INSERT_SETTLE_MS 500U

/* Take ownership of a block device that is configured but not initialized and
 * that implements block_device_is_present. Starts with the local owner and an
 * absent medium; the first storage_step looks at the socket. Returns false for
 * an unusable device or if already initialized. */
bool storage_init(block_device_t *device);

/* One bounded step of foreground work: notice a removal or an insertion, carry
 * out a pending request, bring a new card up. now_ms is any monotonic
 * millisecond clock (it may wrap). A step does at most one piece of card work -
 * one bring-up attempt (init, then mount), one owner change (with its mount),
 * or one format (with its mount) - and a step with nothing to do costs one
 * presence or status check, neither of which touches the bus. A bring-up can
 * block for the card's own initialization time; that only happens when a card
 * has just arrived or the owner has just changed. Does nothing before
 * storage_init. */
void storage_step(uint32_t now_ms);

storage_owner_t storage_owner(void);
storage_medium_t storage_medium(void);

/* Changes every time something a consumer may have cached about the card stops
 * being valid: whenever the medium stops being READY for its owner, and on
 * every change of owner. A consumer records the epoch with whatever it caches
 * (a bookmark, a directory listing, a buffered stream) and discards the cache
 * when the epoch differs. Readable from any core. */
uint32_t storage_epoch(void);

/* Ask for the card to change hands. Accepted (true) unless storage_init has
 * not been called; asking for the current owner cancels a pending change.
 * The change happens in a later step whatever the medium's state, so USB mode
 * can be entered and left with no card in the socket. */
bool storage_request_owner(storage_owner_t owner);

/* Ask for the card to be formatted with a new, empty FAT volume, erasing it.
 * Only for the local owner and an UNFORMATTED or READY medium; refused (false)
 * otherwise, and dropped if the medium or owner changes before it runs. The
 * caller has had the user confirm. Success shows as the medium becoming READY
 * with an empty volume. */
bool storage_request_format(void);

/* True while an owner change or a format has been accepted but not yet done. */
bool storage_request_pending(void);

/* The block device, for the USB mass-storage class: non-NULL only while the
 * USB class owns the card and the medium is READY. The class compares
 * storage_epoch between calls to report a medium change to the host. */
block_device_t *storage_usb_device(void);

#endif
