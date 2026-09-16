/*
 * A stateful SD-over-SPI card model.
 *
 * WHY THIS EXISTS
 * ---------------
 * The previous fake answered a command by pushing an opaque byte string into a
 * queue. That is enough to replay a happy path, but it cannot represent most of
 * what a real card legitimately does, and it silently accepted several things a
 * real card would reject. Concretely, the old fake could not express:
 *
 *   - a command frame with a wrong CRC7 (it never looked at the CRC byte, so a
 *     driver that sent the wrong CMD0/CMD8 CRC passed every test),
 *   - ACMD41 arriving without its CMD55 prefix (commands[41] answered either
 *     way, so dropping CMD55 was undetectable),
 *   - a multiple-block stream longer than the 4096-byte payload buffer,
 *     i.e. more than about seven blocks,
 *   - the card continuing to stream the *next* block while the host clocks out
 *     CMD12 - the documented real-world race this harness now reproduces,
 *   - busy (R1b) periods measured in time rather than in scripted 0x00 bytes,
 *   - failures injected at a named point inside a transaction,
 *   - any write transaction at all.
 *
 * WHAT IT MODELS
 * --------------
 * A byte-level state machine over the SPI bus: command framing and CRC7, the
 * N_CR response window, R1/R1b/R3/R7, read data tokens, payloads and CRC,
 * multiple-block streaming and CMD12 termination, programming busy, and the
 * write path (CMD24/CMD25 data tokens, data-response tokens, stop-tran). Card
 * registers (OCR/CSD) are synthesised from a card description so a test asks
 * for "a 32 GB SDHC card" rather than hand-assembling CSD bits.
 *
 * WHAT IT IS NOT
 * --------------
 * It is not a card. It has no electrical behaviour, no real timing variation,
 * no wear levelling, no internal ECC, no lock/unlock, no SDIO and no CID/SCR
 * detail beyond what the driver reads. See RESIDUAL_RISK.md.
 */
#ifndef TAVERNKEEP_TEST_SD_CARD_MODEL_H
#define TAVERNKEEP_TEST_SD_CARD_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    SD_MODEL_BLOCK_SIZE = 512,
    SD_MODEL_MAX_FAULTS = 8,
    SD_MODEL_TRACE_CAPACITY = 4096,
    /* Blocks the overlay can hold. A multiple-block write stores one entry per
     * block, so this bounds the longest write whose every block a test can
     * read back out of the model. 64 covers the main.c demo's 51 written
     * blocks with room to spare; a write past the overlay is dropped while
     * the card still answers "accepted", so keep this above any demo's
     * footprint. */
    SD_MODEL_OVERLAY_BLOCKS = 64,
    SD_MODEL_SCRIPT_COMMANDS = 64,
    SD_MODEL_SCRIPT_PAYLOAD = 4096,
};

/* ------------------------------------------------------------------ cards */

typedef enum {
    SD_CARD_V1_SDSC = 0, /* CMD8 illegal (R1 0x05), CSD v1, byte addressing */
    SD_CARD_V2_SDSC,     /* CMD8 answers R7, CCS=0, CSD v1, byte addressing */
    SD_CARD_SDHC,        /* CCS=1, CSD v2, block addressing */
    SD_CARD_SDXC,        /* CCS=1, CSD v2, block addressing, >32GB */
    SD_CARD_SDUC,        /* CCS=1, CSD v3 - driver is required to refuse it */
    SD_CARD_MMC_LIKE,    /* CMD8 illegal and ACMD41 unsupported */
    SD_CARD_KIND_COUNT
} sd_card_kind_t;

typedef struct {
    sd_card_kind_t kind;
    uint64_t capacity_bytes;
    uint8_t read_bl_len;    /* CSD v1 only; 9..11 */
    /* Filler bytes the card sends before R1, so R1 lands on the host's
     * (ncr_bytes + 1)-th read after the command frame. The specification's
     * N_CR window for an SD card in SPI mode is 0 to 8 bytes; see PROTOCOL.md
     * finding P-04 for how that lines up with the driver's poll limit. */
    uint32_t ncr_bytes;
    uint32_t read_access_us;/* N_AC: delay before a read data token */
    uint32_t program_us;    /* busy after a write or an erase */
    uint32_t acmd41_ready_after; /* ACMD41 answers idle this many times first */
    bool crc_check_enabled; /* CMD59 state. CMD0/CMD8 are always checked. */
    /* After the host has taken N blocks of a CMD18 stream the card keeps
     * streaming until CMD12. Set false only to model a card that stops on its
     * own, which real cards do not do. */
    bool stream_until_stop;
} sd_card_desc_t;

