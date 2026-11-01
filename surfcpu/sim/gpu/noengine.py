#!/usr/bin/env python3
"""SurfGPU: a surf_gpu.v whose engine is not the one gpudiag expects (make test-gpu-noengine).

    noengine.py SRC DST

Copies SRC (rtl/peripherals/surf_gpu.v) to DST with gpu.ID 0x32555048 instead of 0x32555047
("GPU2"), the first GPU review's no-engine case: a SoC built with it has FEATURES.GPU, VRAM and a
gpu window, and gpudiag must still fail. Exit 1 when the ID line is not in SRC exactly once.
"""

import sys

OLD = "localparam [31:0]  ID_VALUE = 32'h3255_5047;"
NEW = "localparam [31:0]  ID_VALUE = 32'h3255_5048;"


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src).read()
    if text.count(OLD) != 1:
        print("noengine.py: %r is not in %s exactly once" % (OLD, src))
        return 1
    open(dst, "w").write(text.replace(OLD, NEW))
    return 0


if __name__ == "__main__":
    sys.exit(main())
