#!/usr/bin/env python3
"""SurfCPU release: build the reproducible standalone release, and check that it reproduces
--------------------
File: release.py Date: 10/16/26 (SurfCPU)
--------------------
Milestone 9 (docs/RELEASE.md). Runs on the Mac with its own python3 (3.9) and drives the builds in
the Docker images through devenv/run.sh and tools/fpga-build.sh, as a person would:

    tools/release.py make [--prerelease] [--skip-ddr] [--designs D,..] [--boards B,..] [--out DIR] [--force]
        make -C surfcpu release [PRERELEASE=1] [SKIP_DDR=1] [DESIGNS=..] [BOARDS=..] [FORCE=1]
        From a clean tree only, with SOURCE_DATE_EPOCH = the commit's time, LC_ALL=C and TZ=UTC: the
        boot ROMs and the kernel (make build-rom build-kernel, from empty output directories), every
        design of release/release.txt for every board (tools/fpga-build.sh, FLOW=openxc7, every gate
        as it is), the QSPI flash images (tools/bitfile.py), the card's /BOOT files as tools/deploy-sd.py
        installs them, then SHA256SUMS and MANIFEST, in build/release/<version>-<commit>/. A DDR design
        that does not build makes a pre-release, named <version>-pre-<commit> and only with
        --prerelease; anything else that does not build stops the release.
    tools/release.py verify RELEASE [--designs D,..] [--boards B,..] [--work DIR] [--keep] [--report FILE]
        make -C surfcpu release-verify RELEASE=build/release/<name> [DESIGNS=..] [BOARDS=..] [KEEP=1]
        Clones the release's commit into a new directory (a pristine copy: git clone --shared), runs
        the release there with the same settings, and compares every artifact of the release's
        SHA256SUMS byte for byte, with the first differing offset and the likely cause of each
        difference. --designs/--boards rebuild only some of the bitstreams (the others are listed
        as not rebuilt); the software artifacts are always rebuilt.
    tools/release.py compare REFERENCE NEW [--report FILE]
        The same comparison of two release directories (or a SHA256SUMS file and a release directory).
    tools/release.py check RELEASE
        The release's files against its SHA256SUMS and MANIFEST (what shasum -a 256 -c does, and more).

Exit status: make 0 written, 1 refused or failed; verify and compare 0 when every compared artifact
matched, 1 when one differed or is missing, 2 when the comparison could not be made. Stand-ins for
the tests (tools/release-test.py): SURFCPU_RELEASE_RUN (devenv/run.sh), SURFCPU_RELEASE_FPGA
(tools/fpga-build.sh), SURFCPU_RELEASE_DOCKER (docker).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent              # surfcpu/tools
ROOT = HERE.parent                                  # surfcpu/
REPO = ROOT.parent                                  # the repository
LIST = ROOT / "release" / "release.txt"
ME = "release"
CONTRACT_DOCS = ("SURFBUS.md", "ISA.md", "BOOT-ABI.md", "MODULES.md", "MEMORY-MAP.md")
GATES = ("check_synthesis", "check_bitstream", "check_round_trip", "check_block_ram", "check_lvs",
         "check_clocks", "check_netlist", "check_boot_rom")
# the build record keys that are inputs of a bitstream (compared when a rebuilt one differs)
BIT_INPUTS = ("source_revision", "source_dirty", "build_id", "build_time", "placer_seed", "sources_sha256",
              "defines", "generated_files_match", "constraints_sha256", "rom_sha256", "part")
FLOW_INPUTS = ("image", "yosys", "nextpnr", "dsp_mode")
PKG_FIELDS = ((0, 4, "magic"), (4, 2, "version"), (6, 2, "header_size"), (8, 4, "isa"), (12, 4, "platform"),
              (16, 4, "load_addr"), (20, 4, "payload_size"), (24, 4, "mem_size"), (28, 4, "entry"),
              (32, 4, "payload_crc32"), (36, 4, "flags"), (40, 4, "image_version"), (44, 4, "build_id"),
              (48, 12, "name"), (60, 4, "header_crc32"))


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bitfile = load("bitfile")


class Refused(Exception):
    pass


def say(msg: str) -> None:
    print(f"{ME}: {msg}", flush=True)


# ============================================================================== small helpers
def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git(*args: str, cwd: Path = REPO) -> str:
    p = subprocess.run(["git", "-C", str(cwd)] + list(args), capture_output=True, text=True)
    if p.returncode != 0:
        raise Refused(f"git {' '.join(args)}: {p.stderr.strip()}")
    return p.stdout.rstrip()


def utc(epoch: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def minutes(seconds: float) -> str:
    return f"{seconds:.0f} s" if seconds < 120 else f"{seconds / 60:.1f} min"


def docker() -> str:
    d = os.environ.get("SURFCPU_RELEASE_DOCKER") or os.environ.get("DOCKER") or "docker"
    if shutil.which(d) or os.path.exists(d):
        return d
    alt = "/Applications/Docker.app/Contents/Resources/bin/docker"
    return alt if os.path.exists(alt) else d


def image_tag(script: Path, name: str) -> str:
    m = re.search(r"^IMAGE_TAG=(\S+)$", script.read_text(), re.M)
    if not m:
        raise Refused(f"no IMAGE_TAG in {script}")
    return f"{name}:{m.group(1)}"


def toolchain_image() -> str:
    return image_tag(ROOT / "devenv" / "run.sh", "surfcpu-toolchain")


def openxc7_image() -> str:
    return image_tag(ROOT / "tools" / "openxc7-build.sh", "surfcpu-openxc7")


def image_id(tag: str) -> str | None:
    p = subprocess.run([docker(), "image", "inspect", "--format", "{{.Id}}", tag], capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 and p.stdout.strip() else None


def image_versions(tag: str, path: str) -> list[str]:
    p = subprocess.run([docker(), "run", "--rm", tag, "cat", path], capture_output=True, text=True)
    if p.returncode != 0:
        raise Refused(f"cannot read {path} in {tag}: {p.stderr.strip()}")
    return [" ".join(line.split()) for line in p.stdout.splitlines() if line.strip()]


def flow_containers(tag: str) -> int:
    p = subprocess.run([docker(), "ps", "-q", "--filter", f"ancestor={tag}"], capture_output=True, text=True)
    return len([x for x in p.stdout.split() if x])


def nextpnr_threads() -> str:
    m = re.search(r"threads=\$\{OPENXC7_THREADS:-(\d+)\}", (ROOT / "tools" / "openxc7-build.sh").read_text())
    return m.group(1) if m else "?"


def run_logged(cmd: list[str], log: Path, env: dict, cwd: Path = ROOT, echo: bool = False) -> int:
    """run cmd with its output in log (and on the terminal with echo); the exit status"""
    log.parent.mkdir(parents=True, exist_ok=True)
    with open(log, "ab") as f:
        f.write(f"$ {' '.join(shlex.quote(c) for c in cmd)}\n".encode())
        f.flush()
        p = subprocess.Popen(cmd, cwd=str(cwd), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for line in iter(p.stdout.readline, b""):
            f.write(line)
            if echo:
                sys.stdout.write(line.decode(errors="replace"))
                sys.stdout.flush()
        return p.wait()


def tail(log: Path, n: int = 15) -> str:
    try:
        lines = log.read_text(errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join("    | " + x for x in lines[-n:])


# ============================================================================== TOML (the subset used here)
def _value(s: str, pos: int = 0):
    """one value at s[pos:]: (value, next position)"""
    while pos < len(s) and s[pos] in " \t\r\n,":
        pos += 1
    c = s[pos]
    if c == '"':
        out = []
        pos += 1
        while s[pos] != '"':
            if s[pos] == "\\":
                pos += 1
                out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"'}.get(s[pos], s[pos]))
            else:
                out.append(s[pos])
            pos += 1
        return "".join(out), pos + 1
    if c == "[":
        items = []
        pos += 1
        while True:
            while pos < len(s) and s[pos] in " \t\r\n,":
                pos += 1
            if s[pos] == "]":
                return items, pos + 1
            if s[pos] == "#":
                pos = s.index("\n", pos)
                continue
            v, pos = _value(s, pos)
            items.append(v)
    m = re.compile(r"true|false|[-+]?[0-9][0-9_]*(\.[0-9]+)?").match(s, pos)
    if not m:
        raise ValueError(f"cannot read a value at {s[pos:pos + 20]!r}")
    t = m.group(0)
    if t in ("true", "false"):
        return t == "true", m.end()
    return (float(t) if "." in t else int(t.replace("_", ""))), m.end()


def parse_toml(text: str) -> dict:
    """the TOML subset the manifests use: tables, arrays of tables, strings, numbers, booleans, arrays"""
    data: dict = {}
    table = data
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        s = lines[i].strip()
        i += 1
        if not s or s.startswith("#"):
            continue
        if s.startswith("[["):
            name = s[2:s.index("]]")].strip()
            data.setdefault(name, []).append({})
            table = data[name][-1]
            continue
        if s.startswith("["):
            table = data.setdefault(s[1:s.index("]")].strip(), {})
            continue
        key, _, rest = s.partition("=")
        key = key.strip().strip('"')
        rest = rest.strip()
        if rest.startswith("["):
            while _depth(rest) > 0 and i < len(lines):
                rest += "\n" + lines[i]
                i += 1
        table[key] = _value(rest)[0]
    return data


def _depth(s: str) -> int:
    depth, quoted, esc = 0, False, False
    for c in s:
        if quoted:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
        elif c == "#":
            break
    return depth


def toml_str(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return str(v)
    if isinstance(v, (list, tuple)):
        if not v:
            return "[]"
        return "[\n" + "".join(f"    {toml_str(x)},\n" for x in v) + "]"
    s = str(v).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t")
    return f'"{s}"'


def toml_key(k: str) -> str:
    return k if re.fullmatch(r"[A-Za-z0-9_-]+", k) else toml_str(k)


def toml_table(d: dict) -> str:
    return "".join(f"{toml_key(k)} = {toml_str(v)}\n" for k, v in d.items())


# ============================================================================== the release list
def read_list(path: Path = LIST) -> dict:
    cfg: dict = {"version": None, "boards": [], "designs": [], "card_cmdline": None}
    for n, line in enumerate(path.read_text().splitlines(), 1):
        f = line.split("#", 1)[0].split()
        if not f:
            continue
        if f[0] == "version" and len(f) == 2 and re.fullmatch(r"\d+\.\d+\.\d+", f[1]):
            cfg["version"] = f[1]
        elif f[0] == "board" and len(f) == 2:
            cfg["boards"].append(f[1])
        elif f[0] == "design" and len(f) == 4 and f[2] in ("boot", "test", "-") and f[3] in ("ddr", "-"):
            cfg["designs"].append({"name": f[1], "flash": None if f[2] == "-" else f[2], "ddr": f[3] == "ddr"})
        elif f[0] == "card-cmdline" and len(f) >= 2:
            cfg["card_cmdline"] = line.split("#", 1)[0].split(None, 1)[1].strip()
        else:
            raise Refused(f"{path}:{n}: cannot read {line.strip()!r}")
    if not cfg["version"] or not cfg["boards"] or not cfg["designs"]:
        raise Refused(f"{path}: needs a version, boards and designs")
    known = [ln.split()[0] for ln in (ROOT / "boards/arty_a7/boards.txt").read_text().splitlines()
             if ln.split() and not ln.startswith("#")]
    for b in cfg["boards"]:
        if b not in known:
            raise Refused(f"{path}: unknown board {b} (boards/arty_a7/boards.txt)")
    for d in cfg["designs"]:
        if not (ROOT / "boards/arty_a7/designs" / f"{d['name']}.files").is_file():
            raise Refused(f"{path}: unknown design {d['name']} (boards/arty_a7/designs/)")
    return cfg


def pick(given: str | None, allowed: list[str], what: str) -> list[str]:
    if not given:
        return list(allowed)
    want = [x for x in re.split(r"[,\s]+", given) if x]
    for x in want:
        if x not in allowed:
            raise Refused(f"unknown {what} {x!r} (release/release.txt: {', '.join(allowed)})")
    return [x for x in allowed if x in want]


# ============================================================================== contracts and versions
def contracts() -> dict:
    spec = (ROOT / "spec" / "soc.toml").read_text()

    def spec_get(key: str) -> str:
        m = re.search(rf"^{key}\s*=\s*\"?([^\"#\n]*?)\"?\s*(#.*)?$", spec, re.M)
        return m.group(1).strip() if m else ""

    c: dict = {"spec": "spec/soc.toml", "spec_sha256": sha256(ROOT / "spec" / "soc.toml")}
    for k in ("map_version", "soc_version", "isa", "abi", "image_header_version", "bootinfo_version", "platform_abi"):
        v = spec_get(k)
        c[k] = int(v) if v.isdigit() else v
    for doc in CONTRACT_DOCS:
        p = ROOT / "docs" / doc
        text = p.read_text()
        m = (re.search(r"[Vv]ersion \**(\d+)\**", text))
        c[f"docs/{doc}"] = f"version {m.group(1) if m else '?'}, sha256 {sha256(p)}"
    return c


# ============================================================================== ELF symbols (diagnosis)
def elf_sections(data: bytes) -> list[tuple[str, int, int, int]]:
    """(name, address, file offset, size) of each section of an ELF32 little-endian file"""
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return []
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    hdrs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]

    def name(off: int) -> str:
        start = strtab[4] + off
        return data[start:data.index(b"\0", start)].decode(errors="replace")
    return [(name(h[0]), h[3], h[4], h[5] if h[1] != 8 else 0) for h in hdrs]


def elf_symbols(data: bytes) -> list[tuple[int, int, str]]:
    """(address, size, name) of the function and object symbols, sorted"""
    if data[:4] != b"\x7fELF":
        return []
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, _ = struct.unpack_from("<HHH", data, 0x2E)
    hdrs = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for h in hdrs:
        if h[1] != 2:                                   # SHT_SYMTAB
            continue
        strs = hdrs[h[6]]
        for k in range(h[5] // 16):
            st_name, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", data, h[4] + 16 * k)
            if info & 0xF in (1, 2) and shndx:
                start = strs[4] + st_name
                out.append((value, size, data[start:data.index(b"\0", start)].decode(errors="replace")))
    return sorted(out)


def symbol_at(elf: Path | None, addr: int) -> str | None:
    if elf is None or not elf.is_file():
        return None
    best = None
    for value, size, name in elf_symbols(elf.read_bytes()):
        if value <= addr and (addr < value + size or (size == 0 and best is None)):
            best = f"{name}+0x{addr - value:x}"
        if value > addr:
            break
    return best


# ============================================================================== explaining a difference
def first_diff(a: bytes, b: bytes) -> int:
    n = min(len(a), len(b))
    step = 4096
    i = 0
    while i < n and a[i:i + step] == b[i:i + step]:
        i += step
    while i < n and a[i] == b[i]:
        i += 1
    return i


def count_diff(a: bytes, b: bytes) -> int:
    return sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))


def explain_stream(a: bytes, b: bytes, off: int) -> str:
    """a difference inside two configuration streams, located in the packets of a"""
    info = bitfile.walk(a)
    word = off // 4
    where = "outside the packets"
    for start, end, what in info["packets"]:
        if not start <= word <= end:
            continue
        fd = [(s, n) for s, n in info["fdri"] if n and s <= word < s + n]
        if fd:
            s, n = fd[0]
            k = word - s
            fw = bitfile.FRAME_WORDS * 4
            fa, fb = a[s * 4:(s + n) * 4], b[s * 4:(s + n) * 4]
            nf = sum(1 for f in range(n // bitfile.FRAME_WORDS) if fa[f * fw:(f + 1) * fw] != fb[f * fw:(f + 1) * fw])
            where = (f"frame data: configuration frame {k // bitfile.FRAME_WORDS} of {n // bitfile.FRAME_WORDS}, "
                     f"word {k % bitfile.FRAME_WORDS}; {nf} frames differ in all: the placed and routed design, or "
                     "a block RAM's contents, differs")
        else:
            where = f"the packet at stream word {start}: {what} (a configuration option or command)"
        break
    return f"stream byte {off} (word {word}): {where}"


def compare_records(ra: Path, rb: Path) -> str:
    """the build inputs that differ between two bitstreams' build records"""
    if not ra.is_file() or not rb.is_file():
        return "no build records to compare"
    a, b = parse_toml(ra.read_text()), parse_toml(rb.read_text())
    diffs = [f"{k} {a.get(k)!r} against {b.get(k)!r}" for k in BIT_INPUTS if a.get(k) != b.get(k)]
    fa, fb = a.get("openxc7", {}), b.get("openxc7", {})
    diffs += [f"openxc7.{k} {fa.get(k)!r} against {fb.get(k)!r}" for k in FLOW_INPUTS if fa.get(k) != fb.get(k)]
    if diffs:
        return "build inputs that differ (build records): " + "; ".join(diffs)
    return ("the recorded build inputs are the same (sources, constraints, ROM, build id and time, seed, tool "
            "versions): the tools did not reproduce their output; suspects: nextpnr's threads, hash order "
            "(PYTHONHASHSEED), a rebuilt image under the same tag")


