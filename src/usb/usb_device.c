/* SPDX-FileCopyrightText: 2026 Harsh Patel
 * SPDX-License-Identifier: MIT */

/* USB device (usb_device.h): the descriptors and TinyUSB callbacks for one mass-storage interface, each
 * callback handing straight on to msc_disk.h.
 *
 * TinyUSB 0.18 reports every failed READ (10) or WRITE (10) as NOT READY, MEDIUM NOT PRESENT: it sets that
 * sense itself after the callback returns, over whatever the callback set. So the sense msc_disk gives a
 * failed transfer is kept here and written into the answer to the REQUEST SENSE the host sends next
 * (tud_msc_request_sense_cb), and a medium error or an out-of-range address reaches the host as itself
 * rather than as a removal. It is kept exactly as long as TinyUSB keeps its own sense: until a REQUEST SENSE
 * reads it, or a later failure replaces it. */

#include "usb/usb_device.h"

#include <stdio.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#include "usb/msc_disk.h"

/* ------------------------------------------------------- descriptors */

//TinyUSB's example vendor ID and its product ID for a mass-storage-only device: for development only.
//a device given to anyone else needs IDs of its own: Raspberry Pi grants product IDs under its vendor ID
//(0x2E8A) to RP2350-based products, and pid.codes grants them to open-source hardware
#define USB_VID 0xCAFEU
#define USB_PID 0x4002U

//the version in BCD, major.minor.patch as 0xMMmp
#define USB_BCD_DEVICE ((TAVERNKEEP_VERSION_MAJOR << 8) | (TAVERNKEEP_VERSION_MINOR << 4) \
    | TAVERNKEEP_VERSION_PATCH)

//the most the device asks of the host once configured. the charger draws from the same connection, so
//this must cover its charging current once the charger is chosen (docs/architecture.md, "Battery and
//charging"); 500 mA is the most a USB 2.0 port provides
#define USB_MAX_POWER_MA 500

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL,
};

enum {
    EPNUM_MSC_OUT = 0x01,
    EPNUM_MSC_IN = 0x81,
    //full-speed bulk endpoints carry at most 64 bytes a packet
    MSC_EP_SIZE = 64,
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MSC,
    STRID_COUNT,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    //the class is given per interface
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, USB_MAX_POWER_MA),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC, EPNUM_MSC_OUT, EPNUM_MSC_IN, MSC_EP_SIZE),
};

//the strings, ASCII; the serial number is the flash chip's unique ID, filled in at init. a mass-storage
//device must have one (USB Mass Storage Class Bulk-Only Transport, section 4.1.1), and a host uses it to
//tell two devices of the same kind apart
static char serial[2U * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1U];
static const char *const strings[STRID_COUNT] = {
    [STRID_MANUFACTURER] = "Tavernkeep",
    [STRID_PRODUCT] = "Tome",
    [STRID_SERIAL] = serial,
    [STRID_MSC] = "Tome storage",
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device_descriptor;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration_descriptor;
}

//a string descriptor: a 2-byte header, then UTF-16LE characters. it has to outlive the control transfer,
//so it is static; TinyUSB sends one at a time
static uint16_t string_descriptor[1 + 32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t length = 0;
    if (index == STRID_LANGID) {
        string_descriptor[1] = 0x0409;      //English (United States), the only language offered
        length = 1;
    } else if (index < STRID_COUNT) {
        const char *const text = strings[index];
        const size_t limit = sizeof string_descriptor / sizeof string_descriptor[0] - 1U;
        while (length < limit && text[length] != '\0') {
            string_descriptor[1U + length] = (uint8_t)text[length];
            length++;
        }
    } else {
        return NULL;
    }
    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * length + 2U));
    return string_descriptor;
}

/* ------------------------------------------------------ mass storage */

static bool transfer_sense_pending;
static msc_disk_sense_t transfer_sense;

//a command other than a transfer failed: TinyUSB's sense is now this one, and replaces any kept
static void fail_with(uint8_t lun, const msc_disk_sense_t *sense)
{
    transfer_sense_pending = false;
    (void)tud_msc_set_sense(lun, sense->key, sense->code, sense->qualifier);
}

