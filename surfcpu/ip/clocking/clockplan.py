#!/usr/bin/env python3
"""
SurfCPU clock plan checker and generator
--------------------
File: clockplan.py Date: 10/15/26 (SurfCPU)
--------------------
Checks the Arty A7 clock plan (ip/clocking/arty_a7_clocks.toml) against the 7-series
clocking limits of both Arty parts and against the frequencies in spec/soc.toml, and
generates the files that use it:

    boards/arty_a7/rtl/arty_a7_clock_params.vh   localparams for arty_a7_clocks.v
    boards/arty_a7/xdc/arty_a7_clocks.xdc        the oscillator's create_clock

    python3 ip/clocking/clockplan.py gen      write the generated files
    python3 ip/clocking/clockplan.py check    exit 1 if a setting is illegal or a file is stale
    python3 ip/clocking/clockplan.py report   the plan, its margins and the alternatives

Limits (both parts have the same CMT limits; each is checked separately all the same):
  AMD DS181 "Artix 7 FPGAs Data Sheet: DC and AC Switching Characteristics" v1.27.1
  (July 3, 2024), Table 37 (MMCM) and Table 38 (PLL), columns -1 (1.0 V) and -1LI (0.95 V);
  AMD UG472 "7 Series FPGAs Clocking Resources" v1.14 (July 30, 2018), Table 3-7 (MMCM
  attributes) and Table 3-8 (PLL attributes). The Arty A7-35T carries XC7A35TICSG324-1L
  (the -1LI grade, VCCINT 0.95 V) and the A7-100T XC7A100TCSG324-1 (-1, 1.0 V), per the
  Digilent Arty A7 reference manual, sections 1 and 3.
Only the Python standard library is used.
"""

from __future__ import annotations

import sys
import tomllib
from fractions import Fraction as F
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent          # surfcpu/
sys.path.insert(0, str(ROOT / "tools"))
import socspec  # noqa: E402  (tools/socspec.py: the spec loader)

PLAN = ROOT / "ip" / "clocking" / "arty_a7_clocks.toml"
OUT_VH = ROOT / "boards" / "arty_a7" / "rtl" / "arty_a7_clock_params.vh"
OUT_XDC = ROOT / "boards" / "arty_a7" / "xdc" / "arty_a7_clocks.xdc"

MHZ = 1_000_000

# DS181 v1.27.1 Tables 37 and 38: (min, max) in Hz, per speed grade.
SPEED_GRADES = {
    "-1 (1.0 V), XC7A100TCSG324-1 (Arty A7-100T)": {
        "MMCM": {"fin": (10 * MHZ, 800 * MHZ), "fpfd": (10 * MHZ, 450 * MHZ),
                 "fvco": (600 * MHZ, 1200 * MHZ), "fout": (F(469, 100) * MHZ, 800 * MHZ)},
        "PLL": {"fin": (19 * MHZ, 800 * MHZ), "fpfd": (19 * MHZ, 450 * MHZ),
                "fvco": (800 * MHZ, 1600 * MHZ), "fout": (F(625, 100) * MHZ, 800 * MHZ)},
    },
    "-1LI (0.95 V), XC7A35TICSG324-1L (Arty A7-35T)": {
        "MMCM": {"fin": (10 * MHZ, 800 * MHZ), "fpfd": (10 * MHZ, 450 * MHZ),
                 "fvco": (600 * MHZ, 1200 * MHZ), "fout": (F(469, 100) * MHZ, 800 * MHZ)},
        "PLL": {"fin": (19 * MHZ, 800 * MHZ), "fpfd": (19 * MHZ, 450 * MHZ),
                "fvco": (800 * MHZ, 1600 * MHZ), "fout": (F(625, 100) * MHZ, 800 * MHZ)},
    },
}

