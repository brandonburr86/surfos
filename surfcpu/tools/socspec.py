#!/usr/bin/env python3
"""SurfCPU SoC description: load, check and generate (spec/soc.toml is the one source).

    python3 tools/socspec.py gen      write the generated files
    python3 tools/socspec.py check    exit 1 if a generated file is missing or out of date
    python3 tools/socspec.py dump     print the checked description as JSON

Other tools import it:  from socspec import load;  spec = load()  (spec.regions['uart'].base,
spec.periphs['uart'].regs['STATUS'].offset, spec.bootabi['image_magic'], ...).
Only the Python standard library is used (tomllib needs Python 3.11 or later).
"""

from __future__ import annotations

import dataclasses
import json
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # surfcpu/
SPEC = ROOT / "spec" / "soc.toml"

OUT_VERILOG = ROOT / "rtl" / "generated" / "surfcpu_soc.vh"
OUT_C = ROOT / "firmware" / "include" / "surfcpu" / "soc.h"
OUT_LD = ROOT / "firmware" / "ld" / "surfcpu_soc.ld"
OUT_DOC = ROOT / "docs" / "MEMORY-MAP.md"

ACCESS = {"ro", "rw", "wo", "w1c", "rc"}
KINDS = {"rom", "ram", "mmio", "ddr", "reserved"}
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


class SpecError(Exception):
    pass


@dataclasses.dataclass
class Field:
    name: str
    msb: int
    lsb: int
    desc: str

    @property
    def width(self) -> int:
        return self.msb - self.lsb + 1

    @property
    def mask(self) -> int:
        return ((1 << self.width) - 1) << self.lsb


@dataclasses.dataclass
class Reg:
    name: str
    offset: int
    access: str
    desc: str
    count: int
    fields: list[Field]


@dataclasses.dataclass
class Periph:
    name: str
    desc: str
    irq: str | None
    regs: dict[str, Reg]


@dataclasses.dataclass
class Region:
    name: str
    base: int
    size: int
    kind: str
    fetch: bool
    write: bool
    desc: str
    bytes: int | None = None        # memory present from base (the rest of the window: errors)

    @property
    def end(self) -> int:
        return self.base + self.size

    @property
    def length(self) -> int:
        return self.bytes if self.bytes is not None else self.size


@dataclasses.dataclass
class Reserve:
    name: str
    base: int
    size: int
    desc: str


@dataclasses.dataclass
class Hole:
    name: str
    base: int
    size: int
    desc: str


@dataclasses.dataclass
class Spec:
    soc: dict
    clocks: dict
    boards: dict
    regions: dict[str, Region]
    reserves: dict[str, Reserve]
    irqs: dict[str, int]
    periphs: dict[str, Periph]
    bootabi: dict
    holes: dict[str, Hole] = dataclasses.field(default_factory=dict)

    @property
    def soc_version_word(self) -> int:
        major, minor, patch = (int(x) for x in self.soc["soc_version"].split("."))
        return major << 16 | minor << 8 | patch


def _bits(text: str, where: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+)(?::(\d+))?", str(text))
    if not m:
        raise SpecError(f"{where}: bits must be 'msb:lsb' or 'n', not {text!r}")
    msb = int(m.group(1))
    lsb = int(m.group(2)) if m.group(2) is not None else msb
    if not 0 <= lsb <= msb <= 31:
        raise SpecError(f"{where}: bits {text!r} out of 0..31 or reversed")
    return msb, lsb


def _name(n: str, where: str) -> str:
    if not NAME_RE.match(n):
        raise SpecError(f"{where}: bad name {n!r}")
    return n


def _pow2(n: int) -> bool:
    return n > 0 and n & (n - 1) == 0


