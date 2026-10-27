#!/usr/bin/env python3
"""SurfGPU: check a gpudiag capture (docs/GPU.md sections 10 and 14).

    gpudiag_check.py --uart CAPTURE --frames FRAMES --expect gpuscenes_expect.h [--log LOG]
                     [--build-id HHHHHHHH | --manifest MANIFEST] [--engine-scenes N] [--accept-rerun]
                     [--cube-seconds N] [--must-fail REGEX]

CAPTURE is what the UART carried: surfsim --transcript, a board's raw log (make uart-console
LOG=<file>, which appends to the file), or tools/uart.py capture's lines, whose time stamps
("+    0.240 | ") and header line are taken off. FRAMES is the CRC of every frame captured at the
VGA pins (surfsim --vga-crc: "frame N crc XXXXXXXX"), or "-" for none (a board). gpuscenes_expect.h
holds the CRC the C model gave each scene at build time and the checks of a complete first run
(GPUDIAG_CHECKS) and of a complete rerun (GPUDIAG_RERUN_CHECKS): a rerun (Ctrl-R) cannot see the
values a reset leaves, and says so in a line under its banner.

A capture may hold several runs (a RESET, a Ctrl-R rerun, a console opened in the middle of one).
A run starts at its banner ("SurfGPU diagnostic", anywhere in a line), or, where the banner was lost
or garbled, at the first line of a run that comes (a "board", "bitstream build" or "identity" line
again, or any line of a run after the last one ended), and ends at its GPUDIAG line. A run that
reached its GPUDIAG line with PASS and a passing CRC line for every scene of the header is whole;
one that did not (the console opened after its start, a line lost or garbled, a RESET before its
end) is cut short and not judged, but whatever it shows of a failure counts. The capture passes
only when:
  - no line of the capture shows a failure, wherever it falls (in a run, whole or cut short, before
    the first run or between two): a FAIL line, a section's "k of n checks failed", GPUDIAG: FAIL, or
    a scene's FRAME_CRC other than the model's;
  - the capture ends with a whole run: after its GPUDIAG line only the UART echo, the cube's lines
    (GPU2, docs/GPU.md section 16.9: after a PASS gpudiag spins the cube and prints its header line,
    then a line a second), blank lines and bytes without a word in them (a 0x00 0xFF burst while the
    board is reprogrammed) may follow, and the capture may end inside the echo line or a cube line
    (uart.py capture stops where its last read ended); anything else is a later run that did not end;
  - every cube line, wherever it is, shows no torn frame (a frame whose FRAME_CRC is none of the
    cube's), no missed one (shown but not checked), none out of order (one that cannot follow the
    frame shown before it), 58 to 61 frames shown (the video frames counted in its second, 59.94 a
    second: 59 or 60, one fewer or more where a look falls by the second's edge; so 0 shown, no
    frame checked, fails), its new frames at most 2 video frames apart (the rule gpudiag's checks
    apply to frames 2 to 15: a frame every second video frame or sooner, 29.97 a second on this
    59.94 Hz display, the brief's 30 or more; "apart" also counts the video frames since the
    second's last new frame, so a cube that stopped fails) and at least 29 frames drawn (which
    that rule makes of any second: a frame 2 video frames after the one before gives 29 or 30 in
    a second); with --cube-seconds N at least N of them follow the last whole run (the board test
    of the cube);
  - every whole run has the checks of a complete first run or of a complete rerun (a run of the
    rerun's total is a rerun, whether or not its rerun line arrived; one whose rerun line arrived
    must have that total), and, with FRAMES, each scene's CRC is also a captured frame's (so
    FRAME_CRC is the picture that left the chip);
  - every run comes from one build (an appended log that still holds an older build's runs fails),
    and with --build-id or --manifest from the build to test: no run's banner or bitstream line
    names another build, and one whole run at least shows this one (a run whose identity lines were
    lost or garbled shows none, and is not held against it). --manifest is tools/fpga-build.sh's
    manifest.txt of the bitstream tested: it must be a gpudiag build's (its design gpudiag, its
    boot ROM gpudiag.mem) that passed its build checks (result "pass"), of a manifest_format this
    script reads (none or 1, or 2: a later one may mean something read wrongly here, and is refused,
    as tools/program.sh refuses it); it gives the build id, and,
    when it says the tree was clean (source_dirty false), no run may show a dirty banner or
    bitstream line. --build-id alone gives only the id: it does not know whether the build was clean;
  - one complete run at least is a first run: a rerun does not check the GFX_ values a reset
    leaves, so it is not a full board test; --accept-rerun accepts a capture of reruns only.
--engine-scenes N also requires that the last complete run passed N scenes that needed the engine.
--must-fail REGEX turns it round, for the tests of a SoC that must fail gpudiag (make
test-gpu-noengine): it passes only when the last complete run says FAIL, this check fails the
capture too, and that run's text matches REGEX. Exit 0 = pass. Only the Python standard library.
"""

