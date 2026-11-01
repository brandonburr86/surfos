#!/usr/bin/env python3
"""
SurfCPU open flow: the block RAM write enables of the LVS proof
--------------------
File: bramwe.py Date: 10/16/26 (SurfCPU)
--------------------
The LVS extractor of this image (fasm2netlist 0948889f, tileverilog) reads a RAMB18E1's write
enables from the site's first pins, one to one: WEA[1:0] from WEA1..0 and WEBWE[3:0] from
WEBWE3..0 (src/bram_ports.hpp). The site has twice as many (WEA0..3, WEBWE0..7), and the
silicon gives each byte of an 18-bit write port two of them and each byte of the 36-bit
(simple dual port) write port two: byte i's enable is pins 2i and 2i+1. That is how Vivado
connects them (f4pga-xc-fasm2bels, fasm2bels/models/bram_models.py, which rebuilds Vivado
netlists from Vivado's bitstreams) and how nextpnr routes them (himbaechel/uarch/xilinx/
pack.cc: WEA[0] to WEA0 and WEA1, WEA[1] to WEA2 and WEA3; in simple dual port mode WEBWE[i]
to WEBWE(2i) and WEBWE(2i+1)). So with any 18- or 36-bit write port whose byte enables are
not all one signal, the extractor reads the second byte's enable from a pin that carries the
first byte's, and the proof reports a difference that is not in the bitstream: round ddr-3
found it on a 512 x 23 simple dual port memory (WEBWE[3] tied to 0 beside three used byte
lanes, "DIFFER ... WEBWE[3]") and on a 1024 x 16 memory with two byte enables ("DIFFER ...
WEA[1]").

This step runs between the extraction and the proof (tools/openxc7-build.sh) and makes the
extracted netlist read each used byte enable where the silicon reads it, from the block's
mode in the FASM: for the 36-bit write port (SDP_WRITE_WIDTH_36) WEBWE[i] from pin 2i, for an
18-bit port A (WRITE_WIDTH_A_18) WEA[1] from WEA2, for an 18-bit port B (WRITE_WIDTH_B_18)
WEBWE[1] from WEBWE2. Nothing is taken on trust: every pin the silicon ties to a byte enable
must carry the same net as the pin the enable is read from (a pin pair, or all four pins of a
port of 9 bits or less), so the pins the rewritten netlist no longer reads are still checked;
a pin that carries another net, or no route at all beside a routed one, is a difference and
fails LVS, with the pins named. Other pins and blocks (RAMB36E1, whose halves fasm2netlist
already cross-checks) are left as extracted. The original netlist is kept as fabric.raw.v.

    python3 -I devenv/openxc7/bramwe.py --fabric DIR/lvs/fabric.v --fasm DIR/TOP.fasm --report FILE
    python3 -I devenv/openxc7/bramwe.py --self-test

Exit 0 with the report written (its last line: "bramwe: N block RAM(s), M enable(s) re-read,
K difference(s)"), 2 when the netlist cannot be read. Only the Python standard library.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

INST = re.compile(r"^(\s*RAMB18E1\s+\\(\S+)_(RAMB18_Y[01])\s+\()(.*)(\);\s*)$")


def port_bits(conns: str, port: str) -> list[str] | None:
    """The nets of .PORT({a, b, ...}) (MSB first), or None."""
    m = re.search(r"\." + port + r"\(\{([^}]*)\}\)", conns)
    if not m:
        return None
    return [x.strip() for x in m.group(1).split(",")]


def set_port_bits(conns: str, port: str, bits: list[str]) -> str:
    return re.sub(r"\." + port + r"\(\{[^}]*\}\)", "." + port + "({" + ", ".join(bits) + "})", conns, count=1)


def fasm_modes(fasm_text: str) -> dict[tuple[str, str], set[str]]:
    """(tile, site) -> the site's features without the INIT contents."""
    modes: dict[tuple[str, str], set[str]] = {}
    for line in fasm_text.splitlines():
        line = line.strip()
        m = re.match(r"^(\S+?)\.(RAMB18_Y[01])\.([A-Z0-9_]+)$", line)
        if m and not m.group(3).startswith(("INIT", "ZINIT")):
            modes.setdefault((m.group(1), m.group(2)), set()).add(m.group(3))
    return modes


def groups_for(feats: set[str]) -> dict[str, list[list[int]]]:
    """For WEA and WEBWE: the site pins that carry each logical enable the mode uses, in order."""
    sdp = "SDP_WRITE_WIDTH_36" in feats
    g: dict[str, list[list[int]]] = {}
    if sdp:
        g["WEBWE"] = [[0, 1], [2, 3], [4, 5], [6, 7]]
        g["WEA"] = [[0, 1, 2, 3]]                  # unused in this mode: one value on all four
    else:
        g["WEA"] = [[0, 1], [2, 3]] if "WRITE_WIDTH_A_18" in feats else [[0, 1, 2, 3]]
        g["WEBWE"] = [[0, 1], [2, 3]] if "WRITE_WIDTH_B_18" in feats else [[0, 1, 2, 3]]
    return g


