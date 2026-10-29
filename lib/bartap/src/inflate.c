// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

/* Streaming DEFLATE (RFC 1951), puff-style canonical Huffman decoding over a caller-owned 32 KiB window,
 * with an optional 8-bit fast lookup table for literal/length codes (BARTAP_INFLATE_FAST_LUT). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bartap_internal.h"

#define BARTAP_INFLATE_MAXBITS 15U

enum {
    BARTAP_INF_BLOCK_HEAD = 0,
    BARTAP_INF_STORED_LEN,
    BARTAP_INF_STORED_COPY,
    BARTAP_INF_DYN_HEAD,
    BARTAP_INF_DYN_CLEN,
    BARTAP_INF_DYN_TREES,
    BARTAP_INF_DECODE_LIT,
    BARTAP_INF_EMIT_LIT,
    BARTAP_INF_DECODE_LEN_EXT,
    BARTAP_INF_DECODE_DIST,
    BARTAP_INF_DECODE_DIST_EXT,
    BARTAP_INF_COPY_MATCH,
    BARTAP_INF_DONE,
    BARTAP_INF_BAD,
};

static const uint8_t bartap_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const uint16_t bartap_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const uint8_t bartap_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t bartap_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};

static const uint8_t bartap_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

#if defined(BARTAP_INFLATE_FAST_LUT)
static uint8_t bartap_reverse_byte(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0U) >> 4U) | ((b & 0x0FU) << 4U));
    b = (uint8_t)(((b & 0xCCU) >> 2U) | ((b & 0x33U) << 2U));
    b = (uint8_t)(((b & 0xAAU) >> 1U) | ((b & 0x55U) << 1U));
    return b;
}
#endif

/* Build canonical decoding tables from explicit code lengths. count[len] says how many symbols
 * have that length; symbol[] lists those symbols in increasing length, then numeric order. The
 * caller-owned scratch array is separate, so sorting cannot overwrite lengths still being read. */
static bartap_err_t bartap_huff_build(const uint8_t *lengths, uint16_t *count, uint16_t *symbol, uint16_t n,
    uint16_t max_bits, bool allow_empty,
#if defined(BARTAP_INFLATE_FAST_LUT)
    uint16_t *fast)
#else
    void *fast)
#endif
{
#if !defined(BARTAP_INFLATE_FAST_LUT)
    (void)fast;
#endif
    memset(count, 0, 16U * sizeof(uint16_t));
    for (uint16_t i = 0; i < n; i++) {
        const uint16_t len = lengths[i];
        if (len > max_bits) {
            return BARTAP_ERR_CORRUPT;
        }
        count[len]++;
    }

    if (count[0] == n) {
#if defined(BARTAP_INFLATE_FAST_LUT)
        if (fast != NULL) {
            memset(fast, 0, 256U * sizeof(uint16_t));
        }
#endif
        return allow_empty ? BARTAP_OK : BARTAP_ERR_CORRUPT;
    }

    int32_t left = 1;
    uint16_t max_len = 0U;
    for (uint16_t len = 1; len <= max_bits; len++) {
        left <<= 1;
        left -= (int32_t)count[len];
        if (left < 0) {
            return BARTAP_ERR_CORRUPT;
        }
        if (count[len] > 0U) {
            max_len = len;
        }
    }

    if (left > 0 && !(max_len == 1U && count[1] == 1U)) {
        return BARTAP_ERR_CORRUPT;
    }

    uint16_t offs[16];
    offs[1] = 0U;
    for (uint16_t len = 1; len < max_bits; len++) {
        offs[len + 1U] = (uint16_t)(offs[len] + count[len]);
    }

    for (uint16_t i = 0; i < n; i++) {
        const uint16_t len = lengths[i];
        if (len != 0U) {
            symbol[offs[len]++] = i;
        }
    }

#if defined(BARTAP_INFLATE_FAST_LUT)
    if (fast != NULL) {
        memset(fast, 0, 256U * sizeof(uint16_t));
        uint16_t code = 0U;
        uint16_t idx = 0U;
        for (uint16_t len = 1; len <= 8U && len <= max_bits; len++) {
            for (uint16_t i = 0; i < count[len]; i++) {
                const uint16_t sym = symbol[idx++];
                const uint8_t rev = (uint8_t)(bartap_reverse_byte((uint8_t)code) >> (8U - len));
                const uint16_t entry = (uint16_t)((len << 12U) | (sym & 0x0FFFU));
                const uint16_t step = (uint16_t)(1U << len);
                for (uint16_t fill = rev; fill < 256U; fill = (uint16_t)(fill + step)) {
                    fast[fill] = entry;
                }
                code++;
            }
            code <<= 1U;
        }
    }
#endif

    return BARTAP_OK;
}

