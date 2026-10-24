# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT

"""Hardware-in-the-loop plumbing for the Tavernkeep hardware tests.

Everything goes through OpenOCD's Tcl RPC port: flashing, reset, hardware breakpoints, register reads,
memory reads and writes, and RTT. The demo's log arrives over OpenOCD's RTT server, which this module
reads as lines. No GDB session is involved; GDB is only run offline, against the ELF, to resolve
addresses that nm cannot give (a struct member).

The pieces, bottom up:
  build()      configure and build a firmware variant in its own build directory
  Symbols      addresses from the ELF (nm, plus one offline GDB query)
  OpenOcd      the OpenOCD process and its Tcl RPC connection
  Rtt          the RTT server connection, as lines
  Board        flash, boot (with RTT started once logging is up), breakpoint handlers, and the event
               loop that reads log lines and services breakpoints until a condition is met
  log parsing  the demo's log formats (src/main.c, src/storage/storage.c), and DemoRun, one demo pass
"""

import os
import re
import select
import socket
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SDK = os.path.expanduser("~/.pico-sdk")
TOOLCHAIN_BIN = os.path.join(SDK, "toolchain", "15_2_Rel1", "bin")
NM = os.path.join(TOOLCHAIN_BIN, "arm-none-eabi-nm")
GDB = os.path.join(TOOLCHAIN_BIN, "arm-none-eabi-gdb")
OPENOCD_DIR = os.path.join(SDK, "openocd", "0.12.0+dev")
OPENOCD = os.path.join(OPENOCD_DIR, "openocd")
NINJA = os.path.join(SDK, "ninja", "v1.13.2", "ninja")

TCL_PORT = 6666
RTT_PORT = 9090
TARGET = "rp2350.cm0"

# the demo's default rate (SD_BAUD_HZ in src/main.c)
DEFAULT_RATE_HZ = 25000000

# the card-detect pin's input level, readable over the probe while the core is halted: IO_BANK0's
# GPIO12_STATUS (PIN_WP in src/main.c), INFROMPAD
CARD_DETECT_STATUS = 0x40028060
CARD_DETECT_IN = 1 << 17


class HilError(Exception):
    """The test rig failed (probe missing, OpenOCD refused a command): no verdict on the firmware."""


class CheckFailed(Exception):
    """The firmware did not do what the test expected."""


def check(condition, message):
    if not condition:
        raise CheckFailed(message)


# ------------------------------------------------------------------------------------------- build

def build_dir_for(rate_hz=None):
    name = "build-hil"
    if rate_hz is not None and rate_hz != DEFAULT_RATE_HZ:
        name += f"-{rate_hz}"
    return os.path.join(ROOT, name)


def build(rate_hz=None, log=print):
    """Configure (once) and build a Release firmware with RTT logging; returns the ELF path.

    A rate other than the default is compiled in with -DSD_BAUD_HZ, passed through CFLAGS at configure
    time (passing CMAKE_C_FLAGS instead would replace the SDK toolchain's own flags). Each rate has its
    own build directory, since CFLAGS is only read when a build directory is first configured."""
    build_dir = build_dir_for(rate_hz)
    env = os.environ.copy()
    if not os.path.exists(os.path.join(build_dir, "CMakeCache.txt")):
        if rate_hz is not None and rate_hz != DEFAULT_RATE_HZ:
            env["CFLAGS"] = f"-DSD_BAUD_HZ={rate_hz}U"
        configure = ["cmake", "-S", ROOT, "-B", build_dir, "-G", "Ninja",
                     f"-DCMAKE_MAKE_PROGRAM={NINJA}", "-DPICO_BOARD=pico2", "-DPICO_PLATFORM=rp2350-arm-s",
                     "-DCMAKE_BUILD_TYPE=Release", "-DTAVERNKEEP_STDIO_RTT=ON",
                     "-DTAVERNKEEP_ENABLE_LOGGING=ON"]
        log(f"configuring {os.path.relpath(build_dir, ROOT)}")
        result = subprocess.run(configure, env=env, capture_output=True, text=True)
        if result.returncode != 0:
            raise HilError("configure failed:\n" + result.stdout[-3000:] + result.stderr[-3000:])
    result = subprocess.run([NINJA, "-C", build_dir], capture_output=True, text=True)
    if result.returncode != 0:
        raise HilError("build failed:\n" + result.stdout[-4000:] + result.stderr[-2000:])
    warnings = result.stdout.count("warning:")
    if warnings:
        log(f"note: {warnings} compiler warnings in {os.path.relpath(build_dir, ROOT)}")
    return os.path.join(build_dir, "tavernkeep.elf")