def load(path: Path = SPEC) -> Spec:
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    regions: dict[str, Region] = {}
    for r in raw.get("region", []):
        reg = Region(_name(r["name"], "region"), int(r["base"]), int(r["size"]), r["kind"],
                     bool(r["fetch"]), bool(r["write"]), r.get("desc", ""),
                     int(r["bytes"]) if "bytes" in r else None)
        w = f"region {reg.name}"
        if reg.name in regions:
            raise SpecError(f"{w}: defined twice")
        if reg.bytes is not None and (reg.kind not in ("rom", "ram") or not 0 < reg.bytes <= reg.size
                                      or reg.bytes % 4):
            raise SpecError(f"{w}: bytes must be a whole number of words inside a rom or ram window")
        if reg.kind not in KINDS:
            raise SpecError(f"{w}: kind {reg.kind!r} not one of {sorted(KINDS)}")
        if not _pow2(reg.size) or reg.base % reg.size:
            raise SpecError(f"{w}: size must be a power of two and base aligned to it (simple decode)")
        if reg.end > 1 << 32:
            raise SpecError(f"{w}: runs past 4 GiB")
        for o in regions.values():
            if reg.base < o.end and o.base < reg.end:
                raise SpecError(f"{w}: overlaps region {o.name}")
        regions[reg.name] = reg

    reserves: dict[str, Reserve] = {}
    for r in raw.get("reserve", []):
        res = Reserve(_name(r["name"], "reserve"), int(r["base"]), int(r["size"]), r.get("desc", ""))
        w = f"reserve {res.name}"
        if res.name in reserves or res.name in regions:
            raise SpecError(f"{w}: name already used")
        inside = [g for g in regions.values() if g.base <= res.base and res.base + res.size <= g.end]
        if not inside:
            raise SpecError(f"{w}: not inside one region")
        for o in reserves.values():
            if res.base < o.base + o.size and o.base < res.base + res.size:
                raise SpecError(f"{w}: overlaps reserve {o.name}")
        reserves[res.name] = res

    holes: dict[str, Hole] = {}
    for h in raw.get("hole", []):
        hole = Hole(_name(h["name"], "hole"), int(h["base"]), int(h["size"]), h.get("desc", ""))
        w = f"hole {hole.name}"
        if hole.name in holes or hole.name in regions or hole.name in reserves:
            raise SpecError(f"{w}: name already used")
        if hole.size <= 0 or hole.base + hole.size > 1 << 32:
            raise SpecError(f"{w}: empty or past 4 GiB")
        for o in list(regions.values()) + list(holes.values()):
            if hole.base < o.base + o.size and o.base < hole.base + hole.size:
                raise SpecError(f"{w}: overlaps {'region' if o.name in regions else 'hole'} {o.name}")
        holes[hole.name] = hole

    irqs = {_name(k, "irqs"): int(v) for k, v in raw.get("irqs", {}).items()}
    if len(set(irqs.values())) != len(irqs) or any(not 0 <= v < 32 for v in irqs.values()):
        raise SpecError("irqs: numbers must be unique and 0..31")

    periphs: dict[str, Periph] = {}
    for p in raw.get("periph", []):
        name = _name(p["name"], "periph")
        if name not in regions or regions[name].kind != "mmio":
            raise SpecError(f"periph {name}: needs an mmio region of the same name")
        irq = p.get("irq")
        if irq is not None and irq not in irqs:
            raise SpecError(f"periph {name}: irq {irq!r} not in [irqs]")
        regs: dict[str, Reg] = {}
        used: dict[int, str] = {}
        for r in p.get("reg", []):
            w = f"{name}.{r['name']}"
            count = int(r.get("count", 1))
            reg = Reg(_name(r["name"], w), int(r["offset"]), r["access"], r.get("desc", ""), count, [])
            if reg.access not in ACCESS:
                raise SpecError(f"{w}: access {reg.access!r} not one of {sorted(ACCESS)}")
            if reg.offset % 4 or reg.offset + 4 * count > regions[name].size:
                raise SpecError(f"{w}: offset must be word aligned and inside the window")
            for i in range(count):
                off = reg.offset + 4 * i
                if off in used:
                    raise SpecError(f"{w}: offset {off:#x} already used by {used[off]}")
                used[off] = reg.name
            taken = 0
            for fd in r.get("fields", []):
                msb, lsb = _bits(fd["bits"], f"{w}.{fd['name']}")
                fld = Field(_name(fd["name"], w), msb, lsb, fd.get("desc", ""))
                if taken & fld.mask:
                    raise SpecError(f"{w}.{fld.name}: overlaps another field")
                if any(f.name == fld.name for f in reg.fields):
                    raise SpecError(f"{w}.{fld.name}: defined twice")
                taken |= fld.mask
                reg.fields.append(fld)
            if reg.name in regs:
                raise SpecError(f"{w}: defined twice")
            regs[reg.name] = reg
        periphs[name] = Periph(name, p.get("desc", ""), irq, regs)

    clocks = dict(raw["clocks"])
    if clocks["sys_hz"] % clocks["timebase_hz"]:
        raise SpecError("clocks: sys_hz must be a whole multiple of timebase_hz")
    soc = dict(raw["soc"])
    boards = {_name(k, "boards"): int(v) for k, v in raw["boards"].items()}
    spec = Spec(soc, clocks, boards, regions, reserves, irqs, periphs, dict(raw["bootabi"]), holes)
    spec.soc_version_word  # validates the version string
    return spec


