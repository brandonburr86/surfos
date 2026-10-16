#!/usr/bin/env python3
"""SurfCPU firmware tests: the card and image tools
--------------------
File: test_tools.py Date: 10/15/26 (SurfCPU)
--------------------
Tests of tools/surfimg.py, tools/bin2mem.py, tools/mkcard.py, tools/isa-audit.py and
tools/deploy-sd.py, run as the owner would run them (subprocesses), in the toolchain
container (make test-fw):

  surfimg     the CRC test vectors, packing from ELFs (payload = objcopy's binary) and raw
              payloads, refusing x86/RVC ELFs and packages SurfBoot would refuse, and the
              Python checks agreeing with SurfBoot's C checks on crafted and randomly damaged
              packages (several seeds)
  bin2mem     the ROM .mem format: 16384 lines, little-endian words, padding, --check
  mkcard      images verified by fsck.fat and read back with mtools, byte for byte, for
              plain, fragmented, long-name, 4 KiB and 32 KiB cluster and full volumes
  isa-audit   the firmware ELFs pass; programs with compressed, atomic, float, SRET,
              unknown-CSR, read-only-CSR-write and RV64-shift instructions fail
  deploy-sd   against a directory standing in for a card, with fake diskutil and mount: every
              refusal, dry run, first install, update with backup, user files kept, corrupted
              copies caught, system files, explicit backup

    python3 firmware/tests/test_tools.py [-v] [-k PATTERN]
"""
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
FW = ROOT / "build" / "fw"
WORK = FW / "tooltests"
sys.path.insert(0, str(TOOLS))
import mkcard                                                # noqa: E402
import surfimg                                               # noqa: E402

PY = sys.executable


def run(*cmd, env=None, check=False, cwd=None):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env, cwd=cwd, timeout=600)
    if check and r.returncode != 0:
        raise AssertionError(f"{' '.join(map(str, cmd))} failed ({r.returncode}):\n{r.stdout}{r.stderr}")
    return r


def tool(name, *args, **kw):
    return run(PY, TOOLS / name, *args, **kw)


def fresh(name: str) -> Path:
    d = WORK / name
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    return d


def elf32(machine=243, flags=0, entry=0x80000000, segs=((0x80000000, b"\x13\x00\x00\x00" * 4, 64),)):
    """A minimal ELF32 little-endian executable: (paddr, data, memsz) segments."""
    ph_off, n = 52, len(segs)
    data_off = ph_off + 32 * n
    blob = b""
    phdrs = b""
    for paddr, data, memsz in segs:
        phdrs += struct.pack("<8I", 1, data_off + len(blob), paddr, paddr, len(data), memsz, 7, 4)
        blob += data
    hdr = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    hdr += struct.pack("<HHIIIIIHHHHHH", 2, machine, 1, entry, ph_off, 0, flags, 52, 32, n, 40, 0, 0)
    return hdr + phdrs + blob