/* Convenience descriptions. Capacity is rounded to what the CSD can encode. */
sd_card_desc_t sd_card_desc(sd_card_kind_t kind, uint64_t capacity_bytes);

/* --------------------------------------------------------------- faults */

typedef enum {
    SD_PHASE_NONE = 0,
    SD_PHASE_COMMAND,        /* the six command-frame bytes */
    SD_PHASE_RESPONSE_WAIT,  /* the N_CR window before R1 */
    SD_PHASE_R1,             /* the R1 byte itself */
    SD_PHASE_TRAILER,        /* R3 / R7 trailing bytes */
    SD_PHASE_DATA_WAIT,      /* polling for a data token */
    SD_PHASE_DATA_TOKEN,     /* the 0xFE byte */
    SD_PHASE_DATA_PAYLOAD,   /* inside a data payload */
    SD_PHASE_DATA_CRC,       /* the two data CRC bytes */
    SD_PHASE_BETWEEN_BLOCKS, /* after a block's CRC, before the next token */
    SD_PHASE_STOP_STUFF,     /* the CMD12 stuff byte */
    SD_PHASE_BUSY,           /* an R1b busy window */
    SD_PHASE_WRITE_TOKEN,    /* host-sent 0xFE / 0xFC / 0xFD */
    SD_PHASE_WRITE_PAYLOAD,  /* the 512 data bytes and the two CRC bytes */
    SD_PHASE_WRITE_RESPONSE, /* the data-response token */
    /* The N_BR window: idle bytes a card may answer after the stop-tran token
     * before it asserts busy. Only entered when a test asks for one via
     * sd_card_set_stop_tran_busy(); programming busy itself is SD_PHASE_BUSY. */
    SD_PHASE_WRITE_BUSY,
    SD_PHASE_RELEASE,        /* bytes clocked while chip select is high */
    SD_PHASE_COUNT
} sd_phase_t;

typedef enum {
    SD_FAULT_NONE = 0,
    SD_FAULT_STALL,            /* answer 0xFF from here on */
    SD_FAULT_BUSY_FOREVER,     /* answer 0x00 from here on */
    SD_FAULT_DATA_ERROR_TOKEN, /* emit param as a data error token */
    SD_FAULT_SUBSTITUTE_R1,    /* answer param instead of the real R1 */
    SD_FAULT_GARBAGE,          /* answer param instead of this byte */
    SD_FAULT_FLIP_BITS,        /* XOR param into this byte */
    SD_FAULT_TRUNCATE,         /* end the payload here and go idle */
    SD_FAULT_BAD_DATA_CRC,     /* corrupt the trailing data CRC */
    SD_FAULT_EJECT,            /* raise the card-detect line at this byte */
    SD_FAULT_EXTRA_BUSY_US,    /* insert param microseconds of busy */
    /* From this byte on the card answers pseudo-random bytes seeded with
     * param. Used to fuzz the driver's response parsing: R1, tokens, payload
     * and CRC all become arbitrary, so the driver must still terminate,
     * release the bus and return a legal result. */
    SD_FAULT_RANDOM_STREAM,
} sd_fault_kind_t;

enum { SD_ANY_COMMAND = 0xFFU };

typedef struct {
    sd_phase_t phase;
    uint8_t command;      /* SD_ANY_COMMAND matches every command */
    /* Which entry into this phase, 0-based. Counted within the named command
     * when `command` is not SD_ANY_COMMAND, so "the second block of this read"
     * is occurrence 1 regardless of what ran before. */
    uint32_t occurrence;
    uint32_t byte_offset; /* which byte within the phase, 0-based */
    sd_fault_kind_t kind;
    uint32_t param;
} sd_fault_t;

/* --------------------------------------------------------------- tracing */

