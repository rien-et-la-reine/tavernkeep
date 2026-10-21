//tests for src/usb/usb_mode.c, the USB-mode policy, through usb_mode.h, with the real storage coordinator
//carrying out its requests, the real mass-storage disk reporting ejects, and the fake card
//
//what is asserted comes from usb_mode.h: entry only on a request and with a host that has configured the
//device; the card back on an eject, a cable removal, the host leaving or a request, and kept while the
//host stays; VBUS believed only after USB_MODE_VBUS_DEBOUNCE_MS, across a wrap of the clock; a request
//taken once; and the bus following the cable. the policy only asks, so where an owner change is checked a
//storage step follows, as the main loop runs one after the policy.
//
//the policy, the coordinator and the disk are boot-once state, so these cases rely on the runner's
//default of one process per case.

#include "fake_block_device.h"
#include "fixture.h"
#include "harness.h"
#include "storage/storage.h"
#include "usb/msc_disk.h"
#include "usb/usb_mode.h"

#define LOCAL STORAGE_OWNER_LOCAL
#define USB STORAGE_OWNER_USB

/* ------------------------------------------------------------ helpers */

static uint32_t now;

//one pass of the main loop's order: the policy, then the storage step that carries out what it asked.
//returns whether the policy wants the device on the bus
static bool pass_at(uint32_t ms, bool vbus, bool configured)
{
    now = ms;
    const bool attach = usb_mode_step(now, vbus, configured);
    storage_step(now);
    return attach;
}

static bool pass(bool vbus, bool configured)
{
    return pass_at(now + 10U, vbus, configured);
}

//a blank card brought up for the local owner, with the cable in and a host configured from the first
//pass
static bool started(void)
{
    if (!storage_init(&disk.device)) {
        return false;
    }
    (void)pass_at(0, true, true);
    (void)pass_at(STORAGE_INSERT_SETTLE_MS, true, true);
    return storage_owner() == LOCAL && storage_medium() == STORAGE_MEDIUM_UNFORMATTED;
}

//asked for, and lent
static bool lent(void)
{
    if (!started()) {
        return false;
    }
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    return usb_mode_lending() && storage_owner() == USB;
}

static bool returned(void)
{
    return !usb_mode_lending() && storage_owner() == LOCAL;
}

//the cable removed at the current time and held removed until the policy believes it; returns what the
//last pass said about the bus
static bool cable_removed(bool configured)
{
    const uint32_t from = now + 10U;
    (void)pass_at(from, false, configured);
    (void)pass_at(from + USB_MODE_VBUS_DEBOUNCE_MS - 1U, false, configured);
    return pass_at(from + USB_MODE_VBUS_DEBOUNCE_MS, false, configured);
}

/* -------------------------------------------------------------- cable */

static void usb_first_step_takes_the_cable_as_it_is(void)
{
    REQUIRE(storage_init(&disk.device));
    EXPECT(!pass_at(0, false, false));
    EXPECT(!pass(false, false));
    //and from then on a change must hold
    EXPECT(!pass(true, false));
}

static void usb_boot_with_the_cable_in_is_on_the_bus(void)
{
    REQUIRE(storage_init(&disk.device));
    EXPECT(pass_at(0, true, false));
    EXPECT(pass(true, false));
}

static void usb_vbus_is_debounced(void)
{
    REQUIRE(storage_init(&disk.device));
    REQUIRE(pass_at(1000, true, false));

    //a dip shorter than the debounce changes nothing, and the next one is timed from its own start
    EXPECT(pass_at(1100, false, false));
    EXPECT(pass_at(1100 + USB_MODE_VBUS_DEBOUNCE_MS - 1U, false, false));
    EXPECT(pass_at(1100 + USB_MODE_VBUS_DEBOUNCE_MS - 1U, true, false));
    EXPECT(pass_at(1200, false, false));
    EXPECT(pass_at(1200 + USB_MODE_VBUS_DEBOUNCE_MS - 1U, false, false));
    EXPECT(!pass_at(1200 + USB_MODE_VBUS_DEBOUNCE_MS, false, false));

    //the same on the way back in
    EXPECT(!pass_at(1300, true, false));
    EXPECT(!pass_at(1300 + USB_MODE_VBUS_DEBOUNCE_MS - 1U, true, false));
    EXPECT(pass_at(1300 + USB_MODE_VBUS_DEBOUNCE_MS, true, false));
}