class TestSurfimg(unittest.TestCase):
    def test_selftest_and_crc_vectors(self):
        r = tool("surfimg.py", "selftest")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("PASS", r.stdout)
        d = fresh("crc")
        vectors = [(b"", "00000000"), (b"a", "e8b7be43"), (b"123456789", "cbf43926"), (bytes(range(256)), "29058c73")]
        for i, (data, _) in enumerate(vectors):
            (d / f"v{i}").write_bytes(data)
        out = tool("surfimg.py", "crc", *[d / f"v{i}" for i in range(4)], check=True).stdout.split("\n")
        for i, (_, want) in enumerate(vectors):
            self.assertTrue(out[i].startswith(want), out[i])

    def test_pack_from_elf_matches_objcopy(self):
        for name in ("hello", "ddrdiag"):
            d = fresh(f"pack_{name}")
            elf = FW / f"{name}.elf"
            pkg, pay = d / "PKG.BIN", d / "payload.bin"
            r = tool("surfimg.py", "pack", "--elf", elf, "-o", pkg, "--payload-out", pay, "--name", name.upper(),
                     "--version", "2.3.4", "--build-id", "abcdef1234")
            self.assertEqual(r.returncode, 0, r.stderr)
            ref = d / "objcopy.bin"
            run("riscv64-unknown-elf-objcopy", "-O", "binary", elf, ref, check=True)
            self.assertEqual(pay.read_bytes(), ref.read_bytes(), "payload differs from objcopy -O binary")
            data = pkg.read_bytes()
            h = surfimg.parse_header(data)
            self.assertEqual(h["image_version"], 2 << 16 | 3 << 8 | 4)
            self.assertEqual(h["build_id"], 0xABCDEF12)
            self.assertEqual(h["load_addr"], 0x80000000)
            self.assertEqual(data[64:], pay.read_bytes())
            self.assertEqual(tool("surfimg.py", "verify", pkg).returncode, 0)
            info = tool("surfimg.py", "info", pkg).stdout
            self.assertIn("valid", info)
            self.assertIn("6 payload CRC", info)

    def test_refuses_foreign_elves(self):
        d = fresh("foreign")
        x86 = d / "x86.elf"
        x86.write_bytes(elf32(machine=3))
        r = tool("surfimg.py", "pack", "--elf", x86, "-o", d / "X.BIN", "--no-audit")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not RISC-V", r.stderr)
        rvc = d / "rvc.elf"
        rvc.write_bytes(elf32(flags=1))
        r = tool("surfimg.py", "pack", "--elf", rvc, "-o", d / "X.BIN", "--no-audit")
        self.assertIn("compressed", r.stderr)
        hard = d / "hf.elf"
        hard.write_bytes(elf32(flags=4))
        self.assertIn("hard-float", tool("surfimg.py", "pack", "--elf", hard, "-o", d / "X.BIN", "--no-audit").stderr)
        self.assertFalse((d / "X.BIN").exists())

    def test_verify_refuses_non_packages(self):
        d = fresh("verify")
        good = surfimg.make_package(bytes(1000), 0x80000000)
        cases = {
            "x86.bin": (b"\xeb\x63\x90" + bytes(600), "IMG_MAGIC"),
            "kernel.elf": (elf32(), "IMG_MAGIC"),
            "short.bin": (good[:40], "IMG_SHORT"),
            "truncated.bin": (good[:-10], "IMG_SIZE"),
            "padded.bin": (good + b"\0", "IMG_SIZE"),
            "crc.bin": (good[:-1] + bytes([good[-1] ^ 1]), "IMG_CRC"),
        }
        for name, (data, code) in cases.items():
            (d / name).write_bytes(data)
            r = tool("surfimg.py", "verify", d / name)
            self.assertEqual(r.returncode, 1, name)
            self.assertIn(code, r.stdout, name)

    def test_pack_raw_payload_and_refusals(self):
        d = fresh("raw")
        (d / "p.bin").write_bytes(bytes(range(256)) * 8)
        r = tool("surfimg.py", "pack", "--bin", d / "p.bin", "--load-addr", "0x80100000", "--entry", "0x80100010",
                 "--mem-size", "64K", "-o", d / "R.BIN", "--name", "Raw")
        self.assertEqual(r.returncode, 0, r.stderr)
        h = surfimg.parse_header((d / "R.BIN").read_bytes())
        self.assertEqual((h["load_addr"], h["entry"], h["mem_size"], h["payload_size"]),
                         (0x80100000, 0x80100010, 65536, 2048))
        r = tool("surfimg.py", "pack", "--bin", d / "p.bin", "--load-addr", "0x00010000", "-o", d / "BAD.BIN")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("IMG_RANGE", r.stderr)
        r = tool("surfimg.py", "pack", "--bin", d / "p.bin", "--load-addr", "0x8EFFF000", "--mem-size", "8K",
                 "-o", d / "BAD.BIN")
        self.assertIn("ddr_top", r.stderr)
        self.assertFalse((d / "BAD.BIN").exists())

    def test_python_and_surfboot_checks_agree(self):
        """The same packages through surfimg.check and through SurfBoot's image.c (fwtest)."""
        fwtest = FW / "host" / "fwtest"
        if not fwtest.exists():
            self.skipTest("build/fw/host/fwtest not built")
        d = fresh("agree")
        base = bytes((i * 37 + 11) & 0xFF for i in range(3000))
        pkgs = {
            "ok": surfimg.make_package(base, 0x80000000, 0x80000100, 8192),
            "magic": surfimg.make_package(base, 0x80000000, magic=0x12345678),
            "version": surfimg.make_package(base, 0x80000000, version=3),
            "hcrc": surfimg.make_package(base, 0x80000000, header_crc32=1),
            "isa": surfimg.make_package(base, 0x80000000, isa=0x20 | 0xF),
            "isa_no_i": surfimg.make_package(base, 0x80000000, isa=0xE),
            "isa_zero": surfimg.make_package(base, 0x80000000, isa=0),
            "isa_i_only": surfimg.make_package(base, 0x80000000, isa=1),
            "flags": surfimg.make_package(base, 0x80000000, flags=0x80000001),
            "below_small_top": surfimg.make_package(base, 0x83000000 - 4096, 0x83000000 - 4096, 4096),
            "in_small_top": surfimg.make_package(base, 0x83000000 - 2048, 0x83000000 - 2048, 4096),
            "platform": surfimg.make_package(base, 0x80000000, platform=7),
            "align": surfimg.make_package(base, 0x80000001, 0x80000004),
            "range_low": surfimg.make_package(base, 0x7FFF0000, 0x7FFF0000),
            "range_top": surfimg.make_package(base, 0x8EFFFF00, 0x8EFFFF00, 4096),
            "mem": surfimg.make_package(base, 0x80000000, mem_size=10),
            "entry_mis": surfimg.make_package(base, 0x80000000, 0x80000002),
            "entry_out": surfimg.make_package(base, 0x80000000, 0x80000000 + 3000, 8192),
            "zero": surfimg.make_header(load_addr=0x80000000, payload=b"", mem_size=4, entry=0x80000000),
        }
        rng = random.Random(7)
        for seed in range(60):                              # random damage, several seeds
            p = bytearray(pkgs["ok"])
            for _ in range(rng.randint(1, 3)):
                p[rng.randrange(len(p))] ^= 1 << rng.randrange(8)
            if seed % 3 == 0:                               # damage with a fixed header CRC
                p[0x3C:0x40] = struct.pack("<I", surfimg.crc32(bytes(p[:60])))
            pkgs[f"random{seed}"] = bytes(p)
        c = mkcard.Card(size=40 << 20, cluster=512)
        c.add_dir("/BOOT")
        names = {}
        for i, (k, v) in enumerate(pkgs.items()):
            names[k] = f"/BOOT/P{i:04d}.BIN"
            c.add_file(names[k], v)
        img = d / "card.img"
        c.build(str(img))
        disagree = []
        verdicts = {}
        runs = [(k, 256 << 20) for k in pkgs] + [(k, 64 << 20) for k in pkgs if not k.startswith("random")]
        for k, ddr in runs:                                 # ddr_top is the top 16 MiB of either size
            want, _ = surfimg.verdict(surfimg.check(pkgs[k], ram_size=ddr))
            r = run(fwtest, "boot", "--card", img, "--path", names[k], "--ddr-size", str(ddr), "--quiet")
            got = "OK" if "RESULT OK" in r.stdout else r.stdout.split("code=")[1].split()[0]
            verdicts[k, ddr] = got
            if (want or "OK") != got:
                disagree.append(f"{k} with {ddr >> 20} MiB: surfimg {want or 'OK'}, SurfBoot {got}")
        self.assertEqual(disagree, [], "\n".join(disagree))
        for k, ddr, code in (("isa_no_i", 256 << 20, "IMG_ISA"), ("isa_zero", 256 << 20, "IMG_ISA"),
                             ("isa_i_only", 256 << 20, "OK"), ("flags", 256 << 20, "OK"),
                             ("below_small_top", 64 << 20, "OK"), ("in_small_top", 64 << 20, "IMG_RANGE"),
                             ("in_small_top", 256 << 20, "OK")):
            self.assertEqual(verdicts[k, ddr], code, f"{k} with {ddr >> 20} MiB")
        img.unlink()


