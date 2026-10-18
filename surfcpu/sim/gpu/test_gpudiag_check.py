#!/usr/bin/env python3
"""SurfGPU: tests of sim/gpu/gpudiag_check.py itself (make test-gpu-noengine).

    test_gpudiag_check.py

Made-up transcripts against a made-up gpuscenes_expect.h with two scenes and 84 checks: a
complete PASS passes; PASS with fewer checks than a complete run fails (a run cut short or with a
section skipped); a skipped scene fails; a capture holding two runs is judged by the last complete
one only (a failed run before a good one passes, a good run before a failed one fails, a run cut
off after a good one leaves the good one), never mixing the summary of one with the CRCs of
another; --must-fail passes only a failing run that says what it must. Exit 0 when all pass.
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
/* the checks of a complete gpudiag run (gpuscenes.h) */
#define GPUDIAG_CHECKS 84u
"""


def run(crc0="11111111", crc1="22222222", summary="GPUDIAG: PASS (84 checks)", skip=False):
    lines = ["", "SurfGPU diagnostic 1.0.0 (build 12345678)", "identity                           ok (5 checks)",
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
expect("a good run, then one cut off after a wrong CRC (the good run's summary with the cut run's CRC: no)",
       run() + run(crc0="44444444").split("scene  1")[0], True)
expect("a good run, then one cut off at once", run() + "\nSurfGPU diagnostic 1.0.0 (build 12345678)\nidentity  ", True)
expect("--must-fail: a failing run that says why", run(skip=True, summary="GPUDIAG: FAIL (1 of 82 checks failed)"),
       True, "--must-fail", "skipped: no engine")
expect("--must-fail: a failing run that does not say it", run(skip=True, summary="GPUDIAG: FAIL (1 of 82 checks failed)"),
       False, "--must-fail", "gpu.ID reads")
expect("--must-fail: a passing run", run(), False, "--must-fail", "colour bars")
if failures:
    print("test_gpudiag_check: FAIL (%d of %d checks)" % (failures, checks))
    sys.exit(1)
print("test_gpudiag_check: PASS (%d checks of gpudiag_check.py: the check total, skipped scenes, one run of several, "
      "--must-fail)" % checks)