import argparse
import re
import sys

BANNER = re.compile(r"SurfGPU diagnostic ")             # anywhere in a line: a garbled start may precede it
SUMMARY = re.compile(r"GPUDIAG: (PASS|FAIL) \(([^)]*)\)")
ECHO = re.compile(r"UART echo: .*|echo +\d+: .*")      # what gpudiag prints after its GPUDIAG line
ECHO_LINE = "UART echo: type; each character comes back with its code. Ctrl-R runs the diagnostic again."
RERUN = "a rerun (Ctrl-R)"
STAMP = re.compile(r"^\+ *\d+\.\d+ \| ?")              # tools/uart.py capture: "+    0.240 | text"
CAPTURE_HEAD = re.compile(r"^# uart\.py capture ")
LEADING_JUNK = re.compile(r"^[^ -~]+")                  # NULs, U+FFFD, bytes a reset or a garbled start left
FAILED = re.compile(r"^\s*FAIL\b|\bchecks? failed\b")     # a FAIL line (its spaces lost too), a section's end
START = (("board", re.compile(r"board .*\(0x[0-9a-f]+\), SoC ")),
         ("bitstream", re.compile(r"bitstream build [0-9a-f]{8} ")),
         ("identity", re.compile(r"identity\s")))
RUN_LINE = re.compile(r"(board |bitstream build |identity\s|VRAM window|video graphics registers|gpu registers|"
                      r"gpu2 commands|scenes\s|scene +\d+:|cube: \d+ frames at full rate|completeness|"
                      r"crc [0-9a-f]{8} |a rerun \(Ctrl-R\)|\s+FAIL |GPUDIAG: )")
WORD = re.compile(r"[A-Za-z]{2}")
BUILD_BANNER = re.compile(r"SurfGPU diagnostic \S+ \(build ([0-9a-f]{8})")
BUILD_BIT = re.compile(r"bitstream build ([0-9a-f]{8}) ")
BUILD_DIRTY = re.compile(r"SurfGPU diagnostic \S+ \(build [0-9a-f]{8}, dirty tree\)")
BIT_FLAGS = re.compile(r"bitstream build [0-9a-f]{8} flags ([0-9a-f]+)\b")   # bit 0: DIRTY (spec/soc.toml)
SCENE_OK = re.compile(r"^scene +(\d+): (.*?) +crc ([0-9a-f]{8}) ok")
SCENE_FAILED = re.compile(r"^crc ([0-9a-f]{8}) +scene +(\d+): ")     # a scene with a failed check (report.c)
CUT_SUMMARY = re.compile(r"GPUDIAG: (PASS|FAIL)\b(?! \([^)]*\))")
# the cube's free run after a PASS (gpudiag.c cube_free_run)
CUBE_HEAD = re.compile(r"cube: a line a second: .*")
CUBE_LINE = re.compile(r"cube +(\d+) s: +(\d+) drawn, +(\d+) shown, (\d+) apart, (\d+) missed, (\d+) torn, "
                       r"(\d+) out of order \(in all (\d+) drawn, (\d+) shown, (\d+) apart, (\d+) missed, (\d+) torn, "
                       r"(\d+) out of order\)")
CUBE_APART = 2                                          # video frames from a new frame to the next, at most
CUBE_DRAWN = 29                                         # frames drawn a second, at least: what CUBE_APART makes of a
                                                        # second (59.94 video frames: 29 or 30 a frame every two)
CUBE_SHOWN = (58, 61)                                   # frames shown (counted) in a second: 59 or 60, give or take one


