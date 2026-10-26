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
  - the fifth review's cases: a failure shown by a line outside any run (a section's end between two
    runs, or before the first one when the console opened inside a FAIL line, or a FAIL line whose
    spaces were lost) fails; a capture that ends inside the echo line after a whole run (as uart.py
    capture can stop) passes, and one whose last line is not the echo line's start fails;
  - the build: with --build-id, or --manifest (its build_id and source_dirty), a run of another build
    fails, and so do a dirty banner or bitstream line where the build was clean, and a capture whose
    whole runs show no build at all; the sixth review's: a manifest of another design (B6), of a
    gpudiag-named design with another boot ROM, of a failed build, or naming no design fails; a run
    whose banner was garbled but whose bitstream line shows the build passes (B12), and --build-id
    alone does not see a dirty build (B14), as documented;
  - --must-fail passes only a failing run that says what it must;
  - the cube (GPU2): its header and its lines a second after a whole run pass, and so does a capture
    that ends inside one; a line with a torn frame, a missed one, one out of order or fewer than 30
    frames drawn fails, also after an earlier run that a rerun follows; words after them fail; the
    first cube's lines, without frames out of order, are not the cube's lines any more;
    --cube-seconds N wants N lines after the last run.
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
        lines += ["board Arty A7-100T (0x100), SoC 0.4.0, memory map 2, sys 25000000 Hz, pixel clock 25173611 Hz",
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


CUBE = ("cube: a line a second: frames drawn (FLIPs done), shown (frames on the screen), missed (frames not "
        "checked), torn (frames that were none of the cube's 128), out of order (frames that did not follow the one "
        "before)\n")


def cube(n, drawn=60, missed=0, torn=0, at=None, shown=60, order=0):
    """n of the cube's lines a second, the one numbered `at` with the given drawn, shown, missed, torn and out of
    order"""
    out, td, ts, tm, tt, to = "", 0, 0, 0, 0, 0
    for k in range(1, n + 1):
        d, s, m, t, o = (drawn, shown, missed, torn, order) if (at is None or at == k) else (60, 60, 0, 0, 0)
        td, ts, tm, tt, to = td + d, ts + s, tm + m, tt + t, to + o
        out += ("cube %4d s: %3d drawn, %3d shown, %d missed, %d torn, %d out of order (in all %d drawn, %d shown, %d "
                "missed, %d torn, %d out of order)\n" % (k, d, s, m, t, o, td, ts, tm, tt, to))
    return out


def stamped(text, cut_at_summary=False):
    """as tools/uart.py capture writes it (cut at the GPUDIAG line, as --until 'GPUDIAG: ...' stops)"""
    lines = text.split("\n")
    if cut_at_summary:
        lines = lines[:next(i for i, l in enumerate(lines) if l.startswith("GPUDIAG: ")) + 1]
    return ("# uart.py capture /dev/cu.usbserial-X at 115200 8N1, 2026-10-19 10:00:00 UTC; until 'GPUDIAG: "
            "(PASS|FAIL)': matched at +2.000 s\n" + "".join("+%9.3f | %s\n" % (0.01 * i, l) for i, l in enumerate(lines)))


failures = 0
checks = 0


def ending(text, k):
    """text cut k characters after its first GPUDIAG line's ')' (where uart.py capture's last read ended)"""
    i = text.index(")", text.index("GPUDIAG: ")) + 1
    return text[:i + k]


def manifest(build_id="12345678", dirty="false", design="surfcpu_arty_a7_gpudiag", rom="build/fw/gpudiag.mem",
             result="pass"):
    """the lines of tools/fpga-build.sh's manifest.txt that gpudiag_check.py reads (and a section after them,
    whose keys it must not take for the top's)"""
    return ('# SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by tools/fpga-build.sh\n'
            'design = "%s"\nsource_dirty = %s\nbuild_id = "%s"\nrom_image = "%s"\nresult = "%s"\n\n'
            '[openxc7]\nresult = "pass"\n' % (design, dirty, build_id, rom, result))


def expect(name, transcript, want, *extra, man=None):
    global failures, checks
    checks += 1
    with tempfile.TemporaryDirectory() as d:
        u, h = os.path.join(d, "uart.txt"), os.path.join(d, "expect.h")
        with open(u, "wb") as f:
            f.write(transcript.encode("latin-1"))
        with open(h, "w") as f:
            f.write(HEADER)
        if man is not None:
            with open(os.path.join(d, "manifest.txt"), "w") as f:
                f.write(man)
            extra = extra + ("--manifest", os.path.join(d, "manifest.txt"))
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
# the fifth review's cases (gpurev5-tests/capture/adversarial5.py)
SECTION_END = "  gpu registers and errors: 1 of 14 checks failed\n"
expect("X4: a whole run, the echo, a later run's section end alone, then RESET and a whole run",
       run() + ECHO + "\n" + SECTION_END + run() + ECHO, False)
expect("X5: the console opened inside a failing run's FAIL line, RESET in scene 1, then a whole run",
       (lambda b: b[b.index("WAIT_VBLANK"):b.index("scene  1:")])(run(failed_section=True)) + run() + ECHO, False)
expect("a section's end alone before a whole run", SECTION_END + run() + ECHO, False)
expect("a FAIL line whose spaces were lost (the console opened at its F), then RESET and a whole run",
       "FAIL ABORT behind a WAIT_VBLANK: 5 words queued\n" + run() + ECHO, False)
expect("a whole run, then a section's end after the echo", run() + ECHO + SECTION_END, False)
expect("E2: uart.py capture ending inside the echo line ('UA')", stamped(ending(run() + ECHO, 4)), True)
expect("E3: ... ('UART ech')", stamped(ending(run() + ECHO, 10)), True)
expect("E4: ... ('UART echo: ')", stamped(ending(run() + ECHO, 13)), True)
expect("E6: a raw log closed while the echo line arrived ('UART ec')", ending(run() + ECHO, 9), True)
expect("a raw log ending inside the echo line of a rerun after a first run",
       run() + ECHO + ending(run(rerun=True) + ECHO, 6), True)
expect("a capture ending in a line that is not the echo line's start", run() + "\nUART echo! typ", False)
expect("the echo line's start, then more", run() + "\nUART ec\nidentity ok\n", False)
expect("the echo line's start after a failed run", ending(run(crcs=BAD) + ECHO, 6), False)
# the build: --build-id, --manifest
expect("--build-id: the run's build", run() + ECHO, True, "--build-id", "12345678")
expect("B1: --build-id: a run of another build", run() + ECHO, False, "--build-id", "87654321")
expect("--build-id: the build's banner, another bitstream", run().replace("bitstream build 12345678",
       "bitstream build 87654321") + ECHO, False, "--build-id", "12345678")
expect("--build-id: an earlier run of another build, then the build's", run(build="87654321") + ECHO + run() + ECHO,
       False, "--build-id", "12345678")
expect("--build-id: a whole run that shows no build (its start lost)", "identity" + run().split("identity", 1)[1] +
       ECHO, False, "--build-id", "12345678")
expect("--build-id: not 8 hex digits", run() + ECHO, False, "--build-id", "1234567")
expect("--manifest: the run's build", run() + ECHO, True, man=manifest())
expect("--manifest: another build", run() + ECHO, False, man=manifest("87654321"))
expect("--manifest: a dirty banner where the build was clean", run().replace("(build 12345678)",
       "(build 12345678, dirty tree)") + ECHO, False, man=manifest())
expect("--manifest: a dirty bitstream where the build was clean", run().replace("12345678 flags 0",
       "12345678 flags 1") + ECHO, False, man=manifest())
expect("--manifest: a dirty build, as its manifest says", run().replace("(build 12345678)",
       "(build 12345678, dirty tree)").replace("12345678 flags 0", "12345678 flags 1") + ECHO, True,
       man=manifest(dirty="true"))
expect("--manifest without a build_id", run() + ECHO, False, man="result = \"pass\"\n")
# the sixth review's B6: a manifest must be a gpudiag build's, of a build that passed its checks
expect("B6: --manifest: the DDR+GPU design's manifest (the same build id)", run() + ECHO, False,
       man=manifest(design="surfcpu_arty_a7_ddr_gpu", rom="build/fw/surfboot.mem"))
expect("--manifest: a gpudiag-named design with another boot ROM", run() + ECHO, False,
       man=manifest(rom="build/fw/bramdiag.mem"))
expect("--manifest: a build that failed its checks (its flow section's result pass)", run() + ECHO, False,
       man=manifest(result="fail"))
expect("--manifest naming no design", run() + ECHO, False,
       man=manifest().replace('design = "surfcpu_arty_a7_gpudiag"\n', ''))
# B12, B14: what the build rule does not ask, as documented
expect("B12: --manifest: the run's banner garbled, its bitstream line the build's",
       run().replace("(build 12345678)", "(bu\xffld 12345678)") + ECHO, True, man=manifest())
expect("B14: --build-id alone: a dirty build of that id (the manifest would know)",
       run().replace("(build 12345678)", "(build 12345678, dirty tree)").replace("12345678 flags 0", "12345678 flags 1")
       + ECHO, True, "--build-id", "12345678")
expect("--build-id and --manifest that disagree", run() + ECHO, False, "--build-id", "12345678",
       man=manifest("87654321"))
# the cube (GPU2): its lines after a whole run
expect("a whole run, the echo, the cube's header and three seconds", run() + ECHO + CUBE + cube(3), True)
expect("the cube: a torn frame", run() + ECHO + CUBE + cube(3, torn=1, at=2), False)
expect("the cube: a frame not checked", run() + ECHO + CUBE + cube(3, missed=1, at=3), False)
expect("the cube: 29 frames drawn in a second", run() + ECHO + CUBE + cube(3, drawn=29, at=1), False)
# the review of 3fea963: frames out of order (L1: printed and judged since), and the first cube's lines, without them
expect("the cube: a frame out of order", run() + ECHO + CUBE + cube(3, order=1, at=2), False)
expect("the cube: the first cube's line, without frames out of order, then one of today's", run() + ECHO + CUBE +
       "cube    1 s:  60 drawn,  60 shown, 0 missed, 0 torn (in all 60 drawn, 60 shown, 0 missed, 0 torn)\n" + cube(1),
       False)
expect("the cube: the capture ends inside a line", run() + ECHO + CUBE + cube(2) + "cube    3 s:  6", True)
expect("the cube: the capture ends inside the word cube", run() + ECHO + CUBE + cube(2) + "cu", True)
expect("the cube: words after its lines", run() + ECHO + CUBE + cube(2) + "identity ok\n", False)
expect("the cube: an echo among its lines", run() + ECHO + CUBE + cube(1) + "echo   1: 'a' 0x61\n" + cube(2), True)
expect("the cube's torn frame after a run that a rerun follows", run() + ECHO + CUBE + cube(2, torn=2, at=1) +
       run(rerun=True) + ECHO, False)
expect("the cube's lines after a run, then Ctrl-R and a rerun", run() + ECHO + CUBE + cube(2) + run(rerun=True) +
       ECHO + CUBE, True)
expect("--cube-seconds 3: three lines", run() + ECHO + CUBE + cube(3), True, "--cube-seconds", "3")
expect("--cube-seconds 3: two lines", run() + ECHO + CUBE + cube(2), False, "--cube-seconds", "3")
expect("--cube-seconds 1: the lines were the run before's", run() + ECHO + CUBE + cube(2) + run(rerun=True) + ECHO,
       False, "--cube-seconds", "1")
# --must-fail
expect("--must-fail: a failing run that says why", run(skip=True), True, "--must-fail", "skipped: no engine")
expect("--must-fail: a failing run that does not say it", run(skip=True), False, "--must-fail", "gpu.ID reads")
expect("--must-fail: a passing run", run(), False, "--must-fail", "colour bars")
if failures:
    print("test_gpudiag_check: FAIL (%d of %d checks)" % (failures, checks))
    sys.exit(1)
print("test_gpudiag_check: PASS (%d checks of gpudiag_check.py: every whole run's verdict and total, runs cut short "
      "and what they show, the capture's end, reruns, one build, uart.py's layout, the fourth to sixth reviews' "
      "cases, the build to test, --must-fail, the cube's lines)" % checks)
