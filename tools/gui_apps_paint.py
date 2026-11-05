"""The guiapps scenario's checks of Paint, wpaint (user/wpaint/; the desktop applications, track paint:
docs/superpowers/plans/2026-11-05-desktop-apps.md); the Driver is gui_apps_scenario.py's.

Apps > Programs lists Paint (rootfs/apps/wpaint.app), and choosing it starts it: a window with the focus, titled
"Untitled - Paint", the brush lit in its palette and a white canvas; its close button ends it at once (nothing changed).
Then Files mounts the data stick, goes into paint/ and File > Open with offers Paint for sea.bmp (its open-with): Paint
starts with the file's path and shows the picture, a quadrant of a colour each, in the middle of its view. A drag with
the brush draws (the title's * says it changed); Ctrl+Z and Ctrl+Y take it back and draw it again; Ctrl+S saves sea.bmp
in place (saveutil.h, no chooser). Paint's menus stand in the bar in place of File and Edit, and Image > Zoom > 200% (by
the keyboard) shows the picture twice as large; Ctrl+0 goes back to 100%. F, the fill, a click on the palette's white
and a click in the yellow quadrant fill it white; Ctrl+Shift+S asks the desktop's Save as chooser (its own title looked
for anywhere in its bar), in the picture's folder, and copy.png typed saves a PNG. Ctrl+O and bad.png, which is no
picture: Paint says it cannot open it, and the picture stays. A stroke more, and the close button asks first: Esc keeps
the window; then File > Close (Ctrl+W) asks again, and Don't save closes it (the close button pressed a second time is
the desktop's "End the program?", shell/guiclient.c's client_close(), whatever the program answered the first). Files
ejects the stick and closes. The host reads sea.bmp and copy.png back: the stroke black, the quadrant white in the PNG
alone. Everything is left as it was found, also after a failure.
"""
import struct
import zlib

import gui_scenario as base
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND, MENU_H
from gui_apps_suite import files_front, files_status, files_row, select, EJECT
from gui_apps_media import dialog_open, find_row, chooser_row

W, H = 800, 440                         # Paint's window's content (user/wpaint/window.h's PW_W, PW_H)
FRAME_H, FRAME_W = 29, 2
WIN = {'at': (72, 43)}                  # where Paint's window is: found by its title at each start (place())
TOOLS_W, COLOURS_H, STATUS_H = 60, 44, 22
VIEW_W, VIEW_H = W - TOOLS_W, H - COLOURS_H - STATUS_H   # the canvas's view, with no scroll bars
PIC_W, PIC_H = 64, 48
PARK = (900, 480)
CHOOSER = (220, 81)                     # a chooser's top left at 960x540 (shell/guidialog.c's layout)
PROGRAMS_X = 8 + 88 + 56 + 56 + 10      # Apps' items' text (Apps after "\xf7 SurfOS", File and Edit: guicore.c)
OPEN_WITH_X = 8 + 88 - 4 + 14           # File > Open with's items' text, with Files in front
BLACK, WHITE = bytes.fromhex('000000'), bytes.fromhex('ffffff')
RED, GREEN, BLUE, YELLOW = 0xC03030, 0x40A040, 0x3060C0, 0xF0D040   # paint.h's palette's: the picture's quadrants
WHITE_SWATCH = 4                        # paint_palette[4], in the colours' first row


def rgb(c):
    return bytes(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF))


def quadrant(x, y):
    return (RED if x < PIC_W // 2 else GREEN) if y < PIC_H // 2 else (BLUE if x < PIC_W // 2 else YELLOW)


def bmp(w, h, pixel):
    """a 24-bit BMP, rows from the bottom"""
    stride = (w * 3 + 3) // 4 * 4
    rows = b''.join(b''.join(bytes((pixel(x, y) & 0xFF, (pixel(x, y) >> 8) & 0xFF, pixel(x, y) >> 16)) for x in range(w))
                    + bytes(stride - w * 3) for y in reversed(range(h)))
    return (b'BM' + struct.pack('<IHHI', 54 + len(rows), 0, 0, 54) +
            struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(rows), 2835, 2835, 0, 0) + rows)


