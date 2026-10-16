#!/usr/bin/env python3
"""
SurfCPU wiring checker
--------------------
File: check-wiring.py Date: 10/15/26 (SurfCPU)
--------------------
Checks that the Arty A7 designs' pins agree everywhere (brief section 4, docs/WIRING.md):

  * every top-level port bit of each design (boards/arty_a7/designs/*.files, its `top` module)
    has exactly one PACKAGE_PIN and an IOSTANDARD in the design's XDC files, and the XDC
    constrains no port the top does not have;
  * no two ports share a pin;
  * docs/WIRING.md has a row for every such port with the same pin, direction (from the
    Verilog), I/O standard and pin options;
  * the row's Digilent name has that pin and I/O standard in both master XDC files
    (boards/arty_a7/digilent/xdc, A7-35 and A7-100), and the pin is in one of the 3.3 V
    banks 14, 15, 16, 35 (Arty A7 schematic Rev. E.0 sheet 7), never the 1.35 V DDR bank 34;
  * no unconstrained I/O: every input is a clock, has -max and -min input delays or a false
    path from it, every output -max and -min output delays or a false path to it (inouts both), and
    every get_ports pattern in the design's XDC files matches one of its ports;
  * the scoped CDC constraints (boards/arty_a7/xdc/cdc) name registers and ports their
    rtl/lib modules really have (Vivado's names: <reg>_reg, <reg>_reg[i] for vectors), and
    the clock-renaming get_pins paths exist in the RTL hierarchy.

    python3 tools/check-wiring.py              check every design (exit 1 on any problem)
    python3 tools/check-wiring.py --self-test  show that each kind of mistake is caught

Only the Python standard library is used.
"""

from __future__ import annotations

import copy
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent            # surfcpu/
BOARD = ROOT / "boards" / "arty_a7"
DESIGNS = BOARD / "designs"
WIRING = ROOT / "docs" / "WIRING.md"
MASTERS = {
    "A7-35": BOARD / "digilent" / "xdc" / "Arty-A7-35-Master.xdc",
    "A7-100": BOARD / "digilent" / "xdc" / "Arty-A7-100-Master.xdc",
}
BANKS_3V3 = {14, 15, 16, 35}                              # VCCO = VCC3V3 (schematic sheet 7)
BANK_DDR = 34                                             # VCCO = DDRVCC, 1.35 V


class WiringError(Exception):
    pass


# ------------------------------------------------------------------------------------ parsing
def design_list(path: Path) -> dict[str, list[list[str]]]:
    items: dict[str, list[list[str]]] = {}
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            kind, *rest = line.split()
            items.setdefault(kind, []).append(rest)
    return items