def explain_bit(a: bytes, b: bytes, off: int, ref_dir: Path, new_dir: Path, rel: str) -> str:
    try:
        fa, sa, sta = bitfile.parse_header(a)
        fb, _, stb = bitfile.parse_header(b)
    except bitfile.BitError as e:
        return f"not a .bit file: {e}"
    records = compare_records(ref_dir / Path(rel).parent / "manifest.txt", new_dir / Path(rel).parent / "manifest.txt")
    if off < sta:
        for k, (s, e) in sa.items():
            if s - 3 <= off < e:
                same = a[sta:] == b[stb:]
                tail_ = " The configuration streams are identical: only the header differs." if same else ""
                if k in "cd":
                    return (f"the .bit header's date and time ('{fa.get('c')} {fa.get('d')}' against '{fb.get('c')} "
                            f"{fb.get('d')}'): xc7frames2bit writes the time of the build there; tools/openxc7-build.sh "
                            "stamps SOURCE_DATE_EPOCH instead when it is set (make release sets it): was it set, to the "
                            f"commit's time, for both builds?{tail_}")
                if k == "a":
                    return (f"the .bit header's design field ('{fa.get('a')}' against '{fb.get('a')}'): it names the "
                            f"frames file, so the build's output directory differs (SURFCPU_FPGA_OUT).{tail_}")
                if k == "b":
                    return f"the .bit header's part ('{fa.get('b')}' against '{fb.get('b')}')"
                return f"the .bit header's length field: the streams differ in length ({fa.get('e')} against {fb.get('e')})"
        return "the .bit header's layout differs"
    return explain_stream(a[sta:], b[stb:], off - sta) + ". " + records


