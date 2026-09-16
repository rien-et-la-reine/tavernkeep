# Production contract gaps

Gaps that are still open, and the five that were fixed. Each open gap has a
registered regression that asserts the behaviour the driver *should* have and
therefore fails against the current source. They are disabled by default, so a
green default run is not evidence that they are resolved.

```sh
cmake -S tests -B tests/build -DTAVERNKEEP_TEST_KNOWN_GAPS=ON
cmake --build tests/build
ctest --test-dir tests/build -L known-gap --output-on-failure
```

Three of the four currently fail; `sd_gap_command-crc` passes now that SD-006 is
fixed and can be promoted out of the label when convenient.
`-DTAVERNKEEP_TEST_KNOWN_GAPS=OFF` restores the default; `ctest -L host` selects
the enabled coverage alone.

SD-007 was the acceptance test for the write path, written ahead of the
implementation so the target was fixed before the code was. The write path
exists now and its cases were promoted into the enabled suite
(`sd_writes_host_tests`); see "Fixed during this work". SD-006 was the same for
per-frame command CRC7.

## Order of work

1. ~~**SD-006** - CMD12's placeholder CRC.~~ Done 2026-09-09.
2. ~~**SD-007** — writes.~~ Implemented 2026-09-11 with one-shot
   `crc_helper_16()` over the contiguous source buffer, as planned; the
   resumable form below was not needed for it.
3. **Resumable CRC16, together with transfer pipelining.** See the note under
   "CRC16: interleaving versus pipelining" below for why these are one item and
   not two, and why neither belongs ahead of writes.
4. Everything else here, and the card recovery policy in
   [VALIDATION_PLAN.md](VALIDATION_PLAN.md).

The reason this is written down: writes are major functionality that has sat
unimplemented while several hardening and refinement passes went ahead of them.
SD-004 and the recovery policy are all hardening (SD-003 and SD-005 have since
been closed). Each is worth
doing and none of them is worth doing before the storage layer can write, so
they should stop displacing it. A driver that reads reliably and cannot write is
not further from finished than one that reads and writes imperfectly - it is
missing half the contract.

### CRC16: interleaving versus pipelining

An earlier revision put a resumable `crc_helper_16()` ahead of writes, on the
grounds that the API should be generalised before it acquires callers. That
argument does not hold and is recorded here so it is not made again: adding
`crc_helper_16_update(seed, ...)` later is **additive**, with the existing
one-shot becoming a wrapper. Nothing breaks by deferring it, so there is no
cost to doing it when there is evidence rather than in anticipation.

*Status:* the resumable form now exists as `crc_helper_rolling_16(crc, byte)`
and the read path folds each payload byte through it as it arrives (the
interleaved arrangement). The analysis below is unchanged: this buys no
latency on its own, and the pipelined transfer loop that would is still
future work.

**Interleaving on its own buys no latency.** `sd_spi_transfer()` calls
`spi_write_read_blocking()` for a single byte, which busy-waits on the FIFO.
Interleaved (`transfer; crc; transfer; crc; ...`) and batched (`transfer x512;
crc x512`) execute the same instructions serially and cost the same wall time.
Per byte the interleaved form is *spin, then CRC* - the CRC lands after the
wait, not inside it.

**Pipelining is the mechanism that actually hides the cost.** Push byte N+1 into
the TX FIFO, compute the CRC of byte N while it is on the wire, then collect RX.
That requires non-blocking FIFO access instead of a blocking per-byte call, so
it is a change to the transfer loop, not to the CRC helper. A resumable CRC is a
**prerequisite** for it and worthless without it - which is why the two are one
work item.

Order-of-magnitude estimate, to be confirmed by measurement rather than trusted:
at 12 MHz a byte is ~667 ns, about 100 core cycles at 150 MHz; a bit-by-bit
CRC16 over one byte is roughly 40-60 cycles. So the CRC should fit inside a byte
time if pipelined, and un-pipelined it plausibly adds tens of percent to a
512-byte block's wall time. That penalty is paid identically by the interleaved
and batched arrangements today.

**This is still worth doing even though DMA will not need it.** The DMA sniffer
computes the CRC in hardware for that path (see VALIDATION_PLAN.md), so none of
the above applies once DMA lands. But the blocking SPI path is intended to
remain as a fallback, and a fallback should still aim to minimise latency rather
than being left slow on the grounds that something faster exists. It is also the
path that runs first, on every card, before any DMA work is written.

