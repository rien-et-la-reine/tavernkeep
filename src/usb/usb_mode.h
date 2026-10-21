#ifndef TAVERNKEEP_USB_USB_MODE_H
#define TAVERNKEEP_USB_USB_MODE_H

/* The USB-mode policy: when the card is lent to a USB host and when it comes back
 * (docs/architecture.md, "USB Mode Entry and Exit"). It decides and asks; the storage coordinator carries
 * out every change of owner (storage_request_owner), and the policy holds no block device and does no
 * I/O. It also says whether the device should be on the bus, which follows the cable.
 *
 * Entering USB mode takes a request (usb_mode_request, written by the GDB harness) and a host that has
 * configured the device. Automatic entry on configuration, with the charger timeout, comes once reader
 * and audio modes can reach a safe point on an outside request; until then a request is the only way in.
 *
 * The card comes back to the local owner when:
 *   - the host ejects it (msc_disk_take_eject)
 *   - the cable is removed: VBUS low for USB_MODE_VBUS_DEBOUNCE_MS
 *   - the host stops having the device configured (it reset the bus or unconfigured it)
 *   - usb_mode_request asks for it
 * A host that only suspends the bus keeps the card: it may still hold writes in its cache.
 *
 * Execution model: cooperative, one bounded step per pass of the main loop, on the core that runs
 * storage_step and the USB stack's task. Never from an interrupt handler. */

#include <stdbool.h>
#include <stdint.h>

/* How long VBUS must read the same before the policy believes it: a plug's contacts bounce, and a supply
 * can dip as a load is connected. */
#define USB_MODE_VBUS_DEBOUNCE_MS 50U

typedef enum {
    USB_MODE_REQUEST_NONE = 0,
    /* lend the card to USB once a host has configured the device; kept until then, or until the cable
     * is removed or a USB_MODE_REQUEST_LEAVE cancels it */
    USB_MODE_REQUEST_ENTER = 1,
    /* return the card to the local owner, or cancel an entry still waiting for a host */
    USB_MODE_REQUEST_LEAVE = 2,
} usb_mode_request_t;

/* Written by the GDB harness with a usb_mode_request_t; the next step takes it and sets it back to
 * USB_MODE_REQUEST_NONE. Any other value is taken and ignored. The firmware itself never writes a
 * request, and reads no RTT input. */
extern volatile uint32_t usb_mode_request;

/* One step. now_ms is any monotonic millisecond clock (it may wrap); vbus is one sample of the VBUS sense
 * pin (board_vbus_present); configured is whether a host has the device configured (tud_mounted).
 * Returns whether the device should be connected to the bus: whenever the cable is there. The first step
 * takes vbus as it is, so a device that boots with the cable in is on the bus from the start. */
bool usb_mode_step(uint32_t now_ms, bool vbus, bool configured);

/* Whether the policy has the card lent to USB, or asked for it to be: true from entry until the step that
 * asks for it back. */
bool usb_mode_lending(void);

#endif
