#!/usr/bin/env python3
"""SurfGPU: the command encodings in C and in the RTL are the same (docs/GPU.md section 5).

    check_gpu_h.py [--header firmware/include/surfcpu/gpu.h] [--rtl rtl/peripherals/surf_gpu.v]

Compares every opcode (GPU_OP_X in gpu.h, OP_X in surf_gpu.v), the header bits (GPU_COPY_KEY,
GPU_EXPAND_TRANSP, GPU_FENCE_IRQ and GPU2's GPU_TRI_* against HDR_* in surf_gpu.v and
surf_gpu_tri.v, each defined once in each file that uses it), gpu.ID and VRAM's size
(SURF_VRAM_BYTES, generated from spec/soc.toml into soc.h, against surf_gpu's VRAM_BYTES default;
surfcpu_soc.v takes the generated value). Exit 0 when all agree.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", default=str(ROOT / "firmware/include/surfcpu/gpu.h"))
    ap.add_argument("--rtl", default=str(ROOT / "rtl/peripherals/surf_gpu.v"))
    ap.add_argument("--tri", default=str(ROOT / "rtl/peripherals/surf_gpu_tri.v"))
    ap.add_argument("--soc", default=str(ROOT / "rtl/soc/surfcpu_soc.v"))
    ap.add_argument("--soc-h", default=str(ROOT / "firmware/include/surfcpu/soc.h"))
    a = ap.parse_args()
    h = Path(a.header).read_text()
    v = Path(a.rtl).read_text()
    t = Path(a.tri).read_text()
    soc = Path(a.soc).read_text()
    soc_h = Path(a.soc_h).read_text()
    problems = []
    c_ops = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define GPU_OP_(\w+)\s+0x([0-9A-Fa-f]+)u", h)}
    v_ops = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"OP_(\w+)\s*=\s*8'h([0-9A-Fa-f]+)", v)}
    if not c_ops:
        problems.append("no opcodes in " + a.header)
    if c_ops != v_ops:
        problems.append("opcodes differ: gpu.h %s, surf_gpu.v %s" % (sorted(c_ops.items()), sorted(v_ops.items())))
    bits = {"COPY_KEY": "HDR_COPY_KEY", "EXPAND_TRANSP": "HDR_EXPAND_TRANSP", "FENCE_IRQ": "HDR_FENCE_IRQ",
            "TRI_GOURAUD": "HDR_TRI_GOURAUD", "TRI_ZTEST": "HDR_TRI_ZTEST", "TRI_ZFUNC_LE": "HDR_TRI_ZFUNC",
            "TRI_ZWRITE": "HDR_TRI_ZWRITE", "TRI_CULL_CW": "HDR_TRI_CULL_CW", "TRI_CULL_CCW": "HDR_TRI_CULL_CCW"}
    for c, rv in bits.items():
        mc = re.search(r"#define GPU_%s\s+\(1u << (\d+)\)" % c, h)
        found = []
        for name, text in (("surf_gpu.v", v), ("surf_gpu_tri.v", t)):
            ms = re.findall(r"\b%s\s*=\s*(\d+)" % rv, text)
            if len(ms) > 1:
                problems.append("header bit %s defined %d times in %s" % (c, len(ms), name))
            found += [(name, m) for m in ms]
        if not mc or not found or any(m != mc.group(1) for _, m in found):
            problems.append("header bit %s: gpu.h %s, RTL %s" % (c, mc and mc.group(1), found or "none"))
    mc = re.search(r"#define GPU_ID_VALUE\s+0x([0-9A-Fa-f]+)u", h)
    mv = re.search(r"ID_VALUE\s*=\s*32'h([0-9A-Fa-f_]+)", v)
    if not mc or not mv or int(mc.group(1), 16) != int(mv.group(1).replace("_", ""), 16):
        problems.append("gpu.ID differs")
    mc = re.search(r"#define SURF_VRAM_BYTES\s+0x([0-9A-F]+)u", soc_h)
    mv = re.search(r"parameter integer VRAM_BYTES\s*=\s*(\d+)", v)
    ms = re.search(r"localparam integer VRAM_BYTES = SURF_VRAM_BYTES;", soc)
    if not mc or not mv or not ms or int(mc.group(1), 16) != int(mv.group(1)):
        problems.append("VRAM size: soc.h %s, surf_gpu.v %s, surfcpu_soc.v %s"
                        % (mc and int(mc.group(1), 16), mv and mv.group(1), "takes SURF_VRAM_BYTES" if ms else "?"))
    if re.search(r"#define SURF_VRAM_BYTES", h):
        problems.append("gpu.h defines SURF_VRAM_BYTES again (soc.h has it, from spec/soc.toml)")
    if problems:
        for p in problems:
            print("check_gpu_h: FAIL: " + p)
        return 1
    print("check_gpu_h: PASS (%d opcodes, %d header bits and the ID agree in gpu.h, surf_gpu.v and surf_gpu_tri.v; "
          "VRAM's size in soc.h, surf_gpu.v and surfcpu_soc.v)" % (len(c_ops), len(bits)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
