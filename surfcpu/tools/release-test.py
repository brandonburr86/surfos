#!/usr/bin/env python3
"""SurfCPU release tooling: tests (make test-release, on make check)
--------------------
File: release-test.py Date: 10/16/26 (SurfCPU)
--------------------
Without Docker, a board or a bitstream build (docs/RELEASE.md section 9):
  * release.py, bitfile.py and this file parse as Python 3.9 (the Mac's /usr/bin/python3);
  * tools/bitfile.py: its self-test (header stamp, packets, flash image, configuration time), and
    the one header layout it and tools/program.sh accept (TestBitfileStrict);
  * tools/release.py's TOML subset, its comparison and the likely causes it names for a .bit whose
    header time or frames differ, a SURFOS.BIN whose payload differs (with the ELF's symbol), a
    missing and an extra artifact;
  * tools/release.py make in a scratch git repository, with stand-ins for devenv/run.sh,
    tools/fpga-build.sh and docker (taken only with SURFCPU_RELEASE_TEST=1): a dirty tree, a
    foreign SOURCE_DATE_EPOCH and missing designs without PRERELEASE are refused; a complete
    release; a DDR design that fails makes a pre-release only with --prerelease; any other design
    that fails stops the release; the release's own files check against SHA256SUMS and MANIFEST;
  * the review of 2026-10-16 (rel-2), one class per finding: what is shipped is what was checked
    while other builds, a new commit or a changed staged file come in between (M1); verify's
    DESIGNS/BOARDS and its PARTIAL verdict (M2); check and compare on damaged releases (m1); the
    builds' environment and their records (m2); the clean tree whatever git's configuration says
    (m3); no directory deleted that the tools did not make (m4); host targets with paths that hold
    spaces (m5); the .bit header rule (c1); the locale MANIFEST records (c2); the version and the
    flash designs (c3);
  * tools/program.sh's flash path with the board tests' fake openFPGALoader
    (sim/board/tools/fake), named by OPENFPGALOADER and first on a PATH without Homebrew, so that a
    real openFPGALoader is never run even with a board plugged in, on a release's bitstream:
    written with -f --verify only when confirmed, never the flash image or a bitstream without its
    build record or with other bytes.
"""
from __future__ import annotations

import ast
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import textwrap
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "release-test"
PY = sys.executable


def load(name: str):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "tools" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bitfile = load("bitfile")
release = load("release")


def fresh(name: str) -> Path:
    d = WORK / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    return d


def sums(d: Path) -> None:
    files = sorted(p for p in d.rglob("*") if p.is_file() and p.name not in ("SHA256SUMS", "MANIFEST")
                   and p.name != "manifest.txt")
    (d / "SHA256SUMS").write_text("".join(f"{release.sha256(p)}  {p.relative_to(d).as_posix()}\n" for p in files))


def elf32(symbols: list[tuple[str, int, int]]) -> bytes:
    """a minimal ELF32 little-endian file with a symbol table: (name, address, size) functions"""
    strtab = b"\0" + b"".join(n.encode() + b"\0" for n, _, _ in symbols)
    symtab = b"\0" * 16
    off = 1
    for n, addr, size in symbols:
        symtab += struct.pack("<IIIBBH", off, addr, size, 0x12, 0, 1)
        off += len(n) + 1
    shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    text = b"\x13" * 64
    body = text + symtab + strtab + shstr
    shoff = 52 + len(body)
    o_text, o_sym = 52, 52 + len(text)
    o_str, o_shs = o_sym + len(symtab), o_sym + len(symtab) + len(strtab)
    sh = b"\0" * 40
    sh += struct.pack("<10I", 1, 1, 6, 0x80000000, o_text, len(text), 0, 0, 4, 0)
    sh += struct.pack("<10I", 7, 2, 0, 0, o_sym, len(symtab), 3, 1, 4, 16)
    sh += struct.pack("<10I", 15, 3, 0, 0, o_str, len(strtab), 0, 0, 1, 0)
    sh += struct.pack("<10I", 23, 3, 0, 0, o_shs, len(shstr), 0, 0, 1, 0)
    hdr = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 2, 243, 1, 0x80000000, 0, shoff, 0,
                                                            52, 0, 0, 40, 5, 4)
    return hdr + body + sh


class TestPython39(unittest.TestCase):
    """release.py and bitfile.py run on the Mac, maybe with its /usr/bin/python3 (3.9.6)"""

    def test_parse_as_python_3_9(self):
        for name in ("release.py", "bitfile.py", "release-test.py"):
            with self.subTest(name):
                tree = ast.parse((ROOT / "tools" / name).read_text(), filename=name, feature_version=(3, 9))
                future = {a.name for n in tree.body if isinstance(n, ast.ImportFrom) and n.module == "__future__"
                          for a in n.names}
                self.assertIn("annotations", future, f"{name}: `X | None` annotations need the __future__ import")


class TestBitfile(unittest.TestCase):
    def test_self_test(self):
        self.assertEqual(bitfile.self_test(), 0)

    def test_flash_cli(self):
        d = fresh("bitfile")
        (d / "x.bit").write_bytes(bitfile.synthetic())
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "flash", d / "x.bit", "-o", d / "x.bin", "--board",
                            "arty_a7_100t"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("0x000000-", r.stdout)
        _, _, start = bitfile.parse_header((d / "x.bit").read_bytes())
        self.assertEqual((d / "x.bin").read_bytes(), (d / "x.bit").read_bytes()[start:])
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "flash", d / "x.bit", "-o", d / "y.bin", "--board",
                            "arty_a7_35t"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertFalse((d / "y.bin").exists())
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "info", d / "x.bin"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("SPI x1", r.stdout)


class TestToml(unittest.TestCase):
    def test_fpga_manifest(self):
        text = textwrap.dedent('''\
            # SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by tools/fpga-build.sh
            design = "surfcpu_arty_a7_top"
            source_dirty = false
            build_time = 1792158292
            defines = []
            result = "pass"

            [openxc7]
            nextpnr = "\\"nextpnr-himbaechel\\" -- Next Generation Place and Route (Version 1.0.0)"
            timing = ["clock a: PASS", "clock b: PASS", ]
            wns_ns = 25.552
            constraints_not_checked = [
                "x.xdc:5: set_property CFGBVS VCCO [current_design]  [configuration]",
                "y.xdc: 3 set_max_delay",
            ]
            result = "pass"
            ''')
        m = release.parse_toml(text)
        self.assertEqual(m["design"], "surfcpu_arty_a7_top")
        self.assertIs(m["source_dirty"], False)
        self.assertEqual(m["build_time"], 1792158292)
        self.assertEqual(m["defines"], [])
        self.assertEqual(m["openxc7"]["nextpnr"][:20], '"nextpnr-himbaechel"')
        self.assertEqual(m["openxc7"]["timing"], ["clock a: PASS", "clock b: PASS"])
        self.assertEqual(m["openxc7"]["wns_ns"], 25.552)
        self.assertEqual(len(m["openxc7"]["constraints_not_checked"]), 2)

    def test_round_trip(self):
        d = {"s": 'a "quoted" \\ value', "n": 7, "b": True, "l": ["x", "y [1]"], "e": []}
        text = "[t]\n" + release.toml_table(d) + "[[row]]\nk = 1\n[[row]]\nk = 2\n"
        m = release.parse_toml(text)
        self.assertEqual(m["t"], d)
        self.assertEqual([r["k"] for r in m["row"]], [1, 2])


