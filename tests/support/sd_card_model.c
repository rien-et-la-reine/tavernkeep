#include "sd_card_model.h"

#include <stdlib.h>
#include <string.h>

#include "sim_clock.h"

/* ------------------------------------------------------------------ CRC */

uint8_t sd_crc7(const uint8_t *data, size_t length)
{
    uint8_t crc = 0U;
    for (size_t i = 0U; i < length; ++i) {
        uint8_t byte = data[i];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const unsigned int input = ((unsigned int)byte >> 7U) & 1U;
            const unsigned int top = ((unsigned int)crc >> 6U) & 1U;
            crc = (uint8_t)(((unsigned int)crc << 1U) & 0x7FU);
            if ((input ^ top) != 0U) {
                crc = (uint8_t)(crc ^ 0x09U);
            }
            byte = (uint8_t)((unsigned int)byte << 1U);
        }
    }
    return (uint8_t)(crc & 0x7FU);
}

uint16_t sd_crc16_ccitt(const uint8_t *data, size_t length)
{
    uint16_t crc = 0U;
    for (size_t i = 0U; i < length; ++i) {
        crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8U));
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const uint32_t shifted = (uint32_t)crc << 1U;
            crc = (uint16_t)((crc & 0x8000U) != 0U
                ? (shifted ^ 0x1021U)
                : shifted);
        }
    }
    return crc;
}

/* --------------------------------------------------------------- state */

typedef enum {
    ST_IDLE = 0,
    ST_NCR,
    ST_R1,
    ST_TRAILER,
    ST_BUSY,
    ST_READ_WAIT,
    ST_READ_TOKEN,
    ST_READ_PAYLOAD,
    ST_READ_CRC,
    ST_STREAM_GAP,
    ST_STOPPING,
    ST_SCRIPT,
    ST_STALL,
    ST_WRITE_WAIT_TOKEN,
    ST_WRITE_PAYLOAD,
    ST_WRITE_CRC,
    ST_WRITE_RESPONSE,
    ST_STOP_TRAN_GAP, /* the N_BR idle bytes between stop-tran and busy */
} model_state_t;

typedef struct {
    bool configured;
    uint8_t r1;
    uint8_t payload[SD_MODEL_SCRIPT_PAYLOAD];
    size_t payload_size;
    size_t response_delay;
    size_t idle_responses;
    bool busy_after_payload;
    bool stall_after_r1;
    size_t count;
    uint32_t last_argument;
} script_command_t;

typedef struct {
    bool used;
    uint64_t lba;
    uint8_t data[SD_MODEL_BLOCK_SIZE];
} overlay_block_t;

static sd_card_desc_t card;
static model_state_t state;
static bool cs_released = true;

static uint8_t frame[6];
static size_t frame_length;
static bool frame_active;

static uint8_t active_command;
static bool app_command_pending;
static bool card_idle;
static uint8_t pending_r1;
static uint8_t trailer[4];
static size_t trailer_length;
static size_t trailer_index;
static size_t ncr_remaining;

static uint8_t data_buffer[SD_MODEL_BLOCK_SIZE];
static size_t data_length;
static size_t data_index;
static uint16_t data_crc;
static size_t data_crc_index;
static uint8_t data_token_value;
static uint64_t stream_lba;
static uint64_t stream_blocks_sent;
static bool stream_active;
static uint64_t read_ready_at_us;

/* CMD12 termination */
static size_t stop_residual_bytes;
static size_t stop_byte_index;   /* bytes since the CMD12 start byte */
static bool stop_stuff_sent;

static uint64_t write_lba;
static bool write_multiple;
static uint8_t write_response_token;
/* The CRC16 the host appended to the block it just sent, and whether the
 * model checks it. A real card always validates the data CRC on a write -
 * unlike the command CRC, which CMD59 gates - and answers data-response token
 * 0x0B without storing the block when it does not match. Modelled so that a
 * driver sending a wrong write CRC is caught rather than silently accepted. */
static uint16_t host_write_crc;
static bool write_crc_check_enabled = true;
static bool write_crc_rejected;
/* The specification requires at least one idle byte (N_WR) between a write
 * command's R1 and the first start-block token. Set when R1 has just been
 * answered so the very next byte can be checked. */
static bool write_gap_required;
/* A multiple-block write is still in progress when the per-block programming
 * busy ends: the card goes back to waiting for a token, not to idle. */
static bool resume_write_after_busy;
/* Stop-tran behaviour; see sd_card_set_stop_tran_busy(). */
static size_t stop_tran_delay_bytes;
static size_t stop_tran_delay_remaining;
static uint64_t stop_tran_program_us;
static bool stop_tran_busy_overridden;

static uint64_t busy_until_us;
static size_t global_busy_bytes;

static script_command_t scripts[SD_MODEL_SCRIPT_COMMANDS];
static size_t script_index;
static size_t script_length;
static size_t script_r1_position;
static uint8_t script_queue[SD_MODEL_SCRIPT_PAYLOAD + 32];
static bool script_busy_tail;
static bool script_stall_tail;

static sd_response_policy_t response_policy;
static uint32_t content_seed = 0x5A5AU;
static overlay_block_t overlay[SD_MODEL_OVERLAY_BLOCKS];

static sd_fault_t faults[SD_MODEL_MAX_FAULTS];
static uint32_t fault_activations[SD_MODEL_MAX_FAULTS];
static uint64_t fault_activation_bytes[SD_MODEL_MAX_FAULTS];
static size_t fault_count;
static uint32_t phase_entries[SD_PHASE_COUNT];
/* Entries into a phase while a given command is active. A fault that names a
 * command counts occurrences within that command, which is what a test means
 * by "fail on the second block of this read" - a global counter would make the
 * occurrence index depend on how many unrelated commands ran first. */
static uint32_t phase_command_entries[SD_PHASE_COUNT][SD_MODEL_SCRIPT_COMMANDS];
static uint32_t phase_byte_index;
static sd_phase_t current_phase;
static bool state_overridden;
static bool random_stream_active;
static uint32_t random_stream_state;
static bool eject_requested;

static sd_event_t trace[SD_MODEL_TRACE_CAPACITY];
static size_t trace_length;
static bool trace_overflow;
static uint64_t bytes_clocked;
static uint32_t protocol_errors;
static char last_protocol_error[96];

/* ---------------------------------------------------------------- names */

static const char *const phase_names[SD_PHASE_COUNT] = {
    "none", "command", "response-wait", "r1", "trailer", "data-wait",
    "data-token", "data-payload", "data-crc", "between-blocks",
    "stop-stuff", "busy", "write-token", "write-payload",
    "write-response", "write-busy", "release",
};

const char *sd_phase_name(sd_phase_t phase)
{
    return phase < SD_PHASE_COUNT ? phase_names[phase] : "?";
}

