#!/usr/bin/env python3
"""SurfGPU: check a gpudiag run (docs/GPU.md section 10).

    gpudiag_check.py --uart TRANSCRIPT --frames FRAMES --expect gpuscenes_expect.h [--log LOG]
                     [--engine-scenes N] [--must-fail REGEX]

TRANSCRIPT is what gpudiag printed (surfsim --transcript, or a board's UART capture); FRAMES the
CRC of every frame captured at the VGA pins (surfsim --vga-crc: "frame N crc XXXXXXXX"), or "-"
for none (a board); gpuscenes_expect.h the CRCs the C model gave each scene at build time and the
number of checks of a complete run (GPUDIAG_CHECKS, and GPUDIAG_RERUN_CHECKS for a rerun, which
cannot see reset values and says so). A capture may hold several runs (Ctrl-R, a reset): the one
judged is the last, from its banner to the last GPUDIAG line, and after that line only the UART
echo may follow; anything else is a later run that did not end, which fails the capture.

Passes when that run says PASS with exactly its kind's number of checks (a run cut short, or one
that skipped a section, cannot pass), skipped nothing, printed one CRC for every scene of the
header, each equal to the model's, and, with FRAMES, each also the CRC of a frame captured at the
pins (so FRAME_CRC is the picture that left the chip). --engine-scenes N also requires that N scenes
needed the engine and passed. --must-fail REGEX turns it round, for the tests of a SoC that must
fail gpudiag (sim/gpu, make test-gpu-noengine): it passes only when the run says FAIL, this check
fails it too, and the run's text matches REGEX. Exit 0 = pass. Only the Python standard library.
"""

import argparse
import re
import sys

BANNER = re.compile(r"SurfGPU diagnostic ")         # anywhere in a line: a garbled start may precede it
SUMMARY = re.compile(r"GPUDIAG: (PASS|FAIL) \(([^)]*)\)")
ECHO = re.compile(r"(UART echo: .*|echo +\d+: .*)?")  # what gpudiag prints after its GPUDIAG line
RERUN = "a rerun (Ctrl-R)"


def final_run(text: str) -> tuple[str, list[str]]:
    """(the run that ends the capture, from its banner to the last GPUDIAG line; what is wrong with
    the capture's end): after that line only the UART echo may follow, anything else is a later run
    that never reached its own GPUDIAG line"""
    sums = list(SUMMARY.finditer(text))
    if not sums:
        return text, []
    last = sums[-1]
    problems = []
    for line in text[last.end():].split("\n")[1:]:
        if not ECHO.fullmatch(line.strip()):
            problems.append("a run after the last GPUDIAG line did not end (it printed %r)" % line.strip()[:60])
            break
    prev = sums[-2].end() if len(sums) > 1 else 0
    starts = [m.start() for m in BANNER.finditer(text, prev, last.start())]
    return text[starts[-1] if starts else prev:last.end()], problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--expect", help="gpuscenes_expect.h; without it, gpudiag's own comparisons are taken")
    ap.add_argument("--log")
    ap.add_argument("--engine-scenes", type=int)
    ap.add_argument("--must-fail", metavar="REGEX")
    a = ap.parse_args()

    text, problems = final_run(open(a.uart, errors="replace").read().replace("\r", ""))
    rerun = RERUN in text
    expect = {}
    total = None
    if a.expect:
        header = open(a.expect).read()
        for m in re.finditer(r"0x([0-9a-f]{8})u,\s+/\*\s*(\d+) (.*?) \*/", header):
            expect[int(m.group(2))] = (int(m.group(1), 16), m.group(3))
        m = re.search(r"#define %s (\d+)u" % ("GPUDIAG_RERUN_CHECKS" if rerun else "GPUDIAG_CHECKS"), header)
        total = int(m.group(1)) if m else None
    frames = set()
    nframes = 0
    if a.frames != "-":
        for line in open(a.frames):
            m = re.match(r"frame \d+ crc ([0-9a-f]{8})", line)
            if m:
                frames.add(int(m.group(1), 16))
                nframes += 1

    summary = SUMMARY.search(text)
    if not summary:
        problems.append("no GPUDIAG summary line")
    elif summary.group(1) != "PASS":
        problems.append("gpudiag says FAIL (%s)" % summary.group(2))
    elif total is not None:
        m = re.fullmatch(r"(\d+) checks", summary.group(2))
        if not m or int(m.group(1)) != total:
            problems.append("gpudiag says PASS (%s), but a complete %s makes %d checks" % (
                summary.group(2), "rerun" if rerun else "run", total))
    if a.expect and total is None:
        problems.append("no %s in %s" % ("GPUDIAG_RERUN_CHECKS" if rerun else "GPUDIAG_CHECKS", a.expect))
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
    print("gpudiag_check: PASS (%s%s; %d scenes, every FRAME_CRC the model's%s; %d frames captured, %d distinct%s)"
          % (summary.group(2), ", a rerun" if rerun else "", len(expect), " and a captured frame's" if frames else "",
             nframes, len(frames), cycles))
    return 0


if __name__ == "__main__":
    sys.exit(main())
