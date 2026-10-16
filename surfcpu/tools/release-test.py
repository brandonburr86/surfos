#!/usr/bin/env python3
"""SurfCPU release tooling: tests (make test-release, on make check)
--------------------
File: release-test.py Date: 10/16/26 (SurfCPU)
--------------------
Without Docker, a board or a bitstream build (docs/RELEASE.md section 9):
  * release.py, bitfile.py and this file parse as Python 3.9 (the Mac's /usr/bin/python3);
  * tools/bitfile.py: its self-test (header stamp, packets, flash image, configuration time);
  * tools/release.py's TOML subset, its comparison and the likely causes it names for a .bit whose
    header time or frames differ, a SURFOS.BIN whose payload differs (with the ELF's symbol), a
    missing and an extra artifact;
  * tools/release.py make in a scratch git repository, with stand-ins for devenv/run.sh,
    tools/fpga-build.sh and docker: a dirty tree, a foreign SOURCE_DATE_EPOCH and missing designs
    without PRERELEASE are refused; a complete release; a DDR design that fails makes a pre-release
    only with --prerelease; any other design that fails stops the release; the release's own files
    check against SHA256SUMS and MANIFEST;
  * tools/program.sh's flash path with the board tests' fake openFPGALoader
    (sim/board/tools/fake), on a release's bitstream: written with -f --verify only when
    confirmed, never the flash image or a bitstream without its build record or with other bytes.
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
        notes = release.compare(self.ref, self.new)[1]
        self.assertTrue(any("only in the rebuild: extra.txt" in n for n in notes))

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
FAKE_RUN = r'''#!/bin/sh
# stand-in for devenv/run.sh: the software build, or litedram-gen ($FAKE_LITEDRAM_FAIL)
set -eu
repo=$(git rev-parse --show-toplevel)
id=$(git -C "$repo" rev-parse HEAD | cut -c1-8)
case " $* " in
  *" litedram-gen "*)
    case "${FAKE_LITEDRAM_FAIL:-}" in
      '')   exit 0 ;;
      lock) echo "lock.sh: DIFFERS   build/litedram/arty/gateware/litedram_core.v (sha256 bc92, arty.sha256 has 150b)"
            exit 2 ;;
      *)    echo "setup.sh: cannot fetch"; exit 1 ;;
    esac ;;
esac
fw=$repo/surfcpu/build/fw
k=$repo/build/riscv32/arty_a7
mkdir -p "$fw/gen" "$k"
printf '#define FW_BUILD_ID 0x%sU\n#define FW_BUILD_DIRTY 0\n' "$id" > "$fw/gen/fw_build.h"
for f in surfboot bramdiag; do printf '%08x\n' "$SOURCE_DATE_EPOCH" > "$fw/$f.mem"; printf 'ELF %s\n' "$f" > "$fw/$f.elf"; done
printf 'ELF ddrdiag\n' > "$fw/ddrdiag.elf"
printf 'ELF surfos\n' > "$k/surfos.elf"
printf 'payload %s' "$SOURCE_DATE_EPOCH" > "$k/payload"
python3 "$repo/surfcpu/tools/surfimg.py" pack --bin "$k/payload" --load-addr 0x80000000 --build-id "$id" -o "$k/SURFOS.BIN" >/dev/null
python3 "$repo/surfcpu/tools/surfimg.py" pack --bin "$k/payload" --load-addr 0x80000000 --build-id "$id" --name DDRdiag -o "$fw/DDRDIAG.BIN" >/dev/null
'''

FAKE_FPGA = r'''#!/usr/bin/env python3
# stand-in for tools/fpga-build.sh: --design D BOARD; fails the designs named in $FAKE_FPGA_FAIL
import hashlib, os, subprocess, sys
from pathlib import Path
design, board = sys.argv[2], sys.argv[3]
root = Path(os.environ["FAKE_ROOT"])
sys.path.insert(0, str(root / "tools"))
import bitfile
base = root / "build" / "fpga" / board / design
base.mkdir(parents=True, exist_ok=True)
commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
part, idcode = ("xc7a100tcsg324-1", 0x03631093) if board == "arty_a7_100t" else ("xc7a35tcsg324-1", 0x0362D093)
fail = design in os.environ.get("FAKE_FPGA_FAIL", "").split(",")
bit = base / f"{design}.bit"
if bit.exists():
    bit.unlink()
head = [f'design = "{design}"', f'board = "{board}"', f'part = "{part}"', f'source_revision = "{commit}"',
        "source_dirty = false", f'build_time = {os.environ["SOURCE_DATE_EPOCH"]}', "generated_files_match = true",
        'rom_sha256 = ""', 'kernel_sha256 = ""', "placer_seed = 1"]
if fail:
    head += ['result = "fail"', 'reason = "timing not met (nextpnr: dram_clk 63.5 MHz of 100 MHz)"']
else:
    data = bitfile.stamp(bitfile.synthetic(idcode=idcode, part=part), int(os.environ["SOURCE_DATE_EPOCH"]))
    bit.write_bytes(data)
    head += ['result = "pass"', f'bitstream_sha256 = "{hashlib.sha256(data).hexdigest()}"']
head += ["", "[openxc7]", 'check_synthesis = "PASS"', 'check_lvs = "PASS"', 'timing = ["clock sys_clk: PASS", ]',
         "wns_ns = 10.5"]
(base / "manifest.txt").write_text("# SurfCPU bitstream build manifest\n" + "\n".join(head) + "\n")
sys.exit(1 if fail else 0)
'''

FAKE_DOCKER = r'''#!/bin/sh
# stand-in for docker: image inspect, ps, run ... cat
case "$1" in
  image) echo "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" ;;
  ps)    ;;
  run)   echo "fake-tool 1.0" ;;
  *)     exit 1 ;;
esac
'''


class TestMake(unittest.TestCase):
    COPY = ("tools/release.py", "tools/bitfile.py", "tools/deploy-sd.py", "tools/surfimg.py", "tools/socspec.py",
            "spec/soc.toml", "boards/arty_a7/boards.txt", "devenv/run.sh", "tools/openxc7-build.sh",
            "firmware/tests/deploy/fake_diskutil.py", "firmware/tests/deploy/fake_mount.py") + tuple(
        f"docs/{d}" for d in ("SURFBUS.md", "ISA.md", "BOOT-ABI.md", "MODULES.md", "MEMORY-MAP.md"))

    def setUp(self):
        self.dir = fresh(f"make-{self._testMethodName}")
        self.repo = self.dir / "repo"
        self.sc = self.repo / "surfcpu"
        for rel in self.COPY:
            (self.sc / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / rel, self.sc / rel)
        for d in ("base", "top", "dd"):
            (self.sc / "boards/arty_a7/designs").mkdir(parents=True, exist_ok=True)
            (self.sc / f"boards/arty_a7/designs/{d}.files").write_text(f"top {d}\n")
        (self.sc / "release").mkdir()
        (self.sc / "release/release.txt").write_text(
            "version 2.3.4\nboard arty_a7_100t\nboard arty_a7_35t\ndesign base - -\ndesign top test -\n"
            "design dd boot ddr\ncard-cmdline sd=rw\n")
        (self.repo / ".gitignore").write_text("build/\n__pycache__/\n")
        fakes = self.dir / "fakes"
        fakes.mkdir()
        for name, text in (("run.sh", FAKE_RUN), ("fpga-build", FAKE_FPGA), ("docker", FAKE_DOCKER)):
            (fakes / name).write_text(text)
            (fakes / name).chmod(0o755)
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "scratch")
        self.env = {k: v for k, v in os.environ.items() if k not in ("SOURCE_DATE_EPOCH", "SURFCPU_FPGA_OUT")}
        self.env.update(SURFCPU_RELEASE_RUN=str(fakes / "run.sh"), SURFCPU_RELEASE_FPGA=str(fakes / "fpga-build"),
                        SURFCPU_RELEASE_DOCKER=str(fakes / "docker"), FAKE_ROOT=str(self.sc), HOME=str(self.dir))
        self.commit = self.git("rev-parse", "HEAD").strip()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo)] + list(args), capture_output=True, text=True,
                              check=True).stdout

    def make(self, *args, **env):
        e = dict(self.env, **env)
        return subprocess.run([PY, self.sc / "tools/release.py", "make"] + list(args), cwd=self.sc, env=e,
                              capture_output=True, text=True)

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
        rel = self.sc / f"build/release/2.3.4-{self.commit[:8]}"
        self.assertTrue((rel / "MANIFEST").is_file(), r.stdout)
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(man["status"], "complete")
        self.assertIs(man["complete"], True)
        self.assertEqual(man["commit"], self.commit)
        self.assertEqual(man["missing"], [])
        listed = release.read_sums(rel / "SHA256SUMS")
        for p in ("SURFOS.BIN", "DDRDIAG.BIN", "card/BOOT/SURFOS.BIN", "card/BOOT/CMDLINE.TXT",
                  "fpga/arty_a7_100t/dd/dd.bit", "fpga/arty_a7_35t/base/base.bit", "flash/arty_a7_100t/dd.bin",
                  "flash/arty_a7_35t/top.bin", "rom/surfboot.mem", "debug/surfos.elf"):
            self.assertIn(p, listed)
        self.assertNotIn("fpga/arty_a7_100t/dd/manifest.txt", listed)
        self.assertTrue((rel / "fpga/arty_a7_100t/dd/manifest.txt").is_file())
        self.assertEqual((rel / "card/BOOT/CMDLINE.TXT").read_text(), "sd=rw\n")
        self.assertEqual((rel / "card/BOOT/SURFOS.BIN").read_bytes(), (rel / "SURFOS.BIN").read_bytes())
        r = subprocess.run([PY, self.sc / "tools/release.py", "check", rel], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        # a second release of the same commit is the same bytes, and replaces nothing without --force
        r = self.make()
        self.assertEqual(r.returncode, 1)
        self.assertIn("exists", r.stderr)
        r = self.make("--out", str(self.dir / "again"))
        self.assertEqual(r.returncode, 0, r.stderr)
        again = self.dir / "again" / rel.name
        rows, _ = release.compare(rel, again)
        self.assertTrue(rows and all(s == "match" for _, s, _ in rows), rows)

    def test_ddr_failure(self):
        r = self.make(FAKE_FPGA_FAIL="dd")
        self.assertEqual(r.returncode, 1)
        self.assertIn("PRERELEASE=1", r.stderr)
        self.assertIn("dram_clk 63.5 MHz", r.stderr)
        self.assertFalse(list((self.sc / "build/release").glob("*/MANIFEST")))
        r = self.make("--prerelease", FAKE_FPGA_FAIL="dd")
        self.assertEqual(r.returncode, 0, r.stderr)
        rel = self.sc / f"build/release/2.3.4-pre-{self.commit[:8]}"
        man = release.parse_toml((rel / "MANIFEST").read_text())
        self.assertEqual(man["status"], "prerelease")
        self.assertIs(man["complete"], False)
        self.assertEqual(len(man["missing"]), 2)
        self.assertIn("dram_clk", man["missing"][0])
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


# ------------------------------------------------------------------------------- program.sh flash path
class TestProgramFlash(unittest.TestCase):
    """tools/program.sh flash on a release's bitstream, with sim/board/tools/fake/openFPGALoader"""

    def setUp(self):
        self.dir = fresh(f"flash-{self._testMethodName}")
        self.rel = self.dir / "release"
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
        self.env = {k: v for k, v in os.environ.items() if k not in ("SURFCPU_FLASH_CONFIRM", "CABLE_SERIAL",
                                                                       "OPENFPGALOADER", "BIT", "DESIGN")}
        self.env.update(PATH=f"{ROOT / 'sim/board/tools/fake'}:{os.environ['PATH']}", FAKE_LOG=str(self.log),
                        FAKE_IDCODE="0x13631093")

    def record(self, bit: Path, **over):
        m = {"design": "surfcpu_arty_a7_ddr", "board": "arty_a7_100t", "part": "xc7a100tcsg324-1",
             "result": "pass", "bitstream_sha256": release.sha256(bit)}
        m.update(over)
        (bit.parent / "manifest.txt").write_text(
            "# SurfCPU bitstream build manifest (docs/BUILD-HOST.md), written by tools/fpga-build.sh\n"
            + "".join(f'{k} = "{v}"\n' for k, v in m.items()))

    def program(self, *args, **env):
        self.log.write_text("")
        e = dict(self.env, **env)
        r = subprocess.run(["sh", ROOT / "tools/program.sh"] + [str(a) for a in args], env=e, capture_output=True,
                           text=True, stdin=subprocess.DEVNULL)
        return r, self.log.read_text()

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

    def test_volatile_never_flash(self):
        r, calls = self.program("volatile", "arty_a7_100t", self.bit)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn(f"openFPGALoader -b arty_a7_100t {self.bit}", calls)
        self.assertNotIn(" -f ", calls)


if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2)
