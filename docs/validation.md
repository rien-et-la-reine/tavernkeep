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
  inside a multi-block write; removal during mount or bring-up (injected in
  VAL-H04); repeated interruption of the same save point; the format path
  (planned in VAL-H04); any rate above 1 MHz (run in VAL-H05);
  standard-capacity cards; write protect. Procedure note: when a session
  appears to stall after a breakpoint is armed, check the debugger's Call Stack
  pane, which shows whether the core is paused on a breakpoint

### VAL-H04 — Card-detect claim at configure, removal during bring-up and mount, block-device sync, slot-file save points, and the format path, 2026-10-20

- Test method: the VAL-H03 demo with the changes since: card detect claimed
  at configure, a block-device `sync` behind `CTRL_SYNC`, FatFs's open-entry
  table (`FF_FS_LOCK` 5), any mount failure with the device gone treated as a
  removal, and the slot-file save point (`save_point_write`). Automated over
  the Debug Probe, with removals injected by setting the driver's removal
  latch at a hardware breakpoint, the card left seated: at CMD8 and the first
  ACMD41 of init, the mount's first `disk_read`, three save-point stages
  (opening the older slot, its header write, its `CTRL_SYNC`), and inside the
  bulk file's multi-block write. Also a boot with 3 s idle, ten reboots, and
  the stack high-water mark. The format path in a `TAVERNKEEP_DEMO_FORMAT`
  build, the card made to look unformatted by clearing the boot signature in
  the mount's sector-0 buffer, so the format itself was real: once clean, once
  with a removal at the first save on the new volume, once with a removal at
  the format's 200th write
- Environment / hardware: as VAL-H03, with OpenOCD and RTT driven by script
  instead of Cortex-Debug; Release build of b413dad, and for the format path
  b413dad with the two fixes below
- Expected result: init interrupted logs "card not seated", not counted, and
  the card returns with no IO_ERROR; a mount interrupted ends ABSENT, never
  FAULTY; no IN_USE; stack near the 1.7 KB call-graph estimate; a save point
  interrupted at any stage fails with STORAGE_ERROR and leaves the old value
  or the new one, never a third; the format leaves an empty, unlabelled
  FAT32 volume; a removal during it never ends FAULTY
- Observed result:
  - Init and mount interrupted: as expected, each back to READY with a clean
    pass. 37 bring-ups with no counted failure or WARN; 32 clean passes; ten
    reboots, the counter up by one each.
  - Stack: 1604 of 4096 bytes, in the normal and the format build alike.
  - Save point interrupted: STORAGE_ERROR each time; the old record kept at
    the open and the header write, the new one already on the card at the
    sync. Never a third value. The first save on a new volume, interrupted at
    its sync, likewise kept the new record.
  - Bulk write interrupted: STORAGE_ERROR, the save point before it intact.
  - Format: about 26 s at 1 MHz; an empty volume with no label, then a clean
    pass from counter 1. `fsck.fat -n` on the host: no errors; FAT32, 32 KiB
    clusters, two FATs, data area at absolute sector 8192 (4 MiB). Removal
    during the format: UNFORMATTED, formatted again, a clean pass.
  - Two findings, fixed before commit. A format interrupted at its 200th
    write left a volume that mounted READY with the old files listed over a
    FAT already cleared: `f_mkfs` writes the MBR last. `filesystem_format` now
    clears sector 0 first, so that write commits the format. And the format
    made one FAT with the data area unaligned (sector 1991): the SD driver now
    reports the 4 MiB boundary unit of SDHC and SDXC cards for `f_mkfs` to
    align to, and the format asks for two FATs, as the SD file system
    specification does. Bulk write times did not change; a 16 KiB write stays
    in one allocation unit either way. The format path was later removed from
    the firmware altogether: formatting is left to a computer.
  - Not a failure: after the save-point and bulk-write removals the log went
    from READY to PREPARING with the epoch advanced, never ABSENT, since the
    seated card read present at once, as in VAL-H03
- Covered: all of the above on an 8 GB SDHC card, with injected removals
- Not covered here: boot without a card then insertion, and physical
  removal at each point above and while idle, done by hand in VAL-H07. Not
  planned: DMA; removal during a multi-block read; the none owner's release;
  SDSC cards; write protect

### VAL-H05 — SPI data rates of 5, 12.5 and 25 MHz, 2026-10-20

