#!/usr/bin/env python3
"""SurfCPU test ROM images
--------------------
File: mkrom.py Date: 10/15/26 (SurfCPU)
--------------------
Writes a boot ROM image in the format of docs/MODULES.md section 5 ($readmemh: one 32-bit word
per line, 8 hex digits, no address markers, exactly 16384 lines for the 64 KiB ROM) filled
with a seeded pseudo-random pattern, so tests can tell every word apart. The SoC tests and
the bus-script compiler read the same file to know what the ROM holds.

    python3 sim/soc/tools/mkrom.py OUT.mem --seed N [--zero]
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]          # surfcpu/
sys.path.insert(0, str(ROOT / "tools"))
from socspec import load                             # noqa: E402  (the memory map)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--zero", action="store_true", help="all zero (an empty ROM)")
    args = ap.parse_args()

    words = load().regions["bootrom"].size // 4
    rng = random.Random(args.seed)
    lines = ["%08x" % (0 if args.zero else rng.getrandbits(32)) for _ in range(words)]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
