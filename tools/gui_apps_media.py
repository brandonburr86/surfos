"""The guiapps scenario's checks of track MEDIA (docs/superpowers/plans/2026-10-17-gui-suite.md): the Image Viewer, Hex
and Sketch's BMP files on the data stick (usb1, APPSTICK). The Driver is gui_apps_scenario.py's; the geometry below is
shell/guiview.c's, guihex.c's, guicore.c's and guidialog.c's at 960x540 logical pixels.

Files mounts the stick (Enter on its row, as gui_scenario.py does). Hex opens media/bytes.bin with the chooser and its
first row is read off the screen, then a Find; the Image Viewer opens two pictures made here with known quadrants (a
24-bit bottom-up one, fitted and at its actual size, and an 8-bit top-down one with a palette) and their colours are
read off the screen; Sketch draws a line, saves it as media/drawn.bmp (Ctrl+S), clears, and opens it again (its Open
button). Eject then flushes the stick, and readback() reads drawn.bmp on the host and checks that it is a 24-bit BMP with
the line drawn. The desktop is left as it was found, also after a failure: dialogs dismissed, the windows closed, the
stick ejected.
"""
import struct

import gui_scenario as base
from gui_apps_scenario import window, PAPER, INK, TEAL, MUTED, BAND, NAVY

SHEET = bytes.fromhex('fffdf5')
PICK = bytes.fromhex('bfe3dd')                                   # a selected row
TL, TR, BL, BR = 0xE04030, 0x30C060, 0x3050E0, 0xF0D040          # sea24.bmp's quadrants
TL8, TR8, BL8, BR8 = 0x9050C0, 0x50C0B0, 0xC07040, 0x606060      # sea8.bmp's palette
HEX_BYTES = b'BM SurfOS hex!\x00\xff' + bytes(range(256))
SKETCH = (264, 64)                                               # gui_init's place for Sketch at 960x540
SKETCH_TEAL = 0x197D83                                           # the third swatch
STROKE_Y, STROKE_X = 200, (300, 500)                             # where the line is drawn on the screen
FILES = (24, 48, 600, 380)
CHOOSER = (220, 81, 520, 380)                                    # guidialog.c's chooser at 960x540
MESSAGE = (245, 198, 470, 146)                                   # a message or a question
PARK = (900, 480)                                                # the pointer out of the way of what is read


def rgb(c):
    return bytes(((c >> 16) & 255, (c >> 8) & 255, c & 255))


def bmp(w, h, bpp, pix, palette=(), topdown=False):
    """a BITMAPINFOHEADER BMP: pix(x, y) (y from the top) is the colour, or the palette's index at 8 bits"""
    stride = (w * bpp + 31) // 32 * 4
    rows = []
    for r in range(h):
        y = r if topdown else h - 1 - r
        row = bytearray(stride)
        for x in range(w):
            c = pix(x, y)
            if bpp == 24:
                row[x * 3:x * 3 + 3] = bytes((c & 255, (c >> 8) & 255, (c >> 16) & 255))
            else:
                row[x] = c
        rows.append(bytes(row))
    data = b''.join(rows)
    off = 14 + 40 + 4 * len(palette)
    head = b'BM' + struct.pack('<IHHI', off + len(data), 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, w, -h if topdown else h, 1, bpp, 0, len(data), 2835, 2835, len(palette), 0)
    return head + info + b''.join(struct.pack('<I', c) for c in palette) + data


