#!/usr/bin/env python3
"""SurfCPU card installer for the Mac
--------------------
File: deploy-sd.py Date: 10/15/26 (SurfCPU)
--------------------
Installs SurfOS on the microSD card, mounted on the MacBook (docs/CARD.md):

    make deploy-sd MOUNT=/Volumes/SURFOS
    tools/deploy-sd.py --mount /Volumes/SURFOS [--image PATH] [--backup-image PATH] [--no-backup]
                       [--cmdline TEXT | --cmdline-file FILE] [--system-dir DIR] [--new-card] [--dry-run]

Checks first, writes nothing until all pass:
  1. the mount point is named explicitly and `mount` lists it as an msdos volume;
  2. `diskutil info -plist` says FAT32, writable, removable or ejectable media (a built-in SD
     slot counts; a fixed internal disk does not), not the startup disk, and the whole disk
     has an MBR partition table (SurfBoot reads MBR only);
  3. the package passes SurfBoot's own checks (tools/surfimg.py): the x86 surfos.bin, an ELF
     or a disk image is refused;
  4. the card carries /SYSTEM/SURFCARD.TXT, the SurfOS card marker (the first install says
     --new-card and writes it), so a different USB stick or card is not written by mistake;
  5. there is room (os.statvfs).
Then: /BOOT /SYSTEM /PROGRAMS /USERS /USERS/DOCUMENTS are created if missing; the package is
written as /BOOT/SURFOS.NEW, flushed to the card (F_FULLFSYNC), read back past the cache
(F_NOCACHE) and compared byte for byte; the previous SURFOS.BIN, if it is a valid package and
different, becomes /BOOT/BACKUP.BIN (unless --no-backup or --backup-image); SURFOS.NEW is
renamed to SURFOS.BIN; CMDLINE.TXT and /SYSTEM files the same way; every installed file is
read back again; finally it says to eject the card. It never formats or repartitions,
never touches files other than those it installs, and keeps the user directories. FAT32
has no journal: pulling the card out during the copy can damage the file system, and the
backup file does not make an update atomic.

Only Python's standard library and macOS's own diskutil and mount are used. For tests,
SURFCPU_DEPLOY_TEST=1 lets it run off macOS against a directory stand-in, with
SURFCPU_DISKUTIL and SURFCPU_MOUNT naming fake commands; in that mode paths under /Volumes are
refused, so a test can never reach a real card.
"""
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
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent                              # the repository root (surfcpu/..)
DEFAULT_IMAGE = REPO / "build" / "riscv32" / "arty_a7" / "SURFOS.BIN"
LAYOUT = ("BOOT", "SYSTEM", "PROGRAMS", "USERS", "USERS/DOCUMENTS")
MARKER = "SYSTEM/SURFCARD.TXT"
TEST_MODE = os.environ.get("SURFCPU_DEPLOY_TEST") == "1"
F_NOCACHE, F_FULLFSYNC = 48, 51                        # macOS fcntl commands


class Refusal(Exception):
    pass


