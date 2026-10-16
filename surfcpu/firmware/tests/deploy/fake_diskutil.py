#!/usr/bin/env python3
"""SurfCPU firmware tests: a stand-in for macOS's diskutil, with a cache model
--------------------
File: fake_diskutil.py Date: 10/15/26 (SurfCPU)
--------------------
For tools/deploy-sd.py's tests in the container, without macOS or any disk, configured by
the JSON file named by $FAKE_DEPLOY_CONFIG:

    {"mount": ".../card", "device": "/dev/disk9s1", "volume": {...plist keys...},
     "whole": {...plist keys of /dev/disk9...}, "device_dir": ".../device",
     "corrupt_on_flush": ["BOOT/SURFOS.BIN"], "unmount_fails": false, "mount_fails": false,
     "devnum_offset": 0, "devnum_offset_after_mount": 1, "drop": ["ParentWholeDisk"]}

`diskutil info -plist TARGET` answers with the configured keys plus DeviceNode, MountPoint
and TestDeviceNumber (the stand-in directory's st_dev, plus devnum_offset to fake a
mismatch, or devnum_offset_after_mount once `diskutil mount` has run: a different device
after the remount). The cache model: the stand-in directory is the Mac's view of the mounted card (its
cache), device_dir is the card itself. `diskutil unmount` writes back what changed in the view since it was mounted
(dirty data only, as a real unmount does), flipping one bit in the middle of each file named
by corrupt_on_flush (a write that went wrong on the way to the card), and empties the view;
`diskutil mount` fills the view from the card again. So a check that reads back without unmounting sees only the view and misses the
damage, and a check after a remount sees what the card holds. The mounted state is kept in
FAKE_DEPLOY_CONFIG + ".state". Anything else exits 1, as diskutil does for an unknown disk.
"""
import hashlib
import json
import os
import plistlib
import shutil
import sys
from pathlib import Path

cfg_path = os.environ["FAKE_DEPLOY_CONFIG"]
cfg = json.load(open(cfg_path))
state = Path(cfg_path + ".state")
mount = Path(cfg["mount"])
device_dir = Path(cfg.get("device_dir") or cfg_path + ".device")
device = cfg["device"]
whole = device.rsplit("s", 1)[0]


manifest = Path(cfg_path + ".clean")                       # what the view held when it was mounted
remounted = Path(cfg_path + ".remounted")                  # `diskutil mount` has run


def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def mounted() -> bool:
    return not state.exists() or state.read_text().strip() == "mounted"


def fail(msg: str):
    print(msg, file=sys.stderr)
    sys.exit(1)


def clear(d: Path):
    for p in d.iterdir():
        if p.is_dir() and not p.is_symlink():
            shutil.rmtree(p)
        else:
            p.unlink()


args = sys.argv[1:]
if len(args) == 3 and args[:2] == ["info", "-plist"]:
    target = args[2]
    if target in (device, str(mount), device.rsplit("/", 1)[1]):
        info = dict(cfg["volume"])
        info.setdefault("DeviceIdentifier", device.rsplit("/", 1)[1])
        info.setdefault("ParentWholeDisk", whole.rsplit("/", 1)[1])
        info.setdefault("DeviceNode", device)
        info["MountPoint"] = str(mount) if mounted() else ""
        if mount.exists():
            offset = cfg.get("devnum_offset", 0)
            if remounted.exists():
                offset = cfg.get("devnum_offset_after_mount", offset)
            info.setdefault("TestDeviceNumber", os.stat(mount).st_dev + int(offset))
        for key in cfg.get("drop", []):                     # answers that lack a key
            info.pop(key, None)
    elif target in (whole, whole.rsplit("/", 1)[1]):
        info = dict(cfg["whole"])
    else:
        fail(f"Could not find disk: {target}")
    sys.stdout.buffer.write(plistlib.dumps(info))
elif len(args) == 2 and args[0] == "unmount":
    if args[1] not in (str(mount), device):
        fail(f"Could not find disk: {args[1]}")
    if not mounted():
        fail(f"Volume on {device} is not mounted")
    if cfg.get("unmount_fails"):
        fail(f"Unmount of {device.rsplit('/', 1)[1]} failed: at least one volume could not be unmounted")
    # Write back only what changed in the view since it was mounted (dirty data), as a real
    # unmount does: clean cached pages are never written, so the card keeps what it holds.
    clean = json.loads(manifest.read_text()) if manifest.exists() else {}
    device_dir.mkdir(parents=True, exist_ok=True)
    seen = set()
    for p in sorted(mount.rglob("*")):
        rel = str(p.relative_to(mount))
        seen.add(rel)
        dst = device_dir / rel
        if p.is_symlink():
            if dst.is_symlink() or dst.exists():
                dst.unlink() if not dst.is_dir() or dst.is_symlink() else shutil.rmtree(dst)
            dst.symlink_to(os.readlink(p))
        elif p.is_dir():
            dst.mkdir(parents=True, exist_ok=True)
        elif clean.get(rel) != digest(p):
            data = bytearray(p.read_bytes())
            if rel in cfg.get("corrupt_on_flush", []) and data:
                data[len(data) // 2] ^= 0x01                # one write goes wrong on the way to the card
            dst.write_bytes(bytes(data))
    for p in sorted(device_dir.rglob("*"), reverse=True):  # what the view removed or renamed away
        if str(p.relative_to(device_dir)) not in seen:
            p.unlink() if not p.is_dir() or p.is_symlink() else shutil.rmtree(p)
    clear(mount)                                            # the Mac forgets what it cached
    state.write_text("unmounted\n")
    print(f"Volume SURFOS on {device.rsplit('/', 1)[1]} unmounted")
elif len(args) == 2 and args[0] == "mount":
    if args[1] not in (device, device.rsplit("/", 1)[1]):
        fail(f"Could not find disk: {args[1]}")
    if cfg.get("mount_fails"):
        fail(f"Volume on {device.rsplit('/', 1)[1]} failed to mount")
    if not mounted():
        mount.mkdir(parents=True, exist_ok=True)
        clear(mount)
        if device_dir.exists():
            for p in device_dir.iterdir():
                if p.is_dir() and not p.is_symlink():
                    shutil.copytree(p, mount / p.name, symlinks=True)
                else:
                    shutil.copy2(p, mount / p.name, follow_symlinks=False)
        state.write_text("mounted\n")
        remounted.touch()
        manifest.write_text(json.dumps({str(p.relative_to(mount)): digest(p) for p in mount.rglob("*")
                                        if p.is_file() and not p.is_symlink()}))
    print(f"Volume SURFOS on {device.rsplit('/', 1)[1]} mounted")
else:
    fail(f"fake diskutil: unsupported {' '.join(args)}")
