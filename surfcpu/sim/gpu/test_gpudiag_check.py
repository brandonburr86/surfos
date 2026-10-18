#!/usr/bin/env python3
"""SurfGPU: tests of sim/gpu/gpudiag_check.py itself (make test-gpu-noengine).

    test_gpudiag_check.py

Made-up transcripts against a made-up gpuscenes_expect.h with two scenes, 84 checks for a first
run and 83 for a rerun: a complete PASS passes, with the UART echo after it too; PASS with fewer
checks than a complete run of its kind fails (a run cut short or with a section skipped), and so
does a rerun's total without the rerun line or the first run's with it; a skipped scene fails; in
a capture holding several runs the last decides (a failed run before a good one passes, a good
run before a failed one fails), and a later run that never reached its GPUDIAG line fails the
capture, also when its banner is garbled; --must-fail passes only a failing run that says what it
must. Exit 0 when all pass.
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


def run(crc0="11111111", crc1="22222222", summary="GPUDIAG: PASS (84 checks)", skip=False, rerun=False):
    lines = ["", "SurfGPU diagnostic 1.0.0 (build 12345678)"] + ([RERUN] if rerun else []) + [
             "identity                           ok (5 checks)",
             "scene  0: colour bars (MODE 0)     crc %s ok (3 checks)" % crc0]
    if skip:
        lines += ["scene  1: engine: demo             ", "  FAIL skipped: no engine"]
    else:
        lines.append("scene  1: engine: demo             crc %s ok (3 checks)" % crc1)
    return "\n".join(lines + ["", summary, ""])


failures = 0
checks = 0


def expect(name, transcript, want, *extra):
    global failures, checks
    checks += 1
    with tempfile.TemporaryDirectory() as d:
        u, h = os.path.join(d, "uart.txt"), os.path.join(d, "expect.h")
        open(u, "w").write(transcript)
        open(h, "w").write(HEADER)
        r = subprocess.run([sys.executable, CHECK, "--uart", u, "--frames", "-", "--expect", h] + list(extra),
                           capture_output=True, text=True)
    got = r.returncode == 0
    if got != want:
        failures += 1
        print("FAIL %s: gpudiag_check %s, wanted %s:\n%s" % (name, "passed" if got else "failed",
                                                            "pass" if want else "fail", r.stdout.strip()))


expect("a complete PASS", run(), True)
expect("PASS with too few checks", run(summary="GPUDIAG: PASS (81 checks)"), False)
expect("a skipped scene", run(skip=True, summary="GPUDIAG: FAIL (1 of 82 checks failed)"), False)
expect("a wrong CRC", run(crc1="33333333", summary="GPUDIAG: FAIL (1 of 84 checks failed)"), False)
expect("a failed run, then a good one", run(crc1="33333333", summary="GPUDIAG: FAIL (1 of 84 checks failed)") + run(),
       True)
expect("a good run, then a failed one", run() + run(crc1="33333333", summary="GPUDIAG: FAIL (1 of 84 checks failed)"),
       False)
expect("a good run, then the UART echo", run() + ECHO + "echo   1: 'a' 0x61\necho   2: '.' 0x0d\n", True)
expect("a good run, then one that never ended (after a wrong CRC)", run() + ECHO + run(crc0="44444444").split("scene  1")[0],
       False)
expect("a good run, then one that never ended at once", run() + ECHO + "\nSurfGPU diagnostic 1.0.0 (build 12345678)\n",
       False)
expect("a good run, then a failing one whose banner follows a NUL on the echo line",
       run() + ECHO.rstrip("\n") + "\x00" + run(crc1="33333333", summary="GPUDIAG: FAIL (1 of 84 checks failed)").lstrip(),
       False)
expect("a rerun, with its line and its total", run() + ECHO + run(rerun=True, summary="GPUDIAG: PASS (83 checks)"), True)
expect("a rerun's total without the rerun line", run(summary="GPUDIAG: PASS (83 checks)"), False)
expect("the first run's total with the rerun line", run(rerun=True), False)
expect("--must-fail: a failing run that says why", run(skip=True, summary="GPUDIAG: FAIL (1 of 82 checks failed)"),
       True, "--must-fail", "skipped: no engine")
expect("--must-fail: a failing run that does not say it", run(skip=True, summary="GPUDIAG: FAIL (1 of 82 checks failed)"),
       False, "--must-fail", "gpu.ID reads")
expect("--must-fail: a passing run", run(), False, "--must-fail", "colour bars")
if failures:
    print("test_gpudiag_check: FAIL (%d of %d checks)" % (failures, checks))
    sys.exit(1)
print("test_gpudiag_check: PASS (%d checks of gpudiag_check.py: the check totals of a run and a rerun, skipped "
      "scenes, the capture's final run, --must-fail)" % checks)
