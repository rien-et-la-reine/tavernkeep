# Validation

This document records behavior that has been demonstrated on target hardware.
Its purpose is to keep the existence of code distinct from demonstrated
correctness: nothing is claimed here until it has been observed on a real
device under stated conditions.

Each record names the hardware, the configuration and the card used, what was
expected, what was observed, and what the run does not cover. A record is
scoped to its stated conditions; it says nothing about rates, cards or
sequences it did not exercise.

### Validation record format

<!--
### VAL-HNN — Feature or behavior, date

- Test method:
- Environment / hardware:
- Expected result:
- Observed result:
- Covered:
- Not covered:
-->

## Hardware Validation

### VAL-H01 — SD driver demo on a real card, 2026-09-15

- Test method: demo in `src/main.c`, reporting each step over RTT
- Environment / hardware: Raspberry Pi Pico 2 (RP2350, Arm), Pico SDK 2.3.0,
  flashed and observed through the Debug Probe with OpenOCD and Cortex-Debug;
  Adafruit MicroSD card breakout board+ on a breadboard, SPI1 on GPIO 11-14,
  card detect on GPIO 10 (active-high switch); RP2350 internal pull-ups on DO
  and CS as stand-ins for the final board's resistors; 400 kHz bring-up then
  1 MHz; an 8 GB microSDHC card
- Expected result: configure, init, get_info, three argument rejections with
  no bus traffic, block 0 read with `55 aa` at bytes 510-511, a single-block
  write and readback 64 blocks from the end, a 50-block write in five CMD25
  transfers with ACMD23, readback of all 51 blocks against a per-block
  LBA-tagged pattern, an untouched neighbour block on each side, deinit, then
  the heartbeat
- Observed result: 15 of 15 steps PASS; `15523840 blocks of 512 bytes`
  (7.95 GB); `55 aa` present. Two findings on the way, both resolved before
  the passing run: the breakout's detect switch is active-high, so the
  driver's card-detect sense had to be made configurable; and a floating DO
  before CMD0 fails init with BUSY_TIMEOUT rather than anything pointing at
  the line (pull-up added; a more specific diagnostic is still an open
  decision)
- Covered: bring-up with CRC checking on, CSD CRC against a real register,
  CRC-checked reads, CRC-checked single and multiple-block writes, capacity,
  argument rejection, card presence in the active-high sense, deinit
- Not covered: any rate above 1 MHz, hot removal, standard-capacity cards,
  write protect

## Measurements / Performance Characterization

<!--
Record measured timing, throughput, memory, power, signal, or other quantitative
results. Include method, equipment, configuration, uncertainty or limitations,
and expected limits where applicable.
-->

## Not Yet Demonstrated on Hardware

- One real-card run exists (VAL-H01), at 1 MHz on one SDHC card. No bus
  capture, no power measurement, no rate above 1 MHz, no hot removal on a real
  socket, no standard-capacity card.
- FatFs, DMA, PIO, USB mass storage and power management are not
  implemented, so nothing to test.
