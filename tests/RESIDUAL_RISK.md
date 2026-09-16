# Residual risk

What the host suite cannot establish, however green it is. Each item says what
the risk is, why host testing cannot close it, and what would.

## 1. Bugs host testing still cannot catch

### 1.1 Instruction-level interrupt races

Faults and removal edges land between SPI byte transfers or at a named
transaction phase. A card-detect interrupt that arrives between two machine
instructions inside the driver — between reading the latch and acting on it, or
between raising chip select and clocking the release byte — cannot be
represented. The removal coverage is dense (every phase of both read paths,
every SPI byte of a single- and two-block read, every byte of bring-up) but it
is sampling, not exhaustive.

*Would be closed by:* reasoning about the compiled instruction sequence, or a
target test that fires the edge from a hardware timer at randomised offsets
across many repetitions.

### 1.2 Multicore execution and memory ordering

`sd_spi_t::removal_latched` is an `atomic_bool` accessed with
`memory_order_relaxed`, and the architecture anticipates storage work spanning
both RP2350 cores. The interrupt fake models one executing core at a time, calls
handlers synchronously on the test's own thread, and cannot produce two cores
racing. Relaxed ordering is sufficient for a single flag observed by one core,
but nothing here tests the multicore case the architecture says is coming.

*Would be closed by:* a target test with the dispatcher on one core and storage
work on the other; and a deliberate decision about the ordering requirement,
recorded, before shared removal state spans cores.

### 1.3 Real card behaviour outside the model

The model is a reading of the specification. A card that violates it, or that
exercises a corner the specification states in a way this reading gets wrong,
is not represented. Two known instances are already visible: response latency
beyond the specified window (P-04) and the CMD12 collision (P-07), both found
from field reports rather than from the specification text.

*Would be closed by:* bring-up against several real cards of each generation —
v1 SDSC, v2 SDSC, SDHC and SDXC — with bus captures, and adding any deviation
found to the model.

### 1.4 The SDK's own semantics are assumed, not verified

`fakes/` reimplements `time_reached`, `make_timeout_time_ms`, `gpio_put`,
`gpio_get`, `spi_write_read_blocking` and the rest from their documented
behaviour. If the real SDK differs — in how `time_reached` treats an already
expired deadline, in whether `gpio_put` before `gpio_set_dir` latches the output
value, in whether `spi_write_read_blocking` returns before the peripheral has
drained — the host suite cannot see it, because both the driver and the tests
run against the same reimplementation.

*Would be closed by:* a target smoke test that exercises the same sequences
against the real SDK, and a bus capture confirming that chip select is only
raised after the last byte has actually left the peripheral.

### 1.5 One SPI peripheral, one card

`spi_init`, `spi_deinit` and `spi_set_baudrate` ignore the `spi_inst_t *` they
are given and mutate a single global state. Two `sd_spi_t` instances, or an SD
card sharing a bus with the planned display, cannot be represented: the second
device's `spi_deinit()` would appear to release the first device's peripheral,
and no test would notice. The architecture already flags peripheral ownership as
unassigned; this is where the harness would have to change first.

*Would be closed by:* keying the SPI fake's state on the instance pointer, which
is a contained change but is not worth making until there is a second user.

### 1.6 Anything the model and the driver misunderstand identically

The model is the oracle for most tests. Where it shares a misreading with the
driver, both agree and the suite is silent. The mitigations are partial: CSD
registers captured from real cards, CRC7 values recomputed from the polynomial
rather than copied, and the specification citations in
[PROTOCOL.md](PROTOCOL.md).

*Would be closed by:* golden bus captures from real hardware, replayed against
the driver.

## 2. Hardware behaviours that need target or hardware-in-the-loop tests