# UG472 v1.14 Tables 3-7 and 3-8: attribute ranges. (min, max, step); step 1/8 = fractional.
ATTRS = {
    "MMCME2_BASE": {
        "divclk_divide": (1, 106, 1),
        "clkfbout_mult_f": (2, 64, F(1, 8)),
        "clkout0_divide_f": (1, 128, F(1, 8)),   # fractional values only from 2.000 up
        "clkin1_period_ns": (F(938, 1000), 100, None),
    },
    "PLLE2_BASE": {
        "divclk_divide": (1, 56, 1),
        "clkfbout_mult": (2, 64, 1),
        "clkout0_divide": (1, 128, 1),
        "clkin1_period_ns": (F(938, 1000), F(52631, 1000), None),
    },
}
KIND = {"MMCME2_BASE": "MMCM", "PLLE2_BASE": "PLL"}
MULT_KEY = {"MMCME2_BASE": "clkfbout_mult_f", "PLLE2_BASE": "clkfbout_mult"}
DIV_KEY = {"MMCME2_BASE": "clkout0_divide_f", "PLLE2_BASE": "clkout0_divide"}


class PlanError(Exception):
    pass


def frac(v) -> F:
    """An exact value from a TOML number (floats go through their decimal text)."""
    return F(str(v)) if isinstance(v, float) else F(v)


def load_plan():
    spec = socspec.load()
    with open(PLAN, "rb") as f:
        plan = tomllib.load(f)
    fin = F(spec.clocks[plan["input"]["clock"]])
    return spec, plan, fin


def check_domain(name: str, dom: dict, fin: F, spec) -> tuple[dict, list[str]]:
    """Check one clock (sys or pix); returns its numbers and a list of errors."""
    errs: list[str] = []
    prim = dom["primitive"]
    if prim not in ATTRS:
        return {}, [f"{name}: primitive {prim!r} is not MMCME2_BASE or PLLE2_BASE"]
    d = frac(dom["divclk_divide"])
    m = frac(dom[MULT_KEY[prim]])
    o = frac(dom[DIV_KEY[prim]])
    period = F(10**9) / fin
    vals = {"divclk_divide": d, MULT_KEY[prim]: m, DIV_KEY[prim]: o, "clkin1_period_ns": period}
    for key, (lo, hi, step) in ATTRS[prim].items():
        v = vals[key]
        if not lo <= v <= hi:
            errs.append(f"{name}: {key} = {float(v)} outside {float(lo)}..{float(hi)} (UG472)")
        if step is not None and (v / step).denominator != 1:
            errs.append(f"{name}: {key} = {float(v)} is not a multiple of {step} (UG472)")
    if prim == "MMCME2_BASE" and o.denominator != 1 and o < 2:
        errs.append(f"{name}: a fractional clkout0_divide_f must be at least 2.000 (UG472 Table 3-7)")
    fpfd = fin / d
    fvco = fin * m / d
    fout = fvco / o
    nums = {"prim": prim, "d": d, "m": m, "o": o, "fin": fin, "fpfd": fpfd, "fvco": fvco,
            "fout": fout, "period_ns": period}
    for grade, limits in SPEED_GRADES.items():
        lim = limits[KIND[prim]]
        for key, v in (("fin", fin), ("fpfd", fpfd), ("fvco", fvco), ("fout", fout)):
            lo, hi = lim[key]
            if not lo <= v <= hi:
                errs.append(f"{name}: {key} {float(v) / MHZ:.4f} MHz outside "
                            f"{float(lo) / MHZ:g}..{float(hi) / MHZ:g} MHz for {grade} (DS181)")
    target = spec.clocks[dom["spec_hz"]]
    if round(fout) != target or abs(fout - target) >= F(1, 2):
        errs.append(f"{name}: makes {float(fout):.3f} Hz, but spec/soc.toml "
                    f"clocks.{dom['spec_hz']} = {target}")
    nums["spec_hz"] = target
    return nums, errs


def check_plan() -> tuple[dict, list[str]]:
    spec, plan, fin = load_plan()
    out: dict = {"fin": fin}
    errs: list[str] = []
    for name in ("sys", "pix"):
        nums, e = check_domain(name, plan[name], fin, spec)
        out[name] = nums
        errs += e
    return out, errs


