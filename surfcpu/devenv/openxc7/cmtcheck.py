#!/usr/bin/env python3
"""
SurfCPU MMCM/PLL divider check
--------------------
File: cmtcheck.py Date: 10/16/26 (SurfCPU)
--------------------
The open flow's LVS proof does not model clock managers, so this checks their dividers
separately. For every MMCME2/PLLE2 in a synthesized netlist it recomputes the counter
fields (HIGH_TIME, LOW_TIME, EDGE, NO_COUNT and, for fractional MMCM dividers, FRAC, FRAC_EN,
FRAC_WF_R, FRAC_WF_F, PHASE_MUX_F) the way AMD's XAPP888 reference functions do (the integer
divider and mmcm_frac_count_calc, here as ported in F4PGA's f4pga-arch-defs
xilinx/xc7/techmap/cells_map.v, which takes EDGE from the integer calculation as Vivado
does), and the phase fields (DELAY_TIME, PHASE_MUX, MX) as XAPP888's mmcm_phase_calc does,
and compares them with the fields nextpnr wrote into the routed FASM.

The functions are checked against register values Vivado produced for divide ratios 4.000
to 5.875 in steps of 1/8 (published by Eli Billauer, "Reprogramming a Series-7 MMCM for
fractional division ratios", billauer.se/blog/?p=4506). nextpnr 1.0.0 writes different
values whenever the integer part of a fractional CLKFBOUT_MULT_F or CLKOUT0_DIVIDE_F is odd
(see --table); this check makes such a design fail instead of producing a wrong clock.

    cmtcheck.py --self-test
    cmtcheck.py --table                      XAPP888 against nextpnr 1.0.0's rule, 2.000-9.875
    cmtcheck.py --json top.json --placement placement.json --fasm top.fasm

Only 50 % duty cycles are recomputed, and phases only on integer dividers (a fractional MMCM
divider at zero phase only); anything else is reported unchecked. Until round ddr-3 every
output with a phase was unchecked, and no output's phase fields were compared: LiteDRAM's
90-degree DQS clock (CLKOUT3) is now checked. nextpnr 1.0.0 floors the phase to an eighth of
a VCO cycle where XAPP888 rounds to the nearest (see the self-test), so a phase that is not a
whole number of eighths can fail here.
The parameters are read as Yosys's JSON writes them: a constant as its bits, most significant
first, at its own width (LiteDRAM's 5'd16 is "10000", a plain 16 thirty-two bits); a real or a
string as text, with a space appended to a string that would look like bits ("10 "). Until
round ddr-3 bit strings shorter than eight were read as decimal numbers, which made LiteDRAM's
PLL dividers (16 read as 10000) look wrong. The self-test also checks this reading.
The self-test and --table use only the standard library; checking a design needs fasm.
"""

from __future__ import annotations

import argparse
import json
import math
import sys

MMCM_TYPES = ("MMCME2_ADV", "MMCME2_BASE")
PLL_TYPES = ("PLLE2_ADV", "PLLE2_BASE")


def divider(d: int) -> dict:
    """XAPP888's integer counter for divide d at a 50 % duty cycle."""
    if d == 1:
        return {"HIGH_TIME": 1, "LOW_TIME": 1, "EDGE": 0, "NO_COUNT": 1}
    half2 = d                                     # 2 * (0.5 * d): the half-step count
    high, edge = half2 // 2, half2 % 2
    if high == 0:
        high, edge = 1, 0
    if high == d:
        high, edge = d - 1, 1
    return {"HIGH_TIME": high, "LOW_TIME": d - high, "EDGE": edge, "NO_COUNT": 0}


def frac_divider(divide: float) -> dict:
    """XAPP888's mmcm_frac_count_calc at zero phase, EDGE from the integer counter."""
    eighths = round(divide * 8)
    d_int, d_frac = eighths // 8, eighths % 8
    even = d_int >> 1
    odd = d_int - 2 * even
    odd_and_frac = 8 * odd + d_frac
    return {
        "HIGH_TIME": even - (odd_and_frac <= 8),
        "LOW_TIME": even - (odd_and_frac <= 9),
        "EDGE": divider(d_int)["EDGE"],
        "NO_COUNT": 0,
        "FRAC": d_frac,
        "FRAC_EN": 1,
        "FRAC_WF_R": int(1 <= odd_and_frac <= 8),
        "FRAC_WF_F": int(2 <= odd_and_frac <= 9 or (d_frac == 1 and d_int == 2)),
        "PHASE_MUX_F": (4 * odd + (d_frac >> 1)) % 8,
    }


