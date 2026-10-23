#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Generate inputs for Bartap's host tests.

Writes raw DEFLATE streams and small zip archives, each with what reading it must give, into a build
directory (never the source tree), and a manifest.json that lists and describes every case. Uses only
the standard library.

    python3 tests/make_fixtures.py [--out DIR]

Paths are relative to Bartap's root. The default DIR is build/tests/generated, the directory a build in
build/ itself uses, which Bartap's .gitignore already covers (/build*/). The CMake build runs this
script whenever it changes.

Layout of DIR:

    deflate/NAME.deflate, NAME.out    valid raw DEFLATE (RFC 1951, zlib wbits=-15) and its output
    deflate-truncated/NAME.deflate    a valid stream cut short
    deflate-bitflip/NAME.deflate      a valid stream with one bit flipped (NAME.out if it still decodes)
    deflate-invalid/NAME.deflate      streams built to break one rule of RFC 1951 each
    zip/NAME.zip                      valid archives
    zip/contents/PATH                 the contents of every file entry those archives hold
    zip-damaged/NAME.zip              truncated, corrupted and unsupported archives
    manifest.json                     every case: its files, what it covers, and what to expect

Determinism: the output depends only on this script, the Python version's zipfile and the zlib it links
(recorded in the manifest). Every random choice comes from a random.Random seeded from SEED and the
case's name, and every zip timestamp is fixed. Running the script twice gives identical files. A
different zlib may compress the zlib-made streams to different bytes; their expected outputs, and every
hand-encoded stream, do not change.

zlib is the oracle. Every valid stream is checked to decode to its expected output, every truncated one
to be incomplete, and every invalid one to be rejected, before anything is written. For bit-flipped
streams, whose damage is random, zlib's verdict is the expectation: bartap/inflate.h's acceptance rules
are chosen to be exactly zlib's.

zlib cannot produce some cases (its matches never reach the full 32768-byte distance, and it chooses
its own match lengths), so a small fixed-Huffman encoder here writes those by hand. It is an encoder
only; nothing in this script decodes DEFLATE except zlib.
"""

import argparse
import hashlib
import io
import json
import os
import random
import shutil
import struct
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DEFAULT_OUT = os.path.join(ROOT, "build", "tests", "generated")

SEED = 20261023

# the subdirectories this script makes and is therefore free to clear
OWNED_DIRS = ("deflate", "deflate-truncated", "deflate-bitflip", "deflate-invalid", "zip", "zip-damaged")
MANIFEST = "manifest.json"

# every entry in every archive is dated this (zip's DOS timestamps start in 1980)
ZIP_TIMESTAMP = (2026, 1, 1, 0, 0, 0)

BTYPE_NAMES = {0: "stored", 1: "fixed", 2: "dynamic", 3: "reserved"}


def rng(name):
    """A generator for one case, so adding or reordering cases never changes another's bytes."""
    return random.Random(SEED ^ zlib.crc32(name.encode()))


# --------------------------------------------------------------------------- content


WORDS = (
    "it is a truth universally acknowledged that single man in possession of good fortune must be want "
    "wife however little known the feelings or views such may on his first entering neighbourhood this "
    "so well fixed minds surrounding families he considered rightful property some one other their "
    "daughters my dear mister bennet said lady to him day have you heard netherfield park let at last"
).split()


def prose(r, size):
    """size bytes of English-like text: compressible, but with enough variety for dynamic Huffman."""
    out = []
    length = 0
    while length < size:
        sentence = " ".join(r.choice(WORDS) for _ in range(r.randint(6, 18)))
        sentence = sentence[0].upper() + sentence[1:] + (".\n\n" if r.random() < 0.2 else ". ")
        out.append(sentence)
        length += len(sentence)
    return "".join(out).encode("ascii")[:size]


def noise(r, size):
    """size incompressible bytes."""
    return bytes(r.getrandbits(8) for _ in range(size))


# --------------------------------------------------------------------------- DEFLATE encoding by hand

# RFC 1951 3.2.5: literal/length symbols 257..285 and distance symbols 0..29
LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
               131, 163, 195, 227, 258]
LENGTH_EXTRA = [0] * 8 + [1] * 4 + [2] * 4 + [3] * 4 + [4] * 4 + [5] * 4 + [0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
             2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
              13, 13]


class BitWriter:
    """Packs bits as DEFLATE does: data fields least significant bit first, Huffman codes most."""

    def __init__(self):
        self.out = bytearray()
        self.pending = 0
        self.count = 0

    def bits(self, value, count):
        assert 0 <= value < (1 << count) or count == 0
        self.pending |= value << self.count
        self.count += count
        while self.count >= 8:
            self.out.append(self.pending & 0xFF)
            self.pending >>= 8
            self.count -= 8

    def code(self, code, length):
        self.bits(int(format(code, "0%db" % length)[::-1], 2), length)

    def align(self):
        if self.count:
            self.bits(0, 8 - self.count)

    def raw(self, data):
        assert self.count == 0
        self.out += data

    def getvalue(self):
        self.align()
        return bytes(self.out)