def fmt_real(v: F) -> str:
    """A Verilog real literal that is exactly v (all settings here are multiples of 1/8)."""
    s = f"{float(v):.3f}"
    if F(s) != v:
        raise PlanError(f"{v} has no exact 3-decimal form")
    return s


def gen_vh(n: dict) -> str:
    s, p = n["sys"], n["pix"]
    lines = [
        "// Generated from ip/clocking/arty_a7_clocks.toml and spec/soc.toml by",
        "// ip/clocking/clockplan.py gen -- do not edit. Checked against DS181 v1.27.1 Tables 37/38",
        "// (-1 and -1LI) and UG472 v1.14 Tables 3-7/3-8; docs/BOARD.md section 2.",
        "// `include it inside the module body (localparams, no include guard).",
        "",
        "/* verilator lint_off UNUSEDPARAM */",
        f"localparam real    ARTY_CLKIN_PERIOD_NS = {fmt_real(s['period_ns'])};"
        f"  // oscillator, clocks.osc_hz = {int(n['fin'])}",
        "",
        f"// sys_clk: {s['prim']}, VCO {float(s['fvco']) / MHZ:g} MHz, PFD {float(s['fpfd']) / MHZ:g} MHz",
        f"localparam integer ARTY_SYS_DIVCLK_DIVIDE  = {int(s['d'])};",
        f"localparam integer ARTY_SYS_CLKFBOUT_MULT  = {int(s['m'])};",
        f"localparam integer ARTY_SYS_CLKOUT0_DIVIDE = {int(s['o'])};",
        f"localparam integer ARTY_SYS_HZ = {s['spec_hz']};  // exactly {float(s['fout']):.3f} Hz",
        "",
        f"// pix_clk: {p['prim']}, VCO {float(p['fvco']) / MHZ:g} MHz, PFD {float(p['fpfd']) / MHZ:g} MHz",
        f"localparam integer ARTY_PIX_DIVCLK_DIVIDE   = {int(p['d'])};",
        f"localparam real    ARTY_PIX_CLKFBOUT_MULT_F  = {fmt_real(p['m'])};",
        f"localparam real    ARTY_PIX_CLKOUT0_DIVIDE_F = {fmt_real(p['o'])};",
        f"localparam integer ARTY_PIX_HZ = {p['spec_hz']};  // {float(p['fout']):.3f} Hz",
        "/* verilator lint_on UNUSEDPARAM */",
        "",
    ]
    return "\n".join(lines)


def gen_xdc(n: dict) -> str:
    period = n["sys"]["period_ns"]
    return "\n".join([
        "# Generated from spec/soc.toml (clocks.osc_hz) by ip/clocking/clockplan.py gen -- do not edit.",
        "# The board oscillator. Vivado derives the MMCM and PLL output clocks from it by itself;",
        "# boards/arty_a7/xdc/arty_a7_timing.xdc names them sys_clk and pix_clk.",
        f"create_clock -name clk100 -period {fmt_real(period)} [get_ports clk100]",
        "",
    ])


def outputs(n: dict) -> dict[Path, str]:
    return {OUT_VH: gen_vh(n), OUT_XDC: gen_xdc(n)}


def alternatives(fin: F, target: int, nominal: int) -> list[tuple]:
    """Every legal MMCM setting (both grades) within 0.1% of the nominal pixel clock."""
    lim = SPEED_GRADES[next(iter(SPEED_GRADES))]["MMCM"]
    found = []
    for d in range(1, 107):
        fpfd = fin / d
        if not lim["fpfd"][0] <= fpfd <= lim["fpfd"][1]:
            continue
        for m8 in range(16, 64 * 8 + 1):
            m = F(m8, 8)
            fvco = fpfd * m
            if not lim["fvco"][0] <= fvco <= lim["fvco"][1]:
                continue
            ideal = fvco / nominal
            for o8 in {int(ideal * 8), int(ideal * 8) + 1}:
                o = F(o8, 8)
                if not (2 <= o <= 128):
                    continue
                f = fvco / o
                ppm = (f - nominal) / nominal * 10**6
                if abs(ppm) <= 1000:
                    found.append((abs(ppm), d, m, o, f, ppm, round(f) == target))
    found.sort(key=lambda t: (t[0], t[1], -t[4]))
    return found