def run(fabric: Path, fasm: Path) -> tuple[str, list[str], int, int, int]:
    """Rewrite fabric in place; returns (new text, report lines, blocks, re-read, differences)."""
    text = fabric.read_text()
    modes = fasm_modes(fasm.read_text())
    assigns = dict(re.findall(r"^\s*assign\s+(\S+)\s*=\s*(\S+)\s*;", text, re.M))
    declared = set(re.findall(r"^\s*wire\s+(?:\[[^\]]*\]\s*)?(\S+)\s*;", text, re.M))

    def root(net: str) -> str | None:
        """The net a site pin is routed from, or None when nothing routes to it."""
        if net not in assigns:
            return None
        seen = 0
        while net in assigns and seen < 10000:
            net = assigns[net]
            seen += 1
        return net

    report: list[str] = []
    out_lines: list[str] = []
    blocks = reread = diffs = 0
    for line in text.split("\n"):
        m = INST.match(line)
        if not m:
            out_lines.append(line)
            continue
        head, tile, site, conns, tail = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        blocks += 1
        feats = modes.get((tile, site), set())
        groups = groups_for(feats)
        mode = "SDP 36-bit write" if "SDP_WRITE_WIDTH_36" in feats else \
            "write A %s, write B %s" % ("18" if "WRITE_WIDTH_A_18" in feats else "<=9",
                                        "18" if "WRITE_WIDTH_B_18" in feats else "<=9")
        for port, groups_p in groups.items():
            bits = port_bits(conns, port)
            if bits is None or not bits:
                continue
            width = len(bits)                       # the primitive's: WEA 2, WEBWE 4
            pin0 = bits[-1]                         # the extractor's net for pin 0
            mm = re.match(r"^(.*?)(\d+)$", pin0)
            if not mm:
                report.append(f"{tile}/{site} {port}: pin 0 is {pin0}, not a site pin: left as extracted")
                continue
            prefix = mm.group(1)

            def pin(k: int) -> str | None:
                n = f"{prefix}{k}"
                return n if n in declared else None

            new_bits = list(bits)
            for i, grp in enumerate(groups_p):
                nets = [pin(k) for k in grp]
                roots = [root(n) if n else None for n in nets]
                # every pin of the group must carry the net of the first (or all be unrouted)
                for k, r in zip(grp, roots):
                    if r != roots[0]:
                        diffs += 1
                        report.append(f"DIFFER  {tile}_{site} {port} pin {k} ({r or 'not routed'}) is not pin {grp[0]}'s "
                                      f"({roots[0] or 'not routed'}): {port}[{i}] reaches the block on pins {grp} ({mode})")
                if i < width and nets[0] is not None and bits[width - 1 - i] != nets[0]:
                    report.append(f"re-read {tile}_{site} {port}[{i}] from pin {grp[0]} ({nets[0]}), not "
                                  f"{bits[width - 1 - i]} ({mode})")
                    new_bits[width - 1 - i] = nets[0]
                    reread += 1
            conns = set_port_bits(conns, port, new_bits)
        out_lines.append(head + conns + tail)
    report.append(f"bramwe: {blocks} block RAM(s), {reread} enable(s) re-read, {diffs} difference(s)")
    return "\n".join(out_lines), report, blocks, reread, diffs


