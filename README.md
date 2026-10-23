# Tavernkeep RP2350 firmware

Tavernkeep is a learning and portfolio firmware project for an eventual custom
RP2350-based device. It currently contains bring-up firmware with a working
removable-storage stack.

## Status and roadmap

The firmware initializes Pico SDK stdio, prints a startup banner, runs the
storage coordinator in a cooperative loop with a filesystem demo reported over
RTT, and blinks the Pico 2 status LED when the selected board defines one.

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
  and insertion, settles a new card before bringing it up, retries and parks
  failing cards, and carries out owner changes: handing the card to a future
  USB mass-storage class, or releasing it so the peripherals can be powered
  down.

Validated on hardware with one 8 GB SDHC card (`docs/validation.md`, VAL-H01
to VAL-H07): mount, file operations and multi-block transfers at up to 25 MHz,
booting without a card, and hot removal while idle, during writes and at each
save-point stage (physical removal at 1 MHz, injected removal at every rate).
The firmware does not format cards: one without a FAT volume is reported, for
the user to format as FAT32 on a computer.

Planned work, none of which is implemented yet:

1. Native 4-bit SD using PIO on the custom hardware
2. USB mass-storage class, entered automatically when a host configures the
   device (the storage ownership model it plugs into exists)
3. E-paper display driver and update scheduling
4. Encoder/button input and event handling
5. I2S audio transfers using DMA
6. MP3 and audiobook streaming/playback
7. Streaming EPUB/ZIP/XHTML parsing and layout
8. Power-management policy and low-power states
