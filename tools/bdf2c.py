#!/usr/bin/env python3
"""Turn BDF bitmap fonts into the C tables the framebuffer console draws with.

    tools/bdf2c.py driver/fonts/spleen-8x16.bdf driver/fonts/surfos-symbols.bdf > driver/font8x16.c
    tools/bdf2c.py driver/fonts/spleen-12x24.bdf driver/fonts/surfos-symbols-12x24.bdf > driver/font12x24.c
    tools/bdf2c.py driver/fonts/spleen-16x32.bdf x2:driver/fonts/surfos-symbols.bdf > driver/font16x32.c

A console cell holds one byte, a code page 437 character (10/6/26). The table has all 256
of them in that order: the code point of each index comes from cp437_unicode[] in
driver/cp437.c, so the font and the console agree (SurfOS's U+25B8, U+224B and U+2026 at
0x10, 0xF7 and 0xF9 included). Each glyph is taken from the first font that has it: Spleen,
then SurfOS's own symbols for what Spleen lacks, which are listed on stderr. Index 0x00,
the empty cell, is blank. Every entry carries a comment naming its code point
("/* U+25B8 ▸ */"): tools/qemu-usb.py reads them to recognise the characters in
screenshots. The glyph rows are placed by the BDF bounding boxes, so a glyph that is
shorter than the cell sits on the font's baseline. Run by hand when a font changes; kept so
the table can be checked against, or regenerated from, the fonts it came from.

10/25/26 (track A, A7): any cell size up to 16 pixels wide, the first font's FONTBOUNDINGBOX;
the others must have the same cell, or half of it with x2: before their name, each of their
pixels then two by two (the 16x32 table's SurfOS symbols are the 8x16 ones doubled). A row of
8 pixels or fewer is one byte, a wider one two (u16), the most significant bit leftmost. Each
table ends with its struct fb_font (include/sys/framebuffer.h), the font as the console takes it.
"""
import os
import re
import sys

CP437_C = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'driver', 'cp437.c')


def cp437_table(path=CP437_C):
    """the 256 code points of cp437_unicode[] in driver/cp437.c"""
    src = open(path, encoding='utf-8').read()
    m = re.search(r'cp437_unicode\[256\] = \{(.*?)\};', src, re.S)
    if not m:
        sys.exit(f'{path}: no cp437_unicode[256] table')
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)
    table = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]+)', body)]
    if len(table) != 256:
        sys.exit(f'{path}: cp437_unicode[] has {len(table)} entries, not 256')
    return table


def parse(path):
    """the cell's width and height, and {code point: its rows}, each row width bits with the leftmost pixel highest"""
    glyphs = {}
    width = height = ascent = None
    with open(path, encoding='ascii') as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        words = lines[i].split()
        if words and words[0] == 'FONTBOUNDINGBOX':
            width, height, _, yoff = map(int, words[1:5])
            if width > 16:
                sys.exit(f'{path}: font is {width} pixels wide, more than 16')
            ascent = height + yoff
        elif words and words[0] == 'STARTCHAR':
            code, bbx, rows = None, None, []
            i += 1
            while lines[i] != 'ENDCHAR':
                w = lines[i].split()
                if w[0] == 'ENCODING':
                    code = int(w[1])
                elif w[0] == 'BBX':
                    bbx = tuple(map(int, w[1:5]))
                elif w[0] == 'BITMAP':
                    i += 1
                    while lines[i] != 'ENDCHAR':
                        rows.append(int(lines[i], 16))
                        i += 1
                    break
                i += 1
            if code is not None and code >= 0:
                glyphs[code] = place(bbx, rows, ascent, width, height)
        i += 1
    return width, height, glyphs


def place(bbx, rows, ascent, width, height):
    bw, bh, bx, by = bbx
    cell = [0] * height
    top = ascent - (by + bh)                 # first cell row of the bitmap
    bytes_per_row = (bw + 7) // 8
    for r, value in enumerate(rows):
        y = top + r
        if 0 <= y < height:
            value >>= 8 * bytes_per_row - bw  # drop the padding bits
            cell[y] = ((value << (width - bw)) >> bx) & ((1 << width) - 1) if bw <= width else value >> (bw - width)
    return cell


def doubled(rows, width):
    """a glyph twice as wide and tall: each pixel two by two"""
    out = []
    for value in rows:
        wide = 0
        for x in range(width):
            if value & (1 << (width - 1 - x)):
                wide |= 3 << (2 * (width - 1 - x))
        out += [wide, wide]
    return out


def describe(u):
    """the comment of an entry: the code point, and the character where it shows"""
    if u == 0:
        return 'U+0000 the empty cell'
    if u == 0x20:
        return 'U+0020 space'
    if u == 0xA0:
        return 'U+00A0 no-break space'
    return f'U+{u:04X} {chr(u)}'


