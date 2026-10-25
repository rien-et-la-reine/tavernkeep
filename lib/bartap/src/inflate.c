// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Inflate (inflate.h, internal): raw DEFLATE, RFC 1951.
 *
 * Input bits are pulled into bit_buffer a byte at a time, only when a step needs more than it holds, so
 * at most 7 bits beyond the item being decoded are ever taken and no whole byte after the end of the
 * stream is consumed. Every step that cannot finish keeps what it has in the state and returns, which
 * makes the decoder resumable at any byte. Huffman codes are decoded a bit at a time over canonical
 * counts, as in Mark Adler's puff, which needs no lookup tables and stops at the code's longest length,
 * so a stream is reported corrupt exactly where zlib reports it. */

#include "inflate.h"

#include <string.h>

#define WINDOW_MASK (BARTAP_INFLATE_WINDOW_SIZE - 1U)

enum {
    STAGE_HEADER,                       //BFINAL and BTYPE
    STAGE_STORED_LENGTH,                //LEN and NLEN
    STAGE_STORED_COPY,
    STAGE_DYNAMIC_HEADER,               //HLIT, HDIST, HCLEN
    STAGE_DYNAMIC_LENGTH_CODE,          //the 3-bit code-length code lengths
    STAGE_DYNAMIC_LENGTHS,              //literal/length and distance code lengths
    STAGE_DYNAMIC_REPEAT,               //a repeat code's extra bits
    STAGE_SYMBOL,                       //a literal/length symbol
    STAGE_LITERAL,                      //a literal waiting for output space
    STAGE_LENGTH_EXTRA,
    STAGE_DISTANCE,
    STAGE_DISTANCE_EXTRA,
    STAGE_COPY,                         //a match being copied out
};

//RFC 1951 3.2.5: base and extra bits for length symbols 257-285 and distance symbols 0-29
static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
//RFC 1951 3.2.7: the order code-length code lengths arrive in
static const uint8_t length_code_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

typedef enum {
    STEP_DONE,                          //the step finished
    STEP_NEED_INPUT,                    //it needs more input than this call has
    STEP_CORRUPT,
} step_t;

//this call's input, consumed through the state's bit buffer
typedef struct {
    const uint8_t *in;
    size_t size;
    size_t used;
} input_t;

//make at least n bits (n <= 24) available, pulling whole bytes only as needed
static bool need_bits(bartap_inflate_t *s, input_t *input, unsigned n)
{
    while (s->bit_count < n) {
        if (input->used == input->size) {
            return false;
        }
        s->bit_buffer |= (uint32_t)input->in[input->used++] << s->bit_count;
        s->bit_count = (uint8_t)(s->bit_count + 8U);
    }
    return true;
}

static uint32_t take_bits(bartap_inflate_t *s, unsigned n)
{
    const uint32_t value = s->bit_buffer & ((1UL << n) - 1U);
    s->bit_buffer >>= n;
    s->bit_count = (uint8_t)(s->bit_count - n);
    return value;
}

typedef enum {
    CODE_KIND_LENGTHS,                  //the code-length code: must be complete
    CODE_KIND_SYMBOLS,                  //literal/length or distance: one 1-bit code may stand alone
} code_kind_t;

//build a canonical code from its lengths. false if it is over-subscribed, or incomplete where zlib
//refuses that: zlib's inflate_table allows an incomplete code only for literal/length and distance codes
//whose longest code is 1 bit, and allows any code with no symbols at all
static bool build_code(bartap_inflate_code_t *code, const uint8_t *lengths, unsigned n, code_kind_t kind)
{
    memset(code->count, 0, sizeof code->count);
    code->max_length = 0;
    for (unsigned i = 0; i < n; i++) {
        code->count[lengths[i]]++;
        if (lengths[i] > code->max_length) {
            code->max_length = lengths[i];
        }
    }
    if (code->max_length == 0) {
        return true;
    }
    int32_t left = 1;
    for (unsigned length = 1; length < 16; length++) {
        left = left * 2 - code->count[length];
        if (left < 0) {
            return false;
        }
    }
    if (left > 0 && (kind == CODE_KIND_LENGTHS || code->max_length != 1)) {
        return false;
    }
    uint16_t offset[16];
    offset[1] = 0;
    for (unsigned length = 1; length < 15; length++) {
        offset[length + 1] = (uint16_t)(offset[length] + code->count[length]);
    }
    for (unsigned i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            code->symbol[offset[lengths[i]]++] = (uint16_t)i;
        }
    }
    return true;
}