class Run:
    """one gpudiag run as the capture shows it"""

    def __init__(self, banner: bool):
        self.banner = banner
        self.lines = []
        self.kinds = set()          # which of its start lines (board, bitstream, identity) it printed
        self.rerun_line = False
        self.summary = None
        self.failed = []
        self.after = []             # what came after its GPUDIAG line, outside any run
        self.kind = None            # "first" or "rerun", from its total (or its rerun line)
        self.checks = None

    def add(self, line: str) -> None:
        self.lines.append(line)
        self.kinds.update(k for k, rx in START if rx.match(line))
        if RERUN in line:
            self.rerun_line = True
        if FAILED.search(line):
            self.failed.append(line.strip())
        m = SUMMARY.search(line)
        if m and self.summary is None:
            self.summary = m

    def text(self) -> str:
        return "\n".join(self.lines) + "\n"

    def crcs(self) -> tuple[dict, dict]:
        """(the CRC of each scene that passed, the CRC printed for each scene with a failed check)"""
        ok, failed = {}, {}
        for line in self.lines:
            m = SCENE_OK.match(line)
            if m:
                ok.setdefault(int(m.group(1)), int(m.group(3), 16))
            m = SCENE_FAILED.match(line)
            if m:
                failed.setdefault(int(m.group(2)), int(m.group(1), 16))
        return ok, failed


