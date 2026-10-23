#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT
"""Build and run Bartap's host tests under every compiler and word size this machine supports.

    python3 tests/run_matrix.py [--require-all] [--keep-going]

Tries gcc and clang, each as 64-bit (the host's default) and as 32-bit (-m32), with warnings as errors
and, where the toolchain has them, AddressSanitizer and UndefinedBehaviorSanitizer. Each configuration
gets its own build directory, build-matrix/COMPILER[-m32] under Bartap's root (ignored by
.gitignore), and runs the whole ctest suite there, the portability checks included.

A configuration this machine cannot build (no such compiler, no 32-bit libraries, no 32-bit sanitizer
runtime) is skipped and reported, not failed: run with --require-all to fail on a skip instead, for
a machine that is meant to have everything. Exit status is 0 only if every configuration that ran
passed.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
MATRIX_DIR = os.path.join(ROOT, "build-matrix")

COMPILERS = ("gcc", "clang")

PROBE = b"#include <stdio.h>\nint main(void) { printf(\"%zu\\n\", sizeof(void *)); return 0; }\n"


def probe(compiler, flags):
    """True if compiler can build and run a hosted program with flags."""
    with tempfile.TemporaryDirectory() as tmp:
        source = os.path.join(tmp, "probe.c")
        program = os.path.join(tmp, "probe")
        with open(source, "wb") as f:
            f.write(PROBE)
        built = subprocess.run([compiler, *flags, source, "-o", program],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if built.returncode != 0:
            return False
        return subprocess.run([program], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def configurations():
    """(name, compiler, m32, sanitize) to run, and (name, reason) skipped."""
    run = []
    skipped = []
    sanitizers = ["-fsanitize=address,undefined"]
    for compiler in COMPILERS:
        path = shutil.which(compiler)
        for m32 in (False, True):
            name = compiler + ("-m32" if m32 else "")
            if path is None:
                skipped.append((name, "%s is not installed" % compiler))
                continue
            word = ["-m32"] if m32 else []
            if not probe(path, word):
                skipped.append((name, "%s cannot build and run %s programs" % (compiler,
                                                                               "32-bit" if m32 else "host")))
                continue
            sanitize = probe(path, word + sanitizers)
            run.append((name, path, m32, sanitize))
    return run, skipped


def step(command, cwd=None):
    print("$ " + " ".join(command), flush=True)
    return subprocess.run(command, cwd=cwd).returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--require-all", action="store_true",
                        help="fail if any configuration had to be skipped")
    parser.add_argument("--keep-going", action="store_true",
                        help="run every configuration even after one fails")
    args = parser.parse_args()

    run, skipped = configurations()
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    results = []
    for name, compiler, m32, sanitize in run:
        build = os.path.join(MATRIX_DIR, name)
        print("\n=== %s%s" % (name, "" if sanitize else " (no sanitizers: not available here)"), flush=True)
        ok = (step(["cmake", "-S", ROOT, "-B", build, *generator,
                    "-DCMAKE_C_COMPILER=" + compiler,
                    "-DBARTAP_WARNINGS_AS_ERRORS=ON",
                    "-DBARTAP_TESTS_M32=" + ("ON" if m32 else "OFF"),
                    "-DBARTAP_TESTS_SANITIZE=" + ("ON" if sanitize else "OFF")])
              and step(["cmake", "--build", build])
              and step(["ctest", "--output-on-failure"], cwd=build))
        results.append((name, ok, sanitize))
        if not ok and not args.keep_going:
            break

    print("\n=== summary")
    for name, ok, sanitize in results:
        print("  %-10s %s%s" % (name, "passed" if ok else "FAILED", "" if sanitize else " (without sanitizers)"))
    for name, reason in skipped:
        print("  %-10s skipped: %s" % (name, reason))
    not_run = [name for name, *_ in run[len(results):]]
    for name in not_run:
        print("  %-10s not run (an earlier configuration failed)" % name)

    failed = not results or not all(ok for _, ok, _ in results) or bool(not_run)
    if args.require_all and skipped:
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
