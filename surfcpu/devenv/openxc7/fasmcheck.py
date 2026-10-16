#!/usr/bin/env python3
"""
SurfCPU FASM round-trip check
--------------------
File: fasmcheck.py Date: 10/15/26 (SurfCPU)
--------------------
Compares the FASM that nextpnr wrote (what the bitstream was assembled from) with the FASM
that Project X-Ray's bit2fasm decodes back out of the finished bitstream, feature bit by
feature bit, and accounts for every difference with the database:

  * a feature only in nextpnr's FASM must be a pseudo-pip (ppips_*.db: ALWAYS, DEFAULT or
    HINT), a connection with no configuration bits, which no bitstream decoder can see;
  * a feature only in the decoded FASM must be one whose bits are all "clear" (a default
    that bit2fasm reports for a tile in use and nextpnr leaves out).

Anything else (a feature with configuration bits that went missing, or a set bit nobody
asked for) fails the check. It runs in the surfcpu-openxc7 image (fasm, prjxray):

    fasmcheck.py --db-root $PRJXRAY_DB_DIR/artix7 --part xc7a35tcsg324-1 routed.fasm decoded.fasm
"""

from __future__ import annotations

import argparse
import sys
import warnings
from collections import Counter

warnings.simplefilter("ignore", RuntimeWarning)       # fasm's note about its textX parser
import fasm                                            # noqa: E402
from prjxray.db import Database                        # noqa: E402


def features(path: str) -> set[tuple[str, int]]:
    """Every enabled feature bit of a FASM file, as (feature, bit address)."""
    out = set()
    for line in fasm.parse_fasm_filename(path):
        if line.set_feature is not None:
            for flat in fasm.canonical_features(line.set_feature):
                out.add((flat.feature, flat.start or 0))
    return out


def classify(grid, feature: str, address: int) -> str:
    tile, rest = feature.split(".", 1)
    info = grid.gridinfo_at_tilename(tile)
    segbits = grid.get_tile_segbits_at_tilename(tile)
    key = f"{info.tile_type}.{rest}"
    if key in segbits.ppips:
        return "pseudo-pip " + str(segbits.ppips[key]).split(".")[-1].lower()
    try:
        bits = list(segbits.feature_to_bits(info.bits, key, address))
    except KeyError:
        return "not in the database"
    if not bits:
        return "no bits"
    return "sets bits" if any(bit.isset for _, bit in bits) else "clear bits only"


def main() -> int:
    ap = argparse.ArgumentParser(description="FASM round-trip check (see the file header)")
    ap.add_argument("--db-root", required=True)
    ap.add_argument("--part", required=True)
    ap.add_argument("routed")
    ap.add_argument("decoded")
    args = ap.parse_args()

    routed, decoded = features(args.routed), features(args.decoded)
    grid = Database(args.db_root, args.part).grid()
    only_routed = sorted(routed - decoded)
    only_decoded = sorted(decoded - routed)
    kinds_r = Counter(classify(grid, f, a) for f, a in only_routed)
    kinds_d = Counter(classify(grid, f, a) for f, a in only_decoded)

    print(f"routed FASM {len(routed)} feature bits, decoded from the bitstream {len(decoded)}, "
          f"in both {len(routed & decoded)}")
    print(f"only in the routed FASM: {len(only_routed)} "
          + ", ".join(f"{k} {v}" for k, v in sorted(kinds_r.items())))
    print(f"only in the decoded FASM: {len(only_decoded)} "
          + ", ".join(f"{k} {v}" for k, v in sorted(kinds_d.items())))
    bad = [(f, a, k) for f, a in only_routed if not (k := classify(grid, f, a)).startswith("pseudo-pip")]
    bad += [(f, a, k) for f, a in only_decoded if (k := classify(grid, f, a)) != "clear bits only"]
    for f, a, k in bad[:50]:
        print(f"  UNEXPLAINED {f}[{a}]: {k}")
    print("fasmcheck: " + ("PASS (every difference is a pseudo-pip or an all-clear default)"
                           if not bad else f"FAIL ({len(bad)} unexplained differences)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