def expand(pattern: str) -> list[Path]:
    return [Path(p) for p in sorted(glob.glob(str(ROOT / pattern)))]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _matching(text: str, start: int) -> int:
    """Index of the parenthesis closing the one at text[start]."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
    raise WiringError("unbalanced parentheses")


def top_ports(sources: list[Path], top: str) -> dict[str, str]:
    """{port bit: direction} for an ANSI-style module header (literal ranges only)."""
    for src in sources:
        text = strip_comments(src.read_text())
        m = re.search(rf"\bmodule\s+{re.escape(top)}\b", text)
        if not m:
            continue
        i = m.end()
        while text[i].isspace():
            i += 1
        if text[i] == "#":
            i = _matching(text, text.index("(", i)) + 1
        open_ = text.index("(", i)
        body = text[open_ + 1:_matching(text, open_)]
        ports: dict[str, str] = {}
        direction, rng = None, None
        for item in body.split(","):
            item = item.strip()
            if not item or item.startswith("`"):
                continue
            m2 = re.match(r"(input|output|inout)\s+(?:wire\s+|reg\s+)?(\[\s*(\d+)\s*:\s*(\d+)\s*\])?\s*(\w+)$", item)
            if m2:
                direction = m2.group(1)
                rng = (int(m2.group(3)), int(m2.group(4))) if m2.group(2) else None
                name = m2.group(5)
            elif re.match(r"\w+$", item) and direction:
                name = item                         # same declaration as the previous name
            else:
                raise WiringError(f"{src.name}: cannot read port declaration {item!r}")
            dirn = {"input": "in", "output": "out", "inout": "inout"}[direction]
            if rng:
                lo, hi = sorted(rng)
                for b in range(lo, hi + 1):
                    ports[f"{name}[{b}]"] = dirn
            else:
                ports[name] = dirn
        return ports
    raise WiringError(f"module {top} not found in the design's sources")


def norm_port(name: str) -> str:
    return re.sub(r"\s+", "", name)


# [get_ports { name }] (a name may hold brackets: sw[0]) or [get_ports name]
GET_PORTS = r"\[get_ports\s+(?:\{\s*([^}]+?)\s*\}|([^\s\[\]{}]+))\s*\]"


def read_xdc(paths: list[Path]) -> dict[str, list[tuple[str, dict[str, str]]]]:
    """{port: [(file:line, {PROPERTY: value})...]} for every set_property on get_ports."""
    found: dict[str, list[tuple[str, dict[str, str]]]] = {}
    for path in paths:
        for n, raw in enumerate(path.read_text().splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"set_property\s+-dict\s+\{([^}]*)\}\s+" + GET_PORTS, line)
            if m:
                toks = m.group(1).split()
                if len(toks) % 2:
                    raise WiringError(f"{path.name}:{n}: odd -dict list")
                props = {toks[i].upper(): toks[i + 1] for i in range(0, len(toks), 2)}
                where = f"{path.name}:{n}"
                found.setdefault(norm_port(m.group(2) or m.group(3)), []).append((where, props))
                continue
            m = re.match(r"set_property\s+(\w+)\s+(\S+)\s+" + GET_PORTS, line)
            if m:
                found.setdefault(norm_port(m.group(3) or m.group(4)), []).append(
                    (f"{path.name}:{n}", {m.group(1).upper(): m.group(2)}))
    return found


def read_master(path: Path) -> dict[str, tuple[str, str, int]]:
    """{Digilent port name: (pin, iostandard, bank)} from a master XDC's commented lines."""
    out: dict[str, tuple[str, str, int]] = {}
    for line in path.read_text().splitlines():
        m = re.match(r"#\s*set_property\s+-dict\s+\{\s*PACKAGE_PIN\s+(\w+)\s+IOSTANDARD\s+(\w+)\s*\}\s*"
                     r"\[get_ports\s+\{\s*([^}]+?)\s*\}\];\s*#\s*(\S+)", line)
        if m:
            bank = int(re.search(r"_(\d+)$", m.group(4)).group(1))
            out[norm_port(m.group(3))] = (m.group(1), m.group(2), bank)
    if not out:
        raise WiringError(f"{path.name}: no pins found")
    return out


def read_wiring(path: Path) -> dict[str, dict[str, str]]:
    """{port: row} from the tables of WIRING.md whose header names Port and FPGA pin."""
    rows: dict[str, dict[str, str]] = {}
    header: list[str] | None = None
    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line.startswith("|"):
            header = None
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if header is None:
            header = cells
            continue
        if set(cells) <= {"---", ":---", "---:"} or "Port" not in header or "FPGA pin" not in header:
            continue
        row = dict(zip(header, cells))
        port = norm_port(row["Port"].strip("`"))
        if port in rows:
            raise WiringError(f"WIRING.md:{n}: port {port} listed twice")
        row["Master XDC"] = row.get("Master XDC", "").strip("`")
        row["_line"] = str(n)
        rows[port] = row
    return rows


def options_of(props: dict[str, str]) -> set[str]:
    opts = set()
    for k, v in props.items():
        if k in ("PACKAGE_PIN", "IOSTANDARD"):
            continue
        opts.add(k if v.lower() == "true" else f"{k}={v}")
    return opts


