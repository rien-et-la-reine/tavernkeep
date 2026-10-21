#ifndef TAVERNKEEP_STORAGE_STORAGE_H
#define TAVERNKEEP_STORAGE_STORAGE_H

/* The storage coordinator: the owner of the removable card.
 *
 * It holds the block device for the life of the firmware and is the only code
 * that initializes or deinitializes it, mounts or unmounts the filesystem, or
 * lends the device to the USB mass-storage class. Everything
 * else learns what it may do from the two values below and the epoch
 * (docs/architecture.md, "Operating Modes and Storage Ownership").
 *
 *   owner   who may use the card: the filesystem layer (reader and audio
 *           modes), the USB mass-storage class (USB mode), or nobody (the card
 *           released so that the peripheral rail can be powered down)
 *   medium  the card's condition, as far as that owner is concerned
 *
 * Every change of ownership and every loss of the medium goes through
 * storage_step, never through an interrupt handler: the card-detect interrupt
 * only latches the removal inside the driver, and the next step acts on it.
 * Requests (an owner change) are therefore asynchronous: a request
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
    /* Nobody: the card is released so that the power manager can cut the
     * peripheral rail, which powers the socket (docs/architecture.md, "Power
     * management"). Becoming NONE unmounts, deinitializes the device cleanly
     * (waiting out any programming busy, so that the card can lose power
     * safely) and stops bring-up: the socket is not watched, so an idle step
     * costs nothing. The medium is then ABSENT, or FAULTY if the card stayed
     * busy through STORAGE_RELEASE_ATTEMPTS clean deinits and is still held.
     * Leaving NONE treats whatever is in the socket as a card just inserted,
     * settle wait included, since the rail was probably off and the card may
     * have been swapped. The power manager turns the rail on before asking for
     * an owner again. */
    STORAGE_OWNER_NONE,
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
     * it is removed; reinserting it tries again. For the NONE owner: the card
     * could not be released, so cutting the rail now could interrupt the
     * card's own programming. */
    STORAGE_MEDIUM_FAULTY,
    /* Local owner only: the card works but holds no FAT volume (an SDXC card
     * arrives formatted as exFAT). The firmware does not format cards: the
     * user formats it as FAT32 on a computer. Left alone until it is removed. */
    STORAGE_MEDIUM_UNFORMATTED,
    /* Usable by the current owner. Local: mounted, and filesystem.h works.
     * USB: initialized and lent through storage_usb_device. */
    STORAGE_MEDIUM_READY,
} storage_medium_t;

/* Bring-up attempts (init, then mount) on an inserted card before it is
 * declared faulty, and the wait before each attempt after the first: three
 * attempts 250 ms apart, three 500 ms apart, then after a second three more
 * 250 ms apart, about 3.5 s in all. The detect switch can close well before a
 * card pushed only partly in is seated (VAL-H07), so the attempts reach past a
 * fumbled insertion. A refusal because the card turned out not to be seated
 * (INVALID_DEVICE) is not counted: the card is taken to be still arriving and
 * settles again (STORAGE_INSERT_SETTLE_MS) before the next attempt, for as
 * long as the socket reports it. */
#define STORAGE_BRING_UP_ATTEMPTS 9
#define STORAGE_BRING_UP_DELAYS_MS { 250U, 250U, 500U, 500U, 500U, 1000U, 250U, 250U }

/* The interval between other retries: release attempts, and letting go of a
 * card parked as faulty. */
#define STORAGE_RETRY_INTERVAL_MS 250U

/* Clean deinit attempts when releasing the card for the NONE owner, at
 * STORAGE_RETRY_INTERVAL_MS. Each attempt itself waits for the card to finish
 * programming (the SD driver allows 250 ms), so the whole budget is well past
 * the SD specification's write-busy limits. */
#define STORAGE_RELEASE_ATTEMPTS 4

/* How long the socket must report a card, sampled on every step, before the
 * first bring-up attempt; any sample reading empty starts the wait again. The
 * detect switch closes before the card's contacts have seated and before the
 * card has powered up (the SD specification allows up to 250 ms for the supply
 * ramp), so an attempt made at once fails on a good card - always on a
 * deliberately slow insertion (VAL-H02). The driver's own 10 ms debounce
 * rejects contact bounce; this covers seating and power-up. It applies to
 * every new card, including one present at boot, and again to a card refused
 * as not seated; not to counted retries, which already follow a settled card. */
#define STORAGE_INSERT_SETTLE_MS 500U

/* Take ownership of a block device that is configured but not initialized and
 * that implements block_device_is_present (without it an insertion would go
 * unseen) and block_device_sync (required of every device; without it no file
 * close could succeed). Starts with the local owner and an
 * absent medium; the first storage_step looks at the socket. Returns false for
 * an unusable device or if already initialized. */
bool storage_init(block_device_t *device);

/* One bounded step of foreground work: notice a removal or an insertion, carry
 * out a pending request, bring a new card up. now_ms is any monotonic
 * millisecond clock (it may wrap). A step does at most one piece of card work -
 * one bring-up attempt (init, then mount) or one owner change (with its
 * mount) - and a step with nothing to do costs one
 * presence or status check, neither of which touches the bus, or nothing at
 * all once the card is released for the NONE owner. A bring-up can
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

/* True while an owner change has been accepted but not yet done.
 * A change to NONE is done once the card is released, or given up as FAULTY;
 * until then it stays pending, so the power manager waits for
 * storage_owner() == STORAGE_OWNER_NONE and !storage_request_pending() before
 * cutting the rail, and checks for a FAULTY medium. */
bool storage_request_pending(void);

/* The block device, for the USB mass-storage class: non-NULL only while the
 * USB class owns the card and the medium is READY. The class compares
 * storage_epoch between calls to report a medium change to the host. */
block_device_t *storage_usb_device(void);

#endif
