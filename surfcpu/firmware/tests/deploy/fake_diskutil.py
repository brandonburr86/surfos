#!/usr/bin/env python3
"""SurfCPU firmware tests: a stand-in for macOS's diskutil
--------------------
File: fake_diskutil.py Date: 10/15/26 (SurfCPU)
--------------------
Answers `diskutil info -plist TARGET` from the JSON file named by $FAKE_DEPLOY_CONFIG, so
tools/deploy-sd.py can be tested in the container without macOS or any disk:

    {"mount": "/work/.../card", "device": "/dev/disk9s1", "volume": {...plist keys...},
     "whole": {...plist keys of /dev/disk9...}}

The plist keys are the ones real diskutil prints (FilesystemType, FilesystemName,
RemovableMedia, Internal, WritableVolume, ParentWholeDisk, Content, ...). Any other use
exits 1, as diskutil does for an unknown disk.
"""
import json
import os
import plistlib
import sys

cfg = json.load(open(os.environ["FAKE_DEPLOY_CONFIG"]))
args = sys.argv[1:]
if len(args) != 3 or args[:2] != ["info", "-plist"]:
    print(f"fake diskutil: unsupported {' '.join(args)}", file=sys.stderr)
    sys.exit(1)
target = args[2]
whole = cfg["device"].rsplit("s", 1)[0]
if target in (cfg["device"], cfg["mount"]):
    info = dict(cfg["volume"])
    info.setdefault("DeviceIdentifier", cfg["device"].rsplit("/", 1)[1])
    info.setdefault("ParentWholeDisk", whole.rsplit("/", 1)[1])
elif target in (whole, whole.rsplit("/", 1)[1]):
    info = dict(cfg["whole"])
else:
    print(f"Could not find disk: {target}", file=sys.stderr)
    sys.exit(1)
sys.stdout.buffer.write(plistlib.dumps(info))
