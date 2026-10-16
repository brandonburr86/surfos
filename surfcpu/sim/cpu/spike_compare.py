#!/usr/bin/env python3
"""
SurfCPU quick Spike cross-check (cpu track, for its own confidence)
--------------------
File: spike_compare.py Date: 10/15/26 (SurfCPU)
--------------------
Runs one ELF on build/cpu/surfcpu_tb (with --trace) and on Spike (--log-commits) and
compares the two instruction by instruction: pc, instruction, register write, store
address and data, load address. This is a throwaway sanity tool of the cpu track; the
official comparator and the Spike trace converter belong to the verif track (tools/).

    spike_compare.py ELF [--tb build/cpu/surfcpu_tb] [--out DIR] [-- TB-ARGS...]

Known, expected differences are not compared: values read from mip, mimpid, the cycle,
time and instret counters (Spike's CLINT and clock differ from the testbench's, and Spike
runs five boot-ROM instructions before 0x80000000), the data stored into `reset_csrs` (the
framework's copy of those CSRs), and anything after the store to tohost. A trap record of the core (trap = 1) has no commit line in
Spike's log; the next records must still line up. Exit 0 when nothing differs.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

SPIKE = ["spike", "--isa=rv32im_zicsr_zifencei", "--priv=m", "--pmpregions=0",
         "-m0x80000000:0x400000", "--log-commits"]
# CSR reads whose values legitimately differ between Spike and the testbench
UNCOMPARED_CSRS = {0x344, 0xF13, 0xB00, 0xB80, 0xC00, 0xC80, 0xC01, 0xC81, 0xB02, 0xB82, 0xC02, 0xC82}


def parse_spike(text: str) -> list[dict]:
    out = []
    for line in text.splitlines():
        m = re.match(r"core\s+\d+:\s+\d\s+0x([0-9a-f]+)\s+\(0x([0-9a-f]+)\)(.*)", line)
        if not m:
            continue
        pc = int(m.group(1), 16)
        if pc < 0x80000000:
            continue                                 # Spike's boot ROM
        rec = {"pc": pc, "insn": int(m.group(2), 16), "rd": 0, "rd_wdata": 0, "mem": None}
        toks = m.group(3).split()
        i = 0
        while i < len(toks):
            t = toks[i]
            if re.fullmatch(r"x\d+", t) and i + 1 < len(toks):
                rec["rd"] = int(t[1:])
                rec["rd_wdata"] = int(toks[i + 1], 16)
                i += 2
            elif t == "mem" and i + 1 < len(toks):
                addr = int(toks[i + 1], 16)
                if i + 2 < len(toks) and toks[i + 2].startswith("0x"):
                    rec["mem"] = ("w", addr, int(toks[i + 2], 16), (len(toks[i + 2]) - 2) // 2)
                    i += 3
                else:
                    rec["mem"] = ("r", addr, None, None)
                    i += 2
            else:
                i += 1
        out.append(rec)
    return out


def parse_trace(path: Path) -> list[dict]:
    out = []
    for line in path.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        out.append({"order": int(f[0]), "pc": int(f[1], 16), "insn": int(f[2], 16), "trap": int(f[3], 16),
                    "intr": int(f[4], 16), "rd": int(f[5], 16), "rd_wdata": int(f[6], 16),
                    "mem_addr": int(f[7], 16), "rmask": int(f[8], 16), "wmask": int(f[9], 16),
                    "mem_rdata": int(f[10], 16), "mem_wdata": int(f[11], 16), "pc_next": int(f[12], 16)})
    return out


def csr_read_uncompared(insn: int) -> bool:
    return (insn & 0x7F) == 0x73 and ((insn >> 12) & 7) != 0 and (insn >> 20) in UNCOMPARED_CSRS


def symbols(elf: Path) -> dict[str, int]:
    r = subprocess.run(["riscv64-unknown-elf-nm", str(elf)], capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) == 3:
            out[f[2]] = int(f[0], 16)
    return out


def compare(ours: list[dict], spike: list[dict], quiet_lo: int = 0, quiet_hi: int = 0) -> list[str]:
    errs = []
    si = 0
    for r in ours:
        if r["trap"]:
            continue
        if si >= len(spike):
            errs.append(f"order {r['order']}: Spike's log ended (pc {r['pc']:08x})")
            break
        s = spike[si]
        si += 1
        where = f"order {r['order']} pc {r['pc']:08x} insn {r['insn']:08x}"
        if r["pc"] != s["pc"] or r["insn"] != s["insn"]:
            errs.append(f"{where}: Spike is at pc {s['pc']:08x} insn {s['insn']:08x}")
            break
        if not csr_read_uncompared(r["insn"]):
            if r["rd"] != s["rd"] or r["rd_wdata"] != s["rd_wdata"]:
                errs.append(f"{where}: rd x{r['rd']} = {r['rd_wdata']:08x}, Spike x{s['rd']} = {s['rd_wdata']:08x}")
        if r["wmask"]:
            if s["mem"] is None or s["mem"][0] != "w":
                errs.append(f"{where}: we store, Spike does not")
                continue
            _, addr, val, size = s["mem"]
            off = addr & 3
            mine = (r["mem_wdata"] >> (8 * off)) & ((1 << (8 * size)) - 1)
            if (addr & ~3) != r["mem_addr"] or (mine != val and not quiet_lo <= addr < quiet_hi):
                errs.append(f"{where}: store {r['mem_addr']:08x}/{r['wmask']:x} {mine:x}, Spike {addr:08x} {val:x}")
        elif s["mem"] is not None and s["mem"][0] == "w":
            errs.append(f"{where}: Spike stores, we do not")
        if r["rmask"]:
            if s["mem"] is None or s["mem"][0] != "r" or (s["mem"][1] & ~3) != r["mem_addr"]:
                errs.append(f"{where}: load {r['mem_addr']:08x}, Spike {s['mem']}")
        if len(errs) > 20:
            break
    return errs


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--tb", default="build/cpu/surfcpu_tb")
    ap.add_argument("--out", default="build/cpu/spikecmp")
    ap.add_argument("tbargs", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    elf = Path(a.elf)
    trace = out / (elf.stem + ".trace")
    extra = [x for x in a.tbargs if x != "--"]
    r = subprocess.run([a.tb, "--elf", str(elf), "--trace", str(trace), "--quiet"] + extra,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{elf.stem}: surfcpu_tb exit {r.returncode}: {r.stderr.strip()}")
        return 1
    s = subprocess.run(SPIKE + [str(elf)], capture_output=True, text=True, timeout=120)
    (out / (elf.stem + ".spike.log")).write_text(s.stderr)
    ours = parse_trace(trace)
    syms = symbols(elf)
    lo = syms.get("reset_csrs", 0)
    errs = compare(ours, parse_spike(s.stderr), lo, lo + 64 if lo else 0)
    if errs:
        print(f"{elf.stem}: {len(errs)} difference(s) against Spike (spike exit {s.returncode})")
        for e in errs[:20]:
            print("   " + e)
        return 1
    n = sum(1 for x in ours if not x["trap"])
    print(f"{elf.stem}: {n} retired instructions match Spike ({len(ours) - n} trap records)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
