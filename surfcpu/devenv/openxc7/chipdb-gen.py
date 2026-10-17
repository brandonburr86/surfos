#!/usr/bin/env python3
"""
SurfCPU open flow: the chip database without global-buffer route-throughs
--------------------
File: chipdb-gen.py Date: 10/16/26 (SurfCPU)
--------------------
Runs this image's own nextpnr chip database generator (openXC7/nextpnr 1.0.0,
/opt/openxc7/share/nextpnr/himbaechel/uarch/xilinx/gen/xilinx_gen.py, the one the image built
chipdb-*.bin with) with one more rule in its pip filter (filters.include_pip): leave out the
route-through pips of the global buffer tiles (CLK_BUFG_TOP_R / CLK_BUFG_BOT_R:
CLK_BUFG_BUFGCTRLn_O <- CLK_BUFG_BUFGCTRLn_I0, 16 per tile type). Through them nextpnr routed
ordinary nets into unused BUFGCTRL sites and onto the global clock lines, which the bitstream does
not configure and which LVS cannot tell from a wire; on the board those nets did not arrive (round
ddr-4, docs/OPEN-FLOW.md 6.8, devenv/openxc7/gclkcheck.py). A real global buffer is a placed
BUFGCTRL cell and routes through its own pins, so clocks are unaffected. Everything else is the
image's generator unchanged: with --keep-bufg-rt it writes the image's own database byte for byte
(checked in round ddr-4: chipdb-xc7a100t.bin b4098ce6...).

    python3 devenv/openxc7/chipdb-gen.py [--keep-bufg-rt] --xray DB --device xc7a100t --bba OUT.bba
    bbasm -l OUT.bba chipdb-xc7a100t.bin

tools/openxc7-build.sh runs it once per image and die and keeps the result in
build/openxc7/chipdb/; devenv/openxc7/chipdb.sha256 holds the SHA-256 each must have. Inside the
openXC7 image only (it imports the generator from the image).
"""

import runpy
import sys

GEN = "/opt/openxc7/share/nextpnr/himbaechel/uarch/xilinx/gen"


def main() -> None:
    keep = "--keep-bufg-rt" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--keep-bufg-rt"]
    sys.path.insert(0, GEN)
    import filters                      # the generator's own module: runpy reuses it

    original = filters.include_pip
    left_out = set()

    def include_pip(tile_type, pip):
        if not keep and pip.is_route_thru() and tile_type.startswith("CLK_BUFG"):
            left_out.add(f"{tile_type}.{pip.dst_wire().name()}.{pip.src_wire().name()}")
            return False
        return original(tile_type, pip)

    filters.include_pip = include_pip
    sys.argv = [GEN + "/xilinx_gen.py"] + args
    try:
        runpy.run_path(GEN + "/xilinx_gen.py", run_name="__main__")
    finally:
        print(f"chipdb-gen: {len(left_out)} BUFGCTRL route-through pip(s) left out"
              + (" (--keep-bufg-rt: the image's own database)" if keep else ""), file=sys.stderr)
        for name in sorted(left_out):
            print(f"chipdb-gen:   {name}", file=sys.stderr)


if __name__ == "__main__":
    main()
