#!/usr/bin/env python3
"""Run the Tavernkeep hardware tests on a Pico 2 through the Raspberry Pi Debug Probe.

    python3 tests/hardware/run.py                    # every core and removal case
    python3 tests/hardware/run.py normal stack       # chosen cases, or groups: core, removal, rates
    python3 tests/hardware/run.py rates --rates 5000000 12500000
    python3 tests/hardware/run.py --list
    python3 tests/hardware/run.py --offline          # everything that needs no board (see below)

Needs the Pico 2 wired to the SD breakout as in VAL-H03, a card with a FAT volume in the socket, and the
Debug Probe on SWD. The demo writes to the card as usual (its counter's slot files and the bulk file);
nothing here formats it.

Each run writes tests/hardware/results/<time>/: one log per case (every RTT line with its time since
reset, and the rig's own notes), openocd.log, and summary.txt / summary.json, which name the commit.
Exit status is 0 only if every case passed.

--offline builds the default firmware and checks, without a board: that every function the cases break
on exists out of line under its own name, the removal latch's address, that init's CMD8 and ACMD41 reach
the out-of-line sd_spi_command with the command in r1, that the log lines the cases match still exist in
the sources, that the log parser reads a synthetic demo pass, and that OpenOCD and its configs exist.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import hil  # noqa: E402
from cases import CASES  # noqa: E402


def git_commit():
    try:
        commit = subprocess.run(["git", "-C", hil.ROOT, "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", hil.ROOT, "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True).stdout.strip()
        return commit + (" with uncommitted changes" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def select_cases(names):
    if not names:
        return [c for c in CASES if c[1] in ("core", "removal", "faults")]
    chosen = []
    for name in names:
        matches = [c for c in CASES if c[0] == name or c[1] == name]
        if not matches:
            sys.exit(f"no case or group named {name!r}; --list shows them")
        chosen += [c for c in matches if c not in chosen]
    return chosen


# ----------------------------------------------------------------------------------------- offline

class FakeRtt:
    """Stands in for the RTT stream: hands the whole log over in one read, as OpenOCD can when lines
    arrive within one polling interval, so the real Board.wait_for has to keep what a wait does not use."""

    def __init__(self, lines):
        self.feed = list(lines)

    def lines(self, wait):
        if not self.feed:
            raise hil.CheckFailed("ran out of lines")
        batch, self.feed = self.feed, []
        return batch


def fake_board(lines):
    board = hil.Board(ocd=None, case_log=lambda text: None)
    board.rtt = FakeRtt(lines)
    return board


# one boot of the demo, written from the format strings in src/main.c and src/storage/storage.c
SYNTHETIC_PASS = """\
[INFO] Tavernkeep RP2350 firmware
[INFO] PASS storage_init; insert or remove the card at any time
[INFO] storage: owner local, medium absent, epoch 0
[INFO] storage: owner local, medium preparing, epoch 0
[INFO] storage: owner local, medium ready, epoch 0
[INFO] sd bus at 1000000 Hz (asked for 1000000 Hz)
[INFO] PASS label
[INFO] volume label "SCROLL"
[INFO] PASS opendir
[INFO]   System Volume Information/ 0
[INFO]   tavernkeep-demo.a 18
[INFO] PASS readdir
[INFO] 2 entries in /
[INFO] PASS closedir
[INFO] PASS save point write
[INFO] PASS counter readback
[INFO] this card has now been mounted by the demo 12 times
[INFO] PASS bulk write
[INFO] bulk write: 16384 bytes in 210000 us, 76 KiB/s
[INFO] PASS bulk read
[INFO] bulk read: 16384 bytes in 190000 us, 84 KiB/s
[INFO] PASS bulk verify, 16384 bytes
"""


def offline(opts):
    problems = []

    def ok(text):
        print(f"  ok    {text}")

    def bad(text):
        print(f"  FAIL  {text}")
        problems.append(text)

    print("building the default firmware")
    elf = hil.build(log=lambda t: print(f"  {t}"))
    ok(os.path.relpath(elf, hil.ROOT))

    print("symbols")
    try:
        symbols = hil.Symbols(elf)
        for name in hil.Symbols.FUNCTIONS:
            ok(f"{name} at 0x{symbols[name]:08x}")
        ok(f"removal latch (sd.removal_latched) at 0x{symbols.removal_latch:08x}")
        ok(f"stack 0x{symbols['__StackBottom']:08x}-0x{symbols['__StackTop']:08x}, "
           f"fill from 0x{symbols['__StackLimit']:08x}")
    except hil.HilError as error:
        bad(str(error))
        symbols = None

    print("init's commands reach the out-of-line sd_spi_command")
    if symbols is not None:
        objdump = os.path.join(hil.TOOLCHAIN_BIN, "arm-none-eabi-objdump")
        start = symbols["sd_spi_device_init"]
        listing = subprocess.run([objdump, "-d", f"--start-address=0x{start:x}",
                                  f"--stop-address=0x{start + 0x800:x}", elf],
                                 capture_output=True, text=True).stdout.splitlines()
        loaded = None
        seen = set()
        for line in listing:
            if (m := re.search(r"\bmovs?\s+r1, #(\d+)", line)):
                loaded = int(m.group(1))
            elif "<sd_spi_command>" in line and re.search(r"\bbl\b", line):
                if loaded is not None:
                    seen.add(loaded)
                loaded = None
        for command in (8, 41):
            (ok if command in seen else bad)(f"CMD{command}: r1 = {command} before a call to sd_spi_command"
                                             if command in seen else
                                             f"no call to sd_spi_command with r1 = {command} found in init")

    print("log lines the cases match")
    for path, literals in hil.SOURCE_TEXT.items():
        source = open(os.path.join(hil.ROOT, path)).read()
        for literal in literals:
            (ok if literal in source else bad)(f"{path}: {literal}" if literal in source else
                                               f"{path} no longer contains {literal}")

    print("log parser on a synthetic demo pass")
    try:
        run = hil.wait_demo(fake_board(SYNTHETIC_PASS.splitlines()))
        expected = dict(epoch=0, bus_hz=1000000, asked_hz=1000000, counter=12,
                        bulk_kibs={"write": 76, "read": 84})
        got = dict(epoch=run.epoch, bus_hz=run.bus_hz, asked_hz=run.asked_hz, counter=run.counter,
                   bulk_kibs=run.bulk_kibs)
        (ok if got == expected else bad)("fields parsed" if got == expected else f"parsed {got}, expected {expected}")
        (ok if run.complete and not run.missing_steps() else bad)(
            "every demo step seen" if run.complete else f"missing steps {run.missing_steps()}")
        failing = SYNTHETIC_PASS.replace("[INFO] PASS save point write", "[ERROR] FAIL save point write: result 11")
        run = hil.wait_demo(fake_board(failing.splitlines()))
        (ok if run.failed == [("save point write", 11)] else bad)(
            "a FAIL line ends the pass with its result" if run.failed == [("save point write", 11)]
            else f"FAIL parsed as {run.failed}")
    except hil.CheckFailed as error:
        bad(f"parser: {error}")

    print("OpenOCD")
    for path in (hil.OPENOCD, os.path.join(hil.OPENOCD_DIR, "scripts", "interface", "cmsis-dap.cfg"),
                 os.path.join(hil.OPENOCD_DIR, "scripts", "target", "rp2350.cfg")):
        (ok if os.path.exists(path) else bad)(path if os.path.exists(path) else f"missing {path}")
    try:
        probe = "2e8a:000c" in subprocess.run(["lsusb"], capture_output=True, text=True).stdout
        print(f"  info  Debug Probe {'connected' if probe else 'not connected'}")
    except OSError:
        pass

    print()
    print("offline checks passed" if not problems else f"{len(problems)} offline check(s) failed")
    return 0 if not problems else 1


# ------------------------------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("cases", nargs="*", help="case names or groups (core, removal, faults, rates, manual)")
    parser.add_argument("--list", action="store_true", help="list the cases and exit")
    parser.add_argument("--offline", action="store_true", help="run only the checks that need no board")
    parser.add_argument("--boots", type=int, default=10, help="reboots in the soak case (default 10)")
    parser.add_argument("--rates", type=int, nargs="+", default=[5000000, 12500000, 25000000],
                        help="SPI rates in Hz for the rates case, lowest first")
    parser.add_argument("--rate-boots", type=int, default=3, help="boots per rate (default 3)")
    parser.add_argument("--cycles", type=int, default=10,
                        help="removal and insertion cycles in manual_idle (default 10)")
    parser.add_argument("--rate", type=int, default=hil.DEFAULT_RATE_HZ,
                        help=f"SPI rate in Hz the other cases build and run at (default {hil.DEFAULT_RATE_HZ})")
    opts = parser.parse_args()

    if opts.list:
        for name, group, summary, _ in CASES:
            print(f"{name:22} [{group}]\n    {summary}\n")
        return 0
    if opts.offline:
        return offline(opts)

    chosen = select_cases(opts.cases)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = os.path.join(hil.HERE, "results", stamp)
    os.makedirs(out_dir)
    commit = git_commit()
    print(f"commit {commit}; results in {os.path.relpath(out_dir, hil.ROOT)}; SPI at {opts.rate} Hz")

    elf = hil.build(opts.rate, log=print)
    results = []
    ocd = None
    current_log = {"file": None}

    def case_log(line):
        if current_log["file"] is not None:
            current_log["file"].write(line + "\n")
            current_log["file"].flush()

    try:
        ocd = hil.OpenOcd(os.path.join(out_dir, "openocd.log"))
        board = hil.Board(ocd, case_log)
        current_log["file"] = open(os.path.join(out_dir, "flash.log"), "w")
        board.flash(elf)
        current_log["file"].close()
        for name, group, summary, function in chosen:
            current_log["file"] = open(os.path.join(out_dir, f"{name}.log"), "w")
            print(f"{name} ...", end=" ", flush=True)
            started = time.monotonic()
            status, detail, notes = "PASS", "", {}
            try:
                notes = function(board, opts) or {}
            except hil.CheckFailed as error:
                status, detail = "FAIL", str(error)
            except hil.HilError as error:
                status, detail = "ERROR", str(error)
            except Exception:  # noqa: BLE001 - a bug in the rig, reported rather than ending the run
                status, detail = "ERROR", traceback.format_exc()
            seconds = time.monotonic() - started
            case_log(f"# {status} {detail}")
            current_log["file"].close()
            current_log["file"] = None
            print(f"{status} ({seconds:.0f} s)")
            for key, value in notes.items():
                print(f"    {key}: {value}")
            if detail:
                print("    " + detail.replace("\n", "\n    "))
            results.append({"case": name, "group": group, "status": status, "seconds": round(seconds, 1),
                            "notes": notes, "detail": detail})
            if status == "ERROR" and "OpenOCD" in detail:
                print("the rig failed; stopping")
                break
    except hil.HilError as error:
        print(f"ERROR: {error}")
        results.append({"case": "rig", "status": "ERROR", "detail": str(error)})
    finally:
        if current_log["file"] is not None:
            current_log["file"].close()
        if ocd is not None:
            ocd.close()

    passed = sum(r["status"] == "PASS" for r in results)
    lines = [f"Tavernkeep hardware tests, {stamp}", f"commit {commit}, SPI at {opts.rate} Hz", ""]
    for r in results:
        lines.append(f"{r['status']:5} {r['case']}" + (f" ({r['seconds']} s)" if "seconds" in r else ""))
        for key, value in r.get("notes", {}).items():
            lines.append(f"      {key}: {value}")
        if r.get("detail"):
            lines.append("      " + r["detail"].replace("\n", "\n      "))
    lines += ["", f"{passed} of {len(results)} passed"]
    with open(os.path.join(out_dir, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump({"commit": commit, "time": stamp, "rate_hz": opts.rate, "results": results}, f, indent=2)
    print(f"\n{passed} of {len(results)} passed; summary in {os.path.relpath(out_dir, hil.ROOT)}/summary.txt")
    return 0 if results and passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