| Behaviour | Why the host cannot see it | Proposed validation |
| --- | --- | --- |
| Electrical timing and signal integrity | no physical layer | logic-analyser capture of CS/SCK/MOSI/MISO through bring-up, single and multiple reads, timeouts and removal |
| Pad configuration and pull-ups | the GPIO fake records calls, not pad state | measure the availability line before firmware runs, with and without the external pull-up |
| Real switch bounce on card detect | bounce is modelled as discrete samples | remove and insert media repeatedly with the line instrumented; confirm the debounce and the interrupt suppression hold |
| Card timing variation and N_AC | the clock is idealised byte duration | measure response latency and access time across several real cards; compare against the driver's 8-byte poll limit and 100 ms wait |
| Actual SPI FIFO and transfer completion | `spi_write_read_blocking` is a byte loop | confirm the peripheral has drained before chip select is raised |
| DMA races, chained channels, RP2350-E5 abort ordering | no DMA exists yet | required before the planned DMA backend ships; see the architecture's verification requirements |
| PIO state machine cancellation | no PIO exists yet | required before the 4-bit backend ships |
| Bus contention with other SPI users | the fake owns one peripheral | decide the ownership model first, then test it |
| Power and low-power states | out of scope for the harness | instrumented current measurement |

The one bound the host *can* now state honestly: at 12 MHz the 100 ms
data-token wait clocks roughly 150,000 bytes and the 1000 ms ready wait roughly
1.5 million. Whether holding the bus that long is acceptable is a design
question the harness can pose but not answer.

## 3. Ambiguous or unspecified API contracts

These are flagged rather than changed, because settling them changes public
semantics and is the owner's decision.

### 3.1 `sd_spi_configure()` does not validate pin numbers

`baud_rate_hz` and `spi` are validated; the five GPIO numbers are not. An
out-of-range pin reaches `gpio_pull_up()` and `gpio_init()` before the
card-detect interrupt registration rejects it, which on real hardware is an SDK
assertion or a write to a register that does not exist. Duplicate pin
assignments — chip select and card detect on the same pin, say — are also
accepted, and would be catastrophic in the field.

*Decision needed:* whether `configure` should reject these. If yes, the check is
three lines and the test is easy; the reason it is not here is that it would
change what a currently-accepted configuration does.

### 3.2 Argument validation order differs between read and write

`sd_spi_device_read_blocks()` validates its arguments and then checks
usability; `sd_spi_device_write_blocks()` checks usability and range first and
validates `buffer` and `block_count` last. Both backends now refuse a null
buffer and a zero count on their own (`test_argument_validation_costs_no_bus_traffic`
in `test_sd_writes.c` calls the write backend through the operations table to
pin that). Called through `block_device_*` the ordering is hidden, because the
wrapper validates first. Called directly, the same bad call on an uninitialised
device returns `INVALID_ARGUMENT` from one backend and `NOT_INITIALIZED` from
the other.

*Decision needed:* whether the two orders should be made identical. Nothing
depends on the difference today.

### 3.3 `deinit` returns OK for a device that was never initialised

Now deliberate and tested: teardown is idempotent and releases hardware exactly
once. The alternative reading — that tearing down an uninitialised device is a
caller error deserving `NOT_INITIALIZED` — is defensible, and the planned
storage coordinator may want to distinguish them. Recorded so the choice is
visible rather than accidental.

### 3.4 The destination after a failed read

`block_device.h` says the whole destination is unspecified after any non-OK
read, and the tests honour that: they never assert content after a failure. They
do assert *how much* was written, so a change in that behaviour is deliberate.
Worth confirming that the eventual FatFs adapter really does discard the buffer
rather than trusting a partial result.

### 3.5 R1 error bits collapse to `IO_ERROR`

The driver treats any non-zero R1 as a generic I/O error, including the
parameter-error and address-error bits that map naturally onto `OUT_OF_RANGE`.
The architecture permits this, and the driver's own range check means those bits
should be unreachable. Recorded because the day a card sets them is the day the
generic error is least helpful.

## 4. Harness limitations left unsolved on purpose

- **No cycle-accurate timing.** Byte duration ignores inter-byte gaps, FIFO
  behaviour and interrupt latency. Modelling those would make the clock a
  simulator with its own bugs; the current model is a defensible lower bound
  and is documented as such wherever a test relies on it.
