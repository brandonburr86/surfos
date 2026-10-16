#!/usr/bin/env python3
"""SurfCPU bitstream files: the reproducible header, the QSPI flash image, the configuration options
--------------------
File: bitfile.py Date: 10/16/26 (SurfCPU)
--------------------
A Xilinx 7-series .bit file is a short header (fields a: the design, b: the part, c: the date,
d: the time, e: the length of the rest) followed by the configuration stream the FPGA reads
(AMD UG470, 7 Series FPGAs Configuration, chapter 5: packets and registers). docs/RELEASE.md
section 6 explains what this tool is for.

    tools/bitfile.py stamp FILE.bit --epoch N
        Writes the date and time of N (seconds since 1970, in UTC) into the header, in place.
        Project X-Ray's xc7frames2bit writes the time of the build there, so two builds of one
        commit differ in those bytes only; tools/openxc7-build.sh stamps SOURCE_DATE_EPOCH when
        it is set (make release sets it to the commit's time). The stream is not touched.
    tools/bitfile.py flash FILE.bit -o FILE.bin --board BOARD
        The QSPI flash image: the configuration stream alone, to go at address 0 of the board's
        16 MiB flash. These are the bytes openFPGALoader writes for `-f FILE.bit` (its BitParser
        drops the header and reverses no bits for SPI flash), so the image is also what a read
        back of the flash must show. Refused unless the header names the board's part, the stream
        writes the board's IDCODE and it fits slot 0 (the first 4 MiB).
    tools/bitfile.py info FILE.bit|FILE.bin [--json]
        The header, the configuration options the stream sets (COR0's CCLK selection, the SPI bus
        width in BSPI, the watchdog TIMER, MultiBoot's WBSTAR and IPROG, CRC checks) and how long a
        cold configuration from the flash takes with them, and with the faster settings.
    tools/bitfile.py self-test

Only the Python standard library: it runs with the Mac's python3 (3.9) and in the containers.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BOARDS_TXT = HERE.parent / "boards" / "arty_a7" / "boards.txt"

MAGIC = bytes.fromhex("0009" "0ff00ff00ff00ff000" "0001")
SYNC = 0xAA995566
FRAME_WORDS = 101
REGS = {0: "CRC", 1: "FAR", 2: "FDRI", 3: "FDRO", 4: "CMD", 5: "CTL0", 6: "MASK", 7: "STAT", 8: "LOUT",
        9: "COR0", 10: "MFWR", 11: "CBC", 12: "IDCODE", 13: "AXSS", 14: "COR1", 16: "WBSTAR", 17: "TIMER",
        19: "RBCRC_SW", 22: "BOOTSTS", 24: "CTL1", 31: "BSPI"}
CMDS = {0: "NULL", 1: "WCFG", 2: "MFW", 3: "LFRM", 4: "RCFG", 5: "START", 6: "RCAP", 7: "RCRC", 8: "AGHIGH",
        9: "SWITCH", 10: "GRESTORE", 11: "SHUTDOWN", 12: "GCAPTURE", 13: "DESYNC", 15: "IPROG", 16: "CRCC",
        17: "LTIMER", 18: "BSPI_READ", 19: "FALL_EDGE"}
# UG470 table 1-1: the length of a standard (uncompressed) configuration bitstream, in bits
STANDARD_BITS = {"xc7a35t": 17536096, "xc7a50t": 17536096, "xc7a100t": 30606304}
JTAG_IDCODES = {"xc7a35t": 0x0362D093, "xc7a50t": 0x0362C093, "xc7a100t": 0x03631093}   # bits 27:0
# The Arty A7's configuration flash: 128 Mbit whatever the revision (reference manual section 5.2,
# docs/INVENTORY.md gap G7: Micron N25Q128A13ESF40, Spansion S25FL128SAG[M|N]FI00 or S25FL127SABMFx00).
FLASH_BYTES = 16 << 20
SLOT_BYTES = 4 << 20            # slot 0, from address 0: where the FPGA reads at power-on
STARTUP_CCLK_HZ = 3_000_000     # UG470 figure 2-13 note 2: "The CCLK frequency starts at 3 MHz typical"
BUS_WIDTHS = {0: 1, 1: 2, 2: 4}  # BSPI[9:8] (UG470 table 5-41)


class BitError(Exception):
    pass


# ------------------------------------------------------------------------------------------- header
def parse_header(data: bytes) -> tuple[dict, dict, int]:
    """(fields, spans, offset of the stream) of a .bit file: fields maps 'a'..'d' to their text and
    'e' to the stream length; spans maps each key to the (start, end) of its value in the file.
    A file without the header (a .bin) gives ({}, {}, 0)."""
    if not data.startswith(MAGIC[:2]):
        return {}, {}, 0
    if not data.startswith(MAGIC):
        raise BitError("not a .bit header (its first 13 bytes are not the .bit magic)")
    fields: dict = {}
    spans: dict = {}
    pos = len(MAGIC)
    while True:
        if pos + 1 > len(data):
            raise BitError("the .bit header ends before its field e")
        key = chr(data[pos])
        if key == "e":
            if pos + 5 > len(data):
                raise BitError("the .bit header's field e is cut short")
            n = struct.unpack_from(">I", data, pos + 1)[0]
            fields["e"] = n
            spans["e"] = (pos + 1, pos + 5)
            start = pos + 5
            if start + n != len(data):
                raise BitError(f"the .bit header says {n} bytes follow, the file has {len(data) - start}")
            return fields, spans, start
        if key not in "abcd":
            raise BitError(f"unexpected .bit header field {key!r} at byte {pos}")
        if pos + 3 > len(data):
            raise BitError(f"the .bit header's field {key} is cut short")
        n = struct.unpack_from(">H", data, pos + 1)[0]
        value = data[pos + 3:pos + 3 + n]
        if len(value) != n or not value.endswith(b"\0"):
            raise BitError(f"the .bit header's field {key} is not a NUL-terminated string")
        fields[key] = value[:-1].decode("ascii", "replace")
        spans[key] = (pos + 3, pos + 3 + n)
        pos += 3 + n


def stamp(data: bytes, epoch: int) -> bytes:
    """data with the header's date (c) and time (d) set to epoch, UTC; the same length"""
    fields, spans, _ = parse_header(data)
    if not fields:
        raise BitError("no .bit header to stamp")
    t = time.gmtime(epoch)
    new = {"c": time.strftime("%Y/%m/%d", t), "d": time.strftime("%H:%M:%S", t)}
    out = bytearray(data)
    for k, v in new.items():
        if k not in spans:
            raise BitError(f"the .bit header has no field {k}")
        a, b = spans[k]
        raw = v.encode("ascii") + b"\0"
        if len(raw) != b - a:
            raise BitError(f"the .bit header's field {k} is {b - a} bytes, not {len(raw)}: cannot stamp it in place")
        out[a:b] = raw
    return bytes(out)


