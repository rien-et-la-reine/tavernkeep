#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Generate small EPUB books for Bartap's book-level tests, the CFI oracle and the fuzz seeds.

    python3 tests/make_books.py --out DIR

Each book exercises a few things the specifications or the reader-facing API define, and books.json
lists them with what opening each must give. Every word in them is written here, so nothing copyrighted
goes in. Deterministic: fixed zip timestamps, no randomness. Uses only the standard library.

    epub3.epub          EPUB 3: nav with fragments into one file holding two chapters, cover-image
                        property, a stylesheet, inline styling, lists, entities, CDATA, comments,
                        astral characters, a noteref and its aside, an image, an <a name> anchor
    epub3-crlf.epub     the same chapter text with CR LF line ends, and attribute values holding them
    epub2.epub          EPUB 2: an NCX, <meta name="cover">, XHTML 1.1 DOCTYPE and its named entities
    plain.epub          no cover, no table of contents
    dangling.epub       a cover meta naming no item, and a spine item whose entry is missing
    duplicate.epub      two entries named epub/style.css (the first must win, D21)
    lenient.epub        malformed markup in a chapter and in the OPF: opens, with warnings
    long-attributes.epub  a 2 KiB Calibre-style meta content and a 4 KiB SVG path: both must open
"""

import argparse
import json
import os
import shutil
import sys
import zipfile

STAMP = (2020, 1, 1, 0, 0, 0)
CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="{path}" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""
# a 1x1 PNG
PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360000002000154a24f5d0000000049454e44ae426082")

XHTML3_HEAD = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="en">
<head><title>{title}</title><link rel="stylesheet" type="text/css" href="../style.css"/></head>
"""

CHAPTERS = XHTML3_HEAD.format(title="Chapters") + """<body>
<section id="one" epub:type="chapter">
<h1>One</h1>
<p>The <em>quick</em> brown fox&#x2019;s den &amp; its <strong>bold <i>nested</i></strong> run.</p>
<p>Entities&nbsp;here: caf&eacute; &mdash; na&iuml;ve &lt;tag&gt; &quot;quoted&quot; &#169; 2&#8212;3.</p>
<p>Astral: &#x1F600; and \U0001F642 then a <code>code span</code>, x<sup>2</sup> and H<sub>2</sub>O.</p>
<!-- a comment between paragraphs -->
<p>Before a comment<!-- inline -->after it, <![CDATA[a CDATA <section> & more]]> and a line<br/>break.</p>
<blockquote><p>Quoted and indented.</p></blockquote>
<ul><li>First item</li><li>Second <u>underlined</u> <s>struck</s> item</li></ul>
<hr/>
<p>A note<a href="#n1" epub:type="noteref">1</a> and a picture <img src="../images/cover.png" alt="cover"/>.</p>
<aside epub:type="footnote" id="n1"><p>The note itself.</p></aside>
</section>
<section id="two" epub:type="chapter">
<h1>Two</h1>
<p><a name="legacy"></a>A legacy anchor starts this paragraph, which goes on for a while so that it holds
several words: one two three four five six seven eight nine ten.</p>
<p>The end of chapter two.</p>
</section>
</body>
</html>
"""

NAV = XHTML3_HEAD.format(title="Contents") + """<body>
<nav epub:type="toc" id="toc"><h1>Contents</h1>
<ol>
  <li><a href="text/chapters.xhtml#one">Chapter   One</a>
    <ol><li><a href="text/chapters.xhtml#n1">The %20 note</a></li></ol></li>
  <li><a href="text/chapters.xhtml#two">Chapter Two</a></li>
  <li><a href="text/chapters.xhtml#legacy">Legacy anchor</a></li>
  <li><a href="text/end.xhtml">The End</a></li>
  <li><a href="text/caf%C3%A9.xhtml#x%41y">Encoded</a></li>
</ol>
</nav>
<nav epub:type="landmarks"><ol><li><a href="text/end.xhtml">Not the toc</a></li></ol></nav>
</body>
</html>
"""

END = XHTML3_HEAD.format(title="End") + """<body><p>Finis.</p></body></html>
"""

