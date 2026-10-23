# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT

"""The hardware test cases: everything the validation records (VAL-H04 to VAL-H06) need that takes no
hands on the card.

Each case takes the Board and the run options, raises CheckFailed when the firmware misbehaves, and
returns a dict of measurements for the summary. Every case starts from a reset, so a failed case leaves
nothing behind for the next one.

Removals here are injected: the driver's removal latch is set at an exact point, as the card-detect
interrupt would set it. The card stays seated and powered, so these test the firmware's side of a
removal (the error reported, the coordinator's transition, the recovery, what a save point leaves) but
not what an unpowered card does with a write in progress. The physical removals in VAL-H04 remain.

A card is made to look unformatted by clearing the boot signature in the buffer the mount's read of
sector 0 lands in; the card itself is never touched. Read and write errors are injected the same way, by
making disk_read or disk_write return an error from its entry breakpoint without running.
"""

import re
import sys
import time

from hil import (BRING_UP_FAILED, INIT_FAILED, IO_ERROR, NOT_SEATED, RES_ERROR, STORAGE_ERROR, IN_USE,
                 COUNTER, LISTED, USB_CONFIGURED, USB_LENDING, USB_RETURNING, CheckFailed, HilError, bring_up_schedule,
                 build, check, message, states, wait_demo)
import usb_host

CASES = []


def case(name, summary, group="core"):
    def register(function):
        CASES.append((name, group, summary, function))
        return function
    return register


# ---------------------------------------------------------------------------------------- helpers

def expect_clean(run, what="the demo pass"):
    check(not run.failed, f"{what}: step failed: " + ", ".join(
        f"{step} (result {result})" if result is not None else step for step, result in run.failed))
    check(not run.missing_steps(), f"{what}: steps never reported: {', '.join(run.missing_steps())} "
                                   "(a log line dropped, or the demo changed)")
    check(not run.warnings, f"{what}: warnings: {'; '.join(run.warnings)}")


def expect_rate(run, rate_hz):
    check(run.asked_hz == rate_hz, f"the firmware asked for {run.asked_hz} Hz, expected {rate_hz} Hz "
                                   "(wrong build flashed?)")
    check(run.bus_hz == rate_hz, f"the bus runs at {run.bus_hz} Hz, not the {rate_hz} Hz asked for; "
                                 "clk_peri should divide to it exactly")


def expect_no_counted_failure(lines):
    failures = [message(l) for l in lines if BRING_UP_FAILED.match(message(l))]
    check(not failures, "counted bring-up failures: " + "; ".join(failures))


def expect_never(lines, medium):
    seen = [s for s in states(lines) if s[1] == medium]
    check(not seen, f"the medium went {medium.upper()}: {seen}")


def injection_point(board, entry, stage, condition, why):
    """Breakpoint handlers for an injected removal: once `entry` is reached (None: straight away), arm
    `stage` and inject at its first hit where condition(board) holds. Returns (arm, state)."""
    state = {"done": False, "line": None}

    def on_stage(b):
        if condition(b):
            b.disarm(stage)
            b.inject_removal(why)
            state["done"] = True
            state["line"] = len(b.lines)

    def on_entry(b):
        b.disarm(entry)
        b.arm(stage, on_stage)

    def arm(b):
        if entry is None:
            b.arm(stage, on_stage)
        else:
            b.arm(entry, on_entry)

    return arm, state


def wait_injected(board, state, timeout, what):
    board.wait_for(lambda line: False, timeout, what, poll=lambda: state["done"])


def demo_counter(board, opts, at_least):
    """Boot until the demo's counter is at least at_least; returns the last clean demo pass."""
    while True:
        board.boot()
        run = wait_demo(board)
        expect_clean(run, "the preparatory demo pass")
        if run.counter >= at_least:
            return run


# ------------------------------------------------------------------------------------ plain runs

@case("normal", "Boot with the card in: every demo step passes at the rate asked for, no IN_USE, no "
                "warnings, no FAULTY, and nothing changes once idle (VAL-H04: claim at configure, sync "
                "on every close, the lock table)")
def normal(board, opts):
    board.boot()
    run = wait_demo(board)
    expect_clean(run)
    expect_rate(run, opts.rate)
    check(not any(f"result {IN_USE}" in l for l in board.lines), "a step returned IN_USE")
    expect_never(board.lines, "faulty")
    expect_no_counted_failure(board.lines)
    idle = board.drain(3.0)
    check(not states(idle), f"the coordinator changed state while idle: {states(idle)}")
    return {"counter": run.counter, "bus": f"{run.bus_hz} Hz",
            "bulk write": f"{run.bulk_kibs.get('write')} KiB/s", "bulk read": f"{run.bulk_kibs.get('read')} KiB/s"}


@case("soak", "Reboot repeatedly (--boots, default 10): every boot brings the card up with no counted "
              "failure, every demo pass is clean, and the counter advances by exactly one each time")
def soak(board, opts):
    previous = None
    first = None
    write, read = [], []
    for boot in range(1, opts.boots + 1):
        board.note(f"boot {boot} of {opts.boots}")
        board.boot()
        run = wait_demo(board)
        expect_clean(run, f"boot {boot}")
        expect_no_counted_failure(board.lines)
        if previous is not None:
            check(run.counter == previous + 1,
                  f"boot {boot}: counter {run.counter} after {previous}; a save point lost or repeated a value")
        first = run.counter if first is None else first
        previous = run.counter
        write.append(run.bulk_kibs.get("write", 0))
        read.append(run.bulk_kibs.get("read", 0))
    return {"boots": opts.boots, "counter": f"{first} to {previous}",
            "bulk write": f"{min(write)}-{max(write)} KiB/s", "bulk read": f"{min(read)}-{max(read)} KiB/s"}


STACK_PATTERN = 0xDEADBEEF


