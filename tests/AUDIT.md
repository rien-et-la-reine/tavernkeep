# Test-infrastructure audit

What the previous harness could and could not detect, what replaced it, and
what it still cannot see. Written 2026-09-04 against the working tree.

## The starting point

The suite was not thin. It had 15 enabled CTest cases and 60 SD test groups
with table-driven inputs, per-byte removal sweeps, error-token matrices and
two reproduced production defects documented rather than hidden. Judged by
detection power rather than by count, though, several whole classes of bug
could not be caught, because the fake could not produce the behaviour involved.

### Weakness 1 — fake time advanced with poll count, not work

`time_reached()` added one simulated millisecond every time it was called. Every
"timeout" assertion was therefore an assertion about how often the driver
happened to call one function. Consequences:

- A driver that polled twice per loop iteration would have "timed out" in half
  the time, and no test would have noticed.
- A loop that clocked a million SPI bytes between two time checks would never
  have timed out at all.
- `test_card_initialization_timeout_is_bounded` asserted
  `cmd41_count < 1200`, which is not a statement about the ACMD41 budget.
- The claim that a timeout does "bounded work" was an artifact. Under a clock
  driven by real byte time, the 100 ms data-token wait costs roughly 150,000
  bus bytes at 12 MHz, and the 1000 ms ready wait costs about 1.5 million.
  That is true of the driver, was true before this work, and was invisible.

**Replaced by** `sim_clock`, where time comes from SPI byte duration at the
programmed baud rate, from `sleep_ms`, and from explicit advances, and
`time_reached()` is a pure query. The legacy model is retained as
`SIM_CLOCK_POLL_TICK` and the whole SD suite runs under **both**, so a case that
only passes under the artifact is exposed.

### Weakness 2 — the card could not be wrong in most of the ways a card can

The fake answered a command by pushing an opaque byte string into a queue. It
never examined what the host sent beyond the command index. So:

- **CRC7 was never checked.** The driver then sent hardcoded bytes for CMD0
  (0x95) and CMD8 (0x87), the two a real card validates unconditionally. Either
  could have been wrong and every test would still have passed. Those constants
  have since been replaced by a computed `crc_helper_7()` over the whole frame;
  mutating its length (`command-crc-frame-length`) now fails both SD suites
  outright.
- **ACMD41 needed no CMD55.** Responses were keyed on the command index alone,
  so deleting the CMD55 that turns CMD41 into ACMD41 was undetectable. It now
  fails five executables.
- **The HCS bit was not meaningful.** A high-capacity card that is polled
  without HCS never leaves the idle state; the fake answered regardless.
- **Command framing was unchecked**: start bit, transmission bit and end bit
  could all have been wrong.

**Replaced by** a card state machine that validates framing and CRC, tracks the
application-command state, and refuses to leave idle without HCS.

### Weakness 3 — transaction length was a harness limit

A scripted response lived in a 4096-byte per-command buffer, and a
multiple-block read costs 515 bytes per block, so nothing beyond about seven
blocks could be expressed. Any future feature that reads more — a filesystem
cluster, an audio buffer, a USB mass-storage transfer — had no way to be tested.
The transmit log was a fixed 16384-byte array that called `abort()` on overflow,
so a long transaction killed the process rather than failing a test.

**Replaced by** streaming from a generated block store, with no length limit,
and a transmit log that stops growing rather than aborting, with
`pico_mock_spi_tx_log_length()` so a test cannot silently rely on truncated
history. Reads of 16 blocks are now routine; the limit is the simulated clock,
not memory.

### Weakness 4 — the card stopped instantly and cleanly on CMD12

`complete_command()` cleared the response queue when it saw CMD12. Real cards do
not stop instantly: during a CMD18 stream the card keeps sending until it
decodes the stop command, so residual read data can still be on the bus while
the host is already looking for CMD12's R1. This is a documented real-world
hazard — Linux's `mmc_spi` driver carries a fix for a related collision that put
some cards into a state where they rejected every subsequent command.

**Now representable** through `sd_card_set_stop_residual_bytes()`, and the
driver's behaviour under it is registered as gap SD-004: with eight or more
residual bytes overlapping the response window, a read whose data all arrived
correctly is reported as an I/O error, and which way it goes depends on the byte
values of the *next* block.

### Weakness 5 — failures could only be injected where a byte index happened to fall

Removal was injectable at a byte offset, which produced dense coverage but
coupled every case to the driver's exact byte layout, and could not express
"fail while the card is busy" or "fail on the second block" without recomputing
offsets. Nothing else could be injected at all: no substituted R1, no truncated
payload, no corrupted byte, no permanent busy, no arbitrary bytes.

**Replaced by** phase-based fault injection with eleven fault kinds, where the
occurrence index counts entries into a phase *within a command*, so "the second
block of this read" stays correct when bring-up changes.

### Weakness 6 — partial-operation semantics were barely checked

Buffer canaries were checked, which catches an overrun, but not *how much* of a
destination was populated before a failure. The distinction matters: a caller
that ignores the contract and uses a partially written buffer behaves very
differently depending on whether the driver wrote nothing, wrote three blocks,
or wrote three blocks and part of a fourth.

