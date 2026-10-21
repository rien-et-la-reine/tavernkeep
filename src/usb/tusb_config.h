#ifndef TAVERNKEEP_USB_TUSB_CONFIG_H
#define TAVERNKEEP_USB_TUSB_CONFIG_H

/* TinyUSB's configuration, found by name by TinyUSB's own sources (CMakeLists.txt puts src/usb on the
 * include path). Device mode on the RP2350's one USB controller, with a single mass-storage interface and
 * nothing else: stdio stays on RTT or UART, and TAVERNKEEP_STDIO_USB is refused in a build with this
 * class (docs/architecture.md, "USB Mode Entry and Exit"). The MCU and OS options come from the Pico SDK's
 * TinyUSB target. */

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_MSC 1
#define CFG_TUD_CDC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

/* The buffer TinyUSB hands a READ (10) or WRITE (10) over in. It must be a whole number of 512-byte
 * blocks (msc_disk.h), and each buffer becomes one multi-block transfer on the card, so it is the size of
 * the card transfers in USB mode: 8 blocks, the transfer size the bulk measurements in docs/validation.md
 * ran at on a 4 KiB-cluster volume. */
#define CFG_TUD_MSC_EP_BUFSIZE 4096

#endif