class TestBin2mem(unittest.TestCase):
    def test_format(self):
        d = fresh("bin2mem")
        data = bytes([0x13, 0x05, 0x10, 0x00, 0xAA, 0xBB, 0xCC]) + bytes(5)
        (d / "x.bin").write_bytes(data)
        tool("bin2mem.py", d / "x.bin", d / "x.mem", check=True)
        lines = (d / "x.mem").read_text().splitlines()
        self.assertEqual(len(lines), 16384)
        self.assertEqual(lines[0], "00100513")              # little-endian word: addi a0, zero, 1
        self.assertEqual(lines[1], "00ccbbaa")
        self.assertEqual(lines[2], "00000000")
        self.assertTrue(all(len(x) == 8 for x in lines))
        self.assertEqual(tool("bin2mem.py", "--check", d / "x.bin", d / "x.mem").returncode, 0)
        bad = lines[:]
        bad[100] = "deadbeef"
        (d / "y.mem").write_text("\n".join(bad) + "\n")
        r = tool("bin2mem.py", "--check", d / "x.bin", d / "y.mem")
        self.assertEqual(r.returncode, 1)
        self.assertIn("line 101", r.stderr)
        (d / "big.bin").write_bytes(bytes(65537))
        self.assertEqual(tool("bin2mem.py", d / "big.bin", d / "big.mem").returncode, 1)
        self.assertEqual(tool("bin2mem.py", "--check", FW / "surfboot.bin", FW / "surfboot.mem").returncode, 0)


