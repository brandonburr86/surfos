#!/usr/bin/env python3
"""SurfCPU firmware tests: the ROM images in the firmware emulator
--------------------
File: run_emu.py Date: 10/15/26 (SurfCPU)
--------------------
Runs the real RV32 builds (build/fw/surfboot.mem, the exact ROM file, and the diagnostics)
in build/fw/host/surfemu against card images and checks their UART output: SurfBoot
booting the handoff test kernel (which verifies the state of docs/BOOT-ABI.md section 4
from the inside), its fault reporter catching traps after the handoff, the monitor
(commands, memory faults reported, reset), card insertion, the buttons, DDR failures, a
trap during boot, the VGA text log, and the diagnostics programs.

    python3 firmware/tests/run_emu.py [--jobs 4] [-k NAME] [--keep]

These are simulated runs of the firmware on an instruction-level model of the SoC, not of
the RTL; physical results are pending the board.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import mkcard                                                # noqa: E402
import surfimg                                               # noqa: E402

FW = ROOT / "build" / "fw"
OUT = FW / "emu"
SURFEMU = FW / "host" / "surfemu"
ROM = FW / "surfboot.mem"
SCENARIOS = []


def scenario(name, args=(), expect=(), screen=(), rom=None, max_ms=15000):
    def deco(fn):
        SCENARIOS.append(dict(name=name, build=fn, args=list(args), expect=list(expect), screen=list(screen),
                              rom=rom, max_ms=max_ms))
        return fn
    return deco


def hello():
    return (FW / "HELLO.BIN").read_bytes()


def card(path, files, **kw):
    c = mkcard.Card(size=kw.pop("size", 64 << 20), **kw)
    for d in mkcard.STANDARD_DIRS:
        c.add_dir(d)
    for p, data in files.items():
        c.add_file(p, data)
    c.build(str(path))
    return path


def send(trigger, text):
    return ["--send", f"{trigger}={text}"]


BANNER = "SurfBoot 1.0.0"


@scenario("boot_and_trap_after_handoff",
          expect=[BANNER, "ddr: calibrated", "sd: SDHC/SDXC", "fat32: partition 1", "boot: starting the kernel",
                  "HELLO: handoff OK", "SurfBoot fault reporter", "mcause 0000000b", "s11 ", "LEDs blink code 8"],
          screen=["SurfBoot 1.0.0", "boot: starting the kernel"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello(), "/BOOT/CMDLINE.TXT": b"hello trap\n"})


@scenario("illegal_instruction_after_handoff", expect=["HELLO: handoff OK", "mcause 00000002", "mtval 00000000"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello(), "/BOOT/CMDLINE.TXT": b"illegal\n"})


@scenario("load_fault_after_handoff", expect=["HELLO: handoff OK", "mcause 00000005", "mtval 30000000"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello(), "/BOOT/CMDLINE.TXT": b"load\n"})


@scenario("sdsc_v1_card", args=["--type", "sdsc1"], expect=["sd: SDSC (SD 1.x)", "HELLO: handoff OK"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("sdsc_v2_card", args=["--type", "sdsc2"], expect=["sd: SDSC (SD 2.0)", "HELLO: handoff OK"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("fragmented_kernel_large", expect=["fragments", 'cmdline "big"', "HELLO: handoff OK"], max_ms=30000)
def _(p):
    # the test kernel with 600 KiB of extra payload behind it: a long, fragmented load
    elf_payload, load, mem, entry = surfimg.elf_payload(str(FW / "hello.elf"))
    pad = bytes((i * 13) & 0xFF for i in range(600 * 1024))
    pkg = surfimg.make_package(elf_payload + pad, load, entry, max(mem, len(elf_payload) + len(pad)), name="BigHello")
    return card(p, {"/BOOT/SURFOS.BIN": pkg, "/BOOT/CMDLINE.TXT": b"big\n", "/SYSTEM/FILLER.DAT": bytes(200000)},
                cluster=512, fragment="interleave")


@scenario("no_card_then_monitor",
          args=send("surfboot> ", "help\\r") + send("surfboot> ", "info\\r"),
          expect=["SurfBoot stopped: no card (NO_CARD)", "show code 2", "surfboot> ", "monitor commands",
                  "sysinfo: magic 46525553", "card: absent"])
def _(p):
    return None


@scenario("card_inserted_later", args=["--card-insert-ms", "1500"],
          expect=["(NO_CARD)", "card inserted: booting", "HELLO: handoff OK"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("btn1_stays_in_monitor", args=["--btn", "2"] + send("surfboot> ", "ls /BOOT\\r") +
          send("1 file", "boot\\r"),
          expect=["BTN1 held: staying in the monitor", "SURFOS.BIN", "1 file", "HELLO: handoff OK"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("btn0_boots_backup", args=["--btn", "1"],
          expect=["BTN0 held: booting /BOOT/BACKUP.BIN", "boot: starting BACKUP.BIN", "boot_flags 0x00000001",
                  "HELLO: handoff OK"])
def _(p):
    bad = bytearray(hello())
    bad[200] ^= 0xFF
    return card(p, {"/BOOT/SURFOS.BIN": bytes(bad), "/BOOT/BACKUP.BIN": hello()})


@scenario("bad_main_falls_back_to_backup",
          expect=["as read from the card", "trying /BOOT/BACKUP.BIN", "boot_flags 0x00000001", "HELLO: handoff OK"])
def _(p):
    bad = bytearray(hello())
    bad[300] ^= 0x01
    return card(p, {"/BOOT/SURFOS.BIN": bytes(bad), "/BOOT/BACKUP.BIN": hello()})


@scenario("ddr_never_calibrates", args=["--ddr", "never"],
          expect=["did not finish within 2000 ms", "(DDR_TIMEOUT)", "show code 1", "surfboot> "])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("no_ddr_controller", args=["--ddr", "absent"], expect=["no DDR controller", "(DDR_ABSENT)"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("trap_during_boot_recovers_to_monitor", args=["--ddr-fault-at", "0x80000400"] +
          send("surfboot> ", "info\\r"),
          expect=["*** trap: store access fault (mcause 0x00000007)", "mtval 0x80000400", "(TRAP)",
                  "while booting", "surfboot> ", "last boot failure: TRAP"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("monitor_on_a_bad_image",
          args=send("surfboot> ", "check /BOOT/SURFOS.BIN\\r") + send("(IMG_CRC)\r\n", "md 0x30000000 1\\r") +
          send("access fault", "mw 0x00000100 5\\r") + send("mw: access", "md 0x00000000 4\\r") +
          send("00000000:", "chain /BOOT/SURFOS.BIN\\r") + send("chain:", "reset\\r"),
          expect=["(IMG_CRC)", "check: /BOOT/SURFOS.BIN: FAILED", "access fault at 0x30000000",
                  "mw: access fault at 0x00000100", "00000000: 30401073", "fragment", "restarting", BANNER,
                  "(IMG_CRC)"])
def _(p):
    bad = bytearray(hello())
    bad[1000] ^= 0x10
    return card(p, {"/BOOT/SURFOS.BIN": bytes(bad)})


@scenario("card_never_finishes_init", args=["--acmd41-busy", "-1"], expect=["still initializing after 2 s", "(SD_INIT)"])
def _(p):
    return card(p, {"/BOOT/SURFOS.BIN": hello()})


@scenario("superfloppy_refused", expect=["not a partition table", "(SUPERFLOPPY)", "show code 4"])
def _(p):
    with open(p, "wb") as f:
        f.truncate(64 << 20)
    subprocess.run(["mkfs.fat", "-F", "32", str(p)], check=True, capture_output=True)
    return p


def run_scenario(sc, keep):
    img = OUT / f"{sc['name']}.img"
    if img.exists():
        img.unlink()
    t0 = time.time()
    try:
        built = sc["build"](img)
    except Exception as e:
        return sc["name"], False, f"building the image failed: {e!r}", 0.0
    screen = OUT / f"{sc['name']}.screen.txt"
    cmd = [str(SURFEMU), "--rom", str(sc["rom"] or ROM), "--quiet", "--max-ms", str(sc["max_ms"]),
           "--screen", str(screen)] + (["--card", str(built)] if built else []) + sc["args"]
    for e in sc["expect"]:
        cmd += ["--expect", e]
    log = OUT / f"{sc['name']}.log"
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return sc["name"], False, "timed out (host)", time.time() - t0
    ok = r.returncode == 0
    detail = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ""
    shown = screen.read_text() if screen.exists() else ""
    for s in sc["screen"]:
        if s not in shown:
            ok = False
            detail += f"\nthe screen lacks {s!r}"
    # rerun without --quiet to keep the transcript as evidence of what the firmware printed
    full = subprocess.run([c for c in cmd if c != "--quiet"], capture_output=True, text=True, timeout=600)
    log.write_text(full.stdout + full.stderr)
    if not ok:
        detail += f"\n--- transcript ({log}) ---\n" + full.stdout[-2500:] + full.stderr[-800:]
    elif not keep:
        for f in (img, screen):
            if f.exists():
                f.unlink()
    return sc["name"], ok, detail, time.time() - t0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the ROM images in the firmware emulator")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("-k", dest="filter")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)
    OUT.mkdir(parents=True, exist_ok=True)
    chosen = [s for s in SCENARIOS if not a.filter or a.filter in s["name"]]
    t0 = time.time()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for f in concurrent.futures.as_completed([ex.submit(run_scenario, s, a.keep) for s in chosen]):
            name, ok, detail, secs = f.result()
            results.append(ok)
            print(f"{'PASS' if ok else 'FAIL'} {name} ({secs:.1f} s) {detail.splitlines()[0] if ok and detail else ''}")
            if not ok:
                print("    " + detail.replace("\n", "\n    "))
    print(f"run_emu: {sum(results)} of {len(results)} emulator scenarios passed in {time.time() - t0:.0f} s")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
