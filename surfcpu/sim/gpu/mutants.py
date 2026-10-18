#!/usr/bin/env python3
"""SurfGPU: the tests catch a broken GPU (make test-gpu-mutants; docs/GPU.md section 12).

    mutants.py --work DIR [--only NAME,...] [--jobs N]

Each mutant is a one-line change to a copy of surf_gpu.v, surf_vram.v or surf_video.v: a bug
of the kind the design could have (a copy in the wrong direction, a clip off by one, EXPAND's
bits in the wrong order, the engine taking the port without its grant, the overlay rule wrong,
FRAME_CRC's bytes swapped, a placement change not waiting for the frame latch...). The test
that covers it (test_gpu, test_vram or test_video_gfx) is built against the copy with Verilator
and run on seed 1; it must fail. Exit 0 when every mutant is caught. Run in the toolchain
container from surfcpu/ (it needs verilator and g++).
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GPU = "rtl/peripherals/surf_gpu.v"
VRAM = "rtl/soc/surf_vram.v"
VIDEO = "rtl/peripherals/surf_video.v"

# name, file, the line's text, its replacement, the test
MUTANTS = [
    ("copy-rows-always-down", GPU, "wire               k_up = by > ay;", "wire               k_up = 1'b0;", "gpu"),
    ("copy-never-right-to-left", GPU, "wire               k_rl = (by == ay) && (bx > ax);",
     "wire               k_rl = 1'b0;", "gpu"),
    ("clip-right-edge-inclusive", GPU, "assign in_clip[gl] = (xl >= ecx0) && (xl < ecx1) && row_in;",
     "assign in_clip[gl] = (xl >= ecx0) && (xl <= ecx1) && row_in;", "gpu"),
    ("expand-bits-lsb-first", GPU, "wire       px  = sbuf[6'd63 - {4'd0, d}];",
     "wire       px  = sbuf[6'd60 + {4'd0, d}];", "gpu"),
    ("expand-transparency-ignored", GPU, "!(is_exp && !px && hdr[HDR_EXPAND_TRANSP]);", "1'b1;", "gpu"),
    ("write-rect-lanes-unaligned", GPU, "is_wrr ? sbuf[{1'b0, d, 3'b000} +: 8] : hdr[7:0];",
     "is_wrr ? sbuf[{1'b0, LN, 3'b000} +: 8] : hdr[7:0];", "gpu"),
    ("line-tie-moves-x-late", GPU, "wire               l_mvx = le2 >= ldy;", "wire               l_mvx = le2 > ldy;", "gpu"),
    ("copy-key-ignored", GPU, "copy_mask[lk] = in_rect[lk] && !(hdr[HDR_COPY_KEY] && win[8 * lk +: 8] == hdr[7:0]);",
     "copy_mask[lk] = in_rect[lk];", "gpu"),
    ("fill-ignores-grant", GPU, "end else if (lane_mask == 4'd0 || m_gnt) begin",
     "end else if (1'b1) begin", "gpu"),
    ("overflow-never-set", GPU, "if (wr && hit_cmd && fifo_full) ovf_q <= 1'b1;", "if (1'b0) ovf_q <= 1'b1;", "gpu"),
    ("target-bound-off-by-one", GPU, "({4'd0, p0} + mul_acc + {20'd0, tg_w} > VRAM_36)));",
     "({4'd0, p0} + mul_acc + {20'd0, tg_w} >= VRAM_36)));", "gpu"),
    ("copy-window-not-shifted", GPU, "else     begin lo <= hi_n; cw_hi <= 1'b1; end",
     "else     begin cw_hi <= 1'b1; end", "gpu"),
    ("vram-engine-over-cpu", VRAM, "assign g_gnt = g_req && !cpu;", "assign g_gnt = g_req;", "vram"),
    ("vram-bound-inclusive", VRAM, "wire        cpu_ok = !s_req_fetch && ({13'd0, off} < BYTES_W);",
     "wire        cpu_ok = !s_req_fetch && ({13'd0, off} <= BYTES_W);", "vram"),
    ("overlay-background-ignored", VIDEO, "wire [11:0] over3 = (on3 || bg3 != 4'd0) ? txt3 : gfx3;",
     "wire [11:0] over3 = on3 ? txt3 : gfx3;", "video"),
    ("crc-bytes-swapped", VIDEO, "d = {4'd0, p};", "d = {p[7:0], 4'd0, p[11:8]};", "video"),
    ("gfx-placement-not-latched", VIDEO, "                    if (frame_latch) begin\n                        f_gbase",
     "                    if (1'b1) begin\n                        f_gbase", "video"),
    ("gfx-scale-ignored", VIDEO, "wire [18:0] gaddr0 = gline + {9'd0, f_gdbl ? {1'b0, ghc[9:1]} : ghc};",
     "wire [18:0] gaddr0 = gline + {9'd0, ghc};", "video"),
]

CFLAGS = "-std=c++17 -O1 -g -I{r}/sim/models -I{r}/sim/io -I{r}/sim/gpu -I{r}/firmware/include"
TESTS = {
    "gpu": dict(top="tb_gpu", rtl=["sim/gpu/tb_gpu.v", GPU, "rtl/lib/surf_fifo.v", VRAM, "rtl/soc/surf_vram_bank.v"],
                cpp=["sim/gpu/test_gpu.cpp", "sim/models/gpu_model.c"], args=["--seed", "1", "--chunks", "60"]),
    "vram": dict(top="surf_vram", rtl=[VRAM, "rtl/soc/surf_vram_bank.v"], cpp=["sim/gpu/test_vram.cpp"],
                 args=["--seed", "1", "--cycles", "150000"]),
    "video": dict(top="tb_video", rtl=["sim/io/tb_video.v"], gparams=["-GHAS_GFX=1"],
                  cpp=["sim/gpu/test_video_gfx.cpp", "sim/models/vga_capture.cpp", "sim/models/gpu_model.c"],
                  args=["--seed", "1", "--font", str(ROOT.parent / "driver/font8x16.c"), "--out", "{w}"]),
}


def run_one(name, path, old, new, test, work: Path) -> tuple[bool, str]:
    w = work / name
    if w.exists():
        shutil.rmtree(w)
    (w / "rtl").mkdir(parents=True)
    src = (ROOT / path).read_text()
    if src.count(old) != 1:
        return False, "the line to mutate is not in %s exactly once" % path
    mutated = w / "rtl" / Path(path).name
    mutated.write_text(src.replace(old, new, 1))
    t = TESTS[test]
    files = [str(mutated) if f == path else str(ROOT / f) for f in t["rtl"]]
    # the mutated copy first in the search path, so a module found by -y/-I comes from it
    cmd = ["verilator", "--cc", "--exe", "--build", "-j", "2", "-Wno-fatal", "-Wno-lint", "-Wno-style",
           "-I" + str(w / "rtl"), "-I" + str(ROOT / "rtl/generated"), "-I" + str(ROOT / "rtl/lib"),
           "-I" + str(ROOT / "rtl/soc"), "-I" + str(ROOT / "rtl/peripherals"), "--x-assign", "unique",
           "--x-initial", "unique", "-CFLAGS", CFLAGS.format(r=ROOT), "--top-module", t["top"],
           "-Mdir", str(w / "obj"), "-o", "../sim"] + t.get("gparams", []) + files + \
          [str(ROOT / c) for c in t["cpp"]]
    b = subprocess.run(cmd, cwd=w, capture_output=True, text=True)
    if b.returncode != 0:
        return False, "the mutant did not build: " + b.stderr[-400:]
    args = [a.format(w=w) for a in t["args"]]
    r = subprocess.run([str(w / "sim")] + args, cwd=w, capture_output=True, text=True, timeout=1800)
    shutil.rmtree(w / "obj", ignore_errors=True)
    last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
    return r.returncode != 0, last


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--only")
    a = ap.parse_args()
    work = Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    only = set(a.only.split(",")) if a.only else None
    caught = missed = 0
    for name, path, old, new, test in MUTANTS:
        if only and name not in only:
            continue
        ok, last = run_one(name, path, old, new, test, work)
        print("%-30s %-6s %s: %s" % (name, test, "caught" if ok else "NOT CAUGHT", last))
        sys.stdout.flush()
        if ok:
            caught += 1
        else:
            missed += 1
    print("test-gpu-mutants: %d of %d caught" % (caught, caught + missed))
    return 0 if missed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