def explain_package(a: bytes, b: bytes, off: int, elf: Path | None) -> str:
    if len(a) < 64 or len(b) < 64:
        return "not a SURFOS.BIN package (shorter than its 64-byte header)"
    fields = []
    for o, n, name in PKG_FIELDS:
        if a[o:o + n] != b[o:o + n]:
            va = a[o:o + n].rstrip(b"\0").decode("ascii", "replace") if name == "name" else int.from_bytes(a[o:o + n], "little")
            vb = b[o:o + n].rstrip(b"\0").decode("ascii", "replace") if name == "name" else int.from_bytes(b[o:o + n], "little")
            fields.append(f"{name} {va:#x} against {vb:#x}" if isinstance(va, int) else f"{name} {va!r} against {vb!r}")
    parts = []
    if fields:
        parts.append("header fields: " + "; ".join(fields))
        if any(f.startswith("build_id") for f in fields):
            parts.append("a different build id: another commit, or a dirty tree")
    p = first_diff(a[64:], b[64:])
    if a[64:] != b[64:]:
        load_addr = int.from_bytes(a[16:20], "little")
        addr = load_addr + p
        sym = symbol_at(elf, addr)
        parts.append(f"payload: first difference at payload byte {p} = address {addr:#x}"
                     + (f", in {sym}" if sym else "") + f", {count_diff(a[64:], b[64:])} bytes differ: a value compiled "
                     "into the code differs (the build time: SOURCE_DATE_EPOCH must reach the container through "
                     "devenv/run.sh; the build id; the compiler, its flags or the sources)")
    return "; ".join(parts) if parts else f"byte {off}"


