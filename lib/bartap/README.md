<!--
SPDX-FileCopyrightText: 2026 Harsh Patel
SPDX-License-Identifier: MIT
-->

# Bartap

Bartap is an allocation-free, streaming, platform-agnostic EPUB container and package library for small
devices.

It reads what an EPUB file is made of: the zip container, the DEFLATE-compressed entries in it and,
later, the OCF container and OPF package documents that describe the book. Layout and rendering are out
of scope. Bartap hands its caller the book's documents and resources, and what is done with them is the
caller's business.

**Status: scaffolding.** The interfaces in `include/bartap/` are drafts, marked as such, and nothing is
implemented yet. The open design questions are in [DESIGN-QUESTIONS.md](DESIGN-QUESTIONS.md).

## Rules the library keeps

These hold for everything in `include/` and `src/`. The tests check the first and third mechanically.

- **Headers:** only `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<string.h>` (for `memcpy`, `memset`
  and `memcmp`). No stdio, stdlib, `assert.h` or platform headers.
- **Names:** every public symbol and type starts with `bartap_`, and every macro with `BARTAP_`.
- **Memory:** no global or static mutable state, and no dynamic allocation. All state lives in structs
  the caller owns, and the caller provides all working memory and output buffers, so the caller decides
  where each lives (SRAM, PSRAM, ...).
- **I/O:** only through callbacks the caller supplies. No file or OS calls.
- **Errors:** explicit result codes. Nothing in the library aborts or asserts.
- **Untrusted input:** values read from files use fixed-width integer types and are decoded from bytes
  explicitly, with no assumptions about the host's byte order or alignment.
- **Bounded work:** every streaming call does a bounded amount of work and returns, so a caller with
  real-time deadlines can interleave it with other work in a cooperative loop. Each header says which
  of its calls meet this and which run to completion.

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
| `DESIGN-QUESTIONS.md` | Open questions about the draft interfaces |

## Licence

MIT; see [LICENSE](LICENSE). Every file carries SPDX tags, with two exceptions:
- `LICENSE` itself, which is the licence text.
- The test fixture `tests/fixtures/pride-and-prejudice.epub`, which is not Bartap's work. Its licensing
  is described in [tests/README.md](tests/README.md#fixtures).
