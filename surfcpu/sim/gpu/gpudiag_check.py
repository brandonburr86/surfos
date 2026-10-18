#!/usr/bin/env python3
"""SurfGPU: check a gpudiag run (docs/GPU.md section 10).

    gpudiag_check.py --uart TRANSCRIPT --frames FRAMES --expect gpuscenes_expect.h [--log LOG]
                     [--engine-scenes N] [--must-fail REGEX]

TRANSCRIPT is what gpudiag printed (surfsim --transcript, or a board's UART capture); FRAMES the
CRC of every frame captured at the VGA pins (surfsim --vga-crc: "frame N crc XXXXXXXX"), or "-"
for none (a board); gpuscenes_expect.h the CRCs the C model gave each scene at build time and the
number of checks of a complete run (GPUDIAG_CHECKS). A capture may hold several runs (Ctrl-R, a
reset): only the last complete one counts, from its banner to its GPUDIAG line.

Passes when that run says PASS with exactly GPUDIAG_CHECKS checks (a run cut short, or one that
skipped a section, cannot pass), skipped nothing, printed one CRC for every scene of the header,
each equal to the model's, and, with FRAMES, each also the CRC of a frame captured at the pins (so
FRAME_CRC is the picture that left the chip). --engine-scenes N also requires that N scenes
needed the engine and passed. --must-fail REGEX turns it round, for the tests of a SoC that must
fail gpudiag (sim/gpu, make test-gpu-noengine): it passes only when the run says FAIL, this check
fails it too, and the run's text matches REGEX. Exit 0 = pass. Only the Python standard library.
"""

import argparse
import re
import sys

BANNER = re.compile(r"^SurfGPU diagnostic ", re.M)
SUMMARY = re.compile(r"GPUDIAG: (PASS|FAIL) \(([^)]*)\)")


def last_run(text: str) -> str:
    """the last run that reached its GPUDIAG line, from its banner (or the start) to that line"""
    starts = [m.start() for m in BANNER.finditer(text)] or [0]
    for s in reversed(starts):
        m = SUMMARY.search(text, s)
        if m:
            return text[s:m.end()]
    return text[starts[-1]:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--expect", help="gpuscenes_expect.h; without it, gpudiag's own comparisons are taken")
    ap.add_argument("--log")
    ap.add_argument("--engine-scenes", type=int)
    ap.add_argument("--must-fail", metavar="REGEX")
    a = ap.parse_args()

    text = last_run(open(a.uart, errors="replace").read().replace("\r", ""))
    expect = {}
    total = None
    if a.expect:
        header = open(a.expect).read()
        for m in re.finditer(r"0x([0-9a-f]{8})u,\s+/\*\s*(\d+) (.*?) \*/", header):
            expect[int(m.group(2))] = (int(m.group(1), 16), m.group(3))
        m = re.search(r"#define GPUDIAG_CHECKS (\d+)u", header)
        total = int(m.group(1)) if m else None
    frames = set()
    nframes = 0
    if a.frames != "-":
        for line in open(a.frames):
            m = re.match(r"frame \d+ crc ([0-9a-f]{8})", line)
            if m:
                frames.add(int(m.group(1), 16))
                nframes += 1

    problems = []
    summary = SUMMARY.search(text)
    if not summary:
        problems.append("no GPUDIAG summary line")
    elif summary.group(1) != "PASS":
        problems.append("gpudiag says FAIL (%s)" % summary.group(2))
    elif total is not None:
        m = re.fullmatch(r"(\d+) checks", summary.group(2))
        if not m or int(m.group(1)) != total:
            problems.append("gpudiag says PASS (%s), but a complete run makes %d checks" % (summary.group(2), total))
    if a.expect and total is None:
        problems.append("no GPUDIAG_CHECKS in %s" % a.expect)
    seen = {}
    for m in re.finditer(r"^scene +(\d+): (.*?) +crc ([0-9a-f]{8}) ok", text, re.M):
        seen[int(m.group(1))] = int(m.group(3), 16)
    skipped = re.findall(r"skipped: no engine", text)
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

    if a.must_fail is not None:
        why = []
        if not summary or summary.group(1) != "FAIL":
            why.append("gpudiag did not say FAIL (%s)" % (summary.group(0) if summary else "no summary line"))
        if not problems:
            why.append("this check would have passed the run")
        if not re.search(a.must_fail, text):
            why.append("the run does not say %r" % a.must_fail)
        if why:
            for w in why:
                print("gpudiag_check: FAIL (--must-fail): " + w)
            return 1
        print("gpudiag_check: PASS (--must-fail: %s, %d problems found, %r said)" % (summary.group(0), len(problems),
                                                                                    a.must_fail))
        return 0
    if problems:
        for p in problems:
            print("gpudiag_check: FAIL: " + p)
        return 1
    print("gpudiag_check: PASS (%s; %d scenes, every FRAME_CRC the model's%s; %d frames captured, %d distinct%s)"
          % (summary.group(2), len(expect), " and a captured frame's" if frames else "", nframes, len(frames), cycles))
    return 0


if __name__ == "__main__":
    sys.exit(main())