CAFE = XHTML3_HEAD.format(title="Cafe") + """<body><p id="xAy">A file whose name needed percent-encoding.</p></body></html>
"""


def opf3(items, spine, extra_meta="", title="A Test Book", toc_attr=""):
    manifest = "\n".join(
        '    <item id="{}" href="{}" media-type="{}"{}/>'.format(i, h, m, (' properties="%s"' % p) if p else "")
        for i, h, m, p in items)
    itemrefs = "\n".join('    <itemref idref="%s"/>' % s for s in spine)
    return """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:00000000-0000-4000-8000-000000000000</dc:identifier>
    <dc:title>  {title}
    </dc:title>
    <dc:creator>Test Author</dc:creator>
    <dc:language>en</dc:language>
{extra_meta}  </metadata>
  <manifest>
{manifest}
  </manifest>
  <spine{toc_attr}>
{itemrefs}
  </spine>
</package>
""".format(title=title, extra_meta=extra_meta, manifest=manifest, toc_attr=toc_attr, itemrefs=itemrefs)


def write_epub(path, entries, mimetype=True):
    with zipfile.ZipFile(path, "w") as z:
        if mimetype:
            info = zipfile.ZipInfo("mimetype", STAMP)
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, "application/epub+zip")
        for name, data in entries:
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)


def epub3_entries(chapters=CHAPTERS):
    items = [
        ("nav", "toc.xhtml", "application/xhtml+xml", "nav"),
        ("chapters", "text/chapters.xhtml", "application/xhtml+xml", ""),
        ("end", "text/end.xhtml", "application/xhtml+xml", ""),
        ("cafe", "text/caf%C3%A9.xhtml", "application/xhtml+xml", ""),
        ("css", "style.css", "text/css", ""),
        ("cover", "images/cover.png", "image/png", "cover-image"),
    ]
    return [
        ("META-INF/container.xml", CONTAINER.format(path="epub/content.opf")),
        ("epub/content.opf", opf3(items, ["chapters", "end", "cafe"])),
        ("epub/toc.xhtml", NAV),
        ("epub/text/chapters.xhtml", chapters),
        ("epub/text/end.xhtml", END),
        ("epub/text/café.xhtml", CAFE),
        ("epub/style.css", "p { text-indent: 1em; }\n"),
        ("epub/images/cover.png", PNG),
    ]


def books():
    out = {}
    out["epub3.epub"] = (epub3_entries(), {
        "title": "A Test Book", "creator": "Test Author", "language": "en", "spine": 3,
        "cover": "epub/images/cover.png", "cover_type": 2, "toc": 6, "warnings": 0})

    crlf = CHAPTERS.replace("\n", "\r\n").replace('id="n1"', 'id="n1" title="a&#13;&#10;b\r\nc"')
    out["epub3-crlf.epub"] = (epub3_entries(crlf), {"spine": 3, "warnings": 0})

    ncx = """<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="x"/></head>
  <docTitle><text>Old</text></docTitle>
  <navMap>
    <navPoint id="p1" playOrder="1"><navLabel><text>First &amp; Foremost</text></navLabel>
      <content src="one.html#start"/>
      <navPoint id="p2" playOrder="2"><navLabel><text>Nested</text></navLabel><content src="two.html"/></navPoint>
    </navPoint>
  </navMap>
</ncx>
"""
    xhtml11 = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>{t}</title></head>
