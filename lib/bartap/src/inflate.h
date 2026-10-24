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
 * compressed entries as bare DEFLATE, and zip.h checks the CRC-32 itself.
 *
 * TODO: nothing here is implemented yet. What follows is a suggested
 * interface, not a settled one; the choices are explained beside each part so
 * that they are easy to revisit.
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
 * - The caller supplies the window's memory. It is the one large piece of
 *   state, and where it lives (a static buffer shared with something that is
 *   never active at the same time, or a memory bank chosen for speed or to
 *   stay out of another core's way) is a decision for whoever owns the memory
 *   map, not for the decoder. Whether the decode tables should be supplied
 *   the same way, and how a caller keeps both in fast memory while sending
 *   output elsewhere, is Q1.
 * - The state is a plain struct the caller allocates, so no allocation and no
 *   hidden static state: two entries can be decoded at once by keeping two.
 *   How its size is communicated, given that its contents are private, is Q2.
 * - Total output is bounded by the caller: the decoder writes only into the
 *   output it is given, so a caller decoding a raw stream stops a
 *   decompression bomb by offering no more output than it will accept (D6 in
 *   DECISIONS.md; zip.h does this with max_size).
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

/* Bytes of history DEFLATE can refer back to, and so the size of the window
 * the caller supplies. */
#define BARTAP_INFLATE_WINDOW_SIZE 32768U

typedef enum {
    /* Stopped because the input was used up (and more is still to come) or the
     * output was filled. Call again with more of whichever ran out. */
    BARTAP_INFLATE_RESULT_OK = 0,
    /* The end of the final block has been decoded and all of its output
     * delivered. Input after it is left unused (see bartap_inflate_run's
     * in_used). */
    BARTAP_INFLATE_RESULT_DONE,
    /* The input ended, as the caller said it would, before the final block
     * did. */
    BARTAP_INFLATE_RESULT_TRUNCATED,
    /* The stream breaks a rule of RFC 1951; see "Choices" above. Final: every
     * later call returns it again. Which rule, and where, is Q4. */
    BARTAP_INFLATE_RESULT_CORRUPT,
    BARTAP_INFLATE_RESULT_INVALID_ARGUMENT,
} bartap_inflate_result_t;

/* Decoder state. Private: declared here only so that callers can allocate it
 * themselves (Q2). Everything in it is for inflate.c to define.
 *
 * TODO: the rest of the resumable state goes here once the implementation
 * settles it - at least the current block's type and stage, a stored block's
 * remaining length, a match that has not been fully copied out, the code
 * lengths of a dynamic header that has not been fully read, and the decoding
 * tables for the current block (or a pointer to caller-supplied ones: Q1). */
typedef struct {
    uint8_t *window;            //BARTAP_INFLATE_WINDOW_SIZE bytes, from the caller
    uint32_t window_position;   //where the next output byte goes, mod BARTAP_INFLATE_WINDOW_SIZE
    uint32_t total_out;         //bytes produced so far; a distance must not exceed it
    uint32_t bit_buffer;        //input bits taken but not yet used, least significant first
    uint8_t bit_count;
    bool final_block;           //the block being decoded has BFINAL set
    bartap_inflate_result_t status; //sticky once DONE, TRUNCATED or CORRUPT
} bartap_inflate_t;

/* Start decoding a new stream. window is BARTAP_INFLATE_WINDOW_SIZE bytes,
 * kept by the caller for as long as state is in use. Its contents need not be
 * cleared: a distance reaching before the first byte of output is rejected,
 * so stale bytes in it are never read. */
void bartap_inflate_init(bartap_inflate_t *state, uint8_t *window);

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