def split_runs(text: str) -> tuple[list, list]:
    """(the runs of a capture in order, the lines outside any run: before the first, after a run's end)"""
    runs, outside, cur = [], [], None
    for raw in text.replace("\r", "").split("\n"):
        if CAPTURE_HEAD.match(raw):
            continue
        line = LEADING_JUNK.sub("", STAMP.sub("", raw, count=1))
        if BANNER.search(line):
            cur = Run(banner=True)
            runs.append(cur)
        else:
            kind = next((k for k, rx in START if rx.match(line)), None)
            done = cur is None or cur.summary is not None
            if (kind and (done or kind in cur.kinds)) or (done and RUN_LINE.match(line)):
                cur = Run(banner=False)             # its banner lost or garbled: it starts here
                runs.append(cur)
            elif done:
                if cur is not None:
                    cur.after.append(line)
                outside.append(line)
                continue
        cur.add(line)
    return runs, outside


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", required=True)
    ap.add_argument("--frames", required=True)
    ap.add_argument("--expect", help="gpuscenes_expect.h; without it, gpudiag's own comparisons are taken")
    ap.add_argument("--log")
    ap.add_argument("--engine-scenes", type=int)
    ap.add_argument("--accept-rerun", action="store_true",
                    help="accept a capture whose complete runs are all reruns (not a full board test)")
    ap.add_argument("--must-fail", metavar="REGEX")
    ap.add_argument("--cube-seconds", type=int, metavar="N",
                    help="at least N of the cube's lines a second after the last whole run (the board test)")
    ap.add_argument("--build-id", metavar="HHHHHHHH", help="the build every run must show (banner, bitstream line)")
    ap.add_argument("--manifest", help="tools/fpga-build.sh's manifest.txt of the bitstream tested: its build_id "
                    "(and source_dirty) is the build every run must show")
    a = ap.parse_args()

    runs, outside = split_runs(open(a.uart, errors="replace").read())
    expect = {}
    first_total = rerun_total = None
    if a.expect:
        header = open(a.expect).read()
        for m in re.finditer(r"0x([0-9a-f]{8})u,\s+/\*\s*(\d+) (.*?) \*/", header):
            expect[int(m.group(2))] = (int(m.group(1), 16), m.group(3))
        m = re.search(r"#define GPUDIAG_CHECKS (\d+)u", header)
        first_total = int(m.group(1)) if m else None
        m = re.search(r"#define GPUDIAG_RERUN_CHECKS (\d+)u", header)
        rerun_total = int(m.group(1)) if m else None
    frames = set()
    nframes = 0
    if a.frames != "-":
        for line in open(a.frames):
            m = re.match(r"frame \d+ crc ([0-9a-f]{8})", line)
            if m:
                frames.add(int(m.group(1), 16))
                nframes += 1

    problems = []
    # the build to test: --build-id, or the bitstream's manifest
    want_id, want_clean = None, False
    if a.build_id is not None:
        want_id = a.build_id.lower()
        if not re.fullmatch(r"[0-9a-f]{8}", want_id):
            problems.append("--build-id %r is not 8 hex digits (gpudiag's banner shows the commit's first 8)" % a.build_id)
    if a.manifest:
        man = open(a.manifest, errors="replace").read()
        # a gpudiag build's manifest, of a build that passed its checks: another design's (the DDR+GPU
        # one, built at the same commit, has the same build id) or a failed build is not the bitstream
        # this capture can be judged against (the sixth review's B6)
        top = man.split("\n[", 1)[0]
        fmt = re.search(r'^manifest_format = (.*)$', top, re.M)
        if fmt and fmt.group(1).strip().strip('"') not in ("1", "2"):
            problems.append("%s is manifest_format %s, which this script does not know (it reads 1 and 2)"
                            % (a.manifest, fmt.group(1).strip()))
        design = re.search(r'^design = "([^"]*)"', top, re.M)
        rom = re.search(r'^rom_image = "([^"]*)"', top, re.M)
        result = re.search(r'^result = "([^"]*)"', top, re.M)
        if not (design and "gpudiag" in design.group(1) and rom and rom.group(1).split("/")[-1] == "gpudiag.mem"):
            problems.append("%s is not a gpudiag build's manifest (design %s, boot ROM %s): give the manifest of "
                            "the gpudiag bitstream tested, build/fpga/<board>/surfcpu_arty_a7_gpudiag/manifest.txt"
                            % (a.manifest, design.group(1) if design else "not named",
                               rom.group(1) if rom else "not named"))
        if not result or result.group(1) != "pass":
            problems.append("%s says result = %s: that bitstream did not pass its build checks"
                            % (a.manifest, result.group(1) if result else "nothing"))
        m = re.search(r'^build_id = "([0-9a-f]{8})"', top, re.M)
        if not m:
            problems.append("no build_id in %s (tools/fpga-build.sh's manifest.txt of the bitstream)" % a.manifest)
        elif want_id is not None and want_id != m.group(1):
            problems.append("--build-id %s, but %s says build_id %s" % (want_id, a.manifest, m.group(1)))
        else:
            want_id = m.group(1)
        want_clean = re.search(r"^source_dirty = false$", top, re.M) is not None
    if a.expect and (first_total is None or rerun_total is None):
        problems.append("no GPUDIAG_CHECKS and GPUDIAG_RERUN_CHECKS in %s" % a.expect)
    if a.expect and not expect:
        problems.append("no expected CRCs in %s" % a.expect)
    done = [r for r in runs if r.summary]
    count = len(runs)

    def which(r: Run) -> str:
        return "run %d of %d" % (runs.index(r) + 1, count) if count > 1 else "the run"

    tail = next((l for r in reversed(runs) for l in reversed(r.lines) if l.strip()), "")
    cut_tail = bool(CUT_SUMMARY.search(tail)) and not SUMMARY.search(tail)
    if not runs:
        problems.append("no gpudiag output in the capture")
    elif cut_tail:                                  # tools/uart.py capture --until matched half the line
        problems.append("the capture ends in the middle of its GPUDIAG line (%r): capture until the echo line "
                        "after it is whole, --until 'GPUDIAG: (PASS|FAIL) \\([^)]*\\)\\n+UART echo: [^\\n]*\\n'"
                        % tail.strip()[:40])
    elif not done:
        problems.append("no GPUDIAG summary line: no run ended")
    # every run's failures, whole or cut short
    for r in runs:
        if r.summary and r.summary.group(1) == "FAIL":
            problems.append("%s says FAIL (%s)%s" % (which(r), r.summary.group(2),
                                                     ": %s" % r.failed[0] if r.failed else ""))
        elif r.failed:
            problems.append("%s%s printed a failed check: %s" % (which(r), "" if r.summary else ", cut short,",
                                                                r.failed[0]))
        ok, failed = r.crcs()
        for sn, crc in sorted(list(ok.items()) + list(failed.items())):
            if sn in expect and crc != expect[sn][0]:
                problems.append("%s printed scene %d's FRAME_CRC %08x, the model's %08x" % (which(r), sn, crc,
                                                                                         expect[sn][0]))
                break
    # and every line outside the runs (before the first, between two, after the last): a section's end
    # whose run was otherwise lost still shows a failure
    stray = next((l.strip() for l in outside if FAILED.search(l) or "GPUDIAG: FAIL" in l), None)
    if stray is not None:
        problems.append("a line outside any run shows a failure: %r" % stray[:80])
    # the cube's lines (outside the runs): no torn, missed or out-of-order frame, every video frame of the
    # second counted and checked, each new frame at most 2 video frames after the one before (gpudiag's own
    # rule for its checked frames), and so 29 frames drawn a second or more
    for l in outside:
        m = CUBE_LINE.search(l)
        if not m:
            continue
        sec, drawn, shown, apart, missed, torn, order = (int(m.group(k)) for k in range(1, 8))
        why = ("a torn frame" if torn else "a frame not checked" if missed else "a frame out of order" if order else
               "the video frames of a second are 59 or 60" if not CUBE_SHOWN[0] <= shown <= CUBE_SHOWN[1] else
               "a new frame %d video frames after the one before, %d at most: a frame every second video frame"
               % (apart, CUBE_APART) if apart > CUBE_APART else
               "fewer than %d frames a second" % CUBE_DRAWN if drawn < CUBE_DRAWN else None)
        if why:
            problems.append("the cube's second %d: %d frames drawn, %d shown, %d apart, %d missed, %d torn, %d out of "
                            "order (%s)" % (sec, drawn, shown, apart, missed, torn, order, why))
            break

    def whole(r: Run) -> bool:
        ok, _ = r.crcs()
        return bool(r.summary) and r.summary.group(1) == "PASS" and (all(n in ok for n in expect) if expect else bool(ok))
    judged = [r for r in done if whole(r)]
    # the capture must end with a whole run
    if runs and not runs[-1].summary and done and not cut_tail:
        first = next((l.strip() for l in runs[-1].lines if l.strip()), "")
        problems.append("a run after the last GPUDIAG line did not end (it printed %r)" % first[:60])
    if runs and runs[-1].summary:
        after = [l.strip() for l in runs[-1].after]
        end = max((i for i, s in enumerate(after) if s), default=-1)       # the capture's last line with text
        for i, s in enumerate(after):
            if not s or ECHO.fullmatch(s) or not WORD.search(s) or CUBE_HEAD.fullmatch(s) or CUBE_LINE.fullmatch(s):
                continue
            if i == end and (ECHO_LINE.startswith(s) or ((s.startswith("cube ") or "cube ".startswith(s)) and
                                                         not CUBE_LINE.match(s))) and \
                    whole(runs[-1]):
                continue                            # the echo line or a cube line, cut where the capture ended
            problems.append("after the last GPUDIAG line: %r (a run that did not end?)" % s[:60])
            break
        if a.cube_seconds is not None:
            n_cube = sum(1 for l in runs[-1].after if CUBE_LINE.search(l))
            if n_cube < a.cube_seconds:
                problems.append("%d of the cube's lines after the last run, %d wanted (--cube-seconds): capture "
                                "longer" % (n_cube, a.cube_seconds))
        last = runs[-1]
        if last.summary.group(1) == "PASS" and not whole(last):
            ok, _ = last.crcs()
            if not expect and not ok:
                problems.append("%s: no scene printed a CRC" % which(last))
            for sn, (crc, name) in sorted(expect.items()):
                if sn not in ok:
                    problems.append("%s: scene %d (%s): no CRC printed" % (which(last), sn, name))
    # each whole run's kind and total, and its scenes against the frames
    for r in judged:
        m = re.fullmatch(r"(\d+) checks", r.summary.group(2))
        if not m:
            problems.append("%s says PASS (%s), not a number of checks" % (which(r), r.summary.group(2)))
            continue
        r.checks = n = int(m.group(1))
        if first_total is None or rerun_total is None:
            r.kind = "rerun" if r.rerun_line else "first"
        elif n == rerun_total:
            r.kind = "rerun"                        # its rerun line may have been garbled, or not captured
        elif n == first_total and not r.rerun_line:
            r.kind = "first"
        elif n == first_total:
            problems.append("%s says it is a rerun but made %d checks, a first run's (a complete rerun makes %d)"
                            % (which(r), n, rerun_total))
        else:
            problems.append("%s says PASS (%d checks), but a complete run makes %d, a complete rerun %d"
                            % (which(r), n, first_total, rerun_total))
        ok, _ = r.crcs()
        for sn, (crc, name) in sorted(expect.items()):
            if frames and ok.get(sn) not in frames:
                problems.append("%s: scene %d (%s): no captured frame has CRC %08x" % (which(r), sn, name, ok[sn]))
        if not a.expect:
            expect.update({sn: (crc, "scene %d" % sn) for sn, crc in ok.items() if sn not in expect})
    # one build, and a first run among them
    for what, rx in (("banners", BUILD_BANNER), ("bitstream lines", BUILD_BIT)):
        ids = sorted({m.group(1) for r in runs for l in r.lines for m in [rx.search(l)] if m})
        if len(ids) > 1:
            problems.append("the runs' %s name more than one build (%s): an appended log? Capture each test into "
                            "a new file" % (what, ", ".join(ids)))
    if want_id is not None:
        for r in runs:
            ids = {m.group(1) for l in r.lines for rx in (BUILD_BANNER, BUILD_BIT) for m in [rx.search(l)] if m}
            if ids - {want_id}:
                problems.append("%s shows build %s, not %s, the build to test: another gpudiag or bitstream"
                                % (which(r), ", ".join(sorted(ids - {want_id})), want_id))
            elif want_clean and (any(BUILD_DIRTY.search(l) for l in r.lines) or
                                 any(int(m.group(1), 16) & 1 for l in r.lines for m in [BIT_FLAGS.search(l)] if m)):
                problems.append("%s shows build %s from a tree with uncommitted changes, but the build to test "
                                "is a clean one" % (which(r), want_id))
        if judged and not any(BUILD_BANNER.search(l) or BUILD_BIT.search(l) for r in judged for l in r.lines):
            problems.append("no whole run shows its build (banner or bitstream line), so none is known to be "
                            "build %s" % want_id)
    passed = [r for r in judged if r.kind]
    if passed and not any(r.kind == "first" for r in passed) and not a.accept_rerun:
        problems.append("every complete run is a rerun (Ctrl-R, %d checks): a rerun does not check the GFX_ values a "
                        "reset leaves, so it is not a full board test; press RESET and capture the first run "
                        "(--accept-rerun accepts a rerun)" % passed[-1].checks)
    last = done[-1] if done else None
    if a.engine_scenes is not None and last is not None:
        n_eng = len(re.findall(r"^scene +\d+: engine:.* crc [0-9a-f]{8} ok", last.text(), re.M))
        if n_eng != a.engine_scenes:
            problems.append("%d engine scenes passed, %d expected" % (n_eng, a.engine_scenes))
    cycles = ""
    if a.log:
        m = re.search(r"after (\d+) cycles", open(a.log, errors="replace").read())
        if m:
            cycles = ", %s cycles" % m.group(1)

    if a.must_fail is not None:
        why = []
        if last is None or last.summary.group(1) != "FAIL":
            why.append("gpudiag did not say FAIL (%s)" % (last.summary.group(0) if last else "no summary line"))
        if not problems:
            why.append("this check would have passed the run")
        if last is None or not re.search(a.must_fail, last.text()):
            why.append("the run does not say %r" % a.must_fail)
        if why:
            for w in why:
                print("gpudiag_check: FAIL (--must-fail): " + w)
            return 1
        print("gpudiag_check: PASS (--must-fail: %s, %d problems found, %r said)" % (last.summary.group(0),
                                                                                    len(problems), a.must_fail))
        return 0
    if problems:
        for p in problems:
            print("gpudiag_check: FAIL: " + p)
        return 1
    desc = ", then ".join("%d checks%s" % (r.checks, ", a rerun" if r.kind == "rerun" else "") for r in judged)
    cut = count - len(judged)
    cube = [m for l in outside for m in [CUBE_LINE.search(l)] if m]
    cube_desc = ""
    if cube:
        cube_desc = "; the cube %d seconds, %d to %d frames drawn and %d to %d shown a second, %d apart at most, " \
                    "none torn, missed or out of order" % (
                        len(cube), min(int(m.group(2)) for m in cube), max(int(m.group(2)) for m in cube),
                        min(int(m.group(3)) for m in cube), max(int(m.group(3)) for m in cube),
                        max(int(m.group(4)) for m in cube))
    print("gpudiag_check: PASS (%s; %d scenes%s, every FRAME_CRC the model's%s; %s%d frames captured, %d distinct%s%s)"
          % (desc, len(expect), " in each run" if len(judged) > 1 else "", " and a captured frame's" if frames else "",
             "%d run%s cut short, not judged; " % (cut, "" if cut == 1 else "s") if cut else "",
             nframes, len(frames), cycles, cube_desc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
