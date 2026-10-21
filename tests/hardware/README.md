# Hardware tests

The hardware runs behind the validation records (VAL-H04 to VAL-H07 in `docs/validation.md`):
automated cases that need no hands on the card, and a `manual` group that asks for physical removal and
insertion. They drive a Pico 2 through the Raspberry Pi Debug Probe with OpenOCD: flash, reset, hardware
breakpoints, memory reads and writes, and RTT for the demo's log.

## Setup

- The Pico 2 wired to the SD breakout as in VAL-H03 (SPI0: DO 16, CS 17, CLK 18, DI 19; detect on
  GPIO 12), the Debug Probe on SWD, and the usual 8 GB card, with a FAT volume, in the socket.
- The Pico SDK extension's tools under `~/.pico-sdk` (toolchain 15_2_Rel1, OpenOCD 0.12.0+dev, ninja).
- Nothing else uses the probe: close any VS Code debug session first.

The demo writes to the card as it always does (the counter's slot files and the bulk file). Nothing
here formats it: the firmware has no format.

## Running

```bash
python3 tests/hardware/run.py --offline
```

Checks everything that needs no board: it builds, confirms every breakpoint target exists out of line,
resolves the removal latch, confirms init's CMD8 and ACMD41 calls carry the command in `r1`, checks the
log lines the cases match still exist in the sources, and tests the log parser.

```bash
python3 tests/hardware/run.py
```

Runs the `core`, `removal` and `faults` groups. Name cases or groups to run fewer; `--list` shows them all. The
`rates` group rebuilds and reflashes, so it only runs when asked for:

```bash
python3 tests/hardware/run.py rates
```

`--rate` builds and runs the other cases at another SPI rate, in a build directory of its own:

```bash
python3 tests/hardware/run.py --rate 25000000
```

The `usb` group needs the Pico 2's USB port connected to the computer running the tests, which acts as
the USB host; it runs only when asked for:

```bash
python3 tests/hardware/run.py usb
```

It lends the card through the USB-mode policy's request variable (`usb_mode_request`, written over the
probe) and works the host side through `/sys/block` and udisks (`usb_host.py`), so it needs no root.

The `manual` group needs you at the bench, for physical removal and insertion. It runs only when
asked for:

```bash
python3 tests/hardware/run.py manual                   # all of them, about 10 minutes of your time
python3 tests/hardware/run.py manual_idle --cycles 2   # one case, shorter
```

When it needs you, it stops, rings the terminal bell and says what to do (`>>> pull the card out`),
then carries on by itself once it sees it done: from the firmware's log while the core runs, or, while
the core is halted at a breakpoint, from the card-detect pin read over the probe (IO_BANK0 GPIO12
status). Pressing Enter also carries on. Each case starts by reading the pin with the card in, so it
recognises an empty socket whatever the switch's sense. `manual_host_check` runs last and asks you
to check the card on your computer with `fsck.fat -n` (without `-v`) and paste the output.

