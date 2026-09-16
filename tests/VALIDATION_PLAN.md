# Repository assessment and validation plan

Reviewed 2026-09-04, revised 2026-09-05 after the test-infrastructure overhaul
and 2026-09-09 to add the card recovery policy. Covers production sources, build
configuration, README, `docs/requirements.md`, `docs/architecture.md`, `docs/validation.md`, and tests.
No future product subsystem is implemented by this suite.

This document is forward-looking: it records what evidence each planned feature
will owe. For what the current harness can and cannot represent see
[HARNESS.md](HARNESS.md) and [RESIDUAL_RISK.md](RESIDUAL_RISK.md); for what has
actually been demonstrated see [VALIDATION_RESULTS.md](VALIDATION_RESULTS.md).

## Current implementation and evidence

Source determines implementation status; the root README predates current
storage work. Requirements/architecture describe intentions, not proof.

| Area | Implementation | Host evidence |
| --- | --- | --- |
| Boot/system | Board, logging and IRQ initialization; cooperative heartbeat | Real loop for 1.1 seconds of fake time, including initialization failures; separate board/logging compile-time variants |
| GPIO dispatcher | One SDK callback on the owning core, per-pin handlers and masks | Validation, wrong-core operations, two GPIO users, callback filtering/self-removal and nested critical sections; real SD integration |
| Block-device API | Validation and backend dispatch | Every wrapper and missing callback; context/buffer/LBA/count forwarding including 64-bit extremes; all eight result categories |
| SD SPI | Debounced availability in either switch sense; rollback; legacy/v2 initialization; CSD v1/v2 capacity; single/multiple 512-byte reads; bounded waits; removal latch and cleanup | Four suites against a stateful card model: card-variant matrix, command ordering, CRC7 and application-command enforcement, the R1 poll boundary, all 128 R1 values, all 15 error tokens with immediacy bounds, multiple-block reads of arbitrary length, addressing and capacity boundaries, CSD registers from real cards, phase-based fault injection with recovery assertions, and seeded property and fuzz tests |
| SD info/writes | Public info from the cached CSD; CMD24/CMD25 writes with byte addressing, CRC16, data-response decoding, programming busy, stop-tran and CMD12 recovery | `sd_writes_host_tests` against the card model: data read back out of the model on every card kind, token and timing-gap rules, rejections and unknown bytes, busy bounds and `BUSY_TIMEOUT`, removal at every phase, a no-false-success fault sweep. One real-card run at 1 MHz: CMD24 and five CMD25 transfers accepted with CRC checking on and read back byte-for-byte (VALIDATION_RESULTS.md, 2026-09-15); no bus capture yet |
| Filesystem | Prepare/bind state; mount/unmount stubs | Validation, state preservation, rebinding, repeat stub calls and no backend invocation |
| Other product subsystems | Planned | Acceptance matrix below; no passing feature placeholders |

Data is compared with expected bytes only after successful reads. After failure
the entire requested destination is unspecified, so the tests assert how much of
it was populated rather than what it contains. Outside-buffer canaries remain
valid checks even on interrupted transfers.

Removal coverage is both byte-based and phase-based: every SPI byte of a
single- and two-block read and of successful SDHC initialization, plus an
injected ejection at each named transaction phase including the final release
clock. Each verifies bus release, immediate rejection of new operations, no
CMD12 to an absent card, no teardown inside the interrupt handler, interrupt
suppression, exactly-once hardware release, and that reinsertion requires a
fresh initialization. Discrete injection is not exhaustive testing of
machine-instruction races or of multicore execution.

The one still-failing disabled regression in [KNOWN_GAPS.md](KNOWN_GAPS.md) is failed
contract evidence and must accompany any report of a passing enabled suite.
Read data CRC validation is implemented and host-tested for CMD17/CMD18 and
for the CSD register. Bring-up, CRC-checked reads and single and
multiple-block writes were exercised once on a real 8 GB SDHC card at 1 MHz
(VALIDATION_RESULTS.md, 2026-09-15). The FR-009 row below still lacks the
higher-rate captures, hot-removal and SDSC evidence.

## Future acceptance matrix

