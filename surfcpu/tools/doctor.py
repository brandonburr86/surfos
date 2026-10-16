#!/usr/bin/env python3
"""make doctor: check the MacBook, the toolchain container, the board and the card.

Only reads: it never programs the FPGA, never writes to a card, never logs in anywhere.
Prints a report (or JSON with --json). Exit status 0 when everything needed to build and
simulate is present; 1 otherwise. Missing board, card or build host are reported as
"pending", not failures: simulation work does not need them.
"""

from __future__ import annotations

import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent           # surfcpu/
FTDI_VID, FTDI_PID = 0x0403, 0x6010                     # the Arty's FT2232HQ (JTAG + UART)
IDCODES = {                                             # JTAG IDCODE without the version nibble
    0x0362D093: "xc7a35t",
    0x0362C093: "xc7a50t",
    0x13631093 & 0x0FFFFFFF: "xc7a100t",
}
BUILD_HOST_CONF = Path.home() / ".config" / "surfcpu" / "build-host.conf"


def run(cmd: list[str], timeout: float = 20) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).strip()
    except FileNotFoundError:
        return 127, f"{cmd[0]}: not found"
    except subprocess.TimeoutExpired:
        return 124, f"{cmd[0]}: timed out after {timeout:.0f} s"


def image_tag() -> str:
    m = re.search(r"^IMAGE_TAG=(\S+)", (ROOT / "devenv" / "run.sh").read_text(), re.M)
    return f"surfcpu-toolchain:{m.group(1)}" if m else "surfcpu-toolchain"


def docker_bin() -> str | None:
    return shutil.which("docker") or (
        "/Applications/Docker.app/Contents/Resources/bin/docker"
        if Path("/Applications/Docker.app/Contents/Resources/bin/docker").exists() else None)


def probe_host() -> dict:
    out: dict = {}
    _, out["macos"] = run(["sw_vers", "-productVersion"])
    _, out["arch"] = run(["uname", "-m"])
    _, out["cpu"] = run(["sysctl", "-n", "machdep.cpu.brand_string"])
    rc, hw = run(["system_profiler", "SPHardwareDataType"], timeout=30)
    for key in ("Model Name", "Model Identifier", "Memory"):
        m = re.search(rf"{key}: (.+)", hw)
        out[key.lower().replace(" ", "_")] = m.group(1).strip() if m else "?"
    st = os.statvfs(str(ROOT))
    out["free_gib"] = round(st.f_bavail * st.f_frsize / 2**30, 1)
    return out


def probe_docker() -> dict:
    d = docker_bin()
    out: dict = {"binary": d, "running": False, "image": image_tag(), "image_present": False}
    if not d:
        return out
    rc, _ = run([d, "info"], timeout=20)
    out["running"] = rc == 0
    if out["running"]:
        rc, _ = run([d, "image", "inspect", out["image"]])
        out["image_present"] = rc == 0
        if out["image_present"]:
            rc, text = run([d, "run", "--rm", out["image"], "cat", "/etc/surfcpu-toolchain.txt"], 60)
            out["versions"] = dict(
                (ln[:28].strip(), ln[28:].strip()) for ln in text.splitlines() if ln.strip())
    return out


def usb_devices() -> list[dict]:
    rc, text = run(["ioreg", "-p", "IOUSB", "-l", "-w0", "-a"], timeout=20)
    if rc != 0:
        return []
    try:
        tree = plistlib.loads(text.encode())
    except Exception:
        return []
    found: list[dict] = []

    def walk(node):
        if isinstance(node, dict):
            if "idVendor" in node:
                found.append({k: node.get(k) for k in
                              ("idVendor", "idProduct", "USB Product Name", "USB Vendor Name",
                               "USB Serial Number", "kUSBSerialNumberString")})
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)
    walk(tree)
    return found


def probe_board() -> dict:
    out: dict = {"ftdi": [], "serial_ports": [], "openfpgaloader": None, "jtag": None}
    for dev in usb_devices():
        if dev.get("idVendor") == FTDI_VID and dev.get("idProduct") == FTDI_PID:
            out["ftdi"].append(dev)
    out["serial_ports"] = sorted(str(p) for p in Path("/dev").glob("cu.usbserial-*"))
    ofl = shutil.which("openFPGALoader")
    if ofl:
        _, ver = run([ofl, "--Version"])
        out["openfpgaloader"] = {"path": ofl, "version": ver.splitlines()[0] if ver else "?"}
        if out["ftdi"]:
            rc, text = run([ofl, "-c", "digilent", "--detect"], timeout=30)
            m = re.search(r"idcode\s+0x([0-9a-fA-F]+)", text)
            jt = {"rc": rc, "output": text[-800:]}
            if m:
                code = int(m.group(1), 16)
                jt["idcode"] = f"0x{code:08X}"
                jt["part"] = IDCODES.get(code & 0x0FFFFFFF, "unknown")
            out["jtag"] = jt
    return out


