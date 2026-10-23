/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

/* Storage coordinator (storage.h).
 *
 * State, all private to this file:
 *   owner / requested_owner   who has the card, and who should
 *   medium                    the card's condition for that owner (storage_medium_t)
 *   device_up                 block_device_init has succeeded and no deinit has completed since
 *   attempts, retry_scheduled,  counted bring-up failures for the card in the socket, and whether and when
 *   retry_at_ms                 to try next; while the owner is NONE, counted release attempts instead
 *   settling, settled,        the insertion settle wait (STORAGE_INSERT_SETTLE_MS): under way and when
 *   settle_at_ms              it ends, or over for the card in the socket
 *   epoch                     storage.h; bumped whenever READY ends and on every owner change
 *
 * How each event is noticed, without the bus:
 *   removal    the driver latches it from the card-detect interrupt; block_device_get_info then
 *              reports INVALID_DEVICE. polled every step while the device is up
 *   insertion  block_device_is_present, polled every step while the device is down, except while the
 *              owner is NONE: a released coordinator does not watch the socket
 *
 * Teardown after a removal is block_device_deinit with the latch set, which the driver performs without
 * touching the card, preceded by filesystem_unmount, which performs no I/O at all. So the whole loss
 * path is safe against an absent card, as the ownership transition requires. */

#include "storage/storage.h"

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>

#include "platform/debug.h"
#include "storage/filesystem.h"

static block_device_t *device;
static storage_owner_t owner;
static storage_owner_t requested_owner;
static storage_medium_t medium;
static bool device_up;
static unsigned attempts;
static bool retry_scheduled;
static uint32_t retry_at_ms;
static bool settling;
static bool settled;
static uint32_t settle_at_ms;
static atomic_uint epoch;

/* ----------------------------------------------------------- helpers */

//true once now_ms has reached at_ms, across a wrap of the millisecond clock
static bool reached(uint32_t now_ms, uint32_t at_ms)
{
    return (int32_t)(now_ms - at_ms) >= 0;
}

static void bump_epoch(void)
{
    atomic_fetch_add_explicit(&epoch, 1U, memory_order_release);
}

//the wait before each bring-up retry: before attempt n + 2 after attempt n + 1 failed
static const uint16_t bring_up_delays_ms[] = STORAGE_BRING_UP_DELAYS_MS;
static_assert(sizeof bring_up_delays_ms / sizeof bring_up_delays_ms[0] == STORAGE_BRING_UP_ATTEMPTS - 1,
    "STORAGE_BRING_UP_DELAYS_MS needs one wait for each attempt after the first");

static void schedule_retry_after(uint32_t now_ms, uint32_t delay_ms)
{
    retry_scheduled = true;
    retry_at_ms = now_ms + delay_ms;
}

static void schedule_retry(uint32_t now_ms)
{
    schedule_retry_after(now_ms, STORAGE_RETRY_INTERVAL_MS);
}

//forget the bring-up history of the card in the socket: its counted failures, a pending retry, and its
//settle wait. called once the card is brought up, lost or left the socket, so the next card starts afresh
static void clear_retry(void)
{
    attempts = 0;
    retry_scheduled = false;
    settling = false;
    settled = false;
}

static bool card_in_socket(void)
{
    bool present = false;
    return block_device_is_present(device, &present) == BLOCK_DEVICE_RESULT_OK && present;
}

//the device has stopped answering: removed (the latch is set) or deinitialized under us. get_info
//checks the latch first and touches no bus
static bool device_gone(void)
{
    block_device_info_t info;
    const block_device_result_t result = block_device_get_info(device, &info);
    return result == BLOCK_DEVICE_RESULT_INVALID_DEVICE
        || result == BLOCK_DEVICE_RESULT_NOT_INITIALIZED;
}

//release the device; on a removal the driver does this without touching the card. a deinit that fails
//(a present card stuck busy) leaves device_up set, and the caller's state decides when to try again
static void release_device(void)
{
    if (device_up && block_device_deinit(device) == BLOCK_DEVICE_RESULT_OK) {
        device_up = false;
    }
}

//release the device, and if its deinit failed and it is still held, try again at the retry interval
//(handle_faulty). true if it is still held
static bool let_go(uint32_t now_ms)
{
    release_device();
    if (device_up) {
        schedule_retry(now_ms);
    }
    return device_up;
}

//the card is gone: drop everything that depended on it, with no I/O
static void lose_medium(void)
{
    const bool was_ready = medium == STORAGE_MEDIUM_READY;
    filesystem_unmount();
    release_device();
    medium = STORAGE_MEDIUM_ABSENT;
    clear_retry();
    if (was_ready) {
        bump_epoch();
    }
}

