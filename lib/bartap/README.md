<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap

Bartap is an allocation-free, streaming, platform-agnostic EPUB container and package library for small
devices.

It covers the zip container and its DEFLATE-compressed entries, the OCF container, the OPF package and
spine, navigation (the EPUB 3 navigation document or the EPUB 2 NCX), and EPUB CFI parsing and
resolution. Layout and rendering are left to the caller. Whether Bartap also turns XHTML content
documents into a styled text stream is open (Q17).

**Status: scaffolding.** The interfaces in `include/bartap/` are drafts and nothing is implemented yet.
Decisions are in [DECISIONS.md](DECISIONS.md), open questions in
[DESIGN-QUESTIONS.md](DESIGN-QUESTIONS.md).

## Rules the library keeps

These hold for everything in `include/` and `src/`; the reasons are in DECISIONS.md. The tests check
the header and memory rules mechanically.

- **Headers:** only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<string.h>` (for `memcpy`, `memset`
  and `memcmp`).
- **Names:** every public symbol and type starts with `bartap_`, and every macro with `BARTAP_`.
- **Memory:** no dynamic allocation and no global or static mutable state. The caller owns all state
  and provides all working memory and output buffers (D3).
- **I/O:** only through callbacks the caller supplies (D4).
- **Errors:** result codes. Nothing in the library aborts or asserts.
- **Bytes:** byte data is `uint8_t`, never plain `char`, and values in files are decoded byte by byte,
  with no assumptions about byte order or alignment (D7).
- **Bounded work:** every streaming call does a bounded amount of work and returns. Each header says
  which of its calls run to completion instead (D5).

## Building

From this directory:

```sh
cmake -S . -B build -G Ninja
cmake --build build
(cd build && ctest)                 # host tests and the portability checks
python3 tests/run_matrix.py         # the same under gcc and clang, 64- and 32-bit, as available
```

From another CMake project:

```cmake
add_subdirectory(path/to/bartap)
target_link_libraries(your_target PRIVATE bartap)
```

Inside another project, the tests are off (`BARTAP_BUILD_TESTS`) and warnings are not errors
(`BARTAP_WARNINGS_AS_ERRORS`). Both default to on when Bartap is built by itself. The library builds as
ISO C11 without compiler extensions, whatever the including project uses.

See [tests/README.md](tests/README.md) for the tests and their fixtures.

## Layout

| Path | What it is |
|---|---|
| `include/bartap/` | Public headers: `byte_source.h`, `zip.h`, `inflate.h` |
| `src/` | Implementation |
| `tests/` | Host tests, fixture generator, portability checks, the compiler-matrix runner |
| `DECISIONS.md` | Decisions that shape Bartap, with their reasoning |
| `DESIGN-QUESTIONS.md` | Open questions about the draft interfaces |

## Licence

MIT; see [LICENSE](LICENSE). Every file carries SPDX tags, with two exceptions:
- `LICENSE` itself, which is the licence text.
- The test fixture `tests/fixtures/pride-and-prejudice.epub`, which is not Bartap's work. Its licensing
  is described in [tests/README.md](tests/README.md#fixtures).
