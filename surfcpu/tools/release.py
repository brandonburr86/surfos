#!/usr/bin/env python3
"""SurfCPU release: build the reproducible standalone release, and check that it reproduces
--------------------
File: release.py Date: 10/16/26 (SurfCPU)
--------------------
Milestone 9 (docs/RELEASE.md). Runs on the Mac with its own python3 (3.9) and drives the builds in
the Docker images through devenv/run.sh and tools/fpga-build.sh, as a person would:

    tools/release.py make [--prerelease] [--skip-ddr] [--designs D,..] [--boards B,..] [--out DIR] [--logs DIR]
                          [--force]
        make -C surfcpu release [PRERELEASE=1] [SKIP_DDR=1] [DESIGNS=..] [BOARDS=..] [FORCE=1]
        From a clean tree only, with SOURCE_DATE_EPOCH = the commit's time: the
        boot ROMs and the kernel (make build-rom, arch/riscv32/kernel.mk), every design of
        release/release.txt for every board (tools/fpga-build.sh, FLOW=openxc7, every gate as it is),
        the QSPI flash images (tools/bitfile.py), the card's /BOOT files as tools/deploy-sd.py installs
        them, then SHA256SUMS and MANIFEST, in build/release/<version>-<commit>/. A DDR design that
        does not build makes a pre-release, named <version>-pre-<commit> and only with --prerelease;
        anything else that does not build stops the release. Everything is built in the release's own
        empty directory, build/release-work/ (one release at a time in a checkout: build/release-work.lock),
        and each artifact is copied into the stage directory the moment it passed its checks; before the
        release is written, HEAD, the tree, the images and every staged file are checked once more.
    tools/release.py verify RELEASE [--designs D,..] [--boards B,..] [--work DIR] [--keep] [--report FILE]
        make -C surfcpu release-verify RELEASE=build/release/<name> [DESIGNS=..] [BOARDS=..] [KEEP=1]
        Checks the release (as check does), clones its commit into a new directory (a pristine copy:
        git clone --shared), runs the release there with the same settings, and compares every
        artifact of the release's SHA256SUMS byte for byte, with the first differing offset and the
        likely cause of each difference. --designs/--boards rebuild only some of the bitstreams: names
        the release's MANIFEST does not know, or designs it did not build, are refused; the others
        are listed as not rebuilt and the verdict is PARTIAL, never PASS. The software artifacts are
        always rebuilt.
    tools/release.py compare REFERENCE NEW [--report FILE]
        The same comparison of two release directories (or a SHA256SUMS file and a release directory).
    tools/release.py check RELEASE
        The release's files against its SHA256SUMS and MANIFEST (what shasum -a 256 -c does, and more).

Exit status: make 0 written, 1 refused or failed; verify and compare 0 when every compared artifact
matched (verify: PASS, or PARTIAL when DESIGNS/BOARDS asked for only some; compare: PASS only), 1
when one differed, is missing, damaged or extra, or a release does not pass check, 2 when the
comparison could not be made.

LC_ALL=C and TZ=UTC hold on the Mac; devenv/run.sh passes only SOURCE_DATE_EPOCH into the containers,
which run with their images' own environment (no LANG, LC_* or TZ: the POSIX locale and UTC), and
MANIFEST says so. The builds get an environment of their own: PATH, HOME, USER, LOGNAME, SHELL, TMPDIR, TERM and
Docker's settings (DOCKER, DOCKER_HOST, DOCKER_CONFIG, DOCKER_CONTEXT, DOCKER_CERT_PATH,
DOCKER_TLS_VERIFY) from the caller's, and what the release sets (SOURCE_DATE_EPOCH, LC_ALL=C,
TZ=UTC, FLOW, SURFCPU_FPGA_OUT, ROM, SURFCPU_KERNEL); nothing else. Settings that would change what a
build makes or uses (DRY_RUN, SURFCPU_FPGA_OUT, SURFCPU_IMAGE, SURFCPU_OPENXC7_IMAGE,
SURFCPU_OPENXC7_BUILD, SURFCPU_ROMCHECK, ROM, SURFCPU_BUILD_ID, SURFCPU_BUILD_DIRTY) are refused.
Stand-ins for the tests (tools/release-test.py), honoured only with SURFCPU_RELEASE_TEST=1:
SURFCPU_RELEASE_RUN (devenv/run.sh), SURFCPU_RELEASE_FPGA (tools/fpga-build.sh),
SURFCPU_RELEASE_DOCKER (docker); the FAKE_* settings of the stand-ins then reach the builds too.
"""
from __future__ import annotations

import argparse
import calendar
import hashlib
import importlib.util
import json
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
GATES = ("check_srl", "check_synthesis", "check_bitstream", "check_round_trip", "check_block_ram", "check_lvs",
         "check_clocks", "check_netlist", "check_boot_rom")
# the build record keys that are inputs of a bitstream (compared when a rebuilt one differs)
BIT_INPUTS = ("source_revision", "source_dirty", "build_id", "build_time", "sources_sha256",
              "defines", "generated_files_match", "constraints_sha256", "rom_sha256", "part")
# ... and in its [openxc7] table, the placer seed among them (tools/fpga-build.sh writes it there)
FLOW_INPUTS = ("image", "yosys", "nextpnr", "dsp_mode", "placer_seed")
# MANIFEST's format: 2 since the third GPU review (each design's placer_seed is the seed its bitstream was placed
# with, from its build record's [openxc7] table; before, every design said 1; and slack_margin, THIN or PASS)
MANIFEST_FORMAT = 2
# the tests' stand-ins for devenv/run.sh, tools/fpga-build.sh and docker: honoured only with SURFCPU_RELEASE_TEST=1
TEST_MODE = os.environ.get("SURFCPU_RELEASE_TEST") == "1"
HOOKS = ("SURFCPU_RELEASE_RUN", "SURFCPU_RELEASE_FPGA", "SURFCPU_RELEASE_DOCKER")
# the builds' environment: these names of the caller's, and what the release sets itself
PASS_ENV = ("PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "TERM", "DOCKER", "DOCKER_HOST", "DOCKER_CONFIG",
            "DOCKER_CONTEXT", "DOCKER_CERT_PATH", "DOCKER_TLS_VERIFY")
# set, these would change what a build makes or which image it uses: refused, never passed on
REFUSE_ENV = ("DRY_RUN", "SURFCPU_FPGA_OUT", "SURFCPU_IMAGE", "SURFCPU_OPENXC7_IMAGE", "SURFCPU_OPENXC7_BUILD",
              "SURFCPU_ROMCHECK", "ROM", "SURFCPU_BUILD_ID", "SURFCPU_BUILD_DIRTY", "PLACER_SEEDS")
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


def tree_changes(cwd: Path = REPO) -> list[str]:
    """what makes the checkout differ from its commit, by the repository's rules and not the user's git
    configuration: tracked files that changed (whatever status.showUntrackedFiles says), files git does not
    track that the repository's own .gitignore files do not ignore (a global excludes file or
    .git/info/exclude does not count: the release is the commit, and a file only they hide could still reach
    a build), and tracked files git was told not to look at (update-index --assume-unchanged or
    --skip-worktree), whose changes git status would not show"""
    out = git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=none", cwd=cwd).splitlines()
    out += [f"?? {p}" for p in git("ls-files", "--others", "--exclude-per-directory=.gitignore",
                                   cwd=cwd).splitlines() if p]
    for line in git("ls-files", "-v", cwd=cwd).splitlines():
        tag, _, path = line.partition(" ")
        if tag.islower() or tag == "S":
            out.append(f"{tag} {path}  (git update-index --{'skip-worktree' if tag == 'S' else 'assume-unchanged'}: "
                       "git status does not look at it)")
    return out