class TestCompare(unittest.TestCase):
    def setUp(self):
        self.ref, self.new = fresh("cmp-ref"), fresh("cmp-new")
        for d in (self.ref, self.new):
            (d / "fpga/arty_a7_100t/top").mkdir(parents=True)
            (d / "debug").mkdir()
            (d / "fpga/arty_a7_100t/top/top.bit").write_bytes(bitfile.synthetic(frames=4))
            (d / "fpga/arty_a7_100t/top/manifest.txt").write_text('build_time = 1\nrom_sha256 = "aa"\n')
            (d / "debug/surfos.elf").write_bytes(elf32([("kmain", 0x80000000, 0x40), ("time_now", 0x80000040, 0x40)]))
            payload = bytes(range(256)) * 2
            (d / "SURFOS.BIN").write_bytes(release.load("surfimg").make_package(payload, 0x80000000, build_id=0x1234abcd))
            (d / "same.txt").write_text("same\n")

    def test_all_match(self):
        sums(self.ref)
        sums(self.new)
        rows, notes = release.compare(self.ref, self.new)
        self.assertTrue(all(s == "match" for _, s, _ in rows), rows)
        lines = release.report_lines(rows, notes, "t")
        self.assertTrue(lines[-1].startswith("PASS"))

    def test_causes(self):
        sums(self.ref)
        bit = self.new / "fpga/arty_a7_100t/top/top.bit"
        bit.write_bytes(bitfile.stamp(bit.read_bytes(), 86400))                 # another build time
        payload = bytearray(bytes(range(256)) * 2)
        payload[0x44] ^= 1                                                      # in time_now
        pkg = self.new / "SURFOS.BIN"
        pkg.write_bytes(release.load("surfimg").make_package(bytes(payload), 0x80000000, build_id=0x1234abcd))
        (self.new / "same.txt").unlink()
        (self.new / "extra.txt").write_text("x")
        sums(self.new)
        rows = {r[0]: r for r in release.compare(self.ref, self.new)[0]}
        self.assertEqual(rows["fpga/arty_a7_100t/top/top.bit"][1], "DIFFERS")
        self.assertIn("date and time", rows["fpga/arty_a7_100t/top/top.bit"][2])
        self.assertIn("only the header differs", rows["fpga/arty_a7_100t/top/top.bit"][2])
        self.assertIn("time_now+0x4", rows["SURFOS.BIN"][2])
        self.assertIn("payload_crc32", rows["SURFOS.BIN"][2])
        self.assertEqual(rows["same.txt"][1], "MISSING")
        self.assertEqual(rows["extra.txt"][1], "EXTRA")
        self.assertTrue(release.report_lines(list(rows.values()), [], "t")[-1].startswith("FAIL"))

    def test_frames_and_records(self):
        bit = self.new / "fpga/arty_a7_100t/top/top.bit"
        data = bytearray(bit.read_bytes())
        data[-600] ^= 0x40                                                      # in the frame data
        bit.write_bytes(bytes(data))
        sums(self.ref)
        sums(self.new)
        rel = "fpga/arty_a7_100t/top/top.bit"
        row = {r[0]: r for r in release.compare(self.ref, self.new)[0]}[rel]
        self.assertEqual(row[1], "DIFFERS")
        self.assertIn("frame data", row[2])
        self.assertIn("recorded build inputs are the same", row[2])
        (self.new / "fpga/arty_a7_100t/top/manifest.txt").write_text('build_time = 2\nrom_sha256 = "aa"\n')
        row = {r[0]: r for r in release.compare(self.ref, self.new)[0]}[rel]
        self.assertIn("build_time 1 against 2", row[2])

    def test_not_rebuilt(self):
        sums(self.ref)
        (self.new / "fpga").rename(self.new / "elsewhere")
        sums(self.new)
        rows = {r[0]: r for r in release.compare(self.ref, self.new, rebuilt=set())[0]}
        self.assertEqual(rows["fpga/arty_a7_100t/top/top.bit"][1], "not rebuilt")
        rows = {r[0]: r for r in release.compare(self.ref, self.new)[0]}
        self.assertEqual(rows["fpga/arty_a7_100t/top/top.bit"][1], "MISSING")
        for reason, status in (("not built: SKIP_DDR=1", "not rebuilt"), ("did not build: timing", "MISSING")):
            (self.new / "MANIFEST").write_text(f'commit = "x"\n\n[[design]]\nname = "top"\nboard = "arty_a7_100t"\n'
                                               f'status = "missing"\nreason = "{reason}"\n')
            rows = {r[0]: r for r in release.compare(self.ref, self.new)[0]}
            self.assertEqual(rows["fpga/arty_a7_100t/top/top.bit"][1], status, reason)


# ------------------------------------------------------------------------------- make, with stand-ins
FAKE_RUN = r"""#!/usr/bin/env python3
# stand-in for devenv/run.sh: the software build (make build-rom [BUILD=dir], make build-kernel, or kernel.mk with
# OUT=dir, all relative as make takes them), or litedram-gen ($FAKE_LITEDRAM_FAIL: lock, or anything for a fetch
# failure). The packages carry the commit's build id and the version $FAKE_VERSION (default 2.3.4).
import os, subprocess, sys
from pathlib import Path
args = sys.argv[1:]
repo = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip())
sc = repo / "surfcpu"
bid = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()[:8]
epoch = os.environ["SOURCE_DATE_EPOCH"]
if "litedram-gen" in args:
    fail = os.environ.get("FAKE_LITEDRAM_FAIL", "")
    if fail == "lock":
        print("lock.sh: DIFFERS   build/litedram/arty/gateware/litedram_core.v (sha256 bc92, arty.sha256 has 150b)")
        sys.exit(2)
    if fail:
        print("setup.sh: cannot fetch")
        sys.exit(1)
    sys.exit(0)
val = dict(a.split("=", 1) for a in args if "=" in a)
version = os.environ.get("FAKE_VERSION", "2.3.4")


def pack(payload, out, name):
    subprocess.run([sys.executable, str(sc / "tools/surfimg.py"), "pack", "--bin", str(payload), "--load-addr",
                    "0x80000000", "--build-id", bid, "--version", version, "--name", name, "-o", str(out)],
                   check=True, capture_output=True)


if "build-rom" in args:
    fw = sc / val.get("BUILD", "build") / "fw"
    (fw / "gen").mkdir(parents=True, exist_ok=True)
    (fw / "gen/fw_build.h").write_text(f"#define FW_BUILD_ID 0x{bid}U\n#define FW_BUILD_DIRTY 0\n")
    for f in ("surfboot", "bramdiag"):
        (fw / f"{f}.mem").write_text(f"{int(epoch):08x}\n")
        (fw / f"{f}.elf").write_text(f"ELF {f}\n")
    (fw / "ddrdiag.elf").write_text("ELF ddrdiag\n")
    (fw / "payload").write_text(f"ddrdiag {epoch}")
    pack(fw / "payload", fw / "DDRDIAG.BIN", "DDRdiag")
if "build-kernel" in args or "arch/riscv32/kernel.mk" in args:
    k = repo / val.get("OUT", "build/riscv32/arty_a7")
    k.mkdir(parents=True, exist_ok=True)
    (k / "surfos.elf").write_text("ELF surfos\n")
    (k / "payload").write_text(f"payload {epoch}")
    pack(k / "payload", k / "SURFOS.BIN", "SurfOS")
"""

