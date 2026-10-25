#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Run the whole-book flow over a directory of EPUBs and summarize it.

    python3 tests/corpus/run_corpus.py --runner build/tests/bartap_corpus [--jobs N] [--timeout S]
        DIR [-- runner options]

Every *.epub under DIR (recursively) goes through bartap_corpus (corpus.c), one process per book, so a
crash or a hang is one book's result rather than the end of the run. Per book this prints the result of
each stage, the warning bits and the time per MiB decompressed; then a histogram of the result codes for
each stage, the warning bits seen, and every book where an invariant broke, crashed or timed out. Those
last are bugs; the result codes are what the books are like.

The books are never committed: point this at a directory of your own. Options after "--" go to the runner
(--strict, --elements, --feed N, --resolve-every N, --resolve-max N, --no-snapshot).

Exit status: 0 if no invariant broke and nothing crashed or timed out.
"""

import argparse
import collections
import concurrent.futures
import os
import signal
import subprocess
import sys

STAGES = ("open", "metadata", "toc", "streams", "text", "cfi")
WARNINGS = {
    "warnings": ["MIMETYPE_MISSING", "MIMETYPE_NOT_FIRST", "MIMETYPE_COMPRESSED", "MIMETYPE_EXTRA_FIELD",
                 "NAME_NOT_UTF8", None, None, None, "SPINE_ITEM_MISSING", "NO_TOC", "METADATA_TRUNCATED",
                 "MARKUP_RECOVERED"],
    "text_warnings": ["ENTITY", "MARKUP", "END_TAG", "ENCODING"],
    "toc_warnings": ["ENTITY", "MARKUP", "END_TAG", "ENCODING"],
}


def run_one(runner, options, path, timeout):
    try:
        out = subprocess.run([runner] + options + [path], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return path, {"crash": "timed out after %ss" % timeout}
    fields = {}
    for line in out.stdout.splitlines():
        if line.startswith("BOOK\t"):
            for field in line.split("\t")[2:]:
                key, _, value = field.partition("=")
                fields[key] = value
    if out.returncode < 0:
        fields["crash"] = "killed by %s" % signal.Signals(-out.returncode).name
    elif out.returncode not in (0, 1) or "open" not in fields:
        tail = out.stderr.strip().splitlines()[-3:]
        fields["crash"] = "exit %d: %s" % (out.returncode, " | ".join(tail))
    return path, fields


def bit_names(kind, value):
    v = int(value, 16)
    names = WARNINGS[kind]
    return [names[i] if i < len(names) and names[i] else "bit%d" % i for i in range(32) if v >> i & 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runner", required=True, help="the bartap_corpus executable")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("dir")
    parser.add_argument("options", nargs="*", help="runner options, after --")
    args = parser.parse_args()
    books = sorted(os.path.join(d, f) for d, _, files in os.walk(args.dir) for f in files
                   if f.lower().endswith(".epub"))
    if not books:
        sys.exit("no .epub files under %s" % args.dir)

    histogram = {stage: collections.Counter() for stage in STAGES}
    warnings = {kind: collections.Counter() for kind in WARNINGS}
    bugs = []
    rates = []
    print("%-48s %-10s %-10s %-10s %-10s %-10s %-10s %9s  %s" % (("book",) + STAGES + ("ms/MiB", "warnings")))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for path, f in pool.map(lambda p: run_one(args.runner, args.options, p, args.timeout), books):
            name = os.path.relpath(path, args.dir)
            if "crash" in f:
                bugs.append((name, f["crash"]))
                print("%-48s CRASH %s" % (name[-48:], f["crash"]))
                continue
            for stage in STAGES:
                histogram[stage][f.get(stage, "?")] += 1
            seen = []
            for kind in WARNINGS:
                for bit in bit_names(kind, f.get(kind, "0x0")):
                    warnings[kind][bit] += 1
                    seen.append(("" if kind == "warnings" else kind.split("_")[0] + ":") + bit)
            if float(f.get("mib", 0)) > 0.05:
                rates.append(float(f["ms_per_mib"]))
            print("%-48s %-10s %-10s %-10s %-10s %-10s %-10s %9s  %s" % ((name[-48:],)
                  + tuple(f.get(s, "?") for s in STAGES) + (f.get("ms_per_mib", "?"), " ".join(seen))))
            if f.get("failure"):
                bugs.append((name, f["failure"]))

    print("\n%d books" % len(books))
    for stage in STAGES:
        print("  %-9s %s" % (stage, ", ".join("%s %d" % kv for kv in histogram[stage].most_common())))
    for kind in WARNINGS:
        if warnings[kind]:
            print("  %-13s %s" % (kind, ", ".join("%s %d" % kv for kv in warnings[kind].most_common())))
    if rates:
        rates.sort()
        print("  ms per MiB decompressed: median %.1f, slowest %.1f" % (rates[len(rates) // 2], rates[-1]))
    if bugs:
        print("\n%d books broke an invariant, crashed or timed out:" % len(bugs))
        for name, why in bugs:
            print("  %s: %s" % (name, why))
    return 1 if bugs else 0


if __name__ == "__main__":
    sys.exit(main())