TITLES = ('Untitled - Paint', 'sea.bmp - Paint', '*sea.bmp - Paint', 'copy.png - Paint', '*copy.png - Paint')
STICK = {'paint/sea.bmp': bmp(PIC_W, PIC_H, quadrant), 'paint/bad.png': b'this is not a picture\n'}


def places():
    """where shell/guiclient.c opens a program's window: its cascade, 24 by 20 apart, kept on a 960x540 screen (the
    cascade goes on from window to window, so a second start is at the next place)"""
    for k in range(8):
        yield min(72 + 24 * k, 960 - (W + FRAME_W)), min(52 + 20 * k, 540 - 26 - 2 - (H + FRAME_H))


def titled_at(d, s, at, text, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, text, PAPER, bar) for bar in bars)


def place(d, text, focused=True):
    """a predicate: a window titled text at one of the places (which WIN keeps)"""
    def found(s):
        for at in places():
            if titled_at(d, s, at, text, focused):
                WIN['at'] = at
                return True
        return False
    return found


def content():
    """the window's content's top left"""
    return WIN['at'][0] + 1, WIN['at'][1] + 28


def canvas(cx, cy, zoom=1):
    """where canvas pixel cx, cy shows on the screen: a canvas smaller than the view is in its middle"""
    x0, y0 = content()
    cw, ch = PIC_W * zoom, PIC_H * zoom
    return x0 + TOOLS_W + (VIEW_W - cw) // 2 + cx * zoom, y0 + (VIEW_H - ch) // 2 + cy * zoom


def titled(d, text, focused=True):
    return lambda s: titled_at(d, s, WIN['at'], text, focused)


def status(d, text):
    x0, y0 = content()
    return lambda s: d.text(s, x0 + 10, y0 + H - STATUS_H + 3, text, INK, BAND)


def shows(d, s, points, zoom=1):
    """the canvas pixels at points (x, y, colour) show so"""
    return all(d.pixel(s, *canvas(x, y, zoom)) == rgb(c) for x, y, c in points)


def picture(d, zoom=1):
    return lambda s: shows(d, s, [(8, 8, RED), (56, 8, GREEN), (8, 40, BLUE), (56, 40, YELLOW), (0, 0, RED),
                                  (PIC_W - 1, PIC_H - 1, YELLOW)], zoom)


def gone(d, title):
    """no window titled title at any of the places, in front or behind"""
    return lambda s: not any(titled_at(d, s, at, title) for at in places())