def utc(epoch: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


def minutes(seconds: float) -> str:
    return f"{seconds:.0f} s" if seconds < 120 else f"{seconds / 60:.1f} min"


def hook(name: str) -> str | None:
    """a test stand-in's path, only in the tests (SURFCPU_RELEASE_TEST=1)"""
    return os.environ.get(name) if TEST_MODE else None


def check_environment() -> None:
    """refuses the settings that would make the builds something other than the commit's"""
    hooks = [k for k in HOOKS if os.environ.get(k)]
    if hooks and not TEST_MODE:
        raise Refused(f"{', '.join(hooks)}: stand-ins for tools/release-test.py, honoured only with "
                      "SURFCPU_RELEASE_TEST=1; unset them")
    bad = [k for k in REFUSE_ENV if os.environ.get(k)]
    if bad:
        raise Refused("these settings would change what the builds make or which image they use, and a release "
                      "builds its commit with the images that commit names: unset "
                      + ", ".join(f"{k} (= {os.environ[k]})" for k in bad))


def clean_env(**extra) -> dict:
    """the builds' environment: PASS_ENV of the caller's (and, in the tests, the stand-ins' settings), and extra"""
    e = {k: v for k, v in os.environ.items() if k in PASS_ENV}
    if TEST_MODE:
        e.update({k: v for k, v in os.environ.items()
                  if k.startswith("FAKE_") or k in HOOKS or k == "SURFCPU_RELEASE_TEST"})
    e.update(extra)
    return e


def docker() -> str:
    d = hook("SURFCPU_RELEASE_DOCKER") or os.environ.get("DOCKER") or "docker"
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


def image_locale(tag: str) -> str:
    """what an image's own environment sets of the locale and the time zone (docker image inspect)"""
    p = subprocess.run([docker(), "image", "inspect", "--format", "{{json .Config.Env}}", tag], capture_output=True,
                       text=True)
    try:
        env = json.loads(p.stdout) if p.returncode == 0 else None
    except ValueError:
        env = None
    if not isinstance(env, list):
        return "unknown (docker image inspect gave no environment)"
    found = [e for e in env if e.split("=", 1)[0] in ("LANG", "LANGUAGE", "TZ") or e.startswith("LC_")]
    return ", ".join(found) if found else "no LANG, LC_* or TZ set (the POSIX locale, UTC)"


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
        allowed = design_boards(d["name"])
        for b in cfg["boards"]:
            if allowed is not None and b not in allowed:
                raise Refused(f"{path}: design {d['name']} builds for {', '.join(allowed)} only (its design list's "
                              f"board line), but every design is built for every board of the release, {b} too")
    return cfg


def design_boards(design: str) -> list[str] | None:
    """the boards a design's list restricts it to (its board lines, which add up), or None: every board;
    as tools/fpga-build.sh reads them (CRLF line ends taken, a board line naming no board refused)"""
    boards = None
    for line in (ROOT / "boards/arty_a7/designs" / f"{design}.files").read_text().splitlines():
        f = line.split("#", 1)[0].split()
        if f and f[0] == "board":
            if len(f) == 1:
                raise Refused(f"{design}: a board line in its design list names no board")
            boards = (boards or []) + f[1:]
    return boards


def pick(given: str | None, allowed: list[str], what: str, where: str = "release/release.txt") -> list[str]:
    if not given:
        return list(allowed)
    want = [x for x in re.split(r"[,\s]+", given) if x]
    if not want:
        raise Refused(f"no {what} named in {given!r}")
    for x in want:
        if x not in allowed:
            raise Refused(f"unknown {what} {x!r} ({where}: {', '.join(allowed)})")
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
    """rows (path, status, detail) for every artifact the reference's SHA256SUMS lists and every one only the new
    release lists, and notes. Both sides' files are hashed: a reference file that is not the one its SHA256SUMS
    lists, or a new one that is not its own SHA256SUMS's, is DAMAGED. A SHA256SUMS file as the reference is
    hashes only."""
    ref_dir = ref if ref.is_dir() else None
    ref_list = ref / "SHA256SUMS" if ref_dir else ref
    ref_sums = read_sums(ref_list) if ref_list.is_file() else {}
    new_sums = read_sums(new / "SHA256SUMS") if (new / "SHA256SUMS").is_file() else {}
    skipped = set()                         # designs the new release did not attempt (DESIGNS, BOARDS, SKIP_DDR)
    if rebuilt is None and (new / "MANIFEST").is_file():
        skipped = {(d.get("board"), d.get("name")) for d in parse_toml((new / "MANIFEST").read_text()).get("design", [])
                   if d.get("status") == "missing" and str(d.get("reason", "")).startswith("not built:")}
    rows = []
    for rel in sorted(ref_sums):
        want = ref_sums[rel]
        ra = ref_dir / rel if ref_dir else None
        if ra is not None:
            if not ra.is_file():
                rows.append((rel, "DAMAGED", "the reference lists it but has no such file"))
                continue
            have = sha256(ra)
            if have != want:
                rows.append((rel, "DAMAGED", f"the reference's file is not the one its SHA256SUMS lists (sha256 "
                                             f"{have[:16]}..., listed {want[:16]}...)"))
                continue
        sc = scope_of(rel)
        if sc is not None and ((rebuilt is not None and sc not in rebuilt) or sc in skipped):
            rows.append((rel, "not rebuilt", "its design was not attempted in the new build (DESIGNS, BOARDS or "
                                             "SKIP_DDR)"))
            continue
        f = new / rel
        if not f.is_file():
            rows.append((rel, "MISSING", "the new release has no such file" + (" (its design did not build: see "
                                                                               "its MANIFEST)" if sc else "")))
            continue
        got = sha256(f)
        if new_sums.get(rel, got) != got:
            rows.append((rel, "DAMAGED", f"the new release's file is not the one its SHA256SUMS lists"))
            continue
        if got == want:
            rows.append((rel, "match", want))
            continue
        if ra is None:
            rows.append((rel, "DIFFERS", f"sha256 {got}, the reference says {want} (no reference copy of the file "
                                         "to compare byte by byte)"))
            continue
        rows.append((rel, "DIFFERS", explain(rel, ra.read_bytes(), f.read_bytes(), ref_dir, new)))
    for rel in sorted(set(new_sums) - set(ref_sums)):
        rows.append((rel, "EXTRA", "only the new release lists it"))
    notes = []
    if ref_dir is None:
        notes.append(f"the reference is a list of SHA-256s ({ref}): its files were not checked")
    elif (ref_dir / "MANIFEST").is_file() and (new / "MANIFEST").is_file():
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


def report_lines(rows: list, notes: list[str], title: str, problems: list[str] = (), subset: bool = False) -> list[str]:
    """the report of a comparison; its last line is the verdict: PASS (every artifact rebuilt and the same),
    PARTIAL (every rebuilt one the same, but some were not rebuilt: never a pass by default; subset says the
    caller asked for only some designs or boards) or FAIL"""
    kinds = ("match", "DIFFERS", "MISSING", "DAMAGED", "EXTRA", "not rebuilt")
    n = {k: sum(1 for r in rows if r[1] == k) for k in kinds}
    lines = [title, ""]
    for p in problems:
        lines.append(f"  PROBLEM      {p}")
    if problems:
        lines.append("")
    for rel, status, detail in rows:
        if status == "match":
            lines.append(f"  match        {rel}  sha256 {detail[:16]}...")
        else:
            lines.append(f"  {status:<12} {rel}")
            lines.append(f"               {detail}")
    lines += [""] + [f"  note: {x}" for x in notes] + [""]
    compared = n["match"] + n["DIFFERS"] + n["MISSING"]
    bad = n["DIFFERS"] + n["MISSING"] + n["DAMAGED"] + n["EXTRA"] + len(problems)
    if bad or not compared:
        verdict = "FAIL"
    elif n["not rebuilt"]:
        verdict = "PARTIAL"
    else:
        verdict = "PASS"
    tail_ = (f"; {n['not rebuilt']} artifacts NOT verified (not rebuilt: "
             + ("DESIGNS/BOARDS asked for only some)" if subset else "the new release did not build their designs)")
             if n["not rebuilt"] else "")
    lines.append(f"{verdict}: {n['match']} of {compared} compared artifacts are byte for byte the same; "
                 f"{n['DIFFERS']} differ, {n['MISSING']} missing, {n['DAMAGED']} damaged, {n['EXTRA']} extra, "
                 f"{len(problems)} problems with the releases themselves" + tail_)
    return lines


def package_version(pkg: bytes) -> str:
    """a SURFOS.BIN package's image_version as x.y.z (tools/surfimg.py: major << 16 | minor << 8 | patch)"""
    if len(pkg) < 64:
        return "(none)"
    v = struct.unpack_from("<I", pkg, 40)[0]
    return f"{v >> 16}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def check_release(d: Path, name: str | None = None) -> tuple[list[str], list[str]]:
    """(problems, the artifacts found intact) of a release directory: its MANIFEST, its SHA256SUMS and its files
    agree with each other and with what make release writes. name is what the directory must be called
    (default: what it is called)."""
    problems: list[str] = []
    if not d.is_dir():
        return [f"{d}: no release directory there"], []
    for f in ("MANIFEST", "SHA256SUMS"):
        p = d / f
        if p.is_symlink() or not p.is_file() or p.stat().st_size == 0:
            problems.append(f"{f} is missing or empty")
    if problems:
        return problems, []
    try:
        sums = read_sums(d / "SHA256SUMS")
        man = parse_toml((d / "MANIFEST").read_text())
    except Exception as e:                                  # noqa: BLE001 (a damaged file, whatever its damage)
        return [f"cannot read MANIFEST or SHA256SUMS: {e}"], []
    version, commit, status, complete = man.get("version"), man.get("commit"), man.get("status"), man.get("complete")
    if not (isinstance(version, str) and re.fullmatch(r"\d+\.\d+\.\d+", version)):
        problems.append(f"MANIFEST: version {version!r} is not a version")
    if not (isinstance(commit, str) and re.fullmatch(r"[0-9a-f]{40}", commit)):
        problems.append(f"MANIFEST: commit {commit!r} is not a commit")
    if status not in ("complete", "prerelease") or complete is not (status == "complete"):
        problems.append(f"MANIFEST: status {status!r} and complete {complete!r} do not agree")
    if problems:
        return problems, []
    want = f"{version}{'' if complete else '-pre'}-{commit[:8]}"
    if man.get("release") != want:
        problems.append(f"MANIFEST: release {man.get('release')!r}, but a {status} release of {commit[:8]} is {want}")
    if (name or d.name) != want:
        problems.append(f"the directory is named {name or d.name}, but its MANIFEST makes it {want}")
    if (man.get("commit_short") != commit[:8] or man.get("tree_clean") is not True
            or not isinstance(man.get("source_date_epoch"), int)):
        problems.append("MANIFEST: commit_short, tree_clean or source_date_epoch is not what make release writes")
    # the artifacts: MANIFEST's list, SHA256SUMS and the files are the same, and every file is intact
    listed: dict = {}
    for x in man.get("artifact", []):
        if x.get("path") in listed:
            problems.append(f"MANIFEST lists {x.get('path')} twice")
        listed[x.get("path")] = x
    problems += [f"{rel}: in SHA256SUMS, not in MANIFEST" for rel in sorted(set(sums) - set(listed))]
    problems += [f"{rel}: in MANIFEST, not in SHA256SUMS" for rel in sorted(set(listed) - set(sums))]
    designs = man.get("design", [])
    built = {(x.get("board"), x.get("name")): x for x in designs if x.get("status") == "built"}
    records = {f"fpga/{b}/{n}/manifest.txt" for b, n in built}
    files = {p.relative_to(d).as_posix() for p in d.rglob("*") if p.is_file() or p.is_symlink()}
    problems += [f"{rel}: a file the release does not list (SHA256SUMS)"
                 for rel in sorted(files - set(sums) - records - {"MANIFEST", "SHA256SUMS"})]
    problems += [f"{rel}: the build record of a built design is missing" for rel in sorted(records - files)]
    intact = []
    for rel, h in sorted(sums.items()):
        p = d / rel
        if rel not in files or p.is_symlink():
            problems.append(f"{rel}: listed, but " + ("a symbolic link" if p.is_symlink() else "not there"))
            continue
        bad = []
        size = p.stat().st_size
        if size == 0:
            bad.append("empty")
        got = sha256(p)
        if got != h:
            bad.append(f"its sha256 is {got[:16]}..., SHA256SUMS says {h[:16]}...")
        x = listed.get(rel)
        if x is not None and (x.get("sha256") != h or x.get("bytes") != size):
            bad.append("MANIFEST gives another sha256 or size")
        if bad:
            problems.append(f"{rel}: " + "; ".join(bad))
        else:
            intact.append(rel)
    # what every release holds, and how its parts agree
    required = list(SOFTWARE) + ["card/BOOT/SURFOS.BIN"] + (["card/BOOT/CMDLINE.TXT"] if man.get("card_cmdline") else [])
    problems += [f"{rel}: every release has it, this one does not list it" for rel in required if rel not in sums]

    def data(rel: str) -> bytes | None:
        return (d / rel).read_bytes() if rel in intact else None
    for rel in ("SURFOS.BIN", "DDRDIAG.BIN"):
        b = data(rel)
        if b is not None and (len(b) < 64 or b[44:48] != bytes.fromhex(commit[:8])[::-1]):
            problems.append(f"{rel}: not built from {commit[:8]} (its build id)")
    if data("SURFOS.BIN") is not None and package_version(data("SURFOS.BIN")) != version:
        problems.append(f"SURFOS.BIN is version {package_version(data('SURFOS.BIN'))}, the release {version}")
    for key, kind in (("flash_boot_design", "boot"), ("flash_test_design", "test")):
        n = man.get(key, "")
        if n and not any(x.get("name") == n and x.get("flash") == kind and x.get("flash_image")
                         for x in built.values()):
            problems.append(f"MANIFEST: {key} {n!r}, but the release has no {kind} flash image of it")
    if data("card/BOOT/SURFOS.BIN") is not None and data("card/BOOT/SURFOS.BIN") != data("SURFOS.BIN"):
        problems.append("card/BOOT/SURFOS.BIN is not SURFOS.BIN")
    if man.get("card_cmdline") and data("card/BOOT/CMDLINE.TXT") not in (None, (man["card_cmdline"] + "\n").encode()):
        problems.append("card/BOOT/CMDLINE.TXT is not MANIFEST's card_cmdline")
    seen = set()
    for x in designs:
        key = (x.get("board"), x.get("name"))
        if key in seen:
            problems.append(f"MANIFEST lists {key[1]} for {key[0]} twice")
        seen.add(key)
        if x.get("status") not in ("built", "missing") or (x.get("status") == "missing" and not x.get("reason")):
            problems.append(f"MANIFEST: {key[1]} for {key[0]}: status {x.get('status')!r} without a reason")
    missing = [x for x in designs if x.get("status") != "built"]
    thin = [x for x in built.values() if x.get("slack_margin") == "THIN"]
    if complete == bool(missing or thin) or len(man.get("missing", [])) != len(missing) \
            or len(man.get("thin", [])) != len(thin):
        problems.append(f"MANIFEST: a {status} release, with {len(missing)} designs missing and "
                        f"{len(man.get('missing', []))} listed as missing, {len(thin)} built THIN and "
                        f"{len(man.get('thin', []))} listed as THIN")
    new_format = isinstance(man.get("manifest_format"), int) and man["manifest_format"] >= 2
    for (b, n), x in sorted(built.items()):
        bit, rec, flash = f"fpga/{b}/{n}/{n}.bit", f"fpga/{b}/{n}/manifest.txt", f"flash/{b}/{n}.bin"
        has_flash = x.get("flash", "-") != "-"
        if x.get("bitstream") != bit or bit not in sums:
            problems.append(f"{n} for {b} is built, but its bitstream {bit} is not listed")
            continue
        if has_flash != (flash in sums) or x.get("flash_image", "") != (flash if has_flash else ""):
            problems.append(f"{n} for {b}: " + (f"its flash image {flash} is not listed" if has_flash
                                                 else f"{flash} is listed, but the design has no flash image"))
        if rec not in files:
            continue
        try:
            m = parse_toml((d / rec).read_text())
        except Exception as e:                              # noqa: BLE001
            problems.append(f"{rec}: cannot be read ({e})")
            continue
        for k, v in (("result", "pass"), ("design", n), ("board", b), ("source_revision", commit),
                     ("source_dirty", False), ("build_time", man.get("source_date_epoch")),
                     ("bitstream_sha256", sums[bit])):
            if m.get(k) != v:
                problems.append(f"{rec}: {k} = {m.get(k)!r}, not {v!r}")
        if m.get("rom_sha256", "") != x.get("rom_sha256", ""):
            problems.append(f"{rec} and MANIFEST give {n} for {b} different boot ROMs")
        ox = m.get("openxc7", {})
        if new_format and (not isinstance(ox.get("placer_seed"), int) or ox.get("placer_seed") != x.get("placer_seed")):
            problems.append(f"{rec} says placer seed {ox.get('placer_seed')!r}, MANIFEST {x.get('placer_seed')!r}")
        if new_format and ox.get("slack_margin", "") != x.get("slack_margin", ""):
            problems.append(f"{rec} says slack margin {ox.get('slack_margin')!r}, MANIFEST {x.get('slack_margin')!r}")
        if m.get("rom_sha256"):
            rom = f"rom/{Path(str(m.get('rom_image', ''))).name}"
            if sums.get(rom) != m.get("rom_sha256"):
                problems.append(f"{bit} was built with a boot ROM that is not the release's {rom}")
        if flash in intact and bit in intact:
            try:
                if bitfile.flash_image(data(bit), b)[0] != data(flash):
                    problems.append(f"{flash} is not the configuration stream of {bit}")
            except bitfile.BitError as e:
                problems.append(f"{bit}: {e}")
    problems += [f"{rel}: belongs to no design the release built" for rel in sorted(sums)
                 if scope_of(rel) is not None and scope_of(rel) not in built]
    return problems, intact


# ============================================================================== make
WORK = ROOT / "build" / "release-work"              # the release's own build directories: nothing else writes there
LOCK = ROOT / "build" / "release-work.lock"         # one release at a time in a checkout
WORK_MARK = ".surfcpu-release-work"
STAGE_MARK = ".surfcpu-release-stage"
LOGS_MARK = ".surfcpu-release-logs"
VERIFY_MARK = ".surfcpu-release-verify"
# the software artifacts and where the release's own build makes them (relative to WORK)
SOFTWARE = {
    "SURFOS.BIN": "riscv32/arty_a7/SURFOS.BIN",
    "DDRDIAG.BIN": "fw/DDRDIAG.BIN",
    "rom/surfboot.mem": "fw/surfboot.mem",
    "rom/bramdiag.mem": "fw/bramdiag.mem",
    "debug/surfboot.elf": "fw/surfboot.elf",
    "debug/bramdiag.elf": "fw/bramdiag.elf",
    "debug/ddrdiag.elf": "fw/ddrdiag.elf",
    "debug/surfos.elf": "riscv32/arty_a7/surfos.elf",
}


def own_dir(d: Path, mark: str, what: str) -> Path:
    """d as a new, empty directory of this tool's: one it made before (it holds the mark) is emptied, a missing
    or empty one is made or used; one that holds anything else is refused and left as it is"""
    if d.is_symlink() or (d.exists() and not d.is_dir()):
        raise Refused(f"{d} is not a directory this tool made: not touching it")
    if d.is_dir():
        names = sorted(p.name for p in d.iterdir())
        if names and mark not in names:
            raise Refused(f"{d} holds files this tool did not write ({', '.join(names[:3])}"
                          f"{', ...' if len(names) > 3 else ''}): not deleting them; move them away, or name "
                          f"another {what} directory")
        if names:
            shutil.rmtree(d)
    d.mkdir(parents=True, exist_ok=True)
    (d / mark).write_text(f"made by tools/release.py ({what}); it empties this directory when it runs again\n")
    return d


def foreign_file(d: Path) -> str | None:
    """None if d holds only what a release holds (MANIFEST, SHA256SUMS, the artifacts it lists and their
    build records), else the first other thing in it: FORCE replaces nothing but a release"""
    try:
        sums = read_sums(d / "SHA256SUMS")
    except (OSError, Refused):
        return "no readable SHA256SUMS"
    if not (d / "MANIFEST").is_file():
        return "no MANIFEST"
    allowed = {"MANIFEST", "SHA256SUMS"} | set(sums)
    allowed |= {f"{Path(rel).parent.as_posix()}/manifest.txt" for rel in sums
                if rel.startswith("fpga/") and rel.endswith(".bit")}
    for p in sorted(d.rglob("*")):
        rel = p.relative_to(d).as_posix()
        if p.is_symlink() or (p.is_file() and rel not in allowed):
            return rel
    return None


def drop_own_dir(d: Path, mark: str) -> None:
    """d removed if it is this tool's (it holds the mark)"""
    if d.is_dir() and not d.is_symlink() and (d / mark).is_file():
        shutil.rmtree(d, ignore_errors=True)


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


class Lock:
    """LOCK held while a release runs: a second release in the same checkout would share its build directories"""

    def __init__(self, path: Path):
        self.path = path
        self.held = False

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(2):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
            except FileExistsError:
                held = self.path.read_text(errors="replace").split()
                pid = int(held[0]) if held and held[0].isdigit() else None
                if pid is None or pid_alive(pid):
                    raise Refused(f"another release is running in this checkout ({self.path}: "
                                  f"{' '.join(held) or 'empty'}); one at a time, since they share "
                                  f"{WORK.relative_to(ROOT)}/ (if none is running, remove the lock)")
                say(f"removing a stale lock: process {pid} has ended ({self.path})")
                self.path.unlink()
                continue
            with os.fdopen(fd, "w") as f:
                f.write(f"{os.getpid()} {utc(int(time.time()))}\n")
            self.held = True
            return self
        raise Refused(f"cannot take the lock {self.path}")

    def __exit__(self, *exc) -> bool:
        if self.held:
            try:
                if self.path.read_text().split()[:1] == [str(os.getpid())]:
                    self.path.unlink()
            except OSError:
                pass
        return False


def design_rom(design: str) -> str | None:
    """the boot ROM image a design is built with (the file name of its design list's rom line), or None"""
    generics, rom = [], None
    for line in (ROOT / "boards/arty_a7/designs" / f"{design}.files").read_text().splitlines():
        f = line.split("#", 1)[0].split()
        if f and f[0] == "generic":
            generics += f[1:]
        elif f and f[0] == "rom" and len(f) >= 2:
            rom = Path(f[1]).name
    if "BOOTROM_INIT" not in generics or rom is None:
        return None
    if f"rom/{rom}" not in SOFTWARE:
        raise Refused(f"{design} is built with the boot ROM {rom}, which the release does not build")
    return rom


def generated_changed(record: dict) -> list[str]:
    """the files of a build record's generated lists (the LiteDRAM core) whose SHA-256 is no longer the list's"""
    out = []
    for name in record.get("generated_lists") or []:
        lst = ROOT / name
        if not lst.is_file():
            out.append(f"{name} (the list itself is gone)")
            continue
        for line in lst.read_text().splitlines():
            f = line.split()
            if len(f) >= 2 and re.fullmatch(r"[0-9a-f]{64}", f[0]):
                p = ROOT / f[1].lstrip("*")
                if not p.is_file() or sha256(p) != f[0]:
                    out.append(f[1].lstrip("*"))
    return out


class Release:
    def __init__(self, a):
        self.a = a
        self.cfg = read_list()
        self.t0 = time.time()
        self.shipped: dict = {}         # artifact: the SHA-256 of the bytes that were checked and staged
        self.thin: list = []            # (board, design, note): built, but THIN (the open flow's slack rule)
        self.records: dict = {}         # a bitstream's build record (next to it; not an artifact): its SHA-256

    # ---------------------------------------------------------------- preflight
    def preflight(self) -> None:
        a, cfg = self.a, self.cfg
        check_environment()
        self.commit = git("rev-parse", "HEAD")
        self.short = self.commit[:8]
        self.epoch = int(git("log", "-1", "--format=%ct", "HEAD"))
        dirty = tree_changes()
        if dirty:
            raise Refused("the tree is not clean, so the release would not be the commit (commit or remove "
                          "these first; an untracked file counts unless the repository's own .gitignore files "
                          "ignore it):\n" + "\n".join("    " + x for x in dirty[:20])
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
            if not self.only:
                raise Refused("--only names no board/design pair")
            for b, n in self.only:
                if b not in cfg["boards"] or n not in names:
                    raise Refused(f"--only {b}/{n}: no such board and design in release/release.txt")
        self.pins = {}
        for x in a.seed or []:                         # release-verify: each bitstream's recorded placer seed
            mm = re.fullmatch(r"([\w.-]+)/([\w.-]+)=(\d+)", x)
            if not mm:
                raise Refused(f"--seed {x}: not BOARD/DESIGN=SEED")
            if mm.group(1) not in cfg["boards"] or mm.group(2) not in names:
                raise Refused(f"--seed {x}: no such board and design in release/release.txt")
            self.pins[(mm.group(1), mm.group(2))] = int(mm.group(3))
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
        return clean_env(SOURCE_DATE_EPOCH=str(self.epoch), LC_ALL="C", TZ="UTC", **extra)

    def ship(self, rel: str, data: bytes, record: bool = False) -> str:
        """data as the release's file rel, at once: written to the stage and read back. Its SHA-256 is what the
        end of the release checks the stage against: what is shipped is what was checked"""
        p = self.stage / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        h = hashlib.sha256(data).hexdigest()
        if sha256(p) != h:
            raise Refused(f"{rel}: the copy in {self.stage} is not the bytes that were checked")
        (self.records if record else self.shipped)[rel] = h
        return h

    # ---------------------------------------------------------------- steps
    def software(self) -> None:
        """the boot ROMs, the diagnostics and the kernel, built in the release's own directory (make build-rom
        BUILD=..., arch/riscv32/kernel.mk OUT=...), checked and staged"""
        run = hook("SURFCPU_RELEASE_RUN") or str(ROOT / "devenv" / "run.sh")
        log = self.logs / "software.txt"
        t = time.time()
        steps = (("make build-rom", [run, "make", "--no-print-directory", "build-rom",
                                     f"BUILD={self.work.relative_to(ROOT).as_posix()}"]),
                 ("the kernel (arch/riscv32/kernel.mk KBOARD=arty_a7)",
                  [run, "make", "--no-print-directory", "-C", "..", "-f", "arch/riscv32/kernel.mk", "KBOARD=arty_a7",
                   f"OUT={(self.work / SOFTWARE['SURFOS.BIN']).parent.relative_to(REPO).as_posix()}"]))
        for what, cmd in steps:
            rc = run_logged(cmd, log, self.env())
            if rc != 0:
                raise Refused(f"{what} failed (exit {rc}); {log}:\n{tail(log)}")
        data = {}
        for rel, sub in SOFTWARE.items():
            p = self.work / sub
            if not p.is_file():
                raise Refused(f"the software build made no {p}")
            data[rel] = p.read_bytes()
        for rel in ("SURFOS.BIN", "DDRDIAG.BIN"):
            bid = f"{struct.unpack_from('<I', data[rel], 44)[0]:08x}" if len(data[rel]) >= 64 else "(none)"
            if bid != self.short:
                raise Refused(f"{rel} carries build id {bid}, not the commit's {self.short}")
        kv = package_version(data["SURFOS.BIN"])
        if kv != self.version:
            raise Refused(f"SURFOS.BIN is version {kv} (arch/riscv32/kernel.mk packs it with --version), but "
                          f"release/release.txt says {self.version}: make the two agree")
        gen = self.work / "fw" / "gen" / "fw_build.h"
        if not gen.is_file() or not re.search(r"FW_BUILD_DIRTY 0\b", gen.read_text()):
            raise Refused(f"the firmware was built as a dirty tree ({gen})")
        for rel, b in data.items():
            self.ship(rel, b)
        self.package = data["SURFOS.BIN"]
        say(f"software: SurfBoot, the BRAM diagnostics, DDRDIAG.BIN and SURFOS.BIN, build {self.short} "
            f"({minutes(time.time() - t)})")

    def litedram(self) -> str | None:
        """make litedram-gen once for the DDR designs: None, or why it failed"""
        if getattr(self, "_litedram", "unset") != "unset":
            return self._litedram
        run = hook("SURFCPU_RELEASE_RUN") or str(ROOT / "devenv" / "run.sh")
        log = self.logs / "litedram-gen.txt"
        t = time.time()
        rc = run_logged([run, "make", "--no-print-directory", "litedram-gen"], log, self.env())
        self._litedram = None
        if rc != 0:
            text = log.read_text(errors="replace")
            differs = re.findall(r"lock\.sh: DIFFERS\s+(\S+)", text)
            if differs:
                self._litedram = (f"make litedram-gen: the generated {', '.join(differs)} differs from ip/litedram/arty.sha256 "
                                  "(its lock check; docs/RELEASE.md section 4)")
            else:
                last = [x for x in text.splitlines() if x.strip()][-1:] or [""]
                self._litedram = f"make litedram-gen failed (exit {rc}): {last[0].strip()}"
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
        """one design for one board, built in the release's own directory from the staged boot ROM; when its
        build record says pass for this commit, ROM and kernel and describes the bitstream, the bitstream (and
        its flash image) are staged at once"""
        design = d["name"]
        fpga = hook("SURFCPU_RELEASE_FPGA") or str(ROOT / "tools" / "fpga-build.sh")
        out = self.work / "fpga"
        base = out / board / design
        shutil.rmtree(base, ignore_errors=True)
        log = self.logs / f"fpga-{board}-{design}.txt"
        rom = design_rom(design)
        env = self.env(FLOW="openxc7", SURFCPU_FPGA_OUT=str(out),
                       SURFCPU_KERNEL=str(self.work / SOFTWARE["SURFOS.BIN"]))
        if rom:
            env["ROM"] = str(self.work / "fw" / rom)
        if (board, design) in self.pins:               # release-verify: the seed the release's bitstream was placed with
            env["PLACER_SEEDS"] = str(self.pins[(board, design)])
        self.wait_slot()
        started = time.time()
        rc = run_logged([fpga, "--design", design, board], log, env)
        rec = base / "manifest.txt"
        rec_bytes = rec.read_bytes() if rec.is_file() else None
        m = parse_toml(rec_bytes.decode(errors="replace")) if rec_bytes else {}
        bit_path = base / f"{design}.bit"
        bit = bit_path.read_bytes() if bit_path.is_file() else None
        problems = []
        if rec_bytes is None:
            problems.append(f"no build record from this build (tools/fpga-build.sh exit status {rc})")
        elif rc != 0:
            problems.append(m.get("reason") or f"tools/fpga-build.sh exit status {rc}")
        elif m.get("result") != "pass":
            problems.append(m.get("reason") or "its build record does not say pass")
        else:
            checks = {"source_revision": self.commit, "source_dirty": False, "build_time": self.epoch,
                      "generated_files_match": True, "board": board, "design": design,
                      "rom_sha256": self.shipped[f"rom/{rom}"] if rom else "",
                      "kernel_sha256": self.shipped["SURFOS.BIN"]}
            problems += [f"build record {k} = {m.get(k)!r}, not {v!r}" for k, v in checks.items() if m.get(k) != v]
            rid = str(m.get("run_id", ""))
            mt = re.fullmatch(rf"{re.escape(design)}-{re.escape(board)}-{self.short}-(\d{{8}}T\d{{6}}Z)", rid)
            when = calendar.timegm(time.strptime(mt.group(1), "%Y%m%dT%H%M%SZ")) if mt else None
            if when is None or not started - 5 <= when <= time.time() + 5:
                problems.append(f"build record run_id {rid!r}: not from this build (started {utc(int(started))})")
            if m.get("openxc7", {}).get("image") != self.ox_image:
                problems.append(f"built in the image {m.get('openxc7', {}).get('image')!r}, not {self.ox_image}")
            seed = m.get("openxc7", {}).get("placer_seed")
            if not isinstance(seed, int):
                problems.append("its build record names no placer seed ([openxc7] placer_seed)")
            elif (board, design) in self.pins and seed != self.pins[(board, design)]:
                problems.append(f"placed with seed {seed}, not the seed {self.pins[(board, design)]} it was given")
            if m.get("openxc7", {}).get("slack_margin") not in ("PASS", "THIN"):
                problems.append(f"its build record's slack margin is {m.get('openxc7', {}).get('slack_margin')!r}, "
                                "not PASS or THIN (tools/openxc7-build.sh's slack rule)")
            if bit is None or hashlib.sha256(bit).hexdigest() != m.get("bitstream_sha256"):
                problems.append("the bitstream is not the one its build record describes")
            changed = generated_changed(m)
            if changed:
                problems.append("generated files changed during the build (make litedram-gen ran meanwhile?): "
                                + ", ".join(changed[:5]))
        if problems:
            return False, m, "; ".join(problems)
        flash = None
        if d["flash"]:
            try:
                flash = bitfile.flash_image(bit, board)
            except bitfile.BitError as e:
                return False, m, f"no flash image can be made from it: {e}"
        self.ship(f"fpga/{board}/{design}/{design}.bit", bit)
        self.ship(f"fpga/{board}/{design}/manifest.txt", rec_bytes, record=True)
        if flash:
            self.ship(f"flash/{board}/{design}.bin", flash[0])
            m["flash_map"] = bitfile.describe_map(flash[1])[1:]
        return True, m, ""

    # ---------------------------------------------------------------- assembling
    def card(self) -> None:
        """the card's /BOOT files as tools/deploy-sd.py installs them on a new card, from the checked SURFOS.BIN,
        run against a directory standing in for the card (its tests' stand-ins for diskutil and mount), in a
        temporary directory: deploy-sd's test mode refuses anything under /Volumes, where a checkout may be"""
        work = Path(tempfile.mkdtemp(prefix="surfcpu-release-card-")).resolve()   # macOS: /var is /private/var
        try:
            self._card(work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    def _card(self, work: Path) -> None:
        view, device = work / "card", work / "device"
        view.mkdir(parents=True)
        package = work / "SURFOS.BIN"
        package.write_bytes(self.package)
        cfg = work / "fake.json"
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
        for p in sorted((device / "BOOT").iterdir()):
            if p.is_file():
                self.ship(f"card/BOOT/{p.name}", p.read_bytes())
        if self.shipped.get("card/BOOT/SURFOS.BIN") != hashlib.sha256(self.package).hexdigest():
            raise Refused("deploy-sd installed other bytes than SURFOS.BIN")

    def final_check(self) -> None:
        """the last step before the release is written: it is still the commit it started from, built with the
        images it started with, every file in the stage is the one that was checked, and the artifacts agree
        with each other (the build ids, and the ROMs and kernel each bitstream was built with)"""
        head = git("rev-parse", "HEAD")
        if head != self.commit:
            raise Refused(f"HEAD moved during the release ({self.short} -> {head[:8]}): its artifacts would not "
                          "all be one commit's; nothing was written")
        changed = tree_changes()
        if changed:
            raise Refused("the tree changed during the release: " + changed[0])
        for tag, iid in ((self.tc_image, self.tc_id), (self.ox_image, self.ox_id)):
            now = image_id(tag)
            if now != iid:
                raise Refused(f"the image {tag} changed during the release ({iid[:19]} -> {(now or 'gone')[:19]})")
        want = dict(self.shipped, **self.records)
        found = {p.relative_to(self.stage).as_posix() for p in self.stage.rglob("*")
                 if p.is_file() or p.is_symlink()} - {STAGE_MARK}
        for rel in sorted(found - set(want)):
            raise Refused(f"{rel} appeared in {self.stage} during the release, and was never checked")
        for rel, h in sorted(want.items()):
            p = self.stage / rel
            if p.is_symlink() or not p.is_file() or sha256(p) != h:
                raise Refused(f"{rel} is not the file that was checked: it changed in {self.stage} during the release")
        for rel in ("SURFOS.BIN", "DDRDIAG.BIN"):
            if (self.stage / rel).read_bytes()[44:48] != bytes.fromhex(self.short)[::-1]:
                raise Refused(f"{rel}: not the build id of {self.short}")
        for rec in sorted(self.records):
            m = parse_toml((self.stage / rec).read_text())
            bit = f"{rec.rsplit('/', 1)[0]}/{m.get('design')}.bit"
            rom = Path(str(m.get("rom_image", ""))).name
            if m.get("bitstream_sha256") != self.shipped.get(bit):
                raise Refused(f"{rec} does not describe {bit}")
            if m.get("rom_sha256") and m.get("rom_sha256") != self.shipped.get(f"rom/{rom}"):
                raise Refused(f"{bit} was built with a boot ROM other than the release's rom/{rom}")
            if m.get("kernel_sha256") != self.shipped["SURFOS.BIN"]:
                raise Refused(f"{rec} names a kernel other than the release's SURFOS.BIN")

    def make(self) -> int:
        a = self.a
        self.preflight()
        name = self.name(a.prerelease)
        out_root = self.out_root or ROOT / "build" / "release"
        out = out_root / name
        self.replaceable(out)
        with Lock(LOCK):
            self.logs = own_dir(self.logs or out.parent / f"{out.name}.logs", LOGS_MARK, "release logs")
            self.work = own_dir(WORK, WORK_MARK, "release build")
            self.stage = own_dir(out.parent / f".{out.name}.stage", STAGE_MARK, "release stage")
            try:
                return self._make(name, out, out_root)
            finally:
                drop_own_dir(self.stage, STAGE_MARK)            # a release that was not written

    def replaceable(self, out: Path) -> None:
        """refuses an existing out unless FORCE=1 and it is a release (nothing in it but what a release holds)"""
        if not out.exists() and not out.is_symlink():
            return
        if not self.a.force:
            raise Refused(f"{out} exists (FORCE=1 replaces it)")
        what = "a symbolic link" if out.is_symlink() else None if not out.is_dir() else foreign_file(out)
        if out.is_symlink() or not out.is_dir() or what:
            raise Refused(f"{out} exists but is not a release this tool wrote ({what or 'not a directory'}): not "
                          "replacing it, FORCE=1 or not")

    def _make(self, name: str, out: Path, out_root: Path) -> int:
        a = self.a
        say(f"SurfCPU {self.version} at {self.short} (commit time {utc(self.epoch)} = SOURCE_DATE_EPOCH "
            f"{self.epoch}), clean tree")
        say(f"images: {self.tc_image} ({self.tc_id[:19]}), {self.ox_image} ({self.ox_id[:19]})")
        say(f"plan: {len(self.plan)} bitstreams" + (f"; {len(self.missing)} not attempted" if self.missing else "")
            + f"; built in {self.work}, staged in {self.stage}; logs in {self.logs}")
        steps = 1 + len(self.plan)
        say(f"[1/{steps}] the software (make build-rom, the kernel)")
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
                ox = m.get("openxc7", {})
                say(f"    PASS ({minutes(time.time() - t)}): seed {ox.get('placer_seed')}, wns {ox.get('wns_ns')} ns, "
                    f"slack margin {ox.get('slack_margin')}, sha256 {m.get('bitstream_sha256', '')[:16]}...")
                if ox.get("slack_margin") == "THIN":
                    self.thin.append((board, d, ox.get("slack_margin_note", "")))
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
        if self.thin and not a.prerelease:
            raise Refused("the release was not written: these bitstreams met timing with less slack than the open "
                          "flow's rule wants (THIN: every placer seed of their design lists was thin):\n" + "\n".join(
                              f"    {d['name']} for {b}: {note}" for b, d, note in self.thin)
                          + "\nmake release PRERELEASE=1 writes it as a pre-release with them (docs/OPEN-FLOW.md "
                            "section 3)")
        prerelease = bool(self.missing or self.thin)
        if prerelease != a.prerelease:                 # every design built: a complete release
            name = self.name(prerelease)
            out = out_root / name
            self.replaceable(out)
        self.card()
        designs = []
        for board, d, m in built:
            ox = m.get("openxc7", {})
            rel = f"fpga/{board}/{d['name']}/{d['name']}.bit"
            designs.append({
                "name": d["name"], "board": board, "status": "built", "flash": d["flash"] or "-", "needs_ddr": d["ddr"],
                "bitstream": rel, "flash_image": f"flash/{board}/{d['name']}.bin" if d["flash"] else "",
                "build_record": f"fpga/{board}/{d['name']}/manifest.txt",
                "part": m.get("part", ""), "idcode": ox.get("idcode", ""), "rom_sha256": m.get("rom_sha256", ""),
                "placer_seed": ox.get("placer_seed"),
                "slack_margin": ox.get("slack_margin", ""), "slack_margin_note": ox.get("slack_margin_note", ""),
                "gates": [f"{g[6:]} {ox.get(g)}" for g in GATES if g in ox],
                "timing": ox.get("timing", []), "wns_ns": ox.get("wns_ns", ""),
                "use": [f"{k} {ox.get(k)}" for k in ("lut", "ff", "bram36", "dsp") if k in ox],
                "constraints_not_checked": len(ox.get("constraints_not_checked", [])),
            })
            if d["flash"]:
                designs[-1]["flash_map"] = m.get("flash_map", [])
        for b, d, why in self.missing:
            designs.append({"name": d["name"], "board": b, "status": "missing", "flash": d["flash"] or "-",
                            "needs_ddr": d["ddr"], "reason": why})
        self.final_check()
        sums = dict(sorted(self.shipped.items()))
        (self.stage / "SHA256SUMS").write_text("".join(f"{h}  {rel}\n" for rel, h in sums.items()))
        self.write_manifest(self.stage / "MANIFEST", name, prerelease, sums, designs)
        (self.stage / STAGE_MARK).unlink()
        problems, _ = check_release(self.stage, name)
        if problems:
            (self.stage / STAGE_MARK).write_text("made by tools/release.py (release stage)\n")
            raise Refused("the release does not pass its own check (tools/release.py check), so it was not "
                          "written:\n" + "\n".join("    " + p for p in problems[:20]))
        if out.exists():
            self.replaceable(out)                       # still nothing in it but a release
            shutil.rmtree(out)
        os.rename(self.stage, out)
        say(f"written {out}: " + ("PRE-RELEASE, without:" if prerelease else "complete release") +
            "".join(f"\n    {d['name']} for {b}: {why}" for b, d, why in self.missing) +
            "".join(f"\n    {d['name']} for {b}: THIN ({note})" for b, d, note in self.thin))
        say(f"{len(sums)} artifacts in SHA256SUMS (check: cd {out} && shasum -a 256 -c SHA256SUMS); "
            f"{minutes(time.time() - self.t0)} in all")
        return 0

    def write_manifest(self, path: Path, name: str, prerelease: bool, sums: dict, designs: list) -> None:
        top = {
            "release": name, "version": self.version, "status": "prerelease" if prerelease else "complete",
            "complete": not prerelease,
            "missing": [f"{d['name']} for {b}: {why}" for b, d, why in self.missing],
            "thin": [f"{d['name']} for {b}: {note}" for b, d, note in self.thin], "manifest_format": MANIFEST_FORMAT,
            "commit": self.commit, "commit_short": self.short, "source_date_epoch": self.epoch,
            "commit_time_utc": utc(self.epoch), "tree_clean": True,
            # the designs whose flash image this release holds (for some board): none when it was not built
            "flash_boot_design": next((d["name"] for d in designs if d["status"] == "built" and d["flash"] == "boot"), ""),
            "flash_test_design": next((d["name"] for d in designs if d["status"] == "built" and d["flash"] == "test"), ""),
            "card_cmdline": self.cfg["card_cmdline"] or "",
        }
        tools = {
            "toolchain_image": self.tc_image, "toolchain_image_id": self.tc_id,
            "toolchain_versions": image_versions(self.tc_image, "/etc/surfcpu-toolchain.txt"),
            "openxc7_image": self.ox_image, "openxc7_image_id": self.ox_id,
            "openxc7_versions": image_versions(self.ox_image, "/etc/surfcpu-openxc7.txt"),
            "flow": "openxc7 (tools/fpga-build.sh FLOW=openxc7, every gate of docs/BOARD.md section 8)",
            "nextpnr_threads": nextpnr_threads(),
            "locale": ("LC_ALL=C TZ=UTC for tools/release.py and the scripts it runs on the Mac; the containers get "
                       "only SOURCE_DATE_EPOCH from them (devenv/run.sh) and run with their images' environment: "
                       f"{self.tc_image}: {image_locale(self.tc_image)}; {self.ox_image}: {image_locale(self.ox_image)}"),
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
    check_environment()
    ref = Path(a.release).resolve()
    if not (ref / "MANIFEST").is_file() or not (ref / "SHA256SUMS").is_file():
        raise Refused(f"no release at {ref} (no MANIFEST and SHA256SUMS)")
    problems, _ = check_release(ref)
    if problems:
        raise Refused(f"{ref} does not pass tools/release.py check, so there is nothing sound to compare with:\n"
                      + "\n".join("    " + p for p in problems[:20]))
    man = parse_toml((ref / "MANIFEST").read_text())
    commit = man.get("commit", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise Refused(f"{ref}/MANIFEST names no commit")
    try:
        git("cat-file", "-e", commit + "^{commit}")
    except Refused:
        raise Refused(f"the commit {commit} is not in this repository")
    entries = man.get("design", [])
    built = [(d["board"], d["name"]) for d in entries if d.get("status") == "built"]
    where = f"{ref.name}'s MANIFEST"
    boards = pick(a.boards, sorted({d.get("board") for d in entries}), "board", where)
    names = pick(a.designs, list(dict.fromkeys(d.get("name") for d in entries)), "design", where)
    for n in names if a.designs else []:
        if not any((b, n) in built for b in boards):
            why = "; ".join(f"{d.get('board')}: {d.get('reason', '')}" for d in entries
                            if d.get("name") == n and d.get("board") in boards)
            raise Refused(f"no bitstream of {n} in {ref.name} for {', '.join(boards)} to rebuild ({why})")
    for b in boards if a.boards else []:
        if not any((b, n) in built for n in names):
            raise Refused(f"no bitstream for {b} among {', '.join(names)} in {ref.name} to rebuild")
    rebuilt = {(b, n) for b, n in built if b in boards and n in names}
    if not rebuilt:
        raise Refused(f"nothing of {ref.name} to rebuild")
    subset = len(rebuilt) < len(built)
    work = Path(a.work).resolve() if a.work else ROOT / "build" / "release-verify" / f"{ref.name}-{time.strftime('%Y%m%dT%H%M%S')}"
    made = not work.exists()                            # a directory the user gave is kept (emptied of ours)
    own_dir(work, VERIFY_MARK, "release-verify work")
    src = work / "src"
    t0 = time.time()
    say(f"verify {ref.name}: a pristine copy of {commit[:8]} in {src} (git clone --shared)")
    subprocess.run(["git", "clone", "-q", "--shared", "--no-checkout", str(REPO), str(src)], check=True)
    subprocess.run(["git", "-C", str(src), "checkout", "-q", "--detach", commit], check=True)
    st = tree_changes(src)
    if st:
        raise Refused(f"the pristine copy is not clean: {st[0]}")
    rebuild = work / "rebuild"
    cmd = [sys.executable, str(src / "surfcpu" / "tools" / "release.py"), "make", "--prerelease", "--out",
           str(rebuild), "--logs", str(work / "logs")]
    cmd += ["--only", ",".join(f"{b}/{n}" for b, n in sorted(rebuilt))]
    seed_note = None
    if isinstance(man.get("manifest_format"), int) and man["manifest_format"] >= 2:
        for d in entries:                               # the seed each bitstream was placed with, and only that one
            if d.get("status") == "built" and (d.get("board"), d.get("name")) in rebuilt:
                cmd += ["--seed", f"{d['board']}/{d['name']}={d['placer_seed']}"]
    else:
        seed_note = ("this MANIFEST predates format 2: its placer_seed fields say 1 for every design whatever seed "
                     "was used (tools/release.py read a key fpga-build.sh does not write at the top of a build "
                     "record), so the rebuild takes each design list's seeds, as the release did"
                     + ("; its arty_a7_100t surfcpu_arty_a7_ddr_bramdiag was placed with seed 3"
                        if man.get("release") == "1.0.0-f791b8a1" else ""))
    say(f"rebuilding there: {len(rebuilt)} of the release's {len(built)} bitstreams and every other artifact")
    rc = subprocess.run(cmd, cwd=str(src / "surfcpu"), env=clean_env()).returncode
    say(f"the rebuild ended with exit status {rc} after {minutes(time.time() - t0)}")
    outs = sorted(p for p in rebuild.iterdir() if (p / "MANIFEST").is_file()) if rebuild.is_dir() else []
    if len(outs) == 1:
        out = outs[0]
        rows, notes = compare(ref, out, {(b, n) for b, n in rebuilt})
        notes = notes + ([seed_note] if seed_note else [])
        problems = [f"the rebuild: {p}" for p in check_release(out)[0]]
    else:
        rows, notes = [(rel, "MISSING", "the rebuild wrote no release") for rel in sorted(read_sums(ref / "SHA256SUMS"))], []
        problems = []
    lines = report_lines(rows, notes, f"release-verify {ref.name}: commit {commit}, rebuilt in {src} "
                                      f"({minutes(time.time() - t0)})", problems, subset)
    text = "\n".join(lines) + "\n"
    (work / "verify-report.txt").write_text(text)
    if a.report:
        Path(a.report).write_text(text)
    if not a.keep:
        if made:
            shutil.rmtree(work, ignore_errors=True)
        else:
            for x in ("src", "rebuild", "logs", "verify-report.txt", VERIFY_MARK):
                p = work / x
                if p.is_dir() and not p.is_symlink():
                    shutil.rmtree(p, ignore_errors=True)
                elif p.exists() or p.is_symlink():
                    p.unlink()
        say(f"removed {work if made else 'what it wrote in ' + str(work)} (KEEP=1 keeps it)")
    print(text, end="")
    verdict = lines[-1].split(":", 1)[0]
    return 0 if verdict == "PASS" or (verdict == "PARTIAL" and subset) else 1


def cmd_compare(a) -> int:
    ref, new = Path(a.reference).resolve(), Path(a.new).resolve()
    if not ref.exists():
        raise Refused(f"no reference {ref}")
    problems = [f"the reference: {p}" for p in check_release(ref)[0]] if ref.is_dir() else []
    problems += [f"the new release: {p}" for p in check_release(new)[0]]
    rows, notes = compare(ref, new)
    lines = report_lines(rows, notes, f"compare {ref} with {new}", problems)
    text = "\n".join(lines) + "\n"
    if a.report:
        Path(a.report).write_text(text)
    print(text, end="")
    return 0 if lines[-1].startswith("PASS") else 1


def cmd_check(a) -> int:
    d = Path(a.release).resolve()
    problems, intact = check_release(d)
    for rel in intact:
        print(f"ok   {rel}")
    for p in problems:
        print(f"FAIL {p}")
    if (d / "MANIFEST").is_file() and not problems:
        man = parse_toml((d / "MANIFEST").read_text())
        print(f"{d.name}: {man.get('status')}" + ("" if man.get("complete") else
                                                  " (missing: " + "; ".join(man.get("missing", [])) + ")"))
    print(f"check {d}: " + (f"PASS: {len(intact)} artifacts, every one listed, intact and consistent" if not problems
                            else f"FAIL: {len(problems)} problems"))
    return 1 if problems else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SurfCPU release (docs/RELEASE.md)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make", help="build the release from a clean tree")
    p.add_argument("--prerelease", action="store_true", help="allow a pre-release without the DDR designs")
    p.add_argument("--skip-ddr", action="store_true", help="do not attempt the DDR designs (implies a pre-release)")
    p.add_argument("--designs", help="only these designs (comma-separated; a pre-release)")
    p.add_argument("--boards", help="only these boards (comma-separated; a pre-release)")
    p.add_argument("--out", help="the directory for the release directory (default build/release)")
    p.add_argument("--logs", help="where the build logs go (default next to the release): a new or empty "
                                  "directory, or one this tool made before")
    p.add_argument("--force", action="store_true", help="replace an existing release directory (only a release)")
    p.add_argument("--only", help="exactly these BOARD/DESIGN pairs (comma-separated; release-verify uses it)")
    p.add_argument("--seed", action="append", metavar="BOARD/DESIGN=SEED",
                   help="place that bitstream with this seed only (repeatable; release-verify gives each the seed "
                        "its release recorded)")
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