def wiring_options(text: str) -> set[str]:
    return set() if text in ("-", "") else {t.upper() for t in text.split()}


def read_timing(paths: list[Path]) -> list[tuple[str, str, list[str], str]]:
    """[(command, role, port patterns, where)] for every timing command naming ports.
    role: 'clock' (create_clock), 'from'/'to' (set_false_path), 'in-max'/'in-min'/'in'
    (set_input_delay), 'out-max'/'out-min'/'out' (set_output_delay), 'pin' (a pin property)."""
    out = []
    for path in paths:
        for n, raw in enumerate(path.read_text().splitlines(), 1):
            line = raw.split(";")[0].strip() if not raw.strip().startswith("#") else ""
            if not line:
                continue
            where = f"{path.name}:{n}"
            words = line.split()
            cmd = words[0]
            for m in re.finditer(r"(-from|-to)?\s*\[get_ports\s+(?:\{([^}]*)\}|([^\s\]]+))\s*\]", line):
                pats = (m.group(2) or m.group(3)).split()
                if cmd == "create_clock":
                    role = "clock"
                elif cmd == "set_false_path":
                    role = (m.group(1) or "").lstrip("-") or "?"
                elif cmd == "set_input_delay":
                    role = "in-max" if "-max" in words else "in-min" if "-min" in words else "in"
                elif cmd == "set_output_delay":
                    role = "out-max" if "-max" in words else "out-min" if "-min" in words else "out"
                elif cmd == "set_property":
                    role = "pin"
                else:
                    role = "other"
                out.append((cmd, role, pats, where))
    return out


def port_match(pattern: str, ports) -> list[str]:
    rx = re.compile("^" + re.escape(pattern).replace("\\*", ".*") + "$")
    return [p for p in ports if rx.match(p)]


def module_regs(module: str) -> tuple[dict[str, bool], set[str]]:
    """({register name: is a vector}, {port names}) of rtl/lib/<module>.v."""
    text = strip_comments((ROOT / "rtl" / "lib" / f"{module}.v").read_text())
    regs = {}
    for m in re.finditer(r"\breg\b\s*(\[[^\]]+\])?\s*([^;=]+)", text):
        for name in re.findall(r"\b([A-Za-z_]\w*)\b", m.group(2).split("=")[0]):
            regs[name] = m.group(1) is not None
    ports = set(re.findall(r"\b(?:input|output|inout)\s+(?:wire|reg)?\s*(?:\[[^\]]+\])?\s*(\w+)", text))
    return regs, ports


def check_timing_coverage(data: dict) -> list[str]:
    errors = []
    for name, d in data["designs"].items():
        ports = d["ports"]
        cover: dict[str, set[str]] = {p: set() for p in ports}
        for cmd, role, pats, where in d["timing"]:
            for pat in pats:
                hits = port_match(pat, ports)
                if not hits:
                    errors.append(f"{name}: {where}: get_ports {pat} matches no port of {d['top']}")
                for h in hits:
                    cover[h].add(role)
        for port, dirn in sorted(ports.items()):
            roles = cover[port]
            ins = bool(roles & {"clock", "from", "in"}) or {"in-max", "in-min"} <= roles
            outs = "to" in roles or "out" in roles or {"out-max", "out-min"} <= roles
            if dirn in ("in", "inout") and not ins:
                errors.append(f"{name}: input {port} has no clock, -max/-min input delay or false path (unconstrained I/O)")
            if dirn in ("out", "inout") and not outs:
                errors.append(f"{name}: output {port} has no -max/-min output delay or false path (unconstrained I/O)")
        # clock renaming: the hierarchy the get_pins paths name must exist
        rtl = "\n".join(strip_comments(sp.read_text()) for sp in d["sources"])
        for xdc in d["xdc_files"]:
            text = (BOARD / "xdc" / xdc).read_text()
            for path in re.findall(r"\[get_pins\s+([\w/]+)\]", text):
                inst = path.split("/")[:-1]
                ok = all(re.search(rf"\b{re.escape(i)}\s*\(", rtl) for i in inst)
                if not ok:
                    errors.append(f"{name}: {xdc}: get_pins {path}: instance path not found in the sources")
        # scoped CDC constraints: registers and ports of the module
        for module, xdc in d["cdc"]:
            regs, mports = module_regs(module)
            text = xdc.read_text()
            for cell in re.findall(r"get_cells\s+(?:-quiet\s+)?\{?([\w\[\]*]+)\}?\]", text):
                m = re.fullmatch(r"(\w+)_reg(\[[^\]]+\])?", cell)
                if not m or m.group(1) not in regs:
                    errors.append(f"{name}: {xdc.name}: cell {cell} is not a register of {module}")
                elif bool(m.group(2)) != regs[m.group(1)]:
                    errors.append(f"{name}: {xdc.name}: cell {cell}: {m.group(1)} is "
                                  f"{'a vector' if regs[m.group(1)] else 'a single bit'} in {module}")
            for port in re.findall(r"get_ports\s+(\w+)\]", text):
                if port not in mports:
                    errors.append(f"{name}: {xdc.name}: {module} has no port {port}")
    return errors