static bartap_err_t bartap_build_fixed_trees(bartap_inflate_t *inf)
{
    for (uint16_t i = 0; i < 144U; i++) {
        inf->code_lengths[i] = 8U;
    }
    for (uint16_t i = 144U; i < 256U; i++) {
        inf->code_lengths[i] = 9U;
    }
    for (uint16_t i = 256U; i < 280U; i++) {
        inf->code_lengths[i] = 7U;
    }
    for (uint16_t i = 280U; i < 288U; i++) {
        inf->code_lengths[i] = 8U;
    }
#if defined(BARTAP_INFLATE_FAST_LUT)
    const bartap_err_t err = bartap_huff_build(inf->code_lengths, inf->length_count,
        inf->length_symbol, 288U, 15U, false, inf->fast);
#else
    const bartap_err_t err = bartap_huff_build(inf->code_lengths, inf->length_count,
        inf->length_symbol, 288U, 15U, false, NULL);
#endif

    if (err != BARTAP_OK) {
        return err;
    }
    /* Fixed trees include reserved symbols 30 and 31; decoding rejects them. */
    for (uint16_t i = 0; i < 32U; i++) {
        inf->code_lengths[i] = 5U;
    }
    return bartap_huff_build(inf->code_lengths, inf->distance_count, inf->distance_symbol, 32U, 15U, false, NULL);
}

static void bartap_fill_bits(bartap_inflate_t *inf, const uint8_t **in, size_t *in_len)
{
    while (inf->bit_count <= 24U && *in_len > 0U) {
        inf->bits |= (uint32_t)(**in) << inf->bit_count;
        inf->bit_count = (uint8_t)(inf->bit_count + 8U);
        (*in)++;
        (*in_len)--;
    }
}

static int bartap_huff_decode(bartap_inflate_t *inf, const uint16_t *count, const uint16_t *symbol,
#if defined(BARTAP_INFLATE_FAST_LUT)
    const uint16_t *fast,
#endif
    uint16_t *sym)
{
#if defined(BARTAP_INFLATE_FAST_LUT)
    if (fast != NULL && inf->bit_count >= 8U) {
        const uint16_t entry = fast[inf->bits & 0xFFU];
        const uint8_t len = (uint8_t)(entry >> 12U);
        if (len != 0U) {
            inf->bits >>= len;
            inf->bit_count = (uint8_t)(inf->bit_count - len);
            *sym = (uint16_t)(entry & 0x0FFFU);
            return 0;
        }
    }
#endif

    uint32_t code = 0U;
    uint32_t first = 0U;
    uint32_t index = 0U;
    uint32_t bitbuf = inf->bits;

    for (uint8_t len = 1U; len <= BARTAP_INFLATE_MAXBITS; len++) {
        if (len > inf->bit_count) {
            return 1;
        }
        code |= bitbuf & 1U;
        bitbuf >>= 1U;
        const uint32_t c = count[len];
        if (code - first < c) {
            inf->bits = bitbuf;
            inf->bit_count = (uint8_t)(inf->bit_count - len);
            *sym = symbol[index + (code - first)];
            return 0;
        }
        index += c;
        first = (first + c) << 1U;
        code <<= 1U;
    }
    return -1;
}

static inline void bartap_emit_byte(bartap_inflate_t *inf, uint8_t b, uint8_t **out, size_t *out_len)
{
    inf->window[inf->window_pos & (BARTAP_WINDOW_SIZE - 1U)] = b;
    inf->window_pos++;
    **out = b;
    (*out)++;
    (*out_len)--;
}

void bartap_inflate_init(bartap_inflate_t *inflate, uint8_t *window)
{
    if (inflate == NULL) {
        return;
    }
    memset(inflate, 0, sizeof *inflate);
    inflate->window = window;
    inflate->state = BARTAP_INF_BLOCK_HEAD;
}

