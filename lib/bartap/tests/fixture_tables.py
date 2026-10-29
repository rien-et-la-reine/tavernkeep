#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Turn the fixture manifest into C case tables; no JSON parser is needed in the host runner."""
import json
import pathlib
import sys


def literal(value):
    if value is None:
        return "NULL"
    data = value if isinstance(value, bytes) else value.encode()
    return '"' + ''.join(chr(b) if 32 <= b < 127 and b not in (34, 92)
                         else '\\%03o' % b for b in data) + '"'


manifest = json.loads(pathlib.Path(sys.argv[1]).read_text())
lines = ["/* Generated from make_fixtures.py expectations. */", "static const inflate_case_t inflate_cases[] = {"]
for case in manifest["cases"]:
    if case["kind"].startswith("deflate"):
        verdict = {"decodes": "BARTAP_END", "truncated": "BARTAP_OK", "corrupt": "BARTAP_ERR_CORRUPT"}[case["expect"]]
        lines.append("    { %s, %s, %s }," % (literal(case["input"]), literal(case.get("expected")), verdict))
lines += ["};", "static const zip_case_t zip_cases[] = {"]
errors = {"corrupt": "BARTAP_ERR_CORRUPT", "unsupported": "BARTAP_ERR_UNSUPPORTED",
          "not a zip": "BARTAP_ERR_NOT_EPUB", "not an epub": "BARTAP_ERR_NOT_EPUB"}
for case in manifest["cases"]:
    if not case["kind"].startswith("zip"):
        continue
    if case["expect"] == "reject":
        lines.append("    { %s, %s, NULL, %s, %s }," % (
            literal(case["input"]), literal(case.get("entry")), errors[case["reason"]],
            "true" if case["reason"] == "not an epub" else "false"))
    else:
        for entry in case["entries"]:
            name = entry["name"] if entry["name"] is not None else bytes.fromhex(entry["name_hex"])
            lines.append("    { %s, %s, %s, BARTAP_END, false }," % (
                literal(case["input"]), literal(name), literal(entry["contents"])))
lines += ["};", ""]
lines += ["static const book_case_t book_cases[] = {"]
for name, case in json.loads(pathlib.Path(sys.argv[3]).read_text()).items():
    strings = [literal(name)] + [literal(case.get(k)) for k in ("title", "creator", "language", "identifier", "first_label")]
    numbers = [str(case.get(k, -1)) for k in ("spine", "toc", "warnings", "toc_warnings")]
    lines.append("    { " + ", ".join(strings + numbers) + " },")
lines += ["};", ""]
pathlib.Path(sys.argv[2]).write_text('\n'.join(lines))