static const char *const event_names[SD_EV_KIND_COUNT] = {
    "chip-select", "command", "r1", "trailer", "data-token", "error-token",
    "block-sent", "busy-begin", "busy-end", "stop-accepted", "write-token",
    "block-written", "write-response", "stop-tran", "protocol-error",
};

const char *sd_event_kind_name(sd_event_kind_t kind)
{
    return kind < SD_EV_KIND_COUNT ? event_names[kind] : "?";
}

/* --------------------------------------------------------------- tracing */

static void trace_push(const sd_event_t *event)
{
    if (trace_length < SD_MODEL_TRACE_CAPACITY) {
        trace[trace_length++] = *event;
    } else {
        trace_overflow = true;
    }
}

static void trace_simple(sd_event_kind_t kind, uint8_t value, uint32_t argument)
{
    sd_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.command = active_command;
    event.value = value;
    event.argument = argument;
    event.at_us = sim_clock_now_us();
    event.byte_index = bytes_clocked;
    trace_push(&event);
}

static void note_protocol_error(const char *text)
{
    protocol_errors++;
    (void)snprintf(last_protocol_error, sizeof(last_protocol_error), "%s", text);
    trace_simple(SD_EV_PROTOCOL_ERROR, 0U, 0U);
}

/* --------------------------------------------------------------- content */

static void generate_block(uint64_t lba, uint8_t *out)
{
    uint32_t x = (uint32_t)(lba * UINT32_C(2654435761)) ^ content_seed;
    if (x == 0U) {
        x = 0x9E3779B9U;
    }
    for (size_t i = 0U; i < SD_MODEL_BLOCK_SIZE; ++i) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        out[i] = (uint8_t)(x >> 3U);
    }
}

void sd_card_set_content_seed(uint32_t seed)
{
    content_seed = seed;
}

void sd_card_fill_expected_block(uint64_t lba, uint8_t *out)
{
    for (size_t i = 0U; i < SD_MODEL_OVERLAY_BLOCKS; ++i) {
        if (overlay[i].used && overlay[i].lba == lba) {
            memcpy(out, overlay[i].data, SD_MODEL_BLOCK_SIZE);
            return;
        }
    }
    generate_block(lba, out);
}

bool sd_card_set_block(uint64_t lba, const uint8_t *data)
{
    for (size_t i = 0U; i < SD_MODEL_OVERLAY_BLOCKS; ++i) {
        if (overlay[i].used && overlay[i].lba == lba) {
            memcpy(overlay[i].data, data, SD_MODEL_BLOCK_SIZE);
            return true;
        }
    }
    for (size_t i = 0U; i < SD_MODEL_OVERLAY_BLOCKS; ++i) {
        if (!overlay[i].used) {
            overlay[i].used = true;
            overlay[i].lba = lba;
            memcpy(overlay[i].data, data, SD_MODEL_BLOCK_SIZE);
            return true;
        }
    }
    return false;
}

bool sd_card_get_block(uint64_t lba, uint8_t *out)
{
    sd_card_fill_expected_block(lba, out);
    return true;
}

/* -------------------------------------------------------------- registers */

static bool card_is_high_capacity(void)
{
    return card.kind == SD_CARD_SDHC || card.kind == SD_CARD_SDXC
        || card.kind == SD_CARD_SDUC;
}

static bool card_supports_cmd8(void)
{
    return card.kind != SD_CARD_V1_SDSC && card.kind != SD_CARD_MMC_LIKE;
}

uint64_t sd_card_block_count(void)
{
    return card.capacity_bytes / SD_MODEL_BLOCK_SIZE;
}

static bool raw_csd_valid;
static uint8_t raw_csd[16];

void sd_card_set_raw_csd(const uint8_t csd[16])
{
    memcpy(raw_csd, csd, 16U);
    raw_csd_valid = true;
}

void sd_card_clear_raw_csd(void)
{
    raw_csd_valid = false;
}

static void build_csd(uint8_t out[16])
{
    if (raw_csd_valid) {
        memcpy(out, raw_csd, 16U);
        return;
    }
    memset(out, 0, 16);
    if (card.kind == SD_CARD_SDUC) {
        out[0] = 0x80U; /* CSD_STRUCTURE = 2 */
        const uint64_t units = card.capacity_bytes / (UINT64_C(512) * 1024U);
        const uint64_t c_size = units == 0U ? 0U : units - 1U;
        out[6] = (uint8_t)((c_size >> 24U) & 0x0FU);
        out[7] = (uint8_t)(c_size >> 16U);
        out[8] = (uint8_t)(c_size >> 8U);
        out[9] = (uint8_t)c_size;
    } else if (card_is_high_capacity()) {
        if ((card.capacity_bytes % (UINT64_C(512) * 1024U)) != 0U
                || card.capacity_bytes
                    > (UINT64_C(0x400000) * 512U * 1024U)) {
            (void)fprintf(stderr,
                "sd_card_model: capacity %llu bytes is not encodable in a CSD "
                "v2 register (needs a multiple of 512 KiB, at most 2 TiB)\n",
                (unsigned long long)card.capacity_bytes);
            abort();
        }
        out[0] = 0x40U; /* CSD_STRUCTURE = 1 */
        out[1] = 0x0EU;
        out[3] = 0x32U;
        out[5] = 0x59U; /* READ_BL_LEN = 9, fixed for CSD v2 */
        const uint64_t units = card.capacity_bytes / (UINT64_C(512) * 1024U);
        const uint32_t c_size = (uint32_t)(units == 0U ? 0U : units - 1U);
        out[7] = (uint8_t)((c_size >> 16U) & 0x3FU);
        out[8] = (uint8_t)(c_size >> 8U);
        out[9] = (uint8_t)c_size;
        out[10] = 0x7FU;
        out[11] = 0x80U;
        out[12] = 0x0AU;
        out[13] = 0x40U;
    } else {
        const uint8_t read_bl_len = card.read_bl_len;
        out[0] = 0x00U; /* CSD_STRUCTURE = 0 */
        out[1] = 0x26U;
        out[3] = 0x32U;
        out[5] = (uint8_t)(0x50U | (read_bl_len & 0x0FU));
        const uint64_t units = card.capacity_bytes >> read_bl_len;
        uint32_t c_size = 0U;
        uint8_t c_size_mult = 0U;
        bool encodable = false;
        for (uint8_t mult = 0U; mult <= 7U; ++mult) {
            const uint64_t divisor = UINT64_C(1) << (mult + 2U);
            if (units == 0U || (units % divisor) != 0U) {
                continue;
            }
            const uint64_t candidate = units / divisor;
            if (candidate >= 1U && candidate <= 4096U) {
                c_size = (uint32_t)(candidate - 1U);
                c_size_mult = mult;
                encodable = true;
                break;
            }
        }
        /* C_SIZE zero is the smallest legal card, not a failed search, so the
         * guard tracks whether a solution was found rather than inspecting
         * the value. */
        if (!encodable) {
            /* A harness that silently emits a nonsense CSD would let a test
             * pass against a card that cannot exist. Fail loudly instead. */
            (void)fprintf(stderr,
                "sd_card_model: capacity %llu bytes is not encodable in a CSD "
                "v1 register with READ_BL_LEN %u; pick a different capacity or "
                "read_bl_len\n",
                (unsigned long long)card.capacity_bytes,
                (unsigned)read_bl_len);
            abort();
        }
        out[6] = (uint8_t)((c_size >> 10U) & 0x03U);
        out[7] = (uint8_t)(c_size >> 2U);
        out[8] = (uint8_t)((c_size & 0x03U) << 6U);
        out[9] = (uint8_t)((c_size_mult >> 1U) & 0x03U);
        out[10] = (uint8_t)(((c_size_mult & 0x01U) << 7U) | 0x1FU);
        out[11] = 0xF9U;
        out[12] = 0x46U;
    }
    out[15] = (uint8_t)(((unsigned)sd_crc7(out, 15U) << 1U) | 1U);
}

