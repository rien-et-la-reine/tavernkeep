<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap host tests

Tests for the library in `include/bartap/` and `src/`: the byte source, the OCF layer and the DEFLATE
decoder, and later the book-level API. These are scaffolding for now. The interfaces are drafts and
nothing is implemented, so the test cases check only that the fixtures are where the build says they
are and that the headers compile together. The portability checks are real, though, and run already.

The tests may include the library's internal headers from `src/` (`#include "ocf.h"`) as well as its
public ones (`#include "bartap/epub.h"`).

From Bartap's root:

```sh
cmake -S . -B build -G Ninja
cmake --build build                 # also (re)generates the test inputs; needs python3
./build/tests/bartap_tests          # all cases, each in its own process
(cd build && ctest)                 # everything below
python3 tests/run_matrix.py         # all of it under gcc and clang, 64- and 32-bit, as available
```

`bartap_tests --list`, `bartap_tests NAME_SUBSTRING...`, `bartap_tests --shuffle SEED` and
`bartap_tests --no-fork` (one process, for a debugger) are also available.
`-DBARTAP_TESTS_SANITIZE=OFF` builds without AddressSanitizer and UndefinedBehaviorSanitizer, and
`-DBARTAP_TESTS_M32=ON` builds 32-bit.

## What ctest runs

| Test | What it checks |
|---|---|
| `bartap`, `bartap_shuffled` | The test cases, in order and in a shuffled order |
| `harness_selftest` | The harness reports a failing check, a case with no checks, and a crash |
| `include_check` | Every file in `include/` and `src/` includes only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>` and the public `"bartap/..."` headers; files in `src/` may also include the internal headers there by bare name (`"ocf.h"`), and public headers may not (`check_includes.cmake`) |
| `include_check_selftest` | That check finds all seven planted violations in `check_selftest/includes/public/` and `private/` |
| `symbol_check` | An uninstrumented build of the library has no writable data (no mutable global or static state) and calls nothing but `memcpy`, `memset`, `memcmp`, `memmove`, the stack protector's symbols and, on 32-bit x86, the linker's `_GLOBAL_OFFSET_TABLE_`: no `malloc`, `abort`, `assert` or stdio (`check_symbols.cmake`). The build includes one translation unit per header, public and internal, holding only its `#include`, so whatever a header defines is checked too; those units are compiled with `-Werror=unused-function -Werror=unused-variable`, so an unused static function or variable in a header fails the build rather than being optimised away unseen (an unused `static inline` function is allowed; the variable only under GCC, see below). Each unit also holds a typedef, which defines nothing but keeps a header that declares nothing yet (`epub.h`) from making an empty translation unit, which ISO C forbids |
| `symbol_check_selftest` | That check finds both planted violations in `check_selftest/symbols/` |
| `header_unit_selftest_unused_function`, `header_unit_selftest_unused_variable` | A header unit fails to compile on each planted header in `check_selftest/headers/`. The variable case runs under GCC only: clang never reports an unused static variable declared in a header, so under clang only the function is enforced, and the matrix's GCC builds catch the variable |

`symbol_check` covers the host build. Tavernkeep's firmware build runs the same check over its Arm
`libbartap.a`, where the Arm run-time ABI's `__aeabi_*` helpers are also allowed.

The library's rules apply to `include/` and `src/`. The tests themselves may use the whole C library and
POSIX: the runner needs stdio and `fork()`.

## Layout

| File | What it is |
|---|---|
| `harness.h`, `test_main.c` | `EXPECT`/`REQUIRE` macros and the runner. Each case runs in its own process, and a case that runs no checks fails |
| `test_scaffold.c` | The placeholder case. Replace it once there are real ones |
| `test_headers.c` | Includes every header, public and internal, together; checks the constants the specifications fix |
| `make_fixtures.py` | Generates the test inputs into the build directory |
| `check_includes.cmake`, `check_symbols.cmake`, `check_selftest/` | The portability checks and their planted violations |
| `run_matrix.py` | Runs everything under each compiler and word size available |
| `fixtures/pride-and-prejudice.epub` | A real book: Standard Ebooks' edition (its "advanced" EPUB 3 build, with SVG cover and title page). Its licensing is under Fixtures below |