static void usb_vbus_debounce_survives_clock_wrap(void)
{
    REQUIRE(storage_init(&disk.device));
    const uint32_t start = UINT32_MAX - 20U;
    REQUIRE(pass_at(start, true, false));
    EXPECT(pass_at(start + 10U, false, false));
    EXPECT(pass_at(start + 15U, false, false));                                     //before the wrap
    EXPECT(pass_at(start + 10U + USB_MODE_VBUS_DEBOUNCE_MS - 1U, false, false));    //past the wrap
    EXPECT(!pass_at(start + 10U + USB_MODE_VBUS_DEBOUNCE_MS, false, false));
}

/* -------------------------------------------------------------- entry */

static void usb_no_entry_without_a_request(void)
{
    REQUIRE(started());
    for (int i = 0; i < 20; i++) {
        (void)pass(true, true);
    }
    EXPECT(returned());
}

static void usb_entry_waits_for_a_configured_host(void)
{
    REQUIRE(started());
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, false);
    EXPECT_EQ_U(usb_mode_request, USB_MODE_REQUEST_NONE);       //taken at once
    EXPECT(returned());
    (void)pass(true, false);
    EXPECT(returned());

    (void)pass(true, true);
    EXPECT(usb_mode_lending());
    EXPECT(storage_owner() == USB);
    EXPECT(storage_usb_device() == &disk.device);
}

//a host that keeps the device configured - suspended or busy - keeps the card
static void usb_card_stays_lent_while_the_host_does(void)
{
    REQUIRE(lent());
    for (int i = 0; i < 20; i++) {
        EXPECT(pass(true, true));
    }
    EXPECT(usb_mode_lending());
    EXPECT(storage_owner() == USB);
}

static void usb_entry_needs_the_cable(void)
{
    REQUIRE(storage_init(&disk.device));
    (void)pass_at(0, false, false);
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(false, true);
    EXPECT(!usb_mode_lending());
    EXPECT_EQ_U(usb_mode_request, USB_MODE_REQUEST_NONE);

    //the request did not outlive the missing cable
    (void)pass(true, true);
    (void)pass_at(now + USB_MODE_VBUS_DEBOUNCE_MS, true, true);
    (void)pass(true, true);
    EXPECT(!usb_mode_lending());
    EXPECT(storage_owner() == LOCAL);
}

static void usb_entry_waiting_is_dropped_with_the_cable(void)
{
    REQUIRE(started());
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, false);
    EXPECT(!cable_removed(false));
    (void)pass(true, false);
    EXPECT(pass_at(now + USB_MODE_VBUS_DEBOUNCE_MS, true, false));
    (void)pass(true, true);
    EXPECT(returned());
}

static void usb_entry_refused_before_storage_starts(void)
{
    //no storage_init: the coordinator refuses every request
    (void)pass_at(0, true, true);
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    EXPECT(!usb_mode_lending());
}

/* ------------------------------------------------------------- return */

static void usb_eject_returns_the_card(void)
{
    REQUIRE(lent());
    msc_disk_start_stop(false, true);
    EXPECT(pass(true, true));           //still on the bus: the device keeps charging
    EXPECT(returned());
    EXPECT(storage_usb_device() == NULL);
    //and stays returned
    (void)pass(true, true);
    EXPECT(returned());
}

static void usb_eject_while_not_lent_is_forgotten(void)
{
    REQUIRE(started());
    msc_disk_start_stop(false, true);
    (void)pass(true, true);
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    REQUIRE(usb_mode_lending());
    (void)pass(true, true);
    EXPECT(usb_mode_lending());
    EXPECT(storage_owner() == USB);
}