# ------------------------------------------------------------------------------------ checking
def load_all() -> dict:
    designs = {}
    for f in sorted(DESIGNS.glob("*.files")):
        items = design_list(f)
        top = items["top"][0][0]
        srcs = [p for pat in items.get("src", []) for p in expand(pat[0])]
        if not any(re.search(rf"\bmodule\s+{top}\b", strip_comments(s.read_text())) for s in srcs):
            srcs += expand(f"boards/arty_a7/rtl/{top}.v")
        xdcs = [p for pat in items.get("xdc", []) for p in expand(pat[0])]
        designs[f.stem] = {"top": top, "ports": top_ports(srcs, top), "xdc": read_xdc(xdcs),
                           "xdc_files": [x.name for x in xdcs], "timing": read_timing(xdcs),
                           "cdc": [(m, ROOT / x) for m, x in items.get("cdc", [])],
                           "sources": srcs}
    return {"designs": designs, "wiring": read_wiring(WIRING),
            "masters": {k: read_master(v) for k, v in MASTERS.items()}}


def check(data: dict) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []
    wiring, masters = data["wiring"], data["masters"]
    used_ports: set[str] = set()
    for name, d in data["designs"].items():
        ports, xdc = d["ports"], d["xdc"]
        pins: dict[str, str] = {}
        for port, dirn in sorted(ports.items()):
            used_ports.add(port)
            entries = xdc.get(port, [])
            pin_entries = [(w, p["PACKAGE_PIN"]) for w, p in entries if "PACKAGE_PIN" in p]
            if len(pin_entries) != 1:
                errors.append(f"{name}: port {port} has {len(pin_entries)} PACKAGE_PIN constraints "
                              f"({', '.join(w for w, _ in pin_entries) or 'none'})")
                continue
            pin = pin_entries[0][1]
            props: dict[str, str] = {}
            for _, p in entries:
                props.update(p)
            iostd = props.get("IOSTANDARD")
            if pin in pins:
                errors.append(f"{name}: pin {pin} used by both {pins[pin]} and {port}")
            pins[pin] = port
            if not iostd:
                errors.append(f"{name}: port {port} has no IOSTANDARD")
            row = wiring.get(port)
            if not row:
                errors.append(f"{name}: port {port} is not in WIRING.md")
                continue
            where = f"WIRING.md:{row['_line']}"
            if row["FPGA pin"] != pin:
                errors.append(f"{name}: {port} on {pin} in the XDC but {row['FPGA pin']} in {where}")
            if row["Dir"] != dirn:
                errors.append(f"{name}: {port} is '{dirn}' in the Verilog but '{row['Dir']}' in {where}")
            if row["I/O standard"] != iostd:
                errors.append(f"{name}: {port} is {iostd} in the XDC but {row['I/O standard']} in {where}")
            if wiring_options(row.get("Options", "-")) != options_of(props):
                errors.append(f"{name}: {port} options {sorted(options_of(props))} in the XDC but "
                              f"{row.get('Options')} in {where}")
            for board, master in masters.items():
                m = master.get(row["Master XDC"])
                if not m:
                    errors.append(f"{name}: {port}: {row['Master XDC']!r} ({where}) is not in the {board} master XDC")
                    continue
                mpin, mstd, bank = m
                if mpin != pin:
                    errors.append(f"{name}: {port} on {pin}, but {board} master XDC has {row['Master XDC']} on {mpin}")
                if mstd != iostd:
                    errors.append(f"{name}: {port} is {iostd}, but the {board} master XDC says {mstd}")
                if bank == BANK_DDR or bank not in BANKS_3V3:
                    errors.append(f"{name}: {port} on {pin} is in bank {bank}, not a 3.3 V bank")
        for port in sorted(set(xdc) - set(ports)):
            errors.append(f"{name}: the XDC constrains {port}, which {d['top']} does not have "
                          f"({', '.join(w for w, _ in xdc[port])})")
        notes.append(f"{name}: {len(ports)} port bits on {len(pins)} pins ({', '.join(d['xdc_files'])})")
    for port, row in sorted(wiring.items()):
        if port not in used_ports:
            errors.append(f"WIRING.md:{row['_line']}: {port} is not a port of any design")
    errors += check_timing_coverage(data)
    a, b = masters["A7-35"], masters["A7-100"]
    for key in sorted(set(a) | set(b)):
        if a.get(key, (None,))[0] != b.get(key, (None,))[0]:
            errors.append(f"master XDC files differ on {key}: {a.get(key)} vs {b.get(key)}")
    return errors, notes