void sd_card_csd(uint8_t out[16])
{
    build_csd(out);
}

void sd_card_ocr(uint8_t out[4])
{
    out[0] = (uint8_t)(0x80U | (card_is_high_capacity() ? 0x40U : 0x00U));
    out[1] = 0xFFU;
    out[2] = 0x80U;
    out[3] = 0x00U;
    if (card_idle) {
        out[0] = (uint8_t)(out[0] & 0x7FU);
    }
}

sd_card_desc_t sd_card_desc(sd_card_kind_t kind, uint64_t capacity_bytes)
{
    sd_card_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.kind = kind;
    desc.capacity_bytes = capacity_bytes;
    desc.read_bl_len = 9U;
    desc.ncr_bytes = 1U;
    desc.stream_until_stop = true;
    return desc;
}

/* ---------------------------------------------------------------- faults */

void sd_card_clear_faults(void)
{
    memset(faults, 0, sizeof(faults));
    memset(fault_activations, 0, sizeof(fault_activations));
    memset(fault_activation_bytes, 0, sizeof(fault_activation_bytes));
    fault_count = 0U;
}

bool sd_card_add_fault(const sd_fault_t *fault)
{
    if (fault == NULL || fault_count >= SD_MODEL_MAX_FAULTS
            || fault->phase >= SD_PHASE_COUNT) {
        return false;
    }
    faults[fault_count] = *fault;
    fault_activations[fault_count] = 0U;
    fault_activation_bytes[fault_count] = 0U;
    fault_count++;
    return true;
}

uint32_t sd_card_fault_activations(size_t index)
{
    return index < SD_MODEL_MAX_FAULTS ? fault_activations[index] : 0U;
}

uint64_t sd_card_fault_activation_byte(size_t index)
{
    return index < SD_MODEL_MAX_FAULTS ? fault_activation_bytes[index] : 0U;
}

uint32_t sd_card_phase_entries(sd_phase_t phase)
{
    return phase < SD_PHASE_COUNT ? phase_entries[phase] : 0U;
}

bool sd_card_eject_requested(void)
{
    return eject_requested;
}

void sd_card_clear_eject_request(void)
{
    eject_requested = false;
}

static void enter_phase(sd_phase_t phase)
{
    if (phase != current_phase) {
        current_phase = phase;
        phase_entries[phase]++;
        if (active_command < SD_MODEL_SCRIPT_COMMANDS) {
            phase_command_entries[phase][active_command]++;
        }
        phase_byte_index = 0U;
    }
}

static void begin_busy_us(uint64_t microseconds)
{
    busy_until_us = microseconds == UINT64_MAX
        ? UINT64_MAX
        : sim_clock_now_us() + microseconds;
    state = ST_BUSY;
    trace_simple(SD_EV_BUSY_BEGIN, 0U, 0U);
}

static void go_idle(void)
{
    state = ST_IDLE;
    stream_active = false;
    data_length = 0U;
    resume_write_after_busy = false;
    write_gap_required = false;
}

static uint64_t stop_tran_busy_us(void)
{
    return stop_tran_busy_overridden ? stop_tran_program_us : card.program_us;
}

static int match_fault(void)
{
    for (size_t i = 0U; i < fault_count; ++i) {
        const sd_fault_t *const fault = &faults[i];
        if (fault->kind == SD_FAULT_NONE || fault->phase != current_phase) {
            continue;
        }
        if (fault->command != SD_ANY_COMMAND
                && fault->command != active_command) {
            continue;
        }
        const uint32_t entry = fault->command == SD_ANY_COMMAND
            ? phase_entries[current_phase]
            : (fault->command < SD_MODEL_SCRIPT_COMMANDS
                ? phase_command_entries[current_phase][fault->command]
                : 0U);
        if (entry == 0U || fault->occurrence != entry - 1U) {
            continue;
        }
        if (fault->byte_offset != phase_byte_index) {
            continue;
        }
        fault_activations[i]++;
        fault_activation_bytes[i] = bytes_clocked;
        return (int)i;
    }
    return -1;
}

/* Emit one byte for the given phase, applying any matching fault.
 * Sets state_overridden when the fault took over the state machine, in which
 * case the caller must return immediately without advancing its own state. */
static uint8_t emit(sd_phase_t phase, uint8_t value)
{
    enter_phase(phase);
    state_overridden = false;
    const int index = match_fault();
    if (index < 0) {
        return value;
    }
    const sd_fault_t *const fault = &faults[index];
    switch (fault->kind) {
    case SD_FAULT_STALL:
        state = ST_STALL;
        state_overridden = true;
        return 0xFFU;
    case SD_FAULT_BUSY_FOREVER:
        begin_busy_us(UINT64_MAX);
        state_overridden = true;
        return 0x00U;
    case SD_FAULT_DATA_ERROR_TOKEN:
        return (uint8_t)fault->param;
    case SD_FAULT_SUBSTITUTE_R1:
    case SD_FAULT_GARBAGE:
        return (uint8_t)fault->param;
    case SD_FAULT_FLIP_BITS:
        return (uint8_t)(value ^ (uint8_t)fault->param);
    case SD_FAULT_TRUNCATE:
        go_idle();
        state_overridden = true;
        return 0xFFU;
    case SD_FAULT_BAD_DATA_CRC:
        data_crc = (uint16_t)(data_crc ^ 0xFFFFU);
        return value;
    case SD_FAULT_EJECT:
        eject_requested = true;
        return value;
    case SD_FAULT_EXTRA_BUSY_US:
        begin_busy_us(fault->param);
        state_overridden = true;
        return 0x00U;
    case SD_FAULT_RANDOM_STREAM:
        random_stream_active = true;
        random_stream_state = fault->param | 1U;
        break;
    case SD_FAULT_NONE:
    default:
        break;
    }
    return value;
}