static void usb_cable_removal_returns_the_card(void)
{
    REQUIRE(lent());
    const uint32_t from = now + 10U;
    (void)pass_at(from, false, true);
    (void)pass_at(from + USB_MODE_VBUS_DEBOUNCE_MS - 1U, false, true);
    EXPECT(usb_mode_lending());         //not yet believed
    EXPECT(!pass_at(from + USB_MODE_VBUS_DEBOUNCE_MS, false, true));
    EXPECT(returned());
}

//the host reset the bus or unconfigured the device: it no longer has the storage
static void usb_host_leaving_returns_the_card(void)
{
    REQUIRE(lent());
    EXPECT(pass(true, false));
    EXPECT(returned());
    //configured again by a host, it does not come back by itself
    (void)pass(true, true);
    EXPECT(returned());
}

static void usb_leave_request(void)
{
    REQUIRE(lent());
    usb_mode_request = USB_MODE_REQUEST_LEAVE;
    EXPECT(pass(true, true));
    EXPECT(returned());
    EXPECT_EQ_U(usb_mode_request, USB_MODE_REQUEST_NONE);

    //and one cancels an entry still waiting for a host
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, false);
    usb_mode_request = USB_MODE_REQUEST_LEAVE;
    (void)pass(true, false);
    (void)pass(true, true);
    EXPECT(returned());
}

static void usb_requests_that_change_nothing(void)
{
    REQUIRE(lent());
    const uint32_t epoch = storage_epoch();
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    EXPECT_EQ_U(usb_mode_request, USB_MODE_REQUEST_NONE);
    usb_mode_request = 7;
    (void)pass(true, true);
    EXPECT_EQ_U(usb_mode_request, USB_MODE_REQUEST_NONE);
    EXPECT(usb_mode_lending());
    EXPECT(storage_owner() == USB);
    EXPECT_EQ_U(storage_epoch(), epoch);

    //an entry asked for while lent is not kept for later: the host leaving in the same pass returns the
    //card, and the next host to configure the device does not get it
    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, false);
    EXPECT(returned());
    (void)pass(true, true);
    EXPECT(returned());
}

/* -------------------------------------------------- with the disk */

//the whole round as a host meets it: no medium, the request, the medium announced and used, the eject,
//no medium again, a second lend announced afresh
static void usb_round_trip_as_the_host_sees_it(void)
{
    REQUIRE(started());
    msc_disk_sense_t sense;
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense.code, 0x3A);

    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense.key, 0x06);
    EXPECT(msc_disk_test_unit_ready(&sense));

    msc_disk_start_stop(false, true);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense.code, 0x3A);
    (void)pass(true, true);
    EXPECT(returned());
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense.code, 0x3A);

    usb_mode_request = USB_MODE_REQUEST_ENTER;
    (void)pass(true, true);
    EXPECT(!msc_disk_test_unit_ready(&sense));
    EXPECT_EQ_U(sense.key, 0x06);
    EXPECT(msc_disk_test_unit_ready(&sense));
}

const test_case_t usb_mode_tests[] = {
    TEST_CASE(usb_first_step_takes_the_cable_as_it_is),
    TEST_CASE(usb_boot_with_the_cable_in_is_on_the_bus),
    TEST_CASE(usb_vbus_is_debounced),
    TEST_CASE(usb_vbus_debounce_survives_clock_wrap),
    TEST_CASE(usb_no_entry_without_a_request),
    TEST_CASE(usb_entry_waits_for_a_configured_host),
    TEST_CASE(usb_card_stays_lent_while_the_host_does),
    TEST_CASE(usb_entry_needs_the_cable),
    TEST_CASE(usb_entry_waiting_is_dropped_with_the_cable),
    TEST_CASE(usb_entry_refused_before_storage_starts),
    TEST_CASE(usb_eject_returns_the_card),
    TEST_CASE(usb_eject_while_not_lent_is_forgotten),
    TEST_CASE(usb_cable_removal_returns_the_card),
    TEST_CASE(usb_host_leaving_returns_the_card),
    TEST_CASE(usb_leave_request),
    TEST_CASE(usb_requests_that_change_nothing),
    TEST_CASE(usb_round_trip_as_the_host_sees_it),
};
const size_t usb_mode_test_count = sizeof usb_mode_tests / sizeof usb_mode_tests[0];
