#!/usr/bin/env python3
"""SurfGPU: tests of sim/gpu/gpudiag_check.py itself (make test-gpu-noengine).

    test_gpudiag_check.py

Made-up captures, laid out as gpudiag prints them, against a made-up gpuscenes_expect.h with two
scenes, 84 checks for a first run and 83 for a rerun:
  - one run: a complete PASS passes; too few checks, a skipped scene or a wrong CRC fail;
  - several runs: every run that reached its GPUDIAG line must have passed (a failed run before a
    good one fails, after Ctrl-R or after RESET), and so must a run a RESET cut short; the capture
    must end with a run that ended (a later run cut off, or one whose banner is garbled behind a
    NUL, fails it), with only the UART echo, blank lines and wordless bytes after it (a 0x00 0xFF
    burst passes, words do not); a RESET during a rerun, then a first run with a garbled banner,
    passes; the runs of two builds (an appended log) fail;
  - reruns: a rerun after a first run passes, also when its rerun line was garbled or not
    captured; a first run's total with the rerun line fails; a capture of reruns only fails, being
    no full board test, unless --accept-rerun, also when the console missed the rerun's start;
  - layouts: tools/uart.py capture's time-stamped lines, whole or cut at the GPUDIAG line, and a
    console opened in the middle of a run, pass; a capture cut inside its GPUDIAG line fails, saying so;
  - the fourth review's cases: an earlier run seen only in part (its tail, its summary alone, a
    rerun's tail) or with a garbled scene line is cut short, not judged, and a whole run after it
    passes; a failed run whose FAIL lines and summary lost bytes still fails by its wrong CRC; a
    lone PASS run with a scene line missing, or one twice and another missing, fails;
  - --must-fail passes only a failing run that says what it must.
Exit 0 when all pass.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECK = os.path.join(HERE, "gpudiag_check.py")
HEADER = """/* made up for test_gpudiag_check.py */
#define GPU_SCENE_COUNT 2
static const uint32_t gpu_scene_expect[GPU_SCENE_COUNT] = {
    0x11111111u,   /*  0 colour bars (MODE 0) */
    0x22222222u,   /*  1 engine: demo */
};
/* the checks of a complete gpudiag run, a first one and a rerun (gpuscenes.h) */
#define GPUDIAG_CHECKS 84u
#define GPUDIAG_RERUN_CHECKS 83u
"""

ECHO = "\nUART echo: type; each character comes back with its code. Ctrl-R runs the diagnostic again.\n"
RERUN = ("a rerun (Ctrl-R): the GFX_ values a reset leaves are checked by the first run after a reset only, so a "
         "complete rerun makes 83 checks")
SCENES = (("colour bars (MODE 0)", "11111111"), ("engine: demo", "22222222"))


def run(crcs=("11111111", "22222222"), summary=None, skip=False, rerun=False, build="12345678", cut_after=None,
        banner=True, failed_section=False):
    """one run as gpudiag prints it; cut_after=k ends it after scene k (a RESET, or the capture's end)"""
    lines = []
    if banner:
        lines += ["", "SurfGPU diagnostic 1.0.0 (build %s)" % build] + ([RERUN] if rerun else [])
        lines += ["board Arty A7-100T (0x100), SoC 0.3.0, memory map 2, sys 25000000 Hz, pixel clock 25173611 Hz",
                  "bitstream build %s flags 0, features 17f" % build]
    lines.append("identity                           ok (5 checks)")
    if failed_section:
        lines += ["gpu registers and errors           ", "  FAIL ABORT behind a WAIT_VBLANK: 5 words queued",
                  "  gpu registers and errors: 1 of 14 checks failed"]
    failures = int(failed_section)
    for n, ((name, want), crc) in enumerate(zip(SCENES, crcs)):
        head = "scene %2d: %-24s " % (n, name)
        if n == 1 and skip:
            lines += [head, "  FAIL skipped: no engine", "  scene  1: %s: 1 of 1 checks failed" % name]
            failures += 1
        elif crc != want:
            lines += [head, "  FAIL FRAME_CRC 0x%s, the model's 0x%s" % (crc, want),
                      "crc %s   scene %2d: %s: 1 of 3 checks failed" % (crc, n, name)]
            failures += 1
        else:
            lines.append(head + "crc %s ok (3 checks)" % crc)
        if cut_after == n:
            return "\n".join(lines) + "\n"
    total = 83 if rerun else 84
    if summary is None:
        summary = "GPUDIAG: FAIL (%d of %d checks failed)" % (failures, total) if failures else \
                  "GPUDIAG: PASS (%d checks)" % total
    return "\n".join(lines + ["", summary, ""])


def stamped(text, cut_at_summary=False):
    """as tools/uart.py capture writes it (cut at the GPUDIAG line, as --until 'GPUDIAG: ...' stops)"""
    lines = text.split("\n")
    if cut_at_summary:
        lines = lines[:next(i for i, l in enumerate(lines) if l.startswith("GPUDIAG: ")) + 1]
    return ("# uart.py capture /dev/cu.usbserial-X at 115200 8N1, 2026-10-19 10:00:00 UTC; until 'GPUDIAG: "
            "(PASS|FAIL)': matched at +2.000 s\n" + "".join("+%9.3f | %s\n" % (0.01 * i, l) for i, l in enumerate(lines)))


failures = 0
checks = 0


def expect(name, transcript, want, *extra):
    global failures, checks
    checks += 1
    with tempfile.TemporaryDirectory() as d:
        u, h = os.path.join(d, "uart.txt"), os.path.join(d, "expect.h")
        with open(u, "wb") as f:
            f.write(transcript.encode("latin-1"))
        with open(h, "w") as f:
            f.write(HEADER)
        r = subprocess.run([sys.executable, CHECK, "--uart", u, "--frames", "-", "--expect", h] + list(extra),
                           capture_output=True, text=True)
    got = r.returncode == 0
    if got != want:
        failures += 1
        print("FAIL %s: gpudiag_check %s, wanted %s:\n%s" % (name, "passed" if got else "failed",
                                                            "pass" if want else "fail", r.stdout.strip()))


BAD = ("11111111", "33333333")
# one run
expect("a complete PASS", run(), True)
expect("PASS with too few checks", run(summary="GPUDIAG: PASS (81 checks)"), False)
expect("a skipped scene", run(skip=True), False)
expect("a wrong CRC", run(crcs=BAD), False)
# several runs: every complete run must pass, and the capture must end with one
expect("a failed run, then RESET and a good one", run(crcs=BAD) + ECHO + run(), False)
expect("a failed run, then Ctrl-R and a good rerun", run(crcs=BAD) + ECHO + run(rerun=True), False)
expect("a good run, then a failed one", run() + ECHO + run(crcs=BAD), False)
expect("a run that printed a FAIL and was reset, then a good run", run(failed_section=True, cut_after=0) + run(), False)
expect("a run reset in the middle (nothing failed), then a good run", run(cut_after=0) + run(), True)
expect("a good run, then the UART echo", run() + ECHO + "echo   1: 'a' 0x61\necho   2: '.' 0x0d\n", True)
expect("a good run, then one that never ended (after a wrong CRC)", run() + ECHO + run(crcs=("44444444", "22222222"),
       cut_after=0), False)
expect("a good run, then one that never ended at once", run() + ECHO + "\nSurfGPU diagnostic 1.0.0 (build 12345678)\n",
       False)
expect("a good run, then RESET, and the capture stopped during the next run", run() + ECHO + run(cut_after=0), False)
expect("a good run, then a failing one whose banner follows a NUL on the echo line",
       run() + ECHO.rstrip("\n") + "\x00" + run(crcs=BAD).lstrip(), False)
expect("a good run, then a 0x00 0xFF burst (the board reprogrammed while logging)", run() + ECHO + "\x00\xff\xff\n\xfe\n",
       True)
expect("a good run, then words that are not the echo", run() + ECHO + "\x00Surf\xc7PU diagnostic 1.0.0 (bui\n", False)
expect("RESET during a rerun, then a first run whose banner was garbled",
       run() + ECHO + run(rerun=True, cut_after=0) + run().replace("SurfGPU diagnostic", "Surf\xc7PU diagnostic"), True)
expect("the runs of two builds (an appended log)", run(build="aaaaaaaa") + ECHO + run(build="bbbbbbbb"), False)
# reruns
expect("a run, then Ctrl-R and a rerun with its line and its total", run() + ECHO + run(rerun=True), True)
expect("a run, then a rerun whose rerun line was garbled", run() + ECHO + run(rerun=True).replace(
       "a rerun (Ctrl-R)", "a rerun (Ctrl-\xd2)"), True)
expect("the first run's total with the rerun line", run(rerun=True, summary="GPUDIAG: PASS (84 checks)"), False)
expect("a rerun alone: not a full board test", run(rerun=True), False)
expect("a rerun alone, with --accept-rerun", run(rerun=True), True, "--accept-rerun")
expect("a rerun whose start the console missed, alone", run(rerun=True, banner=False), False)
expect("a rerun whose start the console missed, with --accept-rerun", run(rerun=True, banner=False), True,
       "--accept-rerun")
# layouts
expect("tools/uart.py capture's lines", stamped(run() + ECHO), True)
expect("tools/uart.py capture's lines, cut at the GPUDIAG line", stamped(run(), cut_at_summary=True), True)
expect("tools/uart.py capture's lines of a failed run", stamped(run(crcs=BAD)), False)
expect("a console opened in the middle of a run", "VRAM window" + run().split("identity", 1)[1], True)
expect("tools/uart.py capture cut inside the GPUDIAG line", stamped(run()).split("(84 checks)")[0], False)
# the fourth review's cases (gpurev4-tests/capture/adversarial4.py)
expect("P2: the tail of a passing run (its last scene, summary, echo), then RESET and a whole run",
       "scene  1:" + run().split("scene  1:", 1)[1] + ECHO + run() + ECHO, True)
expect("P3: only an earlier run's GPUDIAG line and the echo, then RESET and a whole run",
       "GPUDIAG: PASS (84 checks)\n" + ECHO + run() + ECHO, True)
expect("P5: the tail of a passing rerun, then RESET and a whole first run",
       "scene  1:" + run(rerun=True).split("scene  1:", 1)[1] + ECHO + run() + ECHO, True)
expect("G1: a passing run with one byte garbled in a scene line, then RESET and a whole run",
       run().replace("crc 11111111 ok", "crc 11111111 o\xff") + ECHO + run() + ECHO, True)
expect("F1: a failed run whose FAIL line, section end and summary lost bytes (its CRC still wrong), then a whole run",
       run(crcs=BAD).replace("  FAIL FRAME_CRC", "  FA\xffL FRAME_CRC").replace("checks failed", "checks fa\xffled")
       .replace("GPUDIAG: FAIL", "GPUDIA\xff: FAIL") + ECHO + run() + ECHO, False)
expect("F3: a failed run whose summary alone is garbled, then RESET and a whole run",
       run(crcs=BAD).replace("GPUDIAG: FAIL", "GPUDIA\xff: FAIL") + ECHO + run() + ECHO, False)
expect("F6: a lone PASS run with a scene line missing", run().replace(run().split("\n")[6] + "\n", ""), False)
expect("F7: a lone PASS run with a scene line twice and another missing",
       run().replace(run().split("\n")[6], run().split("\n")[5]), False)
# --must-fail
expect("--must-fail: a failing run that says why", run(skip=True), True, "--must-fail", "skipped: no engine")
expect("--must-fail: a failing run that does not say it", run(skip=True), False, "--must-fail", "gpu.ID reads")
expect("--must-fail: a passing run", run(), False, "--must-fail", "colour bars")
if failures:
    print("test_gpudiag_check: FAIL (%d of %d checks)" % (failures, checks))
    sys.exit(1)
print("test_gpudiag_check: PASS (%d checks of gpudiag_check.py: every whole run's verdict and total, runs cut short "
      "and what they show, the capture's end, reruns, one build, uart.py's layout, the fourth review's cases, "
      "--must-fail)" % checks)
