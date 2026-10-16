#!/usr/bin/env python3
"""SurfCPU card installer for the Mac
--------------------
File: deploy-sd.py Date: 10/15/26 (SurfCPU)
--------------------
Installs SurfOS on the microSD card, mounted on the MacBook (docs/CARD.md):

    make deploy-sd MOUNT=/Volumes/SURFOS
    tools/deploy-sd.py --mount /Volumes/SURFOS [--image PATH] [--backup-image PATH] [--no-backup]
                       [--cmdline TEXT | --cmdline-file FILE] [--system-dir DIR] [--new-card]
                       [--dry-run | --verify-only]

Checks first, writes nothing until all pass:
  1. the mount point is named explicitly and `mount` lists it as an msdos volume;
  2. `diskutil info -plist` says FAT32, writable, removable media (a built-in SD slot or a USB
     card reader; not a fixed disk, not a disk image or other virtual device), not a system
     volume, and the whole disk has an MBR partition table (SurfBoot reads MBR only);
  3. the directory written through is that device: the mount point is opened once
     (O_DIRECTORY|O_NOFOLLOW) and its st_dev must equal the st_rdev of diskutil's DeviceNode;
  4. the package passes SurfBoot's own checks (tools/surfimg.py): the x86 surfos.bin, an ELF
     or a disk image is refused;
  5. the card carries /SYSTEM/SURFCARD.TXT, the SurfOS card marker (the first install says
     --new-card and writes it), so a different USB stick or card is not written by mistake;
  6. nothing it would touch is a symbolic link (macOS shows FAT "XSym" files as links), and
     /BOOT and /SYSTEM hold no link at all;
  7. there is room (fstatvfs).
Every directory and file is opened relative to an open directory with O_NOFOLLOW, and new
files are created under fresh temporary names with O_EXCL, so nothing outside the card can be
reached. Then: /BOOT /SYSTEM /PROGRAMS /USERS /USERS/DOCUMENTS are created if missing; the
package is written to a temporary file in /BOOT and flushed to the card (fsync, F_FULLFSYNC);
the previous SURFOS.BIN, if it is a valid package and different, becomes /BOOT/BACKUP.BIN
(unless --no-backup or --backup-image); the temporary file becomes SURFOS.BIN; CMDLINE.TXT,
/SYSTEM files and the marker the same way.

Verification: the card is then unmounted (which writes everything out and makes the Mac drop
what it cached of the volume) and mounted again with diskutil; the checks of 1-3 are made
again on the remounted volume, which must be on the same device node, and every installed
file is read back and compared byte for byte (which also reads the FAT and the directories
from the card). Exit status: 0 verified; 1 refused (nothing was changed);
2 written but NOT verified (the card could not be unmounted or mounted again, or an error
stopped the update part way); 3 read back and found different. --verify-only does the
remount and the comparison for the given files without writing. It never formats or
repartitions, never touches files other than those it installs, and keeps the user
directories. FAT32 has no journal: pulling the card out during the copy can damage the file
system, and the backup file does not make an update atomic.

Runs with the Mac's /usr/bin/python3 (3.9) as well as Homebrew's; only the standard library
and macOS's own diskutil and mount are used. For tests, SURFCPU_DEPLOY_TEST=1 lets it run off
macOS against a directory stand-in, with SURFCPU_DISKUTIL and SURFCPU_MOUNT naming fake
commands and the expected device number taken from diskutil's answer (TestDeviceNumber); in
that mode a path that is a mount point, or under /Volumes, is refused, so a test can never
reach a real card.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import os
import platform
import plistlib
import re
import secrets
import shlex
import stat
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent                              # the repository root (surfcpu/..)
DEFAULT_IMAGE = REPO / "build" / "riscv32" / "arty_a7" / "SURFOS.BIN"
LAYOUT = ("BOOT", "SYSTEM", "PROGRAMS", "USERS", "USERS/DOCUMENTS")
MARKER_DIR, MARKER_NAME = "SYSTEM", "SURFCARD.TXT"
TEST_MODE = os.environ.get("SURFCPU_DEPLOY_TEST") == "1"
F_FULLFSYNC = 51                                       # macOS fcntl command
O_DIR = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | os.O_NOFOLLOW
EXIT_REFUSED, EXIT_NOT_VERIFIED, EXIT_MISMATCH = 1, 2, 3


class Refusal(Exception):
    """A check failed before anything was written."""


class NotVerified(Exception):
    """Something was written, but it could not be read back from the card."""


def load_surfimg():
    spec = importlib.util.spec_from_file_location("surfimg", HERE / "surfimg.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def command(env_name: str, default: str) -> list[str]:
    value = os.environ.get(env_name) if TEST_MODE else None
    return shlex.split(value) if value else [default]


def run_status(cmd: list[str]) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, str(e)
    return p.returncode, (p.stdout + p.stderr).decode(errors="replace").strip()


def run(cmd: list[str]) -> bytes:
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Refusal(f"could not run {' '.join(cmd)}: {e}")
    if p.returncode != 0:
        raise Refusal(f"{' '.join(cmd)} failed: {p.stderr.decode(errors='replace').strip()}")
    return p.stdout


def diskutil(*args: str) -> list[str]:
    return command("SURFCPU_DISKUTIL", "diskutil") + list(args)


def diskutil_info(target: str) -> dict:
    data = run(diskutil("info", "-plist", target))
    try:
        return plistlib.loads(data)
    except Exception as e:
        raise Refusal(f"diskutil info {target}: unreadable answer ({e})")


def mount_entry(mount: str) -> tuple[str, str]:
    """(device, file system type) of the mount point, from `mount`."""
    out = run(command("SURFCPU_MOUNT", "mount")).decode(errors="replace")
    for line in out.splitlines():
        m = re.match(r"^(\S+) on (.+) \(([^,)]+)", line)
        if m and os.path.realpath(m.group(2)) == mount:
            return m.group(1), m.group(3)
    raise Refusal(f"{mount} is not a mount point (mount does not list it); give the card's mount point, "
                  "for example /Volumes/SURFOS")


def virtual(info: dict) -> bool:
    return info.get("BusProtocol") == "Disk Image" or info.get("VirtualOrPhysical") == "Virtual"


def device_number(info: dict, device: str) -> int:
    """The dev_t that files on this volume must carry: st_rdev of its device node."""
    if TEST_MODE:
        if "TestDeviceNumber" not in info:
            raise Refusal("diskutil's answer has no TestDeviceNumber (test mode)")
        return int(info["TestDeviceNumber"])
    node = info.get("DeviceNode") or device
    try:
        return os.stat(node).st_rdev
    except OSError as e:
        raise Refusal(f"cannot stat the card's device node {node}: {e}")


def test_mode_guard(mount: str):
    """Test mode only: refuse anything that could be a real volume."""
    if mount == "/Volumes" or mount.startswith(("/Volumes/", "/System/Volumes/")):
        raise Refusal("test mode never touches /Volumes")
    parent = os.path.dirname(mount) or "/"
    if os.path.isdir(mount) and os.stat(mount).st_dev != os.stat(parent).st_dev:
        raise Refusal(f"test mode: {mount} is a mount point, not a directory standing in for a card")


class Volume:
    """The mounted card, checked, with its root directory open: every later file operation is
    relative to self.root, whose device was matched against diskutil's DeviceNode."""

    def __init__(self, mount_arg: str):
        mount = os.path.realpath(mount_arg)
        if TEST_MODE:
            test_mode_guard(mount)
        if not os.path.isdir(mount):
            raise Refusal(f"{mount_arg} is not a directory: is the card mounted?")
        device, fstype = mount_entry(mount)
        if fstype != "msdos":
            raise Refusal(f"{mount} is a {fstype} volume, not FAT (msdos); SurfBoot reads FAT32 only")
        info = diskutil_info(device)
        name = f"{info.get('FilesystemName', '')} {info.get('FilesystemUserVisibleName', '')}"
        if info.get("FilesystemType") != "msdos" or "FAT32" not in name:
            raise Refusal(f"{mount} is {name.strip() or info.get('FilesystemType')}: SurfBoot needs FAT32 "
                          "(exFAT and FAT16 are refused; docs/CARD.md shows how to format the card)")
        if virtual(info):
            raise Refusal(f"{mount} ({device}) is a disk image or another virtual device, not a card")
        if not (info.get("RemovableMedia") or info.get("Removable")):
            where = "internal" if info.get("Internal") else "external"
            raise Refusal(f"{mount} ({device}) is on a fixed {where} disk, not removable media; refusing")
        if not info.get("WritableVolume", info.get("Writable", True)):
            raise Refusal(f"{mount} is read-only (is the card's lock switch on?)")
        if info.get("SystemImage") or mount in ("/", "/System/Volumes/Data"):
            raise Refusal(f"{mount} is a system volume; refusing")
        whole = info.get("ParentWholeDisk")
        if not whole:
            raise Refusal(f"diskutil did not name the whole disk of {device}, so its partition table cannot be "
                          "checked; refusing")
        winfo = diskutil_info(whole)
        if winfo.get("Content") != "FDisk_partition_scheme":
            raise Refusal(f"the card ({whole}) has a {winfo.get('Content') or 'unknown'} partition table; SurfBoot "
                          "reads MBR (FDisk) partition tables only: docs/CARD.md")
        if virtual(winfo):
            raise Refusal(f"{whole} is a disk image or another virtual device, not a card")
        total = info.get("TotalSize") or info.get("Size") or 0
        if total and total > 2 << 40:
            raise Refusal(f"{mount} is {total} bytes: larger than any microSD card this is meant for")
        self.devnum = device_number(info, device)
        try:
            self.root = os.open(mount, O_DIR)
        except OSError as e:
            raise Refusal(f"cannot open {mount} as a directory without following links: {e}")
        if os.fstat(self.root).st_dev != self.devnum:
            os.close(self.root)
            raise Refusal(f"{mount} is not on {info.get('DeviceNode') or device}: the directory and the device "
                          "diskutil describes are not the same; refusing")
        self.mount, self.device, self.info = mount, device, info
        self.node = info.get("DeviceNode") or device
        self.whole = whole
        self.fds: list[int] = []

    # ---- operations relative to directory fds, never following links ----
    def kind(self, dirfd: int, name: str, shown: str) -> str:
        try:
            st = os.stat(name, dir_fd=dirfd, follow_symlinks=False)
        except FileNotFoundError:
            return "absent"
        if stat.S_ISLNK(st.st_mode):
            raise Refusal(f"{shown} is a symbolic link (FAT \"XSym\" file); refusing to follow it")
        if stat.S_ISDIR(st.st_mode):
            return "dir"
        if stat.S_ISREG(st.st_mode):
            return "file"
        raise Refusal(f"{shown} is neither a file nor a directory; refusing")

    def opendir(self, dirfd: int, name: str, shown: str) -> int:
        if self.kind(dirfd, name, shown) != "dir":
            raise Refusal(f"{shown} is not a directory")
        try:
            fd = os.open(name, O_DIR, dir_fd=dirfd)
        except OSError as e:
            raise Refusal(f"cannot open {shown}: {e}")
        if os.fstat(fd).st_dev != self.devnum:
            os.close(fd)
            raise Refusal(f"{shown} is on another device (a mount point inside the card?); refusing")
        self.fds.append(fd)
        return fd

    def no_links(self, dirfd: int, shown: str):
        with os.scandir(dirfd) as it:
            for e in it:
                if e.is_symlink():
                    raise Refusal(f"{shown}/{e.name} is a symbolic link (FAT \"XSym\" file); the card's {shown} must "
                                  "hold none: refusing")

    def read(self, dirfd: int, name: str, shown: str) -> bytes | None:
        k = self.kind(dirfd, name, shown)
        if k == "absent":
            return None
        if k != "file":
            raise Refusal(f"{shown} is a directory, not a file")
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=dirfd)
        try:
            st = os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_dev != self.devnum:
                raise Refusal(f"{shown} is not a regular file on the card")
            chunks = []
            while True:
                b = os.read(fd, 1 << 20)
                if not b:
                    break
                chunks.append(b)
            return b"".join(chunks)
        finally:
            os.close(fd)

    def write_temp(self, dirfd: int, data: bytes) -> str:
        for _ in range(64):
            name = f"~DP{secrets.randbelow(1 << 20):05X}.TMP"         # an 8.3 name
            try:
                fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644, dir_fd=dirfd)
            except FileExistsError:
                continue
            try:
                view = memoryview(data)
                while view:
                    n = os.write(fd, view)
                    view = view[n:]
                os.fsync(fd)
                if platform.system() == "Darwin":
                    import fcntl
                    fcntl.fcntl(fd, getattr(fcntl, "F_FULLFSYNC", F_FULLFSYNC))
            except OSError:
                os.close(fd)
                try:
                    os.unlink(name, dir_fd=dirfd)
                except OSError:
                    pass
                raise
            os.close(fd)
            return name
        raise Refusal("no free temporary name could be created")

    def close(self):
        for fd in self.fds:
            os.close(fd)
        self.fds = []
        if self.root >= 0:
            os.close(self.root)
            self.root = -1


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def remount(vol: Volume) -> Volume:
    """Unmount and mount the card again, then check it all over again (NotVerified on failure).
    The unmount writes out everything and the Mac forgets what it cached of the volume, so what
    is read afterwards comes from the card."""
    node, mount = vol.node, vol.mount
    vol.close()
    os.sync()
    for attempt in range(3):                       # Spotlight or fseventsd may hold it for a moment
        rc, out = run_status(diskutil("unmount", mount))
        if rc == 0:
            break
        time.sleep(0 if TEST_MODE else 2)
    if rc:
        raise NotVerified(f"the card could not be unmounted ({out or 'diskutil failed'}), so it could not be read "
                          "back from the card itself")
    rc, out = run_status(diskutil("mount", node))
    if rc:
        raise NotVerified(f"the card was unmounted but could not be mounted again ({out or 'diskutil failed'})")
    try:
        new_mount = diskutil_info(node).get("MountPoint")
        if not new_mount:
            raise Refusal(f"diskutil does not say where {node} is mounted")
        again = Volume(new_mount)
    except Refusal as e:
        raise NotVerified(f"after mounting it again: {e}")
    if again.node != node:
        again.close()
        raise NotVerified(f"after mounting it again, {new_mount} is on {again.node}, not {node}")
    return again


