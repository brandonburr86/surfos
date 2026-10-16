#!/usr/bin/env python3
"""
SurfCPU block RAM contents check
--------------------
File: bramcheck.py Date: 10/15/26 (SurfCPU)
--------------------
LVS checks block RAMs only at their pins, so this checks what they hold: the initial contents
(INIT_00.. and INITP_00.. of every RAMB18E1/RAMB36E1) that fasm2netlist reads back out of the
finished bitstream (bit2fasm's FASM -> fasm2netlist, a decoder independent of nextpnr) must
be exactly those of the synthesized netlist, block for block, as multisets: the placement
may put any block in any site, but no contents may go missing, change or appear. This is
what SurfBoot's ROM relies on. Only the standard library is used.

    bramcheck.py --netlist top.json --top top --extracted fasm2netlist.v
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter

WORDS = {"RAMB18E1": (64, 8), "RAMB36E1": (128, 16)}     # INIT_xx and INITP_xx per block


def netlist_contents(path: str, top: str) -> dict:
    with open(path) as f:
        design = json.load(f)
    found = {t: Counter() for t in WORDS}
    for cell in design["modules"][top].get("cells", {}).values():
        if cell["type"] not in WORDS:
            continue
        p = cell.get("parameters", {})
        n_init, n_initp = WORDS[cell["type"]]

        def word(name: str) -> int:
            v = str(p.get(name, "0")).replace("x", "0").replace("z", "0")
            return int(v, 2) if v else 0
        found[cell["type"]][tuple([word(f"INIT_{i:02X}") for i in range(n_init)]
                                  + [word(f"INITP_{i:02X}") for i in range(n_initp)])] += 1
    return found


def extracted_contents(path: str) -> dict:
    with open(path) as f:
        text = f.read()
    found = {t: Counter() for t in WORDS}
    for m in re.finditer(r"\b(RAMB18E1|RAMB36E1)\s*#\((.*?)\)\s*[\\\w$.\[\]]+\s*\(", text, re.S):
        kind, params = m.group(1), m.group(2)
        vals = {k.upper(): int(v, 16) for k, v in re.findall(r"\.(INITP?_[0-9A-Fa-f]{2})\(\d+'h([0-9a-fA-F]+)\)", params)}
        n_init, n_initp = WORDS[kind]
        found[kind][tuple([vals.get(f"INIT_{i:02X}", 0) for i in range(n_init)]
                          + [vals.get(f"INITP_{i:02X}", 0) for i in range(n_initp)])] += 1
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description="block RAM contents check (see the file header)")
    ap.add_argument("--netlist", required=True)
    ap.add_argument("--top", required=True)
    ap.add_argument("--extracted", required=True)
    args = ap.parse_args()
    want = netlist_contents(args.netlist, args.top)
    got = extracted_contents(args.extracted)
    bad = 0
    for kind in WORDS:
        nw, ng = sum(want[kind].values()), sum(got[kind].values())
        if not nw and not ng:
            continue
        missing = want[kind] - got[kind]
        extra = got[kind] - want[kind]
        print(f"{kind}: {nw} in the netlist, {ng} decoded from the bitstream, "
              f"{sum((want[kind] & got[kind]).values())} with identical contents")
        for contents, n in missing.items():
            bad += n
            nz = sum(1 for w in contents if w)
            print(f"  MISSING {n} block(s) with {nz} non-zero words (first word {contents[0]:064x})")
        for contents, n in extra.items():
            bad += n
            print(f"  UNEXPECTED {n} block(s) decoded (first word {contents[0]:064x})")
    if not any(sum(want[k].values()) or sum(got[k].values()) for k in WORDS):
        print("bramcheck: no block RAM in the design")
        return 0
    print("bramcheck: " + ("PASS (every block's initial contents are in the bitstream)" if not bad
                           else f"FAIL ({bad} blocks differ)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