def extract_partition(img: Path, out: Path):
    """Copy the first partition of img into out, keeping holes (images are sparse)."""
    with mkcard.Image(str(img)) as im:
        start, count = im.part_lba * 512, im.part_sectors * 512
    with open(img, "rb") as src, open(out, "wb") as dst:
        dst.truncate(count)
        src.seek(start)
        done = 0
        while done < count:
            chunk = src.read(min(1 << 20, count - done))
            if not chunk:
                break
            if chunk.count(0) != len(chunk):
                dst.seek(done)
                dst.write(chunk)
            done += len(chunk)


class TestMkcard(unittest.TestCase):
    CONFIGS = [
        ("plain", dict(size=64 << 20, cluster=512), {}),
        ("interleave", dict(size=64 << 20, cluster=512, fragment="interleave"), {}),
        ("reverse", dict(size=64 << 20, cluster=512, fragment="reverse"), {}),
        ("random", dict(size=64 << 20, cluster=512, fragment="random", seed=3), {}),
        ("lfn", dict(size=64 << 20, cluster=512, lfn=True), {}),
        ("4k", dict(size=300 << 20, cluster=4096), {}),
        ("32k", dict(size=(2 << 30) + (32 << 20), cluster=32768), {}),
        ("full", dict(size=40 << 20, cluster=512), {"fill": True}),
    ]

    def files(self, rng):
        f = {"/BOOT/SURFOS.BIN": surfimg.make_package(bytes(rng.getrandbits(8) for _ in range(70000)), 0x80000000),
             "/BOOT/CMDLINE.TXT": b"console=uart\n",
             "/SYSTEM/CONFIG.TXT": b"x" * 1500,
             "/USERS/DOCUMENTS/Notes from the Mac.txt": b"long names live here\n",
             "/EMPTY.TXT": b""}
        for i in range(40):
            f[f"/SYSTEM/F{i:03d}.DAT"] = bytes(rng.getrandbits(8) for _ in range(rng.randint(1, 3000)))
        return f

    def test_images_pass_fsck_and_mtools(self):
        env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
        for name, kw, extra in self.CONFIGS:
            with self.subTest(name):
                d = fresh(f"mkcard_{name}")
                rng = random.Random(name)
                files = self.files(rng)
                c = mkcard.Card(**kw)
                for p in mkcard.STANDARD_DIRS:
                    c.add_dir(p)
                for p, data in files.items():
                    c.add_file(p, data)
                c.fill = extra.get("fill", False)
                img = d / "card.img"
                info = c.build(str(img))
                part = d / "part.img"
                extract_partition(img, part)
                r = run("fsck.fat", "-n", "-v", part)
                self.assertEqual(r.returncode, 0, f"fsck.fat on {name}:\n{r.stdout}{r.stderr}")
                self.assertNotIn("Free cluster summary wrong", r.stdout)
                at = f"{img}@@{info['part_lba'] * 512}"
                listing = run("mdir", "-i", at, "-/", "-b", "::/", env=env, check=True).stdout
                for p, data in files.items():
                    self.assertIn(p.upper(), listing.upper(), p)
                    out = d / "copy.bin"
                    if out.exists():
                        out.unlink()
                    run("mcopy", "-n", "-i", at, f"::{p}", out, env=env, check=True)
                    self.assertEqual(out.read_bytes(), data, f"{name}: mtools read {p} differently")
                    with mkcard.Image(str(img)) as im:
                        self.assertEqual(im.read_file(p), data, f"{name}: mkcard's reader on {p}")
                m = re.search(r"(\d+)/(\d+) clusters", r.stdout)
                self.assertIsNotNone(m, r.stdout)
                if extra.get("fill"):
                    self.assertEqual(m.group(1), m.group(2), f"the full volume has free clusters: {m.group(0)}")
                img.unlink()
                part.unlink()

    def test_refusals(self):
        d = fresh("mkcard_refuse")
        r = tool("mkcard.py", d / "small.img", "--size", "16M", "--cluster", "512")
        self.assertEqual(r.returncode, 1)
        self.assertIn("at least 65525", r.stderr)
        r = tool("mkcard.py", "/dev/null")
        self.assertEqual(r.returncode, 1)
        r = tool("mkcard.py", d / "ok.img", "--standard-dirs", "--file", f"/BOOT/SURFOS.BIN={FW / 'HELLO.BIN'}")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("SURFOS.BIN", tool("mkcard.py", "--inspect", d / "ok.img").stdout)
        (d / "ok.img").unlink()


