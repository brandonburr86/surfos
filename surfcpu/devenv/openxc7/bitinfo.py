#!/usr/bin/env python3
"""
SurfCPU 7-series bitstream inspector
--------------------
File: bitinfo.py Date: 10/15/26 (SurfCPU)
--------------------
Reads a Xilinx 7-series .bit file without any vendor or Project X-Ray code, following the
configuration packet format of AMD UG470 (7 Series FPGAs Configuration User Guide, chapter 5):
the .bit header (design, part, date, time), the sync word, every type 1 and type 2 packet,
the register writes (IDCODE, FAR, CMD, COR0/1, CTL0/1, MASK, ...), the frame data written to
FDRI, and the configuration CRC, which it recomputes (CRC-32C over each written word and
its 5-bit register address) and compares with every value the bitstream checks. The CRC
rules (reset by RCRC and after each check, LOUT left out) reproduce every CRC check in
Project X-Ray's Vivado 2016.3 test bitstreams (lib/test_data/configuration_test*.bit).

    bitinfo.py design.bit                          print a summary
    bitinfo.py --json design.bit                   the same as JSON
    bitinfo.py --expect-device xc7a100t design.bit check it is a complete standard bitstream for
                                                   that device (length, IDCODE, frames, commands,
                                                   CRC); exit 1 (with the reasons) if not
    bitinfo.py --self-test                         check the parser on synthetic bitstreams

The IDCODE check uses the JTAG IDCODEs of the devices, not Project X-Ray's database, and
ignores the 4-bit revision field (bits 31..28), which bitstreams do not carry (a 100T reads
0x13631093 over JTAG; its bitstreams name 0x03631093). Only the Python standard library.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

SYNC = 0xAA995566
FRAME_WORDS = 101                                   # 7-series frame length in 32-bit words
JTAG_IDCODES = {                                    # JTAG IDCODE, revision field included
    "xc7a35t": 0x0362D093,
    "xc7a50t": 0x0362C093,
    "xc7a100t": 0x13631093,
    "xc7a200t": 0x13636093,
}
REGS = {0: "CRC", 1: "FAR", 2: "FDRI", 3: "FDRO", 4: "CMD", 5: "CTL0", 6: "MASK", 7: "STAT",
        8: "LOUT", 9: "COR0", 10: "MFWR", 11: "CBC", 12: "IDCODE", 13: "AXSS", 14: "COR1",
        16: "WBSTAR", 17: "TIMER", 19: "RBCRC_SW", 22: "BOOTSTS", 24: "CTL1", 31: "BSPI"}
CMDS = {0: "NULL", 1: "WCFG", 2: "MFW", 3: "LFRM", 4: "RCFG", 5: "START", 6: "RCAP", 7: "RCRC",
        8: "AGHIGH", 9: "SWITCH", 10: "GRESTORE", 11: "SHUTDOWN", 12: "GCAPTURE", 13: "DESYNC",
        15: "IPROG", 16: "CRCC", 17: "LTIMER", 18: "BSPI_READ", 19: "FALL_EDGE"}
CRC32C_REFLECTED = 0x82F63B78                       # Castagnoli polynomial, bit-reversed
CONFIG_BITS = {                                     # UG470 table 1-1, configuration bitstream
    "xc7a35t": 17536096,                            # length of a standard (uncompressed,
    "xc7a50t": 17536096,                            # non-debug) bitstream
    "xc7a100t": 30606304,
    "xc7a200t": 77845216,
}


def crc_step(crc: int, reg: int, word: int) -> int:
    """One configuration CRC update: the 37 bits {reg[4:0], word[31:0]}, LSB first."""
    val = ((reg & 0x1F) << 32) | (word & 0xFFFFFFFF)
    for _ in range(37):
        if (val ^ crc) & 1:
            crc = (crc >> 1) ^ CRC32C_REFLECTED
        else:
            crc >>= 1
        val >>= 1
    return crc


def parse_header(data: bytes) -> tuple[dict, int]:
    """The .bit header (fields a..e) if present; returns (fields, offset of the raw stream)."""
    fields: dict = {}
    if len(data) < 13 or data[:2] != b"\x00\x09":
        return fields, 0
    if data[2:11] != bytes.fromhex("0ff00ff00ff00ff000") or data[11:13] != b"\x00\x01":
        raise ValueError("not a .bit header (bad magic)")
    pos = 13
    names = {"a": "design", "b": "part", "c": "date", "d": "time"}
    while pos < len(data):
        key = chr(data[pos])
        pos += 1
        if key == "e":
            (length,) = struct.unpack(">I", data[pos:pos + 4])
            fields["length"] = length
            return fields, pos + 4
        if key not in names:
            raise ValueError(f"unknown .bit header field {key!r} at byte {pos - 1}")
        (n,) = struct.unpack(">H", data[pos:pos + 2])
        fields[names[key]] = data[pos + 2:pos + 2 + n].rstrip(b"\x00").decode("ascii", "replace")
        pos += 2 + n
    raise ValueError("the .bit header has no data-length field")


def analyse(data: bytes) -> dict:
    header, start = parse_header(data)
    raw = data[start:]
    if header.get("length") is not None and header["length"] != len(raw):
        raise ValueError(f"header says {header['length']} bytes of stream, file has {len(raw)}")
    if len(raw) % 4:
        raise ValueError("the configuration stream is not a whole number of 32-bit words")
    words = struct.unpack(f">{len(raw) // 4}I", raw)
    try:
        sync_at = words.index(SYNC)
    except ValueError:
        raise ValueError("no sync word (0xAA995566)")

    info = {"header": header, "stream_bytes": len(raw), "sync_word_index": sync_at,
            "writes": {}, "idcodes": [], "commands": [], "far": [], "fdri_words": 0,
            "crc_checks": [], "cor0": None, "cor1": None, "ctl0": None, "ctl1": None,
            "desynced_at": None, "trailing_words": 0, "zero_words": 0}
    crc = 0
    last_reg = None
    data_reg = None                                  # the last register written, LOUT aside
    i = sync_at + 1
    while i < len(words):
        w = words[i]
        kind, op = w >> 29, (w >> 27) & 3
        if w == 0:
            # Vivado's debug bitstreams follow some LOUT writes with two frames of zero words
            # and no packet header; the configuration CRC shows the device takes them as frame
            # data (FDRI), so they are counted as such.
            info["zero_words"] += 1
            if data_reg == 2:
                info["fdri_words"] += 1
                crc = crc_step(crc, 2, 0)
            i += 1
            continue
        if kind == 1:
            reg, count = (w >> 13) & 0x1F, w & 0x7FF
            last_reg = reg
        elif kind == 2:
            if last_reg is None:
                raise ValueError(f"type 2 packet without a type 1 before it at word {i}")
            reg, count = last_reg, w & 0x7FFFFFF
        else:
            raise ValueError(f"word {i} (0x{w:08X}) is not a packet header")
        payload = words[i + 1:i + 1 + count]
        if len(payload) != count:
            raise ValueError(f"packet at word {i} wants {count} words, the file ends first")
        i += 1 + count
        if op != 2:                                  # NOP and reads carry no configuration
            continue
        name = REGS.get(reg, f"REG{reg}")
        info["writes"][name] = info["writes"].get(name, 0) + count
        if reg != 8:
            data_reg = reg
        for v in payload:
            if reg == 0:                             # a CRC check: compare, then it is reset
                info["crc_checks"].append({"word": i - 1, "expected": v, "computed": crc,
                                           "ok": v == crc})
                crc = 0
                continue
            if reg == 4 and v == 7:                  # RCRC resets the CRC
                info["commands"].append("RCRC")
                crc = 0
                continue
            if reg != 8:                             # LOUT (daisy-chain output) is not in the CRC
                crc = crc_step(crc, reg, v)
            if reg == 1:
                info["far"].append(v)
            elif reg == 2:
                info["fdri_words"] += 1
            elif reg == 4:
                info["commands"].append(CMDS.get(v, f"CMD{v}"))
            elif reg == 12:
                info["idcodes"].append(v)
            elif reg in (5, 9, 14, 24):
                info[{5: "ctl0", 9: "cor0", 14: "cor1", 24: "ctl1"}[reg]] = v
        if info["commands"] and info["commands"][-1] == "DESYNC":
            info["desynced_at"] = i
            break
    if info["desynced_at"] is not None:
        rest = words[info["desynced_at"]:]
        info["trailing_words"] = len(rest)
        if any(v != 0x20000000 for v in rest):      # only type 1 NOPs may follow DESYNC
            raise ValueError("words other than NOPs after DESYNC")
    info["frames"] = info["fdri_words"] / FRAME_WORDS
    return info


def check(info: dict, device: str, standard_length: bool = True) -> list[str]:
    """Reasons why `info` is not a complete bitstream for `device` (empty: none found)."""
    problems = []
    if standard_length and info["stream_bytes"] * 8 != CONFIG_BITS[device]:
        problems.append(f"the stream is {info['stream_bytes'] * 8} bits, not the {CONFIG_BITS[device]} "
                        f"of a standard {device} bitstream (UG470 table 1-1)")
    want = JTAG_IDCODES[device] & 0x0FFFFFFF
    ids = [v & 0x0FFFFFFF for v in info["idcodes"]]
    if not ids:
        problems.append("no IDCODE write")
    elif any(v != want for v in ids):
        problems.append("IDCODE " + ", ".join(f"0x{v:08X}" for v in info["idcodes"])
                        + f" is not {device}'s 0x{want:08X} (revision bits ignored)")
    part = info["header"].get("part", "").lower()
    if part and not part.removeprefix("xc").startswith(device.removeprefix("xc")):
        problems.append(f"the header names part {part!r}, not a {device}")
    if info["fdri_words"] == 0:
        problems.append("no frame data written to FDRI")
    elif info["fdri_words"] % FRAME_WORDS:
        problems.append(f"{info['fdri_words']} FDRI words is not a whole number of {FRAME_WORDS}-word frames")
    for c in ("WCFG", "START", "DESYNC"):
        if c not in info["commands"]:
            problems.append(f"no {c} command")
    if "START" in info["commands"] and "WCFG" in info["commands"] \
            and info["commands"].index("START") < info["commands"].index("WCFG"):
        problems.append("START comes before WCFG")
    bad = [c for c in info["crc_checks"] if not c["ok"]]
    for c in bad:
        problems.append(f"CRC check at word {c['word']}: the bitstream expects 0x{c['expected']:08X}, "
                        f"the words before it give 0x{c['computed']:08X}")
    return problems


def summary(info: dict) -> str:
    h = info["header"]
    lines = []
    if h:
        lines.append(f"header: design {h.get('design')!r} part {h.get('part')!r} "
                     f"date {h.get('date')} {h.get('time')}, stream {h.get('length')} bytes")
    else:
        lines.append(f"no .bit header, stream {info['stream_bytes']} bytes")
    lines.append(f"sync word at word {info['sync_word_index']}; stream {info['stream_bytes'] * 8} bits")
    for dev, bits in CONFIG_BITS.items():
        if info["stream_bytes"] * 8 == bits:
            lines.append(f"  = the standard bitstream length of the {dev} (UG470 table 1-1)")
    lines.append("IDCODE written: " + (", ".join(f"0x{v:08X}" for v in info["idcodes"]) or "none"))
    for dev, code in JTAG_IDCODES.items():
        if any((v & 0x0FFFFFFF) == (code & 0x0FFFFFFF) for v in info["idcodes"]):
            lines.append(f"  = {dev} (JTAG IDCODE 0x{code:08X}, revision bits ignored)")
    lines.append(f"frame data: {info['fdri_words']} words = {info['frames']:g} frames of {FRAME_WORDS} words")
    lines.append(f"FAR writes: {len(info['far'])}"
                 + (f" (first 0x{info['far'][0]:08X})" if info["far"] else ""))
    lines.append("commands: " + " ".join(info["commands"]))
    for k in ("cor0", "cor1", "ctl0", "ctl1"):
        if info[k] is not None:
            lines.append(f"{k.upper()} = 0x{info[k]:08X}")
    lines.append("register writes (words): " + ", ".join(f"{k} {v}" for k, v in sorted(info["writes"].items())))
    if info["crc_checks"]:
        ok = sum(c["ok"] for c in info["crc_checks"])
        lines.append(f"CRC checks: {len(info['crc_checks'])}, recomputed and matching: {ok}")
        for c in info["crc_checks"]:
            lines.append(f"  word {c['word']}: expects 0x{c['expected']:08X}, computed 0x{c['computed']:08X}"
                         f" {'ok' if c['ok'] else 'MISMATCH'}")
    else:
        lines.append("CRC checks: none in the bitstream")
    if info["zero_words"]:
        lines.append(f"headerless zero words (taken as frame data after FDRI): {info['zero_words']}")
    lines.append(f"after DESYNC: {info['trailing_words']} NOP words")
    return "\n".join(lines)


# ---------------------------------------------------------------- self-test
def _packet(reg: int, payload: list[int]) -> list[int]:
    return [(1 << 29) | (2 << 27) | (reg << 13) | len(payload)] + payload


def _synthetic(idcode: int, frames: int = 2, crc_ok: bool = True, header: bool = True) -> bytes:
    body = [0xFFFFFFFF] * 8 + [0x000000BB, 0x11220044, 0xFFFFFFFF, 0xFFFFFFFF, SYNC, 0x20000000]
    stream = _packet(4, [7]) + _packet(12, [idcode]) + _packet(4, [1]) + _packet(1, [0])
    fdri = [(n * 2654435761) & 0xFFFFFFFF for n in range(frames * FRAME_WORDS)]
    stream += [(1 << 29) | (2 << 27) | (2 << 13) | 0] + [(2 << 29) | (2 << 27) | len(fdri)] + fdri
    crc = 0
    for reg, vals in ((12, [idcode]), (4, [1]), (1, [0]), (2, fdri)):
        for v in vals:
            crc = crc_step(crc, reg, v)
    stream += _packet(0, [crc if crc_ok else crc ^ 1]) + _packet(4, [5]) + _packet(4, [13])
    stream += [0x20000000] * 4
    raw = struct.pack(f">{len(body) + len(stream)}I", *(body + stream))
    if not header:
        return raw
    def field(k: str, s: str) -> bytes:
        b = s.encode() + b"\x00"
        return k.encode() + struct.pack(">H", len(b)) + b
    hdr = b"\x00\x09" + bytes.fromhex("0ff00ff00ff00ff000") + b"\x00\x01"
    hdr += field("a", "test;UserID=0XFFFFFFFF") + field("b", "7a35tcsg324") + field("c", "2026/10/15")
    hdr += field("d", "12:00:00") + b"e" + struct.pack(">I", len(raw))
    return hdr + raw


def self_test() -> int:
    failures = 0
    def expect(cond: bool, what: str) -> None:
        nonlocal failures
        if not cond:
            failures += 1
            print(f"self-test FAIL: {what}")
    # the CRC: a stream of zeros leaves it zero; one changed bit changes it
    # known answers from Project X-Ray's lib/xilinx/tests/xc7series/crc_test.cc (icap_crc)
    expect(crc_step(0, 0, 0) == 0, "CRC of a zero word at register 0 is 0")
    expect(crc_step(0, 1 << 4, 0) == 0x82F63B78, "CRC of register bit 4")
    expect(crc_step(0, 0x1F, 0xFFFFFFFF) == 0xBF86D4DF, "CRC of all register and data bits")
    expect(crc_step(0xFFFFFFFF, 0, 0) == 0xC631E365, "CRC from all ones")
    expect(crc_step(0, 2, 1) != crc_step(0, 2, 2), "CRC depends on the data")
    expect(crc_step(0, 1, 5) != crc_step(0, 2, 5), "CRC depends on the register address")
    info = analyse(_synthetic(0x0362D093))
    expect(info["header"]["part"] == "7a35tcsg324", "header part")
    expect(info["idcodes"] == [0x0362D093], "IDCODE")
    expect(info["frames"] == 2, "frame count")
    expect(info["commands"] == ["RCRC", "WCFG", "START", "DESYNC"], f"commands {info['commands']}")
    expect(len(info["crc_checks"]) == 1 and info["crc_checks"][0]["ok"], "CRC recomputed")
    expect(check(info, "xc7a35t", False) == [], f"clean check {check(info, 'xc7a35t', False)}")
    expect(any("bits, not the" in p for p in check(info, "xc7a35t")), "short stream refused")
    expect(any("IDCODE" in p for p in check(info, "xc7a100t", False)), "wrong device refused")
    info = analyse(_synthetic(0x0362D093, crc_ok=False))
    expect(any("CRC" in p for p in check(info, "xc7a35t", False)), "bad CRC refused")
    info = analyse(_synthetic(0x03631093, header=False))
    expect(info["header"] == {} and check(info, "xc7a100t", False) == [], "100T without header, revision 0")
    try:
        analyse(_synthetic(0x0362D093)[:-37])
        expect(False, "truncated stream refused")
    except ValueError:
        pass
    try:
        analyse(b"\xff" * 64)
        expect(False, "stream without sync refused")
    except ValueError:
        pass
    print("bitinfo self-test: " + ("PASS" if not failures else f"{failures} failures"))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("--------------------")[0].strip())
    ap.add_argument("bit", nargs="?")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--expect-device", choices=sorted(JTAG_IDCODES))
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.bit:
        ap.error("a .bit file is needed")
    with open(args.bit, "rb") as f:
        data = f.read()
    try:
        info = analyse(data)
    except ValueError as e:
        print(f"bitinfo: {args.bit}: {e}", file=sys.stderr)
        return 1
    problems = check(info, args.expect_device) if args.expect_device else []
    if args.json:
        info["problems"] = problems
        print(json.dumps(info, indent=1, default=lambda v: v))
    else:
        print(summary(info))
        if args.expect_device:
            print(f"check for {args.expect_device}: " + ("PASS" if not problems else "FAIL"))
            for p in problems:
                print(f"  {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