typedef enum {
    SD_EV_CHIP_SELECT = 0, /* value: 1 released, 0 asserted */
    SD_EV_COMMAND,         /* command, argument, crc_ok, app_command */
    SD_EV_R1,              /* value = R1 */
    SD_EV_TRAILER,         /* value = byte, byte_offset within the trailer */
    SD_EV_DATA_TOKEN,      /* value = 0xFE */
    SD_EV_ERROR_TOKEN,     /* value = the error token */
    SD_EV_BLOCK_SENT,      /* argument = the block address just streamed */
    SD_EV_BUSY_BEGIN,
    SD_EV_BUSY_END,
    SD_EV_STOP_ACCEPTED,   /* CMD12 terminated an active stream */
    SD_EV_WRITE_TOKEN,     /* value = the token the host sent */
    SD_EV_BLOCK_WRITTEN,   /* argument = the block address written */
    SD_EV_WRITE_RESPONSE,  /* value = the data-response token */
    SD_EV_STOP_TRAN,       /* the host sent 0xFD */
    SD_EV_PROTOCOL_ERROR,  /* the model rejected what the host did */
    SD_EV_KIND_COUNT
} sd_event_kind_t;

typedef struct {
    sd_event_kind_t kind;
    uint8_t command;
    uint32_t argument;
    uint8_t value;
    uint32_t byte_offset;
    bool crc_ok;
    bool app_command;
    uint64_t at_us;
    uint64_t byte_index; /* global count of bytes clocked on the bus */
} sd_event_t;

/* -------------------------------------------------------------- lifecycle */

void sd_card_reset(const sd_card_desc_t *desc);
const sd_card_desc_t *sd_card_description(void);

/* Chip select. The model answers 0xFF and forgets partial state while the
 * card is deselected, which is what a real card does. */
void sd_card_set_chip_select(bool released);
bool sd_card_chip_select_released(void);

/* Return the card to a quiescent, responsive state: end any permanent busy or
 * stall a fault put it in, without disturbing the driver, the trace or the
 * clock. Models "the card recovered", so a test can ask the separate question
 * of whether the *driver* is still usable after a card-side failure. */
void sd_card_resume(void);

/* One byte on the bus. Returns MISO. */
uint8_t sd_card_transfer(uint8_t mosi);

/* --------------------------------------------------------------- content */

/* Block contents default to a deterministic pattern so an arbitrarily long
 * multiple-block read needs no scripted buffer. */
void sd_card_set_content_seed(uint32_t seed);
void sd_card_fill_expected_block(uint64_t lba, uint8_t *out);
/* Override a specific block; also used to observe what a write stored. */
bool sd_card_set_block(uint64_t lba, const uint8_t *data);
bool sd_card_get_block(uint64_t lba, uint8_t *out);

uint64_t sd_card_block_count(void);
/* Answer CMD9 with this exact register instead of a synthesised one. Used to
 * drive the driver's CSD parser with register dumps captured from real cards,
 * which is the only independent oracle available on the host: the model's own
 * encoder and the driver's decoder could otherwise share a mistake. */
void sd_card_set_raw_csd(const uint8_t csd[16]);
void sd_card_clear_raw_csd(void);
/* The CSD the model will return, so a test can compare byte-for-byte against
 * a known-good register dump instead of trusting the model. */
void sd_card_csd(uint8_t out[16]);
void sd_card_ocr(uint8_t out[4]);

/* ---------------------------------------------------------------- faults */

void sd_card_clear_faults(void);
bool sd_card_add_fault(const sd_fault_t *fault);
/* How many times a fault has actually fired. A test that injects a fault and
 * sees zero activations is testing nothing. */
uint32_t sd_card_fault_activations(size_t index);
/* The bus byte count (as sd_card_bytes_clocked() reports it) at which the
 * fault last fired, so a test can bound how much further the driver kept
 * clocking after, say, the card was ejected. Zero if it never fired. */
uint64_t sd_card_fault_activation_byte(size_t index);

/* How many times the model has entered a phase so far. Pass this as a fault's
 * `occurrence` to mean "the next time this happens", which is far more robust
 * than counting entries by hand when bring-up also visits the phase. */
uint32_t sd_card_phase_entries(sd_phase_t phase);

/* Set by SD_FAULT_EJECT; the SPI fake polls this to raise card detect. */
bool sd_card_eject_requested(void);
void sd_card_clear_eject_request(void);

/* ------------------------------------------------------- response policy */

typedef enum {
    /* Commands that have no script answer with a single 0xFF, exactly as the
     * original fake did. Used by pico_mock_reset() so the fixtures written
     * against the old engine keep meaning what they meant. */
    SD_RESPONSE_SCRIPTED_ONLY = 0,
    /* Commands that have no script are answered by the card state machine.
     * New tests use this: the card behaves like a card unless a test
     * deliberately overrides one command. */
    SD_RESPONSE_MODELLED,
} sd_response_policy_t;

void sd_card_set_response_policy(sd_response_policy_t policy);
sd_response_policy_t sd_card_response_policy(void);

