#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Check the inflater and the OCF layer against make_fixtures.py's cases and their expectations.

    python3 tests/container/check_container.py --dump PATH/TO/bartap_container_dump --generated DIR

For each case in DIR/manifest.json, runs bartap_container_dump and checks what the manifest says reading it
must give (tests/README.md, Fixtures): a DEFLATE stream's verdict and output (a truncated one's output a
prefix of its source's); a valid archive's entries, in order, with their contents; a damaged one refused,
for its reason, at one of the stages where a reader may notice (Bartap's open walks the whole directory,
so a directory fault surfaces at open); and a warning case's warning bits.
"""

import argparse
import json
import os
import subprocess
import sys
import zlib

REASONS = {"not a zip": "not a zip", "corrupt": "corrupt", "unsupported": "unsupported", "not an epub": "not an epub"}
WARNINGS = {"mimetype missing": 1, "mimetype not first": 2, "mimetype compressed": 4, "mimetype extra field": 8,
            "name not utf-8": 16}


def run(dump, *args):
    out = subprocess.run([dump] + list(args), capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("exit %d: %s" % (out.returncode, out.stderr.strip()))
    return [line.split("\t") for line in out.stdout.splitlines()]


def read(generated, path):
    with open(os.path.join(generated, path), "rb") as f:
        return f.read()


def check_deflate(dump, generated, case):
    lines = run(dump, "deflate", os.path.join(generated, case["input"]))
    problems = ["%s" % " ".join(l) for l in lines if l[0] == "MISMATCH"]
    verdict, length, crc = lines[0][1], int(lines[0][2]), int(lines[0][3])
    if verdict != case["expect"]:
        problems.append("%s, expected %s" % (verdict, case["expect"]))
    if verdict == "decodes" and case.get("expected"):
        expected = read(generated, case["expected"])
        if (length, crc) != (len(expected), zlib.crc32(expected)):
            problems.append("output differs: %d bytes" % length)
    if case["kind"] == "deflate-truncated" and case.get("output_prefix_of"):
        expected = read(generated, case["output_prefix_of"])
        if length > len(expected) or zlib.crc32(expected[:length]) != crc:
            problems.append("output is not a prefix of %s" % case["output_prefix_of"])
    return problems


def zip_run(dump, generated, case):
    lines = run(dump, "zip", os.path.join(generated, case["input"]), str(case.get("scan_chunk_size") or 512))
    problems = ["%s" % " ".join(l) for l in lines if l[0] == "MISMATCH"]
    opened = lines[0][1]
    warnings = int(lines[0][2])
    entries = [(bytes.fromhex(l[1]), l[2], int(l[3]), int(l[4])) for l in lines if l[0] == "ENTRY"]
    listing = next((l[1] for l in lines if l[0] == "LIST"), None)
    return problems, opened, warnings, entries, listing


def check_zip(dump, generated, case):
    problems, opened, warnings, entries, listing = zip_run(dump, generated, case)
    if case["expect"] == "reject":
        reason = REASONS[case["reason"]]
        stages = case["fails_at"]
        if opened != "ok":
            if opened != reason or not ("open" in stages or "directory" in stages):
                problems.append("open: %s, expected %s at %s" % (opened, reason, "/".join(stages)))
        elif listing != "ok":
            if listing != reason or "directory" not in stages:
                problems.append("listing: %s, expected %s at %s" % (listing, reason, "/".join(stages)))
        else:
            name = (case.get("entry") or "").encode()
            got = [e for e in entries if e[0] == name]
            if "entry" not in stages or not got or got[0][1] != reason:
                problems.append("everything read; expected %s at %s" % (reason, "/".join(stages)))
        return problems
    if opened != "ok" or listing != "ok":
        return problems + ["open %s, listing %s" % (opened, listing)]
    expected_bits = sum(WARNINGS[w] for w in case.get("warnings", []))
    if warnings != expected_bits:
        problems.append("warnings %#x, expected %#x" % (warnings, expected_bits))
    expected_entries = case["entries"]
    if len(entries) != len(expected_entries):
        return problems + ["%d entries, expected %d" % (len(entries), len(expected_entries))]
    for (name, result, length, crc), e in zip(entries, expected_entries):
        expected_name = bytes.fromhex(e["name_hex"]) if e.get("name") is None else e["name"].encode()
        if name != expected_name:
            problems.append("entry %r, expected %r" % (name, expected_name))
        elif result != "ok" or length != e["size"] or crc != e["crc32"]:
            problems.append("%s: %s, %d bytes, crc %08x; expected %d bytes, crc %08x"
                            % (e.get("name"), result, length, crc, e["size"], e["crc32"]))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump", required=True)
    parser.add_argument("--generated", required=True)
    args = parser.parse_args()
    with open(os.path.join(args.generated, "manifest.json")) as f:
        cases = json.load(f)["cases"]
    failed = 0
    for case in cases:
        try:
            if case["kind"].startswith("deflate"):
                problems = check_deflate(args.dump, args.generated, case)
            else:
                problems = check_zip(args.dump, args.generated, case)
        except RuntimeError as e:
            problems = [str(e)]
        if problems:
            failed += 1
            print("FAIL %s/%s: %s" % (case["kind"], case["name"], "; ".join(problems)))
    print("%d of %d cases as the manifest expects" % (len(cases) - failed, len(cases)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
