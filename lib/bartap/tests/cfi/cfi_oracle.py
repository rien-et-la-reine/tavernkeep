#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""CFI oracle: checks the positions Bartap's text layer gives against a DOM built by lxml.

    python3 tests/cfi/cfi_oracle.py --dump PATH/TO/bartap_cfi_dump BOOK.epub...

For every spine item of every book, bartap_cfi_dump prints each item's CFI and text. This script parses
the same content document with lxml and locates each CFI in the DOM as EPUB CFI 1.1 defines it (section
3.1): after the "!", steps start at the root element's children; an even step 2k is the k-th element
child; an odd step 2k+1 is the run of character data between element children k and k+1 (comments and
processing instructions are not children for counting, and the text on either side of one is one run);
the offset counts UTF-16 code units. A WORD item must find the DOM's text at its offset beginning with the
word; a RULE or element item must find an element (an hr, or one of that name).

The DOM is lxml's, with two preparations a reader's DOM would also have: the XHTML 1.x named entities are
decoded before parsing (lxml loads no DTD), and line ends are normalized, as every XML parser does.

Known divergences are listed in EXPECTED, by book, and reported as expected failures, not hidden: a book
listed there that passes is an unexpected pass, and fails the run.

Exit status: 0 if everything passed or failed as expected, 1 otherwise, 77 (skip) if lxml is missing.
"""

import argparse
import html.entities
import os
import re
import subprocess
import sys
import zipfile

try:
    from lxml import etree
except ImportError:
    print("SKIP: lxml is not installed (python3-lxml); the CFI oracle needs it")
    sys.exit(77)

# book file name: why the DOM cannot agree
EXPECTED = {
    "lenient.epub": "malformed markup, which Bartap recovers from and an XML DOM refuses: its CFIs are "
                    "Bartap's own (text.h)",
}

XML_ENTITIES = {"amp", "lt", "gt", "quot", "apos"}
ENTITY = re.compile(rb"&([A-Za-z][A-Za-z0-9]*);")
CDATA = re.compile(rb"(<!\[CDATA\[.*?\]\]>)", re.S)


def decode_entities(data):
    """The XHTML 1.x named entities, decoded outside CDATA sections."""
    def one(match):
        name = match.group(1).decode("ascii")
        if name in XML_ENTITIES or name not in html.entities.name2codepoint:
            return match.group(0)
        return chr(html.entities.name2codepoint[name]).encode("utf-8")
    parts = CDATA.split(data)
    return b"".join(p if p.startswith(b"<![CDATA[") else ENTITY.sub(one, p) for p in parts)


def element_children(node):
    return [c for c in node if isinstance(c.tag, str)]


def run_text(node, k):
    """The character data between element children k and k+1 of node (k = 0: before the first)."""
    children = list(node)
    parts = []
    count = 0
    if k == 0:
        parts.append(node.text or "")
    for c in children:
        if isinstance(c.tag, str):
            count += 1
            if count > k:
                break
            if count == k:
                parts.append(c.tail or "")
        elif count == k:
            parts.append(c.tail or "")     # a comment or PI inside the run: its tail continues it
    return "".join(parts)


def local(tag):
    return tag.rsplit("}", 1)[-1]


def locate(root, steps):
    """Follow steps from the root element; return (element, None) or (parent, run index), or raise."""
    node = root
    for i, s in enumerate(steps):
        if s % 2 == 1:
            if i != len(steps) - 1:
                raise ValueError("a text step before the last")
            return node, (s - 1) // 2
        kids = element_children(node)
        if s // 2 > len(kids):
            raise ValueError("step /%d: only %d element children" % (s, len(kids)))
        node = kids[s // 2 - 1]
    return node, None


def parse_cfi(text):
    m = re.fullmatch(r"epubcfi\(/6/(\d+)!((?:/\d+)*)(?::(\d+))?\)", text)
    if m is None:
        raise ValueError("not a CFI this oracle reads: " + text)
    steps = [int(s) for s in m.group(2).split("/")[1:]]
    return int(m.group(1)) // 2 - 1, steps, int(m.group(3)) if m.group(3) is not None else None


def check_item(root, kind, cfi, text):
    _, steps, offset = parse_cfi(cfi)
    node, run = locate(root, steps)
    if kind == 0:                                           # WORD
        if run is None:
            return "a word's CFI names an element"
        units = run_text(node, run).encode("utf-16-le")
        at = units[2 * (offset or 0):].decode("utf-16-le", errors="replace")
        if not at.startswith(text):
            return "DOM text at the offset is %r, not %r" % (at[:len(text) + 8], text)
        return None
    if run is not None:
        return "an element item's CFI names text"
    if kind == 1 and local(node.tag) != "hr":               # RULE
        return "a rule at <%s>" % local(node.tag)
    if kind in (2, 4) and local(node.tag) != text.split(":")[-1]:
        return "<%s> where the item names <%s>" % (local(node.tag), text)
    return None


def check_book(dump, path, elements):
    args = [dump] + (["--elements"] if elements else []) + [path]
    out = subprocess.run(args, capture_output=True, text=True)
    if out.returncode not in (0, 1):
        return ["the dumper failed (%d): %s" % (out.returncode, out.stderr.strip()[:200])], 0
    problems = []
    spine = {}
    roots = {}
    checked = 0
    with zipfile.ZipFile(path) as z:
        parser = etree.XMLParser(resolve_entities=False, strip_cdata=True, recover=False, huge_tree=True,
                                 no_network=True, load_dtd=False)
        for line in out.stdout.splitlines():
            fields = line.split("\t")
            if fields[0] == "SPINE":
                spine[int(fields[1])] = fields[2]
            elif fields[0] == "RESULT":
                if fields[3] != "failure=":
                    problems.append("dumper: " + fields[3])
            elif fields[0] == "ITEM":
                index, kind, cfi = int(fields[1]), int(fields[2]), fields[3]
                if not cfi:
                    continue                                # deeper than a CFI can say
                text = bytes.fromhex(fields[4]).decode("utf-8", errors="replace")
                if index not in roots:
                    try:
                        roots[index] = etree.fromstring(decode_entities(z.read(spine[index])), parser)
                    except (etree.XMLSyntaxError, KeyError) as e:
                        roots[index] = e
                root = roots[index]
                if isinstance(root, Exception):
                    problems.append("spine item %d: the DOM could not be built: %s" % (index, root))
                    roots[index] = None
                    continue
                if root is None:
                    continue
                checked += 1
                try:
                    problem = check_item(root, kind, cfi, text)
                except ValueError as e:
                    problem = str(e)
                if problem is not None:
                    problems.append("%s %r: %s" % (cfi, text, problem))
    return problems, checked


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump", required=True, help="the bartap_cfi_dump executable")
    parser.add_argument("books", nargs="+")
    args = parser.parse_args()
    unexpected = 0
    for path in args.books:
        name = os.path.basename(path)
        for elements in (False, True):
            label = "%s%s" % (name, " (with element items)" if elements else "")
            problems, checked = check_book(args.dump, path, elements)
            if name in EXPECTED:
                if problems:
                    print("XFAIL %s: %d disagreements, as expected: %s" % (label, len(problems), EXPECTED[name]))
                else:
                    print("XPASS %s: expected to fail (%s), but %d items all agree" % (label, EXPECTED[name], checked))
                    unexpected += 1
            elif problems:
                print("FAIL %s: %d of %d items" % (label, len(problems), checked))
                for p in problems[:20]:
                    print("    " + p)
                unexpected += 1
            else:
                print("ok   %s: %d items" % (label, checked))
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
