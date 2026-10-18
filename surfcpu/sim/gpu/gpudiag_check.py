#!/usr/bin/env python3
"""SurfGPU: check a gpudiag run (docs/GPU.md section 10).

    gpudiag_check.py --uart TRANSCRIPT --frames FRAMES --expect gpuscenes_expect.h [--log LOG]
                     [--engine-scenes N]

TRANSCRIPT is what gpudiag printed (surfsim --transcript, or a board's UART capture); FRAMES the
CRC of every frame captured at the VGA pins (surfsim --vga-crc: "frame N crc XXXXXXXX"), or "-"
for none (a board); gpuscenes_expect.h the CRCs the C model gave each scene at build time.
Passes when gpudiag says PASS, printed one CRC for every scene of the header, each equal to the
model's, and, with FRAMES, each also the CRC of a frame captured at the pins (so FRAME_CRC is
the picture that left the chip). --engine-scenes N also requires that N scenes needed the engine
and none was skipped. Exit 0 = pass. Only the Python standard library.
"""

import argparse
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--expect", help="gpuscenes_expect.h; without it, gpudiag's own comparisons are taken")
    ap.add_argument("--log")
    ap.add_argument("--engine-scenes", type=int)
    a = ap.parse_args()

    text = open(a.uart, errors="replace").read().replace("\r", "")
    expect = {}
    if a.expect:
        for m in re.finditer(r"0x([0-9a-f]{8})u,\s+/\*\s*(\d+) (.*?) \*/", open(a.expect).read()):
            expect[int(m.group(2))] = (int(m.group(1), 16), m.group(3))
    frames = set()
    nframes = 0
    if a.frames != "-":
        for line in open(a.frames):
            m = re.match(r"frame \d+ crc ([0-9a-f]{8})", line)
            if m:
                frames.add(int(m.group(1), 16))
                nframes += 1

    problems = []
    summary = re.search(r"GPUDIAG: (PASS|FAIL) \(([^)]*)\)", text)
    if not summary:
        problems.append("no GPUDIAG summary line")
    elif summary.group(1) != "PASS":
        problems.append("gpudiag says FAIL (%s)" % summary.group(2))
    seen = {}
    for m in re.finditer(r"^scene +(\d+): (.*?) +crc ([0-9a-f]{8}) ok", text, re.M):
        seen[int(m.group(1))] = int(m.group(3), 16)
    skipped = re.findall(r"^scene +\d+: .*skipped", text, re.M)
    if skipped:
        problems.append("%d scenes skipped" % len(skipped))
    for n, (crc, name) in sorted(expect.items()):
        if n not in seen:
            problems.append("scene %d (%s): no CRC printed" % (n, name))
            continue
        if seen[n] != crc:
            problems.append("scene %d (%s): FRAME_CRC %08x, the model's %08x" % (n, name, seen[n], crc))
        if frames and seen[n] not in frames:
            problems.append("scene %d (%s): no captured frame has CRC %08x" % (n, name, seen[n]))
    if a.expect and not expect:
        problems.append("no expected CRCs in %s" % a.expect)
    if not a.expect:
        if not seen:
            problems.append("no scene printed a CRC")
        expect = {n: (crc, "scene %d" % n) for n, crc in seen.items()}
    if a.engine_scenes is not None:
        n_eng = len(re.findall(r"^scene +\d+: engine:.* crc [0-9a-f]{8} ok", text, re.M))
        if n_eng != a.engine_scenes:
            problems.append("%d engine scenes passed, %d expected" % (n_eng, a.engine_scenes))
    cycles = ""
    if a.log:
        m = re.search(r"after (\d+) cycles", open(a.log, errors="replace").read())
        if m:
            cycles = ", %s cycles" % m.group(1)
    if problems:
        for p in problems:
            print("gpudiag_check: FAIL: " + p)
        return 1
    print("gpudiag_check: PASS (%s; %d scenes, every FRAME_CRC the model's%s; %d frames captured, %d distinct%s)"
          % (summary.group(2), len(expect), " and a captured frame's" if frames else "", nframes, len(frames), cycles))
    return 0


if __name__ == "__main__":
    sys.exit(main())