//a bring-up attempt failed on a card that is still there: try again later, or give up on it. kind names
//the enum result belongs to ("block device" for init, "filesystem" for mount)
static void bring_up_failed(uint32_t now_ms, const char *kind, int result)
{
    attempts++;
    DEBUG_WARN("storage: bring-up attempt %u of %d failed, %s result %d", attempts,
        STORAGE_BRING_UP_ATTEMPTS, kind, result);
    if (attempts < STORAGE_BRING_UP_ATTEMPTS) {
        medium = STORAGE_MEDIUM_PREPARING;
        schedule_retry_after(now_ms, bring_up_delays_ms[attempts - 1U]);
        return;
    }
    medium = STORAGE_MEDIUM_FAULTY;
    retry_scheduled = false;
    (void)let_go(now_ms);
}

//one clean release attempt for the NONE owner. the deinit waits out any programming busy, so once it
//succeeds the card can lose power safely. a card stuck busy is tried again at the retry interval; after
//STORAGE_RELEASE_ATTEMPTS it is reported FAULTY and kept, the request stops being pending, and whether to
//cut the rail anyway is the power manager's decision
static void attempt_release(uint32_t now_ms)
{
    release_device();
    if (!device_up) {
        return;
    }
    attempts++;
    DEBUG_WARN("storage: release attempt %u of %d failed, card still busy", attempts,
        STORAGE_RELEASE_ATTEMPTS);
    if (attempts < STORAGE_RELEASE_ATTEMPTS) {
        schedule_retry(now_ms);
        return;
    }
    medium = STORAGE_MEDIUM_FAULTY;
    retry_scheduled = false;
}

//local owner, device up: mount it and settle the medium from the result
static void mount_now(uint32_t now_ms)
{
    const filesystem_result_t result = filesystem_mount(device);
    switch (result) {
    case FILESYSTEM_RESULT_OK:
        medium = STORAGE_MEDIUM_READY;
        clear_retry();
        return;
    case FILESYSTEM_RESULT_NO_FILESYSTEM:
        medium = STORAGE_MEDIUM_UNFORMATTED;
        clear_retry();
        return;
    default:
        break;
    }
    //a failure with the device gone is a removal, whatever the mount reported: a card pulled before the
    //adapter's disk_initialize shows as MEDIUM_UNAVAILABLE, one pulled during the mount's sector reads as
    //STORAGE_ERROR. with the device still answering it is a counted failure, MEDIUM_UNAVAILABLE included:
    //that is a card the adapter refused (a sector size or capacity FatFs is not built for, or a read-only
    //card), and treating it as a removal would bring it up again forever
    if (device_gone()) {
        lose_medium();
        return;
    }
    bring_up_failed(now_ms, "filesystem", (int)result);
}

/* ------------------------------------------------------ step phases */

//phase 1: has the card gone? only a device that is up can report it; one that is down is looked after
//by the bring-up phase
static void watch_for_loss(void)
{
    if (device_up && device_gone()) {
        lose_medium();
    }
}

//phase 2: hand the card to the requested owner. unmount performs no I/O; mounting for the local owner
//is a fresh mount, because a USB host may have changed anything
static void change_owner(uint32_t now_ms)
{
    const storage_owner_t previous = owner;
    if (previous == STORAGE_OWNER_LOCAL) {
        filesystem_unmount();
    }
    owner = requested_owner;
    bump_epoch();

    if (owner == STORAGE_OWNER_NONE) {
        //release, so that the peripheral rail can be cut: whatever the medium was, nothing may use the
        //card now. a card being settled or brought up is dropped, and one that is up is deinitialized
        medium = STORAGE_MEDIUM_ABSENT;
        clear_retry();
        attempt_release(now_ms);
        return;
    }
    if (previous == STORAGE_OWNER_NONE) {
        //reclaim: the rail was probably cut while released, so the card is treated as one just
        //inserted, settle wait included, which also covers its power-up time. a card still held after
        //a failed release is let go first; if even that fails it is faulty, like any card that cannot be
        //released (handle_faulty)
        clear_retry();
        medium = let_go(now_ms) ? STORAGE_MEDIUM_FAULTY : STORAGE_MEDIUM_ABSENT;
        return;
    }

    if (!device_up || medium == STORAGE_MEDIUM_FAULTY) {
        //no card, a card still being brought up, or a faulty one: the bring-up phase carries on as it
        //would have, now for the new owner
        return;
    }
    if (owner == STORAGE_OWNER_USB) {
        //an unformatted card is lent too: formatting it is the host's business
        medium = STORAGE_MEDIUM_READY;
        clear_retry();
        return;
    }
    medium = STORAGE_MEDIUM_PREPARING;
    mount_now(now_ms);
}

//a faulty card is left alone until it leaves the socket
static void handle_faulty(uint32_t now_ms)
{
    if (device_up) {
        //an earlier deinit failed; removal is still noticed through the latch (watch_for_loss)
        if (retry_scheduled && !reached(now_ms, retry_at_ms)) {
            return;
        }
        retry_scheduled = false;
        (void)let_go(now_ms);
        return;
    }
    if (!card_in_socket()) {
        medium = STORAGE_MEDIUM_ABSENT;
        clear_retry();
    }
}

