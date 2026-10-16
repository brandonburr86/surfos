#!/usr/bin/env python3
"""SurfCPU font ROM generator
--------------------
File: fontrom.py Date: 10/15/26 (SurfCPU)
--------------------
Builds surf_video's font ROM, rtl/peripherals/surf_video_font.v, from the repository's
own font sources, the same ones the x86 SurfOS console font driver/font8x16.c comes from:
Spleen 8x16 (driver/fonts/spleen-8x16.bdf) plus SurfOS's symbols
(driver/fonts/surfos-symbols.bdf) for what Spleen lacks, in the code page 437 order of
cp437_unicode[] in driver/cp437.c (index 0x00, the empty cell, is blank).

    python3 tools/fontrom.py gen      write rtl/peripherals/surf_video_font.v
    python3 tools/fontrom.py check    exit 1 unless that file is current and every glyph
                                      equals driver/font8x16.c's
    python3 tools/fontrom.py dump     print the 256 glyphs as text pictures

Pinned: the SHA-256 of each source is fixed below (PINS); if a source changes, gen and
check stop and name it, so a font change is a deliberate, reviewed step (update the pin,
regenerate, run check). The BDF reader is this file's own, not tools/bdf2c.py's: check
then compares two independent conversions of the same sources. The ROM keeps Spleen's
licence (driver/fonts/LICENSE-spleen, BSD 2-Clause) in its header.

Only the Python standard library is used.
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # surfcpu/
REPO = ROOT.parent                                     # the repository

SPLEEN = REPO / "driver" / "fonts" / "spleen-8x16.bdf"
SYMBOLS = REPO / "driver" / "fonts" / "surfos-symbols.bdf"
LICENSE = REPO / "driver" / "fonts" / "LICENSE-spleen"
CP437 = REPO / "driver" / "cp437.c"
FONT_C = REPO / "driver" / "font8x16.c"                 # the x86 console's table: checked against
OUT_V = ROOT / "rtl" / "peripherals" / "surf_video_font.v"

# SHA-256 of every input, relative to the repository root
PINS = {
    "driver/fonts/spleen-8x16.bdf": "b38b32a66920068965a3101f98071d310c5c74659fe86e55d346140770f8f6e8",
    "driver/fonts/surfos-symbols.bdf": "550383d03f9286686e883f631c76182252631158cacd192fd1d2b8e5d98621e5",
    "driver/fonts/LICENSE-spleen": "f33fe8679d5b2abecc4f1313ce6c6bfa58262964de5f7bca146596a7318047af",
    "driver/cp437.c": "4fd2b58d28b2afda8f505c043cd13c984e3f45d541240a152faae92b4a34d8eb",
}

W, H = 8, 16


class FontError(Exception):
    pass


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_pins() -> None:
    bad = []
    for rel, want in PINS.items():
        path = REPO / rel
        if not path.is_file():
            bad.append(f"{rel}: missing")
            continue
        got = sha256(path)
        if got != want:
            bad.append(f"{rel}: SHA-256 {got}, pinned {want}")
    if bad:
        raise FontError("font sources differ from the pinned ones (review the change, then "
                        "update PINS in tools/fontrom.py):\n  " + "\n  ".join(bad))


def cp437_codepoints() -> list[int]:
    """the 256 code points of cp437_unicode[256] in driver/cp437.c, in order"""
    text = CP437.read_text(encoding="utf-8")
    m = re.search(r"cp437_unicode\s*\[\s*256\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FontError(f"{CP437}: cp437_unicode[256] not found")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", body)]
    if len(values) != 256:
        raise FontError(f"{CP437}: {len(values)} entries in cp437_unicode[], expected 256")
    return values


def read_bdf(path: Path) -> dict[int, list[int]]:
    """{code point: 16 row bytes, MSB = leftmost pixel} for an 8x16 character-cell BDF font"""
    lines = path.read_text(encoding="ascii").splitlines()
    glyphs: dict[int, list[int]] = {}
    ascent = None
    i = 0
    while i < len(lines):
        words = lines[i].split()
        if not words:
            i += 1
            continue
        if words[0] == "FONTBOUNDINGBOX":
            fw, fh, _fx, fy = (int(v) for v in words[1:5])
            if (fw, fh) != (W, H):
                raise FontError(f"{path}: cell {fw}x{fh}, expected {W}x{H}")
            ascent = fh + fy                     # rows above the baseline
        elif words[0] == "STARTCHAR":
            if ascent is None:
                raise FontError(f"{path}: STARTCHAR before FONTBOUNDINGBOX")
            code, bbx, bitmap = None, None, None
            i += 1
            while i < len(lines) and lines[i].split()[:1] != ["ENDCHAR"]:
                w = lines[i].split()
                if w and w[0] == "ENCODING":
                    code = int(w[1])
                elif w and w[0] == "BBX":
                    bbx = tuple(int(v) for v in w[1:5])
                elif w and w[0] == "BITMAP":
                    bitmap = []
                    i += 1
                    while lines[i].strip() != "ENDCHAR":
                        bitmap.append(lines[i].strip())
                        i += 1
                    break
                i += 1
            if code is None or bbx is None or bitmap is None:
                raise FontError(f"{path}: incomplete character near line {i + 1}")
            if code >= 0:
                glyphs[code] = cell_rows(path, code, bbx, bitmap, ascent)
        i += 1
    return glyphs


def cell_rows(path: Path, code: int, bbx: tuple, bitmap: list[str], ascent: int) -> list[int]:
    """place a glyph's bitmap in the 8x16 cell by its bounding box (BDF semantics)"""
    bw, bh, bx, by = bbx
    if len(bitmap) != bh:
        raise FontError(f"{path}: U+{code:04X} has {len(bitmap)} bitmap rows, BBX says {bh}")
    rows = [0] * H
    top = ascent - (by + bh)                     # cell row of the bitmap's first row
    nbits = 4 * len(bitmap[0]) if bitmap else 0  # hex digits are padded to whole bytes
    for r, hexrow in enumerate(bitmap):
        y = top + r
        value = int(hexrow, 16)
        for px in range(bw):                     # px 0 = leftmost pixel = most significant bit
            if value >> (nbits - 1 - px) & 1:
                x = bx + px
                if not (0 <= x < W and 0 <= y < H):
                    raise FontError(f"{path}: U+{code:04X} has a pixel outside the 8x16 cell")
                rows[y] |= 0x80 >> x
    return rows