To add a test file, define its case table as `test_scaffold.c` does, declare the table in `harness.h`
and list it in `suites[]` in `test_main.c`. The build passes two directories in as string literals:
`BARTAP_TESTS_FIXTURE_DIR` (this `fixtures/`) and `BARTAP_TESTS_GENERATED_DIR` (the generated inputs).

## Fixtures

Committed fixtures live in `fixtures/`. `.gitignore` ignores `*.epub` everywhere except
`fixtures/pride-and-prejudice.epub`, so to commit another book, add an exception for it there.

pride-and-prejudice.epub is from Standard Ebooks. Its own dc:rights metadata states the text and
artwork are believed to be in the US public domain and that Standard Ebooks' contributions are
dedicated under CC0 1.0.

Everything else is generated, never committed. The build runs `make_fixtures.py` into
`build/tests/generated/` whenever the script changes. To regenerate by hand:

```sh
python3 tests/make_fixtures.py                # into build/tests/generated/
python3 tests/make_fixtures.py --out DIR      # anywhere else
```

The script uses only the standard library and is deterministic: fixed seeds, fixed zip timestamps, and
two runs give identical files. It clears only the subdirectories it made, and refuses to write into a
directory holding anything else. `generated/manifest.json` lists every case with its files, a
description, and what reading it must give:

| Kind | What it holds | `expect` |
|---|---|---|
| `deflate` | Valid raw DEFLATE streams (`.deflate`) and their output (`.out`): empty (two ways), one byte, stored, fixed Huffman, dynamic Huffman, 258-byte matches (including an overlapping one), matches at distance 32768, and four kinds of multi-block stream | `decodes` |
| `deflate-truncated` | Each valid stream cut at three points, and no input at all. What a decoder produces before it notices must be a prefix of `output_prefix_of` | `truncated` |
| `deflate-bitflip` | Each valid stream with one random bit flipped, three times. Many still decode; zlib's verdict is the expectation, with its output as `.out` when it decodes | `decodes`, `truncated` or `corrupt` |
| `deflate-invalid` | One broken rule each: a reserved block type, a stored length mismatch, symbols 286 and 30, distances before the start of output (including 32768 after 32767 bytes), over-subscribed and incomplete code-length codes | `corrupt` |
| `zip` | Valid archives: normal, a 65535-byte comment, a comment holding a fake end record, data descriptors (flag bit 3), every entry stored, ZIP64 records with values that fit in 32 bits, a local header whose name bytes differ from the directory's (not compared, D17), and the end record's signature at and just before a backward-scan chunk boundary, for 64- and 512-byte scratch buffers (`scan_chunk_size`). Each entry's contents are in `zip/contents/` | `opens` |
| `zip-damaged` | Truncated, corrupted and unsupported versions of `zip/normal.zip` (and some of `zip/stored.zip` and `zip/zip64.zip`), and archives that are not EPUBs, each with a `reason` (`not a zip`, `corrupt`, `unsupported`, `not an epub`), where a reader may notice (`fails_at`), and the `entry` concerned | `reject` |
| `zip-warnings` | Archives that open with warnings (D19, D20), listed in `warnings`: mimetype not first, deflated, missing, empty, with an extra field, and a name flagged UTF-8 that is not (its entry's `name` is null and `name_hex` holds the bytes); and mimetype content accepted without a warning (trailing whitespace, a byte order mark). `entries` gives what reading them must give | `opens` |

zlib is the oracle. The script checks each valid stream against its expected output, each truncated
one for being incomplete, and each invalid one for zlib's error, before writing anything.
`src/inflate.h` accepts exactly what zlib accepts, which is what makes zlib's verdict on a random bit
flip a fair expectation. zlib can't produce a match at distance 32768 or choose its own match lengths,
so the script has a small fixed-Huffman encoder for those cases. The manifest records the zlib version:
a different zlib may compress the zlib-made streams to different bytes, but the expected outputs don't
change.
