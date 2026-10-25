// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_INFLATE_H
#define BARTAP_INFLATE_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * Inflate: a streaming decoder for raw DEFLATE (RFC 1951).
 *
 * Raw means no zlib (RFC 1950) or gzip (RFC 1952) wrapper: zip stores
 * compressed entries as bare DEFLATE, and the OCF layer (ocf.h) checks the
 * CRC-32 itself.
 *
 * Internal to Bartap (D9 in DECISIONS.md): only the OCF layer uses it. Nothing
 * in it is EPUB-specific, so it could be made public later if other users want
 * raw DEFLATE.
 *
 * Implemented in inflate.c, but still a draft interface: the choices are
 * explained beside each part so that they are easy to revisit.
 *
 * Choices:
 * - Streaming and resumable. The caller hands over input and output in pieces
 *   of any size, down to a byte, and the decoder picks up exactly where it
 *   stopped, mid-symbol if need be. A chapter can then be decoded straight
 *   from a small read buffer into a small layout buffer, without holding the
 *   whole compressed or decompressed entry in RAM.
 * - The decoder keeps its own 32 KiB history window. DEFLATE back-references
 *   reach up to 32768 bytes back, and the caller's output buffer may be much
 *   smaller, or already consumed, by then.
 * - The caller supplies the window's memory, and its size, which
 *   bartap_inflate_init checks (D11 in DECISIONS.md). It is the one large
 *   piece of state, and where it lives (a static buffer shared with something
 *   that is never active at the same time, or a memory bank chosen for speed
 *   or to stay out of another core's way) is a decision for whoever owns the
 *   memory map, not for the decoder.
 * - The decode tables live inside the state struct (D11). Their size is not
 *   part of the contract, so the table design can change.
 * - The state is a plain struct, so no allocation and no hidden static state:
 *   two entries can be decoded at once by keeping two. It is declared in full
 *   so that the OCF layer's stream can embed it; how the size of a public
 *   struct that embeds it is communicated is Q2.
 * - Total output is bounded by the caller: the decoder writes only into the
 *   output it is given, so a caller decoding a raw stream stops a
 *   decompression bomb by offering no more output than it will accept (D6 in
 *   DECISIONS.md; ocf.h does this with max_size).
 * - The decoder is told when the input has ended, so that a stream which
 *   stops before its final block is reported as truncated rather than left
 *   waiting for input that will never come.
 * - Everything RFC 1951 allows is accepted, and everything it rules out is
 *   rejected (BARTAP_INFLATE_RESULT_CORRUPT): a reserved block type, a stored
 *   block's length not matching its complement, a code that is
 *   over-subscribed or incomplete (except a code of a single one-bit symbol,
 *   which RFC 1951 allows for distances and zlib accepts for literal/lengths
 *   too), literal/length symbols 286 and 287, distance symbols 30 and 31,
 *   and a distance reaching back before the first byte of output. That is
 *   also exactly what zlib accepts, which lets the fixtures
 *   (tests/make_fixtures.py) use zlib as the oracle for damaged streams.
 *
 * Execution model: none of its own. It does no I/O and touches nothing outside
 * its state, so it runs wherever it is called.
 *
 * Bounded work:
 * - bartap_inflate_init: constant work.
 * - bartap_inflate_run: bounded by the caller. Its work is linear in in_size
 *   plus out_size: every step either consumes input or produces output, a
 *   dynamic block's tables cost a bounded amount per header, and a long
 *   match stops when out is full and resumes on the next call. To bound one
 *   call, bound the buffers passed to it. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bytes of history DEFLATE can refer back to, and so the smallest window the
 * caller may supply. */
#define BARTAP_INFLATE_WINDOW_SIZE 32768U

/* Values are fixed and the list is append-only: never renumbered or reused
 * (D18). */
typedef enum {
    /* Stopped because the input was used up (and more is still to come) or the
     * output was filled. Call again with more of whichever ran out. */
    BARTAP_INFLATE_RESULT_OK = 0,
    /* The end of the final block has been decoded and all of its output
     * delivered. Input after it is left unused (see bartap_inflate_run's
     * in_used). */
    BARTAP_INFLATE_RESULT_DONE = 1,
    /* The input ended, as the caller said it would, before the final block
     * did. */
    BARTAP_INFLATE_RESULT_TRUNCATED = 2,
    /* The stream breaks a rule of RFC 1951; see "Choices" above. Final: every
     * later call returns it again. Which rule, and where, is not reported:
     * codes only (D18). */
    BARTAP_INFLATE_RESULT_CORRUPT = 3,
    /* A NULL pointer where one is required, or a run on a state whose
     * bartap_inflate_init failed, which is final in the same way. */
    BARTAP_INFLATE_RESULT_INVALID_ARGUMENT = 4,
} bartap_inflate_result_t;

/* A canonical Huffman code, as RFC 1951 3.2.2 builds it: how many codes have
 * each length, and the symbols in code order. Private. */
typedef struct {
    uint16_t count[16];                 //codes of each length; count[0] unused
    uint16_t symbol[288];               //symbols ordered by code length, then value
    uint8_t max_length;                 //longest code; 0 for a code with no symbols
} bartap_inflate_code_t;

/* Decoder state. Private: declared here only so that it can be allocated by
 * whoever embeds it (Q2). Everything in it is for inflate.c to define, and the
 * decode tables' size is not part of the contract (D11). */
typedef struct {
    uint8_t *window;                    //at least BARTAP_INFLATE_WINDOW_SIZE bytes, from the caller
    uint32_t window_position;           //where the next output byte goes, mod BARTAP_INFLATE_WINDOW_SIZE
    uint32_t history;                   //bytes of output so far, capped at BARTAP_INFLATE_WINDOW_SIZE
    uint32_t bit_buffer;                //input bits taken but not yet used, least significant first
    uint8_t bit_count;
    bool final_block;                   //the block being decoded has BFINAL set
    uint8_t stage;                      //where decoding resumes; an enum in inflate.c
    bartap_inflate_result_t status;     //sticky once DONE, TRUNCATED, CORRUPT or INVALID_ARGUMENT
    uint32_t stored_left;               //bytes of the current stored block still to copy
    uint8_t stored_header[4];           //LEN and NLEN, gathered a byte at a time
    uint8_t stored_have;
    uint16_t match_length;              //of the match being decoded or copied
    uint16_t match_distance;
    uint16_t match_left;                //bytes of it still to copy out
    uint16_t pending_symbol;            //a decoded symbol waiting for its extra bits, or a literal for space
    uint16_t literal_count;             //HLIT, HDIST and HCLEN of a dynamic header
    uint16_t distance_count;
    uint16_t length_code_count;
    uint16_t lengths_have;              //code lengths read so far
    uint8_t lengths[286 + 30];          //code lengths of the dynamic header being read
    bartap_inflate_code_t length_code;  //the code-length code
    bartap_inflate_code_t literal_code; //literal/length code of the current block
    bartap_inflate_code_t distance_code;
} bartap_inflate_t;

/* Start decoding a new stream. window is window_size bytes, kept by the
 * caller for as long as state is in use.
 *
 * Always writes state's status, whatever the outcome (unless state itself is
 * NULL, when nothing is written). If window is NULL or window_size is smaller
 * than BARTAP_INFLATE_WINDOW_SIZE (D11), returns
 * BARTAP_INFLATE_RESULT_INVALID_ARGUMENT and records it as the status, so
 * every later bartap_inflate_run on that state returns it too, until the
 * state is initialized again.
 *
 * A window serves one live stream at a time. It may be given to another
 * stream once the stream using it has finished (DONE, or an error) or been
 * abandoned; two streams in progress must never share one. The window's
 * contents need not be cleared: a distance reaching before the first byte of
 * output is rejected, so stale bytes in it are never read. */
bartap_inflate_result_t bartap_inflate_init(bartap_inflate_t *state, uint8_t *window, size_t window_size);

/* Decode from in into out.
 *
 * Consumes up to in_size bytes, setting *in_used to how many; produces up to
 * out_size bytes, setting *out_produced to how many. Input that was not used
 * must be offered again, from that point, on the next call. input_ends is
 * true when in holds the last of the stream's input: if that runs out before
 * the final block ends, the result is BARTAP_INFLATE_RESULT_TRUNCATED.
 *
 * in may be NULL when in_size is 0, which is how the rest of the output is
 * collected once all the input has been given. */
bartap_inflate_result_t bartap_inflate_run(
    bartap_inflate_t *state,
    const uint8_t *in,
    size_t in_size,
    size_t *in_used,
    bool input_ends,
    uint8_t *out,
    size_t out_size,
    size_t *out_produced);

#endif