ASM_CASES = {
    "compressed": ("rv32imc", "c.addi a0, 1\n", "16-bit (compressed)"),
    "atomic": ("rv32ima", "amoadd.w a0, a1, (a2)\n", "atomic"),
    "float": ("rv32imf", "fadd.s fa0, fa1, fa2\n", "floating point"),
    "sret": ("rv32im_zicsr", "sret\n", "SYSTEM instruction SurfCPU lacks"),
    "satp": ("rv32im_zicsr", "csrr a0, satp\n", "CSR 0x180 is not implemented"),
    "rowrite": ("rv32im_zicsr", "csrw mhartid, a0\n", "write to read-only CSR mhartid"),
    "shamt": ("rv32im", ".insn i 0x13, 1, a0, a0, 32\n", "SLLI with imm[11:5]"),
    "good": ("rv32im_zicsr_zifencei", "mul a0, a1, a2\n div a0, a0, a1\n fence.i\n wfi\n mret\n ecall\n ebreak\n"
             "csrr a0, mcycle\n csrw mtvec, a0\n .word 0\n nop\n", None),
}


class TestIsaAudit(unittest.TestCase):
    def test_firmware_passes(self):
        for name in ("surfboot", "bramdiag", "ddrdiag", "hello"):
            r = tool("isa-audit.py", FW / f"{name}.elf")
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("objdump agrees", r.stdout)

    def test_catches_each_violation(self):
        d = fresh("isa")
        for name, (march, body, problem) in ASM_CASES.items():
            with self.subTest(name):
                src = d / f"{name}.S"
                src.write_text(f".globl _start\n_start:\n {body}\n j _start\n")
                elf = d / f"{name}.elf"
                r = run("riscv64-unknown-elf-gcc", f"-march={march}", "-mabi=ilp32", "-nostdlib", "-nostartfiles",
                        "-Wl,-Ttext=0x80000000", "-Wl,--no-warn-rwx-segments", src, "-o", elf)
                self.assertEqual(r.returncode, 0, r.stderr)
                a = tool("isa-audit.py", elf)
                if problem is None:
                    self.assertEqual(a.returncode, 0, a.stdout)
                else:
                    self.assertEqual(a.returncode, 1, a.stdout)
                    self.assertIn(problem, a.stdout)
        a = tool("isa-audit.py", d / "compressed.elf")
        self.assertIn("EF_RISCV_RVC", a.stdout)


