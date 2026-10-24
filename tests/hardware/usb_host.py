# SPDX-FileCopyrightText: 2026 Harsh Patel
# SPDX-License-Identifier: MIT

"""The bench computer's side of the USB cases: the device as a Linux host sees it.

The Pico 2's USB port must be connected to the computer running the tests. The disk is found by its
SCSI vendor, "Tome", under /sys/block; its size there is 0 while the host finds no medium. The kernel
polls removable media (block.events_dfl_poll_msecs, 2 s by default), so a medium appearing or leaving is
seen within a few seconds without anything else asking.

Mounting and ejecting go through udisks, as the desktop's own mount and eject do, so the tests need no
root and no membership of the disk group: udisksctl mounts and unmounts, and the drive's Eject method
sends the SCSI eject (START STOP UNIT with LoEj) the firmware acts on. udev identifies the volume first by
reading it over USB, which at 1 MHz SPI takes tens of seconds; the mount waits for that.
"""

import glob
import os
import re
import subprocess

VENDOR = "Tome"


class HostError(Exception):
    pass


def disk():
    """The device's block device name (sdX), or None if the host has not attached it."""
    for path in glob.glob("/sys/block/sd*"):
        try:
            with open(os.path.join(path, "device", "vendor")) as f:
                if f.read().strip() == VENDOR:
                    return os.path.basename(path)
        except OSError:
            pass
    return None


def blocks(name):
    """The medium's size in 512-byte sectors as the host sees it; 0 with no medium, or no disk."""
    try:
        with open(f"/sys/block/{name}/size") as f:
            return int(f.read())
    except (OSError, ValueError, TypeError):
        return 0


def partition(name):
    """The first partition's device name, or the disk itself if it has none (a volume with no MBR)."""
    parts = sorted(os.path.basename(p) for p in glob.glob(f"/sys/block/{name}/{name}[0-9]*"))
    return parts[0] if parts else name


def mountpoint(device):
    out = subprocess.run(["lsblk", "-no", "MOUNTPOINTS", f"/dev/{device}"], capture_output=True, text=True)
    lines = [line.strip() for line in out.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def try_mount(device):
    """Mount through udisks; returns the mount point, or None if udisks does not know the volume yet."""
    existing = mountpoint(device)
    if existing:
        return existing
    out = subprocess.run(["udisksctl", "mount", "-b", f"/dev/{device}", "--no-user-interaction"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        if "Error looking up object" in out.stderr or "not a mountable filesystem" in out.stderr:
            return None
        raise HostError(f"udisksctl mount failed: {out.stderr.strip()}")
    return mountpoint(device)


def unmount(device):
    if mountpoint(device) is None:
        return
    out = subprocess.run(["udisksctl", "unmount", "-b", f"/dev/{device}", "--no-user-interaction"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise HostError(f"udisksctl unmount failed: {out.stderr.strip()}")


def eject(name):
    """Eject the medium through the drive's udisks Eject method."""
    info = subprocess.run(["udisksctl", "info", "-b", f"/dev/{name}"], capture_output=True, text=True)
    match = re.search(r"Drive:\s+'([^']+)'", info.stdout)
    if not match:
        raise HostError(f"no udisks drive for /dev/{name}: {info.stderr.strip()}")
    out = subprocess.run(["gdbus", "call", "--system", "--dest", "org.freedesktop.UDisks2",
                          "--object-path", match.group(1), "--method", "org.freedesktop.UDisks2.Drive.Eject",
                          "{'auth.no_user_interaction': <true>}"], capture_output=True, text=True)
    if out.returncode != 0:
        raise HostError(f"eject failed: {out.stderr.strip()}")


def write_file(directory, filename, text):
    """Write a file and wait until the host has written it out to the device."""
    path = os.path.join(directory, filename)
    with open(path, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    return len(text.encode())