# ------------------------------------------------------------------------------------------------
# generators

HEADER_LINES = [
    "Generated from spec/soc.toml by tools/socspec.py gen -- do not edit.",
    "SurfCPU memory map version {map}, SoC version {ver}.",
]


def _hdr(spec: Spec, open_: str, line: str, close: str) -> list[str]:
    text = [h.format(map=spec.soc["map_version"], ver=spec.soc["soc_version"]) for h in HEADER_LINES]
    if open_:
        return [open_] + [f"{line}{t}" for t in text] + [close, ""]
    return [f"{line}{t}" for t in text] + [""]


def _consts(spec: Spec) -> list[tuple[str, int, str, str]]:
    """(name, value, kind, comment): kind is 'addr' (32-bit), 'num' (integer) or 'bit' (position)."""
    out: list[tuple[str, int, str, str]] = []
    out.append(("SURF_MAP_VERSION", spec.soc["map_version"], "num", "memory map version"))
    out.append(("SURF_SOC_VERSION", spec.soc_version_word, "addr", f"SoC version {spec.soc['soc_version']}"))
    out.append(("SURF_RESET_PC", spec.soc["reset_pc"], "addr", "reset vector"))
    out.append(("SURF_HART_ID", spec.soc["hart_id"], "num", "mhartid"))
    for k, v in spec.clocks.items():
        out.append((f"SURF_{k.upper()}", v, "num", "Hz"))
    for k, v in spec.boards.items():
        out.append((f"SURF_BOARD_{k.upper()}", v, "addr", "sysinfo.BOARD_ID"))
    for r in spec.regions.values():
        n = r.name.upper()
        out.append((f"SURF_{n}_BASE", r.base, "addr", r.desc))
        out.append((f"SURF_{n}_SIZE", r.size, "addr", ""))
        out.append((f"SURF_{n}_MASK", r.size - 1, "addr", "address bits inside the region"))
        if r.bytes is not None:
            out.append((f"SURF_{n}_BYTES", r.bytes, "addr", "bytes present from the base; above, errors"))
    for r in spec.reserves.values():
        n = r.name.upper()
        out.append((f"SURF_{n}_BASE", r.base, "addr", r.desc))
        out.append((f"SURF_{n}_SIZE", r.size, "addr", ""))
    for h in spec.holes.values():
        n = h.name.upper()
        out.append((f"SURF_{n}_BASE", h.base, "addr", h.desc))
        out.append((f"SURF_{n}_SIZE", h.size, "addr", "never mapped"))
    for k, v in spec.irqs.items():
        out.append((f"SURF_IRQ_{k.upper()}", v, "num", "interrupt controller source"))
    out.append(("SURF_IRQ_COUNT", max(spec.irqs.values()) + 1, "num", "sources in use"))
    for p in spec.periphs.values():
        for r in p.regs.values():
            base = f"SURF_{p.name.upper()}_{r.name}"
            out.append((base, r.offset, "addr", f"{r.access}: {r.desc}"))
            if r.count > 1:
                out.append((f"{base}_COUNT", r.count, "num", "consecutive words"))
            for f in r.fields:
                fb = f"{base}_{f.name}"
                out.append((f"{fb}_LSB", f.lsb, "bit", f.desc))
                out.append((f"{fb}_WIDTH", f.width, "num", ""))
                out.append((f"{fb}_MASK", f.mask, "addr", "in place"))
    for k, v in spec.bootabi.items():
        out.append((f"SURF_{k.upper()}", int(v), "addr", "boot ABI"))
    names = [c[0] for c in out]
    dup = {n for n in names if names.count(n) > 1}
    if dup:
        raise SpecError(f"generated names clash: {sorted(dup)}")
    return out


def gen_verilog(spec: Spec) -> str:
    lines = _hdr(spec, "", "// ", "")
    lines += [
        "// `include this file once inside each module body that needs it (it holds localparams,",
        "// so it has no include guard). Addresses and masks are [31:0]; bit positions and counts",
        "// are integers.",
        "",
        "/* verilator lint_off UNUSEDPARAM */",
    ]
    for name, value, kind, comment in _consts(spec):
        c = f"  // {comment}" if comment else ""
        if kind == "addr":
            lines.append(f"localparam [31:0] {name} = 32'h{value >> 16:04X}_{value & 0xFFFF:04X};{c}")
        else:
            lines.append(f"localparam integer {name} = {value};{c}")
    lines.append("/* verilator lint_on UNUSEDPARAM */")
    return "\n".join(lines) + "\n"


