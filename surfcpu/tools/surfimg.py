#!/usr/bin/env python3
"""SurfCPU kernel package tool (SURFOS.BIN)
--------------------
File: surfimg.py Date: 10/15/26 (SurfCPU)
--------------------
Packs and checks SURFOS.BIN, the boot package of docs/BOOT-ABI.md: a 64-byte little-endian
header (magic "SIMG", version, ISA bits, platform, load address, payload size, mem_size,
entry, payload CRC-32, image version, build ID, name, header CRC-32) followed by the payload.

    tools/surfimg.py pack --elf surfos.elf -o SURFOS.BIN [--payload-out surfos.payload.bin]
                          [--name SurfOS] [--version 0.1.0] [--build-id 1a2b3c4d] [--no-audit]
    tools/surfimg.py pack --bin payload.bin --load-addr 0x80000000 [--entry ADDR] [--mem-size N] -o X.BIN
    tools/surfimg.py info X.BIN [--ram-size 256M]    every field, then SurfBoot's checks one by one
    tools/surfimg.py verify X.BIN [--ram-size 256M]  the checks only; exit 0 when SurfBoot would accept it
    tools/surfimg.py crc FILE...                     CRC-32/ISO-HDLC of each file
    tools/surfimg.py selftest                        the CRC test vectors and a pack/verify round trip

From an ELF the payload is every PT_LOAD segment from its first loadable byte to the end of
its initialized data (gaps zero-filled), load_addr is the lowest physical address, mem_size
reaches the end of the last segment in memory (BSS and stack included) and entry is e_entry;
the ELF must be 32-bit little-endian RISC-V, its entry point in a segment that runs where it
is loaded (a segment the program copies elsewhere itself is noted), and it is put through
tools/isa-audit.py unless --no-audit. The checks are SurfBoot's (image.c), in its
order and with its error names, so a package that passes `verify` passes on the board (with
the same usable DDR). The x86 surfos.bin, a GRUB disk image or an ELF are refused: they have
no SIMG header. A CRC detects accidental damage; it is not authentication.

Constants come from spec/soc.toml through tools/socspec.py (the built-in version 1 values
are used, and checked against the spec by selftest, when it cannot be loaded). Only the
Python standard library is used: this runs on the Mac (deploy-sd.py) and in the container.
"""
from __future__ import annotations

import argparse
import importlib.util
import os
import re
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Boot ABI version 1 (docs/BOOT-ABI.md); refreshed from the spec below when possible.
ABI = {
    "image_magic": 0x474D4953, "image_header_version": 1, "image_header_size": 64,
    "platform_abi": 1, "isa_i": 1, "isa_m": 2, "isa_zicsr": 4, "isa_zifencei": 8,
}
DDR_BASE, DDR_SIZE, DDR_TOP_BASE, DDR_TOP_SIZE = 0x80000000, 0x10000000, 0x8F000000, 0x01000000
BUILTIN = (dict(ABI), DDR_BASE, DDR_SIZE, DDR_TOP_BASE, DDR_TOP_SIZE)
SPEC_LOADED = False
try:
    sys.path.insert(0, str(HERE))
    from socspec import load as _load_spec       # noqa: E402
    _spec = _load_spec()
    for _k in ABI:
        ABI[_k] = int(_spec.bootabi[_k])
    DDR_BASE, DDR_SIZE = _spec.regions["ddr"].base, _spec.regions["ddr"].size
    DDR_TOP_BASE, DDR_TOP_SIZE = _spec.reserves["ddr_top"].base, _spec.reserves["ddr_top"].size
    SPEC_LOADED = True
except Exception:                                 # pragma: no cover - fallback only
    pass
finally:
    if sys.path and sys.path[0] == str(HERE):
        sys.path.pop(0)