# ------------------------------------------------------------------------------------------- stream
def words(stream: bytes) -> list[int]:
    n = len(stream) // 4
    return list(struct.unpack(f">{n}I", stream[:n * 4]))


def walk(stream: bytes) -> dict:
    """The configuration packets of a stream (UG470 chapter 5): sync position, every register write
    (word index, register, value), FDRI data, commands; packets lists (first word, last word, what)
    for locating a byte offset."""
    w = words(stream)
    info: dict = {"stream_bytes": len(stream), "sync_word": None, "writes": [], "commands": [], "fdri": [],
                  "packets": [], "desync_word": None, "problems": []}
    try:
        i = w.index(SYNC)
    except ValueError:
        info["problems"].append("no sync word (0xAA995566)")
        return info
    info["sync_word"] = i
    info["packets"].append((0, i, "the bus-width detection and sync words before the packets"))
    i += 1
    reg = None
    while i < len(w):
        hdr = w[i]
        typ = hdr >> 29
        if typ == 1:
            op = (hdr >> 27) & 3
            reg = (hdr >> 13) & 0x1F
            count = hdr & 0x7FF
        elif typ == 2:
            op = (hdr >> 27) & 3
            count = hdr & 0x7FFFFFF
            if reg is None:
                info["problems"].append(f"a type 2 packet at word {i} with no type 1 before it")
                break
        else:
            info["problems"].append(f"word {i} (0x{hdr:08X}) is not a packet header")
            break
        if i + 1 + count > len(w):
            info["problems"].append(f"the packet at word {i} runs past the end")
            break
        name = REGS.get(reg, f"reg{reg}")
        if op == 0:
            info["packets"].append((i, i, "NOP"))
        elif op == 2:
            payload = w[i + 1:i + 1 + count]
            if name == "FDRI":
                info["fdri"].append((i + 1, count))
                info["packets"].append((i, i + count, f"FDRI: {count} words of frame data"))
            else:
                for k, v in enumerate(payload):
                    info["writes"].append((i + 1 + k, name, v))
                    if name == "CMD":
                        info["commands"].append(CMDS.get(v & 0x1F, f"cmd{v}"))
                what = name + (" " + ", ".join(CMDS.get(v & 0x1F, hex(v)) for v in payload) if name == "CMD"
                               else " = " + ", ".join(f"0x{v:08X}" for v in payload))
                info["packets"].append((i, i + count, f"write {what}"))
                if name == "CMD" and payload and payload[-1] & 0x1F == 13:
                    info["desync_word"] = i + count
                    if i + count + 1 < len(w):
                        info["packets"].append((i + count + 1, len(w) - 1, "padding after DESYNC"))
                    break
        else:
            info["packets"].append((i, i + count, f"read {name}" if op == 1 else "reserved opcode"))
        i += 1 + count
    return info