# ----------------------------------------------------------------------------------------- symbols

class Symbols:
    """Addresses the tests break on or touch, read from the ELF that is flashed."""

    # functions breakpoints are set on. each must exist out of line under exactly this name: a clone
    # such as name.constprop.0 may take different arguments, which would make a register condition lie
    FUNCTIONS = ("main", "platform_gpio_irq_init", "sd_spi_command", "sd_spi_device_init",
                 "filesystem_mount", "filesystem_open", "filesystem_write", "filesystem_read",
                 "filesystem_close", "save_point_write", "save_point_read", "disk_read", "disk_write",
                 "disk_ioctl", "crc_helper_rolling_16")
    DATA = ("_SEGGER_RTT", "__StackTop", "__StackBottom", "__StackLimit", "usb_mode_request")

    def __init__(self, elf):
        self.elf = elf
        out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
        self.addresses = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3:
                self.addresses.setdefault(parts[2], int(parts[0], 16))
        for name in self.FUNCTIONS + self.DATA:
            if name not in self.addresses:
                raise HilError(f"{name} is not in {elf}; the tests need it as a real symbol")
        for name in self.FUNCTIONS:
            clones = [n for n in self.addresses if n.startswith(name + ".")]
            if clones:
                raise HilError(f"{name} has compiler clones ({', '.join(clones)}): its arguments may not "
                               "be where the breakpoint conditions expect them")
        self.removal_latch = self._gdb_address("&sd.removal_latched")

    def __getitem__(self, name):
        return self.addresses[name] & ~1

    def _gdb_address(self, expression):
        result = subprocess.run([GDB, "-batch", "-nx", "-ex", f"print/x (unsigned){expression}", self.elf],
                                capture_output=True, text=True)
        match = re.search(r"= (0x[0-9a-f]+)", result.stdout)
        if not match:
            raise HilError(f"cannot resolve {expression} in {self.elf}: {result.stdout}{result.stderr}")
        return int(match.group(1), 16)


# ----------------------------------------------------------------------------------------- openocd