def phase_fields(divide: int, phase: float) -> dict:
    """XAPP888's mmcm_phase_calc: the phase as whole VCO cycles (DELAY_TIME) and eighths of one
    (PHASE_MUX), in its 22.10 fixed point, rounded to the nearest eighth by round_frac; MX 0."""
    p = int(round(phase * 1000))                  # XAPP888 takes thousandths of a degree
    if p < 0:
        p += 360000
    fixed = (p << 10) // 1000
    cycles = (fixed * divide) // 360
    if cycles & (1 << 6):                         # round_frac(cycles, 3)
        cycles += 1 << 7
    return {"DELAY_TIME": (cycles >> 10) & 0x3F, "PHASE_MUX": (cycles >> 7) & 7, "MX": 0}


def nextpnr_phase(divide: int, phase: float) -> dict:
    """What nextpnr 1.0.0 writes (fasm.cc, write_pll_clkout): the phase floored to an eighth."""
    eighths = math.floor((phase / 360) * divide * 8)
    return {"DELAY_TIME": eighths // 8, "PHASE_MUX": eighths % 8, "MX": 0}


def expected(divide: float, fractional_ok: bool) -> dict:
    """The fields for a divider: fractional (MMCM CLKFBOUT/CLKOUT0) or integer."""
    if fractional_ok and round(divide * 8) % 8:
        return frac_divider(divide)
    e = divider(int(round(divide)))
    if fractional_ok:
        e.update({"FRAC": 0, "FRAC_EN": 0, "FRAC_WF_R": 0, "FRAC_WF_F": 0, "PHASE_MUX_F": 0})
    return e


def nextpnr_rule(divide: float) -> dict:
    """What nextpnr 1.0.0 writes (himbaechel/uarch/xilinx/fasm.cc, write_mmcm_clkout)."""
    high = math.floor(divide / 2)
    low = int(math.floor(divide) - high)
    edge = int(high != low)
    frac = math.floor(divide * 8) - math.floor(divide) * 8
    f = {"EDGE": edge, "NO_COUNT": 0, "FRAC": frac, "FRAC_EN": int(frac != 0),
         "FRAC_WF_R": int(frac != 0), "FRAC_WF_F": 0, "PHASE_MUX_F": 0}
    if frac:
        high, low = high - 1, low - 1
        if frac >> 1:
            f["FRAC_WF_F"], f["PHASE_MUX_F"] = 1, frac >> 1
    f["HIGH_TIME"], f["LOW_TIME"] = high, low
    return f


# Vivado's register values for CLKFBOUT_MULT_F / CLKOUT0_DIVIDE_F = 4.000 .. 5.875 (Billauer):
# frac_en, high_time, low_time, edge, frac, phase_mux_f, frac_wf_r, frac_wf_f
VIVADO_TABLE = [
    (0, 2, 2, 0, 0, 0, 0, 0), (1, 1, 1, 0, 1, 0, 1, 0), (1, 1, 1, 0, 2, 1, 1, 1),
    (1, 1, 1, 0, 3, 1, 1, 1), (1, 1, 1, 0, 4, 2, 1, 1), (1, 1, 1, 0, 5, 2, 1, 1),
    (1, 1, 1, 0, 6, 3, 1, 1), (1, 1, 1, 0, 7, 3, 1, 1), (0, 2, 3, 1, 0, 0, 0, 0),
    (1, 2, 1, 1, 1, 4, 0, 1), (1, 2, 2, 1, 2, 5, 0, 0), (1, 2, 2, 1, 3, 5, 0, 0),
    (1, 2, 2, 1, 4, 6, 0, 0), (1, 2, 2, 1, 5, 6, 0, 0), (1, 2, 2, 1, 6, 7, 0, 0),
    (1, 2, 2, 1, 7, 7, 0, 0),
]
TABLE_FIELDS = ("FRAC_EN", "HIGH_TIME", "LOW_TIME", "EDGE", "FRAC", "PHASE_MUX_F", "FRAC_WF_R", "FRAC_WF_F")


def self_test() -> int:
    bad = 0
    for k, row in enumerate(VIVADO_TABLE):
        ratio = 4 + k / 8
        got = expected(ratio, True)
        want = dict(zip(TABLE_FIELDS, row))
        if any(got[f] != want[f] for f in TABLE_FIELDS):
            bad += 1
            print(f"self-test FAIL {ratio:.3f}: XAPP888 {got}, Vivado {want}")
    # integer dividers: 1 is bypassed, an odd divide splits 2:3 with EDGE set
    if divider(1)["NO_COUNT"] != 1 or divider(35) != {"HIGH_TIME": 17, "LOW_TIME": 18, "EDGE": 1, "NO_COUNT": 0}:
        bad += 1
        print("self-test FAIL: integer divider")
    # phases (XAPP888's mmcm_phase_calc): LiteDRAM's DQS clock, 90 degrees of divide 5 (VCO 1600 MHz,
    # 320 MHz) is 1.25 VCO cycles, and of divide 4 (400 MHz) 1 cycle; -90 degrees is 270; 50 degrees of
    # divide 5 is 5.56 eighths, which XAPP888 rounds to 6 and nextpnr floors to 5
    for div, ph, dt, pm in ((5, 90.0, 1, 2), (4, 90.0, 1, 0), (10, -90.0, 7, 4), (8, 45.0, 1, 0),
                            (5, 0.0, 0, 0), (5, 50.0, 0, 6), (64, 180.0, 32, 0)):
        got = phase_fields(div, ph)
        if (got["DELAY_TIME"], got["PHASE_MUX"], got["MX"]) != (dt, pm, 0):
            bad += 1
            print(f"self-test FAIL: phase {ph} of divide {div}: {got}, not DELAY_TIME {dt} PHASE_MUX {pm}")
    if nextpnr_phase(5, 90.0) != phase_fields(5, 90.0) or nextpnr_phase(5, 50.0) == phase_fields(5, 50.0):
        bad += 1
        print("self-test FAIL: nextpnr's phase rule")
    # parameters as Yosys's JSON writes them (a PLLE2_ADV of LiteDRAM's, read by this image's Yosys)
    for raw, want in (("10000", 16), ("111", 7), ("1", 1), ("0", 0), ("10100", 20),
                      ("00000000000000000000000000001010", 10), ("10.000000", 10.0), ("90.000000", 90.0),
                      ("18.125", 18.125), ("10 ", 10.0), ("FALSE", "FALSE"), (36, 36)):
        got = param({"parameters": {"P": raw}}, "P", None)
        if got != want or type(got) is not type(want):
            bad += 1
            print(f"self-test FAIL: parameter {raw!r} read as {got!r}, not {want!r}")
    print("cmtcheck self-test: " + ("PASS (XAPP888 functions reproduce Vivado's 16 values; phases; Yosys's "
                                    "parameter formats read)" if not bad else f"{bad} failures"))
    return 1 if bad else 0


def table() -> int:
    print("divide  XAPP888 (high low edge frac pmf wfr wff)   nextpnr 1.0.0               same?")
    for eighths in range(16, 80):
        d = eighths / 8
        x, n = expected(d, True), nextpnr_rule(d)
        keys = ("HIGH_TIME", "LOW_TIME", "EDGE", "FRAC", "PHASE_MUX_F", "FRAC_WF_R", "FRAC_WF_F")
        fx = " ".join(f"{x[k]:>2}" for k in keys)
        fn = " ".join(f"{n[k]:>2}" for k in keys)
        print(f"{d:6.3f}  {fx}        {fn}   {'yes' if all(x[k] == n[k] for k in keys) else 'NO'}")
    return 0


# ---------------------------------------------------------------- checking a routed design
def param(cell: dict, name: str, default):
    v = cell.get("parameters", {}).get(name)
    if v is None:
        return default
    if isinstance(v, int):
        return v
    v = str(v)
    if v and set(v) <= {"0", "1"}:              # a constant: its bits, whatever its width
        return int(v, 2)
    v = v.strip().strip('"')                    # a real, or a string (maybe with Yosys's space)
    try:
        return float(v)
    except ValueError:
        return v


def fasm_fields(path: str) -> dict:
    import warnings
    warnings.simplefilter("ignore", RuntimeWarning)
    import fasm
    fields: dict = {}
    for line in fasm.parse_fasm_filename(path):
        if line.set_feature is None:
            continue
        for flat in fasm.canonical_features(line.set_feature):
            fields[flat.feature] = fields.get(flat.feature, 0) | (1 << (flat.start or 0))
    return fields


def check_design(netlist: str, placement: str, fasm_path: str) -> int:
    with open(netlist) as f:
        design = json.load(f)
    with open(placement) as f:
        place = json.load(f)
    found = fasm_fields(fasm_path)
    mismatches = unchecked = checked = 0
    cells = [(n, c) for m in design["modules"].values() for n, c in m.get("cells", {}).items()
             if c["type"] in MMCM_TYPES + PLL_TYPES]
    if not cells:
        print("cmtcheck: no MMCME2/PLLE2 in the design")
        return 0
    for name, cell in cells:
        is_mmcm = cell["type"] in MMCM_TYPES
        site = "MMCME2_ADV" if is_mmcm else "PLLE2_ADV"
        if name not in place:
            print(f"cmtcheck: {name} is not in the placement")
            return 1
        prefix = f"{place[name]['tile']}.{site}."
        conns = cell.get("connections", {})
        outs = ["CLKFBOUT"] + [f"CLKOUT{i}" for i in range(7 if is_mmcm else 6)
                               if conns.get(f"CLKOUT{i}")]
        print(f"{name} ({cell['type']} at {place[name]['tile']}):")

        def compare(label: str, want: dict, regs: dict) -> None:
            nonlocal mismatches, checked
            diffs = []
            for field, value in want.items():
                got = found.get(prefix + regs[field], 0)
                checked += 1
                if got != value:
                    diffs.append(f"{field} {got} (XAPP888: {value})")
            mismatches += bool(diffs)
            print(f"  {label}: " + ("matches XAPP888" if not diffs else "MISMATCH " + ", ".join(diffs)))

        div = param(cell, "DIVCLK_DIVIDE", 1)
        compare(f"DIVCLK_DIVIDE {div}", divider(int(div)),
                {k: f"DIVCLK_DIVCLK_{k}" for k in ("HIGH_TIME", "LOW_TIME", "EDGE", "NO_COUNT")})
        for out in outs:
            pname = {"CLKFBOUT": "CLKFBOUT_MULT_F" if is_mmcm else "CLKFBOUT_MULT",
                     "CLKOUT0": "CLKOUT0_DIVIDE_F" if is_mmcm else "CLKOUT0_DIVIDE"}.get(out, f"{out}_DIVIDE")
            value = param(cell, pname, 5.0 if out == "CLKFBOUT" else 1.0)
            duty = param(cell, f"{out}_DUTY_CYCLE", 0.5) if out != "CLKFBOUT" else 0.5
            phase = float(param(cell, f"{out}_PHASE", 0.0))
            frac_ok = is_mmcm and out in ("CLKFBOUT", "CLKOUT0")
            fractional = frac_ok and round(float(value) * 8) % 8 != 0
            if float(duty) != 0.5 or (phase != 0.0 and fractional):
                unchecked += 1
                print(f"  {pname} {value}: unchecked (duty {duty}, phase {phase})")
                continue
            want = expected(float(value), frac_ok)
            want.update(phase_fields(int(round(float(value))), phase))
            own = "CLKOUT2_FRACTIONAL_" if out in ("CLKOUT5", "CLKOUT6") and is_mmcm else "CLKOUT2_"
            regs = {"HIGH_TIME": f"{out}_CLKOUT1_HIGH_TIME", "LOW_TIME": f"{out}_CLKOUT1_LOW_TIME",
                    "EDGE": f"{out}_{own}EDGE", "NO_COUNT": f"{out}_{own}NO_COUNT",
                    "PHASE_MUX": f"{out}_CLKOUT1_PHASE_MUX", "DELAY_TIME": f"{out}_{own}DELAY_TIME",
                    "MX": f"{out}_{own}MX"}
            if frac_ok:
                shared = "CLKOUT5" if out == "CLKOUT0" else "CLKOUT6"
                regs.update({"FRAC": f"{out}_CLKOUT2_FRAC", "FRAC_EN": f"{out}_CLKOUT2_FRAC_EN",
                             "FRAC_WF_R": f"{out}_CLKOUT2_FRAC_WF_R",
                             "FRAC_WF_F": f"{shared}_CLKOUT2_FRACTIONAL_FRAC_WF_F",
                             "PHASE_MUX_F": f"{shared}_CLKOUT2_FRACTIONAL_PHASE_MUX_F"})
            compare(f"{pname} {value:g}" + (f", phase {phase:g}" if phase else ""), want, regs)
    print(f"cmtcheck: {checked} fields compared, {mismatches} counters differ from XAPP888, {unchecked} unchecked: "
          + ("PASS" if not mismatches else "FAIL"))
    return 1 if mismatches else 0


def main() -> int:
    ap = argparse.ArgumentParser(description="MMCM/PLL divider check (see the file header)")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--table", action="store_true")
    ap.add_argument("--json")
    ap.add_argument("--placement")
    ap.add_argument("--fasm")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.table:
        return table()
    if not (args.json and args.placement and args.fasm):
        ap.error("--json, --placement and --fasm are needed (or --self-test, --table)")
    return check_design(args.json, args.placement, args.fasm)


if __name__ == "__main__":
    sys.exit(main())