FAKE_FPGA = r"""#!/usr/bin/env python3
# stand-in for tools/fpga-build.sh --design D BOARD, run in surfcpu/: BOARD/DESIGN/{DESIGN.bit,manifest.txt} under
# $SURFCPU_FPGA_OUT (default build/fpga), the boot ROM from $ROM or the design list's rom line (with the generic
# BOOTROM_INIT), kernel_sha256 of $SURFCPU_KERNEL (default ../build/riscv32/arty_a7/SURFOS.BIN).
# $FAKE_FPGA_FAIL=d1,d2 fails those designs; $FAKE_FPGA_HOOK (a shell command) runs first, $FAKE_FPGA_HOOK_ON=
# design/board limits it to one build; $FAKE_FPGA_NOOP=1 builds nothing and exits 0 (as DRY_RUN=1 does);
# $FAKE_OX_IMAGE records another image (as SURFCPU_OPENXC7_IMAGE would);
# $FAKE_FPGA_ENV appends the environment's names to that file; $FAKE_RUN_ID_TIME puts another time in run_id;
# the placer seed recorded ([openxc7] placer_seed, as fpga-build.sh writes it) is $PLACER_SEEDS's first, else
# $FAKE_FPGA_SEEDS's for design/board (board/design=seed,...), else 1; $FAKE_FPGA_SEEDLOG gets a line
# "board/design PLACER_SEEDS" per build; $FAKE_FPGA_THIN=d1,d2 records those designs' slack margin THIN;
# the record's manifest_format is 2, as fpga-build.sh writes it, or $FAKE_FPGA_FORMAT.
import hashlib, os, re, subprocess, sys, time
from pathlib import Path
design, board = sys.argv[2], sys.argv[3]
root = Path.cwd()
if os.environ.get("FAKE_FPGA_ENV"):
    with open(os.environ["FAKE_FPGA_ENV"], "a") as f:
        f.write(" ".join(sorted(os.environ)) + "\n")
hook, on = os.environ.get("FAKE_FPGA_HOOK"), os.environ.get("FAKE_FPGA_HOOK_ON")
if hook and (not on or on == f"{design}/{board}"):
    subprocess.run(["sh", "-c", hook], check=True, cwd=str(root))
if os.environ.get("FAKE_FPGA_NOOP") == "1":
    sys.exit(0)
sys.path.insert(0, str(root / "tools"))
import bitfile
base = Path(os.environ.get("SURFCPU_FPGA_OUT") or root / "build" / "fpga") / board / design
base.mkdir(parents=True, exist_ok=True)
for p in (base / f"{design}.bit", base / "manifest.txt"):
    if p.exists():
        p.unlink()
commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
part, idcode = ("xc7a100tcsg324-1", 0x03631093) if board == "arty_a7_100t" else ("xc7a35tcsg324-1", 0x0362D093)
fail = design in os.environ.get("FAKE_FPGA_FAIL", "").split(",")
rom_image = rom_sha = ""
lines = [x.split() for x in (root / f"boards/arty_a7/designs/{design}.files").read_text().splitlines() if x.strip()]
if any(f[0] == "generic" and "BOOTROM_INIT" in f for f in lines):
    for f in lines:
        if f[0] == "rom":
            src = Path(os.environ["ROM"]) if os.environ.get("ROM") else root / f[1]
            inside = str(src).startswith(str(root) + "/")             # as fpga-build.sh writes it
            rom_image = str(src)[len(str(root)) + 1:] if inside else str(src)
            rom_sha = hashlib.sha256(src.read_bytes()).hexdigest()
kernel = Path(os.environ.get("SURFCPU_KERNEL") or root.parent / "build/riscv32/arty_a7/SURFOS.BIN")
kernel_sha = hashlib.sha256(kernel.read_bytes()).hexdigest() if kernel.is_file() else ""
tag = re.search(r"^IMAGE_TAG=(\S+)$", (root / "tools/openxc7-build.sh").read_text(), re.M).group(1)
when = os.environ.get("FAKE_RUN_ID_TIME") or time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
head = [f'manifest_format = {os.environ.get("FAKE_FPGA_FORMAT", "2")}', f'run_id = "{design}-{board}-{commit[:8]}-{when}"', f'design = "{design}"', f'board = "{board}"',
        f'part = "{part}"', f'source_revision = "{commit}"', "source_dirty = false",
        f'build_time = {os.environ["SOURCE_DATE_EPOCH"]}', "generated_files_match = true",
        f'rom_image = "{rom_image}"', f'rom_sha256 = "{rom_sha}"', f'kernel_sha256 = "{kernel_sha}"']
if os.environ.get("FAKE_FPGA_SEEDLOG"):
    with open(os.environ["FAKE_FPGA_SEEDLOG"], "a") as f:
        f.write(f"{board}/{design} {os.environ.get('PLACER_SEEDS', '-')}\n")
seeds = dict(x.split("=", 1) for x in os.environ.get("FAKE_FPGA_SEEDS", "").split(",") if "=" in x)
seed = int((os.environ.get("PLACER_SEEDS") or seeds.get(f"{board}/{design}") or "1").split()[0])
thin = design in os.environ.get("FAKE_FPGA_THIN", "").split(",")
if fail:
    head += ['result = "fail"', 'reason = "timing not met (nextpnr: dram_clk 63.5 MHz of 100 MHz)"']
else:
    data = bitfile.stamp(bitfile.synthetic(idcode=idcode, part=part), int(os.environ["SOURCE_DATE_EPOCH"]))
    (base / f"{design}.bit").write_bytes(data)
    head += ['result = "pass"', f'bitstream_sha256 = "{hashlib.sha256(data).hexdigest()}"']
head += ["", "[openxc7]", f'image = "{os.environ.get("FAKE_OX_IMAGE") or "surfcpu-openxc7:" + tag}"', 'check_synthesis = "PASS"', 'check_lvs = "PASS"',
         'timing = ["clock sys_clk: PASS", ]', "wns_ns = 10.5", f"placer_seed = {seed}",
         f'slack_margin = "{"THIN" if thin else "PASS"}"',
         f'slack_margin_note = "{"dram_clk +0.087 ns, wants 0.500; every seed tried was thin or failed, this one has the most margin" if thin else "least margin sys_clk +30.000 ns, wants 1.600"}"']
(base / "manifest.txt").write_text("# SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by "
                                   "tools/fpga-build.sh\n" + "\n".join(head) + "\n")
sys.exit(1 if fail else 0)
"""

FAKE_DOCKER = r"""#!/bin/sh
# stand-in for docker: info, image inspect, ps, run ... cat; with FAKE_DOCKER_ARGS, `run` prints its arguments
case "$1" in
  info)  exit 0 ;;
  image) case "$*" in
           *Config.Env*) echo '["PATH=/usr/local/bin:/usr/bin:/bin","SURFCPU_IN_CONTAINER=1"]' ;;
           *)            echo "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" ;;
         esac ;;
  ps)    ;;
  run)   if [ -n "${FAKE_DOCKER_ARGS:-}" ]; then printf '%s\n' "$@"; else echo "fake-tool 1.0"; fi ;;
  *)     exit 1 ;;
esac
"""


class TestBoardRestriction(unittest.TestCase):
    """a design list's board line (the GPU designs, 10/18/26): release.txt may not build it for another board"""
    def test_restriction(self):
        self.assertIsNone(release.design_boards("surfcpu_arty_a7_ddr"))
        self.assertEqual(release.design_boards("surfcpu_arty_a7_gpudiag"), ["arty_a7_100t"])
        self.assertEqual(release.design_boards("surfcpu_arty_a7_ddr_gpu"), ["arty_a7_100t"])
        d = fresh("TestBoardRestriction")
        both = d / "both.txt"
        both.write_text("version 1.0.0\nboard arty_a7_100t\nboard arty_a7_35t\ndesign surfcpu_arty_a7_gpudiag - -\n")
        with self.assertRaises(release.Refused) as e:
            release.read_list(both)
        self.assertIn("builds for arty_a7_100t only", str(e.exception))
        one = d / "one.txt"
        one.write_text("version 1.0.0\nboard arty_a7_100t\ndesign surfcpu_arty_a7_gpudiag - -\n")
        self.assertEqual(release.read_list(one)["boards"], ["arty_a7_100t"])
        two = d / "designs"                         # several board lines add up, as in fpga-build.sh
        (two / "boards/arty_a7/designs").mkdir(parents=True)
        (two / "boards/arty_a7/designs/x.files").write_text("board arty_a7_100t  # one\nboard arty_a7_35t\ntop x\n")
        (two / "boards/arty_a7/designs/crlf.files").write_bytes(b"board arty_a7_100t\r\ntop x\r\n")
        (two / "boards/arty_a7/designs/bare.files").write_text("board   # no board named\ntop x\n")
        root, release.ROOT = release.ROOT, two
        try:
            self.assertEqual(release.design_boards("x"), ["arty_a7_100t", "arty_a7_35t"])
            self.assertEqual(release.design_boards("crlf"), ["arty_a7_100t"])       # as fpga-build.sh reads it
            with self.assertRaises(release.Refused) as e:                           # as fpga-build.sh refuses it
                release.design_boards("bare")
            self.assertIn("names no board", str(e.exception))
        finally:
            release.ROOT = root
        real = release.read_list()                  # the real list: no GPU design, both boards
        self.assertEqual(real["boards"], ["arty_a7_100t", "arty_a7_35t"])
        self.assertNotIn("surfcpu_arty_a7_gpudiag", [x["name"] for x in real["designs"]])