# ---------------------------------------------------------------------------------- self-test
def self_test() -> int:
    """Each mutation of the real data must produce at least one error mentioning the key word."""
    base = load_all()
    errs, _ = check(base)
    if errs:
        print("check-wiring: the real data has errors; fix those first")
        return 1
    d0 = "surfcpu_arty_a7_top"

    def m_pin(data):          # wrong pin in the XDC
        data["designs"][d0]["xdc"]["sd_sclk"][0][1]["PACKAGE_PIN"] = "D13"

    def m_dup(data):          # a port constrained twice
        x = data["designs"][d0]["xdc"]["vga_hs"]
        x.append(("extra.xdc:1", {"PACKAGE_PIN": "U14"}))

    def m_missing(data):      # a port without a pin
        del data["designs"][d0]["xdc"]["vga_vs"]

    def m_extra(data):        # a constraint on a port the top lacks
        data["designs"][d0]["xdc"]["vga_x"] = [("extra.xdc:2", {"PACKAGE_PIN": "T13", "IOSTANDARD": "LVCMOS33"})]

    def m_shared(data):       # two ports on one pin
        data["designs"][d0]["xdc"]["led[1]"][0][1]["PACKAGE_PIN"] = "H5"
        data["wiring"]["led[1]"]["FPGA pin"] = "H5"

    def m_wiring(data):       # WIRING.md disagrees
        data["wiring"]["uart_tx"]["FPGA pin"] = "A9"

    def m_dir(data):          # direction differs
        data["wiring"]["sd_miso"]["Dir"] = "out"

    def m_std(data):          # I/O standard
        data["designs"][d0]["xdc"]["btn[0]"][0][1]["IOSTANDARD"] = "LVCMOS18"

    def m_opt(data):          # pin option missing
        data["designs"][d0]["xdc"]["sd_cd_n"][0][1].pop("PULLUP")

    def m_master(data):       # the Digilent name points elsewhere
        data["wiring"]["vga_g[0]"]["Master XDC"] = "jc[1]"

    def m_bank(data):         # a pin in the DDR3 bank
        for master in data["masters"].values():
            p, s, _ = master["ja[3]"]
            master["ja[3]"] = (p, s, 34)

    def m_variant(data):      # the two board variants disagree
        p, s, b = data["masters"]["A7-35"]["jd[2]"]
        data["masters"]["A7-35"]["jd[2]"] = ("H2", s, b)

    def m_unlisted(data):     # a WIRING.md row for no port
        row = dict(data["wiring"]["sw[0]"])
        data["wiring"]["sw[9]"] = row

    def m_nofp(data):         # an input without any timing constraint
        t = data["designs"][d0]["timing"]
        data["designs"][d0]["timing"] = [x for x in t if not (x[1] == "from" and "sd_cd_n" in " ".join(x[2]))]
        data["designs"][d0]["timing"].append(("set_false_path", "from", ["sd_dat1", "sd_dat2"], "t.xdc:1"))

    def m_noimin(data):       # an input with only a -max input delay
        t = data["designs"][d0]["timing"]
        data["designs"][d0]["timing"] = [x for x in t if x[1] != "in-min"]

    def m_noomin(data):       # an output with only a -max output delay
        t = data["designs"][d0]["timing"]
        data["designs"][d0]["timing"] = [x for x in t if x[1] != "out-min"]

    def m_dangling(data):     # a constraint on a port pattern that matches nothing
        data["designs"][d0]["timing"].append(("set_false_path", "from", ["vga_q[*]"], "t.xdc:2"))

    def m_cell(data):         # a scoped CDC file naming a register the module lacks
        import tempfile
        tmp = Path(tempfile.mkdtemp()) / "surf_pulse_sync.xdc"
        tmp.write_text("set_max_delay -datapath_only -from [get_cells src_toggel_reg] "
                       "-to [get_cells {dst_sync_reg[0]}] 40\n")
        data["designs"][d0]["cdc"] = [(m, tmp if m == "surf_pulse_sync" else x)
                                      for m, x in data["designs"][d0]["cdc"]]

    cases = [("wrong pin", m_pin, "sd_sclk"), ("constrained twice", m_dup, "vga_hs"),
             ("no pin", m_missing, "vga_vs"), ("unknown port", m_extra, "vga_x"),
             ("shared pin", m_shared, "H5"), ("WIRING.md pin", m_wiring, "uart_tx"),
             ("direction", m_dir, "sd_miso"), ("I/O standard", m_std, "LVCMOS18"),
             ("options", m_opt, "sd_cd_n"), ("Digilent name", m_master, "vga_g[0]"),
             ("DDR bank", m_bank, "bank 34"), ("35T/100T", m_variant, "jd[2]"),
             ("stray row", m_unlisted, "sw[9]"), ("no false path", m_nofp, "sd_cd_n has no clock"),
             ("only -max input delay", m_noimin, "sd_miso has no clock"),
             ("only -max delay", m_noomin, "output delay or false path"),
             ("dangling get_ports", m_dangling, "vga_q[*] matches no port"),
             ("CDC cell name", m_cell, "src_toggel_reg")]
    failed = 0
    for label, mutate, word in cases:
        data = copy.deepcopy(base)
        mutate(data)
        errs, _ = check(data)
        ok = any(word in e for e in errs)
        print(f"check-wiring self-test: {label:18s} {'caught' if ok else 'MISSED'}"
              f"{': ' + errs[0] if ok and errs else ''}")
        failed += not ok
    print(f"check-wiring self-test: {'PASS' if not failed else 'FAIL'} ({len(cases) - failed}/{len(cases)})")
    return 1 if failed else 0


def main(argv: list[str]) -> int:
    try:
        if "--self-test" in argv:
            return self_test()
        errors, notes = check(load_all())
    except (WiringError, OSError, KeyError) as e:
        print(f"check-wiring: {e}", file=sys.stderr)
        return 1
    for n in notes:
        print(f"check-wiring: {n}")
    for e in errors:
        print(f"check-wiring: ERROR: {e}")
    if errors:
        return 1
    print("check-wiring: every port has one pin, matching docs/WIRING.md and both master XDC files;")
    print("check-wiring: every port has a timing constraint; the CDC and clock constraints name real cells")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