def load_surfimg():
    spec = importlib.util.spec_from_file_location("surfimg", HERE / "surfimg.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def command(env_name: str, default: str) -> list[str]:
    value = os.environ.get(env_name) if TEST_MODE else None
    return shlex.split(value) if value else [default]


def run(cmd: list[str]) -> bytes:
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise Refusal(f"could not run {' '.join(cmd)}: {e}")
    if p.returncode != 0:
        raise Refusal(f"{' '.join(cmd)} failed: {p.stderr.decode(errors='replace').strip()}")
    return p.stdout


def mount_entry(mount: str) -> tuple[str, str]:
    """(device, file system type) of the mount point, from `mount`."""
    out = run(command("SURFCPU_MOUNT", "mount")).decode(errors="replace")
    for line in out.splitlines():
        m = re.match(r"^(\S+) on (.+) \(([^,)]+)", line)
        if m and os.path.realpath(m.group(2)) == mount:
            return m.group(1), m.group(3)
    raise Refusal(f"{mount} is not a mount point (mount does not list it); give the card's mount point, "
                  "for example /Volumes/SURFOS")


def diskutil(target: str) -> dict:
    data = run(command("SURFCPU_DISKUTIL", "diskutil") + ["info", "-plist", target])
    try:
        return plistlib.loads(data)
    except Exception as e:
        raise Refusal(f"diskutil info {target}: unreadable answer ({e})")


def check_volume(mount: str) -> dict:
    device, fstype = mount_entry(mount)
    if fstype != "msdos":
        raise Refusal(f"{mount} is a {fstype} volume, not FAT (msdos); SurfBoot reads FAT32 only")
    info = diskutil(device)
    name = f"{info.get('FilesystemName', '')} {info.get('FilesystemUserVisibleName', '')}"
    if info.get("FilesystemType") != "msdos" or "FAT32" not in name:
        raise Refusal(f"{mount} is {name.strip() or info.get('FilesystemType')}: SurfBoot needs FAT32 "
                      "(exFAT and FAT16 are refused; docs/CARD.md shows how to format the card)")
    removable = bool(info.get("RemovableMedia") or info.get("Removable") or info.get("Ejectable")
                     or info.get("RemovableMediaOrExternalDevice"))
    if not removable:
        raise Refusal(f"{mount} ({device}) is on a fixed {'internal ' if info.get('Internal') else ''}disk, "
                      "not removable media; refusing")
    if not (info.get("WritableVolume", info.get("Writable", True))):
        raise Refusal(f"{mount} is read-only (is the card's lock switch on?)")
    if info.get("SystemImage") or mount in ("/", "/System/Volumes/Data"):
        raise Refusal(f"{mount} is a system volume; refusing")
    whole = info.get("ParentWholeDisk")
    if whole:
        winfo = diskutil(whole)
        scheme = winfo.get("Content", "")
        if scheme != "FDisk_partition_scheme":
            raise Refusal(f"the card ({whole}) has a {scheme or 'unknown'} partition table; SurfBoot reads MBR "
                          "(FDisk) partition tables only: docs/CARD.md")
    total = info.get("TotalSize") or info.get("Size") or 0
    if total and total > 2 << 40:
        raise Refusal(f"{mount} is {total} bytes: larger than any microSD card this is meant for")
    return {"device": device, "whole": whole, "info": info}


def nocache_read(path: Path) -> bytes:
    fd = os.open(path, os.O_RDONLY)
    try:
        if platform.system() == "Darwin":
            import fcntl
            fcntl.fcntl(fd, getattr(fcntl, "F_NOCACHE", F_NOCACHE), 1)
        chunks = []
        while True:
            b = os.read(fd, 1 << 20)
            if not b:
                break
            chunks.append(b)
        return b"".join(chunks)
    finally:
        os.close(fd)


def flushed_write(path: Path, data: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        view = memoryview(data)
        while view:
            n = os.write(fd, view)
            view = view[n:]
        os.fsync(fd)
        if platform.system() == "Darwin":
            import fcntl
            fcntl.fcntl(fd, getattr(fcntl, "F_FULLFSYNC", F_FULLFSYNC))
    finally:
        os.close(fd)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class Installer:
    def __init__(self, root: Path, dry: bool):
        self.root, self.dry = root, dry
        self.installed: list[tuple[Path, str, int]] = []
        self.tmp_n = 0

    def say(self, text: str):
        print(("(dry run) " if self.dry else "") + text)

    def mkdirs(self):
        for d in LAYOUT:
            p = self.root / d
            if not p.is_dir():
                self.say(f"create directory /{d}")
                if not self.dry:
                    p.mkdir(parents=True, exist_ok=True)

    def install(self, rel: str, data: bytes, tmp_name: str | None = None):
        """Write data to rel through a temporary name, verified before the rename."""
        dst = self.root / rel
        self.tmp_n += 1
        tmp = dst.parent / (tmp_name or f"~DEPLOY.{self.tmp_n:03d}")
        self.say(f"write /{rel} ({len(data)} bytes, sha256 {sha(data)[:16]}...) via /{tmp.relative_to(self.root)}")
        if self.dry:
            return
        flushed_write(tmp, data)
        if os.environ.get("SURFCPU_DEPLOY_TEST_CORRUPT") == rel and TEST_MODE:
            with open(tmp, "r+b") as f:                 # tests: a copy that went wrong
                f.seek(len(data) // 2)
                f.write(bytes([data[len(data) // 2] ^ 0xFF]))
        back = nocache_read(tmp)
        if back != data:
            tmp.unlink()
            raise Refusal(f"/{rel}: the bytes read back differ from the bytes written (sha256 {sha(back)[:16]} "
                          f"vs {sha(data)[:16]}); nothing was replaced. Is the card failing?")
        os.replace(tmp, dst)
        self.installed.append((dst, sha(data), len(data)))

    def rename(self, src_rel: str, dst_rel: str):
        self.say(f"keep the old /{src_rel} as /{dst_rel}")
        if not self.dry:
            os.replace(self.root / src_rel, self.root / dst_rel)

    def verify_all(self):
        for path, digest, n in self.installed:
            back = nocache_read(path)
            if sha(back) != digest or len(back) != n:
                raise Refusal(f"{path}: changed after installing (read back sha256 {sha(back)[:16]})")
        if self.installed:
            print(f"verified {len(self.installed)} file(s), {sum(n for _, _, n in self.installed)} bytes, read back "
                  "from the card")


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
    ap.add_argument("--dry-run", action="store_true", help="check and show the plan, write nothing")
    a = ap.parse_args(argv)
    try:
        if not a.mount:
            raise Refusal("give the card's mount point explicitly: make deploy-sd MOUNT=/Volumes/<card> "
                          "(the card's name is in Finder's sidebar)")
        if platform.system() != "Darwin" and not TEST_MODE:
            raise Refusal("this runs on the Mac (it uses diskutil); in the container it is tested with "
                          "SURFCPU_DEPLOY_TEST=1")
        mount = os.path.realpath(a.mount)
        if TEST_MODE and (mount == "/Volumes" or mount.startswith("/Volumes/")):
            raise Refusal("test mode never touches /Volumes")
        if not os.path.isdir(mount):
            raise Refusal(f"{a.mount} is not a directory: is the card mounted?")
        root = Path(mount)
        vol = check_volume(mount)
        print(f"card: {mount} ({vol['device']}, {vol['info'].get('FilesystemName', 'FAT32')}, "
              f"{vol['info'].get('MediaName', 'media')}, {vol['info'].get('BusProtocol', '?')})")

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
        print(f"package: {image} ({len(data)} bytes, {h['name'].rstrip(bytes(1)).decode('ascii', 'replace')} "
              f"{iv >> 16}.{(iv >> 8) & 255}.{iv & 255} build {h['build_id']:08x}, CRC {h['payload_crc32']:#010x}): valid")
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
        system_files = []
        if a.system_dir:
            for p in sorted(Path(a.system_dir).iterdir()):
                if p.is_file() and not p.name.startswith("."):
                    system_files.append(p)

        marker = root / MARKER
        if marker.is_file():
            ident = marker.read_text(errors="replace")
            m = re.search(r"id=([0-9a-f]+)", ident)
            print(f"card marker: /{MARKER} (id {m.group(1) if m else '?'})")
        elif not a.new_card:
            raise Refusal(f"this FAT32 volume has no /{MARKER}. If it is the card for SurfCPU, run again with "
                          "--new-card (first install); otherwise check the mount point")
        need = len(data) + (len(backup) if backup else 0) + sum(p.stat().st_size for p in system_files) + (1 << 20)
        st = os.statvfs(mount)
        free = st.f_bavail * st.f_frsize
        if free < need:
            raise Refusal(f"{mount} has {free} bytes free; the files need about {need}")

        ins = Installer(root, a.dry_run)
        ins.mkdirs()
        old = root / "BOOT" / "SURFOS.BIN"
        keep_old = False
        if old.is_file() and not a.no_backup and backup is None:
            prev = old.read_bytes()
            pcode, _ = surfimg.verdict(surfimg.check(prev))
            if prev == data:
                print("the card already has this SURFOS.BIN; it is written again and BACKUP.BIN is left alone")
            elif pcode:
                print(f"the old SURFOS.BIN is not a valid package ({pcode}): it is not kept as the backup")
            else:
                keep_old = True
        if not keep_old:
            ins.install("BOOT/SURFOS.BIN", data, tmp_name="SURFOS.NEW")
        else:
            # the new package is verified under a temporary name before the old one moves
            ins.say(f"write /BOOT/SURFOS.BIN ({len(data)} bytes) via /BOOT/SURFOS.NEW")
            if not ins.dry:
                tmp = root / "BOOT" / "SURFOS.NEW"
                flushed_write(tmp, data)
                if os.environ.get("SURFCPU_DEPLOY_TEST_CORRUPT") == "BOOT/SURFOS.BIN" and TEST_MODE:
                    with open(tmp, "r+b") as f:
                        f.seek(len(data) // 2)
                        f.write(bytes([data[len(data) // 2] ^ 0xFF]))
                if nocache_read(tmp) != data:
                    tmp.unlink()
                    raise Refusal("/BOOT/SURFOS.NEW: the bytes read back differ from the bytes written; the old "
                                  "SURFOS.BIN is untouched. Is the card failing?")
            ins.rename("BOOT/SURFOS.BIN", "BOOT/BACKUP.BIN")
            ins.say("rename /BOOT/SURFOS.NEW to /BOOT/SURFOS.BIN")
            if not ins.dry:
                os.replace(root / "BOOT" / "SURFOS.NEW", old)
                ins.installed.append((old, sha(data), len(data)))
                ins.installed.append((root / "BOOT" / "BACKUP.BIN", sha(prev), len(prev)))
        if backup is not None:
            ins.install("BOOT/BACKUP.BIN", backup)
        if cmdline is not None:
            ins.install("BOOT/CMDLINE.TXT", cmdline)
        for p in system_files:
            ins.install(f"SYSTEM/{p.name}", p.read_bytes())
        if not marker.is_file():
            ins.install(MARKER, (f"SurfOS card for SurfCPU (tools/deploy-sd.py)\nid={secrets.token_hex(8)}\n"
                                 f"created={datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}\n").encode())
        if a.dry_run:
            print("dry run: nothing was written")
            return 0
        os.sync()
        ins.verify_all()
        print(f"done. Eject the card before you take it out: diskutil eject {shlex.quote(mount)} "
              "(or the eject button next to it in Finder). Do not use it on the Mac and the board at once.")
        return 0
    except Refusal as e:
        print(f"deploy-sd: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
