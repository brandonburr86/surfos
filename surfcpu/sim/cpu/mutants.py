#!/usr/bin/env python3
"""
SurfCPU core: mutation check of the verification
--------------------
File: mutants.py Date: 10/15/26 (SurfCPU)
--------------------
Applies one small change (a mutant) at a time to a copy of the core's RTL and shows which
part of the verification notices it: the directed tests on build/cpu/surfcpu_tb (7 bus
timings), then, if the mutant survives, the riscv-tests and the testbench self-test, then,
for the mutants that only the trace can show, the formal check (run_formal.py, bmc).
Mostly the independent review's mutants, adapted to the current RTL, plus a few for the
later changes. Disk-safe: no traces, capped cycles, and each mutant's build directory is
deleted as soon as its verdict is known. Run inside the toolchain container from surfcpu/
after `make build-cpu`; the RTL itself is never modified.

    mutants.py [--formal-depth 14] [--jobs 6] [NAME ...]     (default: every mutant)
"""

from __future__ import annotations

import argparse
import concurrent.futures
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent                                     # surfcpu/
OUT = ROOT / "build" / "cpu" / "mut"
C, S, F, M, D = ("rtl/cpu/surfcpu_core.v", "rtl/cpu/surfcpu_csr.v", "rtl/cpu/surfcpu_fetch.v",
                 "rtl/cpu/surfcpu_muldiv.v", "rtl/cpu/surfcpu_decode.v")
IRQ = "wire irq_take = mem_valid && !mem_started && csr_irq_pending;"