def gen_c(spec: Spec) -> str:
    lines = _hdr(spec, "/*", " * ", " */")
    lines += [
        "#ifndef SURFCPU_SOC_H",
        "#define SURFCPU_SOC_H",
        "",
        "/* Register access: SURF_REG(SURF_UART_BASE, SURF_UART_STATUS). */",
        "#define SURF_REG(base, off) (*(volatile unsigned int *)((base) + (off)))",
        "",
    ]
    for name, value, kind, comment in _consts(spec):
        c = f" /* {comment} */" if comment and "*/" not in comment else ""
        if kind == "addr":
            lines.append(f"#define {name} 0x{value:08X}u{c}")
        else:
            lines.append(f"#define {name} {value}{c}")
    lines += ["", "#endif /* SURFCPU_SOC_H */"]
    return "\n".join(lines) + "\n"


def gen_ld(spec: Spec) -> str:
    lines = _hdr(spec, "/*", " * ", " */")
    lines.append("/* Symbols for every region, reservation and hole; INCLUDE this before SECTIONS. */")
    for r in list(spec.regions.values()) + list(spec.reserves.values()) + list(spec.holes.values()):
        n = r.name.upper()
        lines.append(f"SURF_{n}_BASE = 0x{r.base:08X};")
        lines.append(f"SURF_{n}_SIZE = 0x{r.size:08X};")
    lines += ["", "/* a memory region is as long as the memory in it (bytes), not its decoded window */",
              "MEMORY", "{"]
    for r in spec.regions.values():
        if r.kind in ("mmio", "reserved"):
            continue
        attrs = "r" + ("w" if r.write else "") + ("x" if r.fetch else "")
        lines.append(f"  {r.name:<8} ({attrs:<3}) : ORIGIN = 0x{r.base:08X}, LENGTH = 0x{r.length:08X}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def _hex(v: int) -> str:
    return f"`0x{v >> 16:04X}_{v & 0xFFFF:04X}`"


def gen_doc(spec: Spec) -> str:
    s = spec
    out = [
        "# SurfCPU memory map",
        "",
        f"<!-- Generated from spec/soc.toml by tools/socspec.py gen -- do not edit. -->",
        "",
        f"Memory map version **{s.soc['map_version']}**, SoC version **{s.soc['soc_version']}**, "
        f"ISA `{s.soc['isa']}`, ABI `{s.soc['abi']}`, reset PC {_hex(s.soc['reset_pc'])}, "
        f"one hart (mhartid {s.soc['hart_id']}). Little-endian. The source of truth is "
        "[`spec/soc.toml`](../spec/soc.toml); the Verilog, C and linker definitions are generated "
        "from it ([`rtl/generated/surfcpu_soc.vh`](../rtl/generated/surfcpu_soc.vh), "
        "[`firmware/include/surfcpu/soc.h`](../firmware/include/surfcpu/soc.h), "
        "[`firmware/ld/surfcpu_soc.ld`](../firmware/ld/surfcpu_soc.ld)).",
        "",
        "Unmapped addresses, fetches from regions without *fetch*, and stores to regions without "
        "*write* get a SurfBus error, which the CPU raises as an access fault "
        "([SURFBUS.md](SURFBUS.md)). Inside an MMIO window, an offset with no register also "
        "answers with an error. Registers are 32-bit and are accessed as aligned words.",
        "",
        "## Clocks",
        "",
        "| Clock | Hz |",
        "|---|---|",
    ]
    for k, v in s.clocks.items():
        out.append(f"| `{k}` | {v:,} |")
    out += ["", "## Regions", "", "| Base | End | Size | Name | Kind | Fetch | Write | Purpose |",
            "|---|---|---|---|---|---|---|---|"]
    for r in sorted(s.regions.values(), key=lambda r: r.base):
        size = f"{r.size // 1024} KiB" if r.size < 1 << 20 else f"{r.size >> 20} MiB"
        if r.bytes is not None:
            size += f" ({r.bytes // 1024} KiB present)" if r.bytes % 1024 == 0 else f" ({r.bytes} bytes present)"
        out.append(f"| {_hex(r.base)} | {_hex(r.end - 1)} | {size} | `{r.name}` | {r.kind} | "
                   f"{'yes' if r.fetch else 'no'} | {'yes' if r.write else 'no'} | {r.desc} |")
    out += ["", "## Software reservations", "",
            "Not decoded by hardware; SurfBoot and the kernel keep to them.", "",
            "| Base | Size | Name | Purpose |", "|---|---|---|---|"]
    for r in sorted(s.reserves.values(), key=lambda r: r.base):
        size = f"{r.size // 1024} KiB" if r.size < 1 << 20 else f"{r.size >> 20} MiB"
        out.append(f"| {_hex(r.base)} | {size} | `{r.name}` | {r.desc} |")
    if s.holes:
        out += ["", "## Never mapped", "",
                "Address ranges no SurfCPU SoC decodes, in any design or later memory map: every access "
                "is an error, so tests probe them for access faults.", "",
                "| Base | Size | Name | Purpose |", "|---|---|---|---|"]
        for h in sorted(s.holes.values(), key=lambda h: h.base):
            size = f"{h.size // 1024} KiB" if h.size < 1 << 20 else f"{h.size >> 20} MiB"
            out.append(f"| {_hex(h.base)} | {size} | `{h.name}` | {h.desc} |")
    out += ["", "## Interrupts", "",
            "`mip.MTIP` comes from the timer (mtime >= mtimecmp), `mip.MSIP` from timer.MSIP, and "
            "`mip.MEIP` from the interrupt controller, which ORs these level sources:", "",
            "| Source | Bit |", "|---|---|"]
    for k, v in sorted(s.irqs.items(), key=lambda kv: kv[1]):
        out.append(f"| {k} | {v} |")
    out += ["", "## Boards", "", "| Board | sysinfo.BOARD_ID |", "|---|---|"]
    for k, v in s.boards.items():
        out.append(f"| {k} | {_hex(v)} |")
    out += ["", "## Registers", "",
            "Access: ro read only, rw read/write, wo write only (reads 0), w1c write 1 to clear, "
            "rc reading has a side effect (pops a FIFO). Bits not listed read 0 and ignore writes.", ""]
    for p in s.periphs.values():
        r0 = s.regions[p.name]
        irq = f" Interrupt source {s.irqs[p.irq]} ({p.irq})." if p.irq else ""
        out += [f"### {p.name} at {_hex(r0.base)}", "", p.desc + irq, "",
                "| Offset | Register | Access | Fields | Description |", "|---|---|---|---|---|"]
        for r in p.regs.values():
            off = f"`0x{r.offset:03X}`" if r.count == 1 else \
                f"`0x{r.offset:03X}`..`0x{r.offset + 4 * (r.count - 1):03X}`"
            flds = "<br>".join(
                (f"{f.msb}:{f.lsb}" if f.width > 1 else f"{f.lsb}") + f" `{f.name}`"
                + (f" {f.desc}" if f.desc else "") for f in r.fields) or "31:0"
            out.append(f"| {off} | `{r.name}` | {r.access} | {flds} | {r.desc} |")
        out.append("")
    out += ["## Boot ABI constants", "", "Used by [BOOT-ABI.md](BOOT-ABI.md).", "",
            "| Name | Value |", "|---|---|"]
    for k, v in s.bootabi.items():
        out.append(f"| `SURF_{k.upper()}` | {_hex(int(v))} |")
    return "\n".join(out) + "\n"


OUTPUTS = [(OUT_VERILOG, gen_verilog), (OUT_C, gen_c), (OUT_LD, gen_ld), (OUT_DOC, gen_doc)]


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else "check"
    try:
        spec = load()
    except (SpecError, KeyError, ValueError) as e:
        print(f"socspec: {SPEC}: {e}", file=sys.stderr)
        return 1
    if cmd == "dump":
        print(json.dumps(dataclasses.asdict(spec), indent=2, default=str))
        return 0
    stale = []
    for path, fn in OUTPUTS:
        text = fn(spec)
        old = path.read_text() if path.exists() else None
        if cmd == "gen":
            if old != text:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
                print(f"socspec: wrote {path.relative_to(ROOT)}")
        elif old != text:
            stale.append(str(path.relative_to(ROOT)))
    if cmd == "check":
        if stale:
            print("socspec: out of date (run `make gen`): " + ", ".join(stale), file=sys.stderr)
            return 1
        print("socspec: generated files match spec/soc.toml")
    elif cmd != "gen":
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
