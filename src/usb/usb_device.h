/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

#ifndef TAVERNKEEP_USB_USB_DEVICE_H
#define TAVERNKEEP_USB_USB_DEVICE_H

/* The USB device: TinyUSB in device mode, presenting one mass-storage interface whose disk is
 * msc_disk.h. The device connects to the bus as soon as it is initialized; which owner holds the card is
 * not decided here, and until the card is lent to USB a host sees a reader with no card in it.
 *
 * Execution model: cooperative. usb_device_task runs TinyUSB's task, which calls the mass-storage
 * callbacks, so a READ (10) or WRITE (10) is carried out inside it, one buffer of
 * CFG_TUD_MSC_EP_BUFSIZE bytes per call into msc_disk. Call it from the main loop on the core that
 * called usb_device_init. */

#include <stdbool.h>

/* Initialize the USB controller and connect to the bus. Returns false if TinyUSB could not start. */
bool usb_device_init(void);

/* Connect to the bus or disconnect from it (the D+ pull-up), when that changes; the USB-mode policy
 * says which, following the cable. The controller cannot see a cable's removal itself (board.h), so it
 * is disconnected by hand and connects afresh when the next cable comes. */
void usb_device_attach(bool attached);

/* A host has configured the device (tud_mounted). A host that suspends the bus leaves it configured. */
bool usb_device_configured(void);

/* One pass of the USB stack's work. Does nothing before usb_device_init succeeds. */
void usb_device_task(void);

/* A host is on the bus: the device is connected to it, and a host has reset it and sent its first
 * request (tud_connected). While one is, the main loop does not idle between passes, so that each of the
 * host's commands is answered promptly. */
bool usb_device_host_present(void);

#endif