/* ------------------------------------------------------- script handling */

void sd_card_script_clear(void)
{
    memset(scripts, 0, sizeof(scripts));
    script_index = 0U;
    script_length = 0U;
    script_r1_position = 0U;
    script_busy_tail = false;
    script_stall_tail = false;
}

bool sd_card_script_command(
    uint8_t command,
    uint8_t r1,
    const uint8_t *payload,
    size_t payload_size)
{
    if (command >= SD_MODEL_SCRIPT_COMMANDS
            || payload_size > SD_MODEL_SCRIPT_PAYLOAD
            || (payload_size != 0U && payload == NULL)) {
        return false;
    }
    script_command_t *const script = &scripts[command];
    script->configured = true;
    script->r1 = r1;
    script->payload_size = payload_size;
    if (payload_size != 0U) {
        memcpy(script->payload, payload, payload_size);
    }
    return true;
}

bool sd_card_script_response_delay(uint8_t command, size_t bytes)
{
    if (command >= SD_MODEL_SCRIPT_COMMANDS || bytes > 16U) {
        return false;
    }
    scripts[command].response_delay = bytes;
    return true;
}

bool sd_card_script_idle_responses(uint8_t command, size_t responses)
{
    if (command >= SD_MODEL_SCRIPT_COMMANDS) {
        return false;
    }
    scripts[command].idle_responses = responses;
    return true;
}

bool sd_card_script_busy_after_payload(uint8_t command, bool busy)
{
    if (command >= SD_MODEL_SCRIPT_COMMANDS) {
        return false;
    }
    scripts[command].busy_after_payload = busy;
    return true;
}

bool sd_card_script_stall_after_r1(uint8_t command, bool stall)
{
    if (command >= SD_MODEL_SCRIPT_COMMANDS) {
        return false;
    }
    scripts[command].stall_after_r1 = stall;
    return true;
}

bool sd_card_command_is_scripted(uint8_t command)
{
    return command < SD_MODEL_SCRIPT_COMMANDS && scripts[command].configured;
}

size_t sd_card_command_count(uint8_t command)
{
    return command < SD_MODEL_SCRIPT_COMMANDS ? scripts[command].count : 0U;
}

uint32_t sd_card_last_argument(uint8_t command)
{
    return command < SD_MODEL_SCRIPT_COMMANDS
        ? scripts[command].last_argument : 0U;
}

size_t sd_card_pending_scripted_bytes(void)
{
    return script_length - script_index;
}

void sd_card_set_busy_bytes(size_t bytes)
{
    global_busy_bytes = bytes;
}

void sd_card_set_busy_forever(void)
{
    global_busy_bytes = SD_BUSY_FOREVER;
}

static void script_enqueue(uint8_t value)
{
    if (script_length < sizeof(script_queue)) {
        script_queue[script_length++] = value;
    }
}

static void start_scripted_response(uint8_t command)
{
    script_command_t *const script = &scripts[command];
    script_index = 0U;
    script_length = 0U;
    script_busy_tail = script->busy_after_payload;
    script_stall_tail = script->stall_after_r1;

    if (!script->configured) {
        script_r1_position = 0U;
        script_enqueue(0xFFU);
        state = ST_SCRIPT;
        return;
    }
    if (command == 12U) {
        script_enqueue(0x00U); /* CMD12's positional stuff byte */
    }
    for (size_t i = 0U; i < script->response_delay; ++i) {
        script_enqueue(0xFFU);
    }
    script_r1_position = script_length;
    script_enqueue(script->idle_responses != 0U ? 0x01U : script->r1);
    if (script->idle_responses != 0U) {
        script->idle_responses--;
    }
    if (!script->stall_after_r1) {
        for (size_t i = 0U; i < script->payload_size; ++i) {
            script_enqueue(script->payload[i]);
        }
    }
    state = ST_SCRIPT;
}

/* --------------------------------------------------- modelled dispatch */

static uint8_t idle_bit(void)
{
    return card_idle ? 0x01U : 0x00U;
}

static void respond_r1(uint8_t r1)
{
    pending_r1 = r1;
    trailer_length = 0U;
    trailer_index = 0U;
    ncr_remaining = card.ncr_bytes;
    state = ST_NCR;
}

static void respond_r1_trailer(uint8_t r1, const uint8_t *bytes, size_t length)
{
    respond_r1(r1);
    trailer_length = length;
    memcpy(trailer, bytes, length);
}

static void load_block_for_stream(void)
{
    sd_card_fill_expected_block(stream_lba, data_buffer);
    data_length = SD_MODEL_BLOCK_SIZE;
    data_index = 0U;
    data_crc = sd_crc16_ccitt(data_buffer, SD_MODEL_BLOCK_SIZE);
    data_crc_index = 0U;
    data_token_value = 0xFEU;
}

static void begin_read(uint64_t lba, bool multiple)
{
    stream_lba = lba;
    stream_blocks_sent = 0U;
    stream_active = multiple;
    read_ready_at_us = sim_clock_now_us() + card.read_access_us;
    load_block_for_stream();
    respond_r1(0x00U);
}

static bool address_to_lba(uint32_t argument, uint64_t *lba, uint8_t *r1_error)
{
    if (card_is_high_capacity()) {
        *lba = argument;
        return true;
    }
    if ((argument % SD_MODEL_BLOCK_SIZE) != 0U) {
        *r1_error = 0x40U; /* parameter error */
        return false;
    }
    *lba = argument / SD_MODEL_BLOCK_SIZE;
    return true;
}