bartap_err_t bartap_inflate(bartap_inflate_t *inf, const uint8_t *in, size_t in_len, size_t *in_used,
    uint8_t *out, size_t out_len, size_t *out_made)
{
    if (inf == NULL || inf->window == NULL || in_used == NULL || out_made == NULL
            || (in == NULL && in_len > 0U) || (out == NULL && out_len > 0U)) {
        return BARTAP_ERR_ARG;
    }

    const uint8_t *in_cur = in;
    size_t in_rem = in_len;
    uint8_t *out_cur = out;
    size_t out_rem = out_len;

    for (;;) {
        switch (inf->state) {
        case BARTAP_INF_BLOCK_HEAD: {
            if (inf->final_block) {
                inf->state = BARTAP_INF_DONE;
                break;
            }
            bartap_fill_bits(inf, &in_cur, &in_rem);
            if (inf->bit_count < 3U) {
                goto out_pause;
            }
            inf->final_block = (inf->bits & 1U) != 0U;
            const uint8_t btype = (uint8_t)((inf->bits >> 1U) & 3U);
            inf->bits >>= 3U;
            inf->bit_count = (uint8_t)(inf->bit_count - 3U);

            if (btype == 0U) {
                const uint8_t drop = (uint8_t)(inf->bit_count & 7U);
                inf->bits >>= drop;
                inf->bit_count = (uint8_t)(inf->bit_count - drop);
                inf->state = BARTAP_INF_STORED_LEN;
            } else if (btype == 1U) {
                if (bartap_build_fixed_trees(inf) != BARTAP_OK) {
                    inf->state = BARTAP_INF_BAD;
                    goto out_corrupt;
                }
                inf->state = BARTAP_INF_DECODE_LIT;
            } else if (btype == 2U) {
                inf->state = BARTAP_INF_DYN_HEAD;
            } else {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            break;
        }

        case BARTAP_INF_STORED_LEN: {
            bartap_fill_bits(inf, &in_cur, &in_rem);
            if (inf->bit_count < 32U) {
                goto out_pause;
            }
            const uint16_t len = (uint16_t)(inf->bits & 0xFFFFU);
            const uint16_t nlen = (uint16_t)((inf->bits >> 16U) & 0xFFFFU);
            inf->bits = 0U;
            inf->bit_count = 0U;
            if (len != (uint16_t)(~nlen)) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            inf->copy_length = len;
            inf->state = len == 0U ? BARTAP_INF_BLOCK_HEAD : BARTAP_INF_STORED_COPY;
            break;
        }

        case BARTAP_INF_STORED_COPY: {
            while (inf->copy_length > 0U && inf->bit_count >= 8U) {
                if (out_rem == 0U) {
                    goto out_pause;
                }
                const uint8_t b = (uint8_t)(inf->bits & 0xFFU);
                inf->bits >>= 8U;
                inf->bit_count = (uint8_t)(inf->bit_count - 8U);
                bartap_emit_byte(inf, b, &out_cur, &out_rem);
                inf->copy_length--;
            }
            while (inf->copy_length > 0U) {
                if (out_rem == 0U || in_rem == 0U) {
                    goto out_pause;
                }
                size_t chunk = inf->copy_length;
                if (chunk > in_rem) {
                    chunk = in_rem;
                }
                if (chunk > out_rem) {
                    chunk = out_rem;
                }
                const size_t wpos = inf->window_pos & (BARTAP_WINDOW_SIZE - 1U);
                const size_t wspace = BARTAP_WINDOW_SIZE - wpos;
                if (chunk > wspace) {
                    chunk = wspace;
                }
                memcpy(out_cur, in_cur, chunk);
                memcpy(inf->window + wpos, in_cur, chunk);
                in_cur += chunk;
                in_rem -= chunk;
                out_cur += chunk;
                out_rem -= chunk;
                inf->window_pos += (uint32_t)chunk;
                inf->copy_length = (uint16_t)(inf->copy_length - chunk);
            }
            inf->state = BARTAP_INF_BLOCK_HEAD;
            break;
        }

        case BARTAP_INF_DYN_HEAD: {
            bartap_fill_bits(inf, &in_cur, &in_rem);
            if (inf->bit_count < 14U) {
                goto out_pause;
            }
            const uint16_t hlit = (uint16_t)((inf->bits & 0x1FU) + 257U);
            const uint16_t hdist = (uint16_t)(((inf->bits >> 5U) & 0x1FU) + 1U);
            const uint16_t hclen = (uint16_t)(((inf->bits >> 10U) & 0x0FU) + 4U);
            inf->bits >>= 14U;
            inf->bit_count = (uint8_t)(inf->bit_count - 14U);

            if (hlit > 286U || hdist > 30U) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }

            inf->literal_codes = hlit;
            inf->distance_codes = hdist;
            inf->code_length_codes = hclen;
            inf->code_index = 0U;
            memset(inf->code_lengths, 0, sizeof inf->code_lengths);
            inf->state = BARTAP_INF_DYN_CLEN;
            break;
        }

        case BARTAP_INF_DYN_CLEN: {
            const uint16_t hclen = inf->code_length_codes;
            while (inf->code_index < hclen) {
                bartap_fill_bits(inf, &in_cur, &in_rem);
                if (inf->bit_count < 3U) {
                    goto out_pause;
                }
                inf->code_lengths[bartap_clen_order[inf->code_index]] = (uint8_t)(inf->bits & 7U);
                inf->bits >>= 3U;
                inf->bit_count = (uint8_t)(inf->bit_count - 3U);
                inf->code_index++;
            }

            if (bartap_huff_build(inf->code_lengths, inf->distance_count, inf->distance_symbol, 19U, 7U, false, NULL)
                    != BARTAP_OK) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }

            inf->code_index = 0U;
            inf->state = BARTAP_INF_DYN_TREES;
            break;
        }

        case BARTAP_INF_DYN_TREES: {
            const uint16_t nlen = inf->literal_codes;
            const uint16_t ndist = inf->distance_codes;
            const uint16_t total = (uint16_t)(nlen + ndist);

            while (inf->code_index < total) {
                bartap_fill_bits(inf, &in_cur, &in_rem);
                const uint32_t saved_bits = inf->bits;
                const uint8_t saved_count = inf->bit_count;

                uint16_t sym = 0U;
                const int rc = bartap_huff_decode(inf, inf->distance_count, inf->distance_symbol,
#if defined(BARTAP_INFLATE_FAST_LUT)
                    NULL,
#endif
                    &sym);
                if (rc < 0) {
                    inf->state = BARTAP_INF_BAD;
                    goto out_corrupt;
                }
                if (rc > 0) {
                    goto out_pause;
                }

                if (sym < 16U) {
                    inf->code_lengths[inf->code_index++] = (uint8_t)sym;
                } else {
                    const uint8_t extra = sym == 16U ? 2U : (sym == 17U ? 3U : 7U);
                    if (inf->bit_count < extra) {
                        inf->bits = saved_bits;
                        inf->bit_count = saved_count;
                        goto out_pause;
                    }
                    uint16_t rep = 0U;
                    uint16_t val = 0U;
                    if (sym == 16U) {
                        if (inf->code_index == 0U) {
                            inf->state = BARTAP_INF_BAD;
                            goto out_corrupt;
                        }
                        val = inf->code_lengths[inf->code_index - 1U];
                        rep = (uint16_t)(3U + (inf->bits & 3U));
                    } else if (sym == 17U) {
                        rep = (uint16_t)(3U + (inf->bits & 7U));
                    } else {
                        rep = (uint16_t)(11U + (inf->bits & 0x7FU));
                    }
                    inf->bits >>= extra;
                    inf->bit_count = (uint8_t)(inf->bit_count - extra);

                    if (inf->code_index + rep > total) {
                        inf->state = BARTAP_INF_BAD;
                        goto out_corrupt;
                    }
                    while (rep-- > 0U) {
                        inf->code_lengths[inf->code_index++] = (uint8_t)val;
                    }
                }
            }

            if (inf->code_lengths[256U] == 0U) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }

            if (bartap_huff_build(inf->code_lengths + nlen, inf->distance_count, inf->distance_symbol, ndist, 15U, true,
                    NULL) != BARTAP_OK) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }

#if defined(BARTAP_INFLATE_FAST_LUT)
            if (bartap_huff_build(inf->code_lengths, inf->length_count, inf->length_symbol, nlen, 15U, false,
                    inf->fast) != BARTAP_OK) {
#else
            if (bartap_huff_build(inf->code_lengths, inf->length_count, inf->length_symbol, nlen, 15U, false,
                    NULL) != BARTAP_OK) {
#endif
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }

            inf->state = BARTAP_INF_DECODE_LIT;
            break;
        }

        case BARTAP_INF_DECODE_LIT: {
            bartap_fill_bits(inf, &in_cur, &in_rem);
            uint16_t sym = 0U;
            const int rc = bartap_huff_decode(inf, inf->length_count, inf->length_symbol,
#if defined(BARTAP_INFLATE_FAST_LUT)
                inf->fast,
#endif
                &sym);
            if (rc < 0) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            if (rc > 0) {
                goto out_pause;
            }

            if (sym < 256U) {
                if (out_rem == 0U) {
                    inf->copy_length = sym;
                    inf->state = BARTAP_INF_EMIT_LIT;
                    goto out_pause;
                }
                bartap_emit_byte(inf, (uint8_t)sym, &out_cur, &out_rem);
            } else if (sym == 256U) {
                inf->state = BARTAP_INF_BLOCK_HEAD;
            } else if (sym <= 285U) {
                inf->copy_length = (uint16_t)(sym - 257U);
                inf->state = BARTAP_INF_DECODE_LEN_EXT;
            } else {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            break;
        }

        case BARTAP_INF_EMIT_LIT: {
            if (out_rem == 0U) {
                goto out_pause;
            }
            bartap_emit_byte(inf, (uint8_t)inf->copy_length, &out_cur, &out_rem);
            inf->state = BARTAP_INF_DECODE_LIT;
            break;
        }

        case BARTAP_INF_DECODE_LEN_EXT: {
            const uint16_t idx = inf->copy_length;
            const uint8_t extra = bartap_len_extra[idx];
            bartap_fill_bits(inf, &in_cur, &in_rem);
            if (inf->bit_count < extra) {
                goto out_pause;
            }
            inf->copy_length = (uint16_t)(bartap_len_base[idx] + (inf->bits & ((1U << extra) - 1U)));
            inf->bits >>= extra;
            inf->bit_count = (uint8_t)(inf->bit_count - extra);
            inf->state = BARTAP_INF_DECODE_DIST;
            break;
        }

        case BARTAP_INF_DECODE_DIST: {
            bartap_fill_bits(inf, &in_cur, &in_rem);
            uint16_t sym = 0U;
            const int rc = bartap_huff_decode(inf, inf->distance_count, inf->distance_symbol,
#if defined(BARTAP_INFLATE_FAST_LUT)
                NULL,
#endif
                &sym);
            if (rc < 0) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            if (rc > 0) {
                goto out_pause;
            }
            if (sym >= 30U) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            inf->copy_distance = sym;
            inf->state = BARTAP_INF_DECODE_DIST_EXT;
            break;
        }

        case BARTAP_INF_DECODE_DIST_EXT: {
            const uint16_t idx = inf->copy_distance;
            const uint8_t extra = bartap_dist_extra[idx];
            bartap_fill_bits(inf, &in_cur, &in_rem);
            if (inf->bit_count < extra) {
                goto out_pause;
            }
            const uint16_t dist = (uint16_t)(bartap_dist_base[idx] + (inf->bits & ((1U << extra) - 1U)));
            inf->bits >>= extra;
            inf->bit_count = (uint8_t)(inf->bit_count - extra);

            if (dist > inf->window_pos) {
                inf->state = BARTAP_INF_BAD;
                goto out_corrupt;
            }
            inf->copy_distance = dist;
            inf->state = BARTAP_INF_COPY_MATCH;
            break;
        }

        case BARTAP_INF_COPY_MATCH: {
            while (inf->copy_length > 0U) {
                if (out_rem == 0U) {
                    goto out_pause;
                }
                const uint8_t b = inf->window[(inf->window_pos - inf->copy_distance)
                    & (BARTAP_WINDOW_SIZE - 1U)];
                bartap_emit_byte(inf, b, &out_cur, &out_rem);
                inf->copy_length--;
            }
            inf->state = BARTAP_INF_DECODE_LIT;
            break;
        }

        case BARTAP_INF_DONE:
            *in_used = (size_t)(in_cur - in);
            *out_made = (size_t)(out_cur - out);
            return BARTAP_END;

        default:
            goto out_corrupt;
        }
    }

out_pause:
    *in_used = (size_t)(in_cur - in);
    *out_made = (size_t)(out_cur - out);
    return BARTAP_OK;

out_corrupt:
    *in_used = (size_t)(in_cur - in);
    *out_made = (size_t)(out_cur - out);
    return BARTAP_ERR_CORRUPT;
}
