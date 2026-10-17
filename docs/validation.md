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

### VAL-H02 — Filesystem layer, storage coordinator and hot removal on a real card, 2026-10-17

- Test method: filesystem demo in `src/main.c` driven by the storage
  coordinator (`src/storage/storage.c`), reporting each step and every
  coordinator state change over RTT. On each mount the demo reads the volume
  label, lists `/`, reads a counter file, writes counter + 1 through the save
  point (write `tavernkeep-demo.tmp`, confirm its close, remove
  `tavernkeep-demo.txt`, rename into place) and reads it back. Two runs: one
  from a card without the counter file, then a reboot followed by manual
  removal and reinsertion cycles
- Environment / hardware: Raspberry Pi Pico 2 (RP2350, Arm), Pico SDK 2.3.0,
  Release build of the uncommitted working tree on 70e2863, flashed and
  observed through the Debug Probe with OpenOCD and Cortex-Debug; Adafruit
  MicroSD card breakout board+ on a breadboard, rewired since VAL-H01 to SPI0
  on GPIO 16-19 (DO 16, CS 17, CLK 18, DI 19), availability signal on GPIO 12
  (active-high switch); RP2350 internal pull-ups on DO and CS; 400 kHz
  bring-up then 1 MHz; the 8 GB microSDHC card from VAL-H01, holding a FAT32
  volume labelled `SCROLL` (FAT32 with 4 KiB clusters, from the host's
  `fsck.fat` after VAL-H03)
- Expected result: first mount READY at epoch 0 with every demo step PASS
  and the counter at 1; after reboot the counter read back from the card;
  each removal reported as one ABSENT transition with the epoch advanced and
  no FAIL or dropped-handle warning; each reinsertion brought up and mounted
  fresh, with the demo passing and the counter advancing by one
- Observed result: run 1: READY at epoch 0, every step PASS, counter 1; the
  root listed only `System Volume Information/`. Run 2: READY at epoch 0, the
  counter file listed at 4 bytes and read back as 1, written as 2. Three
  removals, each reported once as ABSENT with the epoch advancing 1, 2, 3, and
  no FAIL lines or warnings. Four mounts in run 2 in all, every demo step PASS
  on each, counter reaching 5. One finding: no reinsertion went straight to
  READY. Each passed through PREPARING, meaning at least one counted bring-up
  failure before a retry 250 ms later succeeded. The first reinsertion,
  deliberately about as slow as a real-world insertion gets, failed all three
  attempts and went FAULTY, then ABSENT when the socket next read empty, then
  READY on a fresh bring-up. The likely cause is that the detect switch closes
  before the card's contacts have made a solid connection, and the
  coordinator's three attempts at 250 ms spacing are too short a window for a
  slow insertion; the bring-up timing is to be loosened. The failing result
  codes were not logged, so the cause is inferred, not observed
- Covered: FatFs mount through the disk I/O adapter on a real card; volume
  label; directory listing; file create, write and close; open and read;
  stat; remove; rename; data persisting across a reboot; hot removal while
  idle, three times, with the no-I/O unmount and removal teardown; remount
  after removal with fresh handles; the coordinator's retry and FAULTY paths
- Not covered: booting with no card; removal during a transfer; multi-block
  transfers under FatFs (every demo access is a single sector, so CMD18 and
  CMD25 were not exercised by this run); the save-point recovery paths; the
  format path; any rate above 1 MHz; standard-capacity cards; write protect

### VAL-H03 — Boot without a card, insertion settle, and removal during transfers and save points, 2026-10-17

- Test method: the VAL-H02 demo with three additions made after that run: an
  insertion settle wait in the coordinator (`STORAGE_INSERT_SETTLE_MS`,
  500 ms of continuous presence before the first bring-up attempt), a log line
  with the result code for every counted bring-up failure, and a final demo
  step writing a 16 KiB file in one call and reading it back in one call,
  every word checked against a pattern unique to its offset and the run. Five
  boots in all; removals made by hand, either while idle or while the core was
  halted at debugger breakpoints, each followed by Continue (never Step, which
  masks interrupts). Breakpoints were function breakpoints, exact in a Release
  build: `disk_write` with `$r3 > 1` (a multi-block write; only the bulk file
  issues one), `filesystem_write` with `$r2 == 4` (the save point's counter
  write), `filesystem_remove` and `filesystem_rename` (between save-point
  steps), and `crc_helper_rolling_16` with a hit count, armed only after one of
  the others had stopped, which halts inside a data block with the card
  selected. Afterwards the card was checked on the host with `fsck.fat -n`
- Environment / hardware: as VAL-H02 - Raspberry Pi Pico 2 (RP2350, Arm), Pico
  SDK 2.3.0, Release build of the uncommitted working tree on 70e2863 with the
  additions above, Debug Probe with OpenOCD and Cortex-Debug, RTT; Adafruit
  MicroSD card breakout board+ on SPI0 (DO 16, CS 17, CLK 18, DI 19),
  availability signal on GPIO 12 (active-high); internal pull-ups on DO and
  CS; 400 kHz bring-up then 1 MHz; the 8 GB microSDHC card from VAL-H01
  (FAT32, 4 KiB clusters, label `SCROLL`)