def explain_mem(a: bytes, b: bytes, off: int, elf: Path | None) -> str:
    line = off // 9
    addr = line * 4
    sym = symbol_at(elf, addr)
    return (f"line {line + 1} (ROM word address {addr:#06x}{', in ' + sym if sym else ''}); "
            f"{sum(1 for x, y in zip(a.split(), b.split()) if x != y)} words differ: the ROM program differs")


def explain_elf(a: bytes, b: bytes, off: int) -> str:
    for name, addr, start, size in elf_sections(a):
        if size and start <= off < start + size:
            what = ("debug information: a build path (the container mounts the tree at /work), the compiler or "
                    "the sources" if name.startswith(".debug") else
                    "the symbol table" if name in (".symtab", ".strtab") else "code or data")
            return f"section {name} at file offset {off} ({what})"
    return f"file offset {off}, in the ELF headers"


def explain(rel: str, a: bytes, b: bytes, ref_dir: Path, new_dir: Path) -> str:
    off = first_diff(a, b)
    size = f"{len(a)} against {len(b)} bytes, first difference at byte {off} (0x{off:x})"
    name = Path(rel).name
    try:
        if rel.endswith(".bit"):
            why = explain_bit(a, b, off, ref_dir, new_dir, rel)
        elif rel.startswith("flash/") and rel.endswith(".bin"):
            why = explain_stream(a, b, off) + ". " + compare_records(
                ref_dir / "fpga" / Path(rel).parent.name / Path(rel).stem / "manifest.txt",
                new_dir / "fpga" / Path(rel).parent.name / Path(rel).stem / "manifest.txt")
        elif name == "SURFOS.BIN":
            why = explain_package(a, b, off, ref_dir / "debug" / "surfos.elf")
        elif name == "DDRDIAG.BIN":
            why = explain_package(a, b, off, ref_dir / "debug" / "ddrdiag.elf")
        elif rel.startswith("rom/") and rel.endswith(".mem"):
            why = explain_mem(a, b, off, ref_dir / "debug" / (Path(rel).stem + ".elf"))
        elif rel.endswith(".elf"):
            why = explain_elf(a, b, off)
        elif name == "CMDLINE.TXT":
            why = f"the command line {a.decode(errors='replace')!r} against {b.decode(errors='replace')!r} (release/release.txt)"
        else:
            why = "no specific explanation for this kind of file"
    except Exception as e:                            # a diagnosis must never hide the difference itself
        why = f"(could not analyse it: {e})"
    return f"{size}: {why}"


# ============================================================================== comparing two releases
def read_sums(path: Path) -> dict:
    out = {}
    for line in path.read_text().splitlines():
        m = re.fullmatch(r"([0-9a-f]{64}) [ *](.+)", line.strip())
        if not m:
            raise Refused(f"{path}: not a SHA256SUMS line: {line!r}")
        out[m.group(2)] = m.group(1)
    return out


def scope_of(rel: str) -> tuple[str, str] | None:
    """(board, design) of a bitstream or flash image; None for everything else"""
    p = Path(rel).parts
    if p[0] == "fpga" and len(p) == 4:
        return p[1], p[2]
    if p[0] == "flash" and len(p) == 3:
        return p[1], Path(p[2]).stem
    return None


def compare(ref: Path, new: Path, rebuilt: set | None = None) -> tuple[list, list[str]]:
    """rows (path, status, detail) for every artifact of the reference's SHA256SUMS, and notes"""
    ref_sums = read_sums(ref / "SHA256SUMS" if ref.is_dir() else ref)
    ref_dir = ref if ref.is_dir() else ref.parent
    new_sums = read_sums(new / "SHA256SUMS") if (new / "SHA256SUMS").is_file() else {}
    rows = []
    for rel in sorted(ref_sums):
        want = ref_sums[rel]
        sc = scope_of(rel)
        if rebuilt is not None and sc is not None and sc not in rebuilt:
            rows.append((rel, "not rebuilt", "its design was not rebuilt (--designs/--boards)"))
            continue
        f = new / rel
        if not f.is_file():
            rows.append((rel, "MISSING", "the rebuild has no such file" + (" (its design did not build: see the "
                                                                           "rebuild's MANIFEST)" if sc else "")))
            continue
        got = sha256(f)
        if got == want:
            rows.append((rel, "match", want))
            continue
        ra = ref_dir / rel
        if not ra.is_file() or sha256(ra) != want:
            rows.append((rel, "DIFFERS", f"sha256 {got}, the reference says {want} (no reference copy of the file "
                                         "to compare byte by byte)"))
            continue
        rows.append((rel, "DIFFERS", explain(rel, ra.read_bytes(), f.read_bytes(), ref_dir, new)))
    notes = []
    for rel in sorted(set(new_sums) - set(ref_sums)):
        notes.append(f"only in the rebuild: {rel}")
    if (ref_dir / "MANIFEST").is_file() and (new / "MANIFEST").is_file():
        notes += compare_manifests(ref_dir / "MANIFEST", new / "MANIFEST", rebuilt)
    return rows, notes


def compare_manifests(a_path: Path, b_path: Path, rebuilt: set | None) -> list[str]:
    a, b = parse_toml(a_path.read_text()), parse_toml(b_path.read_text())
    out = []
    for k in ("commit", "source_date_epoch", "version"):
        if a.get(k) != b.get(k):
            out.append(f"MANIFEST {k}: {a.get(k)!r} against {b.get(k)!r}")
    for t in ("tools", "contracts"):
        ta, tb = a.get(t, {}), b.get(t, {})
        for k in sorted(set(ta) | set(tb)):
            if ta.get(k) != tb.get(k):
                out.append(f"MANIFEST [{t}] {k}: {ta.get(k)!r} against {tb.get(k)!r}")
    da = {(d.get("board"), d.get("name")): d for d in a.get("design", [])}
    db = {(d.get("board"), d.get("name")): d for d in b.get("design", [])}
    for key in sorted(da):
        if rebuilt is not None and key not in rebuilt:
            continue
        x, y = da[key], db.get(key, {})
        if x.get("status") != "built":
            continue
        for k in sorted(set(x) | set(y)):
            if k != "reason" and x.get(k) != y.get(k):
                out.append(f"MANIFEST design {key[1]} for {key[0]}: {k} {x.get(k)!r} against {y.get(k)!r}")
    if not out:
        out.append("MANIFEST: the same commit, tools, contracts and gate results"
                   + (" for the rebuilt designs" if rebuilt is not None else ""))
    return out


