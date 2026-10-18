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
  inside a multi-block write; removal during mount or bring-up (planned in
  VAL-H04*); repeated interruption of the same save point; the format path
  (planned in VAL-H04*); any rate above 1 MHz (planned in VAL-H05*);
  standard-capacity cards; write protect. Procedure note: when a session
  appears to stall after a breakpoint is armed, check the debugger's Call Stack
  pane, which shows whether the core is paused on a breakpoint

### VAL-H04* — Card-detect claim at configure, removal during bring-up and mount, block-device sync, slot-file save points, and the format path (planned, not yet run)

The `*` marks a planned record: nothing below has been observed yet. The
asterisk comes off, and the date goes in, once the run has been made and the
observed result filled in.

- Test method: the VAL-H03 demo, built from the commit that contains the
  changes made after that run: the card-detect pin is claimed in
  `sd_spi_configure` instead of at the first presence check or
  initialization; the block-device contract gains `sync`, which the SD driver
  implements as a ready check on the selected card; the FatFs adapter
  forwards `CTRL_SYNC` to it, so every close, remove and rename now asks the
  device to confirm its writes; FatFs's table of open entries is on
  (`FF_FS_LOCK` 5), so every open, remove and rename checks it; the storage
  coordinator gains the none owner (release and reclaim), which the demo
  never requests; and the save point is now `save_point_write`, two
  alternating slot files with a sequence number and a CRC, replacing the
  temporary-file-and-rename scheme VAL-H03 tested. The demo's counter
  therefore lives in `tavernkeep-demo.a` and `tavernkeep-demo.b`: it starts
  again from 1 on this card, and the old `tavernkeep-demo.txt` stays behind,
  unused. Removals are made by hand while the core is halted at function
  breakpoints, each followed by Continue (never Step), and the card is checked
  on the host with `fsck.fat -n` afterwards. Planned steps:
  - Claim at configure: boot without a card, then insert one; boot with a card
    in. `is_present` no longer claims the pin, so an insertion seen at all
    shows the claim in configure took effect.
  - Removal during SD init: halt in `sd_spi_command` with `$r1 == 8` (CMD8)
    or `$r1 == 41` (inside the ACMD41 loop), pull the card, Continue; then
    reinsert. `sd_spi_command` is static, so first confirm the breakpoint
    resolves in the Release build.
  - Removal during mount: once init has succeeded, halt at the first
    `disk_read` of the mount, pull the card, Continue; then reinsert.
  - Sync on every close and the lock table: a normal run, idle removals and
    reinsertions included, with every demo step passing.
  - Stack high-water mark, in the normal build and again in the format build
    below: halt at `main`, fill the unused part of core 0's stack with a
    pattern from the debugger (take the bounds from the linker map), run the
    whole session, then halt and find the lowest overwritten word. The
    call-graph estimate is about 1.7 KB, deepest in the demo's save-point read
    through `f_open`, and about 0.8 KB for the format (`docs/architecture.md`,
    "Stacks"). Alternatively build with the SDK's stack guard
    (`PICO_USE_STACK_GUARDS`) so that an overflow faults.
  - Save point interrupted at each stage: halt at `save_point_write`, then arm
    one of these and pull the card when it stops, Continue, and reinsert. Use
    a separate interruption for each, with at least two saves on the card first
    so both slots hold a record:
    - `filesystem_open` with `$r1 == 1` (FILESYSTEM_OPEN_REPLACE): just before
      the older slot is truncated.
    - `filesystem_write` with `$r2 == 12` (the record's header): the slot has
      been truncated and nothing new is in it yet.
    - `disk_ioctl` with `$r1 == 0` (`CTRL_SYNC`): the record is written and
      its close is confirming it.
  - First save interrupted: after the format step below, the demo's first
    save on the empty volume, interrupted at `disk_ioctl` with `$r1 == 0`.
  - Format, run last because it erases the card: on the same 8 GB card,
    first image it on the host (`dd`) so its contents can be restored, then
    reformat it there as exFAT, which leaves it with no FAT volume as an SDXC
    card arrives. Run a separate build with `TAVERNKEEP_DEMO_FORMAT` set to 1
    in `main.c`, measuring the stack as above. Afterwards check the card on
    the host. Then reformat it as exFAT again,
    halt in `disk_write` while `f_mkfs` is running, pull the card, Continue,
    and reinsert. Restore the image at the end if the old contents are wanted.
- Environment / hardware: as VAL-H03 - Raspberry Pi Pico 2 (RP2350, Arm), Pico
  SDK 2.3.0, Release build, Debug Probe with OpenOCD and Cortex-Debug, RTT;
  Adafruit MicroSD card breakout board+ on SPI0 (DO 16, CS 17, CLK 18, DI 19),
  availability signal on GPIO 12 (active-high); internal pull-ups on DO and
  CS; 400 kHz bring-up then 1 MHz; the 8 GB microSDHC card from VAL-H01. Record
  the exact commit built, not a working tree
- Expected result:
  - Claim at configure: the cardless boot reports ABSENT, and the insertion
    settles and reaches READY; the boot with a card reaches READY.
  - Removal during SD init: init fails through its rollback and the
    coordinator logs "card not seated", not a counted failure, then ABSENT. On
    reinsertion the card is detected and reaches READY with no
    `block device result 4` (IO_ERROR) line, which shows the rollback kept the
    card-detect pin claimed and unregistered the removal interrupt, so the next
    registration succeeded.
  - Removal during mount: the mount fails, the coordinator treats it as a
    removal (ABSENT, never FAULTY), and the reinserted card reaches READY.
  - Sync on every close and the lock table: every demo step PASS, as in
    VAL-H03; neither the extra ready check nor the lock table causes a
    failure, and no `result 13` (IN_USE) appears, since the demo never holds
    two handles on one file.
  - Stack: in each build a high-water mark near the call-graph estimate plus
    interrupt frames, recorded with its margin below 4 KB; the stack guard, if
    used, does not fire.
  - Save point interrupted at each stage: `FAIL save point write: result 11`
    (STORAGE_ERROR), then ABSENT, including at the sync, where the record's
    sectors may already be on the card. After reinsertion the counter is the
    value before the save or the one after it, never a third, and the next
    save passes. On the host the slot that was not being written still holds
    the older record, untouched. `fsck.fat -n` reports no errors, except
    possibly one lost cluster chain: an interruption between clearing the
    truncated slot's directory entry and freeing its clusters leaves them
    allocated but unreferenced, which wastes space and nothing else.
  - First save interrupted: the next mount logs "no saved counter yet", not a
    partial value, and the following save writes counter 1.
  - Format: the card reports UNFORMATTED, the demo requests the format, and
    the card reaches READY with an empty volume; the demo then passes every
    step with the counter at 1. On the host the card holds an MBR partition
    table (no `FM_SFD`) and a FAT32 volume with no label, which
    `fsck.fat -n` reports clean. A removal during the format ends in ABSENT,
    never FAULTY, and the reinserted card is either UNFORMATTED or READY,
    whatever the interrupted format left
- Observed result: not yet run
- Covered: nothing yet
- Not covered by the plan: DMA transfers, whose `sync` behaviour is the reason
  for the contract change but which do not exist yet; a write that fails after
  it was accepted (needs asynchronous writes to happen at all); removal during
  a multi-block read; formatting an SDXC card or a card small enough for FatFs
  to pick FAT12 or FAT16, since the card's size decides the FAT type; release
  and reclaim for the none owner, which nothing on the board requests yet and
  for which there is no rail to cut; any rate above 1 MHz (planned in
  VAL-H05*); standard-capacity cards; write protect

### VAL-H05* — SPI data rates of 5, 12.5 and 25 MHz (planned, not yet run)

Planned, like VAL-H04*: nothing below has been observed yet.

Why these three. The SPI peripheral is clocked from clk_peri, which the SDK
runs from clk_sys at 150 MHz on the RP2350, and `spi_set_baudrate` divides it
by an even prescale times a post-divide. All three rates divide 150 MHz
exactly (2 × 15, 2 × 6 and 2 × 3), so the card sees the rate that was asked
for. 25 MHz is the ceiling: it is the SD default-speed maximum in SPI mode and
the most `sd_spi_configure` accepts. 12.5 MHz is the likely working rate for
the final firmware if 25 MHz has no margin on the first board; its raw rate of
about 1.5 MB/s is several times uncompressed CD-quality audio (176 KB/s),
the heaviest stream the device is expected to read. 5 MHz is the conservative
fallback, five times the rate validated so far and still ample for compressed
audio and the reader. The 400 kHz bring-up rate is unchanged throughout.

- Test method: the demo from the latest validated build, with `SD_BAUD_HZ` in
  `main.c` set to each rate in turn, lowest first; move up only once a rate
  passes. The demo logs the rate actually set each time the card becomes
  ready (`sd bus at ... Hz`, from the value `spi_set_baudrate` returns), and
  the time and rate of the bulk write and the bulk read call (`bulk write:
  ... KiB/s`), timing the transfer call alone, not the open or the close. At
  each rate: a boot with the card in, then at least ten
  removal and reinsertion cycles, each running the whole demo including the
  16 KiB bulk write, read and verify. At the highest rate that passes, one
  removal during the bulk write, as in VAL-H03, to check the removal path
  still works when a transfer takes a fraction of the time. Finish with
  `fsck.fat -n` on the host. If a logic analyser or scope is available,
  capture SCK and DO at 25 MHz and record the edges under Measurements below
- Environment / hardware: as VAL-H04*, the same 8 GB microSDHC card. The
  breadboard matters more here than in any earlier run: jumper lengths and
  the breakout's 74AHC125 level shifter on DI, CLK and CS all affect signal
  quality at 25 MHz. Record the wiring as it is, and the exact commit built
- Expected result: every demo step PASS at 5 and 12.5 MHz, with the reported
  rate equal to the one requested. At 25 MHz either the same, or failures
  that are reported, never silent: with CRC on every command and data block,
  and every word of the bulk file verified, a marginal bus shows up as
  `IO_ERROR` results and FAIL lines, not as wrong data. No hang at any rate.
  Throughput rises with the clock, but not in proportion: the driver moves one
  byte per `spi_write_read_blocking` call and computes the CRC a byte at a
  time in software, so above some rate the CPU, not SCK, sets the pace. The
  run records where that happens. A failure at 25 MHz on the breadboard is a
  property of the wiring, to be retested on the first board, not a firmware
  result. The removal during the bulk write ends as in VAL-H03: the write
  fails with STORAGE_ERROR, then ABSENT
- Observed result: not yet run
- Covered: nothing yet
- Not covered by the plan: rates above 25 MHz (SD high-speed mode, switched
  with CMD6, which the driver does not implement); DMA, which will change the
  throughput figures entirely; any card other than the 8 GB SDHC card; signal
  integrity on the final board; power at each rate

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
- The card-detect claim at configure and the block-device `sync` behind
  `CTRL_SYNC` came after VAL-H03 and have only run in the host tests; VAL-H04*
  plans their first hardware run, together with removal during bring-up and
  mount.
- The save point was redesigned after VAL-H03 (two slot files with a sequence
  number and a CRC, `src/storage/save_point.h`), because the
  temporary-file-and-rename scheme VAL-H03 exercised turned out to have two
  unsafe interruption points that it did not reach. The new scheme has run only
  in the host tests, including a removal at every device write of a save;
  VAL-H04* plans its hardware run.
- FatFs's table of open entries (`FF_FS_LOCK`) and the storage coordinator's
  release and reclaim (the none owner) also came after VAL-H03 and have run
  only in the host tests. VAL-H04*'s normal run exercises the table; nothing
  requests a release on hardware until the power manager exists.
- The format path is implemented but has not run on hardware, and no stack
  measurement has been made on hardware; VAL-H04* plans both.
- Every run so far has used 1 MHz. VAL-H05* plans 5, 12.5 and 25 MHz.
- DMA, PIO, USB mass storage and power management are not implemented, so
  nothing to test.