def quadrants(w, h, colours):
    return lambda x, y: colours[(0 if y < h // 2 else 2) + (0 if x < w // 2 else 1)]


STICK = {
    'media/sea24.bmp': bmp(42, 30, 24, quadrants(42, 30, (TL, TR, BL, BR))),
    'media/sea8.bmp': bmp(30, 20, 8, quadrants(30, 20, (0, 1, 2, 3)), palette=(TL8, TR8, BL8, BR8), topdown=True),
    'media/bytes.bin': HEX_BYTES,
}


# ---- where things are ----

def viewer_room():
    x, y, w, h = window('Image Viewer')
    return x + 1, y + 28 + 28 + 3, w - 2, h - 23 - 59     # below the toolbar, above the status bar


def fitted(pw, ph):
    """Fit: a picture smaller than the room enlarged by the largest whole number that fits, centred"""
    rx, ry, rw, rh = viewer_room()
    k = min(rw // pw, rh // ph, 16)
    return rx + (rw - pw * k) // 2, ry + (rh - ph * k) // 2, pw * k, ph * k


def actual(pw, ph):
    rx, ry, rw, rh = viewer_room()
    return rx + (rw - pw) // 2, ry + (rh - ph) // 2, pw, ph


def hex_byte_x(i):
    """byte i's two digits in Hex's first row (a pitch of 22 in a window 620 wide)"""
    x, _, _, _ = window('Hex')
    return x + 90 + 22 * i + (8 if i >= 8 else 0)


def chooser_row(k):
    """where row k's name is in the chooser's list"""
    return CHOOSER[0] + 44, CHOOSER[1] + 64 + 18 * k + 1


def title_is(d, x, y, text):
    return lambda s: d.text(s, *base._title(x, y), text, PAPER, TEAL)


def quadrants_shown(d, s, place, colours):
    """the picture at place (x, y, w, h) shows the four colours, a little inside each quadrant, NAVY around it"""
    x, y, w, h = place
    spots = [(x + w // 4, y + h // 4), (x + 3 * w // 4, y + h // 4), (x + w // 4, y + 3 * h // 4),
             (x + 3 * w // 4, y + 3 * h // 4), (x + 1, y + 1), (x + w - 2, y + 1), (x + 1, y + h - 2), (x + w - 2, y + h - 2)]
    want = list(colours) * 2
    return (all(d.pixel(s, px, py) == rgb(c) for (px, py), c in zip(spots, want)) and
            d.pixel(s, x - 2, y + h // 2) == NAVY and d.pixel(s, x + w + 1, y + h // 2) == NAVY)


# ---- the chooser ----

def dialog_open(d, s):
    """a chooser, a message or a question is in front: its teal title bar where guidialog.c puts it"""
    for x, y, w, _ in (CHOOSER, MESSAGE, (245, 191, 470, 160)):
        if all(d.pixel(s, x + i, y + 3) == TEAL for i in (2, w // 2, w - 4)) and d.pixel(s, x, y + 14) == INK:
            return True
    return False


def open_chooser(d, s):
    """11/05/26 (track img): a program's Open chooser in front (the Image Viewer is the program wview), found by the
    chooser's own title, Open a file, wherever its bar puts it after what names the program"""
    return any(d.text(s, x, CHOOSER[1] + 6, 'Open a file', PAPER, TEAL) for x in range(CHOOSER[0] + 4, CHOOSER[0] + CHOOSER[2] - 88))


def menu_open(d, s):
    """a menu is open: its title's teal band in the menu bar (the bar is paper otherwise, left of "Ride the wave.")"""
    return any(d.pixel(s, x, 4) == TEAL for x in range(0, 480, 2))


def find_row(d, s, name):
    for k in range(13):
        x, y = chooser_row(k)
        if d.text(s, x, y, name, INK) and not any(d.pixel(s, x + 8 * len(name) + c, y + r) == INK
                                                  for r in range(16) for c in range(8)):
            return k
    return None


def wait(d, name, predicate, shot):
    """d.visible, but with the screen kept when it fails"""
    s = d.vm.wait_screen(predicate, 15, interval=0.1)
    if s is None:
        d.vm.screen(shot)
    d.require(name, s is not None)
    return s


def choose(d, names, what):
    """in the chooser in front: up to /, then down the names (folders, then a file): a click selects each, Enter
    takes it (the same as a double-click, without depending on two clicks' timing under load). The click is on
    the row but clear of its name, which the pointer would hide, and the pointer is parked afterwards."""
    up = (CHOOSER[0] + CHOOSER[2] - 16 - 28, CHOOSER[1] + 34 + 12)
    folder = (CHOOSER[0] + 80, CHOOSER[1] + 38)
    for _ in range(8):
        s = d.vm.screen()
        if not d.text(s, *chooser_row(0), '.. (up one folder)', MUTED):
            break
        before = [d.pixel(s, folder[0] + i, folder[1] + 8) for i in range(0, 300, 2)]
        d.click(*up)
        d.vm.wait_screen(lambda s: [d.pixel(s, folder[0] + i, folder[1] + 8) for i in range(0, 300, 2)] != before, 10,
                         interval=0.1)
    for name in names:
        s = wait(d, f'{what}: the chooser lists {name}', lambda s: find_row(d, s, name) is not None,
                 'guiapps-media-chooser-failed.png')
        x, y = chooser_row(find_row(d, s, name))
        d.click(CHOOSER[0] + 300, y + 7)
        wait(d, f'{what}: a click selects {name}', lambda s: d.pixel(s, CHOOSER[0] + 18, y + 7) == PICK and
             d.text(s, x, y, name, INK), 'guiapps-media-chooser-failed.png')
        d.key('ret')
    d.move(*PARK)


# ---- the checks ----

def mount(d, state):
    d.move(*PARK)
    d.key('ctrl-b')
    state['files'] = True                               # from now on tidy() closes Files, whatever is checked next
    s = d.visible('media: Files opens, with the data stick usb1p1 not mounted',
                  lambda s: title_is(d, FILES[0], FILES[1], 'Files')(s) and
                  any(d.text(s, *base._files_row(r, 1), 'usb1p1', INK) for r in range(1, 8)))
    row = next(r for r in range(1, 8) if d.text(s, *base._files_row(r, 1), 'usb1p1', INK))
    for chord in ['home'] + ['down'] * row + ['ret']:
        d.key(chord)
    state['mounted'] = True                             # perhaps: tidy() ejects it then (Eject does nothing otherwise)
    d.visible('media: Enter on the stick mounts it',
              lambda s: d.text(s, FILES[0] + 10, FILES[1] + FILES[3] - 23 + 3, 'usb1p1 on /usb1p1: fat32', INK, BAND),
              'guiapps-media-mounted.png')


def hex_check(d):
    x, y, w, h = window('Hex')
    d.open_app('Hex')
    d.visible('Hex opens from the Apps menu, with no file', title_is(d, x, y, 'Hex'))
    d.click(x + 20, y + 40)                             # the toolbar's Open
    d.visible('Hex: Open shows the chooser', lambda s: d.text(s, 248, 87, 'Open a file', PAPER, TEAL))
    choose(d, ['usb1p1', 'media', 'bytes.bin'], 'Hex')
    row_y = y + 77

    def first_row(s):
        return (d.text(s, x + 10, row_y, '00000000', TEAL, SHEET) and                  # the cursor's row
                d.text(s, hex_byte_x(0), row_y, '42', PAPER, TEAL) and                  # the cursor
                all(d.text(s, hex_byte_x(i), row_y, '%02X' % HEX_BYTES[i], INK, SHEET) for i in range(1, 16)) and
                d.text(s, x + 460, row_y, 'B', PAPER, TEAL) and
                d.text(s, x + 468, row_y, 'M SurfOS hex!', INK, SHEET) and
                d.text(s, x + 460 + 14 * 8, row_y, '..', MUTED, SHEET))
    d.visible('Hex shows the file\'s first row: its offset, 16 bytes in hexadecimal, and as characters',
              lambda s: title_is(d, x, y, 'bytes.bin - Hex')(s) and first_row(s), 'guiapps-media-hex.png')
    status = (x + 10, y + h - 23 + 3)
    d.key('down')
    d.visible('Hex: Down moves the cursor a row (the status bar says where)',
              lambda s: d.text(s, *status, '0x00000010 (16)', INK, BAND))
    d.key('home')
    d.visible('Hex: Home goes back to the first byte', lambda s: d.text(s, *status, '0x00000000 (0)', INK, BAND))
    d.key('ctrl-f')
    d.visible('Hex: Ctrl+F asks what to find', lambda s: d.text(s, 273, 197, 'Find bytes', PAPER, TEAL))
    d.keys('SurfOS\n')
    d.visible('Hex: Find selects the bytes found',
              lambda s: d.text(s, *status, '0x00000008 (8), 6 selected  Found at 0x00000003', INK, BAND),
              'guiapps-media-hex-found.png')
    d.close('Hex')
    d.visible('Hex closes', lambda s: not title_is(d, x, y, 'bytes.bin - Hex')(s))


def viewer_check(d):
    x, y, w, h = window('Image Viewer')
    status = (x + 10, y + h - 23 + 3)
    d.open_app('Image Viewer')
    d.visible('the Image Viewer opens from the Apps menu, with no picture', title_is(d, x, y, 'Image Viewer'))
    d.keys('o')
    d.visible('the Image Viewer: O shows the chooser', lambda s: open_chooser(d, s))
    choose(d, ['usb1p1', 'media', 'sea24.bmp'], 'the Image Viewer')
    place = fitted(42, 30)
    d.visible('the Image Viewer fits a 24-bit BMP to the window: its four colours where they belong',
              lambda s: title_is(d, x, y, 'sea24.bmp - Image Viewer')(s) and quadrants_shown(d, s, place, (TL, TR, BL, BR)) and
              d.text(s, *status, 'sea24.bmp  42 x 30, 24-bit', INK, BAND) and
              d.text(s, x + w - 24 - 9 * 8, status[1], 'Fit 1000%', MUTED, BAND), 'guiapps-media-viewer-fit.png')
    d.keys('1')
    place = actual(42, 30)
    d.visible('the Image Viewer shows the actual size (1)',
              lambda s: quadrants_shown(d, s, place, (TL, TR, BL, BR)) and
              d.text(s, x + w - 24 - 4 * 8, status[1], '100%', MUTED, BAND), 'guiapps-media-viewer-actual.png')
    d.keys('o')
    d.visible('the Image Viewer: O again', lambda s: open_chooser(d, s))
    choose(d, ['usb1p1', 'media', 'sea8.bmp'], 'the Image Viewer')
    place = fitted(30, 20)
    d.visible('the Image Viewer shows an 8-bit top-down BMP with its palette, fitted',
              lambda s: title_is(d, x, y, 'sea8.bmp - Image Viewer')(s) and
              quadrants_shown(d, s, place, (TL8, TR8, BL8, BR8)) and d.text(s, *status, 'sea8.bmp  30 x 20, 8-bit', INK, BAND),
              'guiapps-media-viewer-8bit.png')
    d.close('Image Viewer')
    d.visible('the Image Viewer closes', lambda s: not title_is(d, x, y, 'sea8.bmp - Image Viewer')(s))


def stroke(d, s, colour):
    return all(d.pixel(s, px, STROKE_Y) == colour for px in range(STROKE_X[0], STROKE_X[1] + 1, 4))


def sketch_check(d):
    sx, sy = SKETCH
    note = (sx + 16, sy + 266)
    d.key('ctrl-d')
    d.visible('Sketch opens (Ctrl+D)', title_is(d, sx, sy, 'Sketch'))
    d.visible('Sketch has Open and Save buttons', lambda s: d.text(s, sx + 212 + 12, sy + 38 + 4, 'Open', INK) and
              d.text(s, sx + 272 + 12, sy + 38 + 4, 'Save', INK))
    d.click(sx + 16 + 2 * 32 + 10, sy + 38 + 12)        # the teal swatch
    d.move(STROKE_X[0], STROKE_Y)
    d.button(True)
    d.move(STROKE_X[1], STROKE_Y)
    d.button(False)
    d.move(*PARK)
    d.visible('Sketch: a teal line is drawn', lambda s: stroke(d, s, rgb(SKETCH_TEAL)))
    d.key('ctrl-s')
    d.visible('Sketch: Ctrl+S is Save as BMP, in the last chooser\'s folder',
              lambda s: d.text(s, 248, 87, 'Save as', PAPER, TEAL) and d.text(s, 300, 119, '/usb1p1/media', INK))
    d.keys('drawn.bmp\n')
    d.visible('Sketch saves the drawing as a 24-bit BMP on the stick',
              lambda s: not dialog_open(d, s) and d.text(s, *note, 'Saved drawn.bmp (216054 bytes)', INK, PAPER),
              'guiapps-media-sketch-saved.png')
    d.key('ctrl-n')
    d.visible('Sketch: Ctrl+N clears the drawing', lambda s: stroke(d, s, PAPER))
    d.click(sx + 212 + 28, sy + 38 + 12)                # Sketch's Open
    d.visible('Sketch: Open shows the chooser', lambda s: d.text(s, 248, 87, 'Open a file', PAPER, TEAL))
    choose(d, ['usb1p1', 'media', 'drawn.bmp'], 'Sketch')
    d.visible('Sketch opens the BMP it saved: the line is back',
              lambda s: stroke(d, s, rgb(SKETCH_TEAL)) and d.text(s, *note, 'Opened drawn.bmp', INK, PAPER),
              'guiapps-media-sketch-opened.png')
    d.key('ctrl-n')
    d.click(sx + 16 + 32 + 10, sy + 38 + 12)            # the ink swatch again, as it was
    d.key('ctrl-w')
    d.visible('Sketch closes', lambda s: not title_is(d, sx, sy, 'Sketch')(s))


def eject(d, state):
    d.key('ctrl-b')
    d.visible('media: Ctrl+B brings Files back', title_is(d, FILES[0], FILES[1], 'Files'))
    d.click(FILES[0] + 502 + 27, FILES[1] + 32 + 11)    # Eject
    d.visible('media: Eject in Files flushes the stick',
              lambda s: d.text(s, FILES[0] + 10, FILES[1] + FILES[3] - 23 + 3, 'usb1: safe to remove', INK, BAND),
              'guiapps-media-ejected.png')
    state['mounted'] = False
    d.key('ctrl-w')
    d.visible('media: Files closes', lambda s: not title_is(d, FILES[0], FILES[1], 'Files')(s))
    state['files'] = False


def tidy(d, state):
    """after a failure: what the checks opened is closed again, Sketch's brush is ink again, and the stick ejected.
    A button held in a drag is let go first, and a dialog or a menu left open is closed (Esc), so that the Apps menu
    (Ctrl+G, which would close an open menu instead) and the shortcuts reach the desktop."""
    d.button(False)
    for _ in range(4):
        s = d.vm.screen()
        if not dialog_open(d, s) and not menu_open(d, s):
            break
        d.key('esc')
        d.vm.pump(0.3)
    for name in ('Hex', 'Image Viewer'):
        d.open_app(name)
        d.vm.pump(0.3)
        d.key('ctrl-w')
    sx, sy = SKETCH
    d.key('ctrl-d')
    d.vm.pump(0.3)
    d.click(sx + 16 + 32 + 10, sy + 38 + 12)            # the ink swatch: the brush as the checks found it
    d.key('ctrl-n')
    d.vm.pump(0.3)
    d.key('ctrl-w')
    if state['files'] or state['mounted']:
        d.key('ctrl-b')
        d.vm.pump(0.5)
        if state['mounted']:
            d.click(FILES[0] + 502 + 27, FILES[1] + 32 + 11)
            d.vm.wait_screen(lambda s: d.text(s, FILES[0] + 10, FILES[1] + FILES[3] - 23 + 3, 'usb1: safe to remove',
                                              INK, BAND), 15, interval=0.2)
        d.key('ctrl-w')


def checks(d):
    state = {'files': False, 'mounted': False}
    done = False
    try:
        mount(d, state)
        hex_check(d)
        viewer_check(d)
        sketch_check(d)
        eject(d, state)
        done = True
    finally:
        if not done:
            try:
                tidy(d, state)
            except AssertionError:
                pass


def readback(d, stick, helpers):
    """Sketch's drawn.bmp read on the host: a 24-bit bottom-up BMP of the canvas with the teal line"""
    try:
        data = helpers['read_fat32'](stick, 'media/drawn.bmp')
    except Exception as e:                              # the checks stopped before Sketch saved it
        d.check(f'the BMP Sketch saved is on the stick ({e})', False)
        return
    ok = (len(data) == 54 + 1200 * 180 and data[:2] == b'BM' and
          struct.unpack('<IIIiiHHI', data[2:6] + data[10:14] + data[14:18] + data[18:26] + data[26:30] + data[30:34]) ==
          (len(data), 54, 40, 400, 180, 1, 24, 0))
    d.check('the BMP Sketch saved reads back on the host: 54 bytes of headers, 400 x 180, 24 bits, bottom-up', ok)
    if not ok:
        return

    def pixel(x, y):                                    # (x, y) on the canvas, from its top
        at = 54 + (179 - y) * 1200 + x * 3
        return data[at + 2] << 16 | data[at + 1] << 8 | data[at]
    line_y = STROKE_Y - SKETCH[1] - 70
    line = [pixel(x - SKETCH[0] - 16, line_y) for x in range(STROKE_X[0], STROKE_X[1] + 1)]
    paper = int.from_bytes(PAPER, 'big')
    d.check('its pixels are the drawing: the teal line, on paper everywhere else',
            all(c == SKETCH_TEAL for c in line) and pixel(10, 10) == paper and pixel(399, 179) == paper and
            pixel(STROKE_X[0] - SKETCH[0] - 16, line_y - 2) == paper and pixel(300, 150) == paper)