def box(d, title):
    """Paint's own box over its window (the toolkit's: 470 wide, its title bar teal)"""
    x0, y0 = content()
    return lambda s: any(d.text(s, x0 + (W - 470) // 2 + 28, y0 + (H - h) // 2 + 6, title, PAPER, TEAL)
                         for h in (146, 160))


def close_button(d):
    d.click(WIN['at'][0] + W + FRAME_W - 16, WIN['at'][1] + 14)


def drag(d, a, b):
    d.move(*a)
    d.button(True)
    d.move((a[0] + b[0]) // 2, (a[1] + b[1]) // 2)
    d.move(*b)
    d.button(False)


def chooser_titled(d, s, text):
    """the desktop's chooser is up with its own title, text, somewhere in its bar (what names the program or the window
    before it is the desktop's to choose: the window's title until 11/05's fix round, the program's after it)"""
    y = CHOOSER[1] + 6
    return dialog_open(d, s) and any(d.text(s, x, y, text, PAPER, TEAL) for x in range(CHOOSER[0] + 20, CHOOSER[0] + 470))


def lit_program(d, s, name):
    """Apps > Programs is open with name chosen: its row lit"""
    return any(d.text(s, PROGRAMS_X, MENU_H + 4 + 4 + 24 * i, name, PAPER, TEAL) for i in range(16))


def files_open(d):
    """Apps > Files by the keyboard (the bar's fourth menu: \xf7 SurfOS, File, Edit, Apps), as track desk's checks do"""
    for chord in ('ctrl-g', 'right', 'right', 'right', 'down', 'ret'):
        d.key(chord)


def checks(d):
    state = {'paint': False, 'files': False, 'mounted': False}
    done = False
    try:
        d.move(*PARK)
        # Apps > Programs > Paint: the registry's entry
        for chord in ['ctrl-g'] + ['right'] * 3 + ['down'] * 14 + ['right']:
            d.key(chord)
        for _ in range(16):                             # the registry's programs, by their files' names: Paint's row
            if d.vm.wait_screen(lambda s: lit_program(d, s, 'Paint'), 1, interval=0.1) is not None:
                break
            d.key('down')
        d.require('paint: Apps > Programs lists Paint', lit_program(d, d.vm.screen(), 'Paint'))
        d.key('ret')
        state['paint'] = True
        d.visible('paint: choosing it starts it, its window in front with the focus: Untitled - Paint',
                  place(d, 'Untitled - Paint'), timeout=20)
        x0, y0 = content()
        d.visible('paint: the brush lit in its palette, a white canvas',
                  lambda s: d.pixel(s, x0 + 5 + 26 + 2, y0 + 4 + 2) == TEAL and
                  d.pixel(s, x0 + TOOLS_W + VIEW_W // 2, y0 + 100) == WHITE, 'guiapps-paint-new.png')
        close_button(d)
        d.move(*PARK)
        d.visible('paint: its close button ends it at once, as nothing changed', gone(d, 'Untitled - Paint'))
        state['paint'] = False
        # Files: the stick mounted, paint/, Open with > Paint on sea.bmp
        files_open(d)
        s = d.visible('paint: Files, with the data stick usb1p1 not mounted',
                      lambda s: files_front(d)(s) and files_row(d, s, 'usb1p1', 1) is not None)
        state['files'] = True
        select(d, files_row(d, s, 'usb1p1', 1))
        d.key('ret')
        s = d.visible('paint: Enter mounts the stick, with its paint folder',
                      lambda s: files_status(d, 'usb1p1 on /usb1p1: fat32')(s) and files_row(d, s, 'paint', 2) is not None)
        state['mounted'] = True
        select(d, files_row(d, s, 'paint', 2))
        d.key('ret')
        s = d.visible('paint: the paint folder opens', lambda s: files_row(d, s, 'sea.bmp', 3) is not None)
        select(d, files_row(d, s, 'sea.bmp', 3))
        for chord in ('ctrl-g', 'right', 'down', 'right'):
            d.key(chord)
        d.visible('paint: File > Open with offers Paint for a .bmp, first',
                  lambda s: d.text(s, OPEN_WITH_X, MENU_H + 4 + 4, 'Paint', PAPER, TEAL), 'guiapps-paint-openwith.png')
        d.key('ret')
        state['paint'] = True
        d.visible('paint: Paint starts with the picture, titled sea.bmp - Paint', place(d, 'sea.bmp - Paint'), timeout=20)
        x0, y0 = content()
        d.visible('paint: the picture at 100%, in the middle of its view', picture(d), 'guiapps-paint-sea.png')
        # drawing, undo, redo, save
        drag(d, canvas(4, 12), canvas(60, 12))
        d.move(*PARK)
        d.visible('paint: a drag with the brush draws, and the title says it changed',
                  lambda s: titled(d, '*sea.bmp - Paint')(s) and d.pixel(s, *canvas(16, 12)) == BLACK and
                  d.pixel(s, *canvas(48, 12)) == BLACK and d.pixel(s, *canvas(16, 16)) == rgb(RED))
        d.key('ctrl-z')
        d.visible('paint: Ctrl+Z takes it back', lambda s: titled(d, 'sea.bmp - Paint')(s) and picture(d)(s) and
                  d.pixel(s, *canvas(16, 12)) == rgb(RED))
        d.key('ctrl-y')
        d.visible('paint: Ctrl+Y draws it again', lambda s: d.pixel(s, *canvas(16, 12)) == BLACK)
        d.key('ctrl-s')
        d.visible('paint: Ctrl+S saves sea.bmp in place, no chooser asked',
                  lambda s: titled(d, 'sea.bmp - Paint')(s) and status(d, 'Saved sea.bmp')(s) and not dialog_open(d, s))
        # the menus in the bar, and Image > Zoom > 200% by the keyboard
        d.visible('paint: its menus in the bar in place of File and Edit',
                  lambda s: d.text(s, 8 + 88 + 4, 6, 'File', INK, PAPER) and d.text(s, 8 + 88 + 56 + 56 + 4, 6, 'Image', INK,
                                                                                      PAPER) and
                  d.text(s, 8 + 88 + 56 + 56 + 64 + 4, 6, 'Tools', INK, PAPER))
        for chord in ['ctrl-g'] + ['right'] * 3 + ['down'] * 6 + ['right'] + ['down'] * 5 + ['ret']:
            d.key(chord)
        d.visible('paint: Image > Zoom > 200% shows the picture twice as large, about the view\'s middle',
                  lambda s: picture(d, 2)(s) and d.pixel(s, *canvas(16, 12, 2)) == BLACK, 'guiapps-paint-zoom.png')
        d.key('ctrl-0')
        d.visible('paint: Ctrl+0 goes back to 100%', picture(d))
        # the fill, a colour of the palette
        d.keys('f')
        d.click(x0 + 48 + WHITE_SWATCH * 18 + 8, y0 + VIEW_H + 4 + 8)
        d.click(*canvas(50, 40))
        d.move(*PARK)
        d.visible('paint: F, white from the palette and a click fill the yellow quadrant white',
                  lambda s: d.pixel(s, *canvas(56, 40)) == WHITE and d.pixel(s, *canvas(8, 40)) == rgb(BLUE) and
                  d.pixel(s, x0 + 6 + 5, y0 + VIEW_H + 4 + 5) == WHITE)
        # Save as, through the desktop's chooser
        d.key('ctrl-shift-s')
        d.visible('paint: Ctrl+Shift+S asks the desktop\'s Save as chooser, in the picture\'s folder',
                  lambda s: chooser_titled(d, s, 'Save as') and d.text(s, CHOOSER[0] + 80, CHOOSER[1] + 38, '/usb1p1/paint',
                                                                         INK), 'guiapps-paint-saveas.png')
        d.keys('copy.png\n')
        d.visible('paint: copy.png saved, and the title names it',
                  lambda s: not dialog_open(d, s) and titled(d, 'copy.png - Paint')(s) and status(d, 'Saved copy.png')(s))
        # Open: a file that is no picture
        d.key('ctrl-o')
        s = d.visible('paint: Ctrl+O asks the desktop\'s Open chooser, in the picture\'s folder, its pictures listed',
                      lambda s: chooser_titled(d, s, 'Open a file') and find_row(d, s, 'bad.png') is not None)
        d.click(*chooser_row(find_row(d, s, 'bad.png')))
        d.key('ret')
        d.visible('paint: bad.png is no picture: Paint says so over its window', box(d, 'Cannot open the picture'),
                  'guiapps-paint-bad.png')
        d.key('ret')
        d.visible('paint: the picture stays', lambda s: not box(d, 'Cannot open the picture')(s) and
                  titled(d, 'copy.png - Paint')(s) and d.pixel(s, *canvas(56, 40)) == WHITE)
        # changes not saved: the close button asks first
        drag(d, canvas(4, 30), canvas(30, 30))
        close_button(d)
        d.move(*PARK)
        d.visible('paint: with changes not saved, its close button asks first', box(d, 'Save the changes?'))
        d.key('esc')
        d.visible('paint: Esc cancels: the window stays', lambda s: not box(d, 'Save the changes?')(s) and
                  titled(d, '*copy.png - Paint')(s))
        d.key('ctrl-w')                                 # the close button, pressed a second time, is the desktop's
        d.visible('paint: File > Close (Ctrl+W) asks again', box(d, 'Save the changes?'))   # "End the program?"
        d.key('right')
        d.key('ret')
        d.visible('paint: Don\'t save closes it', gone(d, '*copy.png - Paint'))
        state['paint'] = False
        # Files: the stick ejected, Files closed
        files_open(d)
        d.visible('paint: Files in front again', files_front(d))
        d.click(*EJECT)
        d.visible('paint: Eject flushes the stick', files_status(d, 'usb1: safe to remove'))
        state['mounted'] = False
        d.move(*PARK)
        d.key('ctrl-w')
        d.visible('paint: Files closes', lambda s: not files_front(d)(s))
        state['files'] = False
        done = True
    finally:
        if not done:
            if d.outdir:
                d.vm.screen().png(f'{d.outdir}/guiapps-paint-at-failure.png')
            try:
                tidy(d, state)
            except AssertionError:
                pass


def tidy(d, state):
    """after a failure: boxes and choosers dismissed, Paint closed without saving, the stick ejected, Files closed"""
    for _ in range(3):
        if not dialog_open(d, d.vm.screen()):
            break
        d.key('esc')
        d.vm.pump(0.3)
    d.key('esc')
    if state['paint']:
        none = lambda s: not any(titled_at(d, s, at, t) for at in places() for t in TITLES)
        for _ in range(3):
            s = d.vm.screen()
            for at in places():                         # where it is now
                if any(titled_at(d, s, at, t) for t in TITLES):
                    WIN['at'] = at
            d.click(WIN['at'][0] + 250, WIN['at'][1] + 14)   # its title bar: the focus; then File > Close
            d.move(*PARK)
            d.key('ctrl-w')
            if d.vm.wait_screen(none, 3, interval=0.2) is not None:
                break
            d.key('right')                              # its question: Don't save
            d.key('ret')
            if d.vm.wait_screen(none, 3, interval=0.2) is not None:
                break
        if d.vm.wait_screen(none, 1, interval=0.2) is None:   # still there: the desktop's End the program?, as kit's
            close_button(d)                             # tidy does
            close_button(d)
            d.move(*PARK)
            d.key('right')
            d.key('ret')
            d.vm.wait_screen(none, 5, interval=0.2)
    if state['files'] or state['mounted']:
        files_open(d)
        d.vm.pump(0.5)
        if state['mounted']:
            d.click(*EJECT)
            d.vm.wait_screen(files_status(d, 'usb1: safe to remove'), 15, interval=0.2)
        d.move(*PARK)
        d.key('ctrl-w')
        d.vm.pump(0.5)


def png_pixels(data):
    """a PNG of 8 bits a sample, RGB or RGBA, not interlaced, read here: (w, h, rows of bytes)"""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('no PNG signature')
    at, idat, w = 8, b'', None
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b'IHDR':
            w, h, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or colour not in (2, 6) or interlace:
                raise ValueError('not 8-bit RGB or RGBA')
            bpp = 3 if colour == 2 else 4
        elif kind == b'IDAT':
            idat += body
        at += 12 + n
    raw, rows, prev = zlib.decompress(idat), [], bytes(w * bpp)
    for y in range(h):
        line = raw[y * (w * bpp + 1):(y + 1) * (w * bpp + 1)]
        f, cur = line[0], bytearray(line[1:])
        for i in range(len(cur)):
            a = cur[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 0xFF
            elif f == 2:
                cur[i] = (cur[i] + b) & 0xFF
            elif f == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(bytes(cur))
        prev = bytes(cur)
    return w, h, bpp, rows


def readback(d, stick, helpers):
    """sea.bmp saved in place with the stroke; copy.png with the stroke and the quadrant filled white"""
    try:
        sea = helpers['read_fat32'](stick, 'paint/sea.bmp')
        copy = helpers['read_fat32'](stick, 'paint/copy.png')
    except Exception as e:                              # the checks stopped before they were saved
        d.check(f'paint: Paint\'s files are on the stick ({e})', False)
        return
    stride = PIC_W * 3
    ok = (len(sea) == 54 + stride * PIC_H and sea[:2] == b'BM' and
          struct.unpack('<iiHH', sea[18:26] + sea[26:30]) == (PIC_W, PIC_H, 1, 24))

    def at(x, y):
        o = 54 + (PIC_H - 1 - y) * stride + x * 3
        return bytes(reversed(sea[o:o + 3]))
    d.check('paint: sea.bmp reads back on the host: 64 x 48, 24 bits, the stroke black, the quadrants as they were',
            ok and at(16, 12) == BLACK and at(48, 12) == BLACK and at(16, 16) == rgb(RED) and at(56, 40) == rgb(YELLOW))
    try:
        w, h, bpp, rows = png_pixels(copy)
        px = lambda x, y: rows[y][x * bpp:x * bpp + 3]
        ok = (w, h, bpp) == (PIC_W, PIC_H, 3) and px(16, 12) == BLACK and px(56, 40) == WHITE and px(8, 40) == rgb(BLUE)
    except Exception as e:
        print(f'paint: copy.png: {e}', flush=True)
        ok = False
    d.check('paint: copy.png reads back on the host (Python\'s zlib): an RGB PNG, the stroke black, the quadrant white', ok)
