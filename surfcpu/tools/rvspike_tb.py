#!/usr/bin/env python3
"""
SurfCPU stand-in for the CPU testbench, made of Spike (for the runner's self-test)
--------------------
File: rvspike_tb.py Date: 10/15/26 (SurfCPU)
--------------------
Takes exactly the command line of build/cpu/surfcpu_tb (docs/MODULES.md section 6) and
behaves like it, with Spike underneath: runs the ELF, writes the retirement trace the way a
testbench writes RVFI (no reference annotations), dumps the signature, and exits 0 (tohost
1), 1 (another tohost value) or 2 (past --max-cycles, here counted in retirements). Until
the core exists, tools/rvrun.py runs against this, so the runner, its file handling and its
verdicts are proven on the same interface the real testbench will have.

    rvspike_tb.py --elf FILE [--trace FILE] [--seed N] [--max-cycles N]
                  [--ilat MIN:MAX] [--dlat MIN:MAX]
                  [--signature FILE --sig-begin SYM --sig-end SYM]
  stand-in only (also from SURFCPU_STANDIN, e.g. SURFCPU_STANDIN="inject=rd_data@40"):
                  [--inject KIND[@N]]   damage the trace (tools/trace_inject.py kinds)
                  [--lsb-mem]           riscv-formal's default memory layout instead of the
                                        RISCV_FORMAL_ALIGNED_MEM convention of MODULES.md 5
                  [--no-final-store]    stop before logging the tohost store
                  [--bad-signature]     flip a bit of the signature's first word
                  [--exit N]            exit with N whatever happened
                  [--runaway]           never store to tohost: after the program, loop on a
                                        jump to itself until --max-cycles (slowly, so the
                                        runner's trace-size watchdog can be seen to act)
--seed, --ilat and --dlat are accepted and ignored: Spike has no bus latency.
Only the Python standard library is used.
"""

from __future__ import annotations

import argparse
import copy
import os
import shlex
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rvelf          # noqa: E402
import trace_inject   # noqa: E402
import trace_spike    # noqa: E402
import tracefmt       # noqa: E402


def _lsb(r: tracefmt.Rec) -> tracefmt.Rec:
    """riscv-formal's default layout: mem_addr is the first byte, masks and data at bit 0."""
    r = copy.deepcopy(r)
    m = r.rmask | r.wmask
    if m:
        sh = (m & -m).bit_length() - 1
        r.mem_addr += sh
        r.rmask >>= sh
        r.wmask >>= sh
        r.mem_rdata >>= 8 * sh
        r.mem_wdata >>= 8 * sh
    return r


def _run_away(path: str, recs: list[tracefmt.Rec], max_cycles: int) -> int:
    """Append jumps to self until max_cycles retirements, 20000 records every 0.1 s."""
    last = recs[-1] if recs else None
    order = last.order + 1 if last else 0
    pc = last.pc_next if last else 0x80000000
    n = len(recs)
    with open(path, "a", encoding="utf-8") as fh:
        while n < max_cycles:
            k = min(20000, max_cycles - n)
            fh.write("".join(tracefmt.Rec(order + j, pc, 0x0000006F, pc_next=pc).text() + "\n"
                             for j in range(k)))
            fh.flush()
            order, n = order + k, n + k
            time.sleep(0.1)
    return n


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--elf", required=True)
    ap.add_argument("--trace")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--max-cycles", type=int, default=trace_spike.DEFAULT_MAX_RECORDS)
    ap.add_argument("--ilat")
    ap.add_argument("--dlat")
    ap.add_argument("--signature")
    ap.add_argument("--sig-begin", default="begin_signature")
    ap.add_argument("--sig-end", default="end_signature")
    ap.add_argument("--inject")
    ap.add_argument("--lsb-mem", action="store_true")
    ap.add_argument("--no-final-store", action="store_true")
    ap.add_argument("--bad-signature", action="store_true")
    ap.add_argument("--exit", type=int)
    ap.add_argument("--runaway", action="store_true")
    env = os.environ.get("SURFCPU_STANDIN", "")
    extra = []
    for item in shlex.split(env.replace(",", " ")):
        k, _, v = item.partition("=")
        extra += [f"--{k}"] + ([v] if v else [])
    a = ap.parse_args(argv + extra)
    try:
        elf = rvelf.read_elf(a.elf)
    except (OSError, rvelf.ElfError) as e:
        print(f"rvspike_tb: {e}", file=sys.stderr)
        return 3
    if "tohost" not in elf.symbols:
        print(f"rvspike_tb: {a.elf} has no tohost symbol", file=sys.stderr)
        return 3
    sig = (a.sig_begin, a.sig_end) if a.signature else None
    try:
        # --max-cycles counts retirements here, capped so a runaway program cannot fill
        # memory (a record takes a few hundred bytes).
        limit = min(a.max_cycles, trace_spike.DEFAULT_MAX_RECORDS)
        res = trace_spike.run_spike(a.elf, max_records=limit, sig_symbols=sig)
    except trace_spike.ConvertError as e:
        print(f"rvspike_tb: {e}", file=sys.stderr)
        return 3
    recs = res.records
    for r in recs:
        r.notes = None
    if (a.no_final_store or a.runaway) and res.status != "timeout" and recs:
        recs = recs[:-1]
    if a.inject:
        kind, _, n = a.inject.partition("@")
        recs, i, expect = trace_inject.inject(recs, kind, int(n) if n else None)
        print(f"rvspike_tb: injected {kind} at retirement {i} (expect {expect})",
              file=sys.stderr)
    if a.lsb_mem:
        recs = [_lsb(r) for r in recs]
    if a.trace:
        tracefmt.save_trace(recs, a.trace, ["stand-in testbench: Spike via tools/rvspike_tb.py"],
                            with_notes=False)
    if a.runaway:
        n = _run_away(a.trace, recs, a.max_cycles) if a.trace else a.max_cycles
        print(f"rvspike_tb: no tohost store within {n} retirements", file=sys.stderr)
        return 2
    if a.signature and res.signature is not None:
        words = list(res.signature)
        if a.bad_signature and words:
            words[0] ^= 1
        trace_spike.write_signature_file(words, Path(a.signature))
    status = {"pass": 0, "fail": 1, "timeout": 2}.get(res.status, 3)
    if res.status == "fail":
        print(f"rvspike_tb: FAIL test {res.tohost >> 1}", file=sys.stderr)
    elif res.status == "timeout":
        print(f"rvspike_tb: no tohost store within {a.max_cycles} retirements",
              file=sys.stderr)
    return a.exit if a.exit is not None else status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