class OpenOcd:
    """OpenOCD with the Debug Probe and the RP2350's core 0, driven over its Tcl RPC port."""

    def __init__(self, log_path):
        self.log_file = open(log_path, "w")
        args = [OPENOCD, "-s", os.path.join(OPENOCD_DIR, "scripts"),
                "-c", "set USE_CORE cm0",
                "-f", "interface/cmsis-dap.cfg", "-f", "target/rp2350.cfg",
                "-c", "adapter speed 5000",
                "-c", f"tcl port {TCL_PORT}", "-c", "gdb port disabled", "-c", "telnet port disabled"]
        self.process = subprocess.Popen(args, stdout=self.log_file, stderr=subprocess.STDOUT)
        self.sock = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log_file.flush()
                raise HilError("OpenOCD exited during start-up (probe not connected, or no permission?):\n"
                               + open(log_path).read()[-2000:])
            try:
                self.sock = socket.create_connection(("127.0.0.1", TCL_PORT), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if self.sock is None:
            self.close()
            raise HilError("OpenOCD's Tcl port never opened")
        self.sock.settimeout(30)
        self.cmd(f"targets {TARGET}")

    def cmd(self, command, check=True):
        """Run one Tcl command; returns its output. Raises HilError if it fails and check is set."""
        script = "set tk_rc [catch {" + command + "} tk_out]; format \"%d %s\" $tk_rc $tk_out"
        self.sock.sendall(script.encode() + b"\x1a")
        data = b""
        while not data.endswith(b"\x1a"):
            chunk = self.sock.recv(65536)
            if not chunk:
                raise HilError("OpenOCD closed the Tcl connection")
            data += chunk
        text = data[:-1].decode(errors="replace")
        rc, _, output = text.partition(" ")
        if check and rc != "0":
            raise HilError(f"OpenOCD command failed: {command}\n{output}")
        return output

    def state(self):
        return self.cmd(f"{TARGET} curstate").strip()

    def register(self, name):
        match = re.search(r"0x[0-9a-fA-F]+", self.cmd(f"reg {name}"))
        if not match:
            raise HilError(f"cannot read register {name}")
        return int(match.group(0), 16)

    def read_words(self, address, count):
        words = []
        while count:
            n = min(count, 256)
            words += [int(w, 16) for w in self.cmd(f"read_memory 0x{address:08x} 32 {n}").split()]
            address += 4 * n
            count -= n
        return words

    def read_string(self, address, limit=128):
        """A NUL-terminated string in target memory, up to limit bytes."""
        data = bytes(int(b, 16) for b in self.cmd(f"read_memory 0x{address:08x} 8 {limit}").split())
        return data.split(b"\0", 1)[0].decode(errors="replace")

    def write_words(self, address, words):
        for i in range(0, len(words), 256):
            chunk = " ".join(f"0x{w:08x}" for w in words[i:i + 256])
            self.cmd(f"write_memory 0x{address + 4 * i:08x} 32 {{{chunk}}}")

    def write_byte(self, address, value):
        self.cmd(f"write_memory 0x{address:08x} 8 {{{value}}}")

    def close(self):
        try:
            if self.sock is not None:
                self.cmd("shutdown", check=False)
        except (OSError, HilError):
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self.log_file.close()


# --------------------------------------------------------------------------------------------- rtt

class Rtt:
    """The RTT server's TCP stream, split into lines."""

    def __init__(self):
        deadline = time.monotonic() + 5
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", RTT_PORT), timeout=1)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise HilError("cannot connect to OpenOCD's RTT server")
                time.sleep(0.1)
        self.sock.setblocking(False)
        self.pending = b""

    def lines(self, wait):
        """Complete lines received within wait seconds (possibly none)."""
        ready, _, _ = select.select([self.sock], [], [], wait)
        if ready:
            try:
                chunk = self.sock.recv(65536)
            except BlockingIOError:
                chunk = None
            if chunk == b"":
                # readable with nothing to read: OpenOCD dropped the client (an rtt stop can); reconnect
                self.sock.close()
                self.__init__()
            elif chunk:
                self.pending += chunk
        *complete, self.pending = self.pending.split(b"\n")
        return [line.decode(errors="replace").rstrip("\r") for line in complete]


# ------------------------------------------------------------------------------------------- board

class Board:
    """One Pico 2 with the demo firmware, its log, and the breakpoint handlers a test arms."""

    def __init__(self, ocd, case_log):
        self.ocd = ocd
        self.case_log = case_log            # a callable taking one line
        self.symbols = None
        self.rtt = None
        self.rtt_server_started = False
        self.handlers = {}                  # breakpoint address -> handler(board)
        self.lines = []                     # every log line since the last boot
        self.backlog = deque()              # lines read from RTT but not yet handed to a wait
        self.started = time.monotonic()

    # ---- firmware

    def flash(self, elf):
        self.symbols = Symbols(elf)
        self.ocd.cmd("reset halt")
        self.ocd.cmd(f"program {{{elf}}} verify")
        self.case_log(f"# flashed {os.path.relpath(elf, ROOT)}")

    # ---- breakpoints

    def _address(self, target):
        return target & ~1 if isinstance(target, int) else self.symbols[target]

    def arm(self, target, handler):
        """Break at a function, by name, or at an address, and call handler(board) on each hit."""
        address = self._address(target)
        if address not in self.handlers:
            self.ocd.cmd(f"bp 0x{address:08x} 2 hw")
        self.handlers[address] = handler

    def disarm(self, target):
        address = self._address(target)
        if self.handlers.pop(address, None) is not None:
            self.ocd.cmd(f"rbp 0x{address:08x}")

    def disarm_all(self):
        for address in list(self.handlers):
            self.ocd.cmd(f"rbp 0x{address:08x}")
        self.handlers.clear()

    def argument(self, n):
        """Argument n (0-3) of the function the core is halted at the entry of (AAPCS: r0-r3)."""
        return self.ocd.register(f"r{n}")

    def hide_volume(self, buffer):
        """Clear the boot signature (bytes 510-511) of a sector the core has just read into buffer, so FatFs
        finds no volume there. The card itself is untouched."""
        self.ocd.write_byte(buffer + 510, 0)
        self.ocd.write_byte(buffer + 511, 0)
        self.note(f"hid the volume: cleared the boot signature of sector 0 in the buffer at 0x{buffer:08x}")

    # ---- the person at the bench (manual cases)

    def ask(self, text):
        """Tell the person at the bench what to do, on the terminal, with a bell."""
        print(f"\n\a  >>> {text}", flush=True)
        self.note(f"asked: {text}")

    def _enter_pressed(self):
        ready, _, _ = select.select([sys.stdin], [], [], 0)
        if ready:
            sys.stdin.readline()
            return True
        return False

    def drain_input(self):
        """Discard whatever is left on the terminal's input, after a paste."""
        time.sleep(0.2)
        while self._enter_pressed():
            pass

    def socket_level(self):
        """The card-detect pin's level, read from the GPIO block (works with the core halted)."""
        return bool(self.ocd.read_words(CARD_DETECT_STATUS, 1)[0] & CARD_DETECT_IN)

    def calibrate_socket(self):
        """Record the card-detect level with a card known to be in, so absence is the other level."""
        self.present_level = self.socket_level()
        self.note(f"card-detect level with the card in: {'high' if self.present_level else 'low'}")

    def wait_socket_empty(self, timeout=300.0):
        """With the core halted: wait until the card-detect pin reads empty, or Enter is pressed.
        Returns how it ended ("socket" or "enter")."""
        if getattr(self, "present_level", None) is None:
            raise HilError("calibrate_socket was never called, so an empty socket cannot be recognised")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.socket_level() != self.present_level:
                time.sleep(0.2)                     # let the switch settle before resuming
                return "socket"
            if self._enter_pressed():
                return "enter"
            time.sleep(0.05)
        raise CheckFailed(f"the socket never read empty within {timeout:.0f} s")

    def return_early(self, value, why):
        """At a function's entry breakpoint: return value to the caller without running the function, as
        if it had failed (r0 is the return value, lr the return address; AAPCS)."""
        self.ocd.cmd(f"reg r0 {value}")
        self.ocd.cmd(f"reg pc 0x{self.ocd.register('lr') & ~1:08x}")
        self.note(f"made a call return {value}: {why}")

    def inject_removal(self, why):
        """Do what the card-detect interrupt does on a removal: set the driver's removal latch. The card
        stays powered and seated, so this tests the firmware's handling of a removal, not the card's."""
        self.ocd.write_byte(self.symbols.removal_latch, 1)
        self.note(f"injected removal: {why}")

    def request_usb(self, request, why):
        """Write the USB-mode policy's request variable (usb_mode.h), as the firmware's own docs say the GDB
        harness does; the policy's next step takes it. 1 asks for entry, 2 for the card back."""
        self.ocd.write_words(self.symbols["usb_mode_request"], [request])
        self.note(f"usb_mode_request = {request}: {why}")

    # ---- boot

    def _wait_halted(self, timeout, what):
        deadline = time.monotonic() + timeout
        while self.ocd.state() != "halted":
            if time.monotonic() > deadline:
                raise HilError(f"core never halted at {what}")
            time.sleep(0.01)

    def _run_to(self, function, timeout=5.0):
        address = self.symbols[function]
        self.ocd.cmd(f"bp 0x{address:08x} 2 hw")
        self.ocd.cmd("resume")
        self._wait_halted(timeout, function)
        self.ocd.cmd(f"rbp 0x{address:08x}")
        pc = self.ocd.register("pc")
        if pc != address:
            raise HilError(f"halted at 0x{pc:08x}, expected {function} at 0x{address:08x}")

    def boot(self, at_main=None, arm=None):
        """Reset and run the firmware with RTT capturing its log from the first line.

        RTT is started at platform_gpio_irq_init, the first call after stdio is up, so that the control
        block is the one this boot initialized and nothing the demo logs is missed. at_main, if given,
        runs while the core is halted at main(), before anything else has used the stack. arm, if
        given, runs while halted after RTT is up, to set the test's breakpoints before storage starts.
        The core is running when this returns."""
        self.disarm_all()
        self.lines = []
        self.backlog.clear()
        self.started = time.monotonic()
        self.case_log("# reset")
        self.ocd.cmd("reset halt")
        if at_main is not None:
            self._run_to("main")
            at_main(self)
        self._run_to("platform_gpio_irq_init")
        if self.rtt_server_started:
            self.ocd.cmd("rtt stop", check=False)
        self.ocd.cmd(f"rtt setup 0x{self.symbols['_SEGGER_RTT']:08x} 0x40 \"SEGGER RTT\"")
        self.ocd.cmd("rtt polling_interval 10", check=False)
        self.ocd.cmd("rtt start")
        if not self.rtt_server_started:
            self.ocd.cmd(f"rtt server start {RTT_PORT} 0")
            self.rtt_server_started = True
            self.rtt = Rtt()
        if arm is not None:
            arm(self)
        self.ocd.cmd("resume")

    def resume(self):
        self.ocd.cmd("resume")

    def halt(self):
        self.ocd.cmd("halt")
        self._wait_halted(2, "halt request")

    # ---- the event loop

    def note(self, text):
        self.case_log(f"# {text}")

    def wait_for(self, predicate, timeout, what, poll=None):
        """Read log lines and service breakpoints until predicate(line) is true, returning that line, or
        until poll() is true (checked after each round of breakpoint handling), returning None.

        The core must be running (or halted at an armed breakpoint). A halt anywhere else is a rig
        error. Waits are on log lines, never fixed sleeps, so slower rates only take longer."""
        deadline = time.monotonic() + timeout
        while True:
            # one read can bring many lines; those after the one that ends this wait stay in the backlog
            # for the next wait, which would otherwise never see them (READY and the demo's first steps
            # often arrive together)
            if not self.backlog:
                self.backlog.extend(self.rtt.lines(0.02))
            while self.backlog:
                line = self.backlog.popleft()
                self.lines.append(line)
                self.case_log(f"{time.monotonic() - self.started:8.3f} {line}")
                if predicate(line):
                    return line
            if self.handlers and self.ocd.state() == "halted":
                pc = self.ocd.register("pc")
                handler = self.handlers.get(pc)
                if handler is None:
                    raise HilError(f"core halted at 0x{pc:08x}, where no breakpoint is armed")
                handler(self)
                self.ocd.cmd("resume")
            if poll is not None and poll():
                return None
            if time.monotonic() > deadline:
                raise CheckFailed(f"timed out after {timeout:.0f} s waiting for {what}")

    def wait_message(self, pattern, timeout, what=None):
        """Wait for a log message matching a regular expression; returns the match."""
        regex = re.compile(pattern)
        line = self.wait_for(lambda l: regex.search(message(l)) is not None, timeout, what or pattern)
        return regex.search(message(line))

    def drain(self, seconds):
        """Keep logging and servicing breakpoints for a while; returns the lines read."""
        start = len(self.lines)
        try:
            self.wait_for(lambda line: False, seconds, "nothing")
        except CheckFailed:
            pass
        return self.lines[start:]


# ------------------------------------------------------------------------------------- log formats
#
# Patterns for the lines the tests key on, kept few and anchored on the stable parts. Each SOURCE_TEXT
# entry is a literal that must still appear in the sources: run.py --offline checks it, so a reworded log
# line breaks the offline check rather than silently making a test time out on hardware.

LEVEL = re.compile(r"^\[(INFO|WARN|ERROR)\] (.*)$")
STATE = re.compile(r"^storage: owner (\w+), medium (\w+), epoch (\d+)$")
PASS = re.compile(r"^PASS (.+?)(?:, \d+ bytes)?$")
FAIL = re.compile(r"^FAIL ([^:]+)(?:: result (\d+))?")
BUS = re.compile(r"^sd bus at (\d+) Hz \(asked for (\d+) Hz\)$")
COUNTER = re.compile(r"^this card has now been mounted by the demo (\d+) times$")
BULK = re.compile(r"^bulk (write|read): (\d+) bytes in (\d+) us, (\d+) KiB/s$")
NOT_SEATED = re.compile(r"^storage: card not seated")
INIT_FAILED = re.compile(r"^sd: init failed at (.+), result (-?\d+)$")
BRING_UP_FAILED = re.compile(r"^storage: bring-up attempt (\d+) of (\d+) failed, (.+) result (-?\d+)$")
LISTED = re.compile(r"^  (.+?)(/?) (\d+)$")
USB_LENDING = re.compile(r"^usb: lending the card to the host$")
USB_CONFIGURED = re.compile(r"^usb: a host configured the device$")
USB_RETURNING = re.compile(r"^usb: returning the card: (.+)$")

SOURCE_TEXT = {
    "src/main.c": ["\"storage: owner %s, medium %s, epoch %lu\"", "\"PASS %s\"", "\"FAIL %s: result %d\"",
                   "\"sd bus at %lu Hz (asked for %lu Hz)\"",
                   "\"this card has now been mounted by the demo %lu times\"",
                   "\"bulk %s: %u bytes in %lu us, %lu KiB/s\"", "\"PASS bulk verify, %u bytes\"",
                   "\"save point write\"", "\"counter readback\"", "\"bulk write\"", "\"bulk read\"",
                   "\"no FAT volume on this card; format it as FAT32 on a computer\"", "\"  %s%s %lu\""],
    "src/storage/sd_spi.c": ["\"sd: init failed at %s, result %d\""],
    "src/storage/storage.c": ["\"storage: card not seated (init refused it), waiting\"",
                              "\"storage: bring-up attempt %u of %d failed, %s result %d\""],
    "src/platform/debug.c": ["\"[%s] \""],
    "src/usb/usb_mode.c": ["\"usb: lending the card to the host\"", "\"usb: returning the card: %s\"",
                           "\"a host configured the device\"",
                           "\"the host ejected it\"", "\"requested\""],
}

# the steps of one complete, successful demo pass, in order (src/main.c, filesystem_demo)
DEMO_STEPS = ["label", "opendir", "readdir", "closedir", "save point write", "counter readback",
              "bulk write", "bulk read", "bulk verify"]

STORAGE_ERROR = 11      # FILESYSTEM_RESULT_STORAGE_ERROR (src/storage/filesystem.h)
IN_USE = 13             # FILESYSTEM_RESULT_IN_USE
IO_ERROR = 4            # BLOCK_DEVICE_RESULT_IO_ERROR (src/storage/block_device.h)
RES_ERROR = 1           # FatFs's DRESULT for a read or write error (diskio.h)


def message(line):
    """The message part of a log line, without its [LEVEL] prefix."""
    match = LEVEL.match(line)
    return match.group(2) if match else line


def level(line):
    match = LEVEL.match(line)
    return match.group(1) if match else None


@dataclass
class DemoRun:
    """One pass of the filesystem demo, from READY to its last step."""
    epoch: int = -1
    bus_hz: int = 0
    asked_hz: int = 0
    counter: int = None
    passed: list = field(default_factory=list)
    failed: list = field(default_factory=list)      # (step, result or None)
    bulk_us: dict = field(default_factory=dict)     # write/read -> microseconds
    bulk_kibs: dict = field(default_factory=dict)   # write/read -> KiB/s
    warnings: list = field(default_factory=list)

    @property
    def complete(self):
        return not self.failed and self.passed[-1:] == ["bulk verify"]

    def missing_steps(self):
        return [step for step in DEMO_STEPS if step not in self.passed]


READY_OR_FAULTY = re.compile(r"^storage: owner local, medium (ready|faulty), epoch (\d+)$")


def wait_ready(board, timeout):
    """Wait for the local owner's medium to become READY; returns the epoch. A card that goes FAULTY
    instead stays so until it is removed, so that fails at once, with the attempts that failed."""
    start = len(board.lines)
    line = board.wait_for(lambda l: READY_OR_FAULTY.match(message(l)) is not None, timeout, "READY")
    match = READY_OR_FAULTY.match(message(line))
    if match.group(1) == "faulty":
        failures = [message(l) for l in board.lines[start:] if BRING_UP_FAILED.match(message(l))]
        raise CheckFailed("the card went FAULTY instead of READY: " + "; ".join(failures))
    return int(match.group(2))


def wait_demo(board, timeout=30.0, ready_timeout=15.0, epoch=None):
    """Wait for READY, then follow the demo pass it starts until its last step passes or a step fails.
    With epoch given, the READY line (at that epoch) has already been read."""
    run = DemoRun(epoch=wait_ready(board, ready_timeout) if epoch is None else epoch)
    deadline = time.monotonic() + timeout

    def step(line):
        text = message(line)
        if level(line) == "WARN":
            run.warnings.append(text)
        if (m := BUS.match(text)):
            run.bus_hz, run.asked_hz = int(m.group(1)), int(m.group(2))
        elif (m := COUNTER.match(text)):
            run.counter = int(m.group(1))
        elif (m := BULK.match(text)):
            run.bulk_us[m.group(1)] = int(m.group(3))
            run.bulk_kibs[m.group(1)] = int(m.group(4))
        elif (m := FAIL.match(text)):
            run.failed.append((m.group(1), int(m.group(2)) if m.group(2) else None))
            return True
        elif (m := PASS.match(text)):
            run.passed.append(m.group(1))
            return m.group(1) == "bulk verify"
        return False

    board.wait_for(step, max(1.0, deadline - time.monotonic()), "the end of the demo pass")
    return run


def states(lines):
    """The (owner, medium, epoch) transitions in a list of log lines."""
    out = []
    for line in lines:
        if (m := STATE.match(message(line))):
            out.append((m.group(1), m.group(2), int(m.group(3))))
    return out


def bring_up_schedule():
    """STORAGE_BRING_UP_ATTEMPTS and STORAGE_BRING_UP_DELAYS_MS, read from src/storage/storage.h, so the
    tests follow the firmware's schedule rather than a copy of it."""
    header = open(os.path.join(ROOT, "src", "storage", "storage.h")).read()
    attempts = re.search(r"#define STORAGE_BRING_UP_ATTEMPTS (\d+)", header)
    delays = re.search(r"#define STORAGE_BRING_UP_DELAYS_MS \{([^}]*)\}", header)
    if not attempts or not delays:
        raise HilError("cannot read the bring-up schedule from src/storage/storage.h")
    waits = [int(v.strip().rstrip("uU")) for v in delays.group(1).split(",")]
    return int(attempts.group(1)), waits
