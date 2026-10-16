#!/usr/bin/env python3
"""
SurfCPU verification: pinned source archives
--------------------
File: rvfetch.py Date: 10/15/26 (SurfCPU)
--------------------
The pattern of the cpu track's sim/cpu/riscv_tests.py, shared by tools/rvarch.py and
tools/rvformal.py: a GitHub archive tarball of a pinned tag or commit is downloaded into
build/third_party/<name>/, its SHA-256 checked against the pinned value (a mismatch stops
everything), and unpacked under build/third_party/<name>/src/ with tarfile's "data" filter
(no absolute paths, no links leaving the tree).
Only the Python standard library is used.
"""

from __future__ import annotations

import hashlib
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent            # surfcpu/
THIRD = ROOT / "build" / "third_party"


def fetch(url: str, sha: str, dest: Path, who: str) -> Path:
    """Download url to dest unless dest already has the pinned SHA-256."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha:
        return dest
    print(f"{who}: downloading {url}", flush=True)
    data = urllib.request.urlopen(url, timeout=300).read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha:
        raise SystemExit(f"{who}: {url}: SHA-256 {got}, expected {sha}; not using it")
    dest.write_bytes(data)
    return dest


def unpack(archive: Path, into: Path, keep: tuple[str, ...] = ()) -> Path:
    """Unpack archive under into (once) and return its top directory; with keep, only the
    members whose path below the top directory starts with one of those prefixes."""
    into.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as t:
        top = t.getnames()[0].split("/")[0]
        dest = into / top
        stamp = dest / ".surfcpu-unpacked"
        want = hashlib.sha256(archive.read_bytes()).hexdigest() + " " + " ".join(keep)
        if stamp.exists() and stamp.read_text().strip() == want.strip():
            return dest
        members = t.getmembers()
        if keep:
            prefixes = tuple(f"{top}/{k}" for k in keep)
            members = [m for m in members if m.name.startswith(prefixes)]
        t.extractall(into, members=members, filter="data")
    stamp.write_text(want + "\n")
    return dest