def self_test() -> int:
    import tempfile
    def fabric(webwe_src: dict[int, str], wea_src: dict[int, str]) -> str:
        t = "T_X1Y1"
        lines = [f"module fabric (input we0, input we1, input we2);"]
        for k in range(8):
            lines.append(f"  wire {t}_BRAM_RAMB18_WEBWE{k};")
        for k in range(4):
            lines.append(f"  wire {t}_BRAM_RAMB18_WEA{k};")
        for k, s in webwe_src.items():
            lines.append(f"  assign {t}_BRAM_RAMB18_WEBWE{k} = {s};")
        for k, s in wea_src.items():
            lines.append(f"  assign {t}_BRAM_RAMB18_WEA{k} = {s};")
        we = ", ".join(f"{t}_BRAM_RAMB18_WEBWE{k}" for k in (3, 2, 1, 0))
        wa = ", ".join(f"{t}_BRAM_RAMB18_WEA{k}" for k in (1, 0))
        lines.append(f"  RAMB18E1 \\{t}_RAMB18_Y1 (.CLKARDCLK(clk), .WEA({{{wa}}}), .WEBWE({{{we}}}));")
        lines.append("endmodule")
        return "\n".join(lines) + "\n"
    fails = 0
    def case(name, fab, fasm, want_reread, want_diffs, want_webwe=None, want_wea=None):
        nonlocal fails
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "fabric.v"
            f.write_text(fab)
            a = Path(d) / "top.fasm"
            a.write_text(fasm)
            new, rep, blocks, reread, diffs = run(f, a)
            ok = (reread, diffs) == (want_reread, want_diffs)
            if want_webwe is not None:
                ok = ok and port_bits(new, "WEBWE") == ["T_X1Y1_BRAM_RAMB18_WEBWE%d" % k for k in want_webwe]
            if want_wea is not None:
                ok = ok and port_bits(new, "WEA") == ["T_X1Y1_BRAM_RAMB18_WEA%d" % k for k in want_wea]
            print(("ok   " if ok else "FAIL ") + name + f" ({reread} re-read, {diffs} differences)")
            if not ok:
                print("\n".join("     " + r for r in rep))
                fails += 1
    sdp = "T_X1Y1.RAMB18_Y1.SDP_WRITE_WIDTH_36\nT_X1Y1.RAMB18_Y1.WRITE_WIDTH_A_18\nT_X1Y1.RAMB18_Y1.WRITE_WIDTH_B_18\n"
    gnd = {k: "GND" for k in range(4)}
    # the round ddr-3 case: three lanes on one enable, the fourth tied to 0, pins in pairs
    case("SDP, WEBWE[3] tied to 0 beside used lanes: read from pins 0 2 4 6, no difference",
         fabric({0: "we0", 1: "we0", 2: "we0", 3: "we0", 4: "we0", 5: "we0", 6: "GND", 7: "GND"}, gnd), sdp,
         3, 0, [6, 4, 2, 0])
    case("SDP, four enables: each from its pair", fabric({0: "we0", 1: "we0", 2: "we1", 3: "we1", 4: "we2", 5: "we2",
                                                          6: "GND", 7: "GND"}, gnd), sdp, 3, 0, [6, 4, 2, 0])
    case("SDP, pin 7 on another net than pin 6: a difference",
         fabric({0: "we0", 1: "we0", 2: "we0", 3: "we0", 4: "we0", 5: "we0", 6: "GND", 7: "we0"}, gnd), sdp, 3, 1)
    case("SDP, pin 3 not routed beside a routed pin 2: a difference",
         fabric({0: "we0", 1: "we0", 2: "we1", 4: "we2", 5: "we2", 6: "GND", 7: "GND"}, gnd), sdp, 3, 1)
    case("TDP, 18-bit port A: WEA[1] from WEA2",
         fabric({k: "GND" for k in range(8)}, {0: "we0", 1: "we0", 2: "we1", 3: "we1"}),
         "T_X1Y1.RAMB18_Y1.WRITE_WIDTH_A_18\n", 1, 0, None, [2, 0])
    case("TDP, 18-bit port A with WEA1 on the second enable (the extractor's view): a difference",
         fabric({k: "GND" for k in range(8)}, {0: "we0", 1: "we1", 2: "we1", 3: "we1"}),
         "T_X1Y1.RAMB18_Y1.WRITE_WIDTH_A_18\n", 1, 1)
    case("TDP, 18-bit port B, enables interleaved (pin 1 = byte 1's): a difference",
         fabric({0: "we0", 1: "we1", 2: "we0", 3: "we1", 4: "GND", 5: "GND", 6: "GND", 7: "GND"}, gnd),
         "T_X1Y1.RAMB18_Y1.WRITE_WIDTH_B_18\n", 1, 2)
    case("9-bit ports, one enable on four pins: untouched, no difference",
         fabric({0: "we0", 1: "we0", 2: "we0", 3: "we0", 4: "GND", 5: "GND", 6: "GND", 7: "GND"},
                {0: "we1", 1: "we1", 2: "we1", 3: "we1"}),
         "T_X1Y1.RAMB18_Y1.WRITE_WIDTH_A_9\nT_X1Y1.RAMB18_Y1.WRITE_WIDTH_B_9\n", 0, 0, [3, 2, 1, 0], [1, 0])
    print("bramwe self-test: " + ("PASS" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="the block RAM write enables of the LVS proof")
    ap.add_argument("--fabric")
    ap.add_argument("--fasm")
    ap.add_argument("--report")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args(argv[1:])
    if a.self_test:
        return self_test()
    if not (a.fabric and a.fasm and a.report):
        ap.error("--fabric, --fasm and --report are required")
    fabric = Path(a.fabric)
    try:
        new, report, _blocks, _reread, _diffs = run(fabric, Path(a.fasm))
    except (OSError, UnicodeDecodeError) as e:
        print(f"bramwe: cannot read the netlist: {e}", file=sys.stderr)
        return 2
    shutil.copyfile(fabric, fabric.with_name("fabric.raw.v"))
    fabric.write_text(new)
    Path(a.report).write_text("\n".join(report) + "\n")
    print(report[-1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