//a transfer failed: TinyUSB will overwrite its sense, so keep the real one for the REQUEST SENSE
static int32_t fail_transfer(const msc_disk_sense_t *sense)
{
    transfer_sense = *sense;
    transfer_sense_pending = true;
    return -1;
}

//copy text into a space-padded SCSI field, without a terminator; TinyUSB has filled the field with spaces
static void copy_field(uint8_t *field, size_t size, const char *text)
{
    const size_t length = strlen(text);
    memcpy(field, text, length < size ? length : size);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    char revision[8];
    (void)snprintf(revision, sizeof revision, "%u.%u", (unsigned)TAVERNKEEP_VERSION_MAJOR,
        (unsigned)TAVERNKEEP_VERSION_MINOR);
    copy_field(vendor_id, 8, "Tome");
    copy_field(product_id, 16, "SD card");
    copy_field(product_rev, 4, revision);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    msc_disk_sense_t sense;
    if (msc_disk_test_unit_ready(&sense)) {
        return true;
    }
    fail_with(lun, &sense);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    msc_disk_sense_t sense;
    if (!msc_disk_capacity(block_count, block_size, &sense)) {
        //TinyUSB fails the command on a zero count and keeps the sense set here
        fail_with(lun, &sense);
    }
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    msc_disk_sense_t sense;
    const int32_t done = msc_disk_read(lba, offset, buffer, bufsize, &sense);
    return done < 0 ? fail_transfer(&sense) : done;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    msc_disk_sense_t sense;
    const int32_t done = msc_disk_write(lba, offset, buffer, bufsize, &sense);
    return done < 0 ? fail_transfer(&sense) : done;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return msc_disk_writable();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    msc_disk_start_stop(start, load_eject);
    return true;
}

//the socket has no lock, and a removal is handled the same way whether or not the host asked to prevent
//it, so the request is accepted and changes nothing
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control)
{
    (void)lun;
    (void)prohibit_removal;
    (void)control;
    return true;
}

int32_t tud_msc_request_sense_cb(uint8_t lun, void *buffer, uint16_t bufsize)
{
    (void)lun;
    //TinyUSB has filled the buffer with its fixed-format sense
    scsi_sense_fixed_resp_t *const response = buffer;
    if (transfer_sense_pending && bufsize >= sizeof *response) {
        response->sense_key = (uint8_t)(transfer_sense.key & 0x0FU);
        response->add_sense_code = transfer_sense.code;
        response->add_sense_qualifier = transfer_sense.qualifier;
    }
    transfer_sense_pending = false;
    return (int32_t)sizeof *response;
}

//the SCSI commands TinyUSB does not handle itself. SYNCHRONIZE CACHE succeeds with nothing to do: every
//write is on the card before it completes. anything else is refused as ILLEGAL REQUEST, INVALID COMMAND
//OPERATION CODE
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    enum { SYNCHRONIZE_CACHE_10 = 0x35 };   //SBC-3; TinyUSB names no constant for it
    if (scsi_cmd[0] == SYNCHRONIZE_CACHE_10) {
        return 0;
    }
    const msc_disk_sense_t sense = { .key = 0x05, .code = 0x20, .qualifier = 0x00 };
    fail_with(lun, &sense);
    return -1;
}

/* ------------------------------------------------------------ interface */

static bool initialized;
//tusb_init connects to the bus
static bool attached = true;

bool usb_device_init(void)
{
    pico_get_unique_board_id_string(serial, sizeof serial);
    initialized = tusb_init();
    return initialized;
}

void usb_device_attach(bool attach)
{
    if (!initialized || attach == attached) {
        return;
    }
    attached = attach ? tud_connect() : !tud_disconnect();
}

bool usb_device_configured(void)
{
    return initialized && tud_mounted();
}

void usb_device_task(void)
{
    if (initialized) {
        tud_task();
    }
}

bool usb_device_host_present(void)
{
    return initialized && attached && tud_connected();
}
