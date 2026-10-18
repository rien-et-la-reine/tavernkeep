# Tavernkeep RP2350 firmware

Tavernkeep is a learning and portfolio firmware project for an eventual custom
RP2350-based device. This repository currently contains bring-up firmware with
a working removable-storage stack: an SPI SD card driver, a FatFs-based
filesystem layer, and a storage coordinator that owns the card.


## Status and roadmap

Current status: bring-up firmware plus the removable-storage stack. The
firmware initializes Pico SDK stdio, prints a startup banner, runs the storage
coordinator in a cooperative loop with a filesystem demo reported over RTT,
and blinks the Pico 2 status LED when the selected board defines one.

- **SD driver (SPI):** card detection with debounce, SDSC/SDHC/SDXC
  initialization, CSD capacity parsing, single- and multiple-block reads and
  writes with bounded timeouts, latched hot-removal handling, and CRC checking
  for commands and data.
- **Filesystem layer:** FatFs behind `src/storage/filesystem.h`, private to
  the layer, with pooled file and directory handles that a removal or unmount
  invalidates without any I/O.
- **Save points:** persistent state (bookmarks, settings) saved in two
  alternating slot files with a sequence number and a CRC
  (`src/storage/save_point.h`), so an interruption at any moment leaves the
  previous record readable.
- **Storage coordinator:** the single owner of the card. It notices removal
  and insertion, settles a newly inserted card before bringing it up, retries
  and parks failing cards, carries out format and owner-change requests, and
  is ready to hand the card to a USB mass-storage class or release it so that
  the peripherals can be powered down.

Hardware validated at 1 MHz on an 8 GB SDHC card (`docs/validation.md`,
VAL-H01 to VAL-H03): mount, file operations, multi-block transfers, hot
removal while idle and during writes, and booting without a card. The save
points have been redesigned since VAL-H03 tested the earlier
temporary-file-and-rename scheme, and have so far run only in the host tests,
as has formatting a card.

Planned work, none of which is implemented yet:

1. Native 4-bit SD using PIO on the custom hardware
2. USB mass-storage class (the storage ownership model it plugs into exists)
3. E-paper display driver and update scheduling
4. Encoder/button input and event handling
5. I2S audio transfers using DMA
6. MP3 and audiobook streaming/playback
7. Streaming EPUB/ZIP/XHTML parsing and layout
8. Power-management policy and low-power states