Two constraints when it happens: keep it scoped to CRC16 - CRC7 is always
computed over exactly 5 bytes already sitting in a buffer - and do not fold in a
table-driven rewrite, which is a separate and separately measurable
optimisation. `docs/architecture.md` asks for the straightforward form first.

---

## Open

### SD-004 — CMD12 can mistake in-flight read data for its own response

Affected code: `sd_spi_stop_transmission()` in `src/storage/sd_spi.c`.

```sh
ctest --test-dir tests/build -R sd_gap_stop-residual --output-on-failure
```

During a CMD18 stream the card keeps sending until it decodes CMD12, so
residual read data can still be on the bus while the host is already looking for
the response. `sd_spi_stop_transmission()` discards exactly one stuff byte —
which is correct as far as it goes — and then treats the first byte with bit 7
clear as R1. A residual data byte with bit 7 clear satisfies that.

Measured against the model, with all three blocks of a read arriving intact:

| Residual bytes in flight when CMD12 is decoded | Result |
| --- | --- |
| 0 to 7 | `OK` |
| 8 or more | `IO_ERROR` |

Which way it goes above the threshold depends on the byte values of the *next*
block, so the same read can pass or fail depending on the card's contents. This
is a false negative, not a detected error: the data was correct.

This is a documented real-world hazard. Linux's `mmc_spi` driver carries a fix
for a related collision, where an out-of-range error token arriving as CMD12 was
sent left some cards rejecting every subsequent command until reset
([patch](https://lkml.iu.edu/hypermail/linux/kernel/1403.0/00865.html)).

**Two fixes addressing different halves. They are not alternatives.**

The collision is a function of *when* CMD12 is sent. The misread R1 is a
function of *how its response is interpreted*. Treating these as an either/or
was an error in an earlier revision of this note.

1. *Synchronise before stopping*, as Linux does: clock 0xFF until the next data
   token appears, then send CMD12. This fixes the **collision**: waiting until a
   token has arrived and been consumed means CMD12 no longer lands while the
   card is mid-token. The token's content is irrelevant to that — an error token
   works as a synchronisation point exactly as a start-block token does. Costs
   up to a block time of extra latency on every multiple-block read and needs
   its own bounded wait.
2. *Do not gate success on CMD12's response.* This fixes the **false negative**:
   the data has already been received and validated (SD-003), so a
   read whose payload is good should not fail because the response byte could
   not be located. Still wait out the busy period so the bus is quiescent. This
   does **not** address the collision, and by discarding CMD12's response as a
   failure signal it removes the earliest evidence that the card has wedged.

**Chosen: option 1**, because it is the one carrying the safety property. Option
2 remains available on top of it and is worth revisiting now that SD-003 has
landed.

**Not a specification requirement.** The Physical Layer Simplified Specification
was checked at v1.0, v2.00 §7.2.3 and v6.00 §7.2.3: all three say only that
CMD12 "will actually stop the data transfer operation" and none prescribe
waiting for a token first. Option 1 is a driver-level workaround for observed
card behaviour, not a conformance obligation. What the specification does
establish is that CMD12 is an **R1b** command — R1 with an optional trailing
busy signal — and that for R1b commands generally the sanctioned way to learn
the real outcome is `SEND_STATUS` (CMD13) after busy clears (§7.2.10 says this
for lock/unlock). The driver does not currently use CMD13 anywhere.

**Consequence if the collision is not prevented:** the card rejects every
subsequent command until reset. That is unrecoverable through the command
channel, so it needs a recovery path the driver does not yet have. Recorded
with the reset ladder and the open design decisions in
[VALIDATION_PLAN.md](VALIDATION_PLAN.md) under "Card recovery policy".

---

## Fixed during this work

SD-001 and SD-002 were previously reproduced and documented but left unfixed.
They now have regressions in the **enabled** suite, so reintroducing either
fails the default run; mutations `release-race-unchecked-single` and
`deinit-releases-twice` in `tools/mutations.txt` confirm that. SD-006 was closed
later and is recorded below it.

### SD-001 — removal during the release clock published a successful read

`sd_spi_device_read_blocks()` checked the removal latch around every data and
CRC transfer, but not after `sd_spi_release_bus()` clocked its final byte. A
card-detect edge landing on that byte left the read returning `OK` with
`removal_latched` already true.

`docs/architecture.md` requires that "a transaction interrupted by even an
unconfirmed removal edge must fail and must not resume" and that "a late
transfer-completion event must never change a cancelled request into success".
Initialisation already rechecked after its own release helper; the read paths
did not.

**Fix:** recheck the latch after `sd_spi_release_bus()` on both read success
paths and return `INVALID_DEVICE`. Four lines, symmetric with what
initialisation already did.

**Regressions:** `sd_removal_single-release_host_tests`,
`sd_removal_multi-release_host_tests`, and
`test_removal_during_the_release_clock` in `test_sd_faults.c`, which expresses
the same case against the release *phase* rather than a byte index, so it stays
valid if the driver's byte layout changes.

Scope note: only the success paths were changed. A removal edge landing on the
release clock of a path that was already failing still returns that path's
error rather than `INVALID_DEVICE`. That is arguably less precise, but the
operation fails either way, and narrowing the change kept it reviewable.

### SD-002 — teardown of an uninitialised device released hardware again

`sd_spi_device_deinit()` unconditionally called `spi_deinit()` and
`gpio_deinit()` on all five pins, even when the device was never initialised or
had already been torn down. Two consecutive teardowns released the SPI
peripheral twice, against the architecture's requirement that teardown happen
exactly once, and a device that was only ever configured would release a
peripheral it had never acquired — taking it from whatever owns it now.

**Fix:** return `OK` immediately when `sd->initialized` is false. This is safe
because every initialisation path that fails releases what it acquired, so an
uninitialised device owns nothing; the removal teardown path still runs in full,
because the interrupt handler does not clear `initialized`.

**Regressions:** `sd_removal_repeated-teardown_host_tests`, the
exactly-once assertions inside `sd_fx_check_removed_and_teardown_once()` which
every removal case uses, and the operation-sequence fuzz, which asserts one
hardware release per initialisation across random lifecycles.

---

### SD-006 — command frames carried a placeholder CRC7

Fixed 2026-09-09. `sd_spi_command()` computes a real CRC7 with
`crc_helper_7()` for every frame it builds, bring-up issues CMD59 with argument
1 to enable the card's checking and rejects a card that refuses it, and
`sd_spi_stop_transmission()` computes CMD12's CRC the same way instead of
sending the stop-bit-only placeholder.

`sd_gap_command-crc` passes and can be promoted out of the known-gap label when
convenient. In the enabled suite, `test_command_crc_checking_is_actually_enabled`
pins that the card really ends bring-up in checking mode,
`test_cmd59_rejection_fails_initialization` sweeps four refusal responses, and
`test_long_multiple_block_read` asserts the card records no protocol error
across a full multiple-block lifecycle - which is what a placeholder CRC on
CMD12 would trip.

Worth remembering from the fix: the regression briefly *passed* while CMD59 was
still being sent with argument 0, because the driver was switching the strict
card's checking off and CMD12's bad CRC was therefore never examined. Asserting
`sd_card_protocol_errors() == 0` rather than a return code is what made the real
defect visible. Mutation `cmd59-crc-disabled` reproduces that exact trap.

### SD-003 — read data CRC was discarded, so corruption was reported as success

Fixed 2026-09-12 for data blocks. `sd_spi_device_read_blocks()` folds every
payload byte through `crc_helper_rolling_16()` as it leaves the SPI
peripheral, assembles the two CRC bytes that follow most significant byte
first and compares; on a mismatch it issues CMD12, releases the bus and
returns `IO_ERROR` (or the removal / busy-timeout result if the stop sequence
reports one). Both the CMD17 and CMD18 paths do this and reset the running
register at every start-block token.

The former `sd_gap_data-crc` case was promoted, strengthened, into the enabled
`sd_faults_host_tests` as `test_read_data_crc_is_validated` (payload bytes at
the start, middle and end of a block, each CRC byte, and a bad CRC on an intact
payload, on CMD17 and on the first, middle and last block of a CMD18 stream,
on SDHC and a byte-addressed card; every row demands `IO_ERROR`, an intact
guard, a quiescent bus, zero protocol errors from the card and a device that
still reads) and `test_read_data_crc_mismatch_stops_the_stream_only_once`
(exactly one CMD12, no third block consumed). The fault sweep's exclusion for
payload-rewriting faults is gone and it now requires every DATA_PAYLOAD and
DATA_CRC fault row to fail. `test_read_data_crc_is_not_validated` no longer
exists. The helper itself is covered in `sd_crc16_host_tests`, including an
exhaustive check of the byte step at every (register, byte) pair against the
card model's implementation.

Mutants tried by hand against scratch copies of the source, all killed by the
enabled suite: CRC bytes assembled LSB first, the comparison removed, the
running register not reset between blocks, a wrong generator in the helper,
and the helper ignoring its carried register.

`sd_spi_read_csd()` validates the CSD register's CRC the same way (folded per
byte, compared before the structure is decoded, `IO_ERROR` on mismatch), and
the write path now builds its CRC with the rolling helper as each byte is
sent. The legacy scripted suites (`test_sd_spi.c`, `test_sd_irq_integration.c`)
had to start sending a real CSD CRC for bring-up to succeed - the mock had
been encoding the discard.

One thing remains and is recorded rather than hidden:

- **An all-zero block with an all-zero CRC is valid.** CRC-16/XMODEM starts
  from a zero register with no final XOR, so a bus stuck low from the first
  payload byte produces a frame the specification's CRC cannot reject. The
  sweep pins this: `BUSY_FOREVER` at payload offset 0 on a single-block read
  is *expected* to return `OK` with 512 zero bytes; from any later offset, or
  on a stream, it is detected. This is a property of the specified CRC, not
  of the implementation. See [RESIDUAL_RISK.md](RESIDUAL_RISK.md).

On a mismatch the driver fails rather than retrying; the bounded-retry policy
the card's error-recovery model expects is still the open decision recorded in
[VALIDATION_PLAN.md](VALIDATION_PLAN.md).

### SD-005 — the R1 wait was shorter than the specified response window

Fixed 2026-09-15. Both R1 polls - the command frame's in `sd_spi_command()`
and CMD12's in `sd_spi_stop_transmission()` - now read up to
`SD_SPI_R1_POLL_LIMIT` = 16 bytes, tolerating fifteen filler bytes, which is
the limit Linux's `mmc_spi` settled on after real cards were seen needing
twelve. The former `sd_gap_r1-tolerance` case (8..12 filler bytes accepted) is
retired: `test_response_latency_boundary` in the enabled protocol suite now
sweeps 0..16 for bring-up, CMD17 and a CMD18 stream ending in CMD12, and pins
the overrun at 16 as an immediate `IO_ERROR`. Mutations
`r1-poll-limit-off-by-one`, `r1-poll-limit-old-eight` and
`cmd12-poll-limit-old-eight` are killed by the enabled suites.

Worth remembering: the CMD12 poll had no enabled coverage before this - only
the disabled SD-004 case noticed a mutant that left it at 8 - and adding the
coverage found a one-byte timing error in the card model's stop sequence
(PROTOCOL.md P-04). SD-004 itself is unchanged: with residual data in flight
the wider window examines more residual bytes, but whether a residual byte is
mistaken for R1 is still data-dependent.

### SD-007 — writes were unimplemented

Implemented 2026-09-11. `sd_spi_device_write_blocks()` issues CMD24 for one
block and CMD25 for more, converts to byte addresses on standard-capacity
cards, sends the start-block token the command requires after the N_WR idle
byte, appends a real CRC16 from `crc_helper_16()`, decodes the data-response
token under the specification's `xxx0sss1` mask, waits out programming busy
between blocks and after stop-tran (with the N_BR idle byte in between),
sends CMD12 after a rejected block of CMD25 as 7.3.3.1 requires, and reports
`BUSY_TIMEOUT` when a programming wait outlives its 1 s budget.

The three acceptance cases that lived in `test_sd_gaps.c` were promoted into
`sd_writes_host_tests` (`test_sd_writes.c`) together with the rest of the write
coverage: every card kind, multiple-block writes up to sixteen blocks read back
out of the model, don't-care bits in the data-response token, rejections and
unknown response bytes, R1 errors, busy handling and overrun, removal at every
write phase, a fault sweep whose central invariant is that `OK` is only ever
reported when the card holds every block, and argument validation. Twenty-one
mutations in `tools/mutations.txt` under "write path" reproduce the mistakes
the implementation made or nearly made on the way; all are killed by that suite
(first verified per mutation against `sd_writes_host_tests` alone while
the catalogue runner was blocked on an unrelated baseline failure, then by the
full `tools/mutate.py` run recorded in MUTATION.md).

**Model changes made for this:** the card now insists on `0xFE` for CMD24 and
`0xFC` for CMD25 (it previously accepted either for both, which is how a CMD24
sent with `0xFC` passed), records a start token sent with no idle byte after
R1, resumes waiting for the next token after per-block programming busy in a
CMD25 transfer instead of going idle, accepts a command frame while waiting for
a token so CMD12 mid-write is decoded, and takes `sd_card_set_stop_tran_busy()`
to delay busy by N_BR bytes after stop-tran and to size the final programming
busy independently. The block overlay grew to 32 entries so a sixteen-block
write can be read back in full.

Still not asserted, by design: pre-erase (ACMD23), CMD13/ACMD22 after a write
error, partial-failure semantics beyond "the blocks before the rejection are on
the card", and write protection. The "unknown token" exits release the card
without terminating the transfer; the driver marks this TODO and the model
cannot represent the stuck state (it forgets a pending write on chip-select
release, see RESIDUAL_RISK.md), so that is hardware-validation territory.

## Other limits recorded during this review

Not reproduced failures; scope and unfinished work.

- ~~Public `get_info()` still returns `NOT_IMPLEMENTED`~~ — implemented in
  `7676adb`. It reports the block count decoded from the CSD, a 512-byte
  logical block size and `writable = true`, all from state cached at
  bring-up, without touching the bus. Covered by
  `test_get_info_reports_card_geometry` and
  `test_get_info_before_initialization_leaves_output_untouched` in
  `test_sd_spi.c` and by `test_get_info_matches_the_card_model` and
  `test_get_info_after_removal_is_rejected` in `test_sd_protocol.c`, the
  latter checked against the model's own block count rather than the driver's
  cached field.
- `get_info()` reports `writable = true` unconditionally; no write-protect
  state is ever read. The tests pin the current value with a comment rather
  than blessing it; settling it is a contract decision.
- Filesystem mount/unmount are stubs.
- `tools/mutate.py` requires the enabled suite to pass before it runs the
  catalogue. Seven cases asserting a 1 s ready wait had been failing since
  commit `4629e1d` cut `sd_spi_wait_ready()` to 250 ms; resolved 2026-09-11
  by settling the budgets (500 ms before a command, 250 ms elsewhere - see
  PROTOCOL.md P-16) and updating the tests to name them.
- `sd_spi_configure()` does not validate GPIO numbers or reject duplicate pin
  assignments. See [RESIDUAL_RISK.md](RESIDUAL_RISK.md) section 3.1.
- Argument validation order differs between the read and write backends. See
  RESIDUAL_RISK.md section 3.2.
- Foreground removal coordination, handle invalidation, USB media ownership,
  PIO and DMA cancellation, and low-power operation are not implemented.
- Host fakes cannot validate physical SPI completion, GPIO pad state,
  electrical removal, RP2350 errata, multicore memory ordering or real card
  compatibility. See RESIDUAL_RISK.md. One real-card run exists
  (VALIDATION_RESULTS.md, 2026-09-15).
- **The pre-command ready wait runs before CMD0, where it cannot mean
  "busy".** `sd_spi_command()` waits up to 500 ms for `0xFF` on MISO before
  every frame. Before CMD0 the card is still in SD native mode and leaves
  DAT0 undriven, so on a board with no pull-up on DO the line floats and
  `init` fails with `BUSY_TIMEOUT` without ever sending CMD0 - a misleading
  signature, met on the first hardware run. The specification's bring-up is
  74+ clocks with CS high, then CS low and CMD0; no ready wait is defined
  before it. Three ways to settle it, not yet chosen: skip the wait for CMD0;
  have `init` enable the RP2350 pull-up on `pin_controller_in` so the idle
  level is defined regardless of the board (the spec puts the DAT0 pull-up on
  the host, and an external resistor may still be stronger); or leave it to
  the board and document the signature. The host model always answers `0xFF`
  when idle, so no host test sees this; a test would need the SPI fake to
  return an undriven-line value before CMD0 and pin whichever choice is made.