def last_write(info: dict, reg: str):
    vals = [v for _, r, v in info["writes"] if r == reg]
    return vals[-1] if vals else None


def options(info: dict) -> dict:
    """the configuration options that matter for booting from the flash, as the stream sets them"""
    cor0 = last_write(info, "COR0")
    bspi = last_write(info, "BSPI")
    timer = last_write(info, "TIMER")
    wbstar = last_write(info, "WBSTAR")
    crc_checks = sum(1 for _, r, _ in info["writes"] if r == "CRC")
    oscfsel = (cor0 >> 17) & 0x3F if cor0 is not None else None
    width = BUS_WIDTHS.get((bspi >> 8) & 3, 1) if bspi is not None and "BSPI_READ" in info["commands"] else 1
    return {
        "cor0": cor0, "oscfsel": oscfsel,
        "cclk": "start-up rate, about 3 MHz" if oscfsel == 0 else (None if oscfsel is None else f"OSCFSEL {oscfsel}"),
        "bspi": bspi, "spi_width": width,
        "spi_read_opcode": (bspi & 0xFF) if bspi is not None else None,
        "timer": timer, "watchdog": bool(timer and (timer >> 30) & 3),
        "wbstar": wbstar, "iprog": "IPROG" in info["commands"],
        "crc_checks": crc_checks,
        "commands": info["commands"],
        "idcode": last_write(info, "IDCODE"),
    }


def config_seconds(bits: int, width: int, hz: float) -> float:
    return bits / (width * hz)


def time_table(bits: int) -> list[tuple[str, float]]:
    """(setting, seconds) for the stream as built and for the faster settings Vivado can write"""
    rows = [("x1 at the start-up rate (3 MHz typical): the open flow's bitstreams", config_seconds(bits, 1, 3e6))]
    for width, mhz in ((1, 33), (2, 33), (4, 33), (4, 50)):
        rows.append((f"x{width} at {mhz} MHz", config_seconds(bits, width, mhz * 1e6)))
    return rows


# ------------------------------------------------------------------------------------------- boards
def board_line(board: str) -> dict:
    for line in BOARDS_TXT.read_text().splitlines():
        f = line.split()
        if f and not f[0].startswith("#") and f[0] == board:
            return {"board": f[0], "part": f[1], "ofl_board": f[2], "idcode": int(f[3], 16),
                    "bit_parts": f[4].split(",")}
    raise BitError(f"unknown board {board!r} ({BOARDS_TXT.name})")


def device_of(part: str) -> str:
    p = part.lower()
    for d in STANDARD_BITS:
        if p.startswith(d):
            return d
    raise BitError(f"part {part!r} is not an Artix-7 device this tool knows")