//phase 3: notice an insertion and bring the card up for the current owner, one attempt per step
static void bring_up(uint32_t now_ms)
{
    if (owner == STORAGE_OWNER_NONE) {
        //released: nothing is brought up and the socket is not watched. only a release that has not
        //succeeded yet is retried, until it succeeds or is given up as FAULTY (attempt_release stops
        //scheduling retries then)
        if (device_up && retry_scheduled && reached(now_ms, retry_at_ms)) {
            retry_scheduled = false;
            attempt_release(now_ms);
        }
        return;
    }
    if (medium == STORAGE_MEDIUM_READY || medium == STORAGE_MEDIUM_UNFORMATTED) {
        return;
    }
    if (medium == STORAGE_MEDIUM_FAULTY) {
        handle_faulty(now_ms);
        return;
    }
    if (retry_scheduled && !reached(now_ms, retry_at_ms)) {
        return;
    }
    retry_scheduled = false;

    if (!device_up) {
        if (!card_in_socket()) {
            //nothing there, or it left while we were waiting to retry
            medium = STORAGE_MEDIUM_ABSENT;
            clear_retry();
            return;
        }
        //a card that has just appeared is given time to seat and power up before the first attempt. the
        //socket is sampled on every step of the wait, and an empty reading above starts it again
        if (!settled) {
            if (!settling) {
                settling = true;
                settle_at_ms = now_ms + STORAGE_INSERT_SETTLE_MS;
                medium = STORAGE_MEDIUM_PREPARING;
                return;
            }
            if (!reached(now_ms, settle_at_ms)) {
                return;
            }
            settling = false;
            settled = true;
        }
        medium = STORAGE_MEDIUM_PREPARING;
        const block_device_result_t result = block_device_init(device);
        if (result == BLOCK_DEVICE_RESULT_INVALID_DEVICE) {
            //the socket said yes but the debounced check said no: still being inserted, or a glitch. not
            //counted against the card; it settles again from the next step that sees it, which also
            //spaces the attempts on a card that keeps being refused
            DEBUG_INFO("storage: card not seated (init refused it), waiting");
            medium = STORAGE_MEDIUM_ABSENT;
            settled = false;
            return;
        }
        if (result != BLOCK_DEVICE_RESULT_OK) {
            bring_up_failed(now_ms, "block device", (int)result);
            return;
        }
        device_up = true;
    }

    if (owner == STORAGE_OWNER_USB) {
        medium = STORAGE_MEDIUM_READY;
        clear_retry();
        return;
    }
    mount_now(now_ms);
}

/* ------------------------------------------------------------ interface */

bool storage_init(block_device_t *new_device)
{
    if (device != NULL || !block_device_is_valid(new_device)
            || new_device->operations->is_present == NULL
            || new_device->operations->sync == NULL) {
        return false;
    }
    device = new_device;
    owner = STORAGE_OWNER_LOCAL;
    requested_owner = STORAGE_OWNER_LOCAL;
    medium = STORAGE_MEDIUM_ABSENT;
    device_up = false;
    clear_retry();
    return true;
}

void storage_step(uint32_t now_ms)
{
    if (device == NULL) {
        return;
    }

    watch_for_loss();

    //requests are carried out here, at the top of a step, where no consumer is part-way through a call
    //(the loop is cooperative). when a consumer needs to reach a safe point first - persistent state
    //written before a USB handover, the audio stream stopped - that check belongs here, and the request
    //simply stays pending until it passes
    if (requested_owner != owner) {
        change_owner(now_ms);
        return;
    }

    bring_up(now_ms);

    //bounded service work for consumers goes here once there is some (the audio stream's read-ahead),
    //after transitions so that it never runs against a medium that is being lost or handed over
}

storage_owner_t storage_owner(void)
{
    return owner;
}

storage_medium_t storage_medium(void)
{
    return medium;
}

uint32_t storage_epoch(void)
{
    return (uint32_t)atomic_load_explicit(&epoch, memory_order_acquire);
}

bool storage_request_owner(storage_owner_t new_owner)
{
    if (device == NULL || (new_owner != STORAGE_OWNER_LOCAL && new_owner != STORAGE_OWNER_USB
            && new_owner != STORAGE_OWNER_NONE)) {
        return false;
    }
    requested_owner = new_owner;
    return true;
}

bool storage_request_pending(void)
{
    //a release is not done while the card is still held and still being retried; one given up as FAULTY
    //is no longer pending (storage.h)
    const bool releasing = owner == STORAGE_OWNER_NONE && device_up && medium != STORAGE_MEDIUM_FAULTY;
    return requested_owner != owner || releasing;
}

block_device_t *storage_usb_device(void)
{
    return owner == STORAGE_OWNER_USB && medium == STORAGE_MEDIUM_READY ? device : NULL;
}
