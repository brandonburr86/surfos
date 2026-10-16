#!/usr/bin/env python3
"""SurfCPU bus-script compiler
--------------------
File: surfscript.py Date: 10/15/26 (SurfCPU)
--------------------
Compiles a bus script (sim/soc/scripts/*.sbs) into the hex file the scripted core stand-in
runs (sim/soc/bfm/surf_script_port.v, loaded with +script=<file>).

    python3 sim/soc/tools/surfscript.py IN.sbs -o OUT.hex [--rom ROM.mem] [--seed N]
            [--define NAME=VALUE ...] [--listing OUT.lst]

A script is one operation per line; '#' starts a comment. Operations go to the data port (D,
the default) or the instruction port (I); both ports run at the same time and meet at `sync`.

  D read   ADDR [expect V | expect mem | expect rom] [mask M] | error | auto
  D read8  ADDR ...   D read16 ADDR ...     byte / halfword: lanes from the address, V is the
                                            byte or halfword value
  D write  ADDR V [error | auto]    D write8 ADDR V ...   D write16 ADDR V ...
  D rawread ADDR be=BBBB [expect V] [mask M] | error   D rawwrite ADDR be=BBBB V [error]
  D poll   ADDR mask M equal V [within N] [every K]    word reads until (data & M) == V
  I fetch  ADDR [expect V | expect mem | expect rom] [mask M] | error | auto
  idle N | waitirq NAME=0|1 ... [within N] | checkirq NAME=0|1 ...   (either port; NAME is
           ext, timer or soft) | mtime-near N (the core's mtime input minus the last value read
           is at most N)
  sync                          both ports wait for each other
  assume ddr=0|1                DDR calibrated (1) or not from here on (put it after a sync)
  stall ADDR                    the DDR model must hang on accesses to ADDR (its watchdog then
                                answers with an error); written to OUT.args as +ddr_stall=, the
                                plusargs the bench passes to the simulation
  if NAME | if !NAME | else | endif     NAME from --define (true when not 0)
  D random-rw REGION COUNT [span BYTES] [offset BYTES]    random byte/halfword/word writes and
                                checked reads against the compiler's model of the memory
  I random-fetch REGION COUNT [span BYTES] [offset BYTES]   checked fetches (model or ROM)
  D random-holes COUNT | I random-holes COUNT    accesses to addresses in no region: errors
  D mmio-rules PERIPH           every register with every non-word byte enable, and offsets
                                with no register: all errors, without side effects

ADDR, V, M are expressions over numbers and names from spec/soc.toml: a region (`scratch`,
its base) with .base .size .end .last; a register (`uart.STATUS`, its address) with .off; a
field (`uart.STATUS.RX_AVAIL`, its mask in place) with .lsb; clocks.X, soc.map_version,
soc.soc_version, boards.X, irqs.X, bootabi.X, reserves (`ddr_top`, like regions) and the
--define names (where the memory map has no such name: `ddr` stays the region); with
+ - * << >> & | ^ ~ and parentheses. `auto` decides OK or error from the memory map rules (and
the value from the model for memories); `expect mem` and `expect rom` take the value from the
model (scratch, text RAM and DDR start at zero) or the ROM image. Concurrent ports must not
race: an instruction-port fetch of a word the data port wrote since the last sync is refused.
"""

from __future__ import annotations

import argparse
import ast
import random
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]          # surfcpu/
sys.path.insert(0, str(ROOT / "tools"))
from socspec import load                             # noqa: E402

MAGIC = 0x53425331
PROG_WORDS = 1 << 18                                 # surf_script_port's program memory
OP = {"read": 1, "write": 2, "poll": 3, "idle": 4, "sync": 5, "waitirq": 6, "checkirq": 7,
      "mtime": 8, "end": 9}
IRQ_BIT = {"ext": 0, "timer": 1, "soft": 2}
FLAG_ERR = 1
MASK32 = 0xFFFF_FFFF


class ScriptError(Exception):
    pass


# ---------------------------------------------------------------------------------------- names
class Obj:
    """A name in expressions: an integer value with attributes."""

    def __init__(self, value: int, **attrs):
        self.value = value
        self.attrs = attrs


