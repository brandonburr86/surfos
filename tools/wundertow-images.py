#!/usr/bin/env python3
"""The test pictures of the web browser's decoders (user/wundertow/image*.c, tests/host/test_wundertow_image.c): writes
tests/fixtures/wundertow/images/ and its manifest, one line a file:

    NAME WIDTH HEIGHT exact HASH     every pixel as image.h has them; HASH is FNV-1a of them, little-endian
    NAME WIDTH HEIGHT near TOLERANCE each channel within TOLERANCE of the pattern (JPEG is lossy)

Everything but one family comes from this file, byte by byte, so that the fixtures hold what no ordinary encoder
writes: PNG in every colour type and depth, each filter, stored, fixed and dynamic deflate, Adam7, tRNS, IDAT cut
anywhere; GIF with local tables, interlacing, transparency, a frame off the screen's corner, a table that fills up;
BMP in every header and depth, bit fields, RLE. jpeg_encode() below is a small baseline encoder for the samplings and
the headers libjpeg does not write (4x1, 1x2, 3x1, RGB, a 16-bit quantization table, several scans, restarts).

The rest of the JPEGs are real encoders' files: Pillow's libjpeg-turbo, baseline and progressive. The files are
committed, so tests need neither this script nor Pillow; run it only to change them:

    python3 tools/wundertow-images.py [DIR]         (Pillow in that python's site-packages; no network)

It writes the same bytes every time for the same Pillow."""
import os
import struct
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'tests', 'fixtures', 'wundertow',
                                                         'images')
W, H = 23, 17               # a size no block or pass divides
manifest = []


def pattern(x, y, w, h):
    """the colour at (x, y): smooth enough for JPEG; test_wundertow_image.c's pattern() is the same"""
    return x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), (x + y) * 255 // max(w + h - 2, 1)


def fnv(px):
    h = 2166136261
    for p in px:
        for b in struct.pack('<I', p):
            h = ((h ^ b) * 16777619) & 0xffffffff
    return h


def save(name, data, w, h, px=None, tolerance=None):
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(data)
    manifest.append(f'{name} {w} {h} exact {fnv(px):08x}' if px is not None else f'{name} {w} {h} near {tolerance}')


def solid(r, g, b, a=255):
    return (255 - a) << 24 | r << 16 | g << 8 | b


# -- PNG ---------------------------------------------------------------------------------------------------------------