<body><h2 id="start">{t}</h2><p>Old&nbsp;style &ldquo;quotes&rdquo; &hellip; &copy; &AElig;sop &thetasym;.</p></body></html>
"""
    opf2 = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>Second Edition</dc:title>
    <dc:identifier id="uid">isbn-0</dc:identifier>
    <meta name="cover" content="cover-img"/>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="one" href="one.html" media-type="application/xhtml+xml"/>
    <item id="two" href="two.html" media-type="application/xhtml+xml"/>
    <item id="cover-img" href="images/front.jpg" media-type="image/jpeg"/>
  </manifest>
  <spine toc="ncx"><itemref idref="one"/><itemref idref="two"/></spine>
</package>
"""
    out["epub2.epub"] = ([
        ("META-INF/container.xml", CONTAINER.format(path="OEBPS/content.opf")),
        ("OEBPS/content.opf", opf2),
        ("OEBPS/toc.ncx", ncx),
        ("OEBPS/one.html", xhtml11.format(t="One")),
        ("OEBPS/two.html", xhtml11.format(t="Two")),
        ("OEBPS/images/front.jpg", b"\xff\xd8\xff\xd9"),
    ], {"title": "Second Edition", "spine": 2, "cover": "OEBPS/images/front.jpg", "cover_type": 1, "toc": 2,
        "warnings": 0})

    plain_items = [("c", "c.xhtml", "application/xhtml+xml", "")]
    out["plain.epub"] = ([
        ("META-INF/container.xml", CONTAINER.format(path="content.opf")),
        ("content.opf", opf3(plain_items, ["c"], title="Plain")),
        ("c.xhtml", END),
    ], {"title": "Plain", "spine": 1, "toc": 0, "warnings": 1 << 9})

    dangling_items = [("c", "c.xhtml", "application/xhtml+xml", ""),
                      ("gone", "gone.xhtml", "application/xhtml+xml", "")]
    out["dangling.epub"] = ([
        ("META-INF/container.xml", CONTAINER.format(path="content.opf")),
        ("content.opf", opf3(dangling_items, ["c", "gone", "nothing"], extra_meta='    <meta name="cover" content="none"/>\n')),
        ("c.xhtml", END),
    ], {"spine": 3, "toc": 0, "warnings": (1 << 8) | (1 << 9)})

    dup = epub3_entries()
    dup.append(("epub/style.css", "p { color: red; }\n"))
    out["duplicate.epub"] = (dup, {"spine": 3, "warnings": 0, "first_css": "p { text-indent: 1em; }\n"})

    bad_chapter = XHTML3_HEAD.format(title="Bad") + """<body>
<p class=unquoted>An unquoted attribute, a bare & ampersand, an &unknown; entity, and a <b>misnested <i>run</b> here</i>.</p>
<p>A stray </span> end tag, a 1 < 2 comparison, <td nowrap>an empty attribute</td> and &#0; a bad reference.</p>
<div><p>Unclosed paragraph<p>and another</div>
<p>The end.</p>
</body>
</html>
trailing junk <after/> the root
"""
    lenient_items = [("bad", "bad.xhtml", "application/xhtml+xml", "")]
    lenient_opf = opf3(lenient_items, ["bad"], title="Lenient &amp; Sloppy",
                       extra_meta="    <dc:description>Bad &nbsp; entity <b>and markup</dc:description>\n")
    out["lenient.epub"] = ([
        ("META-INF/container.xml", CONTAINER.format(path="content.opf")),
        ("content.opf", lenient_opf),
        ("bad.xhtml", bad_chapter),
    ], {"title": "Lenient & Sloppy", "spine": 1, "toc": 0, "warnings": (1 << 9) | (1 << 11)})

    long_meta = '    <meta name="calibre:user_metadata" content="{}"/>\n'.format("{&quot;x&quot;: " + "1" * 2048 + "}")
    svg_path = "M0 0 " + "L1 1 " * 820
    svg_chapter = XHTML3_HEAD.format(title="SVG") + """<body><p>Before the drawing.</p>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10"><path d="{}"/></svg>
<p>After the drawing.</p></body></html>
""".format(svg_path)
    out["long-attributes.epub"] = ([
        ("META-INF/container.xml", CONTAINER.format(path="content.opf")),
        ("content.opf", opf3([("s", "s.xhtml", "application/xhtml+xml", "")], ["s"], extra_meta=long_meta)),
        ("s.xhtml", svg_chapter),
    ], {"spine": 1, "toc": 0, "warnings": 1 << 9})
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    out = args.out
    marker = os.path.join(out, "books.json")
    if os.path.isdir(out) and os.listdir(out) and not os.path.exists(marker):
        sys.exit("refusing to write into %s: it holds files this script did not make" % out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    manifest = {}
    for name, (entries, expect) in sorted(books().items()):
        write_epub(os.path.join(out, name), entries)
        manifest[name] = expect
    with open(marker, "w") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")


if __name__ == "__main__":
    main()