//decode one symbol, a bit at a time. Bits are only taken once the symbol is complete, so a call that
//runs out of input can start again from the same bits
static step_t decode(bartap_inflate_t *s, input_t *input, const bartap_inflate_code_t *code, uint16_t *symbol)
{
    if (code->max_length == 0) {
        //zlib's table for a code with no symbols spends one bit and then fails
        return need_bits(s, input, 1) ? STEP_CORRUPT : STEP_NEED_INPUT;
    }
    uint32_t value = 0;                 //the code so far, first bit most significant
    uint32_t first = 0;                 //first code of the current length
    uint32_t index = 0;                 //symbols of shorter lengths
    for (unsigned length = 1; length <= code->max_length; length++) {
        if (!need_bits(s, input, length)) {
            return STEP_NEED_INPUT;
        }
        value |= (s->bit_buffer >> (length - 1U)) & 1U;
        const uint32_t count = code->count[length];
        if (value < first + count) {
            *symbol = code->symbol[index + (value - first)];
            (void)take_bits(s, length);
            return STEP_DONE;
        }
        index += count;
        first = (first + count) << 1;
        value <<= 1;
    }
    return STEP_CORRUPT;                //an unused code of an incomplete set
}

//the fixed codes of RFC 1951 3.2.6, built for each fixed block. Distance symbols 30 and 31 are given
//codes so that reading one is corrupt rather than an unused code
static void build_fixed(bartap_inflate_t *s)
{
    uint8_t lengths[288];
    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 112);
    memset(lengths + 256, 7, 24);
    memset(lengths + 280, 8, 8);
    (void)build_code(&s->literal_code, lengths, 288, CODE_KIND_SYMBOLS);
    memset(lengths, 5, 32);
    (void)build_code(&s->distance_code, lengths, 32, CODE_KIND_SYMBOLS);
}

//take one byte from the bit buffer's whole bytes, then from the input
static bool take_byte(bartap_inflate_t *s, input_t *input, uint8_t *byte)
{
    if (s->bit_count >= 8U) {
        *byte = (uint8_t)take_bits(s, 8);
        return true;
    }
    if (input->used == input->size) {
        return false;
    }
    *byte = input->in[input->used++];
    return true;
}

static void put_byte(bartap_inflate_t *s, uint8_t *out, size_t *produced, uint8_t byte)
{
    out[(*produced)++] = byte;
    s->window[s->window_position] = byte;
    s->window_position = (s->window_position + 1U) & WINDOW_MASK;
    if (s->history < BARTAP_INFLATE_WINDOW_SIZE) {
        s->history++;
    }
}

bartap_inflate_result_t bartap_inflate_init(bartap_inflate_t *state, uint8_t *window, size_t window_size)
{
    if (state == NULL) {
        return BARTAP_INFLATE_RESULT_INVALID_ARGUMENT;
    }
    memset(state, 0, sizeof *state);
    state->stage = STAGE_HEADER;
    if (window == NULL || window_size < BARTAP_INFLATE_WINDOW_SIZE) {
        state->status = BARTAP_INFLATE_RESULT_INVALID_ARGUMENT;
        return state->status;
    }
    state->window = window;
    state->status = BARTAP_INFLATE_RESULT_OK;
    return BARTAP_INFLATE_RESULT_OK;
}