static void dispatch_modelled(uint8_t command, uint32_t argument, bool app)
{
    uint8_t registers[16];
    uint64_t lba = 0U;
    uint8_t r1_error = 0U;

    if (app && command == 41U) {
        if (card.kind == SD_CARD_MMC_LIKE) {
            respond_r1((uint8_t)(0x04U | idle_bit()));
            return;
        }
        const bool hcs = (argument & UINT32_C(0x40000000)) != 0U;
        if (card_is_high_capacity() && !hcs) {
            respond_r1(0x01U); /* never leaves idle without HCS */
            return;
        }
        if (card.acmd41_ready_after > 0U) {
            card.acmd41_ready_after--;
            respond_r1(0x01U);
            return;
        }
        card_idle = false;
        respond_r1(0x00U);
        return;
    }
    if (app) {
        respond_r1((uint8_t)(0x04U | idle_bit()));
        return;
    }

    switch (command) {
    case 0U:
        card_idle = true;
        respond_r1(0x01U);
        return;
    case 8U:
        if (!card_supports_cmd8()) {
            respond_r1((uint8_t)(0x04U | idle_bit()));
            return;
        }
        {
            uint8_t r7[4];
            const uint8_t vhs = (uint8_t)((argument >> 8U) & 0x0FU);
            r7[0] = 0x00U;
            r7[1] = 0x00U;
            r7[2] = (uint8_t)(vhs == 0x01U ? 0x01U : 0x00U);
            r7[3] = (uint8_t)(argument & 0xFFU);
            respond_r1_trailer(idle_bit(), r7, 4U);
        }
        return;
    case 9U:
        build_csd(registers);
        memcpy(data_buffer, registers, 16U);
        data_length = 16U;
        data_index = 0U;
        data_crc = sd_crc16_ccitt(data_buffer, 16U);
        data_crc_index = 0U;
        data_token_value = 0xFEU;
        stream_active = false;
        read_ready_at_us = sim_clock_now_us() + card.read_access_us;
        respond_r1(0x00U);
        return;
    case 12U:
        respond_r1(0x00U); /* a bare CMD12 outside a stream is a no-op */
        return;
    case 16U:
        respond_r1(argument == SD_MODEL_BLOCK_SIZE ? 0x00U : 0x40U);
        return;
    case 17U:
    case 18U:
        if (!address_to_lba(argument, &lba, &r1_error)) {
            respond_r1(r1_error);
            return;
        }
        begin_read(lba, command == 18U);
        if (lba >= sd_card_block_count()) {
            /* A real card reports an out-of-range read through a data error
             * token after a clean R1, not through R1 itself. */
            data_token_value = 0x08U;
        }
        return;
    case 24U:
    case 25U:
        if (!address_to_lba(argument, &lba, &r1_error)) {
            respond_r1(r1_error);
            return;
        }
        if (lba >= sd_card_block_count()) {
            respond_r1(0x40U);
            return;
        }
        write_lba = lba;
        write_multiple = command == 25U;
        respond_r1(0x00U);
        return;
    case 55U:
        app_command_pending = true;
        respond_r1(idle_bit());
        return;
    case 58U:
        sd_card_ocr(registers);
        respond_r1_trailer(idle_bit(), registers, 4U);
        return;
    case 59U:
        card.crc_check_enabled = (argument & 1U) != 0U;
        respond_r1(idle_bit());
        return;
    default:
        respond_r1((uint8_t)(0x04U | idle_bit()));
        return;
    }
}

static void complete_frame(void)
{
    const uint8_t command = (uint8_t)(frame[0] & 0x3FU);
    const uint32_t argument = ((uint32_t)frame[1] << 24U)
        | ((uint32_t)frame[2] << 16U)
        | ((uint32_t)frame[3] << 8U)
        | (uint32_t)frame[4];
    const uint8_t expected_crc =
        (uint8_t)(((unsigned)sd_crc7(frame, 5U) << 1U) | 1U);
    const bool crc_ok = frame[5] == expected_crc;
    const bool app = app_command_pending;
    const bool stopping = state == ST_STOPPING;

    active_command = command;
    app_command_pending = false;
    frame_active = false;
    frame_length = 0U;

    if (command < SD_MODEL_SCRIPT_COMMANDS) {
        scripts[command].count++;
        scripts[command].last_argument = argument;
    }

    sd_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = SD_EV_COMMAND;
    event.command = command;
    event.argument = argument;
    event.value = frame[5];
    event.crc_ok = crc_ok;
    event.app_command = app;
    event.at_us = sim_clock_now_us();
    event.byte_index = bytes_clocked;
    trace_push(&event);

    if ((frame[0] & 0xC0U) != 0x40U) {
        note_protocol_error("command frame start/transmission bits wrong");
    }
    if ((frame[5] & 0x01U) == 0U) {
        note_protocol_error("command frame end bit not set");
    }

    /* Real cards validate CRC on CMD0 and CMD8 regardless of the CMD59 state,
     * because those bracket the transition into SPI mode. */
    const bool crc_required =
        card.crc_check_enabled || command == 0U || command == 8U;
    if (crc_required && !crc_ok) {
        note_protocol_error("command CRC7 rejected");
        if (!stopping) {
            respond_r1((uint8_t)(0x08U | idle_bit())); /* COM_CRC_ERROR */
        }
        return;
    }

    if (stopping) {
        /* CMD12 terminating a stream: the stop sequence below answers. */
        return;
    }

    if (sd_card_command_is_scripted(command)
            || response_policy == SD_RESPONSE_SCRIPTED_ONLY) {
        start_scripted_response(command);
        return;
    }
    dispatch_modelled(command, argument, app);
}