def fixed_literal_code(symbol):
    """RFC 1951 3.2.6, the fixed literal/length code (symbols 286 and 287 have codes but are invalid)."""
    if symbol <= 143:
        return 0x30 + symbol, 8
    if symbol <= 255:
        return 0x190 + symbol - 144, 9
    if symbol <= 279:
        return symbol - 256, 7
    return 0xC0 + symbol - 280, 8


def stored_block(w, final, payload, nlen=None):
    w.bits(final, 1)
    w.bits(0, 2)
    w.align()
    length = len(payload)
    w.raw(struct.pack("<HH", length, (~length & 0xFFFF) if nlen is None else nlen))
    w.raw(payload)


def fixed_block(w, final, ops):
    """ops: ints are literals; ("match", length, distance); ("symbol", n) and ("distance_symbol", n)
    write a raw literal/length or distance symbol, for the invalid cases. The end-of-block symbol is
    added."""
    w.bits(final, 1)
    w.bits(1, 2)
    for op in ops:
        if isinstance(op, int):
            w.code(*fixed_literal_code(op))
        elif op[0] == "symbol":
            w.code(*fixed_literal_code(op[1]))
        elif op[0] == "distance_symbol":
            w.code(op[1], 5)
        else:
            _, length, distance = op
            i = max(k for k, base in enumerate(LENGTH_BASE) if base <= length)
            w.code(*fixed_literal_code(257 + i))
            w.bits(length - LENGTH_BASE[i], LENGTH_EXTRA[i])
            j = max(k for k, base in enumerate(DIST_BASE) if base <= distance)
            w.code(j, 5)
            w.bits(distance - DIST_BASE[j], DIST_EXTRA[j])
    w.code(*fixed_literal_code(256))


def literals(data):
    return list(data)


# --------------------------------------------------------------------------- zlib as encoder and oracle


