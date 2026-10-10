#!/usr/bin/env python3
"""Fetch the Wi-Fi firmware SurfOS loads into the laptop's Intel Wireless-AC 9560.

    tools/fetch-firmware.py            download what is missing into firmware/, check every file
    tools/fetch-firmware.py --check    only check what is there (exit 1 when a file is missing or wrong)

The card runs Intel's firmware, which the driver (driver/wifi/) loads into it at `wifi up`. The
files come from linux-firmware at a pinned tag, are checked against the SHA-256 values below, and
are kept in firmware/ (ignored by git: never committed). The initrd carries them under
/lib/firmware/ with Intel's licence (docs/superpowers/plans/2026-10-09-wifi.md).

Intel's licence (LICENCE.iwlwifi_firmware) allows redistributing the files in binary form,
unchanged, with that notice, and forbids reverse engineering, decompiling or disassembling them:
SurfOS only reads the files' container (the TLV layout the open-source drivers document) to hand
the sections to the card.

A download that does not match is deleted and reported; nothing is replaced by a file that does
not match. Without a network the files already in firmware/ are checked and used.
"""
import argparse
import hashlib
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, 'firmware')

TAG = '20260916'                        # linux-firmware, commit ab23307cfe7f9366c819025ca3e4778299bc2db2
BASE = 'https://gitlab.com/kernel-firmware/linux-firmware/-/raw/' + TAG + '/'

# (path in linux-firmware, name in firmware/ and /lib/firmware/, SHA-256)
FILES = [
    ('intel/iwlwifi/iwlwifi-Qu-b0-jf-b0-77.ucode', 'iwlwifi-Qu-b0-jf-b0-77.ucode',
     '4f19e5786f3acfb8f923bb50bce78a1b6c618906a81286bf806e1adf0d6376c1'),
    ('intel/iwlwifi/iwlwifi-Qu-c0-jf-b0-77.ucode', 'iwlwifi-Qu-c0-jf-b0-77.ucode',
     'a4d173d7334226c035b744dec3e53a2c52a82e05b0ed45188d5af8c66b961bc9'),
    ('LICENSES/LICENCE.iwlwifi_firmware', 'LICENCE.iwlwifi_firmware',
     '16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5'),
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def fetch(src, path):
    tmp = path + '.part'
    try:
        with urllib.request.urlopen(BASE + src, timeout=120) as r, open(tmp, 'wb') as f:
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                f.write(chunk)
        return tmp
    except OSError as e:
        if os.path.exists(tmp):
            os.unlink(tmp)
        print(f'fetch-firmware: {src}: {e}', file=sys.stderr)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true', help='only check the files already in firmware/')
    a = ap.parse_args()
    os.makedirs(DEST, exist_ok=True)
    bad = 0
    for src, name, want in FILES:
        path = os.path.join(DEST, name)
        if os.path.exists(path) and sha256(path) == want:
            print(f'fetch-firmware: {name}: ok')
            continue
        if a.check:
            print(f'fetch-firmware: {name}: {"wrong SHA-256" if os.path.exists(path) else "missing"}')
            bad += 1
            continue
        tmp = fetch(src, path)
        if not tmp:
            bad += 1
            continue
        got = sha256(tmp)
        if got != want:
            os.unlink(tmp)
            print(f'fetch-firmware: {name}: SHA-256 {got}, expected {want}: not used', file=sys.stderr)
            bad += 1
            continue
        os.replace(tmp, path)
        print(f'fetch-firmware: {name}: downloaded ({os.path.getsize(path)} bytes), SHA-256 checked')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