def build() -> tuple[list[list[int]], list[int], list[tuple[int, int]]]:
    """(256 glyphs, their code points, [(index, code point) taken from the symbols font])"""
    check_pins()
    cps = cp437_codepoints()
    spleen = read_bdf(SPLEEN)
    symbols = read_bdf(SYMBOLS)
    table, from_symbols, missing = [], [], []
    for index, cp in enumerate(cps):
        if index == 0:
            table.append([0] * H)                # the empty cell
        elif cp in spleen:
            table.append(spleen[cp])
        elif cp in symbols:
            table.append(symbols[cp])
            from_symbols.append((index, cp))
        else:
            missing.append(f"0x{index:02X} (U+{cp:04X})")
            table.append([0] * H)
    if missing:
        raise FontError("no glyph in either font for " + ", ".join(missing))
    return table, cps, from_symbols


def read_font8x16_c() -> list[list[int]]:
    """the 256 glyphs of driver/font8x16.c (the x86 console's table)"""
    text = FONT_C.read_text(encoding="utf-8")
    entries = re.findall(r"\[0x([0-9a-fA-F]{2})\]\s*=\s*\{([^}]*)\}", text)
    table: list[list[int] | None] = [None] * 256
    for idx, body in entries:
        rows = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{1,2})", body)]
        if len(rows) != H:
            raise FontError(f"{FONT_C}: entry 0x{idx} has {len(rows)} rows")
        table[int(idx, 16)] = rows
    if any(g is None for g in table):
        raise FontError(f"{FONT_C}: not all 256 entries found")
    return table  # type: ignore[return-value]


def describe(cp: int) -> str:
    if cp == 0:
        return "U+0000 the empty cell"
    if cp == 0x20:
        return "U+0020 space"
    if cp == 0xA0:
        return "U+00A0 no-break space"
    return f"U+{cp:04X} {chr(cp)}"


def describe_ascii(cp: int) -> str:
    """describe() for the Verilog file, which stays plain ASCII for every tool"""
    if 0x21 <= cp <= 0x7E:
        return f"U+{cp:04X} {chr(cp)}"
    return describe(cp) if cp in (0, 0x20, 0xA0) else f"U+{cp:04X}"