def zlib_deflate(data, level=9, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return c.compress(data) + c.flush()


def zlib_inflate(stream):
    """("ok", output, trailing byte count), ("truncated", partial output, 0) or ("error", message, 0)."""
    d = zlib.decompressobj(-15)
    try:
        out = d.decompress(stream)
    except zlib.error as e:
        return "error", str(e), 0
    if d.eof:
        return "ok", out, len(d.unused_data)
    return "truncated", out, 0


def first_block(stream):
    """(BFINAL, BTYPE name) of a stream's first block header."""
    return stream[0] & 1, BTYPE_NAMES[(stream[0] >> 1) & 3]


# --------------------------------------------------------------------------- output


class Fixtures:
    def __init__(self):
        self.files = {}
        self.cases = []
        self.valid_streams = []     # (name, stream, expected), for the damaged copies

    def file(self, path, data):
        assert path not in self.files, path
        self.files[path] = bytes(data)
        return path

    def case(self, **case):
        assert all(c["name"] != case["name"] or c["kind"] != case["kind"] for c in self.cases), case["name"]
        self.cases.append(case)

    def write(self, out):
        if os.path.exists(out):
            leftovers = set(os.listdir(out)) - set(OWNED_DIRS) - {MANIFEST}
            if leftovers and not os.path.exists(os.path.join(out, MANIFEST)):
                sys.exit("%s holds files this script did not make (%s); refusing to clear it"
                         % (out, ", ".join(sorted(leftovers))))
            for d in OWNED_DIRS:
                shutil.rmtree(os.path.join(out, d), ignore_errors=True)
        for path, data in sorted(self.files.items()):
            full = os.path.join(out, *path.split("/"))
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as f:
                f.write(data)
        manifest = {
            "about": "Inputs for Bartap's host tests, made by tests/make_fixtures.py. "
                     "Paths are relative to this file.",
            "seed": SEED,
            "zlib": zlib.ZLIB_RUNTIME_VERSION,
            "python": "%d.%d" % sys.version_info[:2],
            "cases": self.cases,
        }
        # written last, and atomically, so that a manifest only ever describes a complete set
        with open(os.path.join(out, MANIFEST + ".tmp"), "w") as f:
            json.dump(manifest, f, indent=1)
            f.write("\n")
        os.replace(os.path.join(out, MANIFEST + ".tmp"), os.path.join(out, MANIFEST))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------------------- valid DEFLATE streams


def add_stream(fx, name, stream, expected, description, producer, first=None):
    """A valid stream. first is the (BFINAL, block type) its first block must have, which checks that
    zlib really produced the kind of stream the case is about."""
    verdict, output, trailing = zlib_inflate(stream)
    assert verdict == "ok" and output == expected and trailing == 0, (name, verdict)
    if first is not None:
        assert first_block(stream) == first, (name, first_block(stream), first)
    final, btype = first_block(stream)
    fx.case(kind="deflate", name=name, description=description, producer=producer,
            input=fx.file("deflate/%s.deflate" % name, stream),
            expected=fx.file("deflate/%s.out" % name, expected),
            expect="decodes", first_block={"final": bool(final), "type": btype},
            input_size=len(stream), output_size=len(expected), input_sha256=sha256(stream))
    fx.valid_streams.append((name, stream, expected))


def deflate_cases(fx):
    add_stream(fx, "empty", zlib_deflate(b""), b"",
               "Empty input, as zlib encodes it: one final fixed-Huffman block holding only end-of-block.",
               "zlib", (1, "fixed"))

    w = BitWriter()
    stored_block(w, 1, b"")
    add_stream(fx, "empty_stored", w.getvalue(), b"",
               "Empty input as one final stored block of length 0 (LEN 0, NLEN 0xFFFF).",
               "hand", (1, "stored"))

    add_stream(fx, "one_byte", zlib_deflate(b"T"), b"T",
               "A single byte: one fixed-Huffman literal, then end-of-block.", "zlib", (1, "fixed"))

    data = noise(rng("stored"), 1000)
    add_stream(fx, "stored", zlib_deflate(data, level=0), data,
               "1000 random bytes at level 0: a single final stored block.", "zlib", (1, "stored"))

    data = prose(rng("fixed"), 2000)
    add_stream(fx, "fixed", zlib_deflate(data, strategy=zlib.Z_FIXED), data,
               "2000 bytes of text with Z_FIXED: one final fixed-Huffman block of literals and matches.",
               "zlib", (1, "fixed"))

    data = prose(rng("dynamic"), 4000)
    add_stream(fx, "dynamic", zlib_deflate(data), data,
               "4000 bytes of text at level 9: one final dynamic-Huffman block.", "zlib", (1, "dynamic"))

    r = noise(rng("match_258"), 258)
    w = BitWriter()
    fixed_block(w, 1, literals(r) + [("match", 258, 258), ("match", 258, 1)])
    add_stream(fx, "match_258", w.getvalue(), r + r + bytes([r[-1]]) * 258,
               "Matches of the longest length, 258 (symbol 285, no extra bits): one copying the 258 "
               "literals before it, then one at distance 1, which overlaps itself and repeats the last "
               "byte 258 times. Fixed Huffman, hand-encoded.", "hand", (1, "fixed"))

    r = noise(rng("distance_32768"), 32768)
    w = BitWriter()
    fixed_block(w, 1, literals(r) + [("match", 258, 32768), ("match", 3, 32768)])
    add_stream(fx, "distance_32768", w.getvalue(), r + r[:258] + r[258:261],
               "Matches at the longest distance, 32768 (symbol 29, 13 extra bits all set), after 32768 "
               "literals: the first reaches exactly back to the first byte of output. zlib never emits "
               "this distance (its encoder stops 262 short), so it is hand-encoded.", "hand",
               (1, "fixed"))

    data = noise(rng("multi_block_stored"), 70000)
    add_stream(fx, "multi_block_stored", zlib_deflate(data, level=0), data,
               "70000 random bytes at level 0: more than one stored block can hold (65535), so several "
               "stored blocks, only the last final.", "zlib", (0, "stored"))

    r = rng("multi_block_flushed")
    parts = [prose(r, 3000), prose(r, 3000), prose(r, 3000)]
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
    stream = (c.compress(parts[0]) + c.flush(zlib.Z_SYNC_FLUSH) + c.compress(parts[1])
              + c.flush(zlib.Z_SYNC_FLUSH) + c.compress(parts[2]) + c.flush())
    assert stream.count(b"\x00\x00\xff\xff") >= 2
    add_stream(fx, "multi_block_flushed", stream, b"".join(parts),
               "Three runs of text with a sync flush after the first two: each flush ends the block and "
               "adds an empty non-final stored block (00 00 FF FF), and later blocks match into earlier "
               "ones across those boundaries.", "zlib", (0, "dynamic"))

    r = rng("multi_block_mixed")
    head = prose(r, 600)
    tail = prose(r, 3000)
    tail_stream = zlib_deflate(tail)
    assert first_block(tail_stream) == (1, "dynamic")
    w = BitWriter()
    stored_block(w, 0, head)
    fixed_block(w, 0, literals(b"Mixed: ") + [("match", 40, 607), ("match", 100, 300)])
    stored_block(w, 0, b"")
    w.raw(tail_stream)
    middle = b"Mixed: " + head[:40]
    middle += (head + middle)[len(head) + len(middle) - 300:][:100]
    add_stream(fx, "multi_block_mixed", w.getvalue(), head + middle + tail,
               "One block of each type in turn: a non-final stored block of text; a non-final fixed "
               "block whose matches reach back into the stored block; an empty stored block to return "
               "to a byte boundary; then a final dynamic block (zlib's).", "hand and zlib",
               (0, "stored"))

    data = prose(rng("multi_block_large"), 300000)
    add_stream(fx, "multi_block_large", zlib_deflate(data, level=6), data,
               "300000 bytes of text at level 6: zlib splits it into dynamic blocks by itself.", "zlib",
               (0, "dynamic"))


# --------------------------------------------------------------------------- damaged DEFLATE streams


def truncated_cases(fx):
    fx.case(kind="deflate-truncated", name="zero_bytes",
            description="No input at all, declared final: a stream must have at least one block.",
            input=fx.file("deflate-truncated/zero_bytes.deflate", b""), expect="truncated",
            output_prefix_of=None)
    for name, stream, _ in fx.valid_streams:
        for cut in sorted({1, len(stream) // 2, len(stream) - 1}):
            if not 0 < cut < len(stream):
                continue
            damaged = stream[:cut]
            verdict, partial, _ = zlib_inflate(damaged)
            # a prefix of a valid stream is valid as far as it goes, so it can only run out
            assert verdict == "truncated", (name, cut, verdict)
            case_name = "%s.cut%d" % (name, cut)
            fx.case(kind="deflate-truncated", name=case_name,
                    description="deflate/%s.deflate cut to its first %d of %d bytes." % (name, cut,
                                                                                      len(stream)),
                    input=fx.file("deflate-truncated/%s.deflate" % case_name, damaged), expect="truncated",
                    output_prefix_of="deflate/%s.out" % name, zlib_output_size=len(partial))


def bitflip_cases(fx):
    for name, stream, expected in fx.valid_streams:
        r = rng("bitflip:" + name)
        seen = set()
        for _ in range(3):
            bit = r.randrange(len(stream) * 8)
            if bit in seen:
                continue
            seen.add(bit)
            damaged = bytearray(stream)
            damaged[bit // 8] ^= 1 << (bit % 8)
            damaged = bytes(damaged)
            verdict, result, trailing = zlib_inflate(damaged)
            case_name = "%s.bit%d" % (name, bit)
            case = dict(kind="deflate-bitflip", name=case_name,
                        description="deflate/%s.deflate with bit %d of byte %d flipped (bit %d of the "
                                    "stream)." % (name, bit % 8, bit // 8, bit),
                        input=fx.file("deflate-bitflip/%s.deflate" % case_name, damaged))
            if verdict == "ok":
                case.update(expect="decodes", expected=fx.file("deflate-bitflip/%s.out" % case_name, result),
                            same_as_original=result == expected, trailing_bytes=trailing)
            elif verdict == "truncated":
                case.update(expect="truncated")
            else:
                case.update(expect="corrupt", zlib_message=result)
            fx.case(**case)


def invalid_case(fx, name, stream, description, zlib_message):
    verdict, message, _ = zlib_inflate(stream)
    assert verdict == "error" and message.endswith(zlib_message), (name, verdict, message)
    fx.case(kind="deflate-invalid", name=name, description=description, expect="corrupt",
            zlib_message=message, input=fx.file("deflate-invalid/%s.deflate" % name, stream))


def invalid_cases(fx):
    w = BitWriter()
    w.bits(1, 1)
    w.bits(3, 2)
    invalid_case(fx, "reserved_block_type", w.getvalue(),
                 "A final block of type 3, which RFC 1951 reserves.", "invalid block type")

    w = BitWriter()
    stored_block(w, 1, b"hello", nlen=0x0000)
    invalid_case(fx, "stored_length_mismatch", w.getvalue(),
                 "A stored block whose NLEN (0) is not the complement of LEN (5).",
                 "invalid stored block lengths")

    w = BitWriter()
    fixed_block(w, 1, literals(b"abc") + [("symbol", 286)])
    invalid_case(fx, "length_symbol_286", w.getvalue(),
                 "Literal/length symbol 286 in a fixed block: it has a code but no meaning.",
                 "invalid literal/length code")

    w = BitWriter()
    fixed_block(w, 1, literals(b"abc") + [("symbol", 257), ("distance_symbol", 30)])
    invalid_case(fx, "distance_symbol_30", w.getvalue(),
                 "Distance symbol 30 after a length of 3: it has a code but no meaning.",
                 "invalid distance code")

    w = BitWriter()
    fixed_block(w, 1, literals(b"abc") + [("match", 3, 4)])
    invalid_case(fx, "distance_before_start", w.getvalue(),
                 "A match at distance 4 after only 3 bytes of output.", "invalid distance too far back")

    r = noise(rng("distance_32768_too_far"), 32767)
    w = BitWriter()
    fixed_block(w, 1, literals(r) + [("match", 3, 32768)])
    invalid_case(fx, "distance_32768_too_far", w.getvalue(),
                 "A match at distance 32768 after only 32767 bytes of output: deflate/distance_32768 with "
                 "one literal fewer.", "invalid distance too far back")

    w = BitWriter()
    w.bits(1, 1)
    w.bits(2, 2)
    w.bits(0, 5)        # HLIT: 257 literal/length codes
    w.bits(0, 5)        # HDIST: 1 distance code
    w.bits(0, 4)        # HCLEN: 4 code-length code lengths, for symbols 16, 17, 18 and 0
    for _ in range(4):
        w.bits(1, 3)
    invalid_case(fx, "code_lengths_oversubscribed", w.getvalue(),
                 "A dynamic block header giving four code-length symbols a length of 1 bit: more codes "
                 "than one bit can hold.", "invalid code lengths set")

    w = BitWriter()
    w.bits(1, 1)
    w.bits(2, 2)
    w.bits(0, 5)
    w.bits(0, 5)
    w.bits(0, 4)
    for length in (0, 0, 1, 2):
        w.bits(length, 3)
    invalid_case(fx, "code_lengths_incomplete", w.getvalue(),
                 "A dynamic block header whose code-length code has lengths 1 and 2 only: an incomplete "
                 "code, which RFC 1951 does not allow here.", "invalid code lengths set")


# --------------------------------------------------------------------------- zip archives

CONTAINER_XML = b"""<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
\t<rootfiles>
\t\t<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
\t</rootfiles>
</container>
"""

CONTENT_OPF = b"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
\t<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
\t\t<dc:identifier id="uid">urn:uuid:00000000-0000-4000-8000-000000000000</dc:identifier>
\t\t<dc:title>Bartap test fixture</dc:title>
\t\t<dc:language>en</dc:language>
\t\t<meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
\t</metadata>
\t<manifest>
\t\t<item id="chapter-1" href="chapter-1.xhtml" media-type="application/xhtml+xml"/>
\t</manifest>
\t<spine>
\t\t<itemref idref="chapter-1"/>
\t</spine>
</package>
"""

CHAPTER = "OEBPS/chapter-1.xhtml"


def zip_entries():
    """(name, contents) of the archive every zip case is built from, in its normal order. A name
    ending in "/" is a directory entry."""
    r = rng("zip")
    chapter = (b'<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml">\n'
               b"<head><title>Chapter 1</title></head>\n<body>\n<p>" + prose(r, 20000)
               + b"</p>\n</body>\n</html>\n")
    return [
        ("mimetype", b"application/epub+zip"),
        ("META-INF/", b""),
        ("META-INF/container.xml", CONTAINER_XML),
        ("OEBPS/", b""),
        ("OEBPS/content.opf", CONTENT_OPF),
        (CHAPTER, chapter),
        ("OEBPS/empty.txt", b""),
        ("OEBPS/images/noise.bin", noise(r, 4096)),
    ]


def default_method(name):
    """As an EPUB is made: mimetype and directories stored, everything else deflated."""
    return zipfile.ZIP_STORED if name == "mimetype" or name.endswith("/") else zipfile.ZIP_DEFLATED


class _Unseekable:
    """A write-only stream with no tell() or seek(), so zipfile cannot go back to fill in local headers
    and writes a data descriptor after each entry instead (general-purpose flag bit 3)."""

    def __init__(self):
        self.data = bytearray()

    def write(self, b):
        self.data += b
        return len(b)

    def flush(self):
        pass


def build_zip(entries, method=default_method, comment=b"", seekable=True):
    fp = io.BytesIO() if seekable else _Unseekable()
    with zipfile.ZipFile(fp, "w") as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=ZIP_TIMESTAMP)
            info.create_system = 3      # Unix, whatever platform runs this
            if name.endswith("/"):
                info.external_attr = (0o40755 << 16) | 0x10
            else:
                info.external_attr = 0o100644 << 16
            m = method(name)
            info.compress_type = m
            z.writestr(info, data, compress_type=m, compresslevel=9 if m == zipfile.ZIP_DEFLATED else None)
        z.comment = comment
    data = fp.getvalue() if seekable else bytes(fp.data)
    with zipfile.ZipFile(io.BytesIO(without_comment(data, comment))) as z:
        assert z.testzip() is None
        assert [(i.filename, z.read(i)) for i in z.infolist()] == list(entries)
    return data


def without_comment(data, comment):
    """data, an archive ending in comment, with the comment taken off, for zipfile to read back. zipfile
    takes the last end-record signature it finds without checking that the comment length reaches the
    end of the file, so comment_with_fake_end_record fools it."""
    if not comment:
        return data
    bare = data[:len(data) - len(comment)]
    return bare[:-2] + b"\x00\x00"


# offsets of the fields that the damaged cases change (APPNOTE.TXT 4.3.7, 4.3.12, 4.3.16)
LOCAL = struct.Struct("<4sHHHHHIIIHH")                  # 30 bytes
CENTRAL = struct.Struct("<4sHHHHHHIIIHHHHHII")          # 46 bytes
END = struct.Struct("<4sHHHHIIH")                       # 22 bytes
LOCAL_FLAGS, LOCAL_METHOD, LOCAL_NAME = 6, 8, 30
CENTRAL_FLAGS, CENTRAL_METHOD, CENTRAL_CRC = 8, 10, 16
CENTRAL_COMPRESSED, CENTRAL_UNCOMPRESSED, CENTRAL_LOCAL_OFFSET = 20, 24, 42
END_DISK, END_ENTRIES_ON_DISK, END_ENTRIES = 4, 8, 10
END_DIRECTORY_SIZE, END_DIRECTORY_OFFSET = 12, 16


def zip_layout(data):
    """Where everything is in an archive this script built (no comment). Test tooling only: it trusts
    its input, which a real reader must not."""
    end = len(data) - END.size
    sig, _, _, _, count, size, offset, comment = END.unpack_from(data, end)
    assert sig == b"PK\x05\x06" and comment == 0
    entries = {}
    pos = offset
    for _ in range(count):
        fields = CENTRAL.unpack_from(data, pos)
        assert fields[0] == b"PK\x01\x02"
        name_len, extra_len, comment_len, local = fields[10], fields[11], fields[12], fields[16]
        name = data[pos + CENTRAL.size:pos + CENTRAL.size + name_len].decode()
        lfields = LOCAL.unpack_from(data, local)
        entries[name] = {"central": pos, "local": local, "compressed_size": fields[8],
                         "data": local + LOCAL.size + lfields[9] + lfields[10]}
        pos += CENTRAL.size + name_len + extra_len + comment_len
    assert pos == offset + size
    return {"end": end, "directory": offset, "directory_size": size, "entries": entries}


def patch(data, *changes):
    """Copy data with (offset, struct format, value) fields overwritten."""
    out = bytearray(data)
    for offset, fmt, value in changes:
        struct.pack_into(fmt, out, offset, value)
    assert bytes(out) != data
    return bytes(out)


def flip_byte(data, offset):
    out = bytearray(data)
    out[offset] ^= 0xFF
    return bytes(out)


def add_zip(fx, name, data, entries, method, description, comment=b""):
    with zipfile.ZipFile(io.BytesIO(without_comment(data, comment))) as z:
        infos = z.infolist()
    fx.case(kind="zip", name=name, description=description, expect="opens",
            input=fx.file("zip/%s.zip" % name, data), input_size=len(data), input_sha256=sha256(data),
            entries=[{"name": n,
                      "method": "stored" if method(n) == zipfile.ZIP_STORED else "deflated",
                      "size": len(contents), "crc32": zlib.crc32(contents),
                      "flags": info.flag_bits,
                      "contents": None if n.endswith("/") else "zip/contents/" + n}
                     for (n, contents), info in zip(entries, infos)])


def add_damaged_zip(fx, name, data, description, reason, fails_at, entry=None):
    """reason: "not a zip", "corrupt" or "unsupported". fails_at: where a reader may notice, as a list,
    since a stricter reader may notice earlier: "open", "directory" (listing or finding entries), or
    "entry" (reading entry's contents)."""
    fx.case(kind="zip-damaged", name=name, description=description, expect="reject", reason=reason,
            fails_at=fails_at, entry=entry, input=fx.file("zip-damaged/%s.zip" % name, data))


def zip_cases(fx):
    entries = zip_entries()
    for name, contents in entries:
        if not name.endswith("/"):
            fx.file("zip/contents/" + name, contents)

    normal = build_zip(entries)
    add_zip(fx, "normal", normal, entries, default_method,
            "An EPUB-shaped archive as EPUBs are made: mimetype first and stored, directory entries, and "
            "deflated files, among them an empty one (deflate's two-byte empty stream) and one of random "
            "bytes (which deflate stores in stored blocks).")

    comment = bytes(rng("comment").choice(b"abcdefghijklmnopqrstuvwxyz ") for _ in range(65535))
    data = build_zip(entries, comment=comment)
    assert data[-65535 - END.size:-65535 - END.size + 4] == b"PK\x05\x06"
    add_zip(fx, "comment_65535", data, entries, default_method,
            "normal with the longest archive comment, 65535 bytes, so the end of central directory "
            "record starts 65557 bytes from the end of the file.", comment)

    fake_end = END.pack(b"PK\x05\x06", 0, 0, 1, 1, 46, 0, 0)
    comment = b"x" * 900 + fake_end + b"y" * 100
    data = build_zip(entries, comment=comment)
    add_zip(fx, "comment_with_fake_end_record", data, entries, default_method,
            "normal with a 1022-byte comment that contains an end of central directory signature and "
            "plausible fields 100 bytes from the end of the file. Its comment length (0) does not reach "
            "the end of the file, so it is not the record; a reader that takes the first signature found "
            "scanning backwards gets it wrong, as Python's zipfile does.", comment)

    data = build_zip(entries, seekable=False)
    layout = zip_layout(data)
    for e in layout["entries"].values():
        flags, _, _, _, crc, csize, usize = struct.unpack_from("<HHHHIII", data, e["local"] + LOCAL_FLAGS)
        assert flags & 0x08 and crc == csize == usize == 0
    add_zip(fx, "data_descriptor", data, entries, default_method,
            "normal written as a streaming writer does: every entry has general-purpose flag bit 3 set, "
            "zero CRC-32 and sizes in its local header, and a data descriptor (with its optional "
            "signature) after its data. Only the central directory has the real values.")

    data = build_zip(entries, method=lambda n: zipfile.ZIP_STORED)
    add_zip(fx, "stored", data, entries, lambda n: zipfile.ZIP_STORED,
            "normal with every entry stored, so a stored entry's contents (the 20 KB chapter) span many "
            "reads.")

    reordered = entries[1:] + entries[:1]
    data = build_zip(reordered)
    add_zip(fx, "mimetype_not_first", data, reordered, default_method,
            "normal with mimetype moved to the end. A valid zip archive, which the zip reader must open; "
            "it breaks the OCF rule that mimetype comes first, which is for the layer above to refuse.")

    damaged_zip_cases(fx, normal, build_zip(entries, method=lambda n: zipfile.ZIP_STORED))


def damaged_zip_cases(fx, normal, stored):
    n = zip_layout(normal)
    s = zip_layout(stored)
    ch = n["entries"][CHAPTER]
    end = n["end"]

    add_damaged_zip(fx, "truncated_end_record", normal[:-5],
                    "zip/normal.zip without its last 5 bytes, which are inside the end of central "
                    "directory record.", "not a zip", ["open"])
    add_damaged_zip(fx, "truncated_in_directory", normal[:n["directory"] + n["directory_size"] // 2],
                    "zip/normal.zip cut halfway through its central directory: no end record.",
                    "not a zip", ["open"])
    add_damaged_zip(fx, "truncated_to_20_bytes", normal[:20],
                    "The first 20 bytes of zip/normal.zip: shorter than an end record (22 bytes).",
                    "not a zip", ["open"])
    add_damaged_zip(fx, "bad_end_record_signature", flip_byte(normal, end + 3),
                    "zip/normal.zip with the last byte of the end record's signature changed.",
                    "not a zip", ["open"])
    add_damaged_zip(fx, "directory_offset_out_of_range",
                    patch(normal, (end + END_DIRECTORY_OFFSET, "<I", len(normal))),
                    "zip/normal.zip with the end record's central directory offset set to the size of "
                    "the file.", "corrupt", ["open"])
    add_damaged_zip(fx, "directory_size_too_large",
                    patch(normal, (end + END_DIRECTORY_SIZE, "<I", n["directory_size"] + 1000)),
                    "zip/normal.zip with the end record's central directory size 1000 too large, so the "
                    "directory would run into the end record and past the end of the file.", "corrupt",
                    ["open"])
    count = len(n["entries"])
    add_damaged_zip(fx, "entry_count_too_high",
                    patch(normal, (end + END_ENTRIES_ON_DISK, "<H", count + 1),
                          (end + END_ENTRIES, "<H", count + 1)),
                    "zip/normal.zip with both entry counts in the end record one higher than the central "
                    "directory holds: walking it runs into the end record.", "corrupt",
                    ["open", "directory"])
    add_damaged_zip(fx, "directory_entry_bad_signature",
                    flip_byte(normal, n["entries"]["META-INF/container.xml"]["central"] + 3),
                    "zip/normal.zip with the signature of its third central directory entry changed.",
                    "corrupt", ["open", "directory"])
    add_damaged_zip(fx, "split_archive", patch(normal, (end + END_DISK, "<H", 1)),
                    "zip/normal.zip claiming to be disk 1 of a split archive.", "unsupported", ["open"])

    add_damaged_zip(fx, "local_header_bad_signature", flip_byte(normal, ch["local"] + 3),
                    "zip/normal.zip with the signature of the chapter's local header changed. Every other "
                    "entry still reads.", "corrupt", ["directory", "entry"], CHAPTER)
    add_damaged_zip(fx, "local_header_offset_out_of_range",
                    patch(normal, (ch["central"] + CENTRAL_LOCAL_OFFSET, "<I", 0xFFFFFFF0)),
                    "zip/normal.zip with the chapter's local header offset, in the central directory, far "
                    "past the end of the file.", "corrupt", ["directory", "entry"], CHAPTER)
    add_damaged_zip(fx, "local_header_name_mismatch", flip_byte(normal, ch["local"] + LOCAL_NAME),
                    "zip/normal.zip with the first byte of the chapter's name changed in its local header "
                    "only, so the local header and the central directory disagree.", "corrupt", ["entry"],
                    CHAPTER)
    add_damaged_zip(fx, "crc_mismatch", patch(normal, (ch["central"] + CENTRAL_CRC, "<I",
                                                       zlib.crc32(dict(zip_entries())[CHAPTER]) ^ 1)),
                    "zip/normal.zip with the chapter's CRC-32 in the central directory off by one bit. "
                    "The contents decompress, and only the check at the end catches it.", "corrupt",
                    ["entry"], CHAPTER)
    sch = s["entries"][CHAPTER]
    add_damaged_zip(fx, "stored_data_corrupt",
                    flip_byte(stored, sch["data"] + sch["compressed_size"] // 2),
                    "zip/stored.zip with one byte in the middle of the chapter's stored data changed: "
                    "only the CRC-32 catches it.", "corrupt", ["entry"], CHAPTER)
    damaged = flip_byte(normal, ch["data"] + ch["compressed_size"] // 2)
    verdict, _, _ = zlib_inflate(damaged[ch["data"]:ch["data"] + ch["compressed_size"]])
    add_damaged_zip(fx, "deflated_data_corrupt", damaged,
                    "zip/normal.zip with one byte in the middle of the chapter's deflated data changed. "
                    "zlib's verdict on the damaged stream: %s; if it decodes, the CRC-32 or the size "
                    "catches it." % verdict, "corrupt", ["entry"], CHAPTER)
    usize = len(dict(zip_entries())[CHAPTER])
    add_damaged_zip(fx, "uncompressed_size_understated",
                    patch(normal, (ch["central"] + CENTRAL_UNCOMPRESSED, "<I", usize - 1)),
                    "zip/normal.zip with the chapter's uncompressed size in the central directory one "
                    "byte short: the contents decompress to more than it says, which a reader must not "
                    "deliver (it is how a zip bomb is stopped).", "corrupt", ["entry"], CHAPTER)
    add_damaged_zip(fx, "uncompressed_size_overstated",
                    patch(normal, (ch["central"] + CENTRAL_UNCOMPRESSED, "<I", usize + 1)),
                    "zip/normal.zip with the chapter's uncompressed size in the central directory one "
                    "byte long: the DEFLATE stream ends first.", "corrupt", ["entry"], CHAPTER)
    add_damaged_zip(fx, "compressed_size_overruns",
                    patch(normal, (ch["central"] + CENTRAL_COMPRESSED, "<I",
                                   n["directory"] - ch["data"] + 100)),
                    "zip/normal.zip with the chapter's compressed size in the central directory reaching "
                    "100 bytes into the central directory.", "corrupt", ["directory", "entry"], CHAPTER)
    flags = struct.unpack_from("<H", normal, ch["central"] + CENTRAL_FLAGS)[0]
    add_damaged_zip(fx, "encrypted_entry",
                    patch(normal, (ch["central"] + CENTRAL_FLAGS, "<H", flags | 1),
                          (ch["local"] + LOCAL_FLAGS, "<H", flags | 1)),
                    "zip/normal.zip with the chapter marked encrypted (general-purpose flag bit 0) in both "
                    "headers.", "unsupported", ["directory", "entry"], CHAPTER)
    add_damaged_zip(fx, "unsupported_method",
                    patch(normal, (ch["central"] + CENTRAL_METHOD, "<H", 12),
                          (ch["local"] + LOCAL_METHOD, "<H", 12)),
                    "zip/normal.zip with the chapter's method set to 12 (bzip2) in both headers.",
                    "unsupported", ["directory", "entry"], CHAPTER)
    add_damaged_zip(fx, "zip64_sizes",
                    patch(normal, (ch["central"] + CENTRAL_COMPRESSED, "<I", 0xFFFFFFFF),
                          (ch["central"] + CENTRAL_UNCOMPRESSED, "<I", 0xFFFFFFFF)),
                    "zip/normal.zip with the chapter's sizes in the central directory set to 0xFFFFFFFF, "
                    "which means \"see the ZIP64 extra field\" (there is none).", "unsupported",
                    ["directory", "entry"], CHAPTER)


# --------------------------------------------------------------------------- main


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=DEFAULT_OUT, help="output directory (default: %(default)s)")
    args = parser.parse_args()

    fx = Fixtures()
    deflate_cases(fx)
    truncated_cases(fx)
    bitflip_cases(fx)
    invalid_cases(fx)
    zip_cases(fx)
    fx.write(args.out)

    kinds = {}
    for case in fx.cases:
        kinds[case["kind"]] = kinds.get(case["kind"], 0) + 1
    print("%d cases in %s: %s" % (len(fx.cases), args.out,
                                  ", ".join("%d %s" % (v, k) for k, v in kinds.items())))


if __name__ == "__main__":
    main()