class ScratchRelease(unittest.TestCase):
    """a scratch git repository holding the release tooling, with stand-ins for devenv/run.sh (the software),
    tools/fpga-build.sh (one bitstream) and docker, which tools/release.py takes only with SURFCPU_RELEASE_TEST=1"""
    COPY = ("tools/release.py", "tools/bitfile.py", "tools/deploy-sd.py", "tools/surfimg.py", "tools/socspec.py",
            "spec/soc.toml", "boards/arty_a7/boards.txt", "devenv/run.sh", "tools/openxc7-build.sh",
            "firmware/tests/deploy/fake_diskutil.py", "firmware/tests/deploy/fake_mount.py", "tools/fresh-checkout.sh",
            "Makefile", "mk/host.mk") + tuple(
        f"docs/{d}" for d in ("SURFBUS.md", "ISA.md", "BOOT-ABI.md", "MODULES.md", "MEMORY-MAP.md"))
    LIST = ("version 2.3.4\nboard arty_a7_100t\nboard arty_a7_35t\ndesign base - -\ndesign top test -\n"
            "design dd boot ddr\ncard-cmdline sd=rw\n")
    DESIGNS = {"base": "top base\ngeneric BOARD BUILD_ID\n",
               "top": "top top\ngeneric BOARD BUILD_ID BOOTROM_INIT\nrom build/fw/surfboot.mem u_soc.u_bootrom\n",
               "dd": "top dd\ngeneric BOARD BUILD_ID BOOTROM_INIT\nrom build/fw/surfboot.mem u_soc.u_bootrom\n"}

    PLACE = ""                                                  # a subdirectory to put the repository in

    def setUp(self):
        self.dir = fresh(f"{type(self).__name__}-{self._testMethodName}")
        if self.PLACE:
            self.dir = self.dir / self.PLACE
            self.dir.mkdir(parents=True)
        self.repo = self.dir / "repo"
        self.sc = self.repo / "surfcpu"
        for rel in self.COPY:
            (self.sc / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, self.sc / rel)
        (self.sc / "devenv/run.sh").chmod(0o755)
        (self.sc / "tools/fresh-checkout.sh").chmod(0o755)
        (self.sc / "boards/arty_a7/designs").mkdir(parents=True, exist_ok=True)
        for d, text in self.DESIGNS.items():
            (self.sc / f"boards/arty_a7/designs/{d}.files").write_text(text)
        (self.sc / "release").mkdir()
        (self.sc / "release/release.txt").write_text(self.LIST)
        (self.repo / ".gitignore").write_text("build/\n__pycache__/\n")
        self.fakes = self.dir / "fakes"
        self.fakes.mkdir()
        for name, text in (("run.sh", FAKE_RUN), ("fpga-build", FAKE_FPGA), ("docker", FAKE_DOCKER)):
            (self.fakes / name).write_text(text)
            (self.fakes / name).chmod(0o755)
        self.git("init", "-q")
        self.git("add", "-A")
        self.commit_all("scratch")
        # (in the container, devenv/run.sh sets SURFCPU_BUILD_ID and SURFCPU_BUILD_DIRTY: the release refuses them)
        self.env = {k: v for k, v in os.environ.items()
                    if k not in release.REFUSE_ENV + release.HOOKS + ("SOURCE_DATE_EPOCH", "SURFCPU_IN_CONTAINER",
                                                                      "BIT", "MAKEFLAGS", "MFLAGS", "MAKELEVEL")}
        self.env.update(SURFCPU_RELEASE_TEST="1", SURFCPU_RELEASE_RUN=str(self.fakes / "run.sh"),
                        SURFCPU_RELEASE_FPGA=str(self.fakes / "fpga-build"),
                        SURFCPU_RELEASE_DOCKER=str(self.fakes / "docker"), DOCKER=str(self.fakes / "docker"),
                        HOME=str(self.dir), PYTHON_HOST=PY)

    def commit_all(self, msg):
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "--allow-empty", "-m", msg)
        self.commit = self.git("rev-parse", "HEAD").strip()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo)] + list(args), capture_output=True, text=True,
                              check=True).stdout

    def tool(self, *args, **env):
        e = dict(self.env, **env)
        for k in [k for k, v in e.items() if v is None]:
            del e[k]
        return subprocess.run([PY, self.sc / "tools/release.py"] + [str(a) for a in args], cwd=self.sc, env=e,
                              capture_output=True, text=True, stdin=subprocess.DEVNULL)

    def make(self, *args, **env):
        return self.tool("make", *args, **env)

    def rel_dir(self, pre: bool) -> Path:
        return self.sc / "build" / "release" / f"2.3.4{'-pre' if pre else ''}-{self.commit[:8]}"

    def records(self, rel: Path) -> dict:
        return {(p.parts[-3], p.parts[-2]): release.parse_toml(p.read_text())
                for p in (rel / "fpga").glob("*/*/manifest.txt")}


class TestMake(ScratchRelease):
    def test_refusals(self):
        (self.sc / "new.txt").write_text("x")
        r = self.make("--prerelease")
        self.assertEqual(r.returncode, 1)
        self.assertIn("not clean", r.stderr)
        (self.sc / "new.txt").unlink()
        r = self.make("--prerelease", SOURCE_DATE_EPOCH="12345")
        self.assertEqual(r.returncode, 1)
        self.assertIn("is not the commit's time", r.stderr)
        r = self.make("--designs", "base")
        self.assertEqual(r.returncode, 1)
        self.assertIn("PRERELEASE=1", r.stderr)
        r = self.make("--skip-ddr")
        self.assertEqual(r.returncode, 1)
        self.assertIn("SKIP_DDR=1", r.stderr)
        self.assertFalse((self.sc / "build/release").exists() and any(
            (self.sc / "build/release").glob("*/MANIFEST")))

    def test_complete(self):
        r = self.make()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(False)
        self.assertTrue((rel / "MANIFEST").is_file(), r.stdout)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(man["status"], "complete")
        self.assertIs(man["complete"], True)
        self.assertEqual(man["commit"], self.commit)
        self.assertEqual(man["missing"], [])
        self.assertEqual(man["flash_boot_design"], "dd")
        listed = release.read_sums(rel / "SHA256SUMS")
        for p in ("SURFOS.BIN", "DDRDIAG.BIN", "card/BOOT/SURFOS.BIN", "card/BOOT/CMDLINE.TXT",
                  "fpga/arty_a7_100t/dd/dd.bit", "fpga/arty_a7_35t/base/base.bit", "flash/arty_a7_100t/dd.bin",
                  "flash/arty_a7_35t/top.bin", "rom/surfboot.mem", "debug/surfos.elf"):
            self.assertIn(p, listed)
        self.assertNotIn("fpga/arty_a7_100t/dd/manifest.txt", listed)
        self.assertTrue((rel / "fpga/arty_a7_100t/dd/manifest.txt").is_file())
        self.assertEqual((rel / "card/BOOT/CMDLINE.TXT").read_text(), "sd=rw\n")
        self.assertEqual((rel / "card/BOOT/SURFOS.BIN").read_bytes(), (rel / "SURFOS.BIN").read_bytes())
        r = self.tool("check", rel)
        self.assertEqual(r.returncode, 0, r.stdout)
        # a second release of the same commit is the same bytes, and replaces nothing without --force
        r = self.make()
        self.assertEqual(r.returncode, 1)
        self.assertIn("exists", r.stderr)
        r = self.make("--out", self.dir / "again")
        self.assertEqual(r.returncode, 0, r.stderr)
        again = self.dir / "again" / rel.name
        rows, _ = release.compare(rel, again)
        self.assertTrue(rows and all(s == "match" for _, s, _ in rows), rows)
        self.assertEqual(self.tool("check", again).returncode, 0)
        # --force replaces a release this tool made
        r = self.make("--force")
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_ddr_failure(self):
        r = self.make(FAKE_FPGA_FAIL="dd")
        self.assertEqual(r.returncode, 1)
        self.assertIn("PRERELEASE=1", r.stderr)
        self.assertIn("dram_clk 63.5 MHz", r.stderr)
        self.assertFalse(list((self.sc / "build/release").glob("*/MANIFEST")))
        r = self.make("--prerelease", FAKE_FPGA_FAIL="dd")
        self.assertEqual(r.returncode, 0, r.stderr)
        rel = self.rel_dir(True)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(man["status"], "prerelease")
        self.assertIs(man["complete"], False)
        self.assertEqual(len(man["missing"]), 2)
        self.assertIn("dram_clk", man["missing"][0])
        self.assertEqual(man["flash_boot_design"], "", "a pre-release without the boot design names none")
        self.assertEqual(man["flash_test_design"], "top")
        self.assertFalse((rel / "fpga/arty_a7_100t/dd").exists())
        self.assertTrue((rel / "flash/arty_a7_100t/top.bin").is_file())
        r = self.make("--prerelease", "--force", FAKE_LITEDRAM_FAIL="1")
        self.assertEqual(r.returncode, 0, r.stderr)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertIn("litedram-gen failed (exit 1): setup.sh: cannot fetch", man["missing"][0])
        r = self.make("--prerelease", "--force", FAKE_LITEDRAM_FAIL="lock")
        self.assertEqual(r.returncode, 0, r.stderr)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertIn("litedram_core.v differs from ip/litedram/arty.sha256", man["missing"][0])

    def test_other_failure(self):
        r = self.make("--prerelease", FAKE_FPGA_FAIL="top")
        self.assertEqual(r.returncode, 1)
        self.assertIn("the release needs it", r.stderr)
        self.assertFalse(list((self.sc / "build/release").glob("*/MANIFEST")))

    def test_locale_recorded_truthfully(self):
        """review c2: the MANIFEST does not claim LC_ALL/TZ reach the containers"""
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 0, r.stderr)
        man = release.parse_toml((self.rel_dir(True) / "MANIFEST").read_text())
        self.assertIn("the containers get only SOURCE_DATE_EPOCH", man["tools"]["locale"])
        self.assertIn("no LANG, LC_* or TZ set (the POSIX locale, UTC)", man["tools"]["locale"])

    def test_version_tied_to_the_kernel(self):
        """review c3: release.txt's version and SURFOS.BIN's must agree"""
        r = self.make("--prerelease", "--skip-ddr", FAKE_VERSION="9.9.9")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("9.9.9", r.stderr)
        self.assertIn("2.3.4", r.stderr)