def wrapped(text, first, width=90):
    """text in lines of at most width columns, the first of them first columns shorter (it follows other words)"""
    lines, line, room = [], '', width - first
    for word in text.split(' '):
        if line and len(line) + 1 + len(word) > room:
            lines.append(line)
            line, room = word, width
        else:
            line = f'{line} {word}' if line else word
    return '\n'.join(lines + [line])


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fonts = []
    for arg in sys.argv[1:]:
        twice = arg.startswith('x2:')
        path = arg[3:] if twice else arg
        w, h, glyphs = parse(path)
        if fonts:
            want = (fonts[0][2] // 2, fonts[0][3] // 2) if twice else (fonts[0][2], fonts[0][3])
            if (w, h) != want:
                sys.exit(f'{path}: font is {w}x{h}, not {want[0]}x{want[1]}')
            if twice:
                glyphs = {u: doubled(rows, w) for u, rows in glyphs.items()}
        fonts.append((os.path.basename(path), glyphs, w, h, twice))
    width, height = fonts[0][2], fonts[0][3]
    size = f'{width}x{height}'
    table = cp437_table()
    rows, taken, missing = [], [], []
    for index, u in enumerate(table):
        if index == 0:
            rows.append([0] * height)
            continue
        for n, (name, glyphs, _, _, _) in enumerate(fonts):
            if u in glyphs:
                rows.append(glyphs[u])
                if n:
                    taken.append((index, u, name))
                break
        else:
            missing.append(f'0x{index:02X} {describe(u)}')
    if missing:
        sys.exit('no glyph in ' + ', '.join(f[0] for f in fonts) + ' for: ' + '; '.join(missing))
    for index, u, name in taken:
        print(f'bdf2c: {fonts[0][0]} has no glyph for 0x{index:02X} {describe(u)}: taken from {name}', file=sys.stderr)
    twice = {f[0] for f in fonts if f[4]}
    others = sorted({name for _, _, name in taken})
    out = sys.stdout
    out.write('/*\nSurfOS Console Font\n--------------------\n')
    if size == '8x16':
        out.write('File: font8x16.c  Date: 10/5/26 (USB boot), 10/6/26 (code page 437); generated by tools/bdf2c.py\n')
    else:
        out.write(f'File: font{size}.c  Date: 10/25/26 (track A); generated by tools/bdf2c.py\n')
    out.write('--------------------\n')
    out.write(f'{size} glyphs for the framebuffer console, {"one byte" if width <= 8 else "two bytes"} per row, '
              'the most significant bit\n')
    out.write('leftmost: the 256 characters of code page 437 in its order (driver/cp437.c), so a cell\n')
    out.write('shows on a framebuffer what VGA text mode shows, with SurfOS\'s symbols at 0x10, 0xF7\n')
    out.write('and 0xF9. Each entry names its code point; index 0x00, the empty cell, is blank.\n\n')
    out.write(f'The glyphs are Spleen {size} by Frederic Cambus (https://github.com/fcambus/spleen),\n')
    out.write('Copyright (c) 2018-2026, Frederic Cambus, under the BSD 2-Clause license: see\n')
    out.write('driver/fonts/LICENSE-spleen, which also applies to this table.')
    if taken:
        names = [f'U+{u:04X}' for _, u, _ in taken]
        names = ', '.join(names[:-1]) + ' and ' + names[-1] if len(names) > 1 else names[0]
        where = ('for ' + names + '; ' + ('they come' if len(taken) > 1 else 'it comes') + ' from ' +
                 ' and '.join(f'driver/fonts/{n}' + (' (each pixel two by two)' if n in twice else '') for n in others) +
                 ',')
        drawn = 'drawn for SurfOS in Spleen\'s manner, Copyright (c) 2026, Brandon Burr, under the same'
        if len(where) <= 90:
            out.write(' Spleen has no glyph\n' + where + '\n' + drawn + '\nlicense.')
        else:
            out.write(' Spleen has no glyph\n' + wrapped(where + ' ' + drawn + ' license.', 0))
    out.write('\n*/\n\n')
    out.write('#include <surfos/types.h>\n#include <sys/framebuffer.h>\n\n')
    if size == '8x16':
        out.write('const u8 font8x16[256][FONT_HEIGHT] = {\n')
    else:
        out.write(f'const {"u8" if width <= 8 else "u16"} font{size}[256][{height}] = {{\n')
    shift, digits = (8 - width, 2) if width <= 8 else (16 - width, 4)
    for index, u in enumerate(table):
        bits = ', '.join(f'0x{v << shift:0{digits}x}' for v in rows[index])
        out.write(f'    [0x{index:02x}] = {{ {bits} }}, /* {describe(u)} */\n')
    out.write('};\n')
    out.write('\n/* 10/25/26 (track A, A7): the font as the framebuffer console takes it (include/sys/framebuffer.h) */\n')
    eight, sixteen = (f'&font{size}[0][0]', '0') if width <= 8 else ('0', f'&font{size}[0][0]')
    out.write(f'const struct fb_font fb_font_{size} = {{ "{size}", {width}, {height}, {eight}, {sixteen} }};\n')


if __name__ == '__main__':
    main()
