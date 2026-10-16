#!/usr/bin/env python3
"""SurfCPU firmware tests: a stand-in for macOS's mount listing
--------------------
File: fake_mount.py Date: 10/15/26 (SurfCPU)
--------------------
Prints mount table lines in macOS's format, for tools/deploy-sd.py's tests, from the JSON
file named by $FAKE_DEPLOY_CONFIG ("mount", "device", "fstype"):

    /dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)
    /dev/disk9s1 on /work/.../card (msdos, local, nodev, nosuid, noowners)
"""
import json
import os

cfg = json.load(open(os.environ["FAKE_DEPLOY_CONFIG"]))
print("/dev/disk3s1s1 on / (apfs, sealed, local, read-only, journaled)")
print("devfs on /dev (devfs, local, nobrowse)")
if cfg.get("mounted", True):
    print(f"{cfg['device']} on {cfg['mount']} ({cfg.get('fstype', 'msdos')}, local, nodev, nosuid, noowners)")