# ------------------------------------------------------------------------------- the review's findings (rel-2)
class TestShippedIsChecked(ScratchRelease):
    """review M1: what the release ships is what it checked, whatever else happens in the tree meanwhile"""
    OTHER_SW = ("mkdir -p build/fw ../build/riscv32/arty_a7; printf 'deadbeef\\n' > build/fw/surfboot.mem; "
                "printf 'other payload' > ../build/riscv32/arty_a7/payload; "
                "python3 tools/surfimg.py pack --bin ../build/riscv32/arty_a7/payload --load-addr 0x80000000 "
                "--build-id 0badc0de --version 2.3.4 -o ../build/riscv32/arty_a7/SURFOS.BIN >/dev/null")

    def test_other_software_build_meanwhile(self):
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_HOOK=self.OTHER_SW, FAKE_FPGA_HOOK_ON="base/arty_a7_35t")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(True)
        pkg = (rel / "SURFOS.BIN").read_bytes()
        self.assertEqual(f"{int.from_bytes(pkg[44:48], 'little'):08x}", self.commit[:8])
        rom = release.sha256(rel / "rom/surfboot.mem")
        for (b, d), m in self.records(rel).items():
            if d == "top":
                self.assertEqual(m["rom_sha256"], rom, f"{d} for {b} was built with another boot ROM")
        self.assertEqual(self.tool("check", rel).returncode, 0)

    def test_other_bitstream_build_meanwhile(self):
        tamper = ("mkdir -p build/fpga/arty_a7_100t/base && printf 'not this' > build/fpga/arty_a7_100t/base/base.bit "
                  "&& printf 'result = \"pass\"\\nsource_dirty = true\\n' > build/fpga/arty_a7_100t/base/manifest.txt")
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_HOOK=tamper, FAKE_FPGA_HOOK_ON="top/arty_a7_35t")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(True)
        m = self.records(rel)[("arty_a7_100t", "base")]
        self.assertIs(m["source_dirty"], False)
        self.assertEqual(release.sha256(rel / "fpga/arty_a7_100t/base/base.bit"), m["bitstream_sha256"])
        f, _, _ = bitfile.parse_header((rel / "fpga/arty_a7_100t/base/base.bit").read_bytes())
        self.assertEqual(f"{f['c']} {f['d']}", time.strftime("%Y/%m/%d %H:%M:%S", time.gmtime(
            int(self.git("log", "-1", "--format=%ct").strip()))))

    def test_new_commit_meanwhile(self):
        move = "git -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty -m next"
        r = self.make("--prerelease", FAKE_FPGA_HOOK=move, FAKE_FPGA_HOOK_ON="dd/arty_a7_100t")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("HEAD moved", r.stderr)
        self.assertFalse(list((self.sc / "build/release").glob("*/MANIFEST")))

    def test_staged_file_changed_meanwhile(self):
        poke = "for f in build/release/.*stage/SURFOS.BIN; do [ -f \"$f\" ] && printf x >> \"$f\"; done; true"
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_HOOK=poke, FAKE_FPGA_HOOK_ON="top/arty_a7_35t")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("SURFOS.BIN is not the file that was checked", r.stderr)

    def test_one_release_at_a_time(self):
        lock = self.sc / "build/release-work.lock"
        lock.parent.mkdir(parents=True, exist_ok=True)
        lock.write_text(f"{os.getpid()} now\n")
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("another release is running", r.stderr)
        gone = subprocess.Popen([sys.executable, "-c", "pass"])
        gone.wait()
        lock.write_text(f"{gone.pid} then\n")
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("stale lock", r.stdout)
        self.assertFalse(lock.exists())


class TestVerifyScope(ScratchRelease):
    """review M2: release-verify refuses names it does not know, and a partial check never reads as PASS"""

    def test_scope(self):
        self.assertEqual(self.make("--prerelease", "--skip-ddr").returncode, 0)
        rel = self.rel_dir(True)
        for args, words in ((["--designs", "tpo"], "unknown design"), (["--boards", "arty_a7_10t"], "unknown board"),
                            (["--designs", "dd"], "no bitstream of dd")):
            r = self.tool("verify", rel, *args)
            self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
            self.assertIn(words, r.stderr)
        r = self.tool("verify", rel, "--designs", "top", "--boards", "arty_a7_100t")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        verdict = r.stdout.strip().splitlines()[-1]
        self.assertTrue(verdict.startswith("PARTIAL"), verdict)
        self.assertIn("NOT verified", verdict)
        self.assertIn("not rebuilt  fpga/arty_a7_35t/top/top.bit", r.stdout)
        r = self.tool("verify", rel)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(r.stdout.strip().splitlines()[-1].startswith("PASS:"), r.stdout)
        self.assertNotIn("not rebuilt", r.stdout)


class TestCheckStrict(ScratchRelease):
    """review m1: release-check and compare fail a damaged or inconsistent release"""

    def setUp(self):
        super().setUp()
        self.assertEqual(self.make("--prerelease", "--skip-ddr").returncode, 0)
        self.pre = self.rel_dir(True)

    def copy(self, name: str) -> Path:
        d = self.dir / "copies" / name
        shutil.copytree(self.pre, d)
        return d

    def check(self, d: Path):
        return self.tool("check", d)

    def test_intact(self):
        self.assertEqual(self.check(self.pre).returncode, 0)

    def test_empty_lists(self):
        a = self.copy("a")
        (a / "SHA256SUMS").write_text("")
        (a / "MANIFEST").write_text("")
        self.assertEqual(self.check(a).returncode, 1)
        b = self.dir / "copies" / "b"
        b.mkdir(parents=True)
        (b / "SHA256SUMS").write_text("")
        (b / "MANIFEST").write_text("")
        self.assertEqual(self.check(b).returncode, 1)

    def test_planted_file(self):
        b = self.copy(self.pre.name)
        d = b / "fpga/arty_a7_100t/dd"
        d.mkdir(parents=True)
        shutil.copyfile(b / "fpga/arty_a7_100t/top/top.bit", d / "dd.bit")
        shutil.copyfile(b / "fpga/arty_a7_100t/top/manifest.txt", d / "manifest.txt")
        r = self.check(b)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("fpga/arty_a7_100t/dd", r.stdout)

    def test_renamed(self):
        c = self.dir / "copies" / f"2.3.4-{self.commit[:8]}"
        shutil.copytree(self.pre, c)
        r = self.check(c)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("named", r.stdout)

    def test_reference_bytes(self):
        d = self.copy(self.pre.name)
        p = d / "fpga/arty_a7_100t/top/top.bit"
        p.write_bytes(p.read_bytes()[:100])
        r = self.tool("compare", d, self.pre)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("DAMAGED", r.stdout)

    def test_dropped_design(self):
        e = self.copy(self.pre.name)
        sums = [ln for ln in (e / "SHA256SUMS").read_text().splitlines()
                if "fpga/arty_a7_100t/top/" not in ln and "flash/arty_a7_100t/top" not in ln]
        (e / "SHA256SUMS").write_text("\n".join(sums) + "\n")
        blocks = [x for x in (e / "MANIFEST").read_text().split("\n\n")
                  if not (x.startswith("[[artifact]]") and ("fpga/arty_a7_100t/top/" in x or "flash/arty_a7_100t/top" in x))]
        (e / "MANIFEST").write_text("\n\n".join(blocks))
        shutil.rmtree(e / "fpga/arty_a7_100t/top")
        (e / "flash/arty_a7_100t/top.bin").unlink()
        self.assertEqual(self.check(e).returncode, 1)
        self.assertEqual(self.tool("compare", e, self.pre).returncode, 1)

    def test_version_and_flash_designs(self):
        """review c3: check holds SURFOS.BIN's version and the flash designs MANIFEST names to the release"""
        v = self.copy(self.pre.name)
        (v / "MANIFEST").write_text((v / "MANIFEST").read_text().replace('flash_boot_design = ""',
                                                                         'flash_boot_design = "dd"'))
        r = self.check(v)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("flash_boot_design 'dd'", r.stdout)

    def test_record_format(self):
        """a build record of a format this tool does not know fails the check; 2 (what tools/fpga-build.sh writes
        since 10/27/26) and the older 1, with no manifest_format, are read (docs/BUILD-HOST.md)"""
        rec = "fpga/arty_a7_100t/top/manifest.txt"
        self.assertIn("\nmanifest_format = 2\n", (self.pre / rec).read_text())
        for fmt, ok in (("3", False), ("1", True), (None, True)):
            c = self.dir / f"copies-format-{fmt}" / self.pre.name
            shutil.copytree(self.pre, c)
            (c / rec).write_text((c / rec).read_text().replace(
                "\nmanifest_format = 2\n", "\n" if fmt is None else f"\nmanifest_format = {fmt}\n"))
            r = self.check(c)
            if ok:
                self.assertEqual(r.returncode, 0, f"format {fmt}: {r.stdout}")
            else:
                self.assertEqual(r.returncode, 1, r.stdout)
                self.assertIn(f"{rec}: manifest_format 3, which this tool does not know", r.stdout)

    def test_extra_in_new(self):
        n = self.copy(self.pre.name)
        (n / "extra.txt").write_text("x")
        lines = (n / "SHA256SUMS").read_text() + f"{release.sha256(n / 'extra.txt')}  extra.txt\n"
        (n / "SHA256SUMS").write_text(lines)
        r = self.tool("compare", self.pre, n)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("EXTRA", r.stdout)


