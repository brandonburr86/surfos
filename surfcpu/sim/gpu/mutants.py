#!/usr/bin/env python3
"""SurfGPU: the tests catch a broken GPU (make test-gpu-mutants; docs/GPU.md section 12).

    mutants.py --work DIR [--only NAME,...] [--jobs N]

Each mutant is a one-line change to a copy of surf_gpu.v, surf_gpu_tri.v, surf_gpu_div.v,
surf_vram.v or surf_video.v: a bug of the kind the design could have (a copy in the wrong
direction, a clip off by one, EXPAND's bits in the wrong order, the engine taking the port without
its grant, an ABORT that keeps the FIFO, an interrupt without its enable, the overlay rule wrong,
FRAME_CRC's bytes swapped, a placement change not waiting for the frame latch, a wrong target or
clip after a reset; GPU2's: the top-left rule, a lane, the depth function, the cull sense, the
gradient's rounding, the values' rounding, FLIP before the blanking, the depth buffer kept or
refused wrongly, a depth word's lanes, ZWRITE ignored, the swap of a counter-clockwise triangle, a
step, the clip, ABORT...). The
test that covers it (test_gpu, test_vram or test_video_gfx) is built against the copy with
Verilator and run on seed 1; it must fail. A "soc" mutant is judged by gpudiag on the RTL SoC instead
(surfsim built with HAS_GPU = 1 from the copy, running build/fw/gpudiag.mem): gpudiag must say
FAIL, or gpudiag_check.py fail the capture in its board or its simulation form, since gpudiag is
what runs on the board. An equivalent mutant (EQUIVALENT) changes nothing any test can see, by a
proof in docs/GPU.md (the clamp of section 16.4 never acts): its test must pass, and if it fails
the proof is wrong. A "model" mutant breaks the C model (sim/models/gpu_model.c) instead, and the
model's own tests (sim/gpu/test_gpu_model.c, written from GPU.md) must fail: so that none of their
hand-worked expectations is vacuous. Exit 0 when every mutant is caught and every equivalent one
survives. Run in
the toolchain container from surfcpu/ (it needs verilator and g++, and make build-gpu-rom first).
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
GPU = "rtl/peripherals/surf_gpu.v"
TRI = "rtl/peripherals/surf_gpu_tri.v"
DIV = "rtl/peripherals/surf_gpu_div.v"
VRAM = "rtl/soc/surf_vram.v"
MODEL = "sim/models/gpu_model.c"
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
    ("abort-keeps-fifo", GPU, ".clk(clk), .rst(rst || abort), .wr_en(fifo_push)",
     ".clk(clk), .rst(rst), .wr_en(fifo_push)", "gpu"),
    ("irq-ignores-ie-fence", GPU, "assign irq = (fev_q && ie_q[SURF_GPU_IE_FENCE_LSB]) ||",
     "assign irq = (fev_q) ||", "gpu"),
    ("irq-ignores-ie-err", GPU, "((ovf_q || badop_q || badpar_q) && ie_q[SURF_GPU_IE_ERR_LSB]);",
     "((ovf_q || badop_q || badpar_q));", "gpu"),
    ("busy-early-last-group", GPU, "wire busy = !fifo_empty || st != S_IDLE;",
     "wire busy = !fifo_empty || (st != S_IDLE && !(st == S_RECT && last_g && last_row));", "gpu"),
    ("target-refusal-applies", GPU, "badpar_q <= 1'b1;\n                        else begin",
     "badpar_q <= 1'b1;\n                        begin", "gpu"),
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
    ("pair-reverted", VIDEO, "assign crc_sys    = snap_valid ? crc_snap : crc_cross[31:0];",
     "assign crc_sys    = crc_cross[31:0];", "video"),
    ("pair-never-cleared", VIDEO, "end else if (rd_ok && hit_crc) begin\n                    snap_valid <= 1'b0;\n",
     "end else if (1'b0) begin\n                    snap_valid <= 1'b0;\n", "video"),
    ("pair-cleared-by-any-read", VIDEO, "end else if (rd_ok && hit_crc) begin", "end else if (rd_ok) begin", "video"),
    # gpudiag on the RTL SoC must catch these (the second review: it ran them all to PASS)
    ("soc-abort-ignored", GPU, "wire        abort    = wr && hit_ctrl && s_req_wdata[SURF_GPU_CTRL_ABORT_LSB];",
     "wire        abort    = 1'b0;", "soc"),
    ("soc-irq-err-never", GPU, "((ovf_q || badop_q || badpar_q) && ie_q[SURF_GPU_IE_ERR_LSB]);",
     "((ovf_q || badop_q || badpar_q) && 1'b0);", "soc"),
    ("soc-target-refusal-applies", GPU, "badpar_q <= 1'b1;\n                        else begin",
     "badpar_q <= 1'b1;\n                        begin", "soc"),
    ("soc-busy-early-last-group", GPU, "wire busy = !fifo_empty || st != S_IDLE;",
     "wire busy = !fifo_empty || (st != S_IDLE && !(st == S_RECT && last_g && last_row));", "soc"),
    ("soc-pair-reverted", VIDEO, "assign crc_sys    = snap_valid ? crc_snap : crc_cross[31:0];",
     "assign crc_sys    = crc_cross[31:0];", "soc"),
    ("soc-pair-never-cleared", VIDEO, "end else if (rd_ok && hit_crc) begin\n                    snap_valid <= 1'b0;\n",
     "end else if (1'b0) begin\n                    snap_valid <= 1'b0;\n", "soc"),
    ("soc-pair-cleared-by-any-read", VIDEO, "end else if (rd_ok && hit_crc) begin", "end else if (rd_ok) begin", "soc"),
    # the target and clip a reset leaves, which the first run's refused-target fill checks (the fourth review)
    ("soc-target-reset-base", GPU, "t_base <= 19'd0; t_stride <= 16'd640;", "t_base <= 19'd4096; t_stride <= 16'd640;",
     "soc"),
    ("soc-clip-reset", GPU, "c_x0 <= 16'd0; c_y0 <= 16'd0; c_x1 <= 16'd640; c_y1 <= 16'd480;",
     "c_x0 <= 16'd8; c_y0 <= 16'd0; c_x1 <= 16'd640; c_y1 <= 16'd480;", "soc"),
    # the rest of the target and clip a reset leaves: the stride, the clip's right and bottom edges, the
    # width and height past the clip (seen by COPY_RECT's sources only), which test_gpu's test_reset_target
    # and gpudiag's first run (its fills and copy after the refused target) check (the fifth review)
    ("target-reset-stride-320", GPU, "t_stride <= 16'd640; t_w <= 15'd640;",
     "t_stride <= 16'd320; t_w <= 15'd640;", "gpu"),
    ("clip-reset-right-4", GPU, "c_x1 <= 16'd640; c_y1 <= 16'd480;", "c_x1 <= 16'd4; c_y1 <= 16'd480;", "gpu"),
    ("clip-reset-bottom-1", GPU, "c_x1 <= 16'd640; c_y1 <= 16'd480;", "c_x1 <= 16'd640; c_y1 <= 16'd1;", "gpu"),
    ("target-reset-width-644", GPU, "t_w <= 15'd640; t_h <= 15'd480;", "t_w <= 15'd644; t_h <= 15'd480;", "gpu"),
    ("target-reset-height-481", GPU, "t_w <= 15'd640; t_h <= 15'd480;", "t_w <= 15'd640; t_h <= 15'd481;", "gpu"),
    ("soc-target-reset-stride-320", GPU, "t_stride <= 16'd640; t_w <= 15'd640;",
     "t_stride <= 16'd320; t_w <= 15'd640;", "soc"),
    ("soc-clip-reset-right-4", GPU, "c_x1 <= 16'd640; c_y1 <= 16'd480;", "c_x1 <= 16'd4; c_y1 <= 16'd480;", "soc"),
    ("soc-clip-reset-bottom-1", GPU, "c_x1 <= 16'd640; c_y1 <= 16'd480;", "c_x1 <= 16'd640; c_y1 <= 16'd1;", "soc"),
    ("soc-target-reset-width-644", GPU, "t_w <= 15'd640; t_h <= 15'd480;", "t_w <= 15'd644; t_h <= 15'd480;", "soc"),
    ("soc-target-reset-height-481", GPU, "t_w <= 15'd640; t_h <= 15'd480;", "t_w <= 15'd640; t_h <= 15'd481;", "soc"),
    # GPU2 (docs/GPU.md section 16): the brief's rules (the tie rule, a lane, the depth function, the cull
    # sense, the gradient's rounding, FLIP before blanking) and more
    ("tri-top-edge-excluded", TRI, "wire tl0 = ey0[16] || (ey0 == 17'sd0 && ex0 > 17'sd0);", "wire tl0 = ey0[16];", "gpu"),
    ("tri-right-edge-included", TRI, "e1s <= mdiff - {35'd0, !tl1};", "e1s <= mdiff;", "gpu"),
    ("tri-lane-3-edge-0", TRI, "a0x3 <= a0 + a0x2;", "a0x3 <= a0x2;", "gpu"),
    ("tri-zfunc-swapped", TRI, "zle ? (zq[16 * gl +: 16] <= zbuf) : (zq[16 * gl +: 16] < zbuf);",
     "zle ? (zq[16 * gl +: 16] < zbuf) : (zq[16 * gl +: 16] <= zbuf);", "gpu"),
    ("tri-cull-sense", TRI, "(area > 35'sd0 && fl[HDR_TRI_CULL_CW]) || (area < 35'sd0 && fl[HDR_TRI_CULL_CCW]);",
     "(area > 35'sd0 && fl[HDR_TRI_CULL_CCW]) || (area < 35'sd0 && fl[HDR_TRI_CULL_CW]);", "gpu"),
    ("tri-gradient-toward-zero", DIV, "assign q    = neg ? ~q_mag + {36'd0, rem == 34'd0} : q_mag;",
     "assign q    = neg ? ~q_mag + 37'd1 : q_mag;", "gpu"),
    ("tri-depth-rounded-down", TRI, "t = $signed({v[17], v[17:1]}) + $signed({17'd0, v[0]});",
     "t = $signed({v[17], v[17:1]});", "gpu"),
    ("tri-colour-rounded-down", TRI, "t = $signed({v[9], v[9:1]}) + $signed({9'd0, v[0]});", "t = $signed({v[9], v[9:1]});",
     "gpu"),
    ("flip-before-blanking", GPU, "S_FLIP: if (vblank) begin", "S_FLIP: if (1'b1) begin", "gpu"),
    ("zbuf-kept-by-set-target", GPU, "z_valid <= 1'b0;                              // the depth buffer was the old target's",
     "z_valid <= z_valid;", "gpu"),
    ("zbuf-bound-off-by-one", GPU, "({4'd0, p0} + mul_acc + {19'd0, tw2} > VRAM_36)));",
     "({4'd0, p0} + mul_acc + {19'd0, tw2} >= VRAM_36)));", "gpu"),
    ("tri-depth-word-lanes", TRI, "wire [3:0]  zbe0   = {pass[1], pass[1], pass[0], pass[0]};",
     "wire [3:0]  zbe0   = {pass[0], pass[0], pass[1], pass[1]};", "gpu"),
    ("tri-zwrite-alone-ignored", TRI, "end else if (zwrite) begin                 // every covered lane passes",
     "end else if (1'b0) begin", "gpu"),
    ("tri-colour-ignores-grant", TRI, "T_CW: begin\n                m_req = 1'b1; m_be = pass; m_addr = cword; m_wdata = cval;\n"
     "                if (m_gnt) adv = 1'b1;", "T_CW: begin\n                m_req = 1'b1; m_be = pass; m_addr = cword; "
     "m_wdata = cval;\n                adv = 1'b1;", "gpu"),
    ("tri-ccw-not-swapped", TRI, "if (area[34]) begin                // counter-clockwise",
     "if (1'b0) begin                    // counter-clockwise", "gpu"),
    ("tri-gouraud-x-step-half", TRI, "scx <= {gcx[24:0], 4'd0};", "scx <= {gcx[25:0], 3'd0};", "gpu"),
    ("tri-depth-row-target-stride", TRI, "zrow <= zrow + {16'd0, z_stride};", "zrow <= zrow + {16'd0, t_stride};", "gpu"),
    ("tri-clip-left-ignored", TRI, "bx0_c = (bxa > ecx0) ? bxa : ecx0,", "bx0_c = bxa,", "gpu"),
    ("tri-ignores-abort", TRI, "if (rst || stop) begin", "if (rst) begin", "gpu"),
    # gpudiag on the RTL SoC must catch GPU2's too
    ("soc-flip-before-blanking", GPU, "S_FLIP: if (vblank) begin", "S_FLIP: if (1'b1) begin", "soc"),
    ("soc-zbuf-kept-by-set-target", GPU,
     "z_valid <= 1'b0;                              // the depth buffer was the old target's", "z_valid <= z_valid;", "soc"),
    ("soc-tri-cull-sense", TRI, "(area > 35'sd0 && fl[HDR_TRI_CULL_CW]) || (area < 35'sd0 && fl[HDR_TRI_CULL_CCW]);",
     "(area > 35'sd0 && fl[HDR_TRI_CULL_CCW]) || (area < 35'sd0 && fl[HDR_TRI_CULL_CW]);", "soc"),
    ("soc-tri-zfunc-swapped", TRI, "zle ? (zq[16 * gl +: 16] <= zbuf) : (zq[16 * gl +: 16] < zbuf);",
     "zle ? (zq[16 * gl +: 16] < zbuf) : (zq[16 * gl +: 16] <= zbuf);", "soc"),
    ("soc-tri-top-edge-excluded", TRI, "wire tl0 = ey0[16] || (ey0 == 17'sd0 && ex0 > 17'sd0);", "wire tl0 = ey0[16];",
     "soc"),
    ("soc-tri-gouraud-x-step-half", TRI, "scx <= {gcx[24:0], 4'd0};", "scx <= {gcx[25:0], 3'd0};", "soc"),
    # the C model's own tests catch a broken model (GPU2's rules)
    ("model-top-left-rule", MODEL, "t->tl[e] = t->ey[e] < 0 || (t->ey[e] == 0 && t->ex[e] > 0);",
     "t->tl[e] = t->ey[e] < 0;", "model"),
    ("model-gradient-toward-zero", MODEL, "int64_t q = n / d;\n    return (n % d != 0 && n < 0) ? q - 1 : q;",
     "int64_t q = n / d;\n    return q;", "model"),
    ("model-rounded-down", MODEL, "floor_div128(v + (1 << 19), 1 << 20)", "floor_div128(v, 1 << 20)", "model"),
    ("model-ccw-not-swapped", MODEL, "const int ccw = t->area2 < 0;", "const int ccw = 0;", "model"),
    ("model-zfunc-swapped", MODEL, "pass[L] = zle ? z[L] <= zb : z[L] < zb;", "pass[L] = zle ? z[L] < zb : z[L] <= zb;",
     "model"),
    ("model-zbuf-kept-by-set-target", MODEL, "g->zvalid = 0;                          /* the depth buffer belonged",
     "(void)0;                                /* the depth buffer belonged", "model"),
]

# mutants that change nothing a test can see, by a proof in docs/GPU.md: their test must pass
EQUIVALENT = [
    # 16.4: a covered pixel's value is within 1/8 of the exact plane, so rounding lands inside the
    # vertices' range and the clamp never changes it
    ("tri-depth-clamp-removed", TRI,
     "zround = (t < $signed({2'b00, lo})) ? lo : (t > $signed({2'b00, hi})) ? hi : t[15:0];", "zround = t[15:0];", "gpu"),
    ("tri-colour-clamp-removed", TRI,
     "cround = (t < $signed({2'b00, lo})) ? lo : (t > $signed({2'b00, hi})) ? hi : t[7:0];", "cround = t[7:0];", "gpu"),
    ("model-clamp-removed", MODEL, "const int64_t c = t < p->vmin ? p->vmin : t > p->vmax ? p->vmax : t;",
     "const int64_t c = t;", "model"),
]

CFLAGS = "-std=c++17 -O1 -g -I{r}/sim/models -I{r}/sim/io -I{r}/sim/gpu -I{r}/firmware/include"
TESTS = {
    "gpu": dict(top="tb_gpu", rtl=["sim/gpu/tb_gpu.v", GPU, TRI, DIV, "rtl/lib/surf_fifo.v", VRAM,
                                   "rtl/soc/surf_vram_bank.v"],
                cpp=["sim/gpu/test_gpu.cpp", "sim/models/gpu_model.c"], args=["--seed", "1", "--chunks", "60"]),
    "vram": dict(top="surf_vram", rtl=[VRAM, "rtl/soc/surf_vram_bank.v"], cpp=["sim/gpu/test_vram.cpp"],
                 args=["--seed", "1", "--cycles", "150000"]),
    "video": dict(top="tb_video", rtl=["sim/io/tb_video.v"], gparams=["-GHAS_GFX=1", "-GFLIP_PORT=1"],
                  cpp=["sim/gpu/test_video_gfx.cpp", "sim/models/vga_capture.cpp", "sim/models/gpu_model.c"],
                  args=["--seed", "1", "--font", str(ROOT.parent / "driver/font8x16.c"), "--out", "{w}"]),
}


SOC_CPP = ["sim/soc/surfsim/surfsim.cpp", "sim/models/uart_model.cpp", "sim/models/ddr_model.cpp",
           "sim/models/sdcard_model.cpp", "sim/models/ps2_keyboard_model.cpp", "sim/models/vga_capture.cpp",
           "sim/models/ddrc_model_glue.cpp"]


def soc_const(name: str) -> int:
    m = re.search(r"#define %s (\d+)" % name, (ROOT / "firmware/include/surfcpu/soc.h").read_text())
    return int(m.group(1))


def soc_one(w: Path, path: str, mutated: Path) -> tuple[bool, str]:
    """gpudiag on the RTL SoC built from the mutated copy: (caught, what said so)"""
    rtl = sorted(str(p.relative_to(ROOT)) for d in ("rtl/soc", "rtl/lib", "rtl/cpu", "rtl/peripherals")
                 for p in (ROOT / d).glob("*.v"))
    files = [str(mutated) if f == path else str(ROOT / f) for f in rtl]
    cflags = "-std=c++17 -O2 -DSURFCPU_RVFI -I{r}/sim/models -I{r}/firmware/include".format(r=ROOT)
    cmd = ["verilator", "--cc", "--exe", "--build", "-j", "2", "-O3", "--x-assign", "fast", "-Wno-fatal",
           "--top-module", "surfcpu_soc", "+define+SURFCPU_RVFI"] + \
          ["-I" + str(ROOT / d) for d in ("rtl/generated", "rtl/lib", "rtl/soc", "rtl/cpu", "rtl/peripherals")] + \
          ["-GBUILD_ID=32'h00000000", "-GBUILD_FLAGS=32'h2", "-GSYS_HZ=%d" % soc_const("SURF_SYS_HZ"),
           "-GPIX_HZ=%d" % soc_const("SURF_PIX_HZ"), "-GHAS_GPU=1", "-CFLAGS", cflags, "-Mdir", str(w / "obj"),
           "-o", "../surfsim"] + files + [str(ROOT / c) for c in SOC_CPP]
    b = subprocess.run(cmd, cwd=w, capture_output=True, text=True)
    if b.returncode != 0:
        return False, "the mutant did not build: " + b.stderr[-400:]
    shutil.rmtree(w / "obj", ignore_errors=True)
    uart, frames = w / "uart.txt", w / "frames.txt"
    subprocess.run([str(w / "surfsim"), "--rom", "build/fw/gpudiag.mem", "--quiet", "--max-cycles", "700000000",
                    "--vga-crc", str(frames), "--transcript", str(uart), "--expect", r"GPUDIAG: (PASS|FAIL)[^\n]*\n"],
                   cwd=ROOT, capture_output=True, text=True, timeout=1800)
    (w / "surfsim").unlink()
    text = uart.read_text(errors="replace") if uart.exists() else ""
    summary = re.findall(r"GPUDIAG: (?:PASS|FAIL)[^\n]*", text)
    first_fail = next((l.strip() for l in text.splitlines() if l.strip().startswith("FAIL ")), "")
    forms = []
    for f in ("-", str(frames)):
        c = subprocess.run([sys.executable, "sim/gpu/gpudiag_check.py", "--uart", str(uart), "--frames", f,
                            "--expect", "build/fw/gen/gpuscenes_expect.h"], cwd=ROOT, capture_output=True, text=True)
        forms.append(c.returncode != 0)
    said = summary[-1] if summary else "no GPUDIAG line"
    caught = not summary or "FAIL" in said or any(forms)
    return caught, "gpudiag: %s%s; gpudiag_check board %s, simulation %s" % (
        said, " (%s)" % first_fail[:90] if first_fail else "", "FAIL" if forms[0] else "PASS",
        "FAIL" if forms[1] else "PASS")


def model_one(w: Path, mutated: Path) -> tuple[bool, str]:
    """the model's tests against the mutated model: (caught, their last line)"""
    exe = w / "test_gpu_model"
    b = subprocess.run(["gcc", "-std=c99", "-O2", "-g", "-Wall", "-Wextra", "-I" + str(ROOT / "sim/models"),
                        "-I" + str(ROOT / "firmware/include"), "-o", str(exe), str(ROOT / "sim/gpu/test_gpu_model.c"),
                        str(mutated)], capture_output=True, text=True)
    if b.returncode != 0:
        return False, "the mutant did not build: " + b.stderr[-400:]
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    exe.unlink()
    last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
    return r.returncode != 0, last


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
    if test == "soc":
        return soc_one(w, path, mutated)
    if test == "model":
        return model_one(w, mutated)
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
    caught = missed = survived = wrong = 0
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
    for name, path, old, new, test in EQUIVALENT:
        if only and name not in only:
            continue
        failed, last = run_one(name, path, old, new, test, work)
        built = not last.startswith("the mutant did not build") and not last.startswith("the line to mutate")
        print("%-30s %-6s %s: %s" % (name, test, "survives (equivalent)" if built and not failed else
                                     "CAUGHT: the proof of its equivalence is wrong" if built else "NOT RUN", last))
        sys.stdout.flush()
        if built and not failed:
            survived += 1
        else:
            wrong += 1
    print("test-gpu-mutants: %d of %d caught%s" % (caught, caught + missed, "; %d of %d equivalent ones survive, as "
          "docs/GPU.md says" % (survived, survived + wrong) if survived + wrong else ""))
    return 0 if missed == 0 and wrong == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
