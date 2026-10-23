/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

/* USB-mode policy (usb_mode.h).
 *
 * State, all private to this file but the request:
 *   started                   the first step has taken the cable's state as it was
 *   cable                     VBUS as believed: the last level that held for USB_MODE_VBUS_DEBOUNCE_MS
 *   changing, changing_since  VBUS reads differently from cable, and since when
 *   entering                  an entry request is waiting for a host to configure the device
 *   lending                   the card has been asked for by USB, and not yet asked back
 *   host                      configured as the last step saw it, for the log */

#include "usb/usb_mode.h"

#include "platform/debug.h"
#include "storage/storage.h"
#include "usb/msc_disk.h"

volatile uint32_t usb_mode_request;

static bool started;
static bool cable;
static bool changing;
static uint32_t changing_since;
static bool entering;
static bool lending;
static bool host;

//true once now_ms is at least ms past since, across a wrap of the millisecond clock
static bool held_for(uint32_t now_ms, uint32_t since, uint32_t ms)
{
    return now_ms - since >= ms;
}

//VBUS, debounced: a new level is believed once it has read the same for USB_MODE_VBUS_DEBOUNCE_MS
static void watch_cable(uint32_t now_ms, bool vbus)
{
    if (!started) {
        started = true;
        cable = vbus;
        return;
    }
    if (vbus == cable) {
        changing = false;
        return;
    }
    if (!changing) {
        changing = true;
        changing_since = now_ms;
        return;
    }
    if (held_for(now_ms, changing_since, USB_MODE_VBUS_DEBOUNCE_MS)) {
        cable = vbus;
        changing = false;
        DEBUG_INFO("usb: cable %s", cable ? "connected" : "removed");
    }
}

static void give_back(const char *why)
{
    lending = false;
    (void)storage_request_owner(STORAGE_OWNER_LOCAL);
    DEBUG_INFO("usb: returning the card: %s", why);
}

static void take_request(void)
{
    const uint32_t request = usb_mode_request;
    if (request == USB_MODE_REQUEST_NONE) {
        return;
    }
    usb_mode_request = USB_MODE_REQUEST_NONE;
    if (request == USB_MODE_REQUEST_ENTER && !lending) {
        entering = true;
        DEBUG_INFO("usb: entry requested");
    } else if (request == USB_MODE_REQUEST_LEAVE) {
        entering = false;
        if (lending) {
            give_back("requested");
        }
    }
}

bool usb_mode_step(uint32_t now_ms, bool vbus, bool configured)
{
    watch_cable(now_ms, vbus);
    take_request();
    if (configured != host) {
        host = configured;
        DEBUG_INFO("usb: %s", configured ? "a host configured the device" : "the host left");
    }

    //an eject is taken whether or not the card is lent, so that one from an earlier lend is never acted on
    //later
    const bool ejected = msc_disk_take_eject();
    if (!cable) {
        entering = false;
        if (lending) {
            give_back("cable removed");
        }
    } else if (lending && ejected) {
        give_back("the host ejected it");
    } else if (lending && !configured) {
        give_back("the host left");
    } else if (entering && configured) {
        entering = false;
        if (storage_request_owner(STORAGE_OWNER_USB)) {
            lending = true;
            DEBUG_INFO("usb: lending the card to the host");
        } else {
            DEBUG_WARN("usb: entry refused: the storage coordinator is not running");
        }
    }
    return cable;
}

bool usb_mode_lending(void)
{
    return lending;
}