def compare(vol: Volume, expected: list[tuple[str, bytes]]) -> list[str]:
    """Read every expected file back through the remounted volume; returns the differences."""
    problems = []
    for rel, data in expected:
        parts = rel.split("/")
        dirfd, shown = vol.root, ""
        try:
            for p in parts[:-1]:
                shown += "/" + p
                dirfd = vol.opendir(dirfd, p, shown)
            back = vol.read(dirfd, parts[-1], "/" + rel)
        except Refusal as e:
            problems.append(f"/{rel}: {e}")
            continue
        if back is None:
            problems.append(f"/{rel} is missing")
        elif back != data:
            first = next((i for i, (x, y) in enumerate(zip(back, data)) if x != y), min(len(back), len(data)))
            problems.append(f"/{rel}: {len(back)} bytes read back, {len(data)} written, first difference at byte "
                            f"{first} (sha256 {sha(back)[:16]} vs {sha(data)[:16]})")
    return problems


def not_verified_help(disk: str | None, verify_only: bool = False) -> str:
    eject = f"diskutil eject {disk}" if disk else "Finder's eject button"
    again = ("run the same command again" if verify_only else "run the same command with --verify-only added (it "
             "compares the card with these files, writing nothing)")
    return (f"What to do: eject the card ({eject}; if the Mac no longer shows it, just take it out), put it "
            f"back, and {again}. Until then do not rely on the card: SurfBoot still checks the package's CRC "
            "before it runs anything.")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="install SurfOS on a mounted FAT32 microSD card (docs/CARD.md)")
    ap.add_argument("--mount", help="the card's mount point, for example /Volumes/SURFOS (required)")
    ap.add_argument("--image", default=str(DEFAULT_IMAGE), help=f"the package (default {DEFAULT_IMAGE})")
    ap.add_argument("--backup-image", help="install this package as /BOOT/BACKUP.BIN")
    ap.add_argument("--no-backup", action="store_true", help="leave /BOOT/BACKUP.BIN as it is")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--cmdline", help="write /BOOT/CMDLINE.TXT with this one line")
    g.add_argument("--cmdline-file", help="copy this file to /BOOT/CMDLINE.TXT")
    ap.add_argument("--system-dir", help="copy the files of this directory into /SYSTEM/")
    ap.add_argument("--new-card", action="store_true", help="first install: write the card marker")
    m = ap.add_mutually_exclusive_group()
    m.add_argument("--dry-run", action="store_true", help="check and show the plan, write nothing")
    m.add_argument("--verify-only", action="store_true",
                   help="unmount and mount the card again and compare it with the given files; write nothing")
    a = ap.parse_args(argv)
    vol = None
    changed = False
    disk = None                                         # the card's whole disk, for the eject advice
    try:
        if not a.mount:
            raise Refusal("give the card's mount point explicitly: make deploy-sd MOUNT=/Volumes/<card> "
                          "(the card's name is in Finder's sidebar)")
        if platform.system() != "Darwin" and not TEST_MODE:
            raise Refusal("this runs on the Mac (it uses diskutil); in the container it is tested with "
                          "SURFCPU_DEPLOY_TEST=1")
        if TEST_MODE:
            test_mode_guard(os.path.realpath(a.mount))
        if not ({os.open, os.mkdir, os.stat, os.rename, os.unlink} <= os.supports_dir_fd
                and os.scandir in os.supports_fd and os.stat in os.supports_follow_symlinks):
            raise Refusal("this Python cannot work relative to an open directory (openat, renameat): use "
                          "Homebrew's python3 or the Mac's /usr/bin/python3")

        # ---- the files to install, all checked before the card is looked at ----
        surfimg = load_surfimg()
        image = Path(a.image)
        if not image.is_file():
            raise Refusal(f"no package at {image}: build it first (make build-kernel), or give --image")
        data = image.read_bytes()
        code, msg = surfimg.verdict(surfimg.check(data))
        if code:
            raise Refusal(f"{image} would not boot: {code}: {msg}")
        h = surfimg.parse_header(data)
        iv = h["image_version"]
        backup = None
        if a.backup_image:
            backup = Path(a.backup_image).read_bytes()
            bcode, bmsg = surfimg.verdict(surfimg.check(backup))
            if bcode:
                raise Refusal(f"{a.backup_image} would not boot: {bcode}: {bmsg}")
        cmdline = None
        if a.cmdline is not None:
            if "\n" in a.cmdline or "\r" in a.cmdline:
                raise Refusal("--cmdline takes one line")
            cmdline = (a.cmdline + "\n").encode()
        elif a.cmdline_file:
            cmdline = Path(a.cmdline_file).read_bytes()
        system_files: list[tuple[str, bytes]] = []
        if a.system_dir:
            for p in sorted(Path(a.system_dir).iterdir()):
                if p.is_file() and not p.name.startswith("."):
                    system_files.append((p.name, p.read_bytes()))

        # ---- the card ----
        vol = Volume(a.mount)
        disk = vol.whole
        print(f"card: {vol.mount} ({vol.device}, {vol.info.get('FilesystemName', 'FAT32')}, "
              f"{vol.info.get('MediaName', 'media')}, {vol.info.get('BusProtocol', '?')})")
        print(f"package: {image} ({len(data)} bytes, {h['name'].rstrip(bytes(1)).decode('ascii', 'replace')} "
              f"{iv >> 16}.{(iv >> 8) & 255}.{iv & 255} build {h['build_id']:08x}, CRC {h['payload_crc32']:#010x}): valid")
        root = vol.root
        kinds = {d: "absent" for d in LAYOUT}
        for d in ("BOOT", "SYSTEM", "PROGRAMS", "USERS"):
            kinds[d] = vol.kind(root, d, "/" + d)
            if kinds[d] == "file":
                raise Refusal(f"/{d} is a file, not a directory")
        users_fd = vol.opendir(root, "USERS", "/USERS") if kinds["USERS"] == "dir" else None
        if users_fd is not None:
            kinds["USERS/DOCUMENTS"] = vol.kind(users_fd, "DOCUMENTS", "/USERS/DOCUMENTS")
            if kinds["USERS/DOCUMENTS"] == "file":
                raise Refusal("/USERS/DOCUMENTS is a file, not a directory")
        boot_fd = vol.opendir(root, "BOOT", "/BOOT") if kinds["BOOT"] == "dir" else None
        sys_fd = vol.opendir(root, "SYSTEM", "/SYSTEM") if kinds["SYSTEM"] == "dir" else None
        old = None
        if boot_fd is not None:
            vol.no_links(boot_fd, "/BOOT")
            for n in ("SURFOS.BIN", "BACKUP.BIN", "CMDLINE.TXT"):
                if vol.kind(boot_fd, n, f"/BOOT/{n}") == "dir":
                    raise Refusal(f"/BOOT/{n} is a directory, not a file")
            old = vol.read(boot_fd, "SURFOS.BIN", "/BOOT/SURFOS.BIN")
        marker = None
        if sys_fd is not None:
            vol.no_links(sys_fd, "/SYSTEM")
            for n, _ in system_files:
                if vol.kind(sys_fd, n, f"/SYSTEM/{n}") == "dir":
                    raise Refusal(f"/SYSTEM/{n} is a directory, not a file")
            marker = vol.read(sys_fd, MARKER_NAME, f"/{MARKER_DIR}/{MARKER_NAME}")
        if marker is not None:
            mm = re.search(rb"id=([0-9a-f]+)", marker)
            print(f"card marker: /{MARKER_DIR}/{MARKER_NAME} (id {mm.group(1).decode() if mm else '?'})")
        elif not a.new_card:
            raise Refusal(f"this FAT32 volume has no /{MARKER_DIR}/{MARKER_NAME}. If it is the card for SurfCPU, run "
                          "again with --new-card (first install); otherwise check the mount point")

        # ---- what will be on the card afterwards ----
        expected: list[tuple[str, bytes]] = [("BOOT/SURFOS.BIN", data)]
        if a.verify_only:
            if backup is not None:
                expected.append(("BOOT/BACKUP.BIN", backup))
            if cmdline is not None:
                expected.append(("BOOT/CMDLINE.TXT", cmdline))
            expected += [(f"SYSTEM/{n}", d) for n, d in system_files]
            print(f"verify only: {len(expected)} file(s) to compare; nothing is written")
            vol = remount(vol)
            problems = compare(vol, expected)
            vol.close()
            vol = None
            if problems:
                print("deploy-sd: the card does NOT hold these files:\n  " + "\n  ".join(problems), file=sys.stderr)
                return EXIT_MISMATCH
            print(f"verified {len(expected)} file(s), {sum(len(d) for _, d in expected)} bytes, read back from the "
                  "card after unmounting and mounting it again")
            return 0

        keep_old = False
        if old is not None and not a.no_backup and backup is None:
            pcode, _ = surfimg.verdict(surfimg.check(old))
            if old == data:
                print("the card already has this SURFOS.BIN; it is written again and BACKUP.BIN is left alone")
            elif pcode:
                print(f"the old SURFOS.BIN is not a valid package ({pcode}): it is not kept as the backup")
            else:
                keep_old = True
                expected.append(("BOOT/BACKUP.BIN", old))
        if backup is not None:
            expected.append(("BOOT/BACKUP.BIN", backup))
        if cmdline is not None:
            expected.append(("BOOT/CMDLINE.TXT", cmdline))
        expected += [(f"SYSTEM/{n}", d) for n, d in system_files]
        new_marker = None
        if marker is None:
            new_marker = (f"SurfOS card for SurfCPU (tools/deploy-sd.py)\nid={secrets.token_hex(8)}\n"
                          f"created={datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}\n").encode()
            expected.append((f"{MARKER_DIR}/{MARKER_NAME}", new_marker))
        need = sum(len(d) for _, d in expected) + (1 << 20)
        st = os.fstatvfs(root)
        free = st.f_bavail * st.f_frsize
        if free < need:
            raise Refusal(f"{vol.mount} has {free} bytes free; the files need about {need}")

        pre = "(dry run) " if a.dry_run else ""
        for d in LAYOUT:
            if kinds[d] == "absent":
                print(f"{pre}create directory /{d}")
        for rel, d in expected:
            if rel == "BOOT/BACKUP.BIN" and keep_old:
                print(f"{pre}keep the old /BOOT/SURFOS.BIN as /BOOT/BACKUP.BIN")
            else:
                print(f"{pre}write /{rel} ({len(d)} bytes, sha256 {sha(d)[:16]}...) via a temporary file")
        if a.dry_run:
            print("dry run: nothing was written")
            return 0

        # ---- writing: from here on a failure means a partly updated card ----
        changed = True
        for d in ("BOOT", "SYSTEM", "PROGRAMS", "USERS"):
            if kinds[d] == "absent":
                os.mkdir(d, 0o755, dir_fd=root)
        if users_fd is None:
            users_fd = vol.opendir(root, "USERS", "/USERS")
        if kinds["USERS/DOCUMENTS"] == "absent":
            os.mkdir("DOCUMENTS", 0o755, dir_fd=users_fd)
        if boot_fd is None:
            boot_fd = vol.opendir(root, "BOOT", "/BOOT")
        if sys_fd is None:
            sys_fd = vol.opendir(root, "SYSTEM", "/SYSTEM")
        dirs = {"BOOT": boot_fd, "SYSTEM": sys_fd}
        tmp = vol.write_temp(boot_fd, data)                 # the package first, under a fresh name
        if keep_old:
            os.rename("SURFOS.BIN", "BACKUP.BIN", src_dir_fd=boot_fd, dst_dir_fd=boot_fd)
        os.rename(tmp, "SURFOS.BIN", src_dir_fd=boot_fd, dst_dir_fd=boot_fd)
        for rel, d in expected:
            if rel == "BOOT/SURFOS.BIN" or (rel == "BOOT/BACKUP.BIN" and keep_old):
                continue
            where, name = rel.split("/")
            t = vol.write_temp(dirs[where], d)
            os.rename(t, name, src_dir_fd=dirs[where], dst_dir_fd=dirs[where])
        for fd in (boot_fd, sys_fd):
            try:
                os.fsync(fd)
            except OSError:
                pass
        print(f"written: {len(expected)} file(s); unmounting and mounting the card again to read them back")

        # ---- verification: from the card itself ----
        vol = remount(vol)
        problems = compare(vol, expected)
        mount = vol.mount
        vol.close()
        vol = None
        if problems:
            print("deploy-sd: the card does NOT hold what was written:\n  " + "\n  ".join(problems) + "\n"
                  "SurfBoot checks the package's CRC and refuses a damaged SURFOS.BIN (then it boots BACKUP.BIN). "
                  "Deploy again; if this repeats, the card or the reader is failing.", file=sys.stderr)
            return EXIT_MISMATCH
        print(f"verified {len(expected)} file(s), {sum(len(d) for _, d in expected)} bytes, read back from the card "
              "after unmounting and mounting it again (so from the card, not from the Mac's memory)")
        print(f"done. Eject the card before you take it out: diskutil eject {shlex.quote(mount)} "
              "(or the eject button next to it in Finder). Do not use it on the Mac and the board at once.")
        return 0
    except Refusal as e:
        if vol is not None:
            vol.close()
        if changed:
            print(f"deploy-sd: written but NOT verified: the update stopped part way: {e}\n"
                  f"{not_verified_help(disk)}", file=sys.stderr)
            return EXIT_NOT_VERIFIED
        print(f"deploy-sd: {e}", file=sys.stderr)
        return EXIT_REFUSED
    except NotVerified as e:
        what = "NOT verified (nothing was written)" if a.verify_only else "written but NOT verified"
        print(f"deploy-sd: {what}: {e}.\n{not_verified_help(disk, a.verify_only)}", file=sys.stderr)
        return EXIT_NOT_VERIFIED
    except OSError as e:
        if vol is not None:
            vol.close()
        if changed:
            print(f"deploy-sd: written but NOT verified: the update stopped part way: {e}\n"
                  f"{not_verified_help(disk)}", file=sys.stderr)
            return EXIT_NOT_VERIFIED
        print(f"deploy-sd: {e}", file=sys.stderr)
        return EXIT_REFUSED


if __name__ == "__main__":
    sys.exit(main())
