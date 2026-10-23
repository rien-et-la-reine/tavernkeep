# EPUB container-layer host tests

Tests for `src/epub/`: the byte source (`byte_source.c`), the zip reader (`zip.c`) and the DEFLATE
decoder (`inflate.c`). These are scaffolding for now. The three modules declare a suggested interface
but implement nothing, and the only case checks that the fixtures are where the build says they are.
Standalone, like `tests/filesystem`.

```sh
cmake -S tests/epub -B build-epub-tests -G Ninja
cmake --build build-epub-tests            # also (re)generates the test inputs; needs python3
./build-epub-tests/epub_tests             # all cases, each in its own process
(cd build-epub-tests && ctest)            # the above, a shuffled order, and the harness selftest
```

`epub_tests --list`, `epub_tests NAME_SUBSTRING...`, `epub_tests --shuffle SEED` and
`epub_tests --no-fork` (one process, for a debugger) work as they do in `tests/filesystem`.
`-DEPUB_TESTS_SANITIZE=OFF` builds without AddressSanitizer and UndefinedBehaviorSanitizer.

## Layout

| File | What it is |
|---|---|
| `harness.h`, `test_main.c` | `EXPECT`/`REQUIRE` macros and the runner, adapted from `tests/filesystem` |
| `test_scaffold.c` | The placeholder case; replace it once there are real ones |
| `make_fixtures.py` | Generates the test inputs into the build directory |
| `fixtures/pride-and-prejudice.epub` | A real book: Standard Ebooks' edition (its "advanced" EPUB 3 build, with SVG cover and title page), public-domain text, CC0 markup |

To add a test file, define its case table as `test_scaffold.c` does, declare the table in `harness.h`
and list it in `suites[]` in `test_main.c`. The build passes two directories in as string literals:
`EPUB_TESTS_FIXTURE_DIR` (this `fixtures/`) and `EPUB_TESTS_GENERATED_DIR` (the generated inputs).

## Fixtures

Committed fixtures live in `fixtures/`. `.gitignore` ignores `*.epub` everywhere except
`fixtures/pride-and-prejudice.epub`, so to commit another book, add an exception for it there.

Everything else is generated, never committed. The build runs `make_fixtures.py` into
`build-epub-tests/generated/` whenever the script changes. To regenerate by hand:

```sh
python3 tests/epub/make_fixtures.py                # into build-epub-tests/generated/
python3 tests/epub/make_fixtures.py --out DIR      # anywhere else
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
| `zip` | Valid archives: normal, a 65535-byte comment, a comment holding a fake end record, data descriptors (flag bit 3), every entry stored, and mimetype not first. Each entry's contents are in `zip/contents/` | `opens` |
| `zip-damaged` | Truncated, corrupted and unsupported versions of `zip/normal.zip` (and one of `zip/stored.zip`), each with a `reason` (`not a zip`, `corrupt`, `unsupported`), where a reader may notice (`fails_at`), and the `entry` concerned | `reject` |

zlib is the oracle. The script checks each valid stream against its expected output, each truncated
one for being incomplete, and each invalid one for zlib's error, before writing anything. `inflate.h`
accepts exactly what zlib accepts, which is what makes zlib's verdict on a random bit flip a fair
expectation. zlib can't produce a match at distance 32768 or choose its own match lengths, so the
script has a small fixed-Huffman encoder for those cases. The manifest records the zlib version: a
different zlib may compress the zlib-made streams to different bytes, but the expected outputs don't
change.