Add executable tests with each real public API as it is implemented. Use small,
deterministic fixtures under `tests/fixtures`, recording provenance; avoid full
books/music or large disk images. Do not implement mock product subsystems just
to make this matrix executable.

| Requirement / subsystem | Future host tests and observable outcomes | Integration / hardware acceptance |
| --- | --- | --- |
| FR-009 storage completion | Public info matches CSD and 512-byte sectors; CMD24/CMD25 framing/addressing; accepted/rejected data-response tokens; busy bounds, stop token and cleanup; known CRC vectors and corruption rejection | Disposable media; first/last-sector independent readback hashes; capture initialization and writes at configured SPI rates |
| FR-009 FatFs/diskio | Real adapter over test memory blocks; error/status mapping, sector size/capacity, multi-sector forwarding, range checks and `CTRL_SYNC`; mount/read/write/unmount/reopen a small known filesystem; corrupt/truncated media and injected I/O errors | Host-created files readable by firmware and vice versa; no false mounts/stale handles; define write completion before sync acceptance |
| FR-009/012 removal coordinator | Idle/init/wait/read/write removal; consumer cancellation, handle invalidation, discard whole failed read; exactly-once cleanup after unwinding; bounce still requires fresh init | Remove/write-protect during each phase; no blocking cleanup in ISR, no flush to absent media or CMD12 after cancellation; unrelated input/UI remains responsive |
| FR-009 PIO/4-bit and DMA | Reuse generic storage contracts; register traces for active/cancel/quiesce/completion; no late success; abort ordering and all chained channels disabled | Logic-analyzer traces; removal during chains/FIFO activity; canaries; no late memory writes or retriggering; RP2350-E5 procedure on actual SDK/silicon |
| FR-010 USB mass storage | Explicit local/USB ownership state machine; reject competing ownership; SCSI bounds/mapping; eject/disconnect/removal races; invalidate local cache/handles | Host copy/eject/reconnect on supported OSes; no simultaneous writable mounts; hashes intact after clean handoff |
| FR-001 EPUB / FR-002 text | Small Unicode/plain-text/ZIP/EPUB fixtures; line endings/empty files; malformed archives, unsupported compression/encryption, missing manifests, spine order; streaming bounds, malformed XHTML, ZIP expansion limits and deterministic navigation/layout | Representative local books with bounded memory; malformed content fails locally and returns to navigation |
| FR-003 reading state / FR-004 bookmarks | Versioned persistence round-trip, boundary positions, multiple books, create/delete; corrupt/truncated state and failure at every persistence stage | Restart/power-cycle after checkpoints; recover last committed state; removal cannot falsely commit |
| FR-005 MP3 / FR-006 controls | Decoder vectors/PCM hashes with documented rounding; malformed/truncated frames, tags/seeking; pause/resume/skip/volume transitions; buffer wrap, storage errors and underrun recovery | I2S to PCM5102A and amplifier behavior; continuous audio during storage/display/input load; underrun/seek measurements |
| FR-007 dual displays / CON-001 | Golden command/render fixtures for both panels; clipping/rotation, independent busy/error states, bounded BUSY waits and update coalescing | SSD2677/GDEH0576T81 reset/update sequencing, dual-panel output and current against selected hardware documentation |
| FR-008 input / OPEN-001 | After controls are selected: timestamped bounce, press/hold/release or quadrature traces; queue overflow/order; failures cannot steal other IRQs | Input-to-event latency during display/audio load; test selected controls without assuming dual encoders |
| FR-011 navigation | Empty/corrupt/large directories, selection bounds, removed files, book/audio transitions and unsupported-content recovery | Browse/select entirely local content; media replacement while menus are open |
| FR-012 system/power | Initialization failure matrix, cooperative progress, clean/forced shutdown order, wake events, cancellation before power-off and idempotent ownership | Repeated startup/shutdown/suspend/wake and interrupted shutdown with instrumented power rails |
| STRETCH-001 M4B | Only after scope acceptance: chapter/container fixtures, seek/resume, persistence and malformed input | Long audiobook seek/resume and playback; do not count stretch work as a required delivered feature |