- Expected result: a boot without a card reports ABSENT, and an insertion
  then settles and mounts; insertions, slow ones included, reach READY with no
  counted bring-up failure; every removal during a transfer or a save point
  fails the operation in progress with an error, with no hang, and is followed
  by ABSENT; every remount succeeds; the counter is only ever the value before
  the interrupted save point or the one after it; the save-point recovery
  takes the documented path for each interruption point; the volume is
  consistent afterwards
- Observed result:
  - Boot without a card: ABSENT at epoch 0; on insertion PREPARING (the
    settle wait), then READY; every step PASS including the bulk write, read
    and 16 KiB verify, the first multi-block transfers under FatFs. With 4 KiB
    clusters the 16 KiB file moves as 8-block CMD25 and CMD18 transfers.
  - Slow insertion: the socket read empty three times during the settle wait,
    restarting it each time (PREPARING and ABSENT alternating), then READY with
    no counted bring-up failure. The VAL-H02 finding is resolved: no WARN line
    appeared on any of the 8 bring-ups: 4 boots with the card in and 4
    insertions (one after the cardless boot, three after removals).
  - Removal during the bulk write, after `disk_write` with `$r3 > 1` had
    stopped and `crc_helper_rolling_16` had been armed: `FAIL bulk write:
    result 11` (STORAGE_ERROR), then ABSENT. The B3 stop was not seen in the
    debugger before the card was pulled, so the exact point in the transfer is
    not confirmed; the write was still unfinished when the card left, since at
    1 MHz it completes in about 0.2 s and no later step ran. On the next boot
    the bulk file listed at 0 bytes: truncated by the open, never extended.
  - Removal mid single-block write, halted inside a data block during the
    save point's write: `FAIL save point write: result 11`, then ABSENT. On the
    next boot no temporary file existed (the create had not reached the card)
    and the counter was unchanged.
  - Removal at `filesystem_remove` (temporary written, old file present):
    `FAIL save point remove: result 11`, then ABSENT. On remount both files
    listed at 4 bytes; on the next boot recovery reported "discarded an
    incomplete save point" and the counter read the older value.
  - Removal at `filesystem_rename` (old file removed, temporary not yet
    renamed): `FAIL save point rename: result 11`, then ABSENT. On remount only
    the temporary listed; recovery reported "completed an interrupted save
    point", the counter read the newer value, and the following save point
    and bulk step all passed.
  - Counter across the run: 8, unchanged by the interrupted bulk write and the
    interrupted save-point write; the interrupted remove left 8 and 9 on the
    card, recovery kept 8; the interrupted rename left only 9, recovery
    renamed it into place; the next save point wrote 10. Never a third value.
  - Host check: `fsck.fat -n` reported no errors, 6 files and 9 of 1935360
    clusters in use - exactly the card's contents, so no cluster was left
    allocated by the interrupted writes. Its only remark was a volume label
    present in the root directory but not in the boot sector, which predates
    these tests (nothing in the firmware writes a label).
  - Two observations that are not failures. After one idle removal the log
    went from READY straight to PREPARING with the epoch advanced: the latch
    was handled and the socket bounced back to present within the same step,
    so the settle wait started and absorbed the bounce, and the intermediate
    ABSENT was never visible to the demo, which samples state between steps.
    And one remount's demo stopped after the listing because the
    `filesystem_remove` breakpoint was still enabled and caught the recovery's
    own remove; the next boot ran that recovery to completion
- Covered: boot without a card; the insertion settle wait, including restarts
  on a slow insertion; bring-up with no counted failures across 8 bring-ups;
  multi-block writes and reads under FatFs (8-block CMD25 and CMD18) with
  every word verified; removal during a multi-block write (point within the
  transfer unconfirmed); removal inside a single-block write's data block;
  removal at both interruption points of the save point and both recovery
  paths; remount after each; volume consistency afterwards on the host
- Not covered: removal during a multi-block read; removal at a confirmed point
  inside a multi-block write; removal during mount or bring-up; repeated
  interruption of the same save point; the format path; any rate above 1 MHz;
  standard-capacity cards; write protect. Procedure note: when a session
  appears to stall after a breakpoint is armed, check the debugger's Call Stack
  pane, which shows whether the core is paused on a breakpoint

## Measurements / Performance Characterization

<!--
Record measured timing, throughput, memory, power, signal, or other quantitative
results. Include method, equipment, configuration, uncertainty or limitations,
and expected limits where applicable.
-->

## Not Yet Demonstrated on Hardware

- Three real-card runs exist, all at 1 MHz on one 8 GB SDHC card: the driver
  alone (VAL-H01); the filesystem layer with the storage coordinator, including
  hot removal while idle (VAL-H02); and boot without a card, the insertion
  settle wait, multi-block transfers under FatFs, removal during writes and
  save points, and both save-point recovery paths (VAL-H03). No bus capture,
  no power measurement, no rate above 1 MHz, no removal during a multi-block
  read, no standard-capacity card, no second card.
- The format path is implemented but has not run on hardware.
- DMA, PIO, USB mass storage and power management are not implemented, so
  nothing to test.