def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))


ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def pack(samples, depth):
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b''.join(struct.pack('>H', s) for s in samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = acc << depth | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filtered(row, up, bpp, kind):
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = up[i] if up else 0
        c = up[i - bpp] if up and i >= bpp else 0
        pred = (0, a, b, (a + b) // 2, paeth(a, b, c))[kind]
        out.append((v - pred) & 255)
    return bytes(out)


def scan(rows, channels, depth):
    """filtered rows, each by the next filter in turn; rows: lists of samples"""
    bpp = max(1, channels * depth // 8)
    out, up = bytearray(), None
    for n, r in enumerate(rows):
        raw = pack(r, depth)
        kind = n % 5
        out.append(kind)
        out += filtered(raw, up, bpp, kind)
        up = raw
    return bytes(out)


def png(sample, w, h, color, depth, interlace=False, extra=b'', level=9, strategy=zlib.Z_DEFAULT_STRATEGY, cut=None):
    """sample(x, y): the samples of a pixel"""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    if interlace:
        data = bytearray()
        for x0, y0, dx, dy in ADAM7:
            rows = [[v for x in range(x0, w, dx) for v in sample(x, y)] for y in range(y0, h, dy)]
            if rows and rows[0]:
                data += scan(rows, channels, depth)
    else:
        data = scan([[v for x in range(w) for v in sample(x, y)] for y in range(h)], channels, depth)
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    body = c.compress(bytes(data)) + c.flush()
    head = chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, color, 0, 0, 1 if interlace else 0))
    if cut:
        idat = b''.join(chunk(b'IDAT', body[i:i + cut]) for i in range(0, len(body), cut))
    else:
        idat = chunk(b'IDAT', body)
    return b'\x89PNG\r\n\x1a\n' + head + extra + idat + chunk(b'IEND', b'')


def low(v, depth):
    return v >> (8 - depth)


def scaled(v, depth):
    return v * 255 // ((1 << depth) - 1)


def gray_png(depth, interlace):
    px = []
    for y in range(H):
        for x in range(W):
            v = (x * 255 // (W - 1))
            s = v << 8 | v if depth == 16 else low(v, depth)
            px.append((s >> 8 if depth == 16 else scaled(s, depth)) * 0x010101)
    name = f'gray{depth}{"-adam7" if interlace else ""}.png'
    data = png(lambda x, y: [(lambda v: v << 8 | v if depth == 16 else low(v, depth))(x * 255 // (W - 1))], W, H, 0, depth,
               interlace)
    save(name, data, W, H, px)


def rgb_png(depth, interlace):
    def sample(x, y):
        r, g, b = pattern(x, y, W, H)
        return [c << 8 | c for c in (r, g, b)] if depth == 16 else [r, g, b]
    px = [solid(*pattern(x, y, W, H)) for y in range(H) for x in range(W)]
    save(f'rgb{depth}{"-adam7" if interlace else ""}.png', png(sample, W, H, 2, depth, interlace), W, H, px)


def pal_png(depth, interlace):
    n = 1 << depth
    entries = [(i * 255 // (n - 1), 255 - i * 255 // (n - 1), i * 37 & 255) for i in range(n)]
    plte = chunk(b'PLTE', bytes(v for e in entries for v in e))

    def index(x, y):
        return (x + y) % n
    px = [solid(*entries[index(x, y)]) for y in range(H) for x in range(W)]
    save(f'pal{depth}{"-adam7" if interlace else ""}.png', png(lambda x, y: [index(x, y)], W, H, 3, depth, interlace, plte), W,
         H, px)


def alpha(x, y):
    return (x * 3 + y * 5) * 255 // (3 * (W - 1) + 5 * (H - 1))


def ga_png(depth, interlace):
    def sample(x, y):
        g, a = x * 255 // (W - 1), alpha(x, y)
        return [g << 8 | g, a << 8 | a] if depth == 16 else [g, a]
    px = [solid(x * 255 // (W - 1), x * 255 // (W - 1), x * 255 // (W - 1), alpha(x, y)) for y in range(H) for x in range(W)]
    save(f'ga{depth}{"-adam7" if interlace else ""}.png', png(sample, W, H, 4, depth, interlace), W, H, px)


def rgba_png(depth, interlace):
    def sample(x, y):
        r, g, b = pattern(x, y, W, H)
        a = alpha(x, y)
        return [c << 8 | c for c in (r, g, b, a)] if depth == 16 else [r, g, b, a]
    px = [solid(*pattern(x, y, W, H), alpha(x, y)) for y in range(H) for x in range(W)]
    save(f'rgba{depth}{"-adam7" if interlace else ""}.png', png(sample, W, H, 6, depth, interlace), W, H, px)


def png_family():
    for interlace in (False, True):
        for depth in (1, 2, 4, 8, 16):
            gray_png(depth, interlace)
        for depth in (8, 16):
            rgb_png(depth, interlace)
            ga_png(depth, interlace)
            rgba_png(depth, interlace)
        for depth in (1, 2, 4, 8):
            pal_png(depth, interlace)
    # the deflate kinds
    px = [solid(*pattern(x, y, W, H)) for y in range(H) for x in range(W)]
    sample = lambda x, y: list(pattern(x, y, W, H))
    save('deflate-stored.png', png(sample, W, H, 2, 8, level=0), W, H, px)
    save('deflate-fixed.png', png(sample, W, H, 2, 8, strategy=zlib.Z_FIXED), W, H, px)
    save('deflate-filtered.png', png(sample, W, H, 2, 8, strategy=zlib.Z_FILTERED), W, H, px)
    # one byte in each IDAT; an empty IDAT first
    save('idat-bytes.png', png(sample, W, H, 2, 8, cut=1), W, H, px)
    body = png(sample, W, H, 2, 8)
    at = body.index(b'IDAT') - 4
    save('idat-empty.png', body[:at] + chunk(b'IDAT', b'') + body[at:], W, H, px)
    # ancillary chunks of every place, and one of a kind nobody knows, are passed over
    txt = chunk(b'tEXt', b'Comment\0nothing here') + chunk(b'zzZz', b'unknown but safe to skip')
    save('ancillary.png', png(sample, W, H, 2, 8, extra=txt), W, H, px)
    # tRNS
    key = pattern(5, 3, W, H)
    tr = chunk(b'tRNS', struct.pack('>HHH', *key))
    px2 = [solid(*pattern(xx, yy, W, H)) | (0xff000000 if pattern(xx, yy, W, H) == key else 0) for yy in range(H) for xx in range(W)]
    save('trns-rgb.png', png(sample, W, H, 2, 8, extra=tr), W, H, px2)
    tr = chunk(b'tRNS', struct.pack('>H', 0x55))
    gs = lambda x, y: [x * 255 // (W - 1)]
    px2 = [((x * 255 // (W - 1)) * 0x010101) | (0xff000000 if x * 255 // (W - 1) == 0x55 else 0) for y in range(H) for x in range(W)]
    save('trns-gray.png', png(gs, W, H, 0, 8, extra=tr), W, H, px2)
    entries = [(i * 17, 255 - i * 17, 128) for i in range(16)]
    alphas = [255 - i * 17 for i in range(10)]              # fewer than the palette's: the rest are solid
    extra = chunk(b'PLTE', bytes(v for e in entries for v in e)) + chunk(b'tRNS', bytes(alphas))
    px2 = [solid(*entries[(x + y) % 16], alphas[(x + y) % 16] if (x + y) % 16 < 10 else 255) for y in range(H) for x in range(W)]
    save('trns-pal.png', png(lambda x, y: [(x + y) % 16], W, H, 3, 4, extra=extra), W, H, px2)
    # one pixel, every Adam7 pass but the first empty
    save('one-pixel.png', png(lambda x, y: [9, 8, 7], 1, 1, 2, 8, interlace=True), 1, 1, [solid(9, 8, 7)])
    # bytes far apart: matches across the whole 32 KB window, more than a window of data
    period = 28000
    noise = [(i * i * 7 + i // 3 * 31) & 7 for i in range(period)]
    w, h = 300, 200
    far = lambda x, y: [noise[(y * w + x) * 3 % period] * 30, noise[((y * w + x) * 3 + 1) % period] * 30,
                        noise[((y * w + x) * 3 + 2) % period] * 30]
    px2 = [solid(*far(x, y)) for y in range(h) for x in range(w)]
    save('window.png', png(far, w, h, 2, 8), w, h, px2)


# -- GIF ---------------------------------------------------------------------------------------------------------------

def lzw(indices, root, clear_every=None, no_table_clear=False):
    clear, stop = 1 << root, (1 << root) + 1
    table, size, nxt = {}, root + 1, stop + 1
    out, acc, nbits = bytearray(), 0, 0

    def put(code):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(acc & 255)
            acc >>= 8
            nbits -= 8
    put(clear)
    prefix, emitted = None, 0
    for v in indices:
        if prefix is None:
            prefix = (v,)
            continue
        s = prefix + (v,)
        if len(s) == 1 or s in table:
            prefix = s
            continue
        put(prefix[0] if len(prefix) == 1 else table[prefix])
        emitted += 1
        if nxt < 4096:
            table[s] = nxt
            nxt += 1
            if nxt == (1 << size) + 1 and size < 12:
                size += 1
        elif not no_table_clear:
            put(clear)
            table, size, nxt = {}, root + 1, stop + 1
        if clear_every and emitted % clear_every == 0:
            put(clear)
            table, size, nxt = {}, root + 1, stop + 1
        prefix = (v,)
    put(prefix[0] if len(prefix) == 1 else table[prefix])
    put(stop)
    if nbits:
        out.append(acc & 255)
    return bytes(out)


def blocks(data, size=255):
    return b''.join(bytes([len(data[i:i + size])]) + data[i:i + size] for i in range(0, len(data), size)) + b'\0'


def gif_table(entries, bits):
    return bytes(v for e in entries for v in e) + bytes(3 * ((1 << bits) - len(entries)))


def gif_frame(indices, w, h, root, x=0, y=0, local=None, bits=0, interlace=False, **kw):
    packed = (0x80 | (bits - 1) if local else 0) | (0x40 if interlace else 0)
    head = b'\x2c' + struct.pack('<HHHHB', x, y, w, h, packed) + (gif_table(local, bits) if local else b'')
    return head + bytes([root]) + blocks(lzw(indices, root, **{k: v for k, v in kw.items() if k != 'size'}), kw.get('size', 255))


def gce(transparent=None):
    return b'\x21\xf9\x04' + bytes([1 if transparent is not None else 0]) + b'\0\0' + bytes([transparent or 0]) + b'\0'


def gif_family():
    entries = [(i * 255 // 15, 255 - i * 255 // 15, i * 37 & 255) for i in range(16)]
    index = lambda x, y: (x * 3 + y * 5) % 16
    idx = [index(x, y) for y in range(H) for x in range(W)]
    px = [solid(*entries[v]) for v in idx]
    head = lambda sig, w, h, packed: sig + struct.pack('<HHBBB', w, h, packed, 0, 0)
    table = gif_table(entries, 4)
    plain = head(b'GIF89a', W, H, 0xf3) + table + gif_frame(idx, W, H, 4) + b'\x3b'
    save('plain.gif', plain, W, H, px)
    save('old.gif', head(b'GIF87a', W, H, 0xf3) + table + gif_frame(idx, W, H, 4) + b'\x3b', W, H, px)
    # a local table, none on the file; comment and application blocks first; a second frame that does not count
    other = [(9, 9, 9)] * 16
    data = head(b'GIF89a', W, H, 0x70) + b'\x21\xfe\x05hello\0' + b'\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0'
    data += gif_frame(idx, W, H, 4, local=entries, bits=4) + gif_frame(idx, W, H, 4, local=other, bits=4) + b'\x3b'
    save('local.gif', data, W, H, px)
    # interlaced: rows 0, 8, 16; 4, 12; 2, 6, 10, 14; odd
    order = list(range(0, H, 8)) + list(range(4, H, 8)) + list(range(2, H, 4)) + list(range(1, H, 2))
    data = head(b'GIF89a', W, H, 0xf3) + table + gif_frame([index(x, y) for y in order for x in range(W)], W, H, 4, interlace=True)
    save('interlaced.gif', data + b'\x3b', W, H, px)
    # transparent, and a frame in the middle of the screen: what it does not cover is not there
    fw, fh, fx, fy = 10, 7, 6, 5
    idx2 = [index(x, y) for y in range(fh) for x in range(fw)]
    data = head(b'GIF89a', W, H, 0xf3) + table + gce(3) + gif_frame(idx2, fw, fh, 4, x=fx, y=fy) + b'\x3b'
    px2 = [0xff000000] * (W * H)
    for y in range(fh):
        for x in range(fw):
            v = idx2[y * fw + x]
            if v != 3:
                px2[(y + fy) * W + x + fx] = solid(*entries[v])
    save('offset.gif', data, W, H, px2)
    # a screen of no size, and one too small: the frame's size counts
    save('noscreen.gif', head(b'GIF89a', 0, 0, 0xf3) + table + gif_frame(idx, W, H, 4) + b'\x3b', W, H, px)
    # two colours, the smallest code size; a clear code now and then; sub-blocks of one byte
    bw = [(0, 0, 0), (255, 255, 255)]
    idx3 = [(x ^ y) & 1 for y in range(H) for x in range(W)]
    data = head(b'GIF89a', W, H, 0xf0) + gif_table(bw, 1) + gif_frame(idx3, W, H, 2, clear_every=7, size=1) + b'\x3b'
    save('two.gif', data, W, H, [solid(*bw[v]) for v in idx3])
    # a table that fills up, and goes on full; then one that clears when it is
    side = 120
    entries2 = [(i, 255 - i, i // 2) for i in range(256)]
    noise = [(i * i * 13 + i * 7 + (i >> 3) * (i >> 5)) & 255 for i in range(side * side)]
    data = head(b'GIF89a', side, side, 0xf7) + gif_table(entries2, 8) + gif_frame(noise, side, side, 8, no_table_clear=True) + b'\x3b'
    save('full-table.gif', data, side, side, [solid(*entries2[v]) for v in noise])
    data = head(b'GIF89a', side, side, 0xf7) + gif_table(entries2, 8) + gif_frame(noise, side, side, 8) + b'\x3b'
    save('clear-table.gif', data, side, side, [solid(*entries2[v]) for v in noise])


# -- BMP ---------------------------------------------------------------------------------------------------------------

def bmp_file(header, palette, pixels, offset=None):
    off = 14 + len(header) + len(palette) if offset is None else offset
    return b'BM' + struct.pack('<IHHI', off + len(pixels), 0, 0, off) + header + palette + pixels


def info(w, h, bpp, comp=0, used=0, size=40, extra=b''):
    head = struct.pack('<IiiHHIIiiII', size, w, h, 1, bpp, comp, 0, 2835, 2835, used, 0) + extra
    return head + bytes(size - len(head))


def rows(w, h, bpp, value, topdown=False):
    out = bytearray()
    for y in range(h) if topdown else range(h - 1, -1, -1):
        r = bytearray()
        if bpp <= 8:
            r = pack([value(x, y) for x in range(w)], bpp)
        else:
            for x in range(w):
                r += value(x, y)
        out += bytes(r) + bytes((4 - len(r) % 4) % 4)
    return bytes(out)


def bmp_family():
    px = [solid(*pattern(x, y, W, H)) for y in range(H) for x in range(W)]
    bgr = lambda x, y: bytes(reversed(pattern(x, y, W, H)))
    save('rgb24.bmp', bmp_file(info(W, H, 24), b'', rows(W, H, 24, bgr)), W, H, px)
    save('rgb24-topdown.bmp', bmp_file(info(W, -H, 24), b'', rows(W, H, 24, bgr, True)), W, H, px)
    save('rgb32.bmp', bmp_file(info(W, H, 32), b'', rows(W, H, 32, lambda x, y: bgr(x, y) + b'\x55')), W, H, px)
    core = struct.pack('<IHHHH', 12, W, H, 1, 24)
    save('core24.bmp', bmp_file(core, b'', rows(W, H, 24, bgr)), W, H, px)
    # no final padding: the file stops at the last row's last pixel
    data = bmp_file(info(W, H, 24), b'', rows(W, H, 24, bgr))
    save('nopad.bmp', data[:-((4 - W * 3 % 4) % 4)], W, H, px)
    # palettes: 1, 2, 4, 8 bits; fewer colours than the depth holds; 3-byte entries in the 12-byte header
    for bpp in (1, 2, 4, 8):
        n = 1 << bpp
        entries = [(i * 255 // (n - 1), 255 - i * 255 // (n - 1), i * 37 & 255) for i in range(n)]
        pal = b''.join(bytes((e[2], e[1], e[0], 0)) for e in entries)
        index = lambda x, y: (x + y) % n
        save(f'pal{bpp}.bmp', bmp_file(info(W, H, bpp, used=n), pal, rows(W, H, bpp, index)), W, H,
             [solid(*entries[index(x, y)]) for y in range(H) for x in range(W)])
    entries = [(i * 40, 200 - i * 40, i) for i in range(5)]
    pal = b''.join(bytes((e[2], e[1], e[0], 0)) for e in entries)
    index = lambda x, y: (x + y) % 7                        # past the 5 colours: black
    save('pal-short.bmp', bmp_file(info(W, H, 8, used=5), pal, rows(W, H, 8, index)), W, H,
         [solid(*(entries[index(x, y)] if index(x, y) < 5 else (0, 0, 0))) for y in range(H) for x in range(W)])
    pal = b''.join(bytes((e[2], e[1], e[0])) for e in entries) + bytes(3 * 251)
    save('core8.bmp', bmp_file(struct.pack('<IHHHH', 12, W, H, 1, 8), pal, rows(W, H, 8, lambda x, y: (x + y) % 5)), W, H,
         [solid(*entries[(x + y) % 5]) for y in range(H) for x in range(W)])
    # 16 bits: 5-5-5 by default, 5-6-5 by bit fields after the 40-byte header
    def c5(v):
        return (v >> 3) * 255 // 31

    def c6(v):
        return (v >> 2) * 255 // 63
    p555 = lambda x, y: struct.pack('<H', sum(c >> 3 << s for c, s in zip(pattern(x, y, W, H), (10, 5, 0))))
    save('rgb555.bmp', bmp_file(info(W, H, 16), b'', rows(W, H, 16, p555)), W, H,
         [solid(*(c5(c) for c in pattern(x, y, W, H))) for y in range(H) for x in range(W)])
    masks = struct.pack('<III', 0xf800, 0x07e0, 0x001f)
    p565 = lambda x, y: struct.pack('<H', (lambda r, g, b: r >> 3 << 11 | g >> 2 << 5 | b >> 3)(*pattern(x, y, W, H)))
    save('rgb565.bmp', bmp_file(info(W, H, 16, comp=3), masks, rows(W, H, 16, p565)), W, H,
         [solid(c5(pattern(x, y, W, H)[0]), c6(pattern(x, y, W, H)[1]), c5(pattern(x, y, W, H)[2])) for y in range(H) for x in range(W)])
    # 32 bits by bit fields, alpha in a V4 and a V5 header; an alpha mask with every pixel 0 is a file without alpha
    fields = struct.pack('<IIII', 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000)
    argb = lambda x, y: bgr(x, y) + bytes([alpha(x, y)])
    pa = [solid(*pattern(x, y, W, H), alpha(x, y)) for y in range(H) for x in range(W)]
    save('argb-v4.bmp', bmp_file(info(W, H, 32, comp=3, size=108, extra=fields), b'', rows(W, H, 32, argb)), W, H, pa)
    save('argb-v5.bmp', bmp_file(info(W, H, 32, comp=3, size=124, extra=fields), b'', rows(W, H, 32, argb)), W, H, pa)
    save('noalpha-v4.bmp', bmp_file(info(W, H, 32, comp=3, size=108, extra=fields), b'', rows(W, H, 32, lambda x, y: bgr(x, y) + b'\0')), W, H, px)
    # odd masks: 10 bits a channel
    tenbit = struct.pack('<III', 0x3ff00000, 0x000ffc00, 0x000003ff)
    p10 = lambda x, y: struct.pack('<I', sum(c * 4 << s for c, s in zip(pattern(x, y, W, H), (20, 10, 0))))
    save('rgb10.bmp', bmp_file(info(W, H, 32, comp=3), tenbit, rows(W, H, 32, p10)), W, H, px)
    # RLE8: runs, a literal run, a step over pixels, the row's and the picture's ends; bottom row first
    w, h = 8, 4
    pal = b''.join(bytes((i * 10, i * 12, i * 16, 0)) for i in range(16))
    col = lambda v: solid(v * 16, v * 12, v * 10)
    rle = bytes([3, 1, 0, 3, 2, 3, 4, 0, 2, 5, 0, 0])       # the bottom row: 1 1 1, a literal 2 3 4 (padded), 5 5
    rle += bytes([0, 2, 2, 1, 4, 6, 0, 0])                  # a step 2 right, 1 up, past a row: four 6s
    rle += bytes([8, 7, 0, 0, 0, 1])                        # the top row: eight 7s; the picture's end
    pixels = [0xff000000] * (w * h)
    for x, v in enumerate((1, 1, 1, 2, 3, 4, 5, 5)):
        pixels[3 * w + x] = col(v)
    for x in (2, 3, 4, 5):
        pixels[1 * w + x] = col(6)
    for x in range(8):
        pixels[0 * w + x] = col(7)
    save('rle8.bmp', bmp_file(info(w, h, 8, comp=1, used=16), pal, rle), w, h, pixels)
    rle = bytes([4, 0x12, 0, 3, 0x34, 0x50, 1, 0x60, 0, 0, 0, 1])     # 1 2 1 2, a literal 3 4 5, a run of one
    pixels = [0xff000000] * (w * 1)
    for x, v in enumerate((1, 2, 1, 2, 3, 4, 5, 6)):
        pixels[x] = col(v)
    save('rle4.bmp', bmp_file(info(w, 1, 4, comp=2, used=16), pal, rle), w, 1, pixels)


# -- JPEG --------------------------------------------------------------------------------------------------------------

ZIG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
       28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
       47, 55, 62, 63]


def fdct(s):
    from math import cos, pi, sqrt
    out = [0.0] * 64
    for v in range(8):
        for u in range(8):
            t = 0.0
            for y in range(8):
                for x in range(8):
                    t += (s[y * 8 + x] - 128) * cos((2 * x + 1) * u * pi / 16) * cos((2 * y + 1) * v * pi / 16)
            out[v * 8 + u] = t * (1 / sqrt(2) if u == 0 else 1) * (1 / sqrt(2) if v == 0 else 1) / 4
    return out


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.acc = self.acc << 1 | (v >> i & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 255:
                    self.out.append(0)
                self.acc, self.n = 0, 0

    def flush(self):
        while self.n:
            self.put(1, 1)


def jpeg_encode(w, h, sampling=None, ids=(1, 2, 3), rgb=False, scans=None, restart=0, quant16=False, sof=0xc0, adobe=None,
                pre=b''):
    """a baseline file: sampling, (h, v) of each component; scans: lists of component numbers (one scan of all by
    default); every code 8 bits long, of the values the tables hold"""
    sampling = sampling or [(1, 1)] * 3
    hmax, vmax = max(s[0] for s in sampling), max(s[1] for s in sampling)
    n = len(sampling)

    def value(c, x, y):
        r, g, b = pattern(min(x, w - 1), min(y, h - 1), w, h)
        if n == 1:
            return r
        if rgb:
            return (r, g, b)[c]
        return (0.299 * r + 0.587 * g + 0.114 * b, 128 - 0.168736 * r - 0.331264 * g + 0.5 * b,
                128 + 0.5 * r - 0.418688 * g - 0.081312 * b)[c]

    def plane_block(c, bx, by):
        hs, vs = sampling[c]
        fx, fy = hmax / hs, vmax / vs
        out = []
        for y in range(8):
            for x in range(8):
                x0, y0 = (bx * 8 + x) * fx, (by * 8 + y) * fy        # the middle of what a sample stands for
                out.append(value(c, int(x0 + fx / 2), int(y0 + fy / 2)))
        return out
    q = [2] * 64
    dqt = b'\xff\xdb' + (struct.pack('>HB', 2 + 1 + 128, 0x10) + b''.join(struct.pack('>H', v) for v in q) if quant16
                         else struct.pack('>HB', 2 + 1 + 64, 0) + bytes(q))
    dcv = list(range(12))
    acv = [r << 4 | s for r in range(16) for s in range(1, 11)] + [0, 0xf0]
    def dht(tc, vals):
        return b'\xff\xc4' + struct.pack('>HB', 2 + 17 + len(vals), tc << 4) + bytes(7) + bytes([len(vals)]) + bytes(8) + bytes(vals)
    head = b'\xff\xd8'
    if adobe is not None:
        head += b'\xff\xee' + struct.pack('>H', 14) + b'Adobe' + bytes([0, 100, 0, 0, 0, 0, adobe])
    head += dqt
    head += bytes([0xff, sof]) + struct.pack('>HBHHB', 8 + 3 * n, 8, h, w, n)
    for c in range(n):
        head += bytes([ids[c], sampling[c][0] << 4 | sampling[c][1], 0])
    head += dht(0, dcv) + dht(1, acv)
    if restart:
        head += b'\xff\xdd' + struct.pack('>HH', 4, restart)
    code_dc, code_ac = {v: i for i, v in enumerate(dcv)}, {v: i for i, v in enumerate(acv)}
    mcux, mcuy = -(-w // (8 * hmax)), -(-h // (8 * vmax))

    def block(bits, c, bx, by, pred):
        z = fdct(plane_block(c, bx, by))
        z = [int(round(z[ZIG[k]] / q[k])) for k in range(64)]

        def cat(v):
            return len(bin(abs(v))) - 2 if v else 0

        def extra(v, s):
            bits.put(v if v > 0 else v + (1 << s) - 1, s)
        d = z[0] - pred[c]
        pred[c] = z[0]
        s = cat(d)
        bits.put(code_dc[s], 8)
        if s:
            extra(d, s)
        run = 0
        for k in range(1, 64):
            if not z[k]:
                run += 1
                continue
            while run > 15:
                bits.put(code_ac[0xf0], 8)
                run -= 16
            s = cat(z[k])
            bits.put(code_ac[run << 4 | s], 8)
            extra(z[k], s)
            run = 0
        if run:
            bits.put(code_ac[0], 8)

    body = bytearray()
    for sc in scans or [list(range(n))]:
        body += b'\xff\xda' + struct.pack('>HB', 6 + 2 * len(sc), len(sc))
        for c in sc:
            body += bytes([ids[c], 0])
        body += bytes([0, 63, 0])
        bits, pred, count, mark = Bits(), [0] * n, 0, 0
        if len(sc) > 1:
            total = mcux * mcuy
            units = [(c, mx * sampling[c][0] + i, my * sampling[c][1] + j, mx, my)
                     for my in range(mcuy) for mx in range(mcux) for c in sc
                     for j in range(sampling[c][1]) for i in range(sampling[c][0])]
            per = sum(sampling[c][0] * sampling[c][1] for c in sc)
        else:
            c = sc[0]
            nw = -(-(-(-w * sampling[c][0] // hmax)) // 8)
            nh = -(-(-(-h * sampling[c][1] // vmax)) // 8)
            total = nw * nh
            units = [(c, bx, by, bx, by) for by in range(nh) for bx in range(nw)]
            per = 1
        for i, (c, bx, by, _, _) in enumerate(units):
            block(bits, c, bx, by, pred)
            if (i + 1) % per == 0:
                count += 1
                if restart and count % restart == 0 and count < total:
                    bits.flush()
                    body += bits.out + pre + bytes([0xff, 0xd0 + mark])
                    bits, pred = Bits(), [0] * n
                    mark = (mark + 1) & 7
        bits.flush()
        body += bits.out
    return head + bytes(body) + b'\xff\xd9'


def jpeg_family():
    from PIL import Image
    tolerance = 10
    for w, h, tag in ((40, 30, ''), (129, 97, '-odd')):
        img = Image.new('RGB', (w, h))
        img.putdata([pattern(x, y, w, h) for y in range(h) for x in range(w)])
        def real(name, im, **kw):
            path = os.path.join(OUT, name + tag + '.jpg')
            im.save(path, 'JPEG', quality=95, **kw)
            manifest.append(f'{name}{tag}.jpg {w} {h} near {tolerance}')
        for sub, label in ((0, '444'), (1, '422'), (2, '420')):
            real(f'base{label}', img, subsampling=sub)
            real(f'opt{label}', img, subsampling=sub, optimize=True)
            real(f'prog{label}', img, subsampling=sub, progressive=True)
        path = os.path.join(OUT, f'restart{tag}.jpg')
        img.save(path, 'JPEG', quality=95, subsampling=2, restart_marker_blocks=3)
        manifest.append(f'restart{tag}.jpg {w} {h} near {tolerance}')
        path = os.path.join(OUT, f'progrestart{tag}.jpg')
        img.save(path, 'JPEG', quality=95, subsampling=2, progressive=True, restart_marker_rows=1)
        manifest.append(f'progrestart{tag}.jpg {w} {h} near {tolerance}')
        gray = img.getchannel('R')
        for name, kw in (('gray', {}), ('grayprog', {'progressive': True})):
            gray.save(os.path.join(OUT, name + tag + '.jpg'), 'JPEG', quality=95, **kw)
            manifest.append(f'{name}{tag}.jpg {w} {h} gray {tolerance}')
        cmyk = img.convert('CMYK')
        cmyk.save(os.path.join(OUT, f'cmyk{tag}.jpg'), 'JPEG', quality=95)
        manifest.append(f'cmyk{tag}.jpg {w} {h} near 24')
        cmyk.save(os.path.join(OUT, f'cmykprog{tag}.jpg'), 'JPEG', quality=95, progressive=True)
        manifest.append(f'cmykprog{tag}.jpg {w} {h} near 24')
    # the small encoder's: what libjpeg was not asked to write
    w, h = 40, 30
    t = 10
    def own(name, **kw):
        save(name, jpeg_encode(w, h, **kw), w, h, tolerance=t)
    own('own444.jpg')
    own('own420.jpg', sampling=[(2, 2), (1, 1), (1, 1)])
    own('own411.jpg', sampling=[(4, 1), (1, 1), (1, 1)])
    own('own440.jpg', sampling=[(1, 2), (1, 1), (1, 1)])
    own('own31.jpg', sampling=[(3, 1), (1, 1), (1, 1)])
    own('own2211.jpg', sampling=[(2, 1), (1, 2), (1, 1)])
    own('ownscans.jpg', scans=[[0], [1], [2]], sampling=[(2, 2), (1, 1), (1, 1)])
    own('ownpairs.jpg', scans=[[0, 1], [2]])
    own('ownrgb.jpg', rgb=True, adobe=0)
    own('ownrgbnames.jpg', rgb=True, ids=(ord('R'), ord('G'), ord('B')))
    own('ownquant16.jpg', quant16=True)
    own('ownsof1.jpg', sof=0xc1)
    own('ownrestart.jpg', restart=2, sampling=[(2, 2), (1, 1), (1, 1)])
    own('ownrestartfill.jpg', restart=2, pre=b'\xff\xff')     # fill bytes before each marker
    own('ownadobe1.jpg', adobe=1)


def main():
    os.makedirs(OUT, exist_ok=True)
    png_family()
    gif_family()
    bmp_family()
    jpeg_family()
    with open(os.path.join(OUT, 'manifest'), 'w') as f:
        f.write('\n'.join(manifest) + '\n')
    print(f'{len(manifest)} pictures in {os.path.normpath(OUT)}')


main()