def report_lines(rows: list, notes: list[str], title: str) -> list[str]:
    n = {s: sum(1 for r in rows if r[1] == s) for s in ("match", "DIFFERS", "MISSING", "not rebuilt")}
    lines = [title, ""]
    for rel, status, detail in rows:
        if status == "match":
            lines.append(f"  match        {rel}  sha256 {detail[:16]}...")
        else:
            lines.append(f"  {status:<12} {rel}")
            lines.append(f"               {detail}")
    lines += [""] + [f"  note: {x}" for x in notes] + [""]
    compared = n["match"] + n["DIFFERS"] + n["MISSING"]
    verdict = "PASS" if compared and n["DIFFERS"] == 0 and n["MISSING"] == 0 else "FAIL"
    lines.append(f"{verdict}: {n['match']} of {compared} compared artifacts are byte for byte the same; "
                 f"{n['DIFFERS']} differ, {n['MISSING']} missing, {n['not rebuilt']} not rebuilt")
    return lines


# ============================================================================== make
class Release:
    def __init__(self, a):
        self.a = a
        self.cfg = read_list()
        self.t0 = time.time()

    # ---------------------------------------------------------------- preflight
    def preflight(self) -> None:
        a, cfg = self.a, self.cfg
        if os.environ.get("SURFCPU_FPGA_OUT"):
            raise Refused("SURFCPU_FPGA_OUT is set: a release builds in build/fpga (its path is in the .bit header)")
        self.commit = git("rev-parse", "HEAD")
        self.short = self.commit[:8]
        self.epoch = int(git("log", "-1", "--format=%ct", "HEAD"))
        dirty = git("status", "--porcelain").splitlines()
        if dirty:
            raise Refused("the tree is not clean, so the release would not be the commit (commit or remove "
                          "these first):\n" + "\n".join("    " + x for x in dirty[:20])
                          + ("\n    ..." if len(dirty) > 20 else ""))
        sde = os.environ.get("SOURCE_DATE_EPOCH")
        if sde and sde != str(self.epoch):
            raise Refused(f"SOURCE_DATE_EPOCH={sde} is not the commit's time ({self.epoch}); unset it: the "
                          "release sets it itself")
        self.boards = pick(a.boards, cfg["boards"], "board")
        names = [d["name"] for d in cfg["designs"]]
        chosen = pick(a.designs, names, "design")
        self.only = None
        if a.only is not None:                         # release-verify: exactly these board/design pairs
            self.only = {tuple(x.split("/", 1)) for x in re.split(r"[,\s]+", a.only) if x}
            for b, n in self.only:
                if b not in cfg["boards"] or n not in names:
                    raise Refused(f"--only {b}/{n}: no such board and design in release/release.txt")
        self.plan, self.missing = [], []
        for d in sorted(cfg["designs"], key=lambda d: d["ddr"]):          # the DDR designs last
            for board in cfg["boards"]:
                if d["name"] not in chosen or board not in self.boards or (
                        self.only is not None and (board, d["name"]) not in self.only):
                    self.missing.append((board, d, "not built: not requested (DESIGNS/BOARDS)"))
                elif d["ddr"] and a.skip_ddr:
                    self.missing.append((board, d, "not built: SKIP_DDR=1"))
                else:
                    self.plan.append((board, d))
        if self.missing and not a.prerelease:
            raise Refused("these designs would be missing, and a release without them is a pre-release "
                          "(make release PRERELEASE=1):\n" + "\n".join(
                              f"    {d['name']} for {b}: {why}" for b, d, why in self.missing))
        self.tc_image, self.ox_image = toolchain_image(), openxc7_image()
        self.tc_id, self.ox_id = image_id(self.tc_image), image_id(self.ox_image)
        for tag, iid, how in ((self.tc_image, self.tc_id, "make image"), (self.ox_image, self.ox_id, "make openxc7-image")):
            if not iid:
                raise Refused(f"the image {tag} is missing: build it with make -C surfcpu {how} (docs/RELEASE.md)")
        self.version = cfg["version"]
        self.out_root = Path(a.out).resolve() if a.out else None
        self.logs = Path(a.logs).resolve() if a.logs else None

    def name(self, prerelease: bool) -> str:
        return f"{self.version}{'-pre' if prerelease else ''}-{self.short}"

    def env(self, **extra) -> dict:
        e = dict(os.environ)
        for k in ("DESIGN", "BIT", "ROM", "SURFCPU_FPGA_OUT", "OPENXC7_THREADS", "SURFCPU_BUILD_ID",
                  "SURFCPU_BUILD_DIRTY", "PYTHONHASHSEED"):
            e.pop(k, None)
        e.update({"SOURCE_DATE_EPOCH": str(self.epoch), "LC_ALL": "C", "TZ": "UTC"})
        e.update(extra)
        return e

    # ---------------------------------------------------------------- steps
    def software(self) -> None:
        for p in (ROOT / "build" / "fw", REPO / "build" / "riscv32" / "arty_a7"):
            shutil.rmtree(p, ignore_errors=True)
        run = os.environ.get("SURFCPU_RELEASE_RUN") or str(ROOT / "devenv" / "run.sh")
        log = self.logs / "software.txt"
        t = time.time()
        rc = run_logged([run, "make", "--no-print-directory", "build-rom", "build-kernel"], log, self.env())
        if rc != 0:
            raise Refused(f"make build-rom build-kernel failed (exit {rc}); {log}:\n{tail(log)}")
        self.sw = {
            "SURFOS.BIN": REPO / "build/riscv32/arty_a7/SURFOS.BIN",
            "DDRDIAG.BIN": ROOT / "build/fw/DDRDIAG.BIN",
            "rom/surfboot.mem": ROOT / "build/fw/surfboot.mem",
            "rom/bramdiag.mem": ROOT / "build/fw/bramdiag.mem",
            "debug/surfboot.elf": ROOT / "build/fw/surfboot.elf",
            "debug/bramdiag.elf": ROOT / "build/fw/bramdiag.elf",
            "debug/ddrdiag.elf": ROOT / "build/fw/ddrdiag.elf",
            "debug/surfos.elf": REPO / "build/riscv32/arty_a7/surfos.elf",
        }
        for rel, p in self.sw.items():
            if not p.is_file():
                raise Refused(f"the software build made no {p}")
        for rel in ("SURFOS.BIN", "DDRDIAG.BIN"):
            bid = struct.unpack_from("<I", self.sw[rel].read_bytes(), 44)[0]
            if f"{bid:08x}" != self.short:
                raise Refused(f"{rel} carries build id {bid:08x}, not the commit's {self.short}")
        gen = (ROOT / "build/fw/gen/fw_build.h").read_text()
        if not re.search(r"FW_BUILD_DIRTY 0\b", gen):
            raise Refused("the firmware was built as a dirty tree (build/fw/gen/fw_build.h)")
        say(f"software: SurfBoot, the BRAM diagnostics, DDRDIAG.BIN and SURFOS.BIN, build {self.short} "
            f"({minutes(time.time() - t)})")

    def litedram(self) -> str | None:
        """make litedram-gen once for the DDR designs: None, or why it failed"""
        if getattr(self, "_litedram", "unset") != "unset":
            return self._litedram
        run = os.environ.get("SURFCPU_RELEASE_RUN") or str(ROOT / "devenv" / "run.sh")
        log = self.logs / "litedram-gen.txt"
        t = time.time()
        rc = run_logged([run, "make", "--no-print-directory", "litedram-gen"], log, self.env())
        self._litedram = None if rc == 0 else f"make litedram-gen failed (exit {rc}; it fetches pinned sources the first time)"
        say(f"LiteDRAM core generated and checked against ip/litedram/arty.sha256 ({minutes(time.time() - t)})"
            if rc == 0 else f"{self._litedram}:\n{tail(log)}")
        return self._litedram

    def wait_slot(self) -> None:
        limit = self.a.max_flow_containers
        waited = 0
        while limit and flow_containers(self.ox_image) >= limit:
            if waited % 300 == 0:
                say(f"waiting: {limit} or more {self.ox_image} containers are running (other builds)")
            time.sleep(15)
            waited += 15

    def bitstream(self, board: str, d: dict) -> tuple[bool, dict, str]:
        design = d["name"]
        fpga = os.environ.get("SURFCPU_RELEASE_FPGA") or str(ROOT / "tools" / "fpga-build.sh")
        base = ROOT / "build" / "fpga" / board / design
        log = self.logs / f"fpga-{board}-{design}.txt"
        self.wait_slot()
        rc = run_logged([fpga, "--design", design, board], log, self.env(FLOW="openxc7"))
        m = parse_toml((base / "manifest.txt").read_text()) if (base / "manifest.txt").is_file() else {}
        bit = base / f"{design}.bit"
        problems = []
        if rc != 0:
            problems.append(m.get("reason") or f"tools/fpga-build.sh exit status {rc}")
        elif m.get("result") != "pass":
            problems.append(m.get("reason") or "its manifest does not say pass")
        else:
            checks = {"source_revision": self.commit, "source_dirty": False, "build_time": self.epoch,
                      "generated_files_match": True, "board": board, "design": design}
            problems += [f"manifest {k} = {m.get(k)!r}, not {v!r}" for k, v in checks.items() if m.get(k) != v]
            if not bit.is_file() or sha256(bit) != m.get("bitstream_sha256"):
                problems.append("the bitstream is not the one its manifest describes")
        return not problems, m, "; ".join(problems)

    # ---------------------------------------------------------------- assembling
    def card(self, stage: Path, package: Path) -> list[str]:
        """the card's /BOOT files as tools/deploy-sd.py installs them on a new card, run against a
        directory standing in for the card (its tests' stand-ins for diskutil and mount)"""
        work = self.logs / "card"
        shutil.rmtree(work, ignore_errors=True)
        view, device = work / "card", work / "device"
        view.mkdir(parents=True)
        cfg = work / "fake.json"
        import json
        cfg.write_text(json.dumps({
            "mount": str(view), "device": "/dev/disk9s1", "device_dir": str(device),
            "volume": {"FilesystemType": "msdos", "FilesystemName": "MS-DOS FAT32", "RemovableMedia": True,
                       "Removable": True, "Ejectable": True, "Internal": True, "BusProtocol": "Secure Digital",
                       "VirtualOrPhysical": "Physical", "WritableVolume": True, "VolumeName": "SURFOS",
                       "MediaName": "SDXC Reader", "TotalSize": 31914983424},
            "whole": {"Content": "FDisk_partition_scheme", "Internal": True, "RemovableMedia": True,
                      "BusProtocol": "Secure Digital", "VirtualOrPhysical": "Physical"}}))
        py = shlex.quote(sys.executable)
        fakes = ROOT / "firmware" / "tests" / "deploy"
        env = self.env(SURFCPU_DEPLOY_TEST="1", FAKE_DEPLOY_CONFIG=str(cfg),
                       SURFCPU_DISKUTIL=f"{py} {shlex.quote(str(fakes / 'fake_diskutil.py'))}",
                       SURFCPU_MOUNT=f"{py} {shlex.quote(str(fakes / 'fake_mount.py'))}")
        cmd = [sys.executable, str(ROOT / "tools" / "deploy-sd.py"), "--mount", str(view), "--image", str(package),
               "--new-card"]
        if self.cfg["card_cmdline"]:
            cmd += ["--cmdline", self.cfg["card_cmdline"]]
        log = self.logs / "card.txt"
        rc = run_logged(cmd, log, env)
        if rc != 0:
            raise Refused(f"tools/deploy-sd.py did not install the card files (exit {rc}):\n{tail(log)}")
        names = []
        for p in sorted((device / "BOOT").iterdir()):
            if p.is_file():
                shutil.copyfile(p, stage / "card" / "BOOT" / p.name)
                names.append(f"card/BOOT/{p.name}")
        if (stage / "card/BOOT/SURFOS.BIN").read_bytes() != package.read_bytes():
            raise Refused("deploy-sd installed other bytes than SURFOS.BIN")
        return names

    def make(self) -> int:
        a = self.a
        self.preflight()
        name = self.name(a.prerelease)
        out_root = self.out_root or ROOT / "build" / "release"
        out = out_root / name if not a.out_exact else Path(a.out).resolve()
        if out.exists() and not a.force:
            raise Refused(f"{out} exists (FORCE=1 replaces it)")
        if not self.logs:
            self.logs = out.parent / f"{out.name}.logs"
        shutil.rmtree(self.logs, ignore_errors=True)
        self.logs.mkdir(parents=True)
        say(f"SurfCPU {self.version} at {self.short} (commit time {utc(self.epoch)} = SOURCE_DATE_EPOCH "
            f"{self.epoch}), clean tree")
        say(f"images: {self.tc_image} ({self.tc_id[:19]}), {self.ox_image} ({self.ox_id[:19]})")
        say(f"plan: {len(self.plan)} bitstreams" + (f"; {len(self.missing)} not attempted" if self.missing else "")
            + f"; logs in {self.logs}")
        steps = 1 + len(self.plan)
        say(f"[1/{steps}] the software (make build-rom build-kernel, from empty output directories)")
        self.software()
        built, failed = [], []
        for n, (board, d) in enumerate(self.plan, 2):
            t = time.time()
            say(f"[{n}/{steps}] {d['name']} for {board} (tools/fpga-build.sh, FLOW=openxc7)")
            why = self.litedram() if d["ddr"] else None
            if why:
                ok, m = False, {}
            else:
                ok, m, why = self.bitstream(board, d)
            if ok:
                built.append((board, d, m))
                say(f"    PASS ({minutes(time.time() - t)}): wns {m.get('openxc7', {}).get('wns_ns')} ns, "
                    f"sha256 {m.get('bitstream_sha256', '')[:16]}...")
                continue
            say(f"    FAIL ({minutes(time.time() - t)}): {why}")
            if not d["ddr"]:
                raise Refused(f"{d['name']} for {board} did not build, and the release needs it: {why} "
                              f"(log {self.logs / ('fpga-' + board + '-' + d['name'] + '.txt')})")
            failed.append((board, d, why))
        self.missing += [(b, d, f"did not build: {why}") for b, d, why in failed]
        if failed and not a.prerelease:
            raise Refused("the release was not written: these DDR designs did not build:\n" + "\n".join(
                f"    {d['name']} for {b}: {why}" for b, d, why in failed)
                + "\nmake release PRERELEASE=1 writes it as a pre-release without them")
        prerelease = bool(self.missing)
        if prerelease != a.prerelease:                 # every design built: a complete release
            name = self.name(prerelease)
            out = out_root / name if not a.out_exact else out
            if out.exists() and not a.force:
                raise Refused(f"{out} exists (FORCE=1 replaces it)")
        if git("status", "--porcelain"):
            raise Refused("the tree changed during the release: " + git("status", "--porcelain").splitlines()[0])
        stage = out.parent / f".{out.name}.stage"
        shutil.rmtree(stage, ignore_errors=True)
        (stage / "card" / "BOOT").mkdir(parents=True)
        files: dict = {}
        for rel, p in self.sw.items():
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(p, stage / rel)
            files[rel] = None
        designs = []
        for board, d, m in built:
            src = ROOT / "build" / "fpga" / board / d["name"]
            rel = f"fpga/{board}/{d['name']}/{d['name']}.bit"
            (stage / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src / f"{d['name']}.bit", stage / rel)
            shutil.copyfile(src / "manifest.txt", stage / Path(rel).parent / "manifest.txt")
            files[rel] = None
            flash = None
            if d["flash"]:
                stream, desc = bitfile.flash_image((stage / rel).read_bytes(), board)
                flash = f"flash/{board}/{d['name']}.bin"
                (stage / flash).parent.mkdir(parents=True, exist_ok=True)
                (stage / flash).write_bytes(stream)
                files[flash] = None
            ox = m.get("openxc7", {})
            designs.append({
                "name": d["name"], "board": board, "status": "built", "flash": d["flash"] or "-", "needs_ddr": d["ddr"],
                "bitstream": rel, "flash_image": flash or "", "build_record": f"fpga/{board}/{d['name']}/manifest.txt",
                "part": m.get("part", ""), "idcode": ox.get("idcode", ""), "rom_sha256": m.get("rom_sha256", ""),
                "placer_seed": m.get("placer_seed", 1),
                "gates": [f"{g[6:]} {ox.get(g)}" for g in GATES if g in ox],
                "timing": ox.get("timing", []), "wns_ns": ox.get("wns_ns", ""),
                "use": [f"{k} {ox.get(k)}" for k in ("lut", "ff", "bram36", "dsp") if k in ox],
                "constraints_not_checked": len(ox.get("constraints_not_checked", [])),
            })
            if flash:
                designs[-1]["flash_map"] = bitfile.describe_map(desc)[1:]
        for b, d, why in self.missing:
            designs.append({"name": d["name"], "board": b, "status": "missing", "flash": d["flash"] or "-",
                            "needs_ddr": d["ddr"], "reason": why})
        for rel in self.card(stage, stage / "SURFOS.BIN"):
            files[rel] = None
        sums = {rel: sha256(stage / rel) for rel in sorted(files)}
        (stage / "SHA256SUMS").write_text("".join(f"{h}  {rel}\n" for rel, h in sorted(sums.items())))
        self.write_manifest(stage / "MANIFEST", name, prerelease, sums, designs)
        if out.exists():
            shutil.rmtree(out)
        os.rename(stage, out)
        say(f"written {out}: " + ("PRE-RELEASE, without:" if prerelease else "complete release") +
            "".join(f"\n    {d['name']} for {b}: {why}" for b, d, why in self.missing))
        say(f"{len(sums)} artifacts in SHA256SUMS (check: cd {out} && shasum -a 256 -c SHA256SUMS); "
            f"{minutes(time.time() - self.t0)} in all")
        return 0

    def write_manifest(self, path: Path, name: str, prerelease: bool, sums: dict, designs: list) -> None:
        top = {
            "release": name, "version": self.version, "status": "prerelease" if prerelease else "complete",
            "complete": not prerelease,
            "missing": [f"{d['name']} for {b}: {why}" for b, d, why in self.missing],
            "commit": self.commit, "commit_short": self.short, "source_date_epoch": self.epoch,
            "commit_time_utc": utc(self.epoch), "tree_clean": True,
            "flash_boot_design": next((d["name"] for d in self.cfg["designs"] if d["flash"] == "boot"), ""),
            "card_cmdline": self.cfg["card_cmdline"] or "",
        }
        tools = {
            "toolchain_image": self.tc_image, "toolchain_image_id": self.tc_id,
            "toolchain_versions": image_versions(self.tc_image, "/etc/surfcpu-toolchain.txt"),
            "openxc7_image": self.ox_image, "openxc7_image_id": self.ox_id,
            "openxc7_versions": image_versions(self.ox_image, "/etc/surfcpu-openxc7.txt"),
            "flow": "openxc7 (tools/fpga-build.sh FLOW=openxc7, every gate of docs/BOARD.md section 8)",
            "nextpnr_threads": nextpnr_threads(), "locale": "LC_ALL=C TZ=UTC",
        }
        what = {
            "SURFOS.BIN": "the SurfOS kernel package for the card (make build-kernel; BOOT-ABI.md section 2)",
            "DDRDIAG.BIN": "the DDR diagnostic as a card package (CARD.md section 6)",
            "rom/surfboot.mem": "SurfBoot, the boot ROM image inside the SurfBoot bitstreams",
            "rom/bramdiag.mem": "the BRAM diagnostics, the boot ROM image inside the bramdiag bitstreams",
            "card/BOOT/SURFOS.BIN": "what tools/deploy-sd.py installs on a new card",
            "card/BOOT/CMDLINE.TXT": "what tools/deploy-sd.py installs on a new card (--cmdline)",
        }
        lines = [
            "# SurfCPU release manifest, written by tools/release.py (docs/RELEASE.md). The SHA-256 of every",
            "# artifact is also in SHA256SUMS (cd <this directory> && shasum -a 256 -c SHA256SUMS). Next to each",
            "# bitstream, manifest.txt is its build record (tools/fpga-build.sh), which tools/program.sh reads;",
            "# it carries the build's own times, so it is not one of the reproducible artifacts.",
            toml_table(top).rstrip(), "", "[tools]", toml_table(tools).rstrip(), "", "[contracts]",
            toml_table(contracts()).rstrip()]
        for d in designs:
            lines += ["", "[[design]]", toml_table(d).rstrip()]
        for rel, h in sums.items():
            kind = ("bitstream" if rel.endswith(".bit") else "QSPI flash image" if rel.startswith("flash/")
                    else "symbols for diagnosis" if rel.startswith("debug/") else what.get(rel, ""))
            lines += ["", "[[artifact]]", toml_table({"path": rel, "bytes": (path.parent / rel).stat().st_size,
                                                      "sha256": h, "what": kind}).rstrip()]
        path.write_text("\n".join(lines) + "\n")


