# Tavernkeep RP2350 firmware

Tavernkeep is a learning and portfolio firmware project for an eventual custom
RP2350-based device. It currently contains bring-up firmware with a working
removable-storage stack and USB mass storage.

## Status and roadmap

The firmware initializes Pico SDK stdio, prints a startup banner, runs the USB
device and the storage coordinator in a cooperative loop with a filesystem demo
reported over RTT, and blinks the Pico 2 status LED when the selected board defines one.

- **SD driver (SPI):** card detection with debounce, SDSC/SDHC/SDXC
  initialization, CSD capacity parsing, single- and multiple-block reads and
  writes with bounded timeouts, latched hot-removal handling, and CRC checking
  for commands and data.
- **Filesystem layer:** FatFs behind `src/storage/filesystem.h`, private to
  the layer, with pooled file and directory handles that a removal or unmount
  invalidates without any I/O.
- **Save points:** persistent state (bookmarks and, for now, settings) saved in two
  alternating slot files with a sequence number and a CRC
  (`src/storage/save_point.h`), so an interruption at any moment leaves the
  previous record readable.
- **Storage coordinator:** the single owner of the card. It notices removal
  and insertion, settles a new card before bringing it up, retries and parks
  failing cards, and carries out owner changes: handing the card to the USB
  mass-storage class, or releasing it so the peripherals can be powered
  down.
- **USB mass storage:** TinyUSB in device mode presents the card to a
  computer as a removable disk. The card is lent to the host on request and
  comes back when the host ejects it, the cable is removed (VBUS sensing) or
  another request asks for it. Entering USB mode automatically when a host
  configures the device is not implemented yet.

Validated on hardware with one 8 GB SDHC card (`docs/validation.md`, VAL-H01
to VAL-H07): mount, file operations and multi-block transfers at up to 25 MHz,
booting without a card, and hot removal while idle, during writes and at each
save-point stage (physical removal at 1 MHz, injected removal at every rate).
The USB mass-storage device and lending the card to a host were validated in
VAL-H08 and VAL-H09.
The firmware does not format cards: one without a FAT volume is reported, for
the user to format as FAT32 on a computer.

Planned work, none of which is implemented yet:

1. Native 4-bit SD using PIO on the custom hardware
2. E-paper display driver and update scheduling
3. Encoder/button input and event handling
4. I2S audio transfers using DMA
5. MP3 and audiobook streaming/playback
6. EPUB parsing and layout. The container and package layer is being written
   as Bartap (`lib/bartap/`), a library intended to become its own repository;
   its interfaces are drafts
7. Power-management policy and low-power states

## License

The firmware is released under the MIT License (`LICENSE`). Vendored FatFs
(`third_party/fatfs/`) keeps its own licence; see `third_party/README.md`. The
hardware lives in the separate `tome` repository under CERN-OHL-S-2.0.

`lib/` holds first-party libraries intended to become separate repositories,
such as Bartap (`lib/bartap/`, MIT, with its own `LICENSE`); they are distinct
from the vendored code in `third_party/`.