def probe_cards() -> list[dict]:
    """Mounted FAT volumes (candidate microSD cards). Read-only questions to diskutil."""
    cards = []
    rc, text = run(["mount"])
    for line in text.splitlines():
        m = re.match(r"(/dev/disk\S+) on (/Volumes/.+?) \(msdos,", line)
        if not m:
            continue
        dev, mnt = m.groups()
        rc2, plist = run(["diskutil", "info", "-plist", dev])
        info = {}
        try:
            p = plistlib.loads(plist.encode())
            info = {k: p.get(k) for k in ("FilesystemName", "VolumeName", "TotalSize", "Removable",
                                          "RemovableMedia", "Internal", "DeviceIdentifier",
                                          "ParentWholeDisk", "BusProtocol", "MediaName")}
        except Exception:
            pass
        cards.append({"device": dev, "mount": mnt, **info})
    return cards


def probe_build_host() -> dict:
    out = {"config": str(BUILD_HOST_CONF), "present": BUILD_HOST_CONF.exists()}
    if out["present"]:
        conf = dict(re.findall(r"^\s*([A-Z_]+)\s*=\s*(.*?)\s*$", BUILD_HOST_CONF.read_text(), re.M))
        out["flow"] = conf.get("FLOW", "?")
        out["host"] = conf.get("HOST", "")
        if out["flow"] == "vivado-ssh" and out["host"]:
            rc, text = run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", out["host"],
                            "command -v vivado && vivado -version | head -n 1"], timeout=30)
            out["reachable"] = rc == 0
            out["vivado"] = text.splitlines()[-1] if text else ""
    return out


def main() -> int:
    report = {
        "host": probe_host(),
        "docker": probe_docker(),
        "board": probe_board(),
        "cards": probe_cards(),
        "build_host": probe_build_host(),
    }
    ok = report["docker"]["running"] and report["docker"]["image_present"]
    if "--json" in sys.argv:
        print(json.dumps(report, indent=2, default=str))
        return 0 if ok else 1

    h, d, b = report["host"], report["docker"], report["board"]
    print("SurfCPU doctor")
    print(f"  Mac          {h.get('model_name')} ({h.get('model_identifier')}), {h.get('cpu')}, "
          f"{h.get('memory')}, macOS {h.get('macos')} {h.get('arch')}, {h.get('free_gib')} GiB free")
    if not d["binary"]:
        print("  Docker       MISSING: install Docker Desktop")
    elif not d["running"]:
        print("  Docker       not running: open -a Docker")
    elif not d["image_present"]:
        print(f"  Docker       running; image {d['image']} MISSING: make image")
    else:
        v = d.get("versions", {})
        print(f"  Toolchain    {d['image']} ({v.get('image-arch', '?')}): {v.get('verilator', '?')}; "
              f"{v.get('iverilog', '?')}; {v.get('yosys', '?')}")
        print(f"               {v.get('riscv-gcc', '?')}; rv32im libgcc {v.get('rv32im-multilib', '?')}; "
              f"spike {v.get('spike', '?')[:12]}")
    if b["ftdi"]:
        for f in b["ftdi"]:
            print(f"  Board USB    FT2232 (0403:6010) {f.get('USB Product Name')} serial "
                  f"{f.get('USB Serial Number') or f.get('kUSBSerialNumberString')}")
    else:
        print("  Board USB    pending: no Arty (FTDI 0403:6010) connected")
    print(f"  Serial       {', '.join(b['serial_ports']) or 'pending: no /dev/cu.usbserial-* port'}")
    if b["openfpgaloader"]:
        print(f"  openFPGALoader {b['openfpgaloader']['version']}")
    else:
        print("  openFPGALoader pending: not installed (brew install openfpgaloader)")
    if b["jtag"]:
        j = b["jtag"]
        print(f"  JTAG         {j.get('idcode', 'no IDCODE read')} {j.get('part', '')}")
    if report["cards"]:
        for c in report["cards"]:
            print(f"  FAT volume   {c['mount']} ({c['device']}, {c.get('FilesystemName')}, "
                  f"{(c.get('TotalSize') or 0) / 1e9:.1f} GB, removable={c.get('RemovableMedia')})")
    else:
        print("  Card         pending: no FAT volume mounted")
    bh = report["build_host"]
    if not bh["present"]:
        print(f"  Build host   pending: no {bh['config']} (docs/BUILD-HOST.md)")
    else:
        extra = f", reachable={bh.get('reachable')}" if "reachable" in bh else ""
        print(f"  Build host   FLOW={bh.get('flow')} HOST={bh.get('host')}{extra}")
    print("OK: build and simulation can run" if ok else "NOT READY: see above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
