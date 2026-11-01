#!/usr/bin/env python3
"""SurfCPU firmware tests: a stand-in for macOS's mount listing
--------------------
File: fake_mount.py Date: 10/15/26 (SurfCPU)
--------------------
Prints mount table lines in macOS's format, for tools/deploy-sd.py's tests, from the JSON
file named by $FAKE_DEPLOY_CONFIG ("mount", "device", "fstype", "mounted"); the card's line
is left out while fake_diskutil.py has it unmounted (FAKE_DEPLOY_CONFIG + ".state"):

    /dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)
    /dev/disk9s1 on /work/.../card (msdos, local, nodev, nosuid, noowners)
"""
import json
import os
from pathlib import Path

cfg_path = os.environ["FAKE_DEPLOY_CONFIG"]
cfg = json.load(open(cfg_path))
state = Path(cfg_path + ".state")
print("/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)")
print("devfs on /dev (devfs, local, nobrowse)")
if cfg.get("mounted", True) and (not state.exists() or state.read_text().strip() == "mounted"):
    print(f"{cfg['device']} on {cfg['mount']} ({cfg.get('fstype', 'msdos')}, local, nodev, nosuid, noowners)")