def build_names(spec) -> dict:
    names = {}
    for r in list(spec.regions.values()) + list(spec.reserves.values()):
        names[r.name] = Obj(r.base, base=r.base, size=r.size, end=r.base + r.size,
                            last=r.base + r.size - 4)
    for p in spec.periphs.values():
        region = spec.regions[p.name]
        regs = {}
        for reg in p.regs.values():
            fields = {f.name: Obj(f.mask, lsb=f.lsb, mask=f.mask, width=f.width)
                      for f in reg.fields}
            regs[reg.name] = Obj(region.base + reg.offset, off=reg.offset, count=reg.count,
                                 **fields)
        base = names[p.name]
        base.attrs.update(regs)
    names["clocks"] = Obj(0, **{k: v for k, v in spec.clocks.items()})
    names["soc"] = Obj(0, map_version=spec.soc["map_version"], soc_version=spec.soc_version_word)
    names["boards"] = Obj(0, **dict(spec.boards))
    names["irqs"] = Obj(0, **dict(spec.irqs))
    names["bootabi"] = Obj(0, **dict(spec.bootabi))
    return names


def evaluate(text: str, names: dict) -> int:
    try:
        tree = ast.parse(text, mode="eval")
    except SyntaxError as e:
        raise ScriptError(f"bad expression {text!r}") from e

    def val(node):
        if isinstance(node, ast.Expression):
            return val(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name):
            if node.id not in names:
                raise ScriptError(f"unknown name {node.id!r} in {text!r}")
            return names[node.id]
        if isinstance(node, ast.Attribute):
            obj = val(node.value)
            if not isinstance(obj, Obj) or node.attr not in obj.attrs:
                raise ScriptError(f"unknown name .{node.attr} in {text!r}")
            return obj.attrs[node.attr]
        if isinstance(node, ast.UnaryOp):
            v = num(node.operand)
            if isinstance(node.op, ast.Invert):
                return ~v & MASK32
            if isinstance(node.op, ast.USub):
                return -v
            if isinstance(node.op, ast.UAdd):
                return v
        if isinstance(node, ast.BinOp):
            a, b = num(node.left), num(node.right)
            ops = {ast.Add: lambda: a + b, ast.Sub: lambda: a - b, ast.Mult: lambda: a * b,
                   ast.LShift: lambda: a << b, ast.RShift: lambda: a >> b,
                   ast.BitAnd: lambda: a & b, ast.BitOr: lambda: a | b,
                   ast.BitXor: lambda: a ^ b, ast.FloorDiv: lambda: a // b,
                   ast.Mod: lambda: a % b}
            for k, f in ops.items():
                if isinstance(node.op, k):
                    return f()
        raise ScriptError(f"unsupported expression {text!r}")

    def num(node):
        v = val(node)
        return v.value if isinstance(v, Obj) else v

    return num(tree) & MASK32