/* Returns true when the byte was consumed as command framing. */
static bool absorb_frame_byte(uint8_t mosi)
{
    if (frame_active) {
        frame[frame_length++] = mosi;
        if (frame_length == sizeof(frame)) {
            complete_frame();
        }
        return true;
    }
    if ((mosi & 0xC0U) == 0x40U) {
        frame_active = true;
        frame_length = 0U;
        frame[frame_length++] = mosi;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ lifecycle */

void sd_card_reset(const sd_card_desc_t *desc)
{
    sd_card_desc_t fallback = sd_card_desc(SD_CARD_SDHC,
        UINT64_C(2) * 1024U * 1024U * 1024U);
    card = desc != NULL ? *desc : fallback;
    if (card.read_bl_len < 9U || card.read_bl_len > 11U) {
        card.read_bl_len = 9U;
    }
    if (card.ncr_bytes > 64U) {
        card.ncr_bytes = 64U;
    }
    response_policy = SD_RESPONSE_SCRIPTED_ONLY;
    state = ST_IDLE;
    cs_released = true;
    frame_length = 0U;
    frame_active = false;
    active_command = 0xFFU;
    app_command_pending = false;
    card_idle = true;
    pending_r1 = 0xFFU;
    trailer_length = 0U;
    trailer_index = 0U;
    ncr_remaining = 0U;
    memset(data_buffer, 0, sizeof(data_buffer));
    data_length = 0U;
    data_index = 0U;
    data_crc = 0U;
    data_crc_index = 0U;
    data_token_value = 0xFEU;
    stream_lba = 0U;
    stream_blocks_sent = 0U;
    stream_active = false;
    read_ready_at_us = 0U;
    stop_residual_bytes = 0U;
    stop_byte_index = 0U;
    stop_stuff_sent = false;
    write_lba = 0U;
    write_multiple = false;
    write_response_token = 0x05U;
    host_write_crc = 0U;
    write_crc_check_enabled = true;
    write_crc_rejected = false;
    write_gap_required = false;
    resume_write_after_busy = false;
    stop_tran_delay_bytes = 0U;
    stop_tran_delay_remaining = 0U;
    stop_tran_program_us = 0U;
    stop_tran_busy_overridden = false;
    busy_until_us = 0U;
    global_busy_bytes = 0U;
    sd_card_script_clear();
    memset(overlay, 0, sizeof(overlay));
    content_seed = 0x5A5AU;
    sd_card_clear_faults();
    memset(phase_entries, 0, sizeof(phase_entries));
    memset(phase_command_entries, 0, sizeof(phase_command_entries));
    phase_byte_index = 0U;
    current_phase = SD_PHASE_NONE;
    state_overridden = false;
    random_stream_active = false;
    random_stream_state = 0U;
    eject_requested = false;
    raw_csd_valid = false;
    trace_length = 0U;
    trace_overflow = false;
    bytes_clocked = 0U;
    protocol_errors = 0U;
    last_protocol_error[0] = '\0';
}

const sd_card_desc_t *sd_card_description(void)
{
    return &card;
}

void sd_card_set_response_policy(sd_response_policy_t policy)
{
    response_policy = policy;
}

sd_response_policy_t sd_card_response_policy(void)
{
    return response_policy;
}

void sd_card_set_chip_select(bool released)
{
    if (released == cs_released) {
        return;
    }
    cs_released = released;
    trace_simple(SD_EV_CHIP_SELECT, released ? 1U : 0U, 0U);
    if (released) {
        /* Deselecting abandons any partial command and any pending response.
         * A busy card stays busy: busy is card-internal, not bus state. */
        frame_active = false;
        frame_length = 0U;
        script_index = script_length;
        stop_byte_index = 0U;
        stop_stuff_sent = false;
        if (state != ST_BUSY) {
            go_idle();
        }
    }
}

bool sd_card_chip_select_released(void)
{
    return cs_released;
}

void sd_card_resume(void)
{
    random_stream_active = false;
    busy_until_us = 0U;
    global_busy_bytes = 0U;
    frame_active = false;
    frame_length = 0U;
    script_index = script_length;
    stop_byte_index = 0U;
    stop_stuff_sent = false;
    data_length = 0U;
    stream_active = false;
    resume_write_after_busy = false;
    write_gap_required = false;
    state = ST_IDLE;
}

void sd_card_set_stop_tran_busy(size_t delay_bytes, uint64_t program_us)
{
    stop_tran_delay_bytes = delay_bytes;
    stop_tran_program_us = program_us;
    stop_tran_busy_overridden = true;
}

void sd_card_set_stop_residual_bytes(size_t bytes)
{
    stop_residual_bytes = bytes;
}

size_t sd_card_stop_residual_bytes(void)
{
    return stop_residual_bytes;
}

void sd_card_set_write_response_token(uint8_t token)
{
    write_response_token = token;
}

void sd_card_set_write_crc_check(bool enabled)
{
    write_crc_check_enabled = enabled;
}

uint64_t sd_card_stream_blocks_sent(void)
{
    return stream_blocks_sent;
}

/* ------------------------------------------------------- byte production */

static uint8_t stream_byte(void);

static uint8_t idle_or_busy_byte(uint8_t mosi)
{
    if (global_busy_bytes != 0U && mosi == 0xFFU) {
        if (global_busy_bytes != SD_BUSY_FOREVER) {
            global_busy_bytes--;
        }
        return emit(SD_PHASE_BUSY, 0x00U);
    }
    return emit(SD_PHASE_NONE, 0xFFU);
}

/* Produce the next byte of an active read stream. */
static uint8_t stream_byte(void)
{
    switch (state) {
    case ST_READ_WAIT:
        if (sim_clock_now_us() < read_ready_at_us) {
            return emit(SD_PHASE_DATA_WAIT, 0xFFU);
        }
        state = ST_READ_TOKEN;
        /* fall through */
    case ST_READ_TOKEN: {
        const uint8_t value = emit(SD_PHASE_DATA_TOKEN, data_token_value);
        if (state_overridden) {
            return value;
        }
        if (value == 0xFEU) {
            trace_simple(SD_EV_DATA_TOKEN, value, 0U);
            data_index = 0U;
            state = ST_READ_PAYLOAD;
        } else {
            trace_simple(SD_EV_ERROR_TOKEN, value, 0U);
            data_length = 0U;
            if (stream_active) {
                state = ST_STREAM_GAP;
            } else {
                go_idle();
            }
        }
        return value;
    }
    case ST_READ_PAYLOAD: {
        const uint8_t value =
            emit(SD_PHASE_DATA_PAYLOAD, data_buffer[data_index]);
        if (state_overridden) {
            return value;
        }
        data_index++;
        if (data_index >= data_length) {
            data_crc_index = 0U;
            state = ST_READ_CRC;
        }
        return value;
    }
    case ST_READ_CRC: {
        const uint8_t raw = data_crc_index == 0U
            ? (uint8_t)(data_crc >> 8U)
            : (uint8_t)(data_crc & 0xFFU);
        const uint8_t value = emit(SD_PHASE_DATA_CRC, raw);
        if (state_overridden) {
            return value;
        }
        data_crc_index++;
        if (data_crc_index >= 2U) {
            trace_simple(SD_EV_BLOCK_SENT, 0U, (uint32_t)stream_lba);
            stream_blocks_sent++;
            if (stream_active) {
                stream_lba++;
                state = ST_STREAM_GAP;
            } else {
                go_idle();
            }
        }
        return value;
    }
    case ST_STREAM_GAP: {
        const uint8_t value = emit(SD_PHASE_BETWEEN_BLOCKS, 0xFFU);
        if (state_overridden) {
            return value;
        }
        if (!card.stream_until_stop) {
            go_idle();
            return value;
        }
        /* The gap is one idle byte; then the next block begins. */
        load_block_for_stream();
        state = ST_READ_TOKEN;
        return value;
    }
    default:
        return emit(SD_PHASE_NONE, 0xFFU);
    }
}

static uint8_t produce_byte(uint8_t mosi)
{
    /* A busy card is not listening to the bus at all. */
    if (state == ST_BUSY) {
        if (sim_clock_now_us() >= busy_until_us) {
            trace_simple(SD_EV_BUSY_END, 0U, 0U);
            busy_until_us = 0U;
            if (resume_write_after_busy) {
                /* Programming of one block of a CMD25 transfer finished; the
                 * card is still in the transfer and expects the next start
                 * token or stop-tran. */
                resume_write_after_busy = false;
                state = ST_WRITE_WAIT_TOKEN;
            } else {
                go_idle();
            }
        } else {
            return emit(SD_PHASE_BUSY, 0x00U);
        }
    }

    /* A card waiting for the next block of a CMD25 transfer still decodes
     * command frames: the specification's error recovery for a rejected block
     * is CMD12, sent from exactly this state. */
    if (state == ST_WRITE_WAIT_TOKEN
            && (frame_active || (mosi & 0xC0U) == 0x40U)) {
        if (absorb_frame_byte(mosi)) {
            return emit(SD_PHASE_COMMAND, 0xFFU);
        }
    }

    /* The CMD12 stop sequence runs independently of the frame bytes so a
     * card that is still streaming can be modelled honestly. */
    if (state == ST_STOPPING) {
        /* True for every byte of the CMD12 frame including the one that
         * completes it. The card is still receiving during that byte, so the
         * stuff byte and the N_CR window start on the byte after the frame,
         * exactly as ST_NCR does for every other command. Testing frame_active
         * here instead would emit the stuff byte one position early, which
         * only shows when N_CR is 0. */
        const bool frame_byte = absorb_frame_byte(mosi);
        stop_byte_index++;
        if (stop_byte_index <= stop_residual_bytes) {
            /* Residual read data still in flight while the host clocks the
             * stop command and looks for its response. */
            return emit(SD_PHASE_BETWEEN_BLOCKS,
                data_buffer[stop_byte_index % SD_MODEL_BLOCK_SIZE]);
        }
        if (frame_byte) {
            return emit(SD_PHASE_COMMAND, 0xFFU);
        }
        if (!stop_stuff_sent) {
            stop_stuff_sent = true;
            trace_simple(SD_EV_STOP_ACCEPTED, 0U, 0U);
            return emit(SD_PHASE_STOP_STUFF, 0xFFU);
        }
        pending_r1 = 0x00U;
        ncr_remaining = card.ncr_bytes;
        data_length = 0U;
        stream_active = false;
        stop_byte_index = 0U;
        stop_stuff_sent = false;
        state = ST_NCR;
        return produce_byte(mosi);
    }

    const bool streaming = state == ST_READ_WAIT || state == ST_READ_TOKEN
        || state == ST_READ_PAYLOAD || state == ST_READ_CRC
        || state == ST_STREAM_GAP;

    if (streaming && stream_active && !frame_active
            && (mosi & 0xC0U) == 0x40U && (mosi & 0x3FU) == 12U) {
        /* CMD12 arriving mid-stream. */
        state = ST_STOPPING;
        stop_byte_index = 0U;
        stop_stuff_sent = false;
        return produce_byte(mosi);
    }

    if (state == ST_IDLE || state == ST_STALL || state == ST_SCRIPT
            || frame_active) {
        if (absorb_frame_byte(mosi)) {
            return emit(SD_PHASE_COMMAND, 0xFFU);
        }
    }

    switch (state) {
    case ST_IDLE:
        return idle_or_busy_byte(mosi);

    case ST_STALL:
        return emit(SD_PHASE_DATA_WAIT, 0xFFU);

    case ST_SCRIPT:
        if (script_index < script_length) {
            const size_t position = script_index;
            const uint8_t raw = script_queue[script_index++];
            const sd_phase_t phase = position < script_r1_position
                ? SD_PHASE_RESPONSE_WAIT
                : (position == script_r1_position
                    ? SD_PHASE_R1 : SD_PHASE_TRAILER);
            const uint8_t value = emit(phase, raw);
            if (position == script_r1_position) {
                trace_simple(SD_EV_R1, value, 0U);
            }
            return value;
        }
        if (script_busy_tail) {
            return emit(SD_PHASE_BUSY, 0x00U);
        }
        if (script_stall_tail) {
            return emit(SD_PHASE_DATA_WAIT, 0xFFU);
        }
        return idle_or_busy_byte(mosi);

    case ST_NCR:
        if (ncr_remaining > 0U) {
            ncr_remaining--;
            return emit(SD_PHASE_RESPONSE_WAIT, 0xFFU);
        }
        state = ST_R1;
        /* fall through */
    case ST_R1: {
        const uint8_t value = emit(SD_PHASE_R1, pending_r1);
        if (state_overridden) {
            return value;
        }
        trace_simple(SD_EV_R1, value, 0U);
        if (trailer_length > 0U) {
            trailer_index = 0U;
            state = ST_TRAILER;
        } else if (data_length > 0U) {
            state = ST_READ_WAIT;
        } else if (active_command == 24U || active_command == 25U) {
            state = ST_WRITE_WAIT_TOKEN;
            write_gap_required = true;
        } else if (card.program_us > 0U && active_command == 12U) {
            begin_busy_us(card.program_us);
        } else {
            go_idle();
        }
        return value;
    }

    case ST_TRAILER: {
        const uint8_t value = emit(SD_PHASE_TRAILER, trailer[trailer_index]);
        if (state_overridden) {
            return value;
        }
        sd_event_t event;
        memset(&event, 0, sizeof(event));
        event.kind = SD_EV_TRAILER;
        event.command = active_command;
        event.value = value;
        event.byte_offset = (uint32_t)trailer_index;
        event.at_us = sim_clock_now_us();
        event.byte_index = bytes_clocked;
        trace_push(&event);
        trailer_index++;
        if (trailer_index >= trailer_length) {
            trailer_length = 0U;
            state = data_length > 0U ? ST_READ_WAIT : ST_IDLE;
            if (state == ST_IDLE) {
                go_idle();
            }
        }
        return value;
    }

    case ST_READ_WAIT:
    case ST_READ_TOKEN:
    case ST_READ_PAYLOAD:
    case ST_READ_CRC:
    case ST_STREAM_GAP:
        return stream_byte();

    case ST_WRITE_WAIT_TOKEN: {
        /* Specification 7.3.3.2: 0xFE starts the block of a single-block
         * write, 0xFC starts each block of a multiple-block write, and 0xFD
         * ends a multiple-block write. A card scans for its own token and
         * ignores every other byte, so a host that sends the wrong one is
         * not answered - it is recorded here so a test can see it. */
        const bool first_after_r1 = write_gap_required;
        write_gap_required = false;
        const uint8_t expected = write_multiple ? 0xFCU : 0xFEU;
        if (mosi == expected) {
            if (first_after_r1) {
                note_protocol_error(
                    "start-block token sent with no N_WR idle byte after R1");
            }
            trace_simple(SD_EV_WRITE_TOKEN, mosi, 0U);
            data_index = 0U;
            state = ST_WRITE_PAYLOAD;
            return emit(SD_PHASE_WRITE_TOKEN, 0xFFU);
        }
        if (mosi == 0xFEU || mosi == 0xFCU) {
            note_protocol_error(write_multiple
                ? "0xFE sent in a multiple-block write; 0xFC is required"
                : "0xFC sent in a single-block write; 0xFE is required");
            return emit(SD_PHASE_WRITE_TOKEN, 0xFFU);
        }
        if (mosi == 0xFDU) {
            if (!write_multiple) {
                note_protocol_error("stop-tran token in a single-block write");
                return emit(SD_PHASE_WRITE_TOKEN, 0xFFU);
            }
            trace_simple(SD_EV_STOP_TRAN, mosi, 0U);
            const uint8_t value = emit(SD_PHASE_WRITE_TOKEN, 0xFFU);
            if (!state_overridden) {
                const uint64_t program_us = stop_tran_busy_us();
                if (program_us == 0U) {
                    go_idle();
                } else if (stop_tran_delay_bytes > 0U) {
                    stop_tran_delay_remaining = stop_tran_delay_bytes;
                    state = ST_STOP_TRAN_GAP;
                } else {
                    begin_busy_us(program_us);
                }
            }
            return value;
        }
        return emit(SD_PHASE_WRITE_TOKEN, 0xFFU);
    }

    case ST_STOP_TRAN_GAP: {
        /* N_BR: the card has accepted stop-tran but has not asserted busy
         * yet. It answers idle, and a host that stops waiting here releases
         * a card that is about to start programming. */
        const uint8_t value = emit(SD_PHASE_WRITE_BUSY, 0xFFU);
        if (state_overridden) {
            return value;
        }
        if (--stop_tran_delay_remaining == 0U) {
            begin_busy_us(stop_tran_busy_us());
        }
        return value;
    }

    case ST_WRITE_PAYLOAD: {
        const uint8_t value = emit(SD_PHASE_WRITE_PAYLOAD, 0xFFU);
        if (state_overridden) {
            return value;
        }
        data_buffer[data_index++] = mosi;
        if (data_index >= SD_MODEL_BLOCK_SIZE) {
            data_crc_index = 0U;
            state = ST_WRITE_CRC;
        }
        return value;
    }

    case ST_WRITE_CRC: {
        const uint8_t value = emit(SD_PHASE_WRITE_PAYLOAD, 0xFFU);
        if (state_overridden) {
            return value;
        }
        host_write_crc = (uint16_t)((host_write_crc << 8U) | mosi);
        data_crc_index++;
        if (data_crc_index >= 2U) {
            const uint16_t expected =
                sd_crc16_ccitt(data_buffer, SD_MODEL_BLOCK_SIZE);
            if (write_crc_check_enabled && host_write_crc != expected) {
                /* A real card discards the block and reports the error in the
                 * data-response token; it does not store partial data. */
                note_protocol_error("write data CRC16 rejected");
                write_crc_rejected = true;
            } else {
                write_crc_rejected = false;
                (void)sd_card_set_block(write_lba, data_buffer);
                trace_simple(SD_EV_BLOCK_WRITTEN, 0U, (uint32_t)write_lba);
            }
            host_write_crc = 0U;
            write_lba++;
            state = ST_WRITE_RESPONSE;
        }
        return value;
    }

    case ST_WRITE_RESPONSE: {
        /* 0x0B is the specification's "CRC error" data-response token. It
         * outranks the token a test asked for: the card rejected the block. */
        const uint8_t token = write_crc_rejected ? 0x0BU : write_response_token;
        const uint8_t value = emit(SD_PHASE_WRITE_RESPONSE, token);
        if (state_overridden) {
            return value;
        }
        trace_simple(SD_EV_WRITE_RESPONSE, value, 0U);
        if (card.program_us > 0U) {
            resume_write_after_busy = write_multiple;
            begin_busy_us(card.program_us);
        } else if (write_multiple) {
            state = ST_WRITE_WAIT_TOKEN;
        } else {
            go_idle();
        }
        return value;
    }

    case ST_BUSY:
    case ST_STOPPING:
    default:
        return emit(SD_PHASE_NONE, 0xFFU);
    }
}

uint8_t sd_card_transfer(uint8_t mosi)
{
    bytes_clocked++;
    if (cs_released) {
        enter_phase(SD_PHASE_RELEASE);
        const uint8_t value = emit(SD_PHASE_RELEASE, 0xFFU);
        phase_byte_index++;
        return value;
    }
    const uint8_t value = produce_byte(mosi);
    phase_byte_index++;
    if (random_stream_active) {
        random_stream_state ^= random_stream_state << 13U;
        random_stream_state ^= random_stream_state >> 17U;
        random_stream_state ^= random_stream_state << 5U;
        return (uint8_t)(random_stream_state >> 7U);
    }
    return value;
}

/* --------------------------------------------------------------- queries */

size_t sd_card_trace_length(void)
{
    return trace_length;
}

const sd_event_t *sd_card_trace_at(size_t index)
{
    return index < trace_length ? &trace[index] : NULL;
}

size_t sd_card_trace_count(sd_event_kind_t kind)
{
    size_t count = 0U;
    for (size_t i = 0U; i < trace_length; ++i) {
        if (trace[i].kind == kind) {
            count++;
        }
    }
    return count;
}

const sd_event_t *sd_card_trace_nth(sd_event_kind_t kind, size_t n)
{
    size_t seen = 0U;
    for (size_t i = 0U; i < trace_length; ++i) {
        if (trace[i].kind != kind) {
            continue;
        }
        if (seen == n) {
            return &trace[i];
        }
        seen++;
    }
    return NULL;
}

bool sd_card_trace_overflowed(void)
{
    return trace_overflow;
}

size_t sd_card_trace_command_sequence(uint8_t *out, size_t capacity)
{
    size_t count = 0U;
    for (size_t i = 0U; i < trace_length; ++i) {
        if (trace[i].kind != SD_EV_COMMAND) {
            continue;
        }
        if (count < capacity) {
            out[count] = trace[i].command;
        }
        count++;
    }
    return count;
}

void sd_card_trace_dump(FILE *stream, size_t max_events)
{
    const size_t limit = max_events < trace_length ? max_events : trace_length;
    (void)fprintf(stream, "--- SD trace (%zu events%s) ---\n",
        trace_length, trace_overflow ? ", TRUNCATED" : "");
    for (size_t i = 0U; i < limit; ++i) {
        const sd_event_t *const event = &trace[i];
        (void)fprintf(stream,
            "  [%4zu] byte=%-8llu t=%-10lluus %-14s cmd=%-3u arg=0x%08lx "
            "val=0x%02x%s%s\n",
            i,
            (unsigned long long)event->byte_index,
            (unsigned long long)event->at_us,
            sd_event_kind_name(event->kind),
            (unsigned)event->command,
            (unsigned long)event->argument,
            (unsigned)event->value,
            (event->kind == SD_EV_COMMAND && !event->crc_ok)
                ? " crc=placeholder" : "",
            event->app_command ? " (ACMD)" : "");
    }
    if (limit < trace_length) {
        (void)fprintf(stream, "  ... %zu more\n", trace_length - limit);
    }
}

uint64_t sd_card_bytes_clocked(void)
{
    return bytes_clocked;
}

uint32_t sd_card_protocol_errors(void)
{
    return protocol_errors;
}

const char *sd_card_last_protocol_error(void)
{
    return last_protocol_error;
}
