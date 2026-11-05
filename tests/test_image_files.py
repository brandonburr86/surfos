#!/usr/bin/env python3
"""
SurfOS user library: the pictures libsurf writes, read by other programs
----------------------
File: test_image_files.py  Date: 11/05/26
----------------------
tests/host/test_image_save.c, run as "test_image_save --write DIR", leaves the pictures it checks in DIR (NAME.bmp,
NAME.png and NAME-rgba.png, NAME being KIND-WxH) and each one's pixels (NAME.rgba: red, green, blue and alpha, rows
from the top). This reads them without libsurf: a PNG's chunks with Python's zlib (each CRC by zlib.crc32, the data
by zlib.decompress, which checks its Adler-32 too), its rows unfiltered as the PNG standard's section 9.2 says; a
BMP's headers and rows with struct. Where macOS's sips is (the Mac), sips reads every file as well: it says each
one's size, kind and alpha, and converts the PNGs to BMPs and the BMPs to PNGs, whose pixels must be the same.

    python3 tests/test_image_files.py build/O0/host/test_image_save

Exit status 0 when every check passes, 1 when one fails, 2 when the test cannot run.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

checks = 0
failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print('FAIL', what)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """(width, height, channels, pixels as bytes of 8-bit samples) of a PNG of 8 bits, not interlaced, of colour type
    0, 2, 3, 4 or 6; raises ValueError on anything else or any damage"""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('no PNG signature')
    at, idat, ihdr, palette, trns = 8, b'', None, None, None
    while at < len(data):
        if len(data) - at < 12:
            raise ValueError('a chunk cut short')
        n, = struct.unpack('>I', data[at:at + 4])
        kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + n]
        if len(body) != n or len(data) < at + 12 + n:
            raise ValueError('a chunk cut short')
        crc, = struct.unpack('>I', data[at + 8 + n:at + 12 + n])
        if zlib.crc32(kind + body) & 0xffffffff != crc:
            raise ValueError(f'chunk {kind!r}: its CRC is wrong')
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            if at + 12 + n != len(data):
                raise ValueError('bytes after IEND')
            break
        at += 12 + n
    else:
        raise ValueError('no IEND')
    w, h, depth, color, comp, filt, lace = ihdr
    if depth != 8 or comp or filt or lace or color not in (0, 2, 3, 4, 6):
        raise ValueError(f'a PNG this test does not read: {ihdr}')
    raw = zlib.decompress(idat)                     # the Adler-32 is zlib's to check
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    n = w * bpp
    if len(raw) != (n + 1) * h:
        raise ValueError(f'{len(raw)} bytes of rows, not {(n + 1) * h}')
    out, up = bytearray(), bytearray(n)
    for y in range(h):
        f, row = raw[y * (n + 1)], bytearray(raw[y * (n + 1) + 1:(y + 1) * (n + 1)])
        if f > 4:
            raise ValueError(f'row {y}: filter {f}')
        for i in range(n):
            a = row[i - bpp] if i >= bpp else 0
            b = up[i]
            c = up[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[f]
            row[i] = (row[i] + pred) & 255
        out += row
        up = row
    channels = 4 if color in (4, 6) or (color == 3 and trns) else 3
    pixels = bytearray()
    for i in range(w * h):
        if color == 0:
            pixels += bytes((out[i],) * 3)
        elif color == 4:
            pixels += bytes((out[2 * i],) * 3) + bytes((out[2 * i + 1],))
        elif color == 3:
            k = out[i]
            pixels += palette[3 * k:3 * k + 3]
            if channels == 4:
                pixels.append(trns[k] if k < len(trns) else 255)
        else:
            pixels += out[i * bpp:(i + 1) * bpp]
    return w, h, channels, bytes(pixels)


def read_bmp(path):
    """(width, height, channels, pixels as RGB(A) bytes, rows from the top) of a BMP of 24 bits (BI_RGB) or 32 bits
    (BI_RGB or BI_BITFIELDS), its rows from the bottom or the top"""
    data = open(path, 'rb').read()
    if data[:2] != b'BM':
        raise ValueError('no BM')
    off, = struct.unpack('<I', data[10:14])
    hsize, w, h, planes, bits, comp = struct.unpack('<IiiHHI', data[14:34])
    if planes != 1 or bits not in (24, 32) or comp not in (0, 3) or hsize < 40:
        raise ValueError(f'a BMP this test does not read: {bits} bits, compression {comp}')
    masks = (0xff0000, 0xff00, 0xff, 0)
    if comp == 3:
        masks = struct.unpack('<IIII', data[54:70]) if hsize >= 56 else struct.unpack('<III', data[54:66]) + (0,)
    top = h < 0
    h = abs(h)
    row = (w * bits // 8 + 3) & ~3
    if off + row * h > len(data):
        raise ValueError('rows past the file')
    channels = 4 if bits == 32 and masks[3] else 3

    def field(v, mask):
        if not mask:
            return 255
        shift = (mask & -mask).bit_length() - 1
        size = (mask >> shift).bit_length()
        return ((v & mask) >> shift) * 255 // ((1 << size) - 1)

    pixels = bytearray()
    for y in range(h):
        r = data[off + (y if top else h - 1 - y) * row:]
        for x in range(w):
            if bits == 24:
                b, g, rr = r[3 * x:3 * x + 3]
                pixels += bytes((rr, g, b))
            else:
                v, = struct.unpack('<I', r[4 * x:4 * x + 4])
                pixels += bytes(field(v, masks[k]) for k in range(channels))
    return w, h, channels, bytes(pixels)


def same(expected, w, h, channels, pixels, alpha):
    """expected (RGBA bytes) against pixels of channels each; the alpha only when alpha"""
    if len(pixels) != w * h * channels:
        return False
    for i in range(w * h):
        if pixels[i * channels:i * channels + 3] != expected[i * 4:i * 4 + 3]:
            return False
        if alpha and channels == 4 and pixels[i * channels + 3] != expected[i * 4 + 3]:
            return False
    return True


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().split('\n\n')[1])
        return 2
    tool = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix='surfos-img-') as d:
        r = subprocess.run([tool, '--write', d], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr)
            print('test_image_save --write failed')
            return 2
        names = sorted(n[:-5] for n in os.listdir(d) if n.endswith('.rgba'))
        check(len(names) >= 80, f'test_image_save wrote its pictures ({len(names)})')
        files = []
        for name in names:
            m = re.fullmatch(r'[a-z]+-(\d+)x(\d+)', name)
            w, h = int(m.group(1)), int(m.group(2))
            expected = open(os.path.join(d, name + '.rgba'), 'rb').read()
            check(len(expected) == w * h * 4, f'{name}.rgba holds its pixels')
            for ext, reader, alpha in (('.bmp', read_bmp, False), ('.png', read_png, False), ('-rgba.png', read_png, True)):
                path = os.path.join(d, name + ext)
                files.append((path, name + ext, w, h, expected, alpha))
                try:
                    pw, ph, channels, pixels = reader(path)
                except (ValueError, zlib.error, struct.error) as e:
                    check(False, f'{name}{ext} reads: {e}')
                    continue
                check((pw, ph) == (w, h), f'{name}{ext} is {w}x{h}')
                check(channels == (4 if alpha else 3), f'{name}{ext} has {4 if alpha else 3} channels')
                check(same(expected, w, h, channels, pixels, alpha), f'{name}{ext}: every pixel as written')
        sips = shutil.which('sips')
        if not sips:
            print('sips: not on this machine (it is the Mac\'s): the files were read by Python alone')
        else:
            sips_checks(sips, d, files)
    if failures:
        print(f'IMAGE FILE TESTS FAILED: {failures} of {checks} checks')
        return 1
    print(f'IMAGE FILE TESTS PASSED: {checks} checks, 0 failures' + ('' if shutil.which('sips') else ' (no sips)'))
    return 0


def sips_checks(sips, d, files):
    """sips says each file's size, kind and alpha, and converts each to the other kind; the pixels must not change"""
    paths = [f[0] for f in files]
    out = subprocess.run([sips, '-g', 'pixelWidth', '-g', 'pixelHeight', '-g', 'format', '-g', 'hasAlpha'] + paths,
                         capture_output=True, text=True).stdout
    said, current = {}, None
    for line in out.splitlines():
        if not line.startswith(' '):                 # the file's path, which sips may give resolved: by its name
            current = said.setdefault(os.path.basename(line.strip()), {})
        elif current is not None and ':' in line:
            k, v = line.strip().split(':', 1)
            current[k] = v.strip()
    for path, label, w, h, expected, alpha in files:
        p = said.get(os.path.basename(path), {})
        kind = 'bmp' if label.endswith('.bmp') else 'png'
        check(p.get('pixelWidth') == str(w) and p.get('pixelHeight') == str(h), f'sips: {label} is {w}x{h}')
        check(p.get('format') == kind, f'sips: {label} is a {kind}')
        check(p.get('hasAlpha') == ('yes' if alpha else 'no'), f'sips: {label} {"has" if alpha else "has no"} alpha')
    for kind, ext, reader in (('bmp', '.png', read_bmp), ('png', '.bmp', read_png)):
        todo = [f for f in files if f[1].endswith(ext)]
        outdir = os.path.join(d, 'sips-' + kind)
        os.mkdir(outdir)
        subprocess.run([sips, '-s', 'format', kind] + [f[0] for f in todo] + ['--out', outdir], capture_output=True)
        for path, label, w, h, expected, alpha in todo:
            conv = os.path.join(outdir, os.path.splitext(os.path.basename(path))[0] + '.' + kind)
            try:
                pw, ph, channels, pixels = reader(conv)
            except (OSError, ValueError, zlib.error, struct.error) as e:
                check(False, f'sips: {label} as a {kind}: {e}')
                continue
            check((pw, ph) == (w, h), f'sips: {label} as a {kind} is {w}x{h}')
            if alpha:                               # a pixel of alpha 0 has no colour sips need keep
                expected = bytes(b for i in range(w * h) for b in
                                 (expected[i * 4:i * 4 + 4] if expected[i * 4 + 3] else bytes(4)))
                pixels = bytes(b for i in range(w * h) for b in
                               (pixels[i * channels:i * channels + channels] if channels == 3 or pixels[i * channels + 3]
                                else bytes(channels)))
            check(same(expected, w, h, channels, pixels, alpha), f'sips: {label} as a {kind}: every pixel as written')


if __name__ == '__main__':
    sys.exit(main())