- **The trace is bounded at 4096 events.** A very long transaction can overflow
  it. `sd_card_trace_overflowed()` reports this, so a test cannot silently rely
  on a truncated trace, but the cap is not raised.
- **The transmit log keeps only the first 65536 bytes.** Counters keep counting;
  framing assertions live in the early bytes. `pico_mock_spi_tx_log_length()`
  exposes the boundary.
- **`SIM_CLOCK_POLL_TICK` is kept.** It preserves the artifact deliberately, so
  the suite can be run under both models and any dependence on it is exposed.
  Keeping it costs one CTest case and prevents a silent regression.
- **The block overlay holds sixty-four explicit blocks.** Everything else is
  generated. A multiple-block write stores one entry per block, so this is the
  most distinct blocks a test can write and read back; raise
  `SD_MODEL_OVERLAY_BLOCKS` for more. A write beyond the overlay is dropped
  **silently** - the card still answers with an accepted data-response token -
  so a readback mismatch after a long write sequence should be checked against
  this limit before it is blamed on the driver. The main.c demo writes 51
  blocks, which is how the limit was found at its previous value of 32.
- **The model forgets a pending write when chip select is released.** A real
  card that has answered R1 to CMD24/CMD25 and not yet seen a start-block
  token stays in its receive-data state across a deselect; it does not parse
  command frames there, and it may swallow a later argument byte that happens
  to be `0xFC` as a start token. The model goes idle instead, so a driver that
  abandons a write after an unrecognised data-response byte (the driver's own
  TODO) looks recoverable here and may not be on hardware. That specific
  sequence - unknown token, release, next command - needs a bus capture from a
  real card.
- **Data-response and busy timing are idealised.** The model answers the
  data-response token on the byte after the CRC and, unless
  `sd_card_set_stop_tran_busy()` says otherwise, asserts busy on the very next
  byte; real cards vary within the specification's windows. The N_BR case is
  covered by one test with a one-byte delay; N_CR and N_AC variation on the
  write commands is not swept.
- **Removal cannot be injected between two instructions.** Faults land during
  an SPI transfer or at a phase boundary. One defensive check in
  `sd_spi_stop_transmission()` guards exactly that window and is therefore
  unreachable from here; it is recorded in `tools/mutations.txt` as an
  `unkillable:` mutation with the reason, rather than deleted or pretended to
  be covered.
- **`test_main.c`, `test_board.c` and `test_debug.c` keep their own fakes.**
  They do not touch storage and unifying them would add coupling for no
  detection power.
- **Clang's sanitizers are not exercised in this environment** because the
  compiler-rt runtime is not installed. GCC's ASan and UBSan are, and clang is
  used as a second static-analysis pass. Installing `libclang-rt-<version>-dev`
  enables the clang sanitizer configuration, which carries a few checks GCC's
  does not.

## 5. Known gaps that are open

Two regressions assert desired behaviour and fail against the current source.
They are registered in CTest and disabled by default; see
[KNOWN_GAPS.md](KNOWN_GAPS.md). A green default run is not evidence that they
are resolved.

- **SD-004** — CMD12 can mistake in-flight read data for its own response.
- **SD-005** — the R1 wait is one byte short of the specified window and well
  short of what real cards have needed.

One limit of read CRC validation (SD-003, closed) is inherent rather than an
open gap:

- **An all-zero packet is self-consistent.** CRC-16/XMODEM has a zero initial
  register and no final XOR, so 512 zero bytes followed by `0x00 0x00` is a
  valid frame. A bus stuck low from the first payload byte of a single-block
  read is therefore reported as a successful read of an erased block. Stuck
  high (`0xFF`) is detected, as is stuck low from any later byte or on a
  multiple-block stream. Only a different CRC parameterisation would close
  this, and the specification fixes the parameterisation. The same holds for
  the CSD register, which is also validated: a 16-byte all-zero register with
  a zero CRC would pass the CRC, but its `READ_BL_LEN` of 0 is outside the
  9..11 range the decoder accepts (and a high-capacity card reporting CSD
  structure 0 fails the version sanity check), so bring-up rejects it anyway.