All three divide the RP2350's 150 MHz clk_peri exactly. 25 MHz is the SD
default-speed maximum in SPI mode; 12.5 MHz the fallback if the first board
has no margin there; 5 MHz the conservative one.

- Test method: the VAL-H04 build at each rate, lowest first: three boots of
  the whole demo, then a removal injected inside the bulk write at 25 MHz;
  later, every automated VAL-H04 case except the format at each rate
- Environment / hardware: as VAL-H04, same card and breadboard; b413dad
- Expected result: every step PASS at the requested rate; at 25 MHz either
  the same or reported failures, never wrong data; throughput rising with the
  clock but not in proportion
- Observed result: every case passed at every rate, each reporting exactly
  the rate asked for, with no CRC error at 25 MHz on the breadboard, and the
  same results as VAL-H04 at 1 MHz. Throughput is under Measurements: above
  about 5-6.5 MHz the CPU, not SCK, sets the pace
- Covered: 5, 12.5 and 25 MHz on this card and breadboard, with VAL-H04's
  injected removals at each
- Not covered, **pending**: physical removal and reinsertion above 1 MHz
  (VAL-H07 ran at 1 MHz); a bus capture at 25 MHz. Not planned: rates above
  25 MHz (CMD6 high-speed mode); DMA; other cards; the final board; power

### VAL-H06 — Pull-request run: unformatted cards, failed closes, unreadable save-point slots, bring-up retries, and VAL-H04 and VAL-H05 again, 2026-10-21

- Test method: the firmware as submitted: VAL-H04's build with the changes
  since. The format path is removed: a card with no FAT volume stays
  UNFORMATTED, and the demo asks for it to be formatted as FAT32 on a
  computer. A failed close is retried once before it reports. A save-point
  slot the card cannot read is treated as holding no record, so a read falls
  back to the other slot and the next save rewrites it. After VAL-H07's
  finding, bring-up is attempted nine times over about 3.5 s instead of three
  over 0.5 s, and the SD driver logs which init step failed. Every automated
  case of VAL-H04 and VAL-H05 was rerun, the VAL-H04 cases at 1, 5, 12.5 and
  25 MHz, with four new ones over the Debug Probe: the card made to look
  unformatted (boot signature cleared in the mount's sector-0 buffer, the
  card untouched); a write error at the first device write of a save point's
  close (`disk_write` made to return an error from its entry breakpoint); and
  slot a's record sector made unreadable, every device read of it failing
  until it is written, as a bad sector, starting from the newest record in
  each slot in turn; and SD init made to fail at CMD8 (`sd_spi_command`
  returning IO_ERROR), on all attempts but the last and then on every one
- Environment / hardware: as VAL-H05; built from the commit that adds this
  record
- Expected result: as VAL-H04 and VAL-H05 for their cases. The unformatted
  card reported with the message and left alone, never retried or FAULTY,
  and found unchanged on the next boot. The save whose close write failed
  passing, its record read back on the next boot. With slot a unreadable, no
  step failing: the read returning slot b's record when it is the newest, and
  falling back to it, the previous record, when slot a held the newest; the
  save rewriting slot a. Each failed bring-up naming its init step, the
  attempts following the schedule, the card coming up on the last attempt
  and parked FAULTY when every one fails