**Replaced by** `sd_fx_guard_prefix_from_card()`, which counts leading bytes that
match the card's real content, and `sd_fx_guard_suffix_is_fill()`. Counting
bytes that merely differ from a fill pattern under-counts, because real block
data contains the fill byte by coincidence — an earlier draft of these tests
reported 508 written bytes where 512 were written.

### Weakness 7 — only one card existed

Every SD fixture described the same shape of card by hand-assembling response
bytes. There was no way to say "a 64 GB SDXC card" and no independent check that
the hand-assembled CSD meant what the test thought it meant. Both the test's
encoder and the driver's decoder were written from the same reading of the same
specification, so agreement between them proved only self-consistency.

**Replaced by** six card kinds with registers synthesised from a capacity, plus
five CSD registers captured from real cards, published with their marketed
capacities, each verified by its own CRC7 before use. One candidate vector was
discarded because its CRC7 did not check out — which is exactly the failure mode
the CRC guard exists to catch.

## What the new infrastructure can represent

- Six card kinds: v1 SDSC, v2 SDSC, SDHC, SDXC, SDUC (which must be refused),
  and an MMC-like card that rejects CMD8 and ACMD41.
- Response latency anywhere in and beyond the specified N_CR window.
- Read access delay, programming busy, and busy periods measured in time.
- Multiple-block streams of arbitrary length, with the card streaming until
  CMD12 and residual data still in flight when it arrives.
- Eleven fault kinds at sixteen named transaction phases, with per-command
  occurrence and byte offsets.
- Card ejection injected at a phase rather than a byte index.
- The complete write path — CMD24/CMD25, host data tokens, data-response
  tokens, stop-tran, programming busy — which no production code used at the
  time; the production write path landed 2026-09-11 and is covered by
  `sd_writes_host_tests`.
- Arbitrary and pseudo-random card responses, for parser fuzzing.
- Command CRC7 enforcement, application-command state, and framing validation.
- A full protocol trace, queryable by kind and ordinal, dumped on any failure.

## What is now detected that was not

From `tools/mutations.txt`, 47 of 50 representative incorrect implementations
are caught, 2 are documented equivalent mutants, and 1 is caught only under the
sanitizer configuration. The ones that the previous suite could not have caught
at all, because the fake could not produce the situation:

| Mutation | Why it was invisible before |
| --- | --- |
| `command-crc-frame-length` (then `cmd8-crc-constant`) | the fake never looked at the CRC byte |
| `drop-cmd55` | responses were keyed on the command index alone |
| `acmd41-hcs-bit` | the fake answered regardless of HCS |
| `ocr-powerup-check-removed` | no way to make the card report "still powering up" |
| `r1-accept-nonzero` | only a handful of R1 values were exercised; the sweep covers all 128 |
| `irq-register-overwrites-owner` | the pattern never matched: the mutation script did not handle the CRLF sources |
| `irq-bounds-check-removed` | nothing delivered the boundary GPIO index |

Two production defects that were previously *documented but unfixed* now have
enabled regressions, and reintroducing either fails the default run:

- **SD-001**, removal during the final release clock publishing a successful
  read. Fixed by rechecking the latch after `sd_spi_release_bus()` on both
  success paths.
- **SD-002**, teardown of an uninitialised device releasing the SPI peripheral
  again. Fixed by making `deinit` a no-op when nothing was acquired.

The fault sweep also found the read-data-CRC gap on its own: every payload-phase
corruption is reported as a successful read, because the driver discards the
CRC. That is registered as SD-003 with a failing regression rather than being
absorbed into a weakened assertion.

## Blind spots that remain

These are properties of the approach, not oversights. The full list with
severities and what would close each one is in
[RESIDUAL_RISK.md](RESIDUAL_RISK.md).

- **Instruction-level races.** Faults land between SPI bytes or at a phase
  boundary. An interrupt arriving between two machine instructions inside the
  driver cannot be represented.
- **Multicore.** The interrupt fake models one executing core at a time. It
  cannot represent two cores racing, and it cannot exercise the memory-ordering
  choices in the removal latch.
- **Physical behaviour.** No electrical timing, no signal integrity, no real
  pad configuration, no bus contention, no card-side timing variation, no
  silicon errata. The clock is an idealisation of byte duration.
- **Real card compatibility.** The model is a reading of the specification. A
  card that is out of specification, or specified in a way this reading gets
  wrong, is not represented. The only defence used here is the CSD vectors from
  real cards.
- **Filesystem, DMA and PIO behaviour.** (Writes were in this list when the
  audit was written; the production write path and its suite came later.)
  Nothing here says anything about FatFs, DMA cancellation, RP2350-E5 abort
  ordering or USB media ownership.
- **The model is shared with the tests that use it.** If the model
  misunderstands the protocol in the same way the driver does, both agree and
  the suite is silent. The CSD vectors from real cards, the independently
  computed CRC7 values, and the specification citations in
  [PROTOCOL.md](PROTOCOL.md) are the mitigations; they are partial.
