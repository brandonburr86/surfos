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

fasm2netlist decodes every block RAM site as RAMB18E1 halves: a RAMB36E1 comes back as the two
RAMB18 of its tile, its INIT and INITP bits interleaved between them (bit 2k in the half with
the lower Y, bit 2k+1 in the upper one, as nextpnr writes the FASM and as the SoC's 36 blocks
showed, 36 of 36 matching). So the two halves of each tile are woven back into one 36 Kbit
block and compared with the netlist's RAMB36E1; the halves of tiles that match none are
compared with its RAMB18E1 (two independent RAMB18 in one tile).

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


def weave(lo: int, hi: int, bits: int) -> int:
    """Interleave two halves: lo's bit k becomes bit 2k, hi's bit k bit 2k+1."""
    out = 0
    for k in range(bits):
        out |= ((lo >> k) & 1) << (2 * k) | ((hi >> k) & 1) << (2 * k + 1)
    return out


def ramb36_from_halves(lo: tuple, hi: tuple) -> tuple:
    """The RAMB36E1 contents (128 INIT + 16 INITP words) that two RAMB18 halves hold."""
    def whole(words: tuple, n: int) -> int:
        return sum(w << (256 * i) for i, w in enumerate(words[:n]))
    data = weave(whole(lo, 64), whole(hi, 64), 64 * 256)
    par = weave(whole(lo[64:], 8), whole(hi[64:], 8), 8 * 256)
    mask = (1 << 256) - 1
    return tuple([(data >> (256 * i)) & mask for i in range(128)] + [(par >> (256 * i)) & mask for i in range(16)])


def extracted_contents(path: str) -> tuple[dict, dict]:
    """(contents by kind as decoded, RAMB18 halves by tile: {tile: {y: contents}})."""
    with open(path) as f:
        text = f.read()
    found = {t: Counter() for t in WORDS}
    tiles: dict = {}
    for m in re.finditer(r"\b(RAMB18E1|RAMB36E1)\s*#\((.*?)\)\s*([\\\w$.\[\]]+)\s*\(", text, re.S):
        kind, params, inst = m.group(1), m.group(2), m.group(3)
        vals = {k.upper(): int(v, 16) for k, v in re.findall(r"\.(INITP?_[0-9A-Fa-f]{2})\(\d+'h([0-9a-fA-F]+)\)", params)}
        n_init, n_initp = WORDS[kind]
        contents = tuple([vals.get(f"INIT_{i:02X}", 0) for i in range(n_init)]
                         + [vals.get(f"INITP_{i:02X}", 0) for i in range(n_initp)])
        found[kind][contents] += 1
        site = re.fullmatch(r"\\?bram18_(\w+)_RAMB18_X\d+Y(\d+)", inst)
        if kind == "RAMB18E1" and site:
            tiles.setdefault(site.group(1), {})[int(site.group(2))] = contents
    return found, tiles


def main() -> int:
    ap = argparse.ArgumentParser(description="block RAM contents check (see the file header)")
    ap.add_argument("--netlist", required=True)
    ap.add_argument("--top", required=True)
    ap.add_argument("--extracted", required=True)
    args = ap.parse_args()
    want = netlist_contents(args.netlist, args.top)
    got, tiles = extracted_contents(args.extracted)
    # Each tile's two halves woven into a RAMB36E1 the netlist has: those count as one
    # RAMB36E1 decoded; the halves of other tiles stay RAMB18E1.
    need36 = Counter(want["RAMB36E1"])
    for tile, halves in sorted(tiles.items()):
        if len(halves) != 2:
            continue
        lo_y, hi_y = sorted(halves)
        whole = ramb36_from_halves(halves[lo_y], halves[hi_y])
        if need36[whole] > 0:
            need36[whole] -= 1
            got["RAMB36E1"][whole] += 1
            for half in (halves[lo_y], halves[hi_y]):
                got["RAMB18E1"][half] -= 1
                if got["RAMB18E1"][half] == 0:
                    del got["RAMB18E1"][half]
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