ISA_ALL = ABI["isa_i"] | ABI["isa_m"] | ABI["isa_zicsr"] | ABI["isa_zifencei"]
ISA_NAMES = {"i": ABI["isa_i"], "m": ABI["isa_m"], "zicsr": ABI["isa_zicsr"], "zifencei": ABI["isa_zifencei"]}
HEADER = struct.Struct("<IHHIIIIIIIIII12sI")      # 64 bytes; the last field is header_crc32
assert HEADER.size == 64
FIELDS = ("magic", "version", "header_size", "isa", "platform", "load_addr", "payload_size", "mem_size",
          "entry", "payload_crc32", "flags", "image_version", "build_id", "name", "header_crc32")
PT_LOAD = 1
EM_RISCV = 243


class PackError(Exception):
    pass


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def crc32_bitwise(data: bytes) -> int:
    """An independent CRC-32/ISO-HDLC, used by selftest to check zlib against the vectors."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


CRC_VECTORS = [(b"", 0x00000000), (b"a", 0xE8B7BE43), (b"123456789", 0xCBF43926),
               (bytes(range(256)), 0x29058C73)]


def parse_version(text: str) -> int:
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", text)
    if not m:
        raise PackError(f"version {text!r} is not MAJOR.MINOR.PATCH")
    major, minor, patch = (int(x) for x in m.groups())
    if major > 0xFFFF or minor > 0xFF or patch > 0xFF:
        raise PackError(f"version {text!r}: major <= 65535, minor and patch <= 255")
    return major << 16 | minor << 8 | patch


def parse_build_id(text: str | None) -> int:
    if not text:
        text = os.environ.get("SURFCPU_BUILD_ID", "")
    m = re.match(r"[0-9a-fA-F]{1,8}", text or "")
    return int(m.group(0), 16) if m else 0


def parse_isa(text: str) -> int:
    try:
        return int(text, 0)
    except ValueError:
        pass
    bits = 0
    for part in re.split(r"[,_ ]+", text.lower()):
        if not part:
            continue
        if part not in ISA_NAMES:
            raise PackError(f"unknown ISA part {part!r} (i, m, zicsr, zifencei)")
        bits |= ISA_NAMES[part]
    return bits


def parse_size(text: str) -> int:
    m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)([KkMmGg]?)", text.strip())
    if not m:
        raise argparse.ArgumentTypeError(f"bad size {text!r}")
    return int(m.group(1), 0) * {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}[m.group(2).lower()]


def make_header(*, load_addr: int, payload: bytes, mem_size: int, entry: int, isa: int = ISA_ALL,
                platform: int | None = None, flags: int = 0, image_version: int = 0, build_id: int = 0,
                name: str | bytes = "SurfOS", magic: int | None = None, version: int | None = None,
                header_size: int | None = None, payload_size: int | None = None,
                payload_crc32: int | None = None, header_crc32: int | None = None) -> bytes:
    """A header for payload. The keyword overrides exist so tests can build broken ones."""
    raw_name = name.encode("ascii") if isinstance(name, str) else bytes(name)
    if len(raw_name) > 12:
        raise PackError(f"name {raw_name!r} is longer than 12 bytes")
    h = HEADER.pack(
        ABI["image_magic"] if magic is None else magic,
        ABI["image_header_version"] if version is None else version,
        ABI["image_header_size"] if header_size is None else header_size,
        isa, ABI["platform_abi"] if platform is None else platform,
        load_addr, len(payload) if payload_size is None else payload_size, mem_size, entry,
        crc32(payload) if payload_crc32 is None else payload_crc32,
        flags, image_version, build_id, raw_name.ljust(12, b"\0"), 0)
    hcrc = crc32(h[:60]) if header_crc32 is None else header_crc32
    return h[:60] + struct.pack("<I", hcrc)


def make_package(payload: bytes, load_addr: int, entry: int | None = None, mem_size: int | None = None,
                 **kw) -> bytes:
    entry = load_addr if entry is None else entry
    mem_size = len(payload) if mem_size is None else mem_size
    return make_header(load_addr=load_addr, payload=payload, mem_size=mem_size, entry=entry, **kw) + payload


def parse_header(data: bytes) -> dict:
    return dict(zip(FIELDS, HEADER.unpack_from(data, 0)))


def usable_end(ram_base: int, ram_size: int) -> int:
    """End of the DDR images may use: below ddr_top, the top 16 MiB of the DDR that calibrated
    (0x8F000000 for the 256 MiB part; BOOT-ABI.md section 2, check 4)."""
    return ram_base + max(ram_size - DDR_TOP_SIZE, 0)


def check(data: bytes, ram_size: int = DDR_SIZE, ram_base: int = DDR_BASE) -> list[tuple[str, str | None, str]]:
    """SurfBoot's checks (firmware/surfboot/image.c), in order. Returns a list of
    (check, error name or None, sentence); it stops at the first failure, as SurfBoot does."""
    out = []
    ram_limit = usable_end(ram_base, ram_size)
    size = len(data)

    def fail(step, code, msg):
        out.append((step, code, msg))
        return out

    step = "1 size, magic, version, header CRC"
    if size < ABI["image_header_size"]:
        return fail(step, "IMG_SHORT", f"the file is {size} bytes, shorter than the 64-byte header")
    h = parse_header(data)
    if h["magic"] != ABI["image_magic"]:
        hint = ""
        if data[:4] == b"\x7fELF":
            hint = ": an ELF file (install the packed SURFOS.BIN, not surfos.elf or the x86 kernel)"
        elif data[:1] in (b"\xeb", b"\xfa", b"\x33"):
            hint = ": it looks like x86 boot code (the x86 surfos.bin or a disk image is not a RISC-V kernel)"
        return fail(step, "IMG_MAGIC", f"no SIMG magic: found {data[:4].hex(' ')}{hint}")
    if h["version"] != ABI["image_header_version"] or h["header_size"] != ABI["image_header_size"]:
        return fail(step, "IMG_VERSION", f"header version {h['version']}, size {h['header_size']}: SurfBoot reads "
                    f"version {ABI['image_header_version']}, size {ABI['image_header_size']}")
    hcrc = crc32(data[:60])
    if hcrc != h["header_crc32"]:
        return fail(step, "IMG_HCRC", f"the header CRC is {hcrc:#010x} but the header says {h['header_crc32']:#010x}")
    out.append((step, None, "ok"))
    step = "2 ISA and platform"
    if not h["isa"] & ABI["isa_i"]:
        return fail(step, "IMG_ISA", f"the ISA bits {h['isa']:#010x} do not include I (SURF_ISA_I): every SurfCPU "
                    "program needs the base ISA")
    if h["isa"] & ~ISA_ALL:
        return fail(step, "IMG_ISA", f"the payload needs ISA bits {h['isa'] & ~ISA_ALL:#010x} that SurfCPU lacks")
    if h["platform"] != ABI["platform_abi"]:
        return fail(step, "IMG_PLATFORM", f"platform ABI {h['platform']} is not this board's ({ABI['platform_abi']})")
    out.append((step, None, "ok"))
    step = "3 file size = 64 + payload_size"
    if h["payload_size"] == 0:
        return fail(step, "IMG_SIZE", "the payload size is 0")
    if size != 64 + h["payload_size"]:
        what = "truncated" if size < 64 + h["payload_size"] else "extra bytes"
        return fail(step, "IMG_SIZE", f"the file is {size} bytes but the header says 64 + {h['payload_size']}: {what}")
    out.append((step, None, "ok"))
    step = "4 load range inside usable DDR"
    if h["load_addr"] & 3:
        return fail(step, "IMG_ALIGN", f"the load address {h['load_addr']:#010x} is not 4-byte aligned")
    if h["mem_size"] < h["payload_size"]:
        return fail(step, "IMG_RANGE", f"mem_size {h['mem_size']} is smaller than payload_size {h['payload_size']}")
    end = h["load_addr"] + h["mem_size"]
    if h["load_addr"] < ram_base or end > ram_limit:
        return fail(step, "IMG_RANGE", f"the image would occupy [{h['load_addr']:#010x}, {end:#010x}), outside usable "
                    f"DDR [{ram_base:#010x}, {ram_limit:#010x}) (the top 16 MiB of DDR, ddr_top, is reserved)")
    out.append((step, None, "ok"))
    step = "5 entry inside the payload"
    if h["entry"] & 3:
        return fail(step, "IMG_ENTRY", f"the entry point {h['entry']:#010x} is not 4-byte aligned")
    if not (h["load_addr"] <= h["entry"] < h["load_addr"] + h["payload_size"]):
        return fail(step, "IMG_ENTRY", f"the entry point {h['entry']:#010x} is outside the payload "
                    f"[{h['load_addr']:#010x}, {h['load_addr'] + h['payload_size']:#010x})")
    out.append((step, None, "ok"))
    step = "6 payload CRC"
    pcrc = crc32(data[64:])
    if pcrc != h["payload_crc32"]:
        return fail(step, "IMG_CRC", f"the payload CRC is {pcrc:#010x}, the header says {h['payload_crc32']:#010x}")
    out.append((step, None, "ok"))
    return out


def verdict(results) -> tuple[str | None, str]:
    for step, code, msg in results:
        if code:
            return code, msg
    return None, "valid"


# ---- ELF input ----

RELOCATED_NOTES: list[str] = []


def elf_payload(path: str) -> tuple[bytes, int, int, int]:
    """(payload, load_addr, mem_size, entry) from a linked RV32 ELF. The payload is laid out
    by load (physical) address. A segment whose run address differs (code the program copies
    elsewhere itself, such as firmware's .fast) is allowed and noted; the entry point must lie
    in a segment that runs where it is loaded."""
    RELOCATED_NOTES.clear()
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF":
        raise PackError(f"{path}: not an ELF file")
    if d[4] != 1 or d[5] != 1:
        raise PackError(f"{path}: not a 32-bit little-endian ELF")
    e_machine, = struct.unpack_from("<H", d, 18)
    if e_machine != EM_RISCV:
        raise PackError(f"{path}: machine {e_machine}, not RISC-V (the x86 kernel is not a SurfCPU kernel)")
    e_entry, e_phoff = struct.unpack_from("<II", d, 24)
    e_flags, = struct.unpack_from("<I", d, 36)
    e_phentsize, e_phnum = struct.unpack_from("<HH", d, 42)
    if e_flags & 0x1:
        raise PackError(f"{path}: built for compressed instructions (EF_RISCV_RVC); SurfCPU has no C extension")
    if e_flags & 0x6:
        raise PackError(f"{path}: hard-float ABI; SurfCPU uses ilp32 (soft float)")
    loads = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
            "<8I", d, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD and p_memsz:
            if p_vaddr != p_paddr:
                if p_vaddr <= e_entry < p_vaddr + p_memsz:
                    raise PackError(f"{path}: the entry point is in a segment that runs at {p_vaddr:#x} but loads at "
                                    f"{p_paddr:#x}; SurfBoot jumps to the load address")
                RELOCATED_NOTES.append(f"{p_filesz} bytes loaded at {p_paddr:#010x} run at {p_vaddr:#010x} "
                                       "(the program copies them there)")
            loads.append((p_paddr, p_offset, p_filesz, p_memsz))
    if not loads:
        raise PackError(f"{path}: no loadable segments")
    loads.sort()
    base = loads[0][0]
    file_end = max((p + f for p, o, f, m in loads if f), default=base)
    mem_end = max(p + m for p, o, f, m in loads)
    for (a, _, _, am), (b, _, _, _) in zip(loads, loads[1:]):
        if a + am > b:
            raise PackError(f"{path}: loadable segments overlap at {b:#x}")
    payload = bytearray(file_end - base)
    for p, o, f, m in loads:
        if f:
            payload[p - base:p - base + f] = d[o:o + f]
    return bytes(payload), base, mem_end - base, e_entry


def run_audit(path: str) -> None:
    spec = importlib.util.spec_from_file_location("isa_audit", HERE / "isa-audit.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    if mod.main([path, "--quiet"]) != 0:
        raise PackError(f"{path}: the ISA audit failed (tools/isa-audit.py {path})")


# ---- commands ----

def cmd_pack(a) -> int:
    try:
        if a.elf:
            payload, load_addr, mem_size, entry = elf_payload(a.elf)
            if not a.no_audit:
                run_audit(a.elf)
            if a.load_addr is not None or a.entry is not None:
                raise PackError("--load-addr and --entry come from the ELF; they are only for --bin")
            if a.mem_size is not None:
                mem_size = max(mem_size, a.mem_size)
        else:
            payload = open(a.bin, "rb").read()
            if a.load_addr is None:
                raise PackError("--bin needs --load-addr")
            load_addr = a.load_addr
            entry = load_addr if a.entry is None else a.entry
            mem_size = len(payload) if a.mem_size is None else a.mem_size
        pkg = make_package(payload, load_addr, entry, mem_size, isa=parse_isa(a.isa),
                           image_version=parse_version(a.version), build_id=parse_build_id(a.build_id),
                           name=a.name, flags=a.flags)
    except (OSError, PackError) as e:
        print(f"surfimg: {e}", file=sys.stderr)
        return 1
    code, msg = verdict(check(pkg, a.ram_size))
    if code:
        print(f"surfimg: refusing to write {a.output}: SurfBoot would refuse it: {code}: {msg}", file=sys.stderr)
        return 1
    if a.payload_out:
        tmp = a.payload_out + ".tmp"
        open(tmp, "wb").write(payload)
        os.replace(tmp, a.payload_out)
    tmp = a.output + ".tmp"
    open(tmp, "wb").write(pkg)
    os.replace(tmp, a.output)
    h = parse_header(pkg)
    for note in RELOCATED_NOTES if a.elf else ():
        print(f"surfimg: note: {note}")
    print(f"surfimg: {a.output}: {a.name} {a.version} build {h['build_id']:08x}, load {load_addr:#010x}, "
          f"entry {entry:#010x}, payload {len(payload)} bytes, mem_size {mem_size}, payload CRC {h['payload_crc32']:#010x}")
    return 0


def describe(path: str, data: bytes, ram_size: int) -> int:
    print(f"{path}: {len(data)} bytes")
    if len(data) >= 64:
        h = parse_header(data)
        isa = " ".join(n for n, b in ISA_NAMES.items() if h["isa"] & b) + (" +unknown" if h["isa"] & ~ISA_ALL else "")
        iv = h["image_version"]
        print(f"  magic        {h['magic']:#010x} {data[:4]!r}")
        print(f"  version      {h['version']}, header size {h['header_size']}")
        print(f"  isa          {h['isa']:#010x} ({isa})")
        print(f"  platform     {h['platform']}")
        print(f"  load_addr    {h['load_addr']:#010x}")
        print(f"  payload_size {h['payload_size']}")
        print(f"  mem_size     {h['mem_size']}")
        print(f"  entry        {h['entry']:#010x}")
        print(f"  payload_crc  {h['payload_crc32']:#010x}")
        print(f"  flags        {h['flags']:#x}")
        print(f"  image        {h['name'].rstrip(bytes(1)).decode('ascii', 'replace')} {iv >> 16}.{(iv >> 8) & 255}."
              f"{iv & 255} build {h['build_id']:08x}")
        print(f"  header_crc   {h['header_crc32']:#010x}")
    ram_limit = usable_end(DDR_BASE, ram_size)
    print(f"checks (as SurfBoot, usable DDR [{DDR_BASE:#010x}, {ram_limit:#010x})):")
    results = check(data, ram_size)
    for step, code, msg in results:
        print(f"  {step:<34} {'ok' if not code else code + ': ' + msg}")
    code, msg = verdict(results)
    print("valid" if not code else f"INVALID: {code}")
    return 0 if not code else 1


def cmd_info(a) -> int:
    rc = 0
    for path in a.files:
        rc |= describe(path, open(path, "rb").read(), a.ram_size)
    return rc


def cmd_verify(a) -> int:
    rc = 0
    for path in a.files:
        code, msg = verdict(check(open(path, "rb").read(), a.ram_size))
        print(f"{path}: {'valid' if not code else code + ': ' + msg}")
        rc |= 1 if code else 0
    return rc


def cmd_crc(a) -> int:
    for path in a.files:
        data = open(path, "rb").read()
        print(f"{crc32(data):08x}  {len(data):10d}  {path}")
    return 0


def cmd_selftest(a) -> int:
    failures = 0
    for data, want in CRC_VECTORS:
        for name, fn in (("zlib", crc32), ("bitwise", crc32_bitwise)):
            got = fn(data)
            ok = got == want
            failures += not ok
            label = repr(data) if len(data) < 12 else f"{len(data)} bytes 00..ff"
            print(f"  crc32[{name}] {label:<14} {got:#010x} {'ok' if ok else f'FAIL (want {want:#010x})'}")
    payload = bytes((i * 7 + 3) & 0xFF for i in range(1000))
    pkg = make_package(payload, DDR_BASE, DDR_BASE + 8, 4096, name="selftest", image_version=parse_version("1.2.3"))
    code, _ = verdict(check(pkg))
    failures += code is not None
    print(f"  pack/verify round trip: {'ok' if code is None else 'FAIL ' + code}")
    bad = bytearray(pkg)
    bad[100] ^= 1
    code, _ = verdict(check(bytes(bad)))
    failures += code != "IMG_CRC"
    print(f"  a flipped payload bit is caught: {'ok' if code == 'IMG_CRC' else 'FAIL ' + str(code)}")
    if SPEC_LOADED:
        same = (BUILTIN[0] == ABI and BUILTIN[1:] == (DDR_BASE, DDR_SIZE, DDR_TOP_BASE, DDR_TOP_SIZE))
        failures += not same
        print(f"  built-in constants match spec/soc.toml: {'ok' if same else 'FAIL'}")
    else:
        print("  spec/soc.toml not loaded: built-in version 1 constants in use")
    print("surfimg selftest:", "PASS" if not failures else f"FAIL ({failures})")
    return 0 if not failures else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="pack and check SURFOS.BIN (docs/BOOT-ABI.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack", help="make a package from an ELF or a raw payload")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--elf")
    src.add_argument("--bin")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--payload-out", help="also write the raw payload (surfos.payload.bin)")
    p.add_argument("--load-addr", type=lambda s: int(s, 0))
    p.add_argument("--entry", type=lambda s: int(s, 0))
    p.add_argument("--mem-size", type=parse_size)
    p.add_argument("--name", default="SurfOS")
    p.add_argument("--version", default="0.0.0", help="MAJOR.MINOR.PATCH of the kernel")
    p.add_argument("--build-id", help="source commit (first 8 hex digits used); default $SURFCPU_BUILD_ID")
    p.add_argument("--isa", default="i,m,zicsr,zifencei")
    p.add_argument("--flags", type=lambda s: int(s, 0), default=0, help=argparse.SUPPRESS)
    p.add_argument("--no-audit", action="store_true", help="skip tools/isa-audit.py on the ELF")
    p.add_argument("--ram-size", type=parse_size, default=DDR_SIZE)
    p.set_defaults(fn=cmd_pack)
    for name, fn, hlp in (("info", cmd_info, "show the header and every check"),
                          ("verify", cmd_verify, "check packages as SurfBoot does")):
        q = sub.add_parser(name, help=hlp)
        q.add_argument("files", nargs="+")
        q.add_argument("--ram-size", type=parse_size, default=DDR_SIZE, help="DDR size (default 256M)")
        q.set_defaults(fn=fn)
    q = sub.add_parser("crc", help="CRC-32 of files")
    q.add_argument("files", nargs="+")
    q.set_defaults(fn=cmd_crc)
    q = sub.add_parser("selftest", help="CRC test vectors and a round trip")
    q.set_defaults(fn=cmd_selftest)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