//run the stages until one needs more input or output than this call has, or the stream ends
static step_t run(bartap_inflate_t *s, input_t *input, uint8_t *out, size_t out_size, size_t *produced,
    bool *done)
{
    for (;;) {
        switch (s->stage) {
        case STAGE_HEADER: {
            if (!need_bits(s, input, 3)) {
                return STEP_NEED_INPUT;
            }
            s->final_block = take_bits(s, 1) != 0;
            const uint32_t type = take_bits(s, 2);
            if (type == 0) {
                (void)take_bits(s, s->bit_count % 8U);  //stored blocks start on a byte boundary
                s->stored_have = 0;
                s->stage = STAGE_STORED_LENGTH;
            } else if (type == 1) {
                build_fixed(s);
                s->stage = STAGE_SYMBOL;
            } else if (type == 2) {
                s->stage = STAGE_DYNAMIC_HEADER;
            } else {
                return STEP_CORRUPT;                    //reserved block type
            }
            break;
        }
        case STAGE_STORED_LENGTH:
            while (s->stored_have < 4U) {
                if (!take_byte(s, input, &s->stored_header[s->stored_have])) {
                    return STEP_NEED_INPUT;
                }
                s->stored_have++;
            }
            {
                const uint32_t length = s->stored_header[0] | ((uint32_t)s->stored_header[1] << 8);
                const uint32_t complement = s->stored_header[2] | ((uint32_t)s->stored_header[3] << 8);
                if (length != (~complement & 0xFFFFU)) {
                    return STEP_CORRUPT;
                }
                s->stored_left = length;
            }
            s->stage = STAGE_STORED_COPY;
            break;
        case STAGE_STORED_COPY:
            while (s->stored_left > 0U) {
                if (*produced == out_size) {
                    return STEP_DONE;                   //out is full
                }
                uint8_t byte;
                if (!take_byte(s, input, &byte)) {
                    return STEP_NEED_INPUT;
                }
                put_byte(s, out, produced, byte);
                s->stored_left--;
            }
            goto block_end;
        case STAGE_DYNAMIC_HEADER:
            if (!need_bits(s, input, 14)) {
                return STEP_NEED_INPUT;
            }
            s->literal_count = (uint16_t)(take_bits(s, 5) + 257U);
            s->distance_count = (uint16_t)(take_bits(s, 5) + 1U);
            s->length_code_count = (uint16_t)(take_bits(s, 4) + 4U);
            if (s->literal_count > 286U || s->distance_count > 30U) {
                return STEP_CORRUPT;
            }
            memset(s->lengths, 0, 19);
            s->lengths_have = 0;
            s->stage = STAGE_DYNAMIC_LENGTH_CODE;
            break;
        case STAGE_DYNAMIC_LENGTH_CODE:
            while (s->lengths_have < s->length_code_count) {
                if (!need_bits(s, input, 3)) {
                    return STEP_NEED_INPUT;
                }
                s->lengths[length_code_order[s->lengths_have++]] = (uint8_t)take_bits(s, 3);
            }
            if (!build_code(&s->length_code, s->lengths, 19, CODE_KIND_LENGTHS)) {
                return STEP_CORRUPT;
            }
            s->lengths_have = 0;
            s->stage = STAGE_DYNAMIC_LENGTHS;
            break;
        case STAGE_DYNAMIC_LENGTHS:
            while (s->lengths_have < s->literal_count + s->distance_count) {
                uint16_t symbol;
                if (s->length_code.max_length == 0) {
                    //zlib reads a code-length code with no symbols as 1-bit zero lengths
                    if (!need_bits(s, input, 1)) {
                        return STEP_NEED_INPUT;
                    }
                    (void)take_bits(s, 1);
                    symbol = 0;
                } else {
                    const step_t step = decode(s, input, &s->length_code, &symbol);
                    if (step != STEP_DONE) {
                        return step;
                    }
                }
                if (symbol < 16U) {
                    s->lengths[s->lengths_have++] = (uint8_t)symbol;
                } else {
                    s->pending_symbol = symbol;
                    s->stage = STAGE_DYNAMIC_REPEAT;
                    break;
                }
            }
            if (s->stage == STAGE_DYNAMIC_REPEAT) {
                break;
            }
            if (s->lengths[256] == 0) {
                return STEP_CORRUPT;                    //no end-of-block code
            }
            if (!build_code(&s->literal_code, s->lengths, s->literal_count, CODE_KIND_SYMBOLS)
                    || !build_code(&s->distance_code, s->lengths + s->literal_count, s->distance_count,
                        CODE_KIND_SYMBOLS)) {
                return STEP_CORRUPT;
            }
            s->stage = STAGE_SYMBOL;
            break;
        case STAGE_DYNAMIC_REPEAT: {
            const unsigned extra = s->pending_symbol == 16U ? 2U : s->pending_symbol == 17U ? 3U : 7U;
            if (!need_bits(s, input, extra)) {
                return STEP_NEED_INPUT;
            }
            const uint32_t bits = take_bits(s, extra);
            uint8_t value = 0;
            uint32_t repeat;
            if (s->pending_symbol == 16U) {
                if (s->lengths_have == 0U) {
                    return STEP_CORRUPT;                //nothing to repeat
                }
                value = s->lengths[s->lengths_have - 1U];
                repeat = 3U + bits;
            } else {
                repeat = (s->pending_symbol == 17U ? 3U : 11U) + bits;
            }
            if (s->lengths_have + repeat > (uint32_t)s->literal_count + s->distance_count) {
                return STEP_CORRUPT;
            }
            memset(s->lengths + s->lengths_have, value, repeat);
            s->lengths_have = (uint16_t)(s->lengths_have + repeat);
            s->stage = STAGE_DYNAMIC_LENGTHS;
            break;
        }
        case STAGE_SYMBOL: {
            uint16_t symbol;
            const step_t step = decode(s, input, &s->literal_code, &symbol);
            if (step != STEP_DONE) {
                return step;
            }
            if (symbol < 256U) {
                s->pending_symbol = symbol;
                s->stage = STAGE_LITERAL;
            } else if (symbol == 256U) {
                goto block_end;
            } else if (symbol <= 285U) {
                s->pending_symbol = (uint16_t)(symbol - 257U);
                s->stage = STAGE_LENGTH_EXTRA;
            } else {
                return STEP_CORRUPT;                    //286 and 287
            }
            break;
        }
        case STAGE_LITERAL:
            if (*produced == out_size) {
                return STEP_DONE;
            }
            put_byte(s, out, produced, (uint8_t)s->pending_symbol);
            s->stage = STAGE_SYMBOL;
            break;
        case STAGE_LENGTH_EXTRA: {
            const unsigned extra = length_extra[s->pending_symbol];
            if (!need_bits(s, input, extra)) {
                return STEP_NEED_INPUT;
            }
            s->match_length = (uint16_t)(length_base[s->pending_symbol] + take_bits(s, extra));
            s->stage = STAGE_DISTANCE;
            break;
        }
        case STAGE_DISTANCE: {
            uint16_t symbol;
            const step_t step = decode(s, input, &s->distance_code, &symbol);
            if (step != STEP_DONE) {
                return step;
            }
            if (symbol >= 30U) {
                return STEP_CORRUPT;
            }
            s->pending_symbol = symbol;
            s->stage = STAGE_DISTANCE_EXTRA;
            break;
        }
        case STAGE_DISTANCE_EXTRA: {
            const unsigned extra = distance_extra[s->pending_symbol];
            if (!need_bits(s, input, extra)) {
                return STEP_NEED_INPUT;
            }
            s->match_distance = (uint16_t)(distance_base[s->pending_symbol] + take_bits(s, extra));
            if (s->match_distance > s->history) {
                return STEP_CORRUPT;                    //reaches back before the first byte
            }
            s->match_left = s->match_length;
            s->stage = STAGE_COPY;
            break;
        }
        case STAGE_COPY:
            while (s->match_left > 0U) {
                if (*produced == out_size) {
                    return STEP_DONE;
                }
                put_byte(s, out, produced, s->window[(s->window_position - s->match_distance) & WINDOW_MASK]);
                s->match_left--;
            }
            s->stage = STAGE_SYMBOL;
            break;
        default:
            return STEP_CORRUPT;
        }
        continue;

    block_end:
        if (s->final_block) {
            *done = true;
            return STEP_DONE;
        }
        s->stage = STAGE_HEADER;
    }
}

bartap_inflate_result_t bartap_inflate_run(
    bartap_inflate_t *state,
    const uint8_t *in,
    size_t in_size,
    size_t *in_used,
    bool input_ends,
    uint8_t *out,
    size_t out_size,
    size_t *out_produced)
{
    if (state == NULL || in_used == NULL || out_produced == NULL || (in == NULL && in_size != 0U)
            || (out == NULL && out_size != 0U)) {
        return BARTAP_INFLATE_RESULT_INVALID_ARGUMENT;
    }
    *in_used = 0;
    *out_produced = 0;
    if (state->status != BARTAP_INFLATE_RESULT_OK) {
        return state->status;
    }
    input_t input = { in, in_size, 0 };
    bool done = false;
    const step_t step = run(state, &input, out, out_size, out_produced, &done);
    *in_used = input.used;
    if (step == STEP_CORRUPT) {
        state->status = BARTAP_INFLATE_RESULT_CORRUPT;
    } else if (done) {
        state->status = BARTAP_INFLATE_RESULT_DONE;
    } else if (step == STEP_NEED_INPUT && input_ends) {
        state->status = BARTAP_INFLATE_RESULT_TRUNCATED;
    }
    return state->status;
}
