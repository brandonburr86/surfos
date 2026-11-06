#!/usr/bin/env python3
"""
SurfOS user library: libsurf's JPEG decoder against another
----------------------
File: test_image_jpeg.py  Date: 11/05/26
----------------------
Each JPEG of tests/fixtures/wundertow/images/ decoded by libsurf (user/libsurf/image_jpeg.c, through
"test_wundertow_image --dump FILE RAW") and by macOS's sips, an independent decoder, and their pixels compared
channel by channel. libsurf's must be within these of sips's:

  - full chroma (grey, every component sampled alike): 3 at most, 0.5 on average;
  - chroma sampled less than luma: 8 at most, 3 on average. Here chroma is widened by repeating it; sips smooths it
    as it widens it, so the two part where the colour changes.

Not compared: pictures of four components (CMYK and YCCK: sips converts them through a colour profile, where libsurf
reads the inverted CMYK browsers read); ownjfifadobe0.jpg and ownrgbnames.jpg (whether three components are RGB or
YCbCr: libsurf follows libjpeg's rule for the JFIF and Adobe markers and the components' names, which sips does not
for these two); ownjfifshort.jpg (sips refuses it). Without sips (anywhere but the Mac) the test says so and passes.

    python3 tests/test_image_jpeg.py build/O0/host/test_wundertow_image tests/fixtures

Exit status 0 when every check passes, 1 when one fails, 2 when the test cannot run.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

ASIDE = {
    'ownjfifadobe0.jpg': 'JFIF with an Adobe marker of transform 0: YCbCr here (libjpeg\'s rule), RGB to sips',
    'ownrgbnames.jpg': 'components named R, G and B, no marker: RGB here (libjpeg\'s rule), YCbCr to sips',
    'ownjfifshort.jpg': 'a JFIF marker too short to be one: sips refuses the file',
}

checks = 0
failures = 0


def check(ok, what):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print('FAIL', what)


def frame(data):
    """the components of the file's frame: (count, whether they are all sampled alike), or None"""
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xff:
            return None
        marker, n = data[i + 1], struct.unpack_from('>H', data, i + 2)[0]
        if marker in (0xc0, 0xc1, 0xc2):
            comps = data[i + 9]
            sampling = {data[i + 11 + 3 * k] for k in range(comps)}
            return comps, len(sampling) == 1
        i += 2 + n
    return None


def ours(tool, path, raw):
    out = subprocess.run([tool, '--dump', path, raw], capture_output=True, text=True)
    size = out.stdout.split(': ')[-1].split(',')[0]
    if 'x' not in size or not os.path.exists(raw):
        return None
    w, h = map(int, size.split('x'))
    d = open(raw, 'rb').read()
    return w, h, [d[i + 2 - k] for i in range(0, len(d), 4) for k in range(3)]       # B G R A: red, green, blue


def sips(path, bmp):
    if subprocess.run(['sips', '-s', 'format', 'bmp', path, '--out', bmp], capture_output=True).returncode:
        return None
    d = open(bmp, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    top, h = h < 0, abs(h)
    stride = (w * bpp // 8 + 3) & ~3
    px = []
    for y in range(h):
        row = off + (y if top else h - 1 - y) * stride
        for x in range(w):
            p = row + x * bpp // 8
            px += [d[p + 2], d[p + 1], d[p]]
    return w, h, px


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[-3].strip())
        return 2
    tool, fixtures = sys.argv[1], os.path.join(sys.argv[2], 'wundertow', 'images')
    if not shutil.which('sips'):
        print('sips: not on this machine (it is the Mac\'s): the JPEG decoder is not compared with it here')
        return 0
    tmp = tempfile.mkdtemp(prefix='surfos-jpeg-')
    try:
        names = sorted(n for n in os.listdir(fixtures) if n.endswith('.jpg'))
        for name in names:
            path = os.path.join(fixtures, name)
            data = open(path, 'rb').read()
            f = frame(data)
            if name in ASIDE or not f or f[0] == 4:
                continue
            mine = ours(tool, path, os.path.join(tmp, 'mine.raw'))
            theirs = sips(path, os.path.join(tmp, 'sips.bmp'))
            check(mine is not None and theirs is not None, f'{name}: both decode it')
            if mine is None or theirs is None:
                continue
            check(mine[:2] == theirs[:2], f'{name}: the same size ({mine[0]}x{mine[1]}, {theirs[0]}x{theirs[1]})')
            if mine[:2] != theirs[:2]:
                continue
            apart = [abs(a - b) for a, b in zip(mine[2], theirs[2])]
            worst, mean = max(apart), sum(apart) / len(apart)
            most, average = (3, 0.5) if f[1] else (8, 3.0)
            check(worst <= most and mean <= average,
                  f'{name}: within {most} of sips, {average} on average (the worst {worst}, {mean:.2f} on average)')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if failures:
        print(f'IMAGE JPEG TESTS FAILED: {failures} of {checks} checks')
        return 1
    print(f'IMAGE JPEG TESTS PASSED: {checks} checks, 0 failures (against sips)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