class TestDeploySd(unittest.TestCase):
    def setUp(self):
        self.dir = fresh(f"deploy_{self._testMethodName}")
        self.card = self.dir / "card"
        self.card.mkdir()
        self.cfg = self.dir / "cfg.json"
        self.conf()
        self.env = dict(os.environ, SURFCPU_DEPLOY_TEST="1", FAKE_DEPLOY_CONFIG=str(self.cfg),
                        SURFCPU_DISKUTIL=f"{PY} {ROOT / 'firmware/tests/deploy/fake_diskutil.py'}",
                        SURFCPU_MOUNT=f"{PY} {ROOT / 'firmware/tests/deploy/fake_mount.py'}")
        self.pkg_a = FW / "HELLO.BIN"
        self.pkg_b = FW / "DDRDIAG.BIN"

    def conf(self, volume=None, whole=None, **kw):
        vol = {"FilesystemType": "msdos", "FilesystemName": "MS-DOS FAT32", "FilesystemUserVisibleName":
               "MS-DOS (FAT32)", "RemovableMedia": True, "Removable": True, "Ejectable": True, "Internal": True,
               "BusProtocol": "Secure Digital", "WritableVolume": True, "VolumeName": "SURFOS",
               "MediaName": "SDXC Reader", "TotalSize": 31914983424, "MountPoint": str(self.card)}
        vol.update(volume or {})
        wh = {"Content": "FDisk_partition_scheme", "Internal": True, "RemovableMedia": True}
        wh.update(whole or {})
        cfg = {"mount": str(self.card), "device": "/dev/disk9s1", "volume": vol, "whole": wh}
        cfg.update(kw)
        self.cfg.write_text(json.dumps(cfg))

    def deploy(self, *args, env=None):
        return tool("deploy-sd.py", "--mount", self.card, *args, env=env or self.env)

    def snapshot(self):
        return sorted((str(p.relative_to(self.card)), p.read_bytes() if p.is_file() else None)
                      for p in self.card.rglob("*"))

    def assertRefused(self, r, words):
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn(words, r.stderr)

    def test_refusals(self):
        self.assertRefused(tool("deploy-sd.py", env=self.env), "give the card's mount point")
        no_test = {k: v for k, v in self.env.items() if k != "SURFCPU_DEPLOY_TEST"}
        self.assertRefused(self.deploy("--image", self.pkg_a, env=no_test), "runs on the Mac")
        self.assertRefused(tool("deploy-sd.py", "--mount", "/Volumes/SURFOS", env=self.env), "never touches /Volumes")
        self.conf(mounted=False)
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "not a mount point")
        self.conf(fstype="exfat")
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "exfat volume")
        self.conf(volume={"FilesystemName": "MS-DOS FAT16", "FilesystemUserVisibleName": "MS-DOS (FAT16)"})
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "SurfBoot needs FAT32")
        self.conf(volume={"RemovableMedia": False, "Removable": False, "Ejectable": False, "Internal": True})
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "fixed internal disk")
        self.conf(whole={"Content": "GUID_partition_scheme"})
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "GUID_partition_scheme")
        self.conf(volume={"WritableVolume": False})
        self.assertRefused(self.deploy("--image", self.pkg_a, "--new-card"), "read-only")
        self.conf()
        self.assertRefused(self.deploy("--image", self.pkg_a), "has no /SYSTEM/SURFCARD.TXT")
        x86 = self.dir / "surfos.bin"
        x86.write_bytes(b"\xeb\x63\x90" + bytes(2000))
        self.assertRefused(self.deploy("--image", x86, "--new-card"), "IMG_MAGIC")
        self.assertRefused(self.deploy("--image", self.dir / "missing.bin", "--new-card"), "no package")
        self.assertEqual(self.snapshot(), [], "a refusal wrote something")

    def test_dry_run_writes_nothing(self):
        r = self.deploy("--image", self.pkg_a, "--new-card", "--cmdline", "console=uart", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("dry run: nothing was written", r.stdout)
        self.assertIn("write /BOOT/SURFOS.BIN", r.stdout)
        self.assertEqual(self.snapshot(), [])

    def test_install_update_backup_and_user_files(self):
        r = self.deploy("--image", self.pkg_a, "--new-card", "--cmdline", "console=uart")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        for d in mkcard.STANDARD_DIRS:
            self.assertTrue((self.card / d.strip("/")).is_dir(), d)
        self.assertEqual((self.card / "BOOT/SURFOS.BIN").read_bytes(), self.pkg_a.read_bytes())
        self.assertEqual((self.card / "BOOT/CMDLINE.TXT").read_bytes(), b"console=uart\n")
        self.assertIn("id=", (self.card / "SYSTEM/SURFCARD.TXT").read_text())
        self.assertFalse((self.card / "BOOT/BACKUP.BIN").exists())
        self.assertIn("Eject the card", r.stdout)
        self.assertIn("verified 3 file(s)", r.stdout)
        marker = (self.card / "SYSTEM/SURFCARD.TXT").read_bytes()
        (self.card / "USERS/DOCUMENTS/letter.txt").write_text("mine")
        (self.card / "photo.jpg").write_bytes(b"\xff\xd8" * 100)
        r = self.deploy("--image", self.pkg_b)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((self.card / "BOOT/SURFOS.BIN").read_bytes(), self.pkg_b.read_bytes())
        self.assertEqual((self.card / "BOOT/BACKUP.BIN").read_bytes(), self.pkg_a.read_bytes())
        self.assertEqual((self.card / "USERS/DOCUMENTS/letter.txt").read_text(), "mine")
        self.assertEqual((self.card / "photo.jpg").read_bytes(), b"\xff\xd8" * 100)
        self.assertEqual((self.card / "SYSTEM/SURFCARD.TXT").read_bytes(), marker, "the marker changed")
        self.assertEqual((self.card / "BOOT/CMDLINE.TXT").read_bytes(), b"console=uart\n", "CMDLINE.TXT was lost")
        self.assertFalse((self.card / "BOOT/SURFOS.NEW").exists())
        r = self.deploy("--image", self.pkg_b)              # the same package again: backup untouched
        self.assertEqual(r.returncode, 0)
        self.assertIn("already has this SURFOS.BIN", r.stdout)
        self.assertEqual((self.card / "BOOT/BACKUP.BIN").read_bytes(), self.pkg_a.read_bytes())
        r = self.deploy("--image", self.pkg_a, "--no-backup")
        self.assertEqual(r.returncode, 0)
        self.assertEqual((self.card / "BOOT/BACKUP.BIN").read_bytes(), self.pkg_a.read_bytes())
        self.assertEqual((self.card / "BOOT/SURFOS.BIN").read_bytes(), self.pkg_a.read_bytes())
        for p in self.card.rglob("~DEPLOY*"):
            self.fail(f"temporary file left behind: {p}")

    def test_corrupted_copy_is_caught(self):
        self.assertEqual(self.deploy("--image", self.pkg_a, "--new-card").returncode, 0)
        env = dict(self.env, SURFCPU_DEPLOY_TEST_CORRUPT="BOOT/SURFOS.BIN")
        r = self.deploy("--image", self.pkg_b, env=env)
        self.assertRefused(r, "read back differ")
        self.assertEqual((self.card / "BOOT/SURFOS.BIN").read_bytes(), self.pkg_a.read_bytes(), "the old package changed")
        self.assertFalse((self.card / "BOOT/SURFOS.NEW").exists())
        self.assertFalse((self.card / "BOOT/BACKUP.BIN").exists())
        env = dict(self.env, SURFCPU_DEPLOY_TEST_CORRUPT="BOOT/CMDLINE.TXT")
        r = self.deploy("--image", self.pkg_a, "--cmdline", "x", env=env)
        self.assertRefused(r, "/BOOT/CMDLINE.TXT")

    def test_system_files_and_explicit_backup(self):
        sysdir = self.dir / "system"
        sysdir.mkdir()
        (sysdir / "MOTD.TXT").write_text("Welcome to SurfOS\n")
        (sysdir / "KEYMAP.CFG").write_bytes(b"us\n")
        (sysdir / ".DS_Store").write_bytes(b"junk")
        r = self.deploy("--image", self.pkg_a, "--new-card", "--system-dir", sysdir, "--backup-image", self.pkg_b)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((self.card / "SYSTEM/MOTD.TXT").read_text(), "Welcome to SurfOS\n")
        self.assertEqual((self.card / "SYSTEM/KEYMAP.CFG").read_bytes(), b"us\n")
        self.assertFalse((self.card / "SYSTEM/.DS_Store").exists())
        self.assertEqual((self.card / "BOOT/BACKUP.BIN").read_bytes(), self.pkg_b.read_bytes())
        bad = self.dir / "bad.bin"
        bad.write_bytes(self.pkg_b.read_bytes()[:-3])
        self.assertRefused(self.deploy("--image", self.pkg_a, "--backup-image", bad), "IMG_SIZE")


if __name__ == "__main__":
    WORK.mkdir(parents=True, exist_ok=True)
    unittest.main(verbosity=2 if "-v" in sys.argv else 1)