# (name, file, text, replacement, stage expected to catch it: sim | formal | equivalent)
MUTANTS = [
    ("fwd_mem_a", C, "wire [31:0] ex_a = (mem_fwd && mem_rd == ex_rs1)", "wire [31:0] ex_a = (1'b0 && mem_rd == ex_rs1)", "sim"),
    ("fwd_mem_b", C, "wire [31:0] ex_b = (mem_fwd && mem_rd == ex_rs2)", "wire [31:0] ex_b = (1'b0 && mem_rd == ex_rs2)", "sim"),
    ("fwd_wb_a", C, "(wb_wen  && wb_rd  == ex_rs1) ? wb_wdata   : ex_rs1_val;", "(1'b0  && wb_rd  == ex_rs1) ? wb_wdata   : ex_rs1_val;", "sim"),
    ("fwd_wb_b", C, "(wb_wen  && wb_rd  == ex_rs2) ? wb_wdata   : ex_rs2_val;", "(1'b0  && wb_rd  == ex_rs2) ? wb_wdata   : ex_rs2_val;", "sim"),
    ("lu_rs1", C, "((ex_use_rs1 && ex_rs1 == mem_rd) ||", "((1'b0 && ex_rs1 == mem_rd) ||", "sim"),
    ("lu_rs2", C, "(ex_use_rs2 && ex_rs2 == mem_rd));", "(1'b0 && ex_rs2 == mem_rd));", "sim"),
    ("late_csr", C, "wire mem_late    = mem_load || mem_csr;", "wire mem_late    = mem_load;", "sim"),
    ("refresh_a", C, "ex_rs1_val   <= ex_a;", "ex_rs1_val   <= ex_rs1_val;", "sim"),
    ("refresh_b", C, "ex_rs2_val   <= ex_b;", "ex_rs2_val   <= ex_rs2_val;", "sim"),
    ("idbyp_a", C, "(wb_wen && wb_rd == id_rs1)    ? wb_wdata : rf_rd1;", "(1'b0 && wb_rd == id_rs1)    ? wb_wdata : rf_rd1;", "sim"),
    ("idbyp_b", C, "(wb_wen && wb_rd == id_rs2)    ? wb_wdata : rf_rd2;", "(1'b0 && wb_rd == id_rs2)    ? wb_wdata : rf_rd2;", "sim"),
    ("irq_started", C, IRQ, "wire irq_take = mem_valid && csr_irq_pending;", "sim"),
    ("irq_valid", C, IRQ, "wire irq_take = !mem_started && csr_irq_pending;", "sim"),
    ("irq_wfi_old", C, IRQ, "wire irq_take = mem_valid && !mem_started && !mem_wfi && csr_irq_pending;", "sim"),
    ("wfi_wait_never", C, "else if (mem_valid && mem_wfi)\n                mem_wfi_wait <= 1'b1;",
     "else if (1'b0)\n                mem_wfi_wait <= 1'b1;", "sim"),
    ("dreq_irq", C, "(mem_offered || (!mem_waiting && !csr_irq_pending));", "(mem_offered || (!mem_waiting));", "sim"),
    ("dreq_waiting", C, "(mem_offered || (!mem_waiting && !csr_irq_pending));", "(mem_offered || (!csr_irq_pending));", "sim"),
    ("memfree", C, "assign mem_free     = !mem_valid || (mem_done && !mem_redirect);", "assign mem_free     = !mem_valid || mem_done;", "sim"),
    ("exredir_exc", C, "wire ex_redirect = ex_advance && ex_taken && !ex_exc_all;", "wire ex_redirect = ex_advance && ex_taken;", "sim"),
    ("id_noredir", C, "assign id_advance  = id_valid && ex_free && !redirect;", "assign id_advance  = id_valid && ex_free;", "sim"),
    ("exflush", C, "if (rst || mem_redirect)\n            ex_valid <= 1'b0;", "if (rst)\n            ex_valid <= 1'b0;", "sim"),
    ("divclear", C, ".div_clear(ex_advance || mem_redirect),", ".div_clear(ex_advance),", "sim"),
    ("rdwen_exc", C, "mem_rd_wen    <= ex_rd_wen && !ex_exc_all;", "mem_rd_wen    <= ex_rd_wen;", "equivalent"),
    ("jalr_bit0", C, "wire [31:0] ex_target = ex_jalr ? {alu_y[31:1], 1'b0} : (ex_pc + ex_imm);", "wire [31:0] ex_target = ex_jalr ? alu_y : (ex_pc + ex_imm);", "sim"),
    ("tmis_nottaken", C, "wire ex_target_mis = ex_taken && ex_target[1];", "wire ex_target_mis = (ex_taken || ex_branch) && ex_target[1];", "sim"),
    ("tmis_none", C, "wire ex_target_mis = ex_taken && ex_target[1];", "wire ex_target_mis = 1'b0 && ex_target[1];", "sim"),
    ("fencei_noredir", C, "assign mem_redirect = trap_take || (commit && (mem_mret || mem_fencei));", "assign mem_redirect = trap_take || (commit && mem_mret);", "sim"),
    ("ebreak_tval", C, "d_ebreak  ? id_pc : 32'd0;", "d_ebreak  ? 32'd0 : 32'd0;", "sim"),
    ("ecall_tval", C, "d_ebreak  ? id_pc : 32'd0;", "(d_ebreak || d_ecall) ? id_pc : 32'd0;", "sim"),
    ("ldst_mis_h", C, "((ex_f3[1:0] == 2'b01 && alu_y[0]) ||", "((1'b0) ||", "sim"),
    ("wdata_byte", C, "{4{mem_opnd[7:0]}}", "{24'd0, mem_opnd[7:0]}", "sim"),
    ("lh_sign", C, "3'b001:  ld_data = {{16{ld_word[15]}}, ld_word[15:0]};", "3'b001:  ld_data = {16'd0, ld_word[15:0]};", "sim"),
    ("tval_irq", C, ".trap_tval(irq_take ? 32'd0 : exc_tval),", ".trap_tval(exc_tval),", "sim"),
    ("csr_uimm", C, "ex_f3[2] ? {27'd0, ex_rs1} :", "ex_f3[2] ? ex_a :", "sim"),
    ("fault_tval", C, "csr_bad ? mem_insn : mem_result;", "csr_bad ? mem_insn : mem_pc;", "sim"),
    ("tmis_tval", C, "ex_target_mis ? ex_target : alu_y;", "ex_target_mis ? ex_pc : alu_y;", "sim"),
    ("ro_write", S, "if (write_intent && addr[11:10] == 2'b11)", "if (1'b0 && write_intent && addr[11:10] == 2'b11)", "sim"),
    ("minstret_wr_counted", S, "if (we && addr == 12'hB02)      minstret <= {minstret[63:32], wval};", "if (we && addr == 12'hB02)      minstret <= {minstret[63:32], wval + 32'd1};", "sim"),
    ("minstreth_wr_counted", S, "else if (we && addr == 12'hB82) minstret <= {wval, minstret[31:0]};", "else if (we && addr == 12'hB82) minstret <= {wval, minstret[31:0]} + 64'd1;", "sim"),
    ("mcycle_wr_counted", S, "if (we && addr == 12'hB00)      mcycle <= {mcycle[63:32], wval};", "if (we && addr == 12'hB00)      mcycle <= {mcycle[63:32], wval + 32'd1};", "sim"),
    ("inhibit_cy", S, "else if (!inhibit_cy)           mcycle <= mcycle + 64'd1;", "else                           mcycle <= mcycle + 64'd1;", "sim"),
    ("inhibit_ir", S, "else if (retire && !inhibit_ir) minstret <= minstret + 64'd1;", "else if (retire)                minstret <= minstret + 64'd1;", "sim"),
    ("mpie_trap", S, "mstatus_mpie <= mstatus_mie;", "mstatus_mpie <= 1'b1;", "sim"),
    ("mpie_mret", S, "mstatus_mie  <= mstatus_mpie;\n                mstatus_mpie <= 1'b1;", "mstatus_mie  <= mstatus_mpie;\n                mstatus_mpie <= mstatus_mpie;", "sim"),
    ("mret_mie1", S, "mstatus_mie  <= mstatus_mpie;\n                mstatus_mpie <= 1'b1;", "mstatus_mie  <= 1'b1;\n                mstatus_mpie <= 1'b1;", "sim"),
    ("irq_prio_sw_first", S, "assign irq_code    = take_ext ? CAUSE_IRQ_EXTERNAL :\n                         take_sw  ? CAUSE_IRQ_SOFTWARE : CAUSE_IRQ_TIMER;",
     "assign irq_code    = take_sw ? CAUSE_IRQ_SOFTWARE :\n                         take_ext  ? CAUSE_IRQ_EXTERNAL : CAUSE_IRQ_TIMER;", "sim"),
    ("irq_prio_tmr_over_sw", S, "assign irq_code    = take_ext ? CAUSE_IRQ_EXTERNAL :\n                         take_sw  ? CAUSE_IRQ_SOFTWARE : CAUSE_IRQ_TIMER;",
     "assign irq_code    = take_ext ? CAUSE_IRQ_EXTERNAL :\n                         take_tmr  ? CAUSE_IRQ_TIMER : CAUSE_IRQ_SOFTWARE;", "sim"),
    ("csr_clear", S, "(rdata & ~src);", "(rdata & src);", "sim"),
    ("mie_mask_sw", S, "12'h304: begin mie_msie <= wval[3];", "12'h304: begin mie_msie <= wval[2];", "sim"),
    ("mtvec_mode", S, "12'h305: rdata = {mtvec_base, 2'b00};", "12'h305: rdata = {mtvec_base[31:3], 3'b000};", "sim"),
    ("hold_drop", F, "wire new_drop = (hold_q && hold_drop_q) || redirect;", "wire new_drop = redirect;", "sim"),
    ("out_drop", F, "if (redirect)       out_drop_q <= 1'b1;", "if (1'b0)       out_drop_q <= 1'b1;", "sim"),
    ("room4", F, "<= 4'd3;", "<= 4'd4;", "sim"),
    ("rsp_keep_all", F, "wire rsp_keep = ibus_rsp_valid && !out_drop_q;", "wire rsp_keep = ibus_rsp_valid;", "sim"),
    ("hold_none", F, "hold_q <= stall;", "hold_q <= 1'b0;", "sim"),
    ("fifo_norst", F, ".rst(rst || redirect),", ".rst(rst),", "sim"),
    ("mulhsu", M, "wire               b_signed = (op[1:0] == 2'b01);", "wire               b_signed = (op[1:0] != 2'b11);", "sim"),
    ("neg_r", M, "neg_r    <= is_signed && a[31];", "neg_r    <= is_signed && (a[31] ^ b[31]);", "sim"),
    ("divzero_rem", M, "(want_rem ? dividend : 32'hFFFF_FFFF)", "(want_rem ? 32'd0 : 32'hFFFF_FFFF)", "sim"),
    ("x0_lui", D, "legal = 1'b1; rd_wen = rd_nz;\n                a_zero = 1'b1;", "legal = 1'b1; rd_wen = 1'b1;\n                a_zero = 1'b1;", "sim"),
    ("x0_opimm", D, "use_rs1 = 1'b1; rd_wen = rd_nz; b_imm = 1'b1; imm = imm_i;\n                alu_op = {1'b0, funct3};",
     "use_rs1 = 1'b1; rd_wen = 1'b1; b_imm = 1'b1; imm = imm_i;\n                alu_op = {1'b0, funct3};", "sim"),
    ("rv_order", C, "if (rv_valid)       rv_order <= rv_order + 64'd1;", "if (mem_record)       rv_order <= rv_order + 64'd1;", "sim"),
    ("rv_intr_irq", C, "if (trap_take)      rv_next_intr <= 1'b1;", "if (exc_take)      rv_next_intr <= 1'b1;", "formal"),
    ("rv_npc_mret", C, "rv_npc       <= exc_take ? csr_mtvec : mem_mret ? csr_mepc : mem_rv_npc;", "rv_npc       <= exc_take ? csr_mtvec : mem_rv_npc;", "formal"),
    ("rv_memaddr", C, "rv_mem_addr  <= (rv_ld || rv_st) ? {mem_result[31:2], 2'b00} : 32'd0;", "rv_mem_addr  <= (rv_ld || rv_st) ? mem_result : 32'd0;", "formal"),
    ("rv_rs1", C, "mem_rv_rs1_rdata <= ex_use_rs1 ? ex_a : 32'd0;", "mem_rv_rs1_rdata <= ex_use_rs1 ? ex_rs1_val : 32'd0;", "formal"),
]

