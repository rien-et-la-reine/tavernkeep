# Protocol and specification findings

Every rule the SD tests encode, where it comes from, whether the implementation
follows it, and which test holds it in place. Findings are numbered P-nn and
referenced from the test sources.

## Sources consulted

| Source | Used for |
| --- | --- |
| SD Physical Layer Simplified Specification v2.00, [ECE UT Austin mirror](https://users.ece.utexas.edu/~valvano/EE345M/SD_Physical_Layer_Spec.pdf) | SPI response formats R1/R1b/R3/R7, control tokens, CSD v1.0 and v2.0 field positions and capacity formulas, READ_BL_LEN range, default-speed clock ceiling |
| SD Physical Layer Simplified Specification v6.00, [mirror](https://www.taterli.com/wp-content/uploads/2017/05/Physical-Layer-Simplified-SpecificationV6.0.pdf) | cross-check of the SPI bus-transfer protection and control-token sections |
| ChaN, *How to Use MMC/SDC*, reproduced as [EE445M Lecture 12.1](https://users.ece.utexas.edu/~valvano/EE345M/view12_SPI_SDC.pdf) | N_CR byte window, CMD12's positional stuff byte, data packet tokens, data-response codes, which commands need a valid CRC in SPI mode, busy signalling |
| [Linux `mmc_spi`: wait more bytes for card response](https://lkml.iu.edu/hypermail/linux/kernel/0903.1/01387.html) | real-world response latency beyond the specified window |
| [Linux `mmc_spi`: synchronize STOP_TRANSMISSION with next data token](https://lkml.iu.edu/hypermail/linux/kernel/1403.0/00865.html) | the CMD12 / in-flight-data collision and its consequences |
| [Gough Lui's SD/SDHC/SDXC CID CSD register database](https://goughlui.com/other-pages/sdsdhcsdxc-cid-csd-register-data-database/) | CSD registers captured from real cards, with marketed capacities |
| [Raspberry Pi Pico SDK hardware reference](https://www.raspberrypi.com/documentation/pico-sdk/hardware.html) | shared GPIO callback ownership, DMA abort and RP2350-E5 (future work only) |

The specification is the authority; the secondary sources are used where the
simplified specification omits a detail, and are cross-checked against each
other and against arithmetic wherever possible.

## P-01 — every command frame carries a real CRC7, and CMD59 turns checking on

**Rule.** In SPI mode command CRC checking is disabled by default, so a host
*may* send a placeholder CRC. CMD0 and CMD8 are the exception: they bracket the
transition into SPI mode and the card validates them regardless. CMD59
(`CRC_ON_OFF`) takes `[31:1]` stuff bits and `[0]` as the CRC option — 1 enables
checking, 0 disables it — after which the card validates every frame. A command
frame is five bytes plus `(CRC7 << 1) | 1`.

**Verified independently.** CRC7 over `40 00 00 00 00` is 0x4A, giving the frame
byte 0x95; over `48 00 00 01 AA` it is 0x43, giving 0x87. Both were recomputed
from the polynomial rather than taken from a source, and the test recomputes
them again at run time. The same reference reproduces the driver's bytes for
CMD59 (0x83 with argument 1), CMD55 (0x65), ACMD41 with HCS (0x77), CMD58
(0xFD) and CMD9 (0xAF).

**Implementation.** `sd_spi_command()` builds the five-byte frame in a buffer
and sends `(crc_helper_7(buffer, 5) << 1) | 0x01`, so the two former hardcoded
constants are gone and every frame it sends is covered. Bring-up issues CMD59
with argument 1 immediately after CMD8 — before ACMD41, so the rest of the
sequence is protected — and rejects a card whose R1 comes back above 0x01.

`sd_spi_stop_transmission()` builds its own CMD12 frame rather than calling
`sd_spi_command()`, because CMD12 carries a positional stuff byte before its R1
that the shared path does not model - but it computes the CRC7 the same way. All
twelve command sites therefore send a real CRC. This was SD-006, closed
2026-09-09.

**Tests.** `test_command_framing_and_crc` recomputes the CMD0 and CMD8 bytes and
confirms them on the wire. `test_command_crc_checking_is_actually_enabled`
asserts the card ends bring-up with checking genuinely on — one CMD59, argument
bit 0 set — because a correct CRC the card never inspects is worth nothing.
`test_cmd59_rejection_fails_initialization` sweeps four refusal responses and
requires bring-up to fail rather than proceed unprotected. `sd_gap_command-crc`
requires a full lifecycle against strict cards and passes.
Mutations `command-crc-frame-length` and `cmd59-crc-disabled` are both caught.
The card model validates CMD0 and CMD8 unconditionally, so those bytes can never
be wrong silently.

## P-02 — ACMD41 requires an immediately preceding CMD55

**Rule.** An application command is CMD55 followed by the command; without the
prefix the card treats CMD41 as an unknown command and sets the
illegal-command bit.

**Implementation.** Correct: the initialisation loop issues CMD55 then CMD41 on
every iteration and checks both responses.

**Tests.** `test_acmd41_is_always_prefixed_by_cmd55` walks the trace and asserts
every CMD41 carries the application flag, and that the CMD55 and CMD41 counts
match. Mutation `drop-cmd55` is caught by five executables. This was
undetectable before, because the previous fake keyed responses on the command
index alone.

## P-03 — ACMD41's HCS bit selects high-capacity support

**Rule.** Bit 30 of the ACMD41 argument tells the card the host supports high
capacity. A high-capacity card polled with HCS clear never leaves the idle
state.

**Implementation.** Correct: `0x40000000` for a non-legacy card, `0` for a v1
card.

**Tests.** `test_acmd41_is_always_prefixed_by_cmd55` asserts the argument on
every occurrence; `test_legacy_card_omits_the_hcs_bit` covers the v1 case;
`test_high_capacity_card_needs_hcs_to_leave_idle` proves the driver gives up on
its own 1200 ms budget rather than hanging. Mutation `acmd41-hcs-bit` is caught.

## P-04 — N_CR, the response window, and the driver's poll limit

**Rule.** The command response time in SPI mode is quoted as 0 to 8 bytes for an
SD card. `sd_spi_command()` and `sd_spi_stop_transmission()` each read at most
`SD_SPI_R1_POLL_LIMIT` = 16 bytes while waiting for a byte with bit 7 clear, so
they accept a response at read positions one through sixteen, which is at most
**fifteen** filler bytes.

**Finding.** The limit was 8 reads (7 filler bytes) until 2026-09-15, one byte
short of the specified window under its strictest reading and well short of
what real cards have needed: Linux's `mmc_spi` driver raised its own limit from
8 to 16 after observing cards that needed 12. That was gap **SD-005**.

**Disposition.** Raised to 16, Linux's number, on 2026-09-15; there was no
reason to choose differently. The one card run on hardware so far answered
within the old window. The overrun case still fails fast with `IO_ERROR`
rather than burning a timeout.

**Tests.** `test_response_latency_boundary` sweeps 0 to 16 filler bytes for
bring-up and for CMD17, asserts success below the limit and failure at it, and
asserts the failure is immediate rather than a timeout; a third sweep runs a
CMD18 stream so CMD12's own poll is covered for every accepted count. Mutations
`r1-poll-limit-off-by-one`, `r1-poll-limit-old-eight` and
`cmd12-poll-limit-old-eight` are caught. Adding the CMD12 sweep exposed a
one-byte error in the card model: it emitted CMD12's stuff byte as the response
to the last frame byte instead of the byte after it, which only shows at
N_CR = 0; fixed in `sd_card_model.c` at the same time.

## P-05 — data error tokens

**Rule.** When a read fails the card sends an error token instead of a data
packet: bits 7..4 are zero and bits 3..0 carry error flags — bit 0 Error, bit 1
CC Error, bit 2 Card ECC Failed, bit 3 Out Of Range. Any combination of the low
four bits is valid, so there are fifteen legal tokens. A byte of 0x00 has no
error bits and is not an error token.

**Implementation.** `sd_spi_is_data_error_token()` tests
`(token & 0xF0) == 0 && (token & 0x0F) != 0`. Correct, including the multi-bit
combinations.

**Tests.** `test_data_error_tokens_are_recognised_immediately` covers all fifteen
tokens on both read paths and asserts recognition is immediate — under a
microsecond and under forty bus bytes — rather than the same generic result
reached by exhausting the 100 ms data-token wait. That distinction is the whole
point: a driver that ignored the token would still return IO_ERROR eventually.
`test_zero_byte_is_not_a_data_error_token` covers the negative case and confirms
the driver still honours its 100 ms budget. Mutations
`data-error-token-mask` and `data-error-token-ignored` are both caught.

## P-06 — CSD field positions and capacity

**Rule.** CSD version 1.0: READ_BL_LEN at bits 83..80 with supported values 9 to
11; C_SIZE at bits 73..62; C_SIZE_MULT at bits 49..47; capacity is
`(C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN`. CSD version 2.0: C_SIZE at
bits 69..48, capacity `(C_SIZE + 1) * 512 KiB`, so `(C_SIZE + 1) * 1024` blocks
of 512 bytes.

**Implementation.** All field extractions and both formulas are correct, and
CSD structure 2 and 3 are refused with NOT_IMPLEMENTED.

**Independent check.** The tests decode five CSD registers captured from real
cards and compare against the cards' marketed capacities:

| Card | CSD | Decoded blocks | Capacity |
| --- | --- | --- | --- |
| Kingston 2 GB SDSC | `002d00325b5a83d5fefbff80168000cf` | 4 022 272 | 2.06 GB |
| SanDisk 2 GB Blue SDSC | `002600325f5a83c93efbcfff928040cb` | 3 970 048 | 2.03 GB |
| Samsung 32 GB C10 SDHC | `400e00325b590000ee9d7f800a400013` | 62 552 064 | 32.0 GB |
| Toshiba 64 GB SDXC | `400e00325b590001dbff7f800a40003f` | 124 780 544 | 63.9 GB |
| Kingston 128 GB SDXC | `400e00325b590003a5df7f800a400007` | 244 809 728 | 125.3 GB |

Each vector's own CRC7 is verified before use, so a transcription error fails
loudly instead of looking like a decoder bug. A sixth candidate was discarded
for exactly that reason: its CRC7 did not check out.

Both SDSC vectors use READ_BL_LEN 10, confirming that a 2 GB standard-capacity
card cannot be encoded with READ_BL_LEN 9 — the largest C_SIZE and C_SIZE_MULT
pair reaches only 1 GiB. The harness asserts this rather than silently
producing an impossible card.

**Tests.** `test_real_card_csd_registers`, `test_csd_structure_and_field_rejection`
(which sweeps all sixteen READ_BL_LEN encodings), and the property tests
`test_csd_v1_capacity_property` and `test_csd_v2_capacity_property`, which cover
every C_SIZE_MULT and READ_BL_LEN against edge and random C_SIZE values.
Mutations `csd-v2-capacity-multiplier`, `csd-v2-c-size-mask`,
`csd-v1-mult-off-by-one`, `csd-v1-read-bl-len-range` and
`csd-structure-check-removed` are all caught.

## P-07 — CMD12 has a positional stuff byte, and the card may still be sending

**Rule.** In SPI mode the byte immediately following CMD12 is a stuff byte and
must be discarded before the R1 response. CMD12's response is R1b: R1 followed
by a busy period during which the card holds the line low.

**Implementation.** `sd_spi_stop_transmission()` discards exactly one stuff byte,
polls up to eight bytes for R1, then waits for the busy period to clear.
Correct, and a subtle detail to have got right.

**Finding.** The rule above assumes the card has stopped. It has not: during a
CMD18 stream the card keeps sending until it decodes CMD12, so residual read
data can still be on the bus while the host is looking for the response. The
driver treats the first byte with bit 7 clear as R1, and a data byte with bit 7
clear satisfies that. Measured against the model: with zero to seven residual
bytes the read succeeds; with eight or more it returns IO_ERROR even though
every block arrived intact, and which way it goes depends on the byte values of
the following block. Linux carries a fix for a related collision, where an
out-of-range error token arriving as CMD12 is sent left some cards rejecting
every subsequent command until reset.

**Disposition.** Not changed here. The two candidate fixes — draining to the
next data token before sending CMD12, as Linux does, or not gating success on
CMD12's response at all — are design decisions with latency and
hardware-validation consequences. Registered as gap **SD-004** with a failing
regression and a reproduction across zero to sixteen residual bytes.

**Tests.** `sd_gap_stop-residual`; mutation `cmd12-stuff-byte-missing` is caught
by the enabled suite.

## P-08 — bring-up requires at least 74 clocks with the card deselected

**Rule.** The card needs at least 74 clock cycles with chip select released
before it will accept CMD0.

**Implementation.** Ten 0xFF bytes, which is 80 clocks. Correct.

**Tests.** `test_idle_clocks_precede_the_first_command` asserts the byte values,
that chip select was genuinely high for all ten, and that the first command byte
only appears after chip select drops.

## P-09 — OCR bit 31 is the power-up status and bit 30 is CCS

**Rule.** CMD58 returns R3: R1 followed by the four-byte OCR. Bit 31 is set once
the card has finished its power-up sequence; bit 30, CCS, is set for a
high-capacity card and selects block addressing.

**Implementation.** Correct on both bits, and CCS is ignored for a card that
identified as legacy, which is sound: a v1 card cannot be high capacity.

**Tests.** `test_card_variant_initialization` covers CCS across four card kinds;
`test_ocr_reports_card_still_powering_up` clears bit 31 and asserts bring-up
fails without going on to read the CSD. Mutations `ocr-ccs-bit` and
`ocr-powerup-check-removed` are both caught.

## P-10 — addressing mode follows card capacity

**Rule.** High-capacity cards take a block address in read and write commands;
standard-capacity cards take a byte address, which must be aligned to the block
length. CMD16 sets the block length and is only meaningful for standard-capacity
cards.

**Implementation.** Correct. CMD16 with argument 512 is issued only when CCS is
clear.

**Boundary.** The driver passes the address as `uint32_t`. The largest CSD v1
encoding is 4 GiB, whose last block sits at byte address `0xFFFFFE00` — 512
bytes below a 32-bit overflow. The largest CSD v2 encoding gives exactly `2^32`
blocks, whose last LBA is exactly `UINT32_MAX`. Both fit, with no margin. This
is safe only because CSD v3 (SDUC) is refused; an SDUC card would exceed 32 bits
and truncate silently.

**Tests.** `test_addressing_mode_per_card_type`, `test_capacity_and_address_boundaries`,
`test_csd_v1_maximum_encoding_is_addressable`, and the
`test_address_conversion_property` fuzz over random cards and addresses.
Mutations `sdsc-byte-address-dropped`, `sdsc-byte-address-inverted`,
`address-truncated-to-16-bits`, `cmd16-skipped-for-sdsc` and
`range-check-count-removed` are all caught.

## P-11 — the default-speed clock ceiling is 25 MHz

**Rule.** Default speed mode runs from 0 to 25 MHz. Bring-up runs at a low
frequency, typically 100–400 kHz, and the host raises the clock only after the
card is initialised.

**Implementation.** `sd_spi_configure()` rejects a configured rate above 25 MHz;
bring-up runs at 400 kHz and the configured rate is applied only after success.
Correct.

**Tests.** `test_card_variant_initialization` asserts both baud rates on every
card kind. Mutations `baud-limit-removed` and `baud-raised-before-init` are
caught.

## P-12 — read data CRC16

**Rule.** Every data packet carries a 16-bit CRC (CRC-CCITT, polynomial
`x^16 + x^12 + x^5 + 1`) after the payload.

**Implementation.** `sd_spi_device_read_blocks()` folds each payload byte
through `crc_helper_rolling_16()` as it is received, reads the two CRC bytes
most significant first and compares. A mismatch on the CMD18 path issues CMD12,
releases the bus and returns `IO_ERROR`; on the CMD17 path the card has already
finished transmitting, so the driver releases the bus and returns `IO_ERROR`
without a stop command. `sd_spi_read_csd()` validates the CRC after the 16-byte
CSD register the same way, before the register is decoded.

**Disposition.** Gap **SD-003** closed for data blocks on 2026-09-12. The
parameterisation is CRC-16/XMODEM: generator `0x1021`, zero initial register,
MSB first, no reflection, no final XOR. Its one structural blind spot is that
an all-zero payload has CRC `0x0000`, so a bus stuck low across an entire
packet is accepted; that is a property of the specified CRC and is recorded in
RESIDUAL_RISK.md.

**Tests.** `test_read_data_crc_is_validated` and
`test_read_data_crc_mismatch_stops_the_stream_only_once` in `test_sd_faults.c`;
the fault sweep there requires every payload and CRC-byte fault row to fail
except the stuck-low-from-offset-0 single-block row, which it pins as the
false success it is. `sd_crc16_host_tests` covers the helper, including an
exhaustive (register, byte) check against the model's implementation.

## P-13 — write data packets: start-block tokens and the N_WR idle byte

**Rule.** A single-block write (CMD24) sends its block behind the `0xFE` start
token, the same token reads use; a multiple-block write (CMD25) sends each
block behind `0xFC` and ends the transfer with the `0xFD` stop-tran token
(specification §7.3.3.2). The host must clock at least one idle byte between
the command's R1 and the first start token (N_WR, §7.5 timing values); a
block is the token, 512 data bytes and the 16-bit CRC of P-12 computed by the
host, which the card verifies whenever CMD59 has enabled checking (§7.2.2).

**Implementation.** `sd_spi_device_write_blocks()` sends the token each
command requires after one `0xFF`, computes `crc_helper_16()` over the block
in the caller's buffer, and ends CMD25 with `0xFD`. An earlier draft sent
`0xFC` for CMD24 and passed, because the model then accepted either token for
either command; the model now insists on the right one.

**Tests.** `test_single_block_write_stores_the_data_on_every_card_kind` and
`test_multi_block_write_stores_every_block_in_order` check the token per block
from the trace, the data the card holds (including an all-zero block whose CRC
is `0x0000`, an all-`0xFF` block whose CRC is the published `0x7FA1`, and a
block containing every token byte), and `sd_card_protocol_errors() == 0`,
which is where a missing N_WR byte or a wrong token surfaces. Mutations
`single-write-multi-token`, `multi-write-single-token`, `*-nwr-gap-dropped`,
`write-crc16-end-bit`, `write-crc16-short`, `write-source-stride`.

## P-14 — the data-response token

**Rule.** Every written block is answered with one byte of the form
`xxx0sss1` (§7.3.3.1): `sss = 010` accepted, `101` rejected for CRC error,
`110` rejected for write error; the upper three bits are don't-care, and real
cards commonly drive them high (`0xE5`). It is positional - the byte after the
second CRC byte - and is followed by busy while the card programs. In a
multiple-block write, any rejection is to be followed by CMD12 from the host;
ACMD22 then reports how many blocks were written and CMD13 the cause. For a
single-block write there is no transfer to stop.

**Implementation.** The driver masks the byte with `0x1F`, treats `0x05` as
acceptance, `0x0B`/`0x0D` as rejection (waits out busy, sends CMD12 for
CMD25 only, reports `IO_ERROR`) and anything else as an unknown response
(`IO_ERROR`, no further blocks). It does not use ACMD22 or CMD13.

**Tests.** `test_data_response_upper_bits_are_dont_care` (`0x05`, `0xE5`,
`0x25`, `0xC5`), `test_rejected_data_response_is_an_error` (both codes with
both bit patterns; asserts CMD12 once for CMD25 and never for CMD24, and that
stop-tran is not sent instead), `test_rejection_mid_stream_stops_the_transfer`,
`test_rejection_followed_by_busy_is_waited_out`,
`test_unknown_data_response_is_an_error`. Mutations `multi-write-response-mask`,
`*-rejection-ignored`, `*-unknown-token-accepted`, `multi-write-cmd12-dropped`,
`multi-write-error-mapping-precedence`.

## P-15 — programming busy after a block and after stop-tran, and N_BR

**Rule.** After the data-response token, and after the stop-tran token, the
card holds DataOut low while it programs; the host must not send the next
block until that busy ends, and a write is not complete until the last busy
ends. After stop-tran the card may answer up to one idle byte before it
asserts busy (N_BR, §7.5), so a host that polls for "not busy" on the very
next byte can mistake that idle byte for completion. The write timeout the
specification gives (§4.6.2.2) is 250 ms for SDHC, with later revisions
quoting 500 ms for SDXC - confirm against the revision in hand; the driver
budgets 1 s for every programming wait, which covers either.

**Implementation.** The driver waits (up to 1 s) after every accepted block,
clocks one `0xFF` after `0xFD` before polling, and waits again before
releasing chip select; a wait that expires is `BUSY_TIMEOUT`, never `OK`.

**Tests.** `test_programming_busy_is_awaited_between_blocks_and_before_release`
asserts from the trace that every token after the first follows a busy end
and that chip select rises after the last one;
`test_stop_tran_busy_that_starts_one_byte_late_is_still_awaited` uses
`sd_card_set_stop_tran_busy(1, ...)` for the N_BR case; the two
`*_beyond_the_budget_is_a_busy_timeout` cases pin the overrun result and its
timing. Mutations `stop-tran-nbr-gap-dropped`, `final-busy-wait-ignored`,
`inter-block-busy-wait-ignored`, `stop-tran-dropped`.

## P-16 — the pre-command ready wait is a driver convention, not a specification value

**Rule.** The specification bounds operations: read access (N_AC), programming
(§4.6.2.2, 250 ms for SDHC), erase. It does not define how long a host should
wait for DataOut to return high before sending its *next* command; that wait
exists to absorb whatever busy the previous operation left behind, and its
length is the host's choice. ChaN's reference driver uses 500 ms for it.

**Implementation.** Two budgets, both the driver's own. `sd_spi_command()`
waits up to **500 ms** for a ready card before every frame it sends
(`sd_spi_wait_ready_timeout(sd, 500)`). Everything else that waits for a
ready card without sending a command - teardown, the wait after CMD12's R1b in
`sd_spi_stop_transmission()`, the read path's R1-error cleanup - goes through
`sd_spi_wait_ready()` at **250 ms**. Programming waits in the write path are
a separate 1 s budget (P-15). History: the generic wait was 1 s until
`4629e1d` cut it to 250 ms; the pre-command wait was split out at 500 ms on
2026-09-11, and the tests that had encoded 1 s were updated at the same time.

**Consequence worth knowing.** The pre-command wait is the safety net for a
busy the driver did *not* wait out itself - after a write's `BUSY_TIMEOUT`,
the next command has 500 ms to absorb the rest of that programming. A card
that needs longer than 1.5 s in total fails twice.

**Tests.** Each wait is pinned from both sides (`>= budget` and
`< 2 x budget`) so the two cannot be confused: `test_initialization_reports_busy_timeout`
and `test_read_command_reports_busy_timeout` (pre-command);
`test_deinit_timeout_preserves_resources`,
`test_stop_transmission_reports_busy_timeout`,
`test_error_cleanup_reports_busy_timeout` (generic), all in `test_sd_spi.c`;
`test_busy_at_each_phase_is_bounded` (one row of each) and
`test_deinit_busy_timeout_can_be_retried` in `test_sd_faults.c`. The budgets
are named `DRIVER_COMMAND_READY_WAIT_US` and `DRIVER_READY_WAIT_US` in both
suites and `COMMAND_READY_WAIT_US` in `test_sd_writes.c`; change them together
with the driver.

## Discrepancies found between documentation and code

- The root `README.md` states that "Storage and filesystem entry points
  explicitly return not-implemented or not-initialized results; no card protocol
  or fake filesystem behavior exists." The SD SPI driver has been implemented
  since. Corrected as part of this work.
- `docs/architecture.md` requires that "a transaction interrupted by even an
  unconfirmed removal edge must fail and must not resume" and that teardown
  happen "exactly once". The implementation violated both; see
  [KNOWN_GAPS.md](KNOWN_GAPS.md) SD-001 and SD-002, now fixed with enabled
  regressions.
- `docs/validation.md` contained only templates. A validation record covering
  the host evidence has been added.
