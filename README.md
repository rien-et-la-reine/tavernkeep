# Tavernkeep RP2350 firmware

Tavernkeep is a learning and portfolio firmware project for an eventual custom
RP2350-based device. This repository currently contains a bring-up scaffold, with a
functional SPI SD card driver.


## Status and roadmap

Current status: bring-up firmware plus an SPI SD driver, with CRC checking. The
firmware initializes Pico SDK stdio, prints a startup banner, runs a hardware
demo/test suite, and then blinks the Pico 2 status LED when the selected board
defines one. The SD backend implements card detection with debounce, 
SDSC/SDHC/SDXC initialization, CSD capacity parsing, 
single- and multiple-block reads and writes with bounded timeouts, 
latched hot-removal handling, and CRC checking for reads/writes and commands.
The filesystem entry points still return not-implemented. The driver has
been hardware validated for an 8GB SDHC card at 1MHz.

Planned work, none of which is implemented yet:

1. FatFs integration through a disk I/O adapter
2. Native 4-bit SD using PIO on the custom hardware
3. USB mass-storage support and explicit media ownership
4. E-paper display driver and update scheduling
5. Encoder/button input and event handling
6. I2S audio transfers using DMA
7. MP3 and audiobook streaming/playback
8. Streaming EPUB/ZIP/XHTML parsing and layout
9. Power-management policy and low-power states