# (ilat, dlat, seed, extra): the first seed of each run_tests.py configuration
CONFIGS = [("1:1", "1:1", 1, []), ("1:3", "1:3", 2, []), ("1:12", "1:12", 3, []), ("1:2", "4:40", 4, []),
           ("4:40", "1:2", 5, []), ("6:30", "6:30", 6, ["--rand-reset"]), ("20:100", "20:100", 7, [])]
CYCLE_CAP = 40_000_000


def sh(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def budget(elf: Path) -> int:
    tests = HERE / "tests"
    src = tests / "c" / (elf.stem[2:] + ".c") if elf.stem.startswith("c_") else tests / (elf.stem + ".S")
    m = re.search(r"max-cycles=(\d+)", src.read_text()) if src.exists() else None
    return min(int(m.group(1)), CYCLE_CAP) if m else 3_000_000


def make_tree(name: str, file: str, old: str, new: str) -> tuple[Path | None, str]:
    d = OUT / name
    shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(ROOT / "rtl" / "cpu", d / "rtl" / "cpu")
    (d / "rtl" / "lib").mkdir(parents=True)
    shutil.copy(ROOT / "rtl" / "lib" / "surf_fifo.v", d / "rtl" / "lib" / "surf_fifo.v")
    text = (d / file).read_text()
    if text.count(old) != 1:
        return None, f"the text to change is found {text.count(old)} times"
    (d / file).write_text(text.replace(old, new))
    return d, ""


def build_tb(d: Path) -> tuple[str | None, str]:
    rtl = sorted(str(p) for p in (d / "rtl" / "cpu").glob("surfcpu_*.v")) + [str(d / "rtl" / "lib" / "surf_fifo.v")]
    r = sh(["verilator", "--cc", "--exe", "--build", "-j", "4", "-O3", "-Wno-fatal", "-Wno-lint", "-Wno-style",
            f"-I{d}/rtl/cpu", f"-I{ROOT}/rtl/generated", f"-I{d}/rtl/lib", "-DSURFCPU_RVFI",
            "--x-assign", "unique", "--x-initial", "unique", "-CFLAGS", "-O2",
            "--top-module", "surfcpu_tb_top", "-Mdir", str(d / "obj"), "-o", "surfcpu_tb",
            str(HERE / "surfcpu_tb_top.v")] + rtl + [str(HERE / "surfcpu_tb.cpp")])
    return (str(d / "obj" / "surfcpu_tb"), "") if r.returncode == 0 else (None, "build failed: " + r.stderr[-300:])


def run_one(tb: str, elf: Path, cfg: tuple) -> int:
    il, dl, seed, extra = cfg
    try:
        r = sh([tb, "--elf", str(elf), "--ilat", il, "--dlat", dl, "--seed", str(seed), "--quiet",
                "--max-cycles", str(budget(elf))] + extra, timeout=900)
    except subprocess.TimeoutExpired:
        return -1
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--formal-depth", type=int, default=14)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    known = {m[0] for m in MUTANTS}
    for n in a.names:
        if n not in known:
            print(f"mutants: no mutant {n}")
            return 2
    elfs = sorted((ROOT / "build" / "cpu" / "tests").glob("*.elf"))
    if not elfs:
        print("mutants: no test ELFs; run make build-cpu first")
        return 2
    unexpected = 0
    counts = {"sim": 0, "riscv-tests": 0, "selftest": 0, "formal": 0, "survived": 0, "error": 0}
    for name, file, old, new, expect in MUTANTS:
        if a.names and name not in a.names:
            continue
        d, err = make_tree(name, file, old, new)
        verdict = ""
        if d is None:
            verdict, stage = f"ERROR: {err}", "error"
        else:
            tb, err = build_tb(d)
            if tb is None:
                verdict, stage = f"ERROR: {err}", "error"
            else:
                failed: dict[str, set[str]] = {}
                with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
                    futs = {pool.submit(run_one, tb, e, c): e.stem for e in elfs for c in CONFIGS}
                    for fut, stem in futs.items():
                        rc = fut.result()
                        if rc != 0:
                            failed.setdefault(stem, set()).add(str(rc))
                if failed:
                    stage = "sim"
                    verdict = "KILLED by the directed tests: " + " ".join(
                        f"{k}({'/'.join(sorted(v))})" for k, v in sorted(failed.items()))
                else:
                    r = sh([sys.executable, str(HERE / "riscv_tests.py"), "--tb", tb, "--jobs", str(a.jobs)])
                    s = sh([sys.executable, str(HERE / "tb_selftest.py"), "--tb", tb])
                    if r.returncode != 0:
                        stage, verdict = "riscv-tests", "KILLED by the riscv-tests"
                    elif s.returncode != 0:
                        bad = [l.strip() for l in s.stdout.splitlines() if l.strip().startswith("FAIL")]
                        stage, verdict = "selftest", "KILLED by the testbench self-test: " + "; ".join(bad[:2])
                    else:
                        f = sh([sys.executable, str(HERE / "formal" / "run_formal.py"), "--depth",
                                str(a.formal_depth), "--rtl", str(d / "rtl"), "--out", str(d / "formal"),
                                "--tasks", "bmc"])
                        if f.returncode != 0:
                            stage, verdict = "formal", "KILLED by formal-cpu: " + f.stdout.strip().split(": ", 1)[-1].split("; logs in")[0]
                        else:
                            stage, verdict = "survived", "SURVIVED the directed tests, riscv-tests, self-test and formal-cpu"
        shutil.rmtree(OUT / name, ignore_errors=True)
        counts[stage] += 1
        ok = (expect == "equivalent" and stage == "survived") or (expect != "equivalent" and stage not in ("survived", "error"))
        unexpected += not ok
        note = "" if ok else "   <-- unexpected"
        if expect == "equivalent":
            note += "   (equivalent: no observable difference)"
        print(f"{name:22} {verdict}{note}", flush=True)
    shutil.rmtree(OUT, ignore_errors=True)
    print("mutants: " + ", ".join(f"{k} {v}" for k, v in counts.items() if v) + f"; {unexpected} unexpected")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(main())
