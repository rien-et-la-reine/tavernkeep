<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Implementation validation — 2026-10-28

These are host tests and a standalone Cortex-M33 cross-compile, not board measurements.
The implementation is still a draft API. No hardware was flashed and no SD cards were written.

## Reproduce the host checks

From the repository root (omit `-G Ninja` when Ninja is not installed):

```sh
cmake -S lib/bartap -B build-bartap
cmake --build build-bartap
ctest --test-dir build-bartap --output-on-failure

cmake -S lib/bartap -B build-bartap-clang -DCMAKE_C_COMPILER=clang \
  -DBARTAP_INFLATE_FAST_LUT=ON -DBARTAP_BUILD_FUZZERS=ON
cmake --build build-bartap-clang
ctest --test-dir build-bartap-clang --output-on-failure
```

Checked with GCC 15.2 and Clang 21.1.8, ASan/UBSan, warnings as errors, on x86-64.
The GCC run used the normal inflater; Clang used the fast lookup table. The host symbol/include checks
and their selftests passed. LeakSanitizer required running outside the development sandbox's tracing.

The functional suite includes:

- 163 generated DEFLATE/ZIP cases, including 24 additional deterministic zlib differential inputs.
  Inflater cases run with input/output chunks of 1/13, 7/1 and 512/511 bytes. Valid outputs are compared
  byte for byte; corrupt and truncated streams have explicit expected outcomes.
- Thirteen generated books, with metadata, spine, TOC, warning and fragment expectations.
- Exact text/whitespace/style tests, malformed nesting, nesting past the frame stack, table cells and
  structural containers, escaped `/` in hrefs, UTF-8 boundaries, long
  attributes, raw anchors and token round trips at every boundary of the small regression document.
- I/O and CRC failures, detached memory iteration, shifted bookmarks and extreme saved offsets.
- Every chapter of the 65-item Standard Ebooks fixture, comparing stream and memory tokens and checking
  block kinds/levels balance. A complete TOC walk also reaches verified EOF.

Opening that fixture uses **432 source reads** with the memory-backed counting adapter. The review
handoff reported 4,773 for the preceding implementation; that earlier count was not remeasured here.
This is an I/O-call count, not a FatFs/SD latency result.

Five sanitizer fuzz smoke runs of 45 seconds each: inflate 222,880 executions, XML 19,093, ZIP 514,926,
EPUB 12,835, text 152,486. No failures. (The text harness gives memory-mode opening a minimal local
header to read; without one every input was rejected at opening.) Inflater fuzzing uses
zlib as an oracle up to 1 MiB output; text fuzzing checks anchors and early-token seek round trips.
The runs used temporary corpus copies. They are short smoke tests, not a proof of safety. A subsequent
small whitespace fix for skipped images is covered by its dedicated regression and the corpus suite.

## Arm footprint and stack evidence

Compiled the seven library translation units with arm-none-eabi-gcc 15.2.1 (Arm GNU Toolchain
15.2.Rel1), without a Pico SDK:

```sh
arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -Os -ffreestanding -std=c11 \
  -Wall -Wextra -Wpedantic -Werror -fstack-usage \
  -Ilib/bartap/include -Ilib/bartap/src -c lib/bartap/src/text.c
```

Apply those flags to each source and archive the objects. The Arm symbol check passed: no writable
static/global data, and only the allowed mem/string functions were undefined. Summed object text and
read-only data were about **20.4 KiB**, excluding runtime/linker effects. Fast LUT was off.

| Caller-owned type | Arm bytes |
|---|---:|
| `bartap_book_t` | 856 |
| `bartap_work_t` (includes window) | 37,744 |
| `bartap_stream_t` (excludes window) | 1,608 |
| `bartap_text_t` (excludes window) | 3,928 |
| `bartap_toc_t` (excludes window) | 3,328 |
| `bartap_spine_item_t` | 28 |
| `bartap_loc_t` | 40 |

For 65 spine items: book + work + spine is 40,420 bytes while opening. Book + one text iterator + one
32 KiB window + spine is 39,372 bytes while streaming, assuming other opening scratch is reclaimed.
Keeping the whole work object alive adds its non-window storage. A live TOC/second stream needs its own
state and window. Inflated PSRAM chapter bytes, renderer, adapter, application and stack are additional.
Each embedded inflater grows by 512 bytes when the optional LUT is enabled.

The compiler's `.su` files report individual frames of 872 bytes for `bartap_open`, 760 for
`bartap_zip_open`, and 168 for `bartap_zip_read_cd_entry`: that call chain already totals 1,800 bytes
before source-read/callback and interrupt overhead. `bartap_toc_next` is 592 and
`bartap_text_generate` 336. This is a static estimate, not a measured high-water
mark. Do not assume a 2 KiB stack is safe. Start with at least 4 KiB reserved for integration and measure
the complete firmware including FatFs; increase it if required. Keep work, spine and iterators off the
small call stack.

## Deliberately deferred

No fragment index, full HTML tree builder, strict-mode switch, source cache, larger
CRC table or token-clearing optimization. Stream seek may inflate twice, and memory seek still replays
formatting; the tail buffer that would spare most stream seeks is decided (D16) but not implemented. Device opening/seek latency, PSRAM throughput and full firmware stack high-water marks are
not yet measured. ZIP64 remains limited to values that fit the existing 32-bit offsets and entry count.