FatFs low-level sector and cache completion expectations follow the official
[disk read](https://elm-chan.org/fsw/ff/doc/dread.html),
[disk ioctl](https://elm-chan.org/fsw/ff/doc/dioctl.html) and
[integration notes](https://elm-chan.org/fsw/ff/doc/appnote.html).
In particular, `CTRL_SYNC` must complete pending cached writes; tests must
reflect the eventual write-completion contract.

GPIO ownership and future DMA tests follow the
[Pico SDK hardware reference](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html):
ordinary GPIO callbacks are per core; RP2350-E5 requires clearing enable on the
aborted DMA channel and chained channels before abort. Host register-order
checks need physical validation too. For future protocol fixtures, pin an SD
Physical Layer revision from the
[SD Association archive](https://www.sdcard.org/downloads/pls/archives/)
and record the sections used for golden expectations.

### Note: the DMA sniffer computes CRC-16-CCITT in hardware

Relevant to FR-009 and to any decision about the shape of the software CRC.

RP2350's DMA has a sniffer that computes a checksum over data passing through a
sniffed channel, in hardware, concurrently with the transfer and at no CPU cost.
The pico-sdk exposes it as `dma_sniffer_enable(channel, mode, force)` with
`dma_sniffer_set_data_accumulator()` to seed it. Mode `0x2` is CRC-16-CCITT
(`0x3` is the bit-reversed variant; `0x0`/`0x1` are CRC-32, `0xe` XOR reduction,
`0xf` a 32-bit sum).

The consequence for design: **a DMA data path would not run the software CRC
alongside the transfer.** DMA does not compute a checksum as a side effect of
moving bytes - the sniffer is the thing that does, and it replaces
`crc_helper_16()` in the data path rather than being fed by it. The software
implementation keeps two roles in that future: the reference the sniffer is
validated against, and the fallback for any path that is not DMA.

So "DMA will run the CRC in parallel" is not an argument for a particular
software API shape. Software chunking would only be needed for a DMA design
that deliberately did *not* use the sniffer - half-buffer interrupts CRCing each
chunk on the CPU - which is strictly worse on this part.

**To verify before relying on it:** "CRC-16-CCITT" is an ambiguous name. The SD
data CRC is CRC-16/XMODEM - polynomial 0x1021, initial value 0x0000,
non-reflected, no final XOR. Mode `0x2` with the accumulator seeded to zero
should match, but that must be confirmed against known vectors on real silicon,
and the datasheet's transfer-width and byte-order rules for the sniffer checked.
`sd_crc16_host_tests` already pins the catalogue check value for "123456789",
which is exactly the vector to compare a sniffer result against.

## Cross-cutting requirements

| Requirement | Evidence to add |
| --- | --- |
| NFR-001 responsiveness | Input-to-action latency under supported concurrent load. Agree on a numerical limit first; fake time is not a latency measurement. |
| NFR-002 audio continuity | DMA/buffer underrun counts and audio capture during reading, display and input workloads; define duration/load. |
| NFR-003 integrity | Corruption fixtures, I/O fault/commit-boundary injection, independent hashes and disposable-media power-cut trials. |
| NFR-004 isolation | Book/audio/display/storage failures leave unrelated work progressing; current shared IRQ integration is partial evidence only. |
| NFR-005 resources | Host sanitizers where supported; target map, stack high-water marks, buffer bounds and allocation ceilings on worst supported content. Set budgets before acceptance. |
| NFR-006 power | Instrumented current/energy for defined active/idle/sleep workloads and unused peripheral inactivity. |
| NFR-007 offline | Future product flows with network absent. Today's no-download host suite alone does not prove future offline product functionality. |
| NFR-008 recovery | Fault/retry, removal/reinsertion and peripheral recovery without unnecessary full reset; ownership never leaked or released twice. The SD side of this is unsettled - see "Card recovery policy" below. |

## Card recovery policy

**Status: undecided, and deliberately deferred.** The intended order of work was
SD-006 (closed 2026-09-09), then SD-007 writes (implemented 2026-09-11), then
this. Writes now exist, so recovery is next in line; settle it before the SD
driver is called finished. The write path's "unknown data-response byte" exit,
which releases the card without terminating the transfer, is one of the
inputs to this policy.

It is recorded here rather than in [KNOWN_GAPS.md](KNOWN_GAPS.md) because no
regression can be written until the policy is chosen.

The driver today has an initialisation path and an initialisation *rollback*
path, and nothing in between. Any card that stops responding correctly after
bring-up produces an error to the caller and stays broken. The goal is a driver
that is reasonably robust against cards that are less than perfectly
specification-conforming, which is the common case in the field.

### The failure mode that motivates this

SD-004 describes a collision in which a card, having received CMD12 while it was
emitting a token, rejects every subsequent command until it is reset. This is
the sharp case because it is **unrecoverable through the command channel**: no
sequence of commands gets the card back, so error handling that only retries
commands will loop until it gives up.

It is not the only way to arrive there. A card wedged by a glitch, a brown-out
during programming, or a marginal card mid-write can land in the same place. The
recovery path is therefore worth having on its own merits, not only as SD-004
insurance. Choosing SD-004 option 1 makes this rarer; it does not make it
impossible.

### Detecting it

The signature is that the *next* command after the triggering operation fails:
either persistent 0xFF with bit 7 never clearing, which the driver currently
surfaces as `IO_ERROR` or `BUSY_TIMEOUT`, or an R1 with the illegal-command bit
set. Note that the failure is attributed to the operation *after* the one that
caused it, which makes it awkward to diagnose from logs alone.

Two open questions:

- Is a deliberate liveness check wanted after a multiple-block read? CMD13
  (`SEND_STATUS`) is the specification's sanctioned way to obtain the real
  outcome of an R1b command, and CMD12 is R1b. It costs one command per
  multi-block read. The driver does not use CMD13 anywhere today.
- How is "wedged" distinguished from "removed" and from "legitimately busy"?
  The card-detect GPIO and the existing removal latch already separate removal
  from the other two, which is a real advantage this driver has.

### The reset ladder

Escalating, cheapest first. Each rung needs a decision on how many attempts
before escalating, and the whole ladder needs a decision on what the caller
sees while it runs.

1. **Bus resynchronisation.** Deassert CS, clock idle bytes, retry. Adequate for
   a desynchronised byte stream, useless for a card in a rejecting state.
2. **Soft reset — full SPI re-initialisation.** CS deasserted, 74+ clocks with
   MOSI high, CMD0 to re-enter idle, then the normal bring-up: CMD8, CMD59,
   ACMD41, CMD58, CMD9, and CMD16 for byte-addressed cards. This is the
   documented escape for most bad states and reuses the bring-up code that
   already exists.
3. **Power cycle.** Required when CMD0 itself is refused, because nothing in the
   command channel can reach the card. **The board can support this** — the
   specific mechanism (a load switch or equivalent on the card supply, under
   GPIO control) needs confirming against the schematic and recording here,
   along with the off-time the card needs to fully discharge before power is
   reapplied. That off-time is a real constraint: too short and the card does
   not actually reset. After power returns, bring-up restarts from the 74-clock
   sequence.

### Design decisions this needs

- Retry counts at each rung, and whether the in-flight operation is retried
  transparently or the error is surfaced to the caller.
- Whether recovery is automatic or the caller must ask for it. Automatic
  recovery hides real hardware problems; manual recovery pushes protocol
  knowledge up into the filesystem layer.
- What state is invalidated by a reset. `card_type_legacy`, `card_type_hcxc`
  and `block_count` are re-derived by bring-up, but any cached position or
  outstanding operation is not.
- How recovery interacts with the removal/hot-plug machinery. A deliberate
  re-initialisation must not be mistaken for a removal event, and the card-detect
  IRQ and debounce state have to stay coherent across it. This is the part most
  likely to introduce a regression in behaviour that currently works.
- When to stop. A card that fails recovery repeatedly should be declared dead
  rather than retried forever, and the caller needs a way to see that.

### Structural prerequisite: split bring-up from resource acquisition

`sd_spi_device_init()` currently does two jobs in one function: it **acquires
resources** (GPIO configuration, SPI init, the debounced presence check, the
removal-latch clear, and the IRQ registration with its recheck-after-register
ordering) and then **brings the card up** (CMD0, CMD8, CMD59, ACMD41, CMD58,
CMD9, CMD16). Recovery needs only the second job.

There is a specific landmine in re-entering the first. Bring-up clears the
removal latch before registering the IRQ:

```c
//clear the latch
atomic_store_explicit(&sd->removal_latched, false, memory_order_relaxed);
```

If recovery calls the existing init, **it clears a latch that a genuine removal
may have just set**, and the driver proceeds believing a card is present that is
not. That is the same failure class as SD-001. Re-entering init would also
double-register the card-detect IRQ.

So the split is not tidiness, it is a correctness prerequisite:

- **acquire** — runs once, owns the IRQ registration and the presence-check /
  latch-clear / register / recheck ordering that closes the race between the
  presence check and registration;
- **bring up** — repeatable, pure protocol, assumes resources are already held
  and that latch ownership belongs to the caller.

Initialisation is then acquire + bring-up, recovery is bring-up alone, and
reinsertion is presence re-establishment + bring-up. Three callers converge on
one bring-up implementation instead of a second copy that drifts from the first.

Worth noting even if recovery is never built: reinsertion and initialisation
already share this sequence informally.

### Invariants to pin before writing the code

These are testable on the host today, in the style the suite already uses for
removal sweeps. Writing them first means the policy is fixed before the code is.

1. **Recovery never clears the removal latch.** The latch is cleared in exactly
   one place, and that place has just proven presence with a debounced check.
2. **Removal always wins.** A latch set before recovery starts means this is a
   removal, not a wedge, and goes to teardown. A latch set during any recovery
   phase abandons recovery. Assert this at every rung, including inside the
   power-cycle window.
3. **Teardown during recovery still releases hardware exactly once.** SD-002 is
   the existing regression for this property; recovery adds new paths into
   teardown and must not break it.
4. **Recovery cannot re-enter itself.** A bring-up failure inside recovery must
   not start another recovery.
5. **Recovery exhaustion is distinguishable from removal.** A caller must be
   able to tell "the card is gone" from "the card is present but will not
   respond", because the correct response differs.
6. **The card-detect IRQ is registered exactly once** across any sequence of
   recovery attempts, successful or not. This extends
   `sd_removal_repeated-teardown`.
7. **Derived state is re-derived, never stale.** After a successful recovery,
   `card_type_legacy`, `card_type_hcxc` and `block_count` reflect the card that
   is present now. A recovery that silently keeps values from before the reset
   is a correctness bug even when the same card is reinserted.
8. **No I/O is attempted while recovery is in progress**, and requests arriving
   during it are refused rather than interleaved onto the bus.

### Evidence this will owe

Host tests can cover the policy but not the phenomenon: the card model can be
told to reject all commands until reset, which exercises the ladder's logic,
the state invalidation and the interaction with removal. What it cannot
establish is whether real cards actually recover at each rung, or what off-time
a power cycle needs.

That part is hardware work: induce the wedge (SD-004's collision is the
reproducible route), then confirm at which rung each card in the test set comes
back, with bus captures. Until that exists, any retry count or off-time written
into the driver is a guess. See NFR-008 in the cross-cutting table.

## Hardware procedure (planned, not performed)

Record firmware revision and dirty-tree state, SDK/toolchain, board revision,
wiring/pull-ups, card model/capacity, instruments and raw traces. Capture
CS/SCK/MOSI/MISO/availability for initialization, single/multiple reads,
timeouts and removal. Verify idle clocks, command bytes, selection/release,
operating frequency and pin direction against the selected specifications.
Include v1 SDSC, v2 SDSC and SDHC/SDXC where available.

Compare reads against independent host data. Repeat with switch bounce,
removal at each phase, reinsertion and write protection. Observe that ISRs do
no blocking protocol cleanup and foreground cleanup waits for operations to
unwind. Fake byte boundaries cannot prove physical IRQ or SPI timing.

Write/corruption/power-cut trials require disposable media and a recorded
restore image once those paths exist. No physical hardware tests were performed
during this review.