def flash_image(data: bytes, board: str) -> tuple[bytes, dict]:
    """the stream of a .bit made for board, checked; with a description of where it goes"""
    b = board_line(board)
    fields, _, start = parse_header(data)
    if not fields:
        raise BitError("not a .bit file (no header): the flash image is made from the .bit")
    norm = fields["b"].lower().removeprefix("xc").split("-")[0]
    if norm not in b["bit_parts"]:
        raise BitError(f"the .bit header names the part {fields['b']}, not {board}'s ({', '.join(b['bit_parts'])})")
    stream = data[start:]
    info = walk(stream)
    if info["problems"]:
        raise BitError("the stream is not a complete configuration: " + "; ".join(info["problems"]))
    idcode = last_write(info, "IDCODE")
    if idcode is None or (idcode & 0x0FFFFFFF) != b["idcode"] & 0x0FFFFFFF:
        raise BitError(f"the stream writes IDCODE {idcode:#010x}, not {board}'s {b['idcode']:#010x}"
                       if idcode is not None else "the stream writes no IDCODE")
    if info["desync_word"] is None:
        raise BitError("the stream has no DESYNC command: not a complete configuration")
    if len(stream) > SLOT_BYTES:
        raise BitError(f"the stream is {len(stream)} bytes, more than slot 0's {SLOT_BYTES}")
    dev = device_of(b["part"])
    end = len(stream) - 1
    desc = {
        "board": board, "part": fields["b"], "device": dev, "bytes": len(stream), "bits": len(stream) * 8,
        "standard_length": len(stream) * 8 == STANDARD_BITS[dev],
        "first": 0, "last": end, "end_64k": -(-len(stream) // 0x10000) * 0x10000,
        "end_256k": -(-len(stream) // 0x40000) * 0x40000, "flash_bytes": FLASH_BYTES, "slot_bytes": SLOT_BYTES,
    }
    return stream, desc


def describe_map(d: dict) -> list[str]:
    return [
        f"QSPI flash image for {d['board']} ({d['part']}): {d['bytes']:,} bytes = {d['bits']:,} bits"
        + (f", the standard length of the {d['device']} (UG470 table 1-1)" if d["standard_length"] else ""),
        f"  0x000000-0x{d['last']:06X}  the configuration stream (slot 0: the FPGA reads from address 0 at power-on)",
        f"  0x{d['last'] + 1:06X}-0x{d['slot_bytes'] - 1:06X}  rest of slot 0, unused (erased with the image up to "
        f"0x{d['end_64k'] - 1:06X} with 64 KiB sectors, 0x{d['end_256k'] - 1:06X} with 256 KiB ones)",
        f"  0x{d['slot_bytes']:06X}-0x{d['flash_bytes'] - 1:06X}  free ({(d['flash_bytes'] - d['slot_bytes']) >> 20} MiB):"
        " nothing of SurfCPU's (no MultiBoot update image; docs/RELEASE.md section 7)",
    ]


# ------------------------------------------------------------------------------------------- report
def report(data: bytes) -> dict:
    fields, spans, start = parse_header(data)
    stream = data[start:]
    info = walk(stream)
    opts = options(info)
    bits = len(stream) * 8
    return {"header": fields, "stream_offset": start, "stream_bytes": len(stream), "bits": bits,
            "sync_word": info["sync_word"], "problems": info["problems"], "options": opts,
            "fdri_words": sum(n for _, n in info["fdri"]),
            "times": [{"setting": s, "seconds": round(t, 3)} for s, t in time_table(bits)],
            "as_built_seconds": round(config_seconds(bits, opts["spi_width"], STARTUP_CCLK_HZ), 3)
            if opts["oscfsel"] == 0 else None}


def print_report(path: str, r: dict) -> None:
    h = r["header"]
    if h:
        print(f"{path}: .bit header: design '{h['a']}', part {h['b']}, {h['c']} {h['d']}; stream {r['stream_bytes']:,} bytes "
              f"from byte {r['stream_offset']}")
    else:
        print(f"{path}: no .bit header (a flash image): stream {r['stream_bytes']:,} bytes")
    for p in r["problems"]:
        print(f"  PROBLEM: {p}")
    o = r["options"]
    print(f"  IDCODE written: {o['idcode']:#010x}" if o["idcode"] is not None else "  IDCODE: none written")
    print(f"  COR0 = {o['cor0']:#010x}: OSCFSEL {o['oscfsel']} ({o['cclk']})" if o["cor0"] is not None else "  COR0: not written")
    if o["bspi"] is None:
        print("  BSPI: not written, so SPI x1 with the start-up read command (no x2/x4 switch)")
    else:
        print(f"  BSPI = {o['bspi']:#010x}: read opcode 0x{o['spi_read_opcode']:02X}, bus width x{o['spi_width']}"
              + ("" if "BSPI_READ" in o["commands"] else " (but no BSPI_READ command: stays x1)"))
    print(f"  TIMER = {o['timer']:#010x}: watchdog {'on' if o['watchdog'] else 'off'}" if o["timer"] is not None
          else "  TIMER: not written")
    print(f"  WBSTAR = {o['wbstar']:#010x}; IPROG {'present' if o['iprog'] else 'absent'} (MultiBoot: "
          f"{'a jump to another image' if o['iprog'] else 'none'})" if o["wbstar"] is not None else "  WBSTAR: not written")
    print(f"  CRC checks: {o['crc_checks']}" + ("" if o["crc_checks"] else " (the FPGA cannot catch a corrupted stream)"))
    print(f"  commands: {' '.join(o['commands'])}")
    print(f"  configuration time from the flash, {r['bits']:,} bits (plus power-on reset and initialization):")
    for t in r["times"]:
        print(f"    {t['setting']:<62} {t['seconds']:7.3f} s")
    if r["as_built_seconds"] is not None:
        print(f"  as built: x{o['spi_width']} at about 3 MHz: {r['as_built_seconds']:.2f} s (3 MHz is typical; the "
              "internal oscillator's tolerance, DS181 F_MCCKTOL, stretches it)")


# ------------------------------------------------------------------------------------------- files
def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix="." + path.name + ".")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def cmd_stamp(a) -> int:
    p = Path(a.file)
    data = p.read_bytes()
    new = stamp(data, a.epoch)
    if new != data:
        write_atomic(p, new)
    f, _, _ = parse_header(new)
    print(f"bitfile: {p}: header date and time {f['c']} {f['d']} (UTC, epoch {a.epoch})")
    return 0


def cmd_flash(a) -> int:
    stream, desc = flash_image(Path(a.file).read_bytes(), a.board)
    write_atomic(Path(a.output), stream)
    for line in describe_map(desc):
        print(line)
    print(f"  written to {a.output}")
    return 0


def cmd_info(a) -> int:
    r = report(Path(a.file).read_bytes())
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_report(a.file, r)
    return 1 if r["problems"] else 0


# ------------------------------------------------------------------------------------------- self-test
def _type1(reg: int, payload: list[int]) -> list[int]:
    return [(1 << 29) | (2 << 27) | (reg << 13) | len(payload)] + payload


def synthetic(idcode: int = 0x03631093, frames: int = 2, part: str = "xc7a100tcsg324-1",
              header: bool = True, date: str = "2026/10/16", tod: str = "13:41:43", extra: list | None = None) -> bytes:
    """a small stream with the open flow's packet sequence (UG470 table 5-19, no CRC check)"""
    w = [0xFFFFFFFF] * 8 + [0x000000BB, 0x11220044, 0xFFFFFFFF, 0xFFFFFFFF, SYNC, 0x20000000]
    w += _type1(17, [0]) + _type1(16, [0]) + _type1(4, [0]) + [0x20000000] + _type1(4, [7]) + [0x20000000] * 2
    w += _type1(19, [0]) + _type1(9, [0x02003FE5]) + _type1(14, [0]) + _type1(12, [idcode]) + _type1(4, [9])
    w += (extra or [])
    w += _type1(1, [0]) + _type1(4, [1]) + [0x20000000]
    n = frames * FRAME_WORDS
    w += [(1 << 29) | (2 << 27) | (2 << 13)] + [(2 << 29) | (2 << 27) | n] + [i & 0xFFFF for i in range(n)]
    w += _type1(4, [7]) + _type1(4, [10]) + _type1(4, [3]) + _type1(4, [5]) + _type1(4, [7]) + _type1(4, [13])
    w += [0x20000000] * 4
    stream = struct.pack(f">{len(w)}I", *w)
    if not header:
        return stream

    def field(k: str, s: str) -> bytes:
        raw = s.encode() + b"\0"
        return k.encode() + struct.pack(">H", len(raw)) + raw
    return (MAGIC + field("a", "build/x/openxc7/top.frames;Generator=xc7frames2bit") + field("b", part)
            + field("c", date) + field("d", tod) + b"e" + struct.pack(">I", len(stream)) + stream)


def self_test() -> int:
    bad = 0

    def expect(cond: bool, what: str) -> None:
        nonlocal bad
        print(("ok   " if cond else "FAIL ") + what)
        bad += not cond

    bit = synthetic()
    f, spans, start = parse_header(bit)
    expect(f["b"] == "xc7a100tcsg324-1" and f["c"] == "2026/10/16" and f["d"] == "13:41:43", "header fields read")
    expect(f["e"] == len(bit) - start, "field e is the stream length")
    s = stamp(bit, 1792158292)
    f2, _, _ = parse_header(s)
    expect(len(s) == len(bit) and s[start:] == bit[start:], "stamp keeps the length and the stream")
    expect([f2["c"], f2["d"]] == ["2026/10/16", "13:44:52"] and
           [f2["c"], f2["d"]] == time.strftime("%Y/%m/%d %H:%M:%S", time.gmtime(1792158292)).split(),
           "stamp writes the epoch's UTC date and time")
    expect(stamp(s, 1792158292) == s, "stamp is idempotent")
    expect(stamp(synthetic(tod="01:02:03"), 7) == stamp(synthetic(tod="23:59:59"), 7),
           "two builds at different times are the same bytes once stamped")
    try:
        stamp(synthetic(header=False), 7)
        expect(False, "stamp refuses a file without a header")
    except BitError:
        expect(True, "stamp refuses a file without a header")
    info = walk(bit[start:])
    o = options(info)
    expect(info["sync_word"] == 12 and not info["problems"], "sync word and packets parsed")
    expect(o["cor0"] == 0x02003FE5 and o["oscfsel"] == 0 and o["spi_width"] == 1, "COR0 and the x1 default read")
    expect(o["timer"] == 0 and o["wbstar"] == 0 and not o["iprog"] and o["crc_checks"] == 0,
           "TIMER and WBSTAR placeholders, no IPROG, no CRC check")
    expect(o["commands"][-1] == "DESYNC", "commands up to DESYNC")
    x4 = walk(synthetic(header=False, extra=_type1(31, [0x0000026B]) + _type1(4, [18])))
    expect(options(x4)["spi_width"] == 4 and options(x4)["spi_read_opcode"] == 0x6B, "a BSPI x4 write is read")
    stream, d = flash_image(bit, "arty_a7_100t")
    expect(stream == bit[start:], "the flash image is the stream after the header (what openFPGALoader -f writes)")
    expect(d["first"] == 0 and d["last"] == len(stream) - 1, "the image goes at address 0")
    for what, data, board in (("another part", synthetic(part="xc7a35tcsg324-1"), "arty_a7_100t"),
                              ("another IDCODE", synthetic(idcode=0x0362D093), "arty_a7_100t"),
                              ("no header", synthetic(header=False), "arty_a7_100t")):
        try:
            flash_image(data, board)
            expect(False, f"flash refuses {what}")
        except BitError:
            expect(True, f"flash refuses {what}")
    ok35, _ = flash_image(synthetic(part="xc7a35tcsg324-1", idcode=0x0362D093), "arty_a7_35t")
    expect(len(ok35) > 0, "the 35T takes the open flow's xc7a35tcsg324-1 header")
    t = dict(time_table(STANDARD_BITS["xc7a100t"]))
    expect(abs(t["x1 at the start-up rate (3 MHz typical): the open flow's bitstreams"] - 10.202) < 0.01,
           "100T at x1, 3 MHz: 10.2 s")
    expect(abs(t["x4 at 33 MHz"] - 0.2319) < 0.001, "100T at x4, 33 MHz: 0.23 s")
    print(f"bitfile self-test: {'PASS' if not bad else f'{bad} FAILED'}")
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SurfCPU .bit files: header stamp, QSPI flash image, options")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stamp", help="set the header's date and time to an epoch (UTC), in place")
    p.add_argument("file")
    p.add_argument("--epoch", type=int, required=True)
    p = sub.add_parser("flash", help="the QSPI flash image of a .bit for a board")
    p.add_argument("file")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--board", required=True)
    p = sub.add_parser("info", help="header, configuration options, configuration time")
    p.add_argument("file")
    p.add_argument("--json", action="store_true")
    sub.add_parser("self-test")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "stamp":
            return cmd_stamp(a)
        if a.cmd == "flash":
            return cmd_flash(a)
        if a.cmd == "info":
            return cmd_info(a)
        return self_test()
    except (BitError, OSError) as e:
        print(f"bitfile: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