Results go to `tests/hardware/results/<time>/`: a log per case (every RTT line with its time since reset,
and the rig's notes, such as where a removal was injected), `openocd.log`, and `summary.txt` /
`summary.json` with the commit. A case is PASS, FAIL (the firmware did not do what was expected) or
ERROR (the rig failed: no verdict).

## Cases

| Case | What it checks |
| --- | --- |
| `normal` | A boot with the card in: every demo step passes at the rate asked for, no IN_USE, no warnings, never FAULTY, and nothing changes once idle |
| `soak` | `--boots` reboots (default 10): no counted bring-up failure, every pass clean, counter up by exactly one each boot |
| `stack` | Fills the unused stack at `main`, runs a full pass, and reports the deepest use and the margin; fails only if the stack grew past 4 KB |
| `removal_init_cmd8`, `removal_init_acmd41` | A removal during SD init: "card not seated", not a counted failure, then brought up again |
| `removal_mount` | A removal at the mount's first sector read: ABSENT, never FAULTY, not counted, then mounted again |
| `removal_save_open`, `_header`, `_sync` | A removal at each stage of a save point: the save fails with STORAGE_ERROR, then the counter is the old value or the new one, never a third |
| `removal_bulk_write` | A removal inside the bulk file's multi-block write: STORAGE_ERROR, the save point before it intact, the next pass clean |
| `unformatted` | A card made to look unformatted (the boot signature cleared in the mount's buffer; the card is untouched) is reported UNFORMATTED with the message to format it on a computer, and left alone; the next boot finds it as it was |
| `close_retry` | A write error injected at the first write of a save point's close (`disk_write` made to return an error once): the close's retry absorbs it, the save passes, and the next boot reads the record back |
| `unreadable_slot` | Slot a's record sector made unreadable (every device read of it fails until something writes over it, as a bad sector does), starting from the newest record in each slot in turn: no step fails; with the newest in slot b the read gets it, with it in slot a the read falls back to slot b's previous record and the counter repeats; the save rewrites slot a |
| `bring_up_retries` | SD init made to fail at CMD8 (`sd_spi_command` returning IO_ERROR): each failure logs its step and is a counted attempt, the attempts follow `STORAGE_BRING_UP_DELAYS_MS` (read from `storage.h`), a card failing all but the last comes up, and one failing them all is parked FAULTY |
| `manual_idle` | You pull and reinsert the card `--cycles` times (the last insertion slowly): one ABSENT per removal, every insertion READY with no counted failure, a clean pass each time |
| `manual_cardless_boot` | A boot with the socket empty: ABSENT at epoch 0, no bring-up; your insertion reaches READY |
| `manual_removal_init`, `_mount`, `_save_open`, `_save_header`, `_save_sync` | The core halts at the same points as the injected cases and you pull the card: the same outcomes, with ABSENT, then your reinsertion reads cleanly |
| `manual_removal_bulk_write` | The core halts mid data block of the bulk file's multi-block write and you pull the card: STORAGE_ERROR, ABSENT, the save point before it intact |
| `manual_fumbled_insertion` | You push the card partly in, pause, then push it home: it reaches READY, never FAULTY, and the case reports how many attempts failed first and at which init step |
| `manual_host_check` | You check the card on your computer with `fsck.fat -n`, paste its output, and put the card back: any structural error fails; a stale FSInfo free cluster count (a hint FatFs keeps from mount to sync, not structure) is reported as a note |
| `usb_round_trip` | The card lent on request; the host mounts it, writes a file and ejects it; the card comes back with a clean pass whose listing has the host's file at its size, and the host is left with no medium, the device still on the bus |
| `usb_leave_by_request` | The card lent, then asked back through the request variable: a clean pass, and no medium on the host |
| `usb_removal_while_lent` | A removal injected while lent: lost for the USB owner at the next epoch, brought up for USB again at that epoch and found again by the host, with no return to the local owner; one CMD0 retry is allowed and reported (VAL-H09) |
| `rates` | Each of `--rates` (default 5, 12.5, 25 MHz) for `--rate-boots` boots, the bus at exactly that rate, with throughput; then a bulk-write removal at the highest passing rate |

## What injected removals do and do not show

A removal is injected by setting the driver's removal latch (`sd.removal_latched`) at a breakpoint,
exactly as the card-detect interrupt does. The card stays seated and powered. That tests the firmware's
side: which error is reported, the coordinator's transition, the recovery, and what a save point leaves
readable. It does not show what an unpowered card does with a write in progress; the `manual` group
does, with the card really pulled.

## When a case breaks for the wrong reason

- **A demo log line was reworded:** the cases match a few anchored patterns (`hil.py`, log formats).
  `--offline` names any that no longer exist in the sources.
- **A function's signature changed:** the removal cases' conditions read arguments from `r0`–`r3` at
  function entry (`filesystem_open`'s mode in `r1`, `filesystem_write`'s count in `r2`, `disk_ioctl`'s
  command in `r1`, `disk_write`'s count in `r3`). `--offline` refuses a function the compiler cloned.
- **Lines missing from a log:** RTT here drops a whole line rather than block when its 1 KB buffer is
  full. The rig polls every 10 ms, so this should not happen; a "steps never reported" failure with no
  FAIL line suggests it did.