class TestEnvironment(ScratchRelease):
    """review m2: the builds get an allow-listed environment, fresh output directories and records from this run"""

    def test_stale_results_never_shipped(self):
        self.assertEqual(self.make("--prerelease", "--skip-ddr").returncode, 0)
        r = self.make("--prerelease", "--skip-ddr", "--out", self.dir / "second", FAKE_FPGA_NOOP="1")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("no build record from this build", r.stderr)

    def test_record_from_another_run(self):
        r = self.make("--prerelease", "--skip-ddr", FAKE_RUN_ID_TIME="20200101T000000Z")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("not from this build", r.stderr)

    def test_record_from_another_image(self):
        r = self.make("--prerelease", "--skip-ddr", FAKE_OX_IMAGE="surfcpu-openxc7:other")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("built in the image 'surfcpu-openxc7:other'", r.stderr)

    def test_record_of_a_format_it_does_not_know(self):
        """refused as the build finishes, not only by the release's own check at its end"""
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_FORMAT="3")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("its build record is manifest_format 3, which this tool does not know", r.stderr)
        self.assertNotIn("does not pass its own check", r.stderr)

    def test_settings_that_change_the_builds_are_refused(self):
        for var, val in (("DRY_RUN", "1"), ("SURFCPU_OPENXC7_IMAGE", "x:y"), ("SURFCPU_IMAGE", "x:y"),
                         ("SURFCPU_OPENXC7_BUILD", "/bin/true"), ("SURFCPU_ROMCHECK", "/bin/true"), ("ROM", "/x"),
                         ("PLACER_SEEDS", "3")):
            r = self.make("--prerelease", "--skip-ddr", **{var: val})
            self.assertEqual(r.returncode, 1, f"{var}: {r.stdout}")
            self.assertIn(var, r.stderr)

    def test_allow_listed_environment(self):
        dump = self.dir / "env.txt"
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_ENV=str(dump), JUNK_SETTING="1", DESIGN="other")
        self.assertEqual(r.returncode, 0, r.stderr)
        names = set(dump.read_text().split())
        for gone in ("JUNK_SETTING", "DESIGN"):
            self.assertNotIn(gone, names)
        for there in ("SOURCE_DATE_EPOCH", "FLOW", "SURFCPU_FPGA_OUT", "PATH", "LC_ALL", "TZ"):
            self.assertIn(there, names)

    def test_stand_ins_only_for_the_tests(self):
        r = self.make("--prerelease", "--skip-ddr", SURFCPU_RELEASE_TEST=None)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("SURFCPU_RELEASE_TEST", r.stderr)


class TestSeedAndSlack(ScratchRelease):
    """the third GPU review, M5 and M4: the release records the placer seed each bitstream was placed with (its
    build record's [openxc7] placer_seed, not a key fpga-build.sh never writes at the top), release-verify rebuilds
    each with exactly that seed, and a bitstream every seed of whose design list was thin (THIN) is refused without
    --prerelease"""

    def test_seed_recorded_and_pinned_by_verify(self):
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_SEEDS="arty_a7_35t/top=3")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(True)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(man["manifest_format"], 2)
        seeds = {(d["board"], d["name"]): d.get("placer_seed") for d in man["design"] if d["status"] == "built"}
        self.assertEqual(seeds[("arty_a7_35t", "top")], 3)
        self.assertEqual(seeds[("arty_a7_100t", "top")], 1)
        self.assertEqual({d.get("slack_margin") for d in man["design"] if d["status"] == "built"}, {"PASS"})
        self.assertEqual(self.tool("check", rel).returncode, 0)
        log = self.dir / "seeds.txt"
        r = self.tool("verify", rel, FAKE_FPGA_SEEDLOG=str(log))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(r.stdout.strip().splitlines()[-1].startswith("PASS:"), r.stdout)
        got = dict(line.split() for line in log.read_text().splitlines())
        self.assertEqual(got["arty_a7_35t/top"], "3")
        self.assertEqual(got["arty_a7_100t/top"], "1")
        self.assertEqual(got["arty_a7_35t/base"], "1")
        # a MANIFEST that names another seed than its build record fails release-check
        doctored = self.dir / "doctored" / rel.name
        shutil.copytree(rel, doctored)
        text = (doctored / "MANIFEST").read_text()
        (doctored / "MANIFEST").write_text(text.replace("placer_seed = 3", "placer_seed = 1", 1))
        r = self.tool("check", doctored)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("says placer seed 3, MANIFEST 1", r.stdout)
        # two records placed with other seeds: compare_records names the seed
        ra, rb = self.dir / "ra.toml", self.dir / "rb.toml"
        ra.write_text("[openxc7]\nplacer_seed = 1\n")
        rb.write_text("[openxc7]\nplacer_seed = 3\n")
        self.assertIn("openxc7.placer_seed 1 against 3", release.compare_records(ra, rb))

    def test_older_manifest_rebuilt_with_the_design_lists_seeds(self):
        r = self.make("--prerelease", "--skip-ddr", FAKE_FPGA_SEEDS="arty_a7_35t/top=3")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(True)
        text = (rel / "MANIFEST").read_text()
        self.assertIn("manifest_format = 2\n", text)
        # as releases before format 2 were written: no format, every seed 1, no slack margin
        old = text.replace("manifest_format = 2\n", "").replace("placer_seed = 3", "placer_seed = 1")
        (rel / "MANIFEST").write_text(old)
        self.assertEqual(self.tool("check", rel).returncode, 0)
        log = self.dir / "seeds.txt"
        r = self.tool("verify", rel, FAKE_FPGA_SEEDLOG=str(log))
        got = dict(line.split() for line in log.read_text().splitlines())
        self.assertEqual(set(got.values()), {"-"}, got)
        self.assertIn("predates format 2", r.stdout)
        self.assertNotIn("seed 3", r.stdout)            # 1.0.0-f791b8a1's design is named in its own report only

    def test_thin_needs_a_prerelease(self):
        r = self.make("--skip-ddr", "--prerelease", "--designs", "base,top", FAKE_FPGA_THIN="top")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        rel = self.rel_dir(True)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(len(man["thin"]), 2, man["thin"])
        self.assertEqual({d["slack_margin"] for d in man["design"] if d["name"] == "top"}, {"THIN"})
        self.assertEqual(self.tool("check", rel).returncode, 0)
        lost = self.dir / "lost" / rel.name
        shutil.copytree(rel, lost)
        text = (lost / "MANIFEST").read_text()
        (lost / "MANIFEST").write_text(text.replace("thin = [", "thin_was = [", 1))
        r = self.tool("check", lost)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("2 built THIN and 0 listed as THIN", r.stdout)
        # every design built, one THIN: without --prerelease nothing is written
        r = self.make("--out", self.dir / "complete", FAKE_FPGA_THIN="top")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("THIN", r.stderr)
        self.assertIn("PRERELEASE=1", r.stderr)
        self.assertFalse(any((self.dir / "complete").glob("*/MANIFEST")))