def cmd_make(a) -> int:
    return Release(a).make()


# ============================================================================== verify, compare, check
def cmd_verify(a) -> int:
    ref = Path(a.release).resolve()
    if not (ref / "MANIFEST").is_file() or not (ref / "SHA256SUMS").is_file():
        raise Refused(f"{ref} is not a release directory (no MANIFEST and SHA256SUMS)")
    man = parse_toml((ref / "MANIFEST").read_text())
    commit = man.get("commit", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise Refused(f"{ref}/MANIFEST names no commit")
    try:
        git("cat-file", "-e", commit + "^{commit}")
    except Refused:
        raise Refused(f"the commit {commit} is not in this repository")
    built = [(d["board"], d["name"]) for d in man.get("design", []) if d.get("status") == "built"]
    boards = sorted({b for b, _ in built})
    names = sorted({n for _, n in built})
    if a.boards:
        boards = [b for b in re.split(r"[,\s]+", a.boards) if b]
    if a.designs:
        names = [n for n in re.split(r"[,\s]+", a.designs) if n]
    rebuilt = {(b, n) for b, n in built if b in boards and n in names}
    work = Path(a.work).resolve() if a.work else ROOT / "build" / "release-verify" / f"{ref.name}-{time.strftime('%Y%m%dT%H%M%S')}"
    if work.exists() and any(work.iterdir()):
        raise Refused(f"{work} exists and is not empty")
    work.mkdir(parents=True, exist_ok=True)
    src = work / "src"
    t0 = time.time()
    say(f"verify {ref.name}: a pristine copy of {commit[:8]} in {src} (git clone --shared)")
    subprocess.run(["git", "clone", "-q", "--shared", "--no-checkout", str(REPO), str(src)], check=True)
    subprocess.run(["git", "-C", str(src), "checkout", "-q", "--detach", commit], check=True)
    st = subprocess.run(["git", "-C", str(src), "status", "--porcelain"], capture_output=True, text=True).stdout
    if st.strip():
        raise Refused(f"the pristine copy is not clean: {st.splitlines()[0]}")
    out = work / "release"
    cmd = [sys.executable, str(src / "surfcpu" / "tools" / "release.py"), "make", "--prerelease", "--out", str(out),
           "--out-exact", "--logs", str(work / "logs")]
    cmd += ["--only", ",".join(f"{b}/{n}" for b, n in sorted(rebuilt))]
    say(f"rebuilding there: {len(rebuilt)} of the release's {len(built)} bitstreams and every other artifact")
    env = {k: v for k, v in os.environ.items() if k not in ("SOURCE_DATE_EPOCH", "SURFCPU_FPGA_OUT")}
    rc = subprocess.run(cmd, cwd=str(src / "surfcpu"), env=env).returncode
    say(f"the rebuild ended with exit status {rc} after {minutes(time.time() - t0)}")
    rows, notes = compare(ref, out, {(b, n) for b, n in rebuilt}) if out.is_dir() else (
        [(rel, "MISSING", "the rebuild wrote no release") for rel in sorted(read_sums(ref / "SHA256SUMS"))], [])
    lines = report_lines(rows, notes, f"release-verify {ref.name}: commit {commit}, rebuilt in {src} "
                                      f"({minutes(time.time() - t0)})")
    text = "\n".join(lines) + "\n"
    (work / "verify-report.txt").write_text(text)
    if a.report:
        Path(a.report).write_text(text)
    print(text, end="")
    verdict = lines[-1].startswith("PASS")
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
        say(f"removed {work} (KEEP=1 keeps it)")
    return 0 if verdict else 1


def cmd_compare(a) -> int:
    ref, new = Path(a.reference).resolve(), Path(a.new).resolve()
    rows, notes = compare(ref, new)
    lines = report_lines(rows, notes, f"compare {ref} with {new}")
    text = "\n".join(lines) + "\n"
    if a.report:
        Path(a.report).write_text(text)
    print(text, end="")
    return 0 if lines[-1].startswith("PASS") else 1


def cmd_check(a) -> int:
    rel_dir = Path(a.release).resolve()
    sums = read_sums(rel_dir / "SHA256SUMS")
    man = parse_toml((rel_dir / "MANIFEST").read_text())
    bad = 0
    listed = {x["path"]: x for x in man.get("artifact", [])}
    for rel, h in sorted(sums.items()):
        p = rel_dir / rel
        ok = p.is_file() and sha256(p) == h and listed.get(rel, {}).get("sha256") == h
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {rel}")
    for rel in sorted(set(listed) - set(sums)):
        bad += 1
        print(f"FAIL {rel}: in MANIFEST, not in SHA256SUMS")
    status = man.get("status")
    print(f"{rel_dir.name}: {status}" + ("" if man.get("complete") else " (missing: " + "; ".join(man.get("missing", [])) + ")"))
    print(f"check: {'PASS' if not bad else f'{bad} FAILED'}: {len(sums)} artifacts")
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SurfCPU release (docs/RELEASE.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make", help="build the release from a clean tree")
    p.add_argument("--prerelease", action="store_true", help="allow a pre-release without the DDR designs")
    p.add_argument("--skip-ddr", action="store_true", help="do not attempt the DDR designs (implies a pre-release)")
    p.add_argument("--designs", help="only these designs (comma-separated; a pre-release)")
    p.add_argument("--boards", help="only these boards (comma-separated; a pre-release)")
    p.add_argument("--out", help="the directory for the release directory (default build/release)")
    p.add_argument("--out-exact", action="store_true", help="--out is the release directory itself")
    p.add_argument("--logs", help="where the build logs go (default next to the release)")
    p.add_argument("--force", action="store_true", help="replace an existing release directory")
    p.add_argument("--only", help="exactly these BOARD/DESIGN pairs (comma-separated; release-verify uses it)")
    p.add_argument("--max-flow-containers", type=int, default=3,
                   help="start a bitstream build only while fewer surfcpu-openxc7 containers run (0: no limit)")
    p = sub.add_parser("verify", help="rebuild a release from a pristine copy and compare")
    p.add_argument("release")
    p.add_argument("--designs")
    p.add_argument("--boards")
    p.add_argument("--work")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--report")
    p = sub.add_parser("compare", help="compare two releases artifact by artifact")
    p.add_argument("reference")
    p.add_argument("new")
    p.add_argument("--report")
    p = sub.add_parser("check", help="a release's files against its SHA256SUMS and MANIFEST")
    p.add_argument("release")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "make":
            return cmd_make(a)
        if a.cmd == "verify":
            return cmd_verify(a)
        if a.cmd == "compare":
            return cmd_compare(a)
        return cmd_check(a)
    except Refused as e:
        print(f"{ME}: {e}", file=sys.stderr)
        return 1 if a.cmd in ("make", "check") else 2
    except KeyboardInterrupt:
        print(f"{ME}: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