- Observed result:
  - 57 of 57 cases passed: 14 at each rate and the rate sweep. 179
    bring-ups and 162 clean demo passes, with no IN_USE. The only counted
    bring-up failures and WARN lines were the 68 injected CMD8 failures, the
    only FAULTY the four injected parks, and the only FAIL lines the 17
    injected removals, each STORAGE_ERROR.
  - Bring-up retries, at every rate: each failure logged "sd: init failed at
    CMD8, result 4" and a counted attempt of 9; the attempts came about 400,
    400, 700, 700, 700, 1200, 400 and 400 ms apart, the schedule plus about
    150 to 200 ms for each attempt, mostly the test's own breakpoints; with
    eight failing the card came up on the ninth, and with all nine it was
    parked FAULTY.
  - Unformatted: UNFORMATTED and the message, no state change in 3 s idle;
    the next boot mounted the volume with the counter one higher.
  - Failed close write: the save passed and read back, and the next boot
    read the record, at every rate.
  - Unreadable slot a, at every rate: with the newest record in slot b the
    counter advanced; with it in slot a the read fell back and the counter
    repeated. Each time the save rewrote slot a's sector, which then read
    normally.
  - Stack: 1628 of 4096 bytes, 24 more than VAL-H04, as the save-point read
    now checks its slots in a frame of its own. Reads as fast as in VAL-H05.
    Bulk writes were as fast at best but varied more, the slowest pass at
    each rate slower than in VAL-H05: 60.0 ms at 5 MHz (54.4), 48.1 ms at
    12.5 MHz (37.3), 43.4 ms at 25 MHz (32.2), each on an ordinary boot with
    nothing injected. Measured afterwards (Measurements, "Bulk write: time
    waiting on the card"): the variation is all the card's programming time;
    the firmware's own share of a write is constant.
  - Two findings, fixed before this run. A close whose retry also failed
    reported the retry's error, so a removal there showed as STALE_HANDLE;
    it now reports the first, and the save-point removal at the sync gave
    STORAGE_ERROR. And a first full run measured 1860 bytes of stack: the
    slot check had been inlined beside `f_open`, the deepest path; it is now
    kept out of line
- Covered: every case above at 1, 5, 12.5 and 25 MHz on the 8 GB SDHC card,
  with removals and errors injected
- Not covered here: physical removal and insertion (VAL-H07). Not planned: a
  real bad sector or transient write error (both injected at the FatFs
  adapter's entry, not produced by the card); DMA; SDSC cards; write protect

### VAL-H07 — Physical removal and insertion, 2026-10-21

- Test method: the firmware at 1 MHz, with a person at the bench pulling and
  inserting the card when the test run asked (the hardware tests' `manual`
  group). Ten removal and insertion cycles while idle, the last insertion
  slow; a boot with the socket empty, then an insertion; the card pulled
  while the core was halted at a breakpoint, at CMD8 of SD init, at the
  mount's first sector read, at each of VAL-H04's three save-point stages,
  and inside the first data block of the bulk file's multi-block write (the
  card selected, mid-CMD25), the run resuming once the card-detect pin, read
  over the probe, showed the socket empty; a card pushed partly in, left,
  then pushed home; and last, the card checked on a computer with
  `fsck.fat -n`. Run three times: twice on the VAL-H06 firmware before its
  bring-up change, then on the firmware as submitted
- Environment / hardware: as VAL-H06
- Expected result: each removal one ABSENT, never FAULTY, not counted; each
  insertion settling to READY with a clean pass; the cardless boot ABSENT at
  epoch 0 with no bring-up; at the halted points the outcomes of the injected
  cases in VAL-H04 (init: "card not seated"; save point and bulk write:
  STORAGE_ERROR), the counter only ever the old or the new value; the
  fumbled insertion reaching READY; no structural error on the host
- Observed result, final run:
  - Every case passed but the host check, which the test run could not read
    (a rig fault: the output was pasted as one line); read afterwards, it
    passes. 21 ABSENT transitions and no FAULTY; no WARN beyond the fumbled
    insertion's; the only FAIL lines were the four expected
    STORAGE_ERRORs. Every halted removal was seen from the card-detect pin.
  - Idle: ten removals, the epoch advancing to 10; every insertion, the slow
    one included, reached READY with a clean pass and the counter one higher.
  - Cardless boot: ABSENT at epoch 0, no state change in 3 s, then READY on
    insertion: the card-detect claim at configure works.
  - Halted removals: init gave "card not seated"; mount, ABSENT with no
    counted failure; the save point kept the previous record at the open and
    the header write and the new one at the sync; the bulk write failed with
    STORAGE_ERROR, the save point before it intact. Every reinsertion read
    cleanly.
  - Fumbled insertion: the first attempt failed at CMD0 ("sd: init failed at
    CMD0, result 4"), the card not yet answering, and the next reached READY.
    The card was pushed home quickly enough that three attempts would have
    done; the full schedule is shown by VAL-H06's injected case.
  - Host check: no structural error; 3 files in 4 clusters, exactly the
    card's contents; the FSInfo free cluster count 25 low, as predicted.
- Findings, from the earlier runs:
  - The FSInfo free cluster count drifts. FatFs keeps it in RAM from mount
    and writes it back at each close; opening a file to replace it frees its
    cluster on the card at once (directory entry first, then the FAT, so the
    volume stays consistent), so a removal between that truncation and the
    close leaves the count one cluster low. A measurement build logging the
    stored count against one from the FAT at every mount, with each automated
    case run alone, found it at that point and no other. The count was 38 low
    after the first run and, once repaired, 14 and then 25 low after later
    ones, each exactly the number of such removals in between. Left as it is:
    the firmware never reads the count, a host repairs it, and keeping it
    exact would cost a sector write on every save.
  - A card put back partly in and pushed home after a pause (the breakout,
    held only by its header pins, moves under the push) went FAULTY: the
    detect switch closed and stayed closed through the 500 ms settle wait
    while the contacts were not seated, and all three bring-up attempts, then
    250 ms apart, failed with IO_ERROR. Bring-up now makes nine attempts over
    about 3.5 s, and the driver logs the failing init step (VAL-H06)
- Covered: physical removal while idle and at each of VAL-H04's points,
  including a power loss mid-CMD25; insertion, slow and fumbled insertion,
  and a cardless boot; the volume afterwards on a host
- Not covered: removal during a multi-block read; physical removal above
  1 MHz; other cards; the final board

### VAL-H08 — USB mass-storage device with nothing lent, and the storage cases again, 2026-10-21

- Test method: the firmware with the USB mass-storage device (TinyUSB, one
  mass-storage interface over `msc_disk`) at 1 MHz, the Pico 2's USB port
  connected to the bench computer (Linux), so a host configured the device on
  every boot and the main loop no longer idled between passes. The hardware
  tests' `core`, `removal` and `faults` groups, then the device as the host
  saw it (`lsusb`, `lsblk`, the kernel log). Nothing lends the card to USB
  yet, so the demo kept it throughout
- Environment / hardware: as VAL-H06, with the Pico 2's USB port on the bench
  computer
- Expected result: every case as in VAL-H06, the loop's change and the USB
  stack running alongside making no difference; the host finding a removable
  disk with no medium in it
- Observed result: all 14 cases passed, with the same results as VAL-H06:
  bulk write 98-99 KiB/s and read 106 KiB/s, the deepest stack use 1628 of
  4096 bytes, and the bring-up retries on their schedule. The host found the
  device as `cafe:4002`, "Tavernkeep Tome", with the inquiry "Tome SD card
  0.1", attached it as a removable SCSI disk, and reported its size as 0
  bytes: no medium
- Covered: enumeration and the no-medium answer on Linux; the storage cases
  with a host on the bus
- Not covered, **pending**: the card lent to USB (reads, writes, eject,
  removal while lent), which needs the USB-mode policy (VAL-H09); other host
  operating systems; the stack use of a USB transfer, which nothing triggers
  yet

### VAL-H09 — The card lent to a USB host and returned, 2026-10-21

- Test method: the firmware with the USB-mode policy at 1 MHz, the Pico 2's
  USB port on the bench computer (Linux, GNOME, udisks), and the hardware
  tests' `usb` group. Entry and leaving asked for through the policy's
  request variable, written over the probe; on the host, the medium watched
  in `/sys/block`, the volume mounted, written and ejected through udisks,
  as the desktop does. A removal injected while lent, by setting the
  driver's latch. Before the group existed, the same round trip done by hand
  (`udisksctl`, a file written, the drive's Eject method)
- Environment / hardware: as VAL-H08
- Expected result: the card lent only once asked for and with a host
  configured; the host finding the medium, and its writes landing on the
  card; an eject or a request returning the card, the device staying on the
  bus, the next local pass clean and seeing the host's file; a removal while
  lent being a new medium for the USB owner, not a return
- Observed result: all three cases passed:
  - Round trip: the host found the medium (15523840 sectors, the card's size)
    within a second of the request and read its partition table; udev took
    about 30 s to identify the volume over USB at 1 MHz (about 150 reads),
    and udisks then mounted it, showing the demo's own files. A file written
    and synced by the host was listed by the demo at its size after the eject
    ("usb: returning the card: the host ejected it"), the counter one higher,
    and the host left with no medium and the device still on the bus.
    GNOME did not automount the volume
  - Leave by request: returned ("requested") with a clean pass, and the host
    saw no medium
  - Removal while lent: the loss at the next epoch for the USB owner, the
    seated card brought up for USB again at that epoch and found again by the
    host, which had a read fail with an I/O error at the removal; no return
    until asked
  - The `core`, `removal` and `faults` groups on the same firmware: all 14
    passed, as in VAL-H08
- Findings:
  - A host configures the device about 0.25 s after reset, but the host can
    take longer to attach it again after the previous boot's device, so the
    first run of the removal case asked for entry, the policy waited (as it
    should), and the case timed out. The policy now logs a host configuring
    the device, and the cases wait for that line
  - An injected removal leaves the card powered. When the host was reading it
    at that moment, the card was left mid multi-block read, the first CMD0 of
    the next bring-up went unanswered, and the retry 250 ms later succeeded.
    The same followed a reset with the card lent and the host reading. A
    physical removal takes the card's power, so it cannot happen there; the
    removal case allows and reports one such retry
- Covered: entry by request, the host's reads and writes, an eject, leaving
  by request, a removal while lent, on Linux at 1 MHz
- Not covered, **pending**: a cable removal (the bench Pico 2 is powered
  through the cable; host-tested only); the host leaving while lent (bus
  reset or unconfigure; host-tested only); other host operating systems;
  higher SPI rates in USB mode; the stack use of a USB transfer

## Measurements / Performance Characterization

<!--
Record measured timing, throughput, memory, power, signal, or other quantitative
results. Include method, equipment, configuration, uncertainty or limitations,
and expected limits where applicable.
-->

### Bulk transfer throughput by SPI rate, 2026-10-20

The demo's 16 KiB bulk file, written and read in one call each, timed around
the call alone (VAL-H04, VAL-H05). On the host-formatted volume (4 KiB
clusters, 8-block transfers) and on one made by the firmware's format path in
VAL-H04, since removed (32 KiB, one 32-block transfer). Reads vary under 1%;
writes up to 12% on the firmware's volume, aligned or not, so with the card's
own write latency.

| SPI rate | SCK ceiling | Write, 4 KiB / 32 KiB clusters | Read, 4 KiB / 32 KiB clusters |
| --- | --- | --- | --- |
| 1 MHz | 122 KiB/s | 98 / 98-99 KiB/s | 102 / 106 KiB/s |
| 5 MHz | 610 KiB/s | 307 / 293-328 KiB/s | 339 / 361-364 KiB/s |
| 12.5 MHz | 1526 KiB/s | 451 / 428-490 KiB/s | 515-521 / 570 KiB/s |
| 25 MHz | 3052 KiB/s | 535 / 496-593 KiB/s | 634 / 703 KiB/s |

Beyond 8 SCK periods per byte, each byte costs about 1.2 µs read and 1.5 µs
written, so 25 MHz is only about 23% faster than 12.5 MHz.

### Bulk write: time waiting on the card, 2026-10-21

To find where VAL-H06's bulk-write variation came from: a measurement build
(not committed) timed every call to the SD driver's wait for the card's busy
to end, totalled over the demo's timed 16 KiB write, on 30 boots at each rate
with nothing injected, on the VAL-H04 firmware-formatted volume. Every write
made the same 37 waits (one per block of the 32-block CMD25, and those around
the command and the FAT and directory sectors).

| SPI rate | Write, min / median / max | Not waiting | Waiting on the card |
| --- | --- | --- | --- |
| 1 MHz | 159.8 / 162.0 / 166.4 ms | 154.3 ms | 5.4-12.0 ms |
| 5 MHz | 50.0 / 52.1 / 56.2 ms | 45.0 ms | 5.0-11.2 ms |
| 12.5 MHz | 32.6 / 33.1 / 47.7 ms | 28.6 ms | 4.0-19.1 ms |
| 25 MHz | 28.0 / 28.6 / 32.7 ms | 23.2-23.5 ms | 4.9-9.5 ms |

The time not spent waiting, sending data and computing CRCs, is the same on
every pass to within 0.4 ms. All the variation is in the waits: a slow write
has one or two long ones (the slowest, 47.7 ms, waited 19.1 ms in all, 12.6 ms
of it in one wait), which is the card taking longer to program some blocks.

## Not Yet Demonstrated on Hardware

- All runs so far are on one 8 GB SDHC card: no bus capture, no power
  measurement, no removal during a multi-block read, no SDSC card, no second
  card.
- Physical removal, power loss mid-write included, has run only at 1 MHz
  (VAL-H07); above that, removals have been injected with the card seated.
- A removal while a file is being replaced leaves the FSInfo free cluster
  count one cluster low (VAL-H07), until a host repairs it.
- The storage coordinator's release and reclaim (the none owner) has run only
  in the host tests; nothing requests a release on hardware until the power
  manager exists.
- USB mode: a cable removal and a host leaving while lent have run only in
  the host tests (VAL-H09), and only on Linux.
- DMA, PIO and power management are not implemented, so nothing to test.