def fill_stack(board, filled):
    """At main(): fill the unused stack with STACK_PATTERN, from the bottom of core 1's unused stack
    (SCRATCH_X), so an overflow past 4 KB shows too, up to just below main()'s own frame."""
    low = board.symbols["__StackLimit"]
    sp = board.ocd.register("sp")
    end = (sp - 64) & ~3
    board.ocd.write_words(low, [STACK_PATTERN] * ((end - low) // 4))
    filled.update(sp=sp, end=end)


def measure_stack(board, filled):
    """Halt, find the lowest overwritten word, resume; returns the summary notes. Fails past 4 KB."""
    symbols = board.symbols
    low, bottom, top = symbols["__StackLimit"], symbols["__StackBottom"], symbols["__StackTop"]
    board.halt()
    words = board.ocd.read_words(low, (filled["end"] - low) // 4)
    board.resume()
    lowest = next((low + 4 * i for i, word in enumerate(words) if word != STACK_PATTERN), filled["end"])
    used = top - lowest
    size = top - bottom
    check(lowest >= bottom, f"the stack grew {bottom - lowest} bytes past its {size} bytes, into core 1's "
                            "stack (SCRATCH_X)")
    margin = size - used
    return {"deepest use": f"{used} of {size} bytes", "margin": f"{margin} bytes" + (" (LOW)" if margin < 1024 else ""),
            "sp at main": f"0x{filled['sp']:08x}"}


@case("stack", "Stack high-water mark of the normal build: fill the unused stack at main(), run a full "
               "demo pass, and find the lowest overwritten word (VAL-H04, fs-6). Fails if the stack "
               "grew past its 4 KB")
def stack(board, opts):
    filled = {}
    board.boot(at_main=lambda b: fill_stack(b, filled))
    run = wait_demo(board)
    expect_clean(run)
    board.drain(1.0)
    return measure_stack(board, filled)


# ---------------------------------------------------------------------------- injected removals

def init_case(command, label):
    def run_case(board, opts):
        arm, state = injection_point(board, None, "sd_spi_command",
                                     lambda b: b.argument(1) == command, f"during init, at {label}")
        board.boot(arm=arm)
        wait_injected(board, state, 15, f"init to send {label}")
        board.wait_message(NOT_SEATED.pattern, 5, "the coordinator's 'card not seated'")
        run = wait_demo(board)
        expect_clean(run, "the pass after re-insertion")
        expect_no_counted_failure(board.lines)
        expect_never(board.lines, "faulty")
        return {"counter": run.counter}
    return run_case


case("removal_init_cmd8",
     "Removal injected at CMD8 during SD init: init rolls back, the coordinator logs 'card not seated' "
     "(not a counted failure), and the card is brought up again with no IO_ERROR, which shows the "
     "rollback kept the detect pin and unregistered the interrupt (VAL-H04)",
     group="removal")(init_case(8, "CMD8"))
case("removal_init_acmd41",
     "Removal injected at the first ACMD41 during SD init: as for CMD8 (VAL-H04)",
     group="removal")(init_case(41, "ACMD41"))


@case("removal_mount", "Removal injected at the mount's first sector read: the mount fails, the "
                       "coordinator treats it as a removal (ABSENT, never FAULTY, not counted), and the card "
                       "is mounted again (VAL-H04)", group="removal")
def removal_mount(board, opts):
    arm, state = injection_point(board, "filesystem_mount", "disk_read", lambda b: True,
                                 "at the mount's first sector read")
    board.boot(arm=arm)
    wait_injected(board, state, 15, "the mount")
    run = wait_demo(board)
    expect_clean(run, "the pass after re-insertion")
    after = states(board.lines[state["line"]:])
    check(any(medium == "absent" for _, medium, _ in after), f"no ABSENT after the injected removal: {after}")
    expect_never(board.lines, "faulty")
    expect_no_counted_failure(board.lines)
    return {"counter": run.counter}


SAVE_STAGES = {
    # name: (function, condition on its arguments, what the card holds at that point)
    "open": ("filesystem_open", lambda b: b.argument(1) == 1,           # FILESYSTEM_OPEN_REPLACE
             "just before the older slot is truncated"),
    "header": ("filesystem_write", lambda b: b.argument(2) == 12,       # the record's 12-byte header
               "the slot truncated, nothing new in it yet"),
    "sync": ("disk_ioctl", lambda b: b.argument(1) == 0,                # CTRL_SYNC
             "the record written, its close confirming it"),
}


def save_point_case(stage):
    function, condition, why = SAVE_STAGES[stage]

    def run_case(board, opts):
        # both slots must hold a record, so the save rewrites one while the other keeps the previous value
        base = demo_counter(board, opts, at_least=2)
        before = base.counter
        arm, state = injection_point(board, "save_point_write", function, condition,
                                     f"in a save point, {why}")
        board.boot(arm=arm)
        wait_injected(board, state, 20, f"the save point to reach {function}")
        m = board.wait_message(r"^FAIL save point write: result (\d+)$", 10, "the save point to fail")
        check(int(m.group(1)) == STORAGE_ERROR,
              f"the save failed with result {m.group(1)}, not {STORAGE_ERROR} (STORAGE_ERROR) as VAL-H04 expects")
        run = wait_demo(board)
        expect_clean(run, "the pass after re-insertion")
        # the interrupted save tried before + 1; the card may hold either value, and the next save adds one
        check(run.counter in (before + 1, before + 2),
              f"counter {run.counter} after the remount: the interrupted save could only leave {before} or "
              f"{before + 1}, so the next save gives {before + 1} or {before + 2}")
        expect_never(board.lines, "faulty")
        expect_no_counted_failure(board.lines)
        kept = "previous record" if run.counter == before + 1 else "new record"
        return {"counter": f"{before} before, {run.counter} after", "survived": kept}
    return run_case


for _stage, (_function, _condition, _why) in SAVE_STAGES.items():
    case(f"removal_save_{_stage}",
         f"Removal injected in a save point, {_why}: the save fails with STORAGE_ERROR, then after the "
         "remount the counter is the value before the save or the one after, never a third (VAL-H04)",
         group="removal")(save_point_case(_stage))


def bulk_write_removal(board):
    """Inject a removal inside the bulk file's multi-block write; check the failure and the recovery."""
    arm, state = injection_point(board, None, "disk_write", lambda b: b.argument(3) > 1,
                                 "inside the bulk file's multi-block write")
    board.boot(arm=arm)
    counter = int(board.wait_message(COUNTER.pattern, 20, "the demo's counter").group(1))
    wait_injected(board, state, 20, "the bulk write's multi-block transfer")
    m = board.wait_message(r"^FAIL bulk write: result (\d+)$", 10, "the bulk write to fail")
    check(int(m.group(1)) == STORAGE_ERROR,
          f"the bulk write failed with result {m.group(1)}, not {STORAGE_ERROR} (STORAGE_ERROR)")
    run = wait_demo(board)
    expect_clean(run, "the pass after re-insertion")
    check(run.counter == counter + 1, f"counter {run.counter} after the remount, expected {counter + 1}: "
                                      "the save point before the bulk step had completed")
    expect_never(board.lines, "faulty")
    expect_no_counted_failure(board.lines)
    return run


@case("removal_bulk_write", "Removal injected inside the bulk file's multi-block write: the write fails "
                            "with STORAGE_ERROR, the save point before it is intact, and the next pass is "
                            "clean (VAL-H03's step, at the current build)", group="removal")
def removal_bulk_write(board, opts):
    run = bulk_write_removal(board)
    return {"counter": run.counter}


# ------------------------------------------------------------------------------------------ rates

@case("rates", "VAL-H05: rebuild at each rate (--rates, default 5, 12.5 and 25 MHz), boot each "
               "--rate-boots times (default 3) with every pass clean and the bus at exactly the rate asked "
               "for, recording throughput; stop at the first rate that fails; then a removal during the "
               "bulk write at the highest passing rate. Reflashes the default build at the end",
      group="rates")
def rates(board, opts):
    results = {}
    highest = None
    failure = None
    try:
        for rate in opts.rates:
            board.flash(build(rate, log=board.note))
            write, read = [], []
            try:
                for boot in range(1, opts.rate_boots + 1):
                    board.note(f"{rate} Hz, boot {boot} of {opts.rate_boots}")
                    board.boot()
                    run = wait_demo(board)
                    expect_clean(run, f"{rate} Hz boot {boot}")
                    expect_rate(run, rate)
                    expect_no_counted_failure(board.lines)
                    write.append(run.bulk_kibs.get("write", 0))
                    read.append(run.bulk_kibs.get("read", 0))
            except CheckFailed as error:
                failure = f"{rate} Hz: {error}"
                results[f"{rate} Hz"] = "FAILED"
                break
            results[f"{rate} Hz"] = f"write {min(write)}-{max(write)}, read {min(read)}-{max(read)} KiB/s"
            highest = rate
        if highest is not None:
            if highest != opts.rates[-1] or failure is not None:
                board.flash(build(highest, log=board.note))
            bulk_write_removal(board)
            results["removal at"] = f"{highest} Hz, recovered"
    finally:
        board.flash(build(opts.rate, log=board.note))
    if failure is not None:
        raise CheckFailed(f"{failure}. Results so far: {results}")
    return results


# ------------------------------------------------------------------------------------ unformatted

UNFORMATTED_MESSAGE = re.compile(r"^no FAT volume on this card; format it as FAT32 on a computer$")


def hide_volume_at_mount(board):
    """Arm breakpoints that make the next mount find no volume: at the mount's read of sector 0, run to the
    read's return and clear the boot signature in the buffer. Call while halted (from boot's arm)."""

    def on_mount(b):
        b.disarm("filesystem_mount")
        b.arm("disk_read", on_read)

    def on_read(b):
        if b.argument(2) != 0:          # LBA_t is 32 bits (FF_LBA64 0), so the sector is r2
            return
        b.disarm("disk_read")
        buffer = b.argument(1)
        returned = b.ocd.register("lr")

        def on_return(b2):
            b2.disarm(returned)
            b2.hide_volume(buffer)

        b.arm(returned, on_return)

    board.arm("filesystem_mount", on_mount)


@case("unformatted", "A card whose mount finds no FAT volume (made to look so; the card is untouched) is "
                     "reported UNFORMATTED with the message to format it on a computer, and left alone: no "
                     "retry and no state change while idle. The firmware never formats. A normal boot "
                     "afterwards finds the volume and the counter as they were")
def unformatted(board, opts):
    before = demo_counter(board, opts, at_least=1).counter
    board.boot(arm=hide_volume_at_mount)
    board.wait_message(r"^storage: owner local, medium unformatted, epoch \d+$", 15, "UNFORMATTED")
    board.wait_message(UNFORMATTED_MESSAGE.pattern, 5, "the message to format the card on a computer")
    idle = board.drain(3.0)
    check(not states(idle), f"the coordinator changed state with an unformatted card: {states(idle)}")
    expect_never(board.lines, "faulty")
    expect_no_counted_failure(board.lines)
    board.boot()
    run = wait_demo(board)
    expect_clean(run, "the next boot")
    check(run.counter == before + 1, f"counter {run.counter} on the next boot, after {before}: the card "
                                     "changed while it was reported unformatted")
    return {"counter": f"{before}, then {run.counter}"}


# ------------------------------------------------------------------------------- injected faults

def fail_once_after(board, gates, target, why):
    """Breakpoint handlers that pass through each function in gates in turn, then make the next call of
    target return RES_ERROR without running. Each gate is a name or (name, condition). Returns (arm,
    state); arm is passed to boot."""
    state = {"done": False}
    gates = [g if isinstance(g, tuple) else (g, lambda b: True) for g in gates]

    def on_target(b):
        b.disarm(target)
        b.return_early(RES_ERROR, why)
        state["done"] = True

    def gate(i):
        name, condition = gates[i]

        def on_gate(b):
            if not condition(b):
                return
            b.disarm(name)
            if i + 1 < len(gates):
                b.arm(gates[i + 1][0], gate(i + 1))
            else:
                b.arm(target, on_target)
        return on_gate

    return (lambda b: b.arm(gates[0][0], gate(0))), state


@case("close_retry", "A write error at the first write of a save point's close (injected: disk_write "
                     "returns an error once) is absorbed by the close's retry: the save passes, the "
                     "readback matches, and the next boot reads the record back (VAL-H06)", group="faults")
def close_retry(board, opts):
    before = demo_counter(board, opts, at_least=2).counter
    # in the save: past the record's header write, into the slot's close, then its first device write
    arm, state = fail_once_after(board, ["save_point_write", ("filesystem_write", lambda b: b.argument(2) == 12),
                                         "filesystem_close"], "disk_write",
                                 "the first write of the save point's close")
    board.boot(arm=arm)
    run = wait_demo(board)
    check(state["done"], "the injection never fired: the save's close made no device write")
    expect_clean(run, "the pass with the failed write")
    check(run.counter == before + 1, f"counter {run.counter} after {before}")
    board.boot()
    run = wait_demo(board)
    expect_clean(run, "the next boot")
    check(run.counter == before + 2, f"counter {run.counter} on the next boot, expected {before + 2}: the "
                                     "retried save did not reach the card")
    expect_never(board.lines, "faulty")
    return {"counter": f"{before}, then {before + 1}, then {run.counter}"}


def bad_slot_a_sector(board):
    """Breakpoint handlers that make slot a's record sector unreadable, as a bad sector is, from the first
    save-point read of the boot until something writes over it (a card remaps a sector it rewrites). The
    sector is found at the open of a ".a" file for reading, that file's first filesystem_read, and the
    disk_read it makes. A slot a checked without a device read is a rig error (state "missed").
    Returns (arm, state)."""
    state = {"sector": None, "failed": 0, "rewritten": False, "missed": False}

    def covers(b):
        first, count = b.argument(2), b.argument(3)
        return first <= state["sector"] < first + count

    def on_any_read(b):
        if covers(b):
            state["failed"] += 1
            b.return_early(RES_ERROR, f"a read of slot a's sector {state['sector']}")

    def on_any_write(b):
        if covers(b):
            b.disarm("disk_read")
            b.disarm("disk_write")
            state["rewritten"] = True
            b.note(f"slot a's sector {state['sector']} rewritten: readable again")

    def on_first_read(b):
        b.disarm("filesystem_close")
        state["sector"] = b.argument(2)
        b.note(f"slot a's record is in sector {state['sector']}; it now fails every read until rewritten")
        b.arm("disk_read", on_any_read)
        b.arm("disk_write", on_any_write)
        on_any_read(b)

    def on_close(b):
        b.disarm("disk_read")
        b.disarm("filesystem_close")
        state["missed"] = True

    def on_read(b):
        b.disarm("filesystem_read")
        b.arm("disk_read", on_first_read)
        b.arm("filesystem_close", on_close)

    def on_open(b):
        path = b.ocd.read_string(b.argument(0))
        if not path.endswith(".a") or b.argument(1) != 0:       # FILESYSTEM_OPEN_READ
            return
        b.disarm("filesystem_open")
        b.arm("filesystem_read", on_read)

    def on_entry(b):
        b.disarm("save_point_read")
        b.arm("filesystem_open", on_open)

    return (lambda b: b.arm("save_point_read", on_entry)), state


def clean_boot(board, what):
    board.boot()
    run = wait_demo(board)
    expect_clean(run, what)
    return run.counter


def unreadable_round(board, label, newest_in=None, extra_clean=0):
    """Clean boots (1 + extra_clean), then one boot with slot a's record sector bad. newest_in, if known, is
    the slot holding the newest record before the bad boot. Returns (before, counter, rewritten)."""
    for _ in range(extra_clean):
        clean_boot(board, f"{label}: a boot before")
    before = clean_boot(board, f"{label}: the boot before")
    board.note(f"{label}: the boot with slot a's sector bad")
    arm, state = bad_slot_a_sector(board)
    board.boot(arm=arm)
    run = wait_demo(board)
    if state["missed"]:
        raise HilError(f"{label}: slot a was checked without a device read, so its sector is unknown")
    check(state["failed"] > 0, f"{label}: no read of slot a's sector was failed")
    expect_clean(run, f"{label}: the boot with slot a's sector bad")
    # newest in b: read as normal. newest in a: the read falls back to b's previous record. either way the
    # save rewrites slot a, so the counter is before + 1 or before
    allowed = {None: (before, before + 1), "a": (before,), "b": (before + 1,)}[newest_in]
    check(run.counter in allowed, f"{label}: counter {run.counter} after {before}; expected "
                                  f"{' or '.join(str(v) for v in allowed)}")
    return before, run.counter, state["rewritten"]


@case("unreadable_slot", "Slot a's record sector made unreadable (injected: every device read of it fails "
                         "until it is written, as a bad sector), starting from the newest record in each slot "
                         "in turn: no step fails; with the newest in slot b the read gets it, with it in slot a "
                         "the read falls back to slot b's previous record and the counter repeats; the save "
                         "rewrites slot a, after which it reads normally (VAL-H06)", group="faults")
def unreadable_slot(board, opts):
    demo_counter(board, opts, at_least=2)
    # after a bad-sector boot slot a holds the newest record, so the next clean boot leaves it in b, and
    # a second clean boot moves it back to a
    rounds = [unreadable_round(board, "as found"),
              unreadable_round(board, "newest in b", newest_in="b"),
              unreadable_round(board, "newest in a", newest_in="a", extra_clean=1)]
    after = clean_boot(board, "the clean boot after")
    check(after == rounds[-1][1] + 1, f"counter {after} on the clean boot after {rounds[-1][1]}")
    expect_never(board.lines, "faulty")
    return {label: f"{b}, then {c}" + (", slot a rewritten in place" if w else "")
            for label, (b, c, w) in zip(("as found", "newest in b", "newest in a"), rounds)}


def fail_cmd8(board, times):
    """Arm sd_spi_command to fail CMD8 with IO_ERROR on the next `times` calls, as a card that answers badly
    would, then let it through. Returns the state dict ("failed": how many were failed)."""
    state = {"failed": 0}

    def on_command(b):
        if b.argument(1) != 8:
            return
        if state["failed"] < times:
            state["failed"] += 1
            b.return_early(IO_ERROR, f"CMD8, attempt {state['failed']}")
        if state["failed"] >= times:
            b.disarm("sd_spi_command")

    return (lambda b: b.arm("sd_spi_command", on_command)), state


@case("bring_up_retries", "SD init made to fail at CMD8 (sd_spi_command returning IO_ERROR): each failure "
                          "names its step ('sd: init failed at CMD8') and is a counted attempt; the attempts "
                          "follow the schedule in storage.h (STORAGE_BRING_UP_DELAYS_MS); a card failing all "
                          "but the last comes up on it, and one failing every attempt is parked FAULTY after "
                          "the whole schedule (VAL-H07's fumbled insertion)", group="faults")
def bring_up_retries(board, opts):
    attempts, waits = bring_up_schedule()
    check(len(waits) == attempts - 1, f"storage.h has {len(waits)} waits for {attempts} attempts")
    results = {}
    for label, failing in (("recovers", attempts - 1), ("parked", attempts)):
        board.note(f"{label}: CMD8 failing on {failing} of {attempts} attempts")
        arm, state = fail_cmd8(board, failing)
        board.boot(arm=arm)
        start = len(board.lines)
        stamps = []

        def watch(line):
            text = message(line)
            if BRING_UP_FAILED.match(text):
                stamps.append(time.monotonic())
            return STATE_READY_OR_FAULTY.match(text) is not None

        line = board.wait_for(watch, 30, "READY or FAULTY")
        ended = STATE_READY_OR_FAULTY.match(message(line)).group(1)
        lines = board.lines[start:]
        failures = [m for m in (BRING_UP_FAILED.match(message(l)) for l in lines) if m]
        steps = [m.group(1) for m in (INIT_FAILED.match(message(l)) for l in lines) if m]
        check(state["failed"] == failing, f"{label}: {state['failed']} CMD8s failed, meant {failing}")
        check(len(failures) == failing, f"{label}: {len(failures)} counted failures, expected {failing}")
        check([int(m.group(1)) for m in failures] == list(range(1, failing + 1))
              and all(int(m.group(2)) == attempts for m in failures),
              f"{label}: attempts logged as {[m.group(0) for m in failures]}")
        check(steps == ["CMD8"] * failing, f"{label}: the driver named the failed steps {steps}")
        check(all(int(m.group(4)) == IO_ERROR for m in failures), f"{label}: results {[m.group(4) for m in failures]}")
        # the gap from one failure to the next is the scheduled wait plus the next attempt's own time,
        # breakpoints included (RTT timestamps are polled, so allow some slack both ways)
        gaps = [round((b - a) * 1000) for a, b in zip(stamps, stamps[1:])]
        for n, (gap, wait) in enumerate(zip(gaps, waits), start=2):
            check(wait - 40 <= gap <= wait + 250, f"{label}: attempt {n} came {gap} ms after the one before, "
                                                  f"scheduled {wait} ms")
        if label == "recovers":
            check(ended == "ready", f"recovers: the card went {ended.upper()} on its last attempt")
            run = wait_demo(board, epoch=int(STATE_READY_OR_FAULTY.match(message(line)).group(2)))
            expect_clean(run, "recovers: the pass")
        else:
            check(ended == "faulty", "parked: the card did not go FAULTY after every attempt failed")
            idle = board.drain(2.0)
            check(not states(idle), f"parked: the coordinator changed state: {states(idle)}")
        results[label] = f"{len(failures)} failures at {', '.join(str(g) for g in gaps)} ms"
    board.boot()
    run = wait_demo(board)
    expect_clean(run, "a clean boot after")
    return results


STATE_READY_OR_FAULTY = re.compile(r"^storage: owner local, medium (ready|faulty), epoch (\d+)$")


# ------------------------------------------------------------------------------- manual (physical)
#
# These need a person at the bench: the run stops, says on the terminal what to do (with a bell), and
# carries on by itself once it sees it done - from the firmware's log while the core runs, or from the
# card-detect pin over the probe while the core is halted at a breakpoint. Enter also carries on. Only
# run when asked for (run.py manual).

HUMAN = 300.0           # seconds allowed for a person to do something


def wait_absent(board, what):
    match = board.wait_message(r"^storage: owner local, medium absent, epoch (\d+)$", HUMAN, what)
    return int(match.group(1))


def wait_inserted_pass(board, what):
    run = wait_demo(board, timeout=HUMAN, ready_timeout=HUMAN)
    expect_clean(run, what)
    return run


def ready_and_calibrated(board):
    """Boot with the card in, run a clean pass, and calibrate the card-detect level."""
    board.boot()
    run = wait_demo(board)
    expect_clean(run, "the first pass")
    board.calibrate_socket()
    return run


@case("manual_idle", "Physical removal and insertion while idle, --cycles times (default 10), the last "
                     "insertion made slowly: each removal is one ABSENT with the epoch advanced, each "
                     "insertion settles and reaches READY with no counted failure, and every pass is clean "
                     "with the counter one higher (VAL-H04, VAL-H05)", group="manual")
def manual_idle(board, opts):
    counter = ready_and_calibrated(board).counter
    epoch = 0
    for cycle in range(1, opts.cycles + 1):
        board.ask(f"cycle {cycle} of {opts.cycles}: pull the card out")
        new_epoch = wait_absent(board, "the removal")
        check(new_epoch > epoch, f"cycle {cycle}: epoch {new_epoch} after {epoch}; a removal must advance it")
        epoch = new_epoch
        slow = cycle == opts.cycles
        board.ask(f"cycle {cycle} of {opts.cycles}: put the card back in"
                  + (" SLOWLY, taking a second or two to push it home" if slow else ""))
        run = wait_inserted_pass(board, f"cycle {cycle}")
        check(run.counter == counter + 1, f"cycle {cycle}: counter {run.counter} after {counter}")
        counter = run.counter
    expect_no_counted_failure(board.lines)
    expect_never(board.lines, "faulty")
    return {"cycles": opts.cycles, "counter": f"to {counter}", "epoch": epoch}


@case("manual_cardless_boot", "Boot with no card in: ABSENT at epoch 0 and no bring-up attempted; then "
                              "insertion settles and reaches READY with a clean pass, which shows the "
                              "card-detect pin was claimed at configure (VAL-H04)", group="manual")
def manual_cardless_boot(board, opts):
    counter = ready_and_calibrated(board).counter
    board.ask("pull the card out, and leave it out")
    wait_absent(board, "the removal")
    board.boot()
    board.wait_message(r"^storage: owner local, medium absent, epoch 0$", 15, "ABSENT at boot")
    idle = board.drain(3.0)
    check(not states(idle), f"the coordinator changed state with no card: {states(idle)}")
    board.ask("put the card back in")
    run = wait_inserted_pass(board, "the pass after insertion")
    check(run.counter == counter + 1, f"counter {run.counter} after {counter}")
    expect_no_counted_failure(board.lines)
    expect_never(board.lines, "faulty")
    return {"counter": f"{counter}, then {run.counter}"}


def physical_point(board, entry, stage, condition, why):
    """As injection_point, but the person at the bench pulls the card while the core is halted at the
    stage: the run waits until the card-detect pin reads empty, then resumes."""
    state = {"done": False, "line": None}

    def on_stage(b):
        if not condition(b):
            return
        b.disarm(stage)
        b.ask(f"the core is halted {why}: pull the card out now")
        how = b.wait_socket_empty(HUMAN)
        b.note(f"card pulled while halted {why} (seen by {how}); resuming")
        print("  ... resuming", flush=True)
        state["done"] = True
        state["line"] = len(b.lines)

    def on_entry(b):
        b.disarm(entry)
        b.arm(stage, on_stage)

    def arm(b):
        if entry is None:
            b.arm(stage, on_stage)
        else:
            b.arm(entry, on_entry)

    return arm, state


def manual_removal_case(entry, stage, condition, why, expect_fail=None, needs_records=False,
                        expect_message=None):
    def run_case(board, opts):
        counter = ready_and_calibrated(board).counter
        if needs_records:
            counter = demo_counter(board, opts, at_least=2).counter
        arm, state = physical_point(board, entry, stage, condition, why)
        board.boot(arm=arm)
        wait_injected(board, state, 60, f"the run to reach the point {why}")
        if expect_message is not None:
            board.wait_message(expect_message, 10, expect_message)
        if expect_fail is not None:
            m = board.wait_message(rf"^FAIL {expect_fail}: result (\d+)$", 10, f"{expect_fail} to fail")
            check(int(m.group(1)) == STORAGE_ERROR, f"{expect_fail} failed with result {m.group(1)}, "
                                                     f"not {STORAGE_ERROR} (STORAGE_ERROR)")
        wait_absent(board, "ABSENT after the removal")
        board.ask("put the card back in")
        run = wait_inserted_pass(board, "the pass after reinsertion")
        if needs_records:
            check(run.counter in (counter + 1, counter + 2),
                  f"counter {run.counter} after {counter}: the interrupted save could only leave {counter} "
                  f"or {counter + 1}")
        expect_no_counted_failure(board.lines)
        expect_never(board.lines, "faulty")
        notes = {"counter": f"{counter} before, {run.counter} after"}
        if needs_records:
            notes["survived"] = "previous record" if run.counter == counter + 1 else "new record"
        return notes
    return run_case


case("manual_removal_init", "Card pulled with the core halted at CMD8 of SD init: 'card not seated', not a "
                            "counted failure, ABSENT; reinserted, READY with a clean pass (VAL-H04)",
     group="manual")(manual_removal_case(None, "sd_spi_command", lambda b: b.argument(1) == 8,
                                         "at CMD8 of SD init", expect_message=NOT_SEATED.pattern))
case("manual_removal_mount", "Card pulled with the core halted at the mount's first sector read: ABSENT, "
                             "never FAULTY, not counted; reinserted, mounted with a clean pass (VAL-H04)",
     group="manual")(manual_removal_case("filesystem_mount", "disk_read", lambda b: True,
                                         "at the mount's first sector read"))
for _stage, (_function, _condition, _why) in SAVE_STAGES.items():
    case(f"manual_removal_save_{_stage}",
         f"Card pulled with the core halted in a save point, {_why}: the save fails with STORAGE_ERROR, "
         "ABSENT; reinserted, the counter is the value before the save or the one after (VAL-H04)",
         group="manual")(manual_removal_case("save_point_write", _function, _condition,
                                             f"in a save point, {_why}", expect_fail="save point write",
                                             needs_records=True))


@case("manual_removal_bulk_write", "Card pulled with the core halted inside the first data block of the "
                                   "bulk file's multi-block write (the card selected, mid-CMD25): the write "
                                   "fails with STORAGE_ERROR, ABSENT; reinserted, a clean pass with the save "
                                   "point before it intact (VAL-H03's gap, at a confirmed point)",
      group="manual")
def manual_removal_bulk_write(board, opts):
    counter = ready_and_calibrated(board).counter
    # inside the transfer: the per-byte CRC of the first data block, once a multi-block write has begun
    state = {"done": False}

    def on_crc(b):
        b.disarm("crc_helper_rolling_16")
        b.ask("the core is halted inside the bulk file's multi-block write, mid data block: pull the card "
              "out now")
        how = b.wait_socket_empty(HUMAN)
        b.note(f"card pulled mid data block (seen by {how}); resuming")
        print("  ... resuming", flush=True)
        state["done"] = True

    def on_write(b):
        if b.argument(3) > 1:
            b.disarm("disk_write")
            b.arm("crc_helper_rolling_16", on_crc)

    board.boot(arm=lambda b: b.arm("disk_write", on_write))
    wait_injected(board, state, 60, "the bulk write")
    board.wait_message(r"^FAIL bulk write: result 11$", 10, "the bulk write to fail with STORAGE_ERROR")
    wait_absent(board, "ABSENT after the removal")
    board.ask("put the card back in")
    run = wait_inserted_pass(board, "the pass after reinsertion")
    check(run.counter == counter + 2, f"counter {run.counter}: expected {counter + 2}, the save point of the "
                                      "interrupted pass intact and the next one written")
    expect_no_counted_failure(board.lines)
    expect_never(board.lines, "faulty")
    return {"counter": f"{counter}, then {run.counter}"}


@case("manual_fumbled_insertion", "The card pushed partly in, left a moment, then pushed home, as in "
                                  "VAL-H07's finding: the bring-up attempts spread over about 3.5 s outlast "
                                  "it, and the card reaches READY, never FAULTY, with a clean pass",
      group="manual")
def manual_fumbled_insertion(board, opts):
    counter = ready_and_calibrated(board).counter
    board.ask("pull the card out")
    wait_absent(board, "the removal")
    start = len(board.lines)
    board.ask("push the card in only PARTLY, until the breakout's LED starts flashing, wait about two "
              "seconds, then push it home")
    run = wait_inserted_pass(board, "the pass after the fumbled insertion")
    failures = [message(l) for l in board.lines[start:] if BRING_UP_FAILED.match(message(l))]
    steps = [INIT_FAILED.match(message(l)).group(1) for l in board.lines[start:]
             if INIT_FAILED.match(message(l))]
    check(run.counter == counter + 1, f"counter {run.counter} after {counter}")
    expect_never(board.lines, "faulty")
    return {"failed attempts before READY": len(failures),
            "init steps that failed": ", ".join(steps) or "none"}


# pieces of fsck.fat -n output that say nothing about the volume's state. matched anywhere in the pasted
# text, not as whole lines: a terminal can paste the output with its line breaks removed
FSCK_ROUTINE = re.compile(r"fsck\.fat \d[\d.]* \([^)]*\)|Leaving filesystem unchanged\.?|Auto-correcting\.?"
                          r"|Checking [^.]*\.")
# the device path starts at its "/", which also finds it when a lost line break runs it into the text before
FSCK_SUMMARY = re.compile(r"/[^\s:]*: (\d+) files?, (\d+)/(\d+) clusters")
# the FSInfo free cluster count: a hint FatFs keeps from mount to sync, which a removal between a file's
# truncation and its close leaves stale (VAL-H07). not a structural error; reported, not failed
FSCK_FREE_COUNT = re.compile(r"Free cluster summary (?:wrong \((\d+) vs\. really (\d+)\)|uninitialized)")


def read_fsck_output(readline):
    """The pasted fsck.fat output, read up to its summary, which fsck prints last (its output has blank
    lines of its own, so a blank line cannot end it), or to end of input (Ctrl-D) for output with no
    summary. readline returns "" at end of input."""
    lines = []
    while True:
        line = readline()
        if line == "":
            return lines
        lines.append(line.rstrip("\n"))
        if FSCK_SUMMARY.search(line):
            return lines


def classify_fsck(lines):
    """Split fsck.fat -n output into (summary match, free-count note or None, anything else left over)."""
    text = " ".join(lines)
    summary = FSCK_SUMMARY.search(text)
    count = FSCK_FREE_COUNT.search(text)
    note = None
    if count:
        note = (f"FSInfo free count {int(count.group(2)) - int(count.group(1)):+d} off ({count.group(1)} "
                f"stored, {count.group(2)} counted)" if count.group(1) else "FSInfo free count uninitialized")
    rest = FSCK_ROUTINE.sub(" ", FSCK_FREE_COUNT.sub(" ", FSCK_SUMMARY.sub(" ", text)))
    other = rest.split() and [" ".join(rest.split())]
    return summary, note, other or []


@case("manual_host_check", "Run last: the card checked on a computer with fsck.fat -n after everything "
                           "above, then put back and read again with a clean pass. Any structural error "
                           "fails; a stale FSInfo free cluster count (a hint, not structure) is reported",
      group="manual")
def manual_host_check(board, opts):
    counter = ready_and_calibrated(board).counter
    board.ask("take the card out and check it on your computer: sudo fsck.fat -n /dev/<its partition>. "
              "Leave it out until you have the output")
    wait_absent(board, "the removal")
    print("  paste fsck.fat's whole output (it ends at the 'files, clusters' line; Ctrl-D if there is "
          "none):", flush=True)
    pasted = read_fsck_output(sys.stdin.readline)
    board.drain_input()             # the rest of the paste, so it cannot act as an Enter press later
    for line in pasted:
        board.note(f"fsck: {line}")
    summary, note, other = classify_fsck(pasted)
    board.ask("put the card back in")
    run = wait_inserted_pass(board, "the pass after the host check")
    check(summary is not None, "no fsck.fat summary line (files and clusters) in the pasted output")
    check(not other, "fsck.fat reported: " + "; ".join(other))
    check(run.counter == counter + 1, f"counter {run.counter} after {counter}")
    notes = {"fsck": summary.group(0)}
    if note:
        notes["note"] = note
    return notes


# ------------------------------------------------------------------------------------------- usb
#
# The card lent to USB and returned, with the bench computer as the host: the Pico 2's USB port must be
# connected to the computer running the tests (usb_host.py). The policy is asked through its request
# variable, as the GDB harness is meant to; nothing here needs hands at the bench. A cable removal cannot
# be tested on this bench: the Pico 2 is powered through the same cable, so pulling it is a power cut.

USB_ENTER = 1           # USB_MODE_REQUEST_ENTER (src/usb/usb_mode.h)
USB_LEAVE = 2           # USB_MODE_REQUEST_LEAVE
HOST_FILE = "tome-hil.txt"


def every(seconds, poll):
    """poll, but run at most once per interval: the host-side checks start processes, and a wait polls
    after every round of log reading."""
    last = [float("-inf")]

    def limited():
        if time.monotonic() - last[0] < seconds:
            return False
        last[0] = time.monotonic()
        return poll()
    return limited


def wait_host(board, poll, timeout, what, interval=0.2):
    """Keep reading the log and servicing breakpoints until poll() holds on the host side."""
    if not poll():
        board.wait_for(lambda line: False, timeout, what, poll=every(interval, poll))


def usb_disk(board):
    """The device's disk on the host, once the host has attached it: it enumerates afresh on every reset."""
    found = {}

    def attached():
        found["name"] = usb_host.disk()
        return found["name"] is not None
    wait_host(board, attached, 10, "the host to attach the device")
    return found["name"]


def wait_medium(board, name, present, timeout, what):
    wait_host(board, lambda: (usb_host.blocks(name) > 0) == present, timeout, what)
    return usb_host.blocks(name)


def run_start(board):
    """Where the case's own work starts in the log: the request that lent the card. Before it, a counted
    bring-up failure can be the last case's doing: a reset while the card was lent, with the host reading
    it, can catch the card mid-transfer, and the first CMD0 after it then goes unanswered."""
    return next((i for i, line in enumerate(board.lines) if USB_LENDING.match(message(line))), 0)


def lend(board):
    """Boot with a clean pass, check the host finds no medium, and ask for USB mode until the host finds
    the card. Returns (the pass, the disk, its size on the host in sectors)."""
    board.boot()
    run = wait_demo(board)
    expect_clean(run)
    #the request waits for a host to configure the device, and the device enumerates afresh after every
    #reset, which can take the host a few seconds; the disk in /sys/block may still be the last boot's
    if not any(USB_CONFIGURED.match(message(line)) for line in board.lines):
        board.wait_message(USB_CONFIGURED.pattern, 20, "a host to configure the device")
    name = usb_disk(board)
    check(usb_host.blocks(name) == 0, "the host found a medium before the card was lent")
    board.request_usb(USB_ENTER, "lend the card to the host")
    board.wait_message(USB_LENDING.pattern, 5, "the policy to lend the card")
    board.wait_message(r"^storage: owner usb, medium ready", 10, "the card ready for USB")
    return run, name, wait_medium(board, name, True, 15, "the host to find the medium")


def mount(board, name):
    """Mount the volume through udisks, once udev has identified it; returns (device, mount point)."""
    found = {}

    def mounted():
        found["device"] = usb_host.partition(name)
        found["point"] = usb_host.try_mount(found["device"])
        return found["point"] is not None
    wait_host(board, mounted, 120, "udisks to mount the volume", interval=1.0)
    board.note(f"host: /dev/{found['device']} mounted at {found['point']}")
    return found["device"], found["point"]


def wait_returned(board, why):
    match = board.wait_message(USB_RETURNING.pattern, 10, "the policy to return the card")
    check(match.group(1) == why, f"the card was returned because {match.group(1)!r}, expected {why!r}")


def listing(lines):
    """The demo's root listing in a list of log lines: name -> size."""
    return {m.group(1): int(m.group(3)) for m in (LISTED.match(message(l)) for l in lines) if m}


@case("usb_round_trip", "The card lent on request; the host mounts it, writes a file and ejects it; the "
                        "card comes back with a clean pass whose listing has the host's file at its size, "
                        "and the host is left with no medium", group="usb")
def usb_round_trip(board, opts):
    run, name, blocks = lend(board)
    device, point = mount(board, name)
    #a different size each run, so a file left by an earlier run cannot pass for this one
    text = "written by the host over USB after demo pass %d\n" % run.counter
    text += "." * (run.counter % 97) + "\n"
    size = usb_host.write_file(point, HOST_FILE, text)
    usb_host.unmount(device)
    start = len(board.lines)
    usb_host.eject(name)
    board.note(f"host: wrote {HOST_FILE} ({size} bytes), unmounted and ejected")
    wait_returned(board, "the host ejected it")
    after = wait_demo(board)
    expect_clean(after, "the pass after the return")
    check(after.counter == run.counter + 1, f"counter {after.counter} after {run.counter}")
    listed = listing(board.lines[start:])
    check(HOST_FILE in listed, f"{HOST_FILE} is not in the listing after the return: {sorted(listed)}")
    check(listed[HOST_FILE] == size, f"{HOST_FILE} listed at {listed[HOST_FILE]} bytes, written {size}")
    wait_medium(board, name, False, 10, "the host to lose the medium")
    check(usb_host.disk() == name, "the device left the bus after the eject; it should stay on it, charging")
    expect_never(board.lines[run_start(board):], "faulty")
    expect_no_counted_failure(board.lines[run_start(board):])
    return {"host size": f"{blocks} sectors", "file": f"{size} bytes", "counter": after.counter}


@case("usb_leave_by_request", "The card lent, then asked back through the request variable: returned with "
                              "a clean pass, and the host left with no medium", group="usb")
def usb_leave_by_request(board, opts):
    run, name, _ = lend(board)
    usb_host.unmount(usb_host.partition(name))      #in case the desktop mounted it
    board.request_usb(USB_LEAVE, "ask for the card back")
    wait_returned(board, "requested")
    after = wait_demo(board)
    expect_clean(after, "the pass after the return")
    check(after.counter == run.counter + 1, f"counter {after.counter} after {run.counter}")
    wait_medium(board, name, False, 10, "the host to lose the medium")
    return {"counter": after.counter}


@case("usb_removal_while_lent", "A removal injected while the card is lent: ABSENT for the USB owner with "
                                "no return to the local owner, the seated card brought up for USB again at a "
                                "new epoch and found by the host; then asked back, with a clean pass",
      group="usb")
def usb_removal_while_lent(board, opts):
    run, name, _ = lend(board)
    usb_host.unmount(usb_host.partition(name))
    start = len(board.lines)
    board.inject_removal("while lent to USB")
    #the card stays seated, so the step that loses it starts settling it again: the log shows the loss as
    #PREPARING at the new epoch, or ABSENT if a step falls between
    lost = board.wait_message(r"^storage: owner usb, medium (absent|preparing), epoch (\d+)$", 5,
                              "the card lost for USB")
    ready = board.wait_message(r"^storage: owner usb, medium ready, epoch (\d+)$", 10,
                               "the card brought up for USB again")
    check(int(lost.group(2)) == run.epoch + 2, f"lost at epoch {lost.group(2)}: the lend is epoch "
                                               f"{run.epoch + 1}, so the loss should be {run.epoch + 2}")
    check(int(ready.group(1)) == int(lost.group(2)), "the epoch changed again between the loss and READY")
    check(not any(USB_RETURNING.match(message(l)) for l in board.lines[start:]),
          "the policy returned the card on a removal; it should stay lent")
    wait_medium(board, name, True, 15, "the host to find the medium again")
    board.request_usb(USB_LEAVE, "ask for the card back")
    wait_returned(board, "requested")
    after = wait_demo(board)
    expect_clean(after, "the pass after the return")
    expect_never(board.lines[run_start(board):], "faulty")
    #an injected removal leaves the card powered, and if the host was reading it (udev probes a new
    #medium for tens of seconds at 1 MHz) the card is left mid multi-block read: the first CMD0 after it
    #can go unanswered and be retried. a physical removal takes the card's power, so it cannot happen
    #there. any other counted failure is one
    failures = [message(l) for l in board.lines[start:] if BRING_UP_FAILED.match(message(l))]
    cmd0 = [message(l) for l in board.lines[start:] if (m := INIT_FAILED.match(message(l))) and m.group(1) == "CMD0"]
    check(len(failures) <= 1 and len(cmd0) == len(failures),
          "counted bring-up failures other than one CMD0 retry: " + "; ".join(failures))
    expect_no_counted_failure(board.lines[run_start(board):start])
    return {"epoch": f"lost and ready again at {ready.group(1)}", "counter": after.counter,
            "CMD0 retries": len(cmd0)}