class TestCleanTree(ScratchRelease):
    """review m3: the clean-tree check follows the repository's own rules, not the user's git configuration"""

    def plant(self):
        (self.repo / "arch/riscv32").mkdir(parents=True)
        (self.repo / "arch/riscv32/extra.c").write_text("int planted;\n")

    def test_show_untracked_no(self):
        self.plant()
        self.git("config", "status.showUntrackedFiles", "no")
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("arch/riscv32/extra.c", r.stderr)

    def test_global_excludes(self):
        self.plant()
        ex = self.dir / "excludes"
        ex.write_text("*.c\n")
        self.git("config", "core.excludesFile", str(ex))
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("arch/riscv32/extra.c", r.stderr)

    def test_assume_unchanged(self):
        self.git("update-index", "--assume-unchanged", "surfcpu/release/release.txt")
        (self.sc / "release/release.txt").write_text(self.LIST.replace("2.3.4", "2.3.5"))
        r = self.make("--prerelease", "--skip-ddr")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("release.txt", r.stderr)

    def test_run_sh_dirty_flag(self):
        self.plant()
        self.git("config", "status.showUntrackedFiles", "no")
        e = dict(self.env, FAKE_DOCKER_ARGS="1")
        r = subprocess.run(["sh", str(self.sc / "devenv/run.sh"), "true"], cwd=self.sc, env=e, capture_output=True,
                           text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("SURFCPU_BUILD_DIRTY=1", r.stdout.split())


class TestNoForeignDeletes(ScratchRelease):
    """review m4: the tool deletes only what it made"""

    def precious(self, name: str) -> Path:
        d = self.dir / name
        d.mkdir()
        (d / "precious.txt").write_text("keep me\n")
        return d

    def test_logs(self):
        d = self.precious("notes")
        r = self.make("--prerelease", "--skip-ddr", "--logs", d)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertTrue((d / "precious.txt").is_file())

    def test_out_force(self):
        d = self.dir / "project" / self.rel_dir(True).name
        d.mkdir(parents=True)
        (d / "precious.txt").write_text("keep me\n")
        r = self.make("--prerelease", "--skip-ddr", "--out", d.parent, "--force")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("not a release", r.stderr)
        self.assertTrue((d / "precious.txt").is_file())
        r = self.make("--prerelease", "--skip-ddr", "--out", d.parent / "x", "--out-exact")
        self.assertEqual(r.returncode, 2, "--out-exact is gone: a release directory is named by its MANIFEST")

    def test_verify_work(self):
        self.assertEqual(self.make("--prerelease", "--skip-ddr").returncode, 0)
        w = self.dir / "work"
        w.mkdir()
        r = self.tool("verify", self.rel_dir(True), "--work", w)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(w.is_dir(), "the directory the user gave is kept")
        self.assertEqual(list(w.iterdir()), [])

    def test_fresh_checkout_work(self):
        self.assertEqual(self.make("--prerelease", "--skip-ddr").returncode, 0)
        w = self.dir / "fc"
        w.mkdir()
        e = dict(self.env)
        r = subprocess.run(["sh", str(self.sc / "tools/fresh-checkout.sh"), "--work", str(w), "--reference",
                            str(self.rel_dir(True)), "--", "PRERELEASE=1", "SKIP_DDR=1"], cwd=self.sc, env=e,
                           capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(w.is_dir())
        self.assertEqual(list(w.iterdir()), [])
        p = self.precious("fc2")
        r = subprocess.run(["sh", str(self.sc / "tools/fresh-checkout.sh"), "--work", str(p)], cwd=self.sc, env=e,
                           capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertTrue((p / "precious.txt").is_file())


class TestOddPaths(ScratchRelease):
    """what the review confirmed (its s5), kept: the whole cycle through make, in a checkout whose path holds
    spaces, quotes, a dollar sign, an ampersand, parentheses and a hash"""
    PLACE = 'it\'s "q" $HOME & (co) #1/SurfOS Restoration Project'

    def mk(self, *args):
        return subprocess.run(["make", "--no-print-directory", "-C", str(self.sc)] + list(args), env=self.env,
                              capture_output=True, text=True, stdin=subprocess.DEVNULL)

    def test_cycle(self):
        r = self.mk("release", "PRERELEASE=1", "SKIP_DDR=1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        name = self.rel_dir(True).name
        r = self.mk("release-check", f"RELEASE=build/release/{name}")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = self.mk("release-verify", f"RELEASE=build/release/{name}")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue(r.stdout.strip().splitlines()[-1].startswith("PASS:"), r.stdout)
        r = self.mk("fresh-checkout", f"REFERENCE=build/release/{name}", "ARGS=PRERELEASE=1 SKIP_DDR=1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("the release matches the reference", r.stdout)
        self.assertEqual([p.name for p in (self.sc / "build").iterdir()
                          if p.name in ("release-verify", "fresh-checkout") and any(p.iterdir())], [])


# ------------------------------------------------------------------------------- program.sh flash path
FAKE_OFL = (ROOT / "sim" / "board" / "tools" / "fake" / "openFPGALoader").resolve()
# the stand-in first, then only the system's own directories: never Homebrew's, where the real one lives
SAFE_PATH = f"{FAKE_OFL.parent}:/usr/bin:/bin:/usr/sbin:/sbin"


class FlashFixture(unittest.TestCase):
    """a release's bitstream and build record for tools/program.sh, with sim/board/tools/fake/openFPGALoader named
    by OPENFPGALOADER (program.sh's override) and first on a PATH without Homebrew: never the real openFPGALoader,
    since a board may be plugged in"""
    REL = "release"

    def setUp(self):
        self.dir = fresh(f"flash-{type(self).__name__}-{self._testMethodName}")
        self.rel = self.dir / self.REL
        self.d = self.rel / "fpga/arty_a7_100t/surfcpu_arty_a7_ddr"
        self.d.mkdir(parents=True)
        self.bit = self.d / "surfcpu_arty_a7_ddr.bit"
        self.bit.write_bytes(bitfile.synthetic())
        self.record(self.bit)
        stream, _ = bitfile.flash_image(self.bit.read_bytes(), "arty_a7_100t")
        (self.rel / "flash/arty_a7_100t").mkdir(parents=True)
        self.bin = self.rel / "flash/arty_a7_100t/surfcpu_arty_a7_ddr.bin"
        self.bin.write_bytes(stream)
        self.log = self.dir / "calls.log"
        self.env = {k: v for k, v in os.environ.items()
                    if k not in release.REFUSE_ENV + ("SURFCPU_FLASH_CONFIRM", "CABLE_SERIAL", "OPENFPGALOADER", "BIT",
                                                      "DESIGN", "BOARD", "MAKEFLAGS", "MFLAGS", "MAKELEVEL",
                                                      "SURFCPU_IN_CONTAINER")}
        self.env.update(PATH=SAFE_PATH, FAKE_LOG=str(self.log), FAKE_IDCODE="0x13631093", OPENFPGALOADER=str(FAKE_OFL))

    def record(self, bit: Path, **over):
        m = {"design": "surfcpu_arty_a7_ddr", "board": "arty_a7_100t", "part": "xc7a100tcsg324-1",
             "result": "pass", "bitstream_sha256": release.sha256(bit)}
        m.update(over)
        (bit.parent / "manifest.txt").write_text(
            "# SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by tools/fpga-build.sh\n"
            + "".join(f'{k} = "{v}"\n' for k, v in m.items()))

    def safe(self, e: dict) -> None:
        """never a real programmer: the stand-in by its full path, and the only openFPGALoader on PATH"""
        assert e["OPENFPGALOADER"] == str(FAKE_OFL), e["OPENFPGALOADER"]
        assert FAKE_OFL.read_text().startswith("#!/bin/sh\n# Fake openFPGALoader"), FAKE_OFL
        assert e["PATH"] == SAFE_PATH, e["PATH"]
        found = subprocess.run(["sh", "-c", "command -v openFPGALoader"], env=e, capture_output=True,
                               text=True).stdout.strip()
        assert found == str(FAKE_OFL), found

    def run_logged(self, cmd: list, **env):
        self.log.write_text("")
        e = dict(self.env, **env)
        self.safe(e)
        r = subprocess.run([str(a) for a in cmd], env=e, capture_output=True, text=True, stdin=subprocess.DEVNULL)
        return r, self.log.read_text()

    def program(self, *args, **env):
        return self.run_logged(["sh", ROOT / "tools/program.sh"] + list(args), **env)

    def make(self, *args, **env):
        return self.run_logged(["make", "--no-print-directory", "-C", ROOT, "-f", "mk/host.mk"] + list(args),
                               PYTHON_HOST=PY, **env)


class TestProgramFlash(FlashFixture):
    def test_confirmed_write(self):
        r, calls = self.program("flash", "arty_a7_100t", self.bit, SURFCPU_FLASH_CONFIRM="write-flash-arty_a7_100t")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t -f --verify {self.bit}", calls)
        self.assertIn("set the MODE jumper (JP1)", r.stdout)

    def test_refusals(self):
        r, calls = self.program("flash", "arty_a7_100t", self.bit)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not confirmed", r.stderr)
        self.assertNotIn(" -f ", calls)
        r, calls = self.program("flash", "arty_a7_100t", self.bit, SURFCPU_FLASH_CONFIRM="write-flash-arty_a7_35t")
        self.assertIn("not confirmed", r.stderr)
        self.assertNotIn(" -f ", calls)
        ok = {"SURFCPU_FLASH_CONFIRM": "write-flash-arty_a7_100t"}
        r, calls = self.program("flash", "arty_a7_100t", self.bin, **ok)
        self.assertIn("no .bit header", r.stderr)
        self.assertNotIn(" -f ", calls)
        alone = self.dir / "alone.bit"
        shutil.copyfile(self.bit, alone)
        r, calls = self.program("flash", "arty_a7_100t", alone, **ok)
        self.assertIn("flash needs the bitstream's build manifest", r.stderr)
        self.assertNotIn(" -f ", calls)
        r, calls = self.program("flash", "arty_a7_100t", self.bit, FAKE_CABLES="210319A28C7F 210319B0C8A6", **ok)
        self.assertIn("refusing to guess", r.stderr)
        self.assertNotIn(" -f ", calls)
        r, calls = self.program("flash", "arty_a7_100t", self.bit, FAKE_IDCODE="0x0362d093", **ok)
        self.assertIn("refusing", r.stderr)
        self.assertNotIn(" -f ", calls)
        data = bytearray(self.bit.read_bytes())
        data[-100] ^= 1
        self.bit.write_bytes(bytes(data))                                       # changed after the build
        r, calls = self.program("flash", "arty_a7_100t", self.bit, **ok)
        self.assertIn("describes another bitstream", r.stderr)
        self.assertNotIn(" -f ", calls)
        self.record(self.bit, result="fail")
        r, calls = self.program("flash", "arty_a7_100t", self.bit, **ok)
        self.assertIn("does not say the build passed", r.stderr)
        self.assertNotIn(" -f ", calls)

    def test_record_formats(self):
        """tools/program.sh reads build records of format 1 (no manifest_format) and 2, and refuses another"""
        ok = {"SURFCPU_FLASH_CONFIRM": "write-flash-arty_a7_100t"}
        self.record(self.bit, manifest_format="3")
        r, calls = self.program("flash", "arty_a7_100t", self.bit, **ok)
        self.assertIn("manifest format 3, which this script does not know", r.stderr)
        self.assertNotIn(" -f ", calls)
        self.record(self.bit, manifest_format="2")
        r, calls = self.program("flash", "arty_a7_100t", self.bit, **ok)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t -f --verify {self.bit}", calls)

    def test_volatile_never_flash(self):
        r, calls = self.program("volatile", "arty_a7_100t", self.bit)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t {self.bit}", calls)
        self.assertNotIn(" -f ", calls)


class TestHostQuoting(FlashFixture):
    """review m5: mk/host.mk's targets take a path with spaces in BIT, RELEASE, MOUNT, ..."""
    REL = "SurfOS Restoration Project/release 1"

    def test_program_targets(self):
        r, calls = self.make("program-flash", "BOARD=arty_a7_100t", f"BIT={self.bit}",
                             SURFCPU_FLASH_CONFIRM="write-flash-arty_a7_100t")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t -f --verify {self.bit}\n", calls)
        r, calls = self.make("program-volatile", "BOARD=arty_a7_100t", f"BIT={self.bit}")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t {self.bit}\n", calls)
        self.assertNotIn(" -f ", calls)
        # without BIT, program.sh's own default (the last build of DESIGN under SURFCPU_FPGA_OUT)
        r, calls = self.make("program-volatile", "BOARD=arty_a7_100t", "DESIGN=surfcpu_arty_a7_ddr",
                             SURFCPU_FPGA_OUT=str(self.rel / "fpga"))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t {self.bit}\n", calls)

    def test_release_and_card_targets(self):
        gone = self.dir / "no such dir" / "2.3.4-pre-01234567"
        for target in ("release-check", "release-verify"):
            r, calls = self.make(target, f"RELEASE={gone}")
            self.assertNotEqual(r.returncode, 0, target)
            self.assertIn(str(gone), r.stdout + r.stderr, target)
        # deploy-sd in its test mode (never diskutil): MOUNT and a path inside DEPLOY_ARGS, quoted in the value
        pkg = self.dir / "SurfOS Restoration Project" / "SURFOS.BIN"
        r, calls = self.make("deploy-sd", f"MOUNT={self.dir / 'my card'}", f'DEPLOY_ARGS=--image "{pkg}"',
                             SURFCPU_DEPLOY_TEST="1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f"no package at {pkg}", r.stdout + r.stderr)
        pkg.write_bytes(release.load("surfimg").make_package(bytes(256), 0x80000000, build_id=0x1234abcd))
        r, calls = self.make("deploy-sd", f"MOUNT={self.dir / 'my card'}", f'DEPLOY_ARGS=--image "{pkg}"',
                             SURFCPU_DEPLOY_TEST="1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn(f"{self.dir / 'my card'} is not a directory", r.stdout + r.stderr)
        self.assertEqual(calls, "")


class TestBitfileStrict(FlashFixture):
    """review c1: one rule for a .bit header in tools/bitfile.py and tools/program.sh, fields a, b, c, d once each
    and in this order, then e; anything else, and a stream with no IDCODE or DESYNC, fails cleanly"""

    @staticmethod
    def field(k: str, s: str) -> bytes:
        raw = s.encode() + b"\0"
        return k.encode() + struct.pack(">H", len(raw)) + raw

    def header(self, keys: list, stream: bytes) -> bytes:
        val = {"a": "top.frames;Generator=xc7frames2bit", "b": "xc7a100tcsg324-1", "B": "xc7a35tcsg324-1",
               "c": "2026/10/16", "d": "13:41:43"}
        return (bitfile.MAGIC + b"".join(self.field(k.lower(), val[k]) for k in keys) + b"e"
                + struct.pack(">I", len(stream)) + stream)

    def cases(self) -> dict:
        _, _, start = bitfile.parse_header(bitfile.synthetic())
        stream = bitfile.synthetic()[start:]
        return {name: self.header(keys, stream) for name, keys in (
            ("no field b", ["a", "c", "d"]), ("field b twice", ["a", "b", "B", "c", "d"]),
            ("field b twice, the other part first", ["a", "B", "b", "c", "d"]),
            ("field c twice", ["a", "b", "c", "c", "d"]), ("b before a", ["b", "a", "c", "d"]),
            ("no field d", ["a", "b", "c"]))}

    def test_header_rule(self):
        ok = self.header(["a", "b", "c", "d"], bitfile.synthetic()[bitfile.parse_header(bitfile.synthetic())[2]:])
        self.assertEqual(bitfile.parse_header(ok)[0]["b"], "xc7a100tcsg324-1")
        for name, data in self.cases().items():
            with self.subTest(name):
                p = self.dir / "case.bit"
                p.write_bytes(data)
                for fn in (bitfile.parse_header, lambda d: bitfile.stamp(d, 7), bitfile.report,
                           lambda d: bitfile.flash_image(d, "arty_a7_100t")):
                    self.assertRaises(bitfile.BitError, fn, data)
                for args in (["stamp", p, "--epoch", "7"], ["flash", p, "-o", self.dir / "x.bin", "--board",
                                                            "arty_a7_100t"], ["info", p]):
                    r = subprocess.run([PY, ROOT / "tools/bitfile.py"] + [str(a) for a in args], capture_output=True,
                                       text=True)
                    self.assertEqual(r.returncode, 1, (args, r.stdout, r.stderr))
                    self.assertTrue(r.stderr.startswith("bitfile: "), r.stderr)
                    self.assertNotIn("Traceback", r.stderr)
                self.assertEqual(p.read_bytes(), data, "stamp leaves a file it refuses alone")
                r, calls = self.program("volatile", "arty_a7_100t", p)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn("refusing", r.stderr)
                self.assertNotIn(str(p), calls)

    def test_random_damage(self):
        import random
        good = bitfile.synthetic()
        _, _, start = bitfile.parse_header(good)
        rng = random.Random(1)
        odd = []
        for n in range(3000):
            d = bytearray(good)
            if n % 3 == 0:
                d[rng.randrange(0, start)] = rng.randrange(256)
            elif n % 3 == 1:
                d = d[:rng.randrange(0, start + 64)]
            else:
                k = rng.randrange(13, start)
                d = d[:k] + bytes([rng.randrange(256)]) + d[k:]
            for fn in (bitfile.parse_header, bitfile.report, lambda x: bitfile.stamp(x, 7),
                       lambda x: bitfile.flash_image(x, "arty_a7_100t")):
                try:
                    fn(bytes(d))
                except bitfile.BitError:
                    pass
                except Exception as e:                                          # noqa: BLE001
                    odd.append((n, type(e).__name__, str(e)))
        self.assertEqual(odd, [])

    def test_info_on_an_incomplete_stream(self):
        good = bitfile.synthetic()
        _, _, start = bitfile.parse_header(good)
        cut = good[start:start + 80]
        p = self.dir / "short.bit"
        p.write_bytes(good[:start - 4] + struct.pack(">I", len(cut)) + cut)
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "info", p], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("PROBLEM: no IDCODE", r.stdout)
        self.assertIn("PROBLEM: no DESYNC", r.stdout)
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "info", p.with_name("whole.bit")], capture_output=True,
                           text=True)
        self.assertEqual(r.returncode, 1)
        p.with_name("whole.bit").write_bytes(good)
        r = subprocess.run([PY, ROOT / "tools/bitfile.py", "info", p.with_name("whole.bit")], capture_output=True,
                           text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn("PROBLEM", r.stdout)


if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2)