def render_verilog(table: list[list[int]], cps: list[int], from_symbols: list[tuple[int, int]]) -> str:
    licence = LICENSE.read_text(encoding="ascii").rstrip("\n")
    sym = ", ".join(f"0x{i:02X} (U+{cp:04X})" for i, cp in from_symbols)
    out = []
    out.append("/*\nSurfCPU font ROM\n--------------------\n")
    out.append("File: surf_video_font.v Date: 10/15/26 (SurfCPU); generated by tools/fontrom.py, do not edit\n")
    out.append("--------------------\n")
    out.append(
        "surf_video's character generator: the 256 glyphs of code page 437 in the order of\n"
        "cp437_unicode[] in driver/cp437.c, 8x16 pixels, one byte per glyph row with the most\n"
        "significant bit leftmost: the same glyphs as the x86 SurfOS console's driver/font8x16.c\n"
        "(tools/fontrom.py check compares every byte). addr = {code, row}; data is the row for addr\n"
        "one clk edge later (a registered read, so it maps to one block RAM).\n\n"
        "Sources, pinned by SHA-256 in tools/fontrom.py:\n")
    for rel in PINS:
        out.append(f"  {rel}  {PINS[rel]}\n")
    out.append(
        f"Glyphs from driver/fonts/surfos-symbols.bdf (Spleen has none): {sym}.\n"
        "Index 0x00, the empty cell, is blank.\n\n"
        "The glyphs are Spleen 8x16 by Frederic Cambus (https://github.com/fcambus/spleen),\n"
        "released under the BSD 2-Clause license below, which also applies to this table; the\n"
        "SurfOS symbols are Copyright (c) 2026, Brandon Burr, under the same license.\n\n")
    out.append(licence + "\n*/\n")
    out.append("module surf_video_font (\n"
               "    input  wire        clk,\n"
               "    input  wire [11:0] addr,   // {code point index 7:0, glyph row 3:0}\n"
               "    output reg  [7:0]  data    // that row, bit 7 = leftmost pixel; one clk edge after addr\n"
               ");\n"
               "    reg [7:0] rom [0:4095];\n\n"
               "    initial begin\n")
    for index in range(256):
        out.append(f"        // 0x{index:02X} {describe_ascii(cps[index]) if index else describe(0)}\n")
        rows = table[index]
        for half in (0, 8):
            stmts = " ".join(f"rom[12'h{index * 16 + r:03x}] = 8'h{rows[r]:02x};" for r in range(half, half + 8))
            out.append(f"        {stmts}\n")
    out.append("    end\n\n"
               "    always @(posedge clk) data <= rom[addr];\n"
               "endmodule\n")
    return "".join(out)


def compare_with_c(table: list[list[int]], cps: list[int]) -> list[str]:
    ref = read_font8x16_c()
    diffs = []
    for index in range(256):
        if table[index] != ref[index]:
            diffs.append(f"0x{index:02X} {describe(cps[index])}: ROM "
                         + " ".join(f"{v:02x}" for v in table[index]) + ", font8x16.c "
                         + " ".join(f"{v:02x}" for v in ref[index]))
    return diffs


def cmd_gen() -> int:
    table, cps, from_symbols = build()
    diffs = compare_with_c(table, cps)
    if diffs:
        print("fontrom: glyphs differ from driver/font8x16.c:\n  " + "\n  ".join(diffs), file=sys.stderr)
        return 1
    text = render_verilog(table, cps, from_symbols)
    OUT_V.parent.mkdir(parents=True, exist_ok=True)
    old = OUT_V.read_text(encoding="utf-8") if OUT_V.exists() else None
    if old != text:
        OUT_V.write_text(text, encoding="utf-8")
        print(f"fontrom: wrote {OUT_V.relative_to(ROOT)} (256 glyphs, {len(from_symbols)} from the symbols font)")
    else:
        print(f"fontrom: {OUT_V.relative_to(ROOT)} is current")
    return 0


def cmd_check() -> int:
    table, cps, from_symbols = build()
    problems = []
    diffs = compare_with_c(table, cps)
    if diffs:
        problems.append("glyphs differ from driver/font8x16.c:\n    " + "\n    ".join(diffs))
    if not OUT_V.exists():
        problems.append(f"{OUT_V.relative_to(ROOT)} is missing (python3 tools/fontrom.py gen)")
    elif OUT_V.read_text(encoding="utf-8") != render_verilog(table, cps, from_symbols):
        problems.append(f"{OUT_V.relative_to(ROOT)} is out of date (python3 tools/fontrom.py gen)")
    if problems:
        print("fontrom: " + "\n  ".join(problems), file=sys.stderr)
        return 1
    blank = sum(1 for g in table if not any(g))
    print(f"fontrom: check passed: sources match their pins, {OUT_V.relative_to(ROOT)} is current, "
          f"all 256 glyphs equal driver/font8x16.c ({len(from_symbols)} from the symbols font, {blank} blank)")
    return 0


def cmd_dump() -> int:
    table, cps, _ = build()
    for index in range(256):
        print(f"0x{index:02X} {describe(cps[index])}")
        for row in table[index]:
            print("  " + "".join("#" if row & (0x80 >> x) else "." for x in range(W)))
    return 0


def main(argv: list[str]) -> int:
    cmds = {"gen": cmd_gen, "check": cmd_check, "dump": cmd_dump}
    if len(argv) != 2 or argv[1] not in cmds:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        return cmds[argv[1]]()
    except FontError as e:
        print(f"fontrom: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