def report(n: dict) -> str:
    spec = socspec.load()
    out = []
    for name in ("sys", "pix"):
        x = n[name]
        out.append(f"{name}_clk: {x['prim']} D={x['d']} M={float(x['m']):g} O={float(x['o']):g}: "
                   f"fin {float(x['fin']) / MHZ:g} MHz, PFD {float(x['fpfd']) / MHZ:g} MHz, "
                   f"VCO {float(x['fvco']) / MHZ:g} MHz, out {float(x['fout']):.3f} Hz "
                   f"(spec {x['spec_hz']} Hz)")
        for grade, limits in SPEED_GRADES.items():
            lim = limits[KIND[x["prim"]]]
            vlo, vhi = lim["fvco"]
            plo, phi = lim["fpfd"]
            out.append(f"    {grade}: VCO {float(vlo) / MHZ:g}..{float(vhi) / MHZ:g} MHz "
                       f"(margin {float(x['fvco'] - vlo) / MHZ:g} / {float(vhi - x['fvco']) / MHZ:g}), "
                       f"PFD {float(plo) / MHZ:g}..{float(phi) / MHZ:g} MHz: ok")
    nominal = spec.clocks["pix_hz_nominal"]
    p = n["pix"]["fout"]
    out.append(f"pix_clk error against the 25.175 MHz standard: {float((p - nominal) / nominal * 10**6):+.1f} ppm; "
               f"frame rate {float(p / (800 * 525)):.4f} Hz (800 x 525)")
    out.append("legal MMCM settings nearest clocks.pix_hz_nominal, one line per frequency (smallest D):")
    seen: dict[F, tuple] = {}
    for alt in alternatives(n["fin"], spec.clocks["pix_hz"], nominal):
        seen.setdefault(alt[4], alt)
    ranked = sorted(seen.values(), key=lambda t: t[0])
    sys_hz = spec.clocks["sys_hz"]
    for rank, (_, d, m, o, f, ppm, same) in enumerate(ranked, 1):
        if rank > 6 and not same:
            continue
        fvco = n["fin"] * m / d
        one = (fvco / sys_hz).denominator == 1 and 1 <= fvco / sys_hz <= 128
        out.append(f"    #{rank}: D={d} M={float(m):g} O={float(o):g}  VCO {float(fvco) / MHZ:g} MHz  "
                   f"{float(f):.1f} Hz  {float(ppm):+.1f} ppm"
                   f"{'  (VCO/' + str(int(fvco / sys_hz)) + ' also gives sys_hz: one MMCM for both)' if one else ''}"
                   f"{'  <- the spec (clocks.pix_hz)' if same else ''}")
    return "\n".join(out)


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else "check"
    try:
        n, errs = check_plan()
    except (socspec.SpecError, PlanError, KeyError) as e:
        print(f"clockplan: {e}", file=sys.stderr)
        return 1
    if errs:
        for e in errs:
            print(f"clockplan: {e}", file=sys.stderr)
        return 1
    if cmd == "gen":
        for path, text in outputs(n).items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            print(f"clockplan: wrote {path.relative_to(ROOT)}")
        return 0
    if cmd == "check":
        stale = [p for p, t in outputs(n).items() if not p.exists() or p.read_text() != t]
        for p in stale:
            print(f"clockplan: {p.relative_to(ROOT)} is missing or out of date (run: "
                  f"python3 ip/clocking/clockplan.py gen)", file=sys.stderr)
        if stale:
            return 1
        print("clockplan: sys_clk and pix_clk settings legal for -1 and -1LI; generated files current")
        return 0
    if cmd == "report":
        print(report(n))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