# ---------------------------------------------------------------------------------------- model
class Model:
    """The memory map rules and the contents of the memories, as the SoC should behave."""

    def __init__(self, spec, rom_words: list[int] | None, has_ddr: bool):
        self.spec = spec
        self.rom = rom_words
        self.has_ddr = has_ddr
        self.ddr_ok = False
        self.mem: dict[int, int] = {}            # word address -> value (RAM regions)
        self.d_written: set[int] = set()         # words the data port wrote since the last sync
        self.i_fetched: dict[int, int] = {}      # words the fetch port read since then -> line
        self.regions = sorted(spec.regions.values(), key=lambda r: r.base)
        self.mmio_offsets = {}
        for p in spec.periphs.values():
            offs = set()
            for reg in p.regs.values():
                for k in range(reg.count):
                    offs.add(reg.offset + 4 * k)
            self.mmio_offsets[p.name] = offs

    def region_of(self, addr: int):
        for r in self.regions:
            if r.base <= addr < r.base + r.size:
                return r
        return None

    def access_ok(self, port: str, addr: int, write: bool, be: int) -> bool:
        r = self.region_of(addr)
        if r is None or r.kind == "reserved":
            return False
        if port == "I" and (write or not r.fetch):
            return False
        if write and not r.write:
            return False
        if r.kind == "ddr" and not (self.has_ddr and self.ddr_ok):
            return False
        if r.kind == "mmio":
            if be != 0xF or addr & 3:
                return False
            if (addr - r.base) not in self.mmio_offsets[r.name]:
                return False
        return True

    def word(self, addr: int) -> int:
        a = addr & ~3
        r = self.region_of(a)
        if r is not None and r.kind == "rom":
            if self.rom is None:
                raise ScriptError("the ROM's contents are needed: pass --rom")
            return self.rom[(a - r.base) // 4]
        return self.mem.get(a, 0)

    def write(self, addr: int, be: int, data: int):
        a = addr & ~3
        old = self.mem.get(a, 0)
        new = 0
        for k in range(4):
            byte = (data if be >> k & 1 else old) >> (8 * k) & 0xFF
            new |= byte << (8 * k)
        self.mem[a] = new
        self.d_written.add(a)

    def end_epoch(self):
        """At a sync (or the end): the two ports ran concurrently since the last one, so no
        word may have been both fetched and written in between."""
        clash = sorted(set(self.i_fetched) & self.d_written)
        self.d_written.clear()
        self.i_fetched.clear()
        if clash:
            raise ScriptError(f"word {clash[0]:#x} was fetched and written between two syncs "
                              "(a race between the ports)")

    def is_memory(self, addr: int) -> bool:
        r = self.region_of(addr)
        return r is not None and r.kind in ("rom", "ram", "ddr")


# ---------------------------------------------------------------------------------------- program
class Program:
    def __init__(self):
        self.ops = {"D": [], "I": []}
        self.listing: list[str] = []

    def add(self, port, op, line, flags=0, be=0xF, addr=0, data=0, mask=0, limit=0, ival=0):
        rec = [(OP[op] << 24) | (flags << 16) | (be & 0xF), addr & MASK32, data & MASK32,
               mask & MASK32, limit & MASK32, line, ival & MASK32, 0]
        self.ops[port].append(rec)
        self.listing.append(f"{port} {len(self.ops[port]) - 1:5d} line {line:4d}: {op:8s} "
                            f"addr {addr & MASK32:08x} data {data & MASK32:08x} "
                            f"mask {mask & MASK32:08x} be {be:x} flags {flags}")

    def emit(self, path: Path, listing: Path | None):
        for port in "DI":
            if not self.ops[port] or self.ops[port][-1][0] >> 24 != OP["end"]:
                self.add(port, "end", 0)
        nd, ni = len(self.ops["D"]), len(self.ops["I"])
        if max(nd, ni) >= 1 << 15:
            raise ScriptError("more than 32767 operations on one port")
        words = [MAGIC, nd, 8, ni, 8 + 8 * nd, 0, 0, 0]
        for rec in self.ops["D"] + self.ops["I"]:
            words += rec
        if len(words) > PROG_WORDS:
            raise ScriptError(f"the program needs {len(words)} words, more than {PROG_WORDS}")
        path.parent.mkdir(parents=True, exist_ok=True)
        # the last word of the engine's program memory, so $readmemh fills the whole array
        tail = f"@{PROG_WORDS - 1:x}\n00000000\n" if len(words) < PROG_WORDS else ""
        path.write_text("".join(f"{w:08x}\n" for w in words) + tail)
        if listing:
            listing.write_text("\n".join(self.listing) + "\n")


# ---------------------------------------------------------------------------------------- compiler
def lanes(addr: int, size: int) -> tuple[int, int]:
    """(byte enables, shift) of an access of `size` bytes at addr."""
    if size == 1:
        return 1 << (addr & 3), 8 * (addr & 3)
    if size == 2:
        if addr & 1:
            raise ScriptError(f"misaligned halfword at {addr:#x}")
        return (0b0011 << (addr & 2)), 8 * (addr & 2)
    if addr & 3:
        raise ScriptError(f"misaligned word at {addr:#x}")
    return 0xF, 0


class Compiler:
    def __init__(self, spec, model: Model, defines: dict, seed: int):
        self.spec = spec
        self.names = build_names(spec)
        self.model = model
        self.defines = defines
        self.rng = random.Random(seed)
        self.prog = Program()
        self.line = 0
        self.plusargs: list[str] = []
        for k, v in defines.items():      # usable in expressions unless the map has the name
            self.names.setdefault(k, v)

    def ev(self, text: str) -> int:
        return evaluate(text, self.names)

    @staticmethod
    def opts(words: list[str]) -> dict:
        """keyword arguments: 'expect V' 'mask M' 'within N' 'every K' 'span S' 'offset O'
        'equal V' and the flags 'error' 'auto'."""
        out = {}
        i = 0
        while i < len(words):
            w = words[i]
            if w in ("error", "auto"):
                out[w] = True
                i += 1
            elif w in ("expect", "mask", "within", "every", "span", "offset", "equal", "seed"):
                if i + 1 >= len(words):
                    raise ScriptError(f"'{w}' needs a value")
                out[w] = words[i + 1]
                i += 2
            elif w.startswith("be="):
                out["be"] = int(w[3:], 2)
                i += 1
            else:
                raise ScriptError(f"unexpected {w!r}")
        return out

    # ---- single accesses
    def read(self, port, addr, size, o, raw_be=None):
        if raw_be is not None:
            be, shift = raw_be, 0
        else:
            be, shift = lanes(addr, size)
        lane_mask = 0
        for k in range(4):
            if be >> k & 1:
                lane_mask |= 0xFF << (8 * k)
        if o.get("auto"):
            ok = self.model.access_ok(port, addr, False, be)
            if not ok:
                o = {"error": True}
            elif self.model.is_memory(addr):
                o = {"expect": "mem"}
            else:
                o = {}
        if port == "I" and not o.get("error") and self.model.is_memory(addr):
            self.model.i_fetched.setdefault(addr & ~3, self.line)   # its value matters
        if o.get("error"):
            self.prog.add(port, "read", self.line, flags=FLAG_ERR, be=be, addr=addr)
            return
        data, mask = 0, 0
        if "expect" in o:
            if o["expect"] in ("mem", "rom"):
                data, mask = self.model.word(addr), lane_mask
            else:
                data, mask = (self.ev(o["expect"]) << shift) & MASK32, lane_mask
        if "mask" in o:
            mask = (self.ev(o["mask"]) << shift) & MASK32
        self.prog.add(port, "read", self.line, be=be, addr=addr, data=data, mask=mask)

    def write(self, addr, size, value, o, raw_be=None):
        if raw_be is not None:
            be, shift = raw_be, 0
        else:
            be, shift = lanes(addr, size)
        data = (value << shift) & MASK32
        err = bool(o.get("error"))
        if o.get("auto"):
            err = not self.model.access_ok("D", addr, True, be)
        if not err and self.model.is_memory(addr):
            self.model.write(addr, be, data)
        self.prog.add("D", "write", self.line, flags=FLAG_ERR if err else 0, be=be, addr=addr,
                      data=data)

    # ---- generators
    def window(self, region_name: str, o: dict) -> tuple[int, int]:
        if region_name not in self.spec.regions:
            raise ScriptError(f"unknown region {region_name!r}")
        r = self.spec.regions[region_name]
        off = self.ev(o["offset"]) if "offset" in o else 0
        span = self.ev(o["span"]) if "span" in o else r.size - off
        if off < 0 or span <= 0 or off + span > r.size or off % 4 or span % 4:
            raise ScriptError("offset/span outside the region or not word multiples")
        return r.base + off, span

    def random_rw(self, region_name: str, count: int, o: dict):
        lo, span = self.window(region_name, o)
        written = []
        for _ in range(count):
            if written and self.rng.random() < 0.45:
                addr = self.rng.choice(written)
            else:
                addr = lo + self.rng.randrange(span)
            size = self.rng.choice((1, 2, 4))
            addr &= ~(size - 1)
            if self.rng.random() < 0.5:
                self.write(addr, size, self.rng.getrandbits(8 * size), {"auto": True})
                written.append(addr)
            else:
                self.read("D", addr, size, {"auto": True})

    def random_fetch(self, region_name: str, count: int, o: dict):
        lo, span = self.window(region_name, o)
        for _ in range(count):
            addr = (lo + self.rng.randrange(span)) & ~3
            self.read("I", addr, 4, {"auto": True})

    def random_holes(self, port: str, count: int):
        n = 0
        while n < count:
            pick = self.rng.random()
            if pick < 0.4:
                addr = self.rng.getrandbits(32)
            else:   # just past or just before a region
                r = self.rng.choice(self.model.regions)
                addr = (r.base + r.size + self.rng.randrange(0x40)) if pick < 0.7 else \
                       (r.base - 1 - self.rng.randrange(0x40))
                addr &= MASK32
            reg = self.model.region_of(addr)
            if reg is not None and reg.kind != "reserved":
                continue
            addr &= ~3
            if port == "I":
                self.read("I", addr, 4, {"error": True})
            elif self.rng.random() < 0.5:
                self.read("D", addr, 4, {"error": True})
            else:
                self.write(addr, 4, self.rng.getrandbits(32), {"error": True})
            n += 1

    def mmio_rules(self, periph: str):
        if periph not in self.spec.periphs:
            raise ScriptError(f"unknown peripheral {periph!r}")
        r = self.spec.regions[periph]
        offs = self.model.mmio_offsets[periph]
        for off in sorted(offs):
            for be in (0b0001, 0b0010, 0b0100, 0b1000, 0b0011, 0b1100):
                self.read("D", r.base + off, 0, {"error": True}, raw_be=be)
                self.write(r.base + off, 0, 0xFFFF_FFFF, {"error": True}, raw_be=be)
        holes = [o for o in range(0, r.size, 4) if o not in offs]
        for off in self.rng.sample(holes, min(24, len(holes))) + [holes[0], holes[-1]]:
            self.read("D", r.base + off, 4, {"error": True})
            self.write(r.base + off, 4, 0xFFFF_FFFF, {"error": True})

    # ---- one line
    def statement(self, port: str, op: str, args: list[str]):
        if op in ("read", "read8", "read16"):
            if port != "D":
                raise ScriptError("the instruction port fetches: use 'I fetch'")
            size = {"read": 4, "read8": 1, "read16": 2}[op]
            self.read("D", self.ev(args[0]), size, self.opts(args[1:]))
        elif op == "fetch":
            if port != "I":
                raise ScriptError("'fetch' is for the instruction port")
            self.read("I", self.ev(args[0]), 4, self.opts(args[1:]))
        elif op in ("write", "write8", "write16"):
            if port != "D":
                raise ScriptError("the instruction port never writes")
            size = {"write": 4, "write8": 1, "write16": 2}[op]
            self.write(self.ev(args[0]), size, self.ev(args[1]), self.opts(args[2:]))
        elif op == "rawread":
            o = self.opts(args[1:])
            self.read("D", self.ev(args[0]), 0, o, raw_be=o.pop("be"))
        elif op == "rawwrite":                       # rawwrite ADDR be=BBBB VALUE [error]
            o = self.opts([args[1]] + args[3:])
            self.write(self.ev(args[0]), 0, self.ev(args[2]), o, raw_be=o.pop("be"))
        elif op == "poll":
            if port != "D":
                raise ScriptError("poll is for the data port")
            o = self.opts(args[1:])
            if "mask" not in o or "equal" not in o:
                raise ScriptError("poll needs 'mask M equal V'")
            self.prog.add("D", "poll", self.line, addr=self.ev(args[0]), data=self.ev(o["equal"]),
                          mask=self.ev(o["mask"]), limit=self.ev(o.get("within", "100000")),
                          ival=self.ev(o.get("every", "0")))
        elif op == "idle":
            self.prog.add(port, "idle", self.line, data=self.ev(args[0]))
        elif op in ("waitirq", "checkirq"):
            data = mask = 0
            limit = 100000
            i = 0
            while i < len(args):
                if args[i] == "within":
                    limit = self.ev(args[i + 1])
                    i += 2
                    continue
                m = re.fullmatch(r"(ext|timer|soft)=([01])", args[i])
                if not m:
                    raise ScriptError(f"expected ext|timer|soft=0|1, not {args[i]!r}")
                bit = IRQ_BIT[m.group(1)]
                mask |= 1 << bit
                data |= int(m.group(2)) << bit
                i += 1
            self.prog.add(port, op, self.line, data=data, mask=mask, limit=limit)
        elif op == "mtime-near":
            self.prog.add(port, "mtime", self.line, data=self.ev(args[0]))
        elif op == "random-rw":
            if port != "D":
                raise ScriptError("random-rw is for the data port")
            self.random_rw(args[0], self.ev(args[1]), self.opts(args[2:]))
        elif op == "random-fetch":
            if port != "I":
                raise ScriptError("random-fetch is for the instruction port")
            self.random_fetch(args[0], self.ev(args[1]), self.opts(args[2:]))
        elif op == "random-holes":
            self.random_holes(port, self.ev(args[0]))
        elif op == "mmio-rules":
            if port != "D":
                raise ScriptError("mmio-rules is for the data port")
            self.mmio_rules(args[0])
        else:
            raise ScriptError(f"unknown operation {op!r}")

    def compile(self, text: str):
        stack: list[bool] = []          # enclosing if/else: is the branch active
        for self.line, raw in enumerate(text.splitlines(), 1):
            words = raw.split("#", 1)[0].split()
            if not words:
                continue
            try:
                head = words[0]
                if head == "if":
                    neg = words[1].startswith("!")
                    name = words[1].lstrip("!")
                    if name not in self.defines:
                        raise ScriptError(f"'{name}' is not defined (--define {name}=...)")
                    stack.append(bool(self.defines[name]) != neg)
                    continue
                if head == "else":
                    if not stack:
                        raise ScriptError("else without if")
                    stack[-1] = not stack[-1]
                    continue
                if head == "endif":
                    if not stack:
                        raise ScriptError("endif without if")
                    stack.pop()
                    continue
                if not all(stack):
                    continue
                if head == "sync":
                    self.prog.add("D", "sync", self.line)
                    self.prog.add("I", "sync", self.line)
                    self.model.end_epoch()
                    continue
                if head == "stall":
                    self.plusargs.append(f"+ddr_stall={self.ev(words[1]):08x}")
                    continue
                if head == "assume":
                    m = re.fullmatch(r"ddr=([01])", words[1])
                    if not m:
                        raise ScriptError("assume ddr=0|1")
                    self.model.ddr_ok = m.group(1) == "1"
                    continue
                port = "D"
                if head in ("D", "I"):
                    port, words = head, words[1:]
                self.statement(port, words[0], words[1:])
            except (ScriptError, IndexError, KeyError) as e:
                raise ScriptError(f"line {self.line}: {raw.strip()}: {e}") from None
        if stack:
            raise ScriptError("missing endif")
        self.model.end_epoch()


def main() -> int:
    ap = argparse.ArgumentParser(description="SurfCPU bus-script compiler")
    ap.add_argument("script", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--rom", type=Path, help="the boot ROM image the SoC runs with (.mem)")
    ap.add_argument("--seed", type=int, default=1, help="seed of the random generators")
    ap.add_argument("--define", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--listing", type=Path)
    args = ap.parse_args()

    spec = load()
    defines = {}
    for d in args.define:
        name, _, value = d.partition("=")
        defines[name] = int(value or "1", 0)
    rom = None
    if args.rom:
        rom = [int(x, 16) for x in args.rom.read_text().split()]
    model = Model(spec, rom, has_ddr=bool(defines.get("ddr", 1)))
    comp = Compiler(spec, model, defines, args.seed)
    try:
        comp.compile(args.script.read_text())
        comp.prog.emit(args.out, args.listing)
        args.out.with_suffix(".args").write_text(" ".join(comp.plusargs) + "\n")
    except ScriptError as e:
        print(f"{args.script}: {e}", file=sys.stderr)
        return 1
    print(f"{args.out}: {len(comp.prog.ops['D'])} data-port and {len(comp.prog.ops['I'])} "
          f"fetch-port operations")
    return 0


if __name__ == "__main__":
    sys.exit(main())