/* ------------------------------------------------- legacy scripted mode */

/* Answer one command from an opaque byte string instead of from card state.
 * Retained so the original suite keeps running against the new engine.
 * Framing, CRC, app-command state, chip select, faults and tracing still
 * apply. Prefer the modelled path for new tests. */
bool sd_card_script_command(
    uint8_t command,
    uint8_t r1,
    const uint8_t *payload,
    size_t payload_size);
bool sd_card_script_response_delay(uint8_t command, size_t bytes);
bool sd_card_script_idle_responses(uint8_t command, size_t responses);
/* After the scripted payload runs out, stay busy instead of going idle. */
bool sd_card_script_busy_after_payload(uint8_t command, bool busy);
/* Answer R1 and then never send a data token. Expresses "the card never
 * produced data" without scripting a timeout's worth of 0xFF bytes. */
bool sd_card_script_stall_after_r1(uint8_t command, bool stall);
void sd_card_script_clear(void);
bool sd_card_command_is_scripted(uint8_t command);

size_t sd_card_command_count(uint8_t command);
uint32_t sd_card_last_argument(uint8_t command);
size_t sd_card_pending_scripted_bytes(void);

/* Global busy: the card answers 0x00 for this many bytes, or forever. */
void sd_card_set_busy_bytes(size_t bytes);
void sd_card_set_busy_forever(void);
#define SD_BUSY_FOREVER ((size_t)-1)

/* --------------------------------------------------------------- tracing */

size_t sd_card_trace_length(void);
const sd_event_t *sd_card_trace_at(size_t index);
size_t sd_card_trace_count(sd_event_kind_t kind);
const sd_event_t *sd_card_trace_nth(sd_event_kind_t kind, size_t n);
bool sd_card_trace_overflowed(void);
/* Command indexes in order, for exact-sequence assertions. Returns how many
 * were written. */
size_t sd_card_trace_command_sequence(uint8_t *out, size_t capacity);
void sd_card_trace_dump(FILE *stream, size_t max_events);

/* Bytes the card keeps streaming after it decodes the CMD12 start byte,
 * before it answers. Zero (the default) is the idealised card the original
 * fixtures assume. A non-zero value reproduces the documented real-world race
 * in which residual read data is still on the bus while the host is already
 * looking for CMD12's R1. See PROTOCOL.md finding P-07. */
void sd_card_set_stop_residual_bytes(size_t bytes);
size_t sd_card_stop_residual_bytes(void);

/* Data-response token the model returns after a written block.
 * 0x05 accepted, 0x0B CRC error, 0x0D write error. A block whose CRC16 the
 * card rejected answers 0x0B regardless of what was set here. */
void sd_card_set_write_response_token(uint8_t token);

/* The card validates the CRC16 the host appends to every written block and,
 * on a mismatch, answers 0x0B and stores nothing. This is on by default
 * because real cards always check the data CRC in SPI mode; turn it off only
 * to isolate a test from the write CRC deliberately. */
void sd_card_set_write_crc_check(bool enabled);

/* What the card does after the host's stop-tran token (0xFD). By default it
 * asserts busy immediately for the description's program_us, which is the
 * idealised card. The specification's N_BR allows a card up to one byte of
 * idle (0xFF) before busy begins, and a host that polls for "not busy" right
 * after the token can read that idle byte, conclude programming is over, and
 * release the card mid-program. `delay_bytes` inserts that window; `program_us`
 * is how long the final programming busy lasts, independent of the per-block
 * value, so a test can make only the stop-tran busy exceed the driver's
 * budget. Reset by sd_card_reset(). */
void sd_card_set_stop_tran_busy(size_t delay_bytes, uint64_t program_us);

/* Blocks the card has actually streamed for the current/last read. Lets a
 * test prove the driver consumed exactly the blocks it asked for. */
uint64_t sd_card_stream_blocks_sent(void);

uint64_t sd_card_bytes_clocked(void);
uint32_t sd_card_protocol_errors(void);
const char *sd_card_last_protocol_error(void);

/* CRC7 over a command frame's first five bytes; exposed so tests can build
 * and check frames without duplicating the driver's constants. */
uint8_t sd_crc7(const uint8_t *data, size_t length);
uint16_t sd_crc16_ccitt(const uint8_t *data, size_t length);

const char *sd_phase_name(sd_phase_t phase);
const char *sd_event_kind_name(sd_event_kind_t kind);

#endif
