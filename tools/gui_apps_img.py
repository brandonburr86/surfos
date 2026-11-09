"""The guiapps scenario's checks of the Image Viewer as a program (wview, docs/superpowers/plans/2026-11-05-desktop-apps.md,
track img; the Driver is gui_apps_scenario.py's). 11/05/26: in the Terminal, wview on a picture of the data stick opens a
window titled after it; a click on its title bar gives it the focus. A PNG of four colours shows fitted (ten times,
centred, the room navy around it) under the toolbar's nine buttons, and at its actual size (1), the status bar saying its
name, size, kind, place in the folder and zoom. N steps through the folder's pictures in their order: a GIF, a BMP and a
JPEG, each where its pixels should be, then a PNG with transparency over the squares of grey, then a PNG cut short,
which the status bar says is no picture while the one before stays, then a screenshot as the desktop names them (spaces
in its name), then around to the first; P steps back. I shows the file's details over the room, and Esc puts them
away; Ctrl+C puts the path on the desktop's clipboard, which the Terminal's Edit > Paste gives back once Esc has closed
the window. Then the desktop's ways in: Files opens a PNG in the Image Viewer, where the desktop's Image Viewer stood,
then a GIF and a BMP, each in place of the one before, so that one viewer is left; O there asks the desktop's chooser
(found by its own title), which opens the JPEG; Apps > Image Viewer starts it with no picture. The pictures are the stick's (STICK): two made here, the others the browser's test pictures
(tests/fixtures/wundertow/images/). The stick is mounted in the Terminal and in Files and let go again, and the
Terminal and Files closed, also when a check fails.
"""
import os
import struct
import zlib

import gui_apps_media as media
import gui_apps_suite as suite
import gui_apps_term as term
from gui_apps_scenario import window, PAPER, TEAL, MUTED, FOAM
from gui_apps_win import EDIT_X, PASTE_Y

TITLE_H = 28
CONTENT = (558, 371)                        # user/wview/view.h: VIEW_W x VIEW_H
FRAME_W = CONTENT[0] + 2
ROOM_Y, STATUS_H = 31, 22                   # VIEW_ROOM_Y, VIEW_STATUS_H
NAVY, INK, BAND = bytes.fromhex('102e3b'), bytes.fromhex('153d49'), bytes.fromhex('e7e2d2')
TEAL_C, PAPER_C = bytes.fromhex('197d83'), bytes.fromhex('f4f1e6')
SQUARES = (bytes.fromhex('f4f1e6'), bytes.fromhex('c9cfc4'))     # view.c's over(): where a picture is not there
PARK = (900, 480)
FILES_BAR = (24 + 40, 48 + 14)                      # Files' title bar left of where the Image Viewer stands (gui_init)
QUAD = ((0x10, 0x20, 0x30), (0x40, 0x50, 0x60), (0x70, 0x80, 0x90), (0xa0, 0xb0, 0xc0))   # TL, TR, BL, BR
SHOT = 'Screenshot 2026-11-05 120000.bmp'         # as the desktop names a screenshot: spaces in it
FOLDER = ['a-quad.png', 'b-plain.gif', 'c-rgb24.bmp', 'd-base444.jpg', 'e-rgba8.png', 'g-broken.png', SHOT]


def quad_png():
    """a PNG of 42 x 30 in four colours, a quadrant each"""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + b''.join(bytes(QUAD[(y >= 15) * 2 + (x >= 21)]) for x in range(42)) for y in range(30))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 42, 30, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def shot_bmp():
    """a BMP of 64 x 40 in 24 bits, rows from the bottom, as the desktop writes a screenshot: the four colours"""
    row = lambda y: b''.join(bytes(reversed(QUAD[(y >= 20) * 2 + (x >= 32)])) for x in range(64))
    body = b''.join(row(y) for y in reversed(range(40)))
    return (b'BM' + struct.pack('<IHHI', 54 + len(body), 0, 0, 54) +
            struct.pack('<IiiHHIIiiII', 40, 64, 40, 1, 24, 0, len(body), 0, 0, 0, 0) + body)


IMAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'fixtures', 'wundertow', 'images')


def fixture(name):
    with open(os.path.join(IMAGES, name), 'rb') as f:
        return f.read()


STICK = {
    'img/a-quad.png': quad_png(),
    'img/b-plain.gif': fixture('plain.gif'),
    'img/c-rgb24.bmp': fixture('rgb24.bmp'),
    'img/d-base444.jpg': fixture('base444.jpg'),
    'img/e-rgba8.png': fixture('rgba8.png'),
    'img/f-notes.txt': b'Not a picture: N passes it by.\n',
    'img/g-broken.png': fixture('rgb8.png')[:200],
    'img/' + SHOT: shot_bmp(),
}


def pattern(x, y, w, h):
    """tools/wundertow-images.py's: what the test pictures hold"""
    return bytes((x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), (x + y) * 255 // max(w + h - 2, 1)))


def gif(x, y):
    """plain.gif's colours (tools/wundertow-images.py)"""
    i = (x * 3 + y * 5) % 16
    return bytes((i * 255 // 15, 255 - i * 255 // 15, i * 37 & 255))


def alpha(x, y):
    """rgba8.png's alpha (tools/wundertow-images.py): 0 at the top left, 255 at the bottom right"""
    return (x * 3 + y * 5) * 255 // (3 * 22 + 5 * 16)


def cascade(k):
    """where shell/guiclient.c opens a desktop's k-th program window (from 0), wview's kept above the taskbar as
    gui_place() keeps a window (at 960 x 540: no lower than 540 - 26 - 2 - its 400 pixels)"""
    return 72 + 24 * (k % 8), min(52 + 20 * (k % 8), 540 - 26 - 2 - (TITLE_H + CONTENT[1] + 1))


def titled(d, s, at, text, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, text, PAPER, bar) for bar in bars)


def find(d, s, text):
    for k in range(8):
        if titled(d, s, cascade(k), text):
            return cascade(k)
    return None


def fitted(w, h):
    """view.c's Fit for a picture smaller than the room (558 x 318): the largest whole factor, 16 at most, centred"""
    k = min(558 // w, 318 // h, 16)
    return (558 - w * k) // 2, ROOM_Y + (318 - h * k) // 2, k


def picture(d, s, at, w, h, colour, tolerance=0, step=1):
    """a picture of w x h fitted in the window at at: the middle of each pixel shown (every step-th) is colour(x, y)"""
    ox, oy, k = fitted(w, h)
    for y in range(0, h, step):
        for x in range(0, w, step):
            got = d.pixel(s, at[0] + 1 + ox + x * k + k // 2, at[1] + TITLE_H + oy + y * k + k // 2)
            want = colour(x, y)
            if any(abs(a - b) > tolerance for a, b in zip(got, want)):
                return False
    return True


def room_around(d, s, at, w, h):
    """the room navy just outside the fitted picture, on its four sides"""
    ox, oy, k = fitted(w, h)
    x0, y0 = at[0] + 1 + ox, at[1] + TITLE_H + oy
    return all(d.pixel(s, x, y) == NAVY for x, y in ((x0 - 1, y0 + 5), (x0 + w * k, y0 + 5), (x0 + 5, y0 - 1),
                                                     (x0 + 5, y0 + h * k)))


def status(d, s, at, text):
    """the status bar's left text"""
    return d.text(s, at[0] + 1 + 9, at[1] + TITLE_H + CONTENT[1] - STATUS_H + 3, text, INK, BAND)


def zoom_said(d, s, at, text):
    """the status bar's right text, the zoom"""
    x = CONTENT[0] - 23 - 8 * len(text)
    return d.text(s, at[0] + 1 + x, at[1] + TITLE_H + CONTENT[1] - STATUS_H + 3, text, MUTED, BAND)


def quadrant(x, y):
    return bytes(QUAD[(y >= 15) * 2 + (x >= 21)])


def shot_quadrant(x, y):
    return bytes(QUAD[(y >= 20) * 2 + (x >= 32)])


TOOLS = ('Open', '\x11', '\x10', 'Fit', 'Actual', 'Zoom in', 'Zoom out', 'Details', 'Copy path')    # view.c's, in code page 437
SHELL = bytes.fromhex('dee3d8')


def toolbar(d, s, at, on=(True,) * len(TOOLS)):
    """the toolbar's nine buttons where view.c puts them: 7 from the content's left, 4 apart, each its label's cells and
    14 more, the label centred (gd_button()); one that can be used in ink on the buttons' grey, one that cannot muted on
    the bar's band (main.c's draw_bar())"""
    x = 7
    for label, usable in zip(TOOLS, on):
        w = len(label) * 8 + 14
        if not d.text(s, at[0] + 1 + x + 7, at[1] + TITLE_H + 4 + 3, label, *((INK, SHELL) if usable else (MUTED, BAND))):
            return False
        x += w + 4
    return True


def shown(d, at, name, index, colour, w, h, tolerance=0, step=1, shot=None):
    """the folder's picture name in front, where its pixels should be, the status bar saying its place"""
    d.visible(f'wview: {name}, {index} of {len(FOLDER)}, where its pixels should be',
              lambda s: titled(d, s, at, f'{name} - Image Viewer', True) and
              picture(d, s, at, w, h, colour, tolerance, step) and
              status(d, s, at, f'{name}  {w} x {h}, {kind(name)}  ({index} of {len(FOLDER)})'), shot, timeout=30)


def kind(name):
    return {'png': 'PNG', 'gif': 'GIF', 'bmp': '24-bit BMP', 'jpg': 'JPEG'}[name.rsplit('.', 1)[1]]


def transparent(d, s, at):
    """e-rgba8.png: its top left pixel is not there (the squares show), its bottom right is solid"""
    ox, oy, k = fitted(23, 17)
    x0, y0 = at[0] + 1 + ox, at[1] + TITLE_H + oy
    cx, cy = ox + 3, oy + 3                         # a pixel of the window's content inside the picture's first
    square = SQUARES[((cx >> 3) ^ (cy >> 3)) & 1]
    return d.pixel(s, x0 + 3, y0 + 3) == square and d.pixel(s, x0 + 22 * k + 8, y0 + 16 * k + 8) == pattern(22, 16, 23, 17)


def terminal(d):
    """wview started in the Terminal, on the stick the Terminal mounts: the pictures, the keys, Details, Copy path"""
    grid = term.Grid()
    mounted = False
    at = None
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal, for the Image Viewer', d.title('Terminal'))
        d.require('the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('mount usb1p1\n')
        mounted = d.check('the data stick is mounted in the Terminal',
                          term.wait_rows(d, grid, lambda rows: any('usb1p1 on /usb1p1' in r for r in rows), 30) is not None)
        d.keys('wview /usb1p1/img/a-quad.png &\n')
        s = d.visible('wview opens a window titled after the picture',
                      lambda s: find(d, s, 'a-quad.png - Image Viewer') is not None, timeout=40)
        at = find(d, s, 'a-quad.png - Image Viewer')
        d.click(at[0] + 300, at[1] + 14)            # its title bar: the focus
        d.move(*PARK)
        d.visible('a click on its title bar gives it the focus; the picture fitted, ten times, the room navy round it',
                  lambda s: titled(d, s, at, 'a-quad.png - Image Viewer', True) and
                  picture(d, s, at, 42, 30, quadrant, step=3) and room_around(d, s, at, 42, 30) and
                  status(d, s, at, 'a-quad.png  42 x 30, PNG  (1 of 7)') and zoom_said(d, s, at, 'Fit 1000%'),
                  'guiapps-img-fit.png')
        d.check('its toolbar: Open, the arrows, Fit, Actual, Zoom in and out, Details, Copy path',
                toolbar(d, d.vm.screen(), at))
        d.key('1')
        cx, cy = at[0] + 1 + (558 - 42) // 2, at[1] + TITLE_H + ROOM_Y + (318 - 30) // 2
        d.visible('1: its actual size, centred',
                  lambda s: all(d.pixel(s, cx + x, cy + y) == quadrant(x, y) for x, y in ((0, 0), (41, 0), (0, 29), (41, 29),
                                                                                         (20, 14), (21, 15))) and
                  d.pixel(s, cx - 1, cy) == NAVY and zoom_said(d, s, at, '100%'), 'guiapps-img-actual.png')
        d.key('f')
        d.visible('F: fitted again', lambda s: zoom_said(d, s, at, 'Fit 1000%') and picture(d, s, at, 42, 30, quadrant, step=5))
        d.key('n')
        shown(d, at, 'b-plain.gif', 2, gif, 23, 17, step=2, shot='guiapps-img-gif.png')
        d.key('n')
        shown(d, at, 'c-rgb24.bmp', 3, lambda x, y: pattern(x, y, 23, 17), 23, 17, step=2)
        d.key('n')
        shown(d, at, 'd-base444.jpg', 4, lambda x, y: pattern(x, y, 40, 30), 40, 30, tolerance=12, step=3)
        d.key('n')
        d.visible('wview: e-rgba8.png, a PNG with transparency: the squares where it is not there, solid where it is',
                  lambda s: titled(d, s, at, 'e-rgba8.png - Image Viewer', True) and transparent(d, s, at) and
                  status(d, s, at, 'e-rgba8.png  23 x 17, PNG  (5 of 7)'), 'guiapps-img-alpha.png', timeout=30)
        d.key('n')
        d.visible('a picture cut short: the status bar says why, and the picture before stays',
                  lambda s: titled(d, s, at, 'e-rgba8.png - Image Viewer', True) and status(d, s, at, 'g-broken.png: a PNG') and
                  transparent(d, s, at), 'guiapps-img-broken.png', timeout=30)
        d.key('n')
        d.visible('a screenshot as the desktop names it, spaces and all',
                  lambda s: titled(d, s, at, SHOT + ' - Image Viewer', True) and picture(d, s, at, 64, 40, shot_quadrant, step=4) and
                  status(d, s, at, SHOT + '  64 x 40, 24-bit BMP'), 'guiapps-img-shot.png', timeout=30)
        d.key('n')
        d.visible('N goes around to the first picture',
                  lambda s: titled(d, s, at, 'a-quad.png - Image Viewer', True) and status(d, s, at, 'a-quad.png  42 x 30, PNG  (1 of 7)'),
                  timeout=30)
        d.key('p')
        d.key('p')
        d.key('p')
        d.visible('P steps back, past the screenshot and the one that is no picture, to e-rgba8.png',
                  lambda s: titled(d, s, at, 'e-rgba8.png - Image Viewer', True) and transparent(d, s, at), timeout=30)
        d.key('i')
        px, py = at[0] + 1 + 12 + 12, at[1] + TITLE_H + ROOM_Y + 12 + 8
        d.visible('I: the file\'s details over the room',
                  lambda s: d.text(s, px, py, 'e-rgba8.png', TEAL_C, PAPER_C) and
                  d.text(s, px, py + 18, 'Folder: /usb1p1/img', INK, PAPER_C) and
                  d.text(s, px, py + 36, 'Kind: PNG picture', INK, PAPER_C) and
                  d.text(s, px, py + 72, 'Picture: 23 x 17 pixels', INK, PAPER_C), 'guiapps-img-details.png')
        d.key('esc')
        d.visible('Esc puts the details away', lambda s: not d.text(s, px, py + 36, 'Kind: PNG picture', INK, PAPER_C) and
                  transparent(d, s, at))
        d.key('ctrl-c')
        d.visible('Ctrl+C copies the path: the status bar says so', lambda s: status(d, s, at, 'Copied the path'))
        d.key('esc')
        d.visible('Esc ends wview, and its window goes', lambda s: find(d, s, 'e-rgba8.png - Image Viewer') is None, timeout=20)
        at = None
        d.keys('echo ')
        d.click(EDIT_X, 14)                         # the Terminal's Edit menu: Paste, then Enter
        d.click(EDIT_X + 18, PASTE_Y)
        d.move(*PARK)
        d.key('ret')
        d.require('the path wview put on the clipboard, pasted into the Terminal',
                  term.wait_rows(d, grid, lambda rows: '/usb1p1/img/e-rgba8.png' in rows and term.prompted(rows)) is not None)
    except AssertionError:
        if d.outdir:
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-img-at-failure.png'))
        raise
    finally:
        s = d.vm.screen()
        for name in FOLDER:
            there = find(d, s, f'{name} - Image Viewer')
            if there:
                d.click(there[0] + FRAME_W - 16, there[1] + 14)
                d.vm.wait_screen(lambda s: find(d, s, f'{name} - Image Viewer') is None, 10, interval=0.2)
                break
        if mounted:
            d.move(*PARK)
            d.keys('umount /usb1p1\n')
            term.wait_rows(d, grid, term.prompted, 20)
        term.close_terminal(d)


def desktop(d):
    """the desktop's ways in: Files' Enter on a picture starts wview on it where the desktop's Image Viewer stood, and O
    there asks the desktop's chooser; Apps > Image Viewer starts it with no picture"""
    x, y, _, _ = window('Image Viewer')
    at = (x, y)
    state = {'files': False, 'mounted': False}
    try:
        d.move(*PARK)
        d.key('ctrl-b')
        s = d.visible('Files opens, with the data stick usb1p1 not mounted',
                      lambda s: suite.files_front(d)(s) and suite.files_row(d, s, 'usb1p1', 1) is not None)
        state['files'] = True
        suite.select(d, suite.files_row(d, s, 'usb1p1', 1))
        d.key('ret')
        state['mounted'] = True                         # perhaps: tidy() ejects it then
        s = d.visible('Enter on the stick in Files mounts it, with its img folder',
                      lambda s: suite.files_status(d, 'usb1p1 on /usb1p1: fat32')(s) and
                      suite.files_row(d, s, 'img', 2) is not None)
        suite.select(d, suite.files_row(d, s, 'img', 2))
        d.key('ret')
        s = d.visible('Enter opens the img folder', lambda s: suite.files_row(d, s, 'a-quad.png', 3) is not None)
        rows = {name: suite.files_row(d, s, name, 3) for name in ('a-quad.png', 'b-plain.gif', 'c-rgb24.bmp')}
        suite.select(d, rows['a-quad.png'])
        d.key('ret')
        d.visible('Files: Enter on a PNG starts wview on it, where the desktop\'s Image Viewer stood, with the focus',
                  lambda s: titled(d, s, at, 'a-quad.png - Image Viewer', True) and picture(d, s, at, 42, 30, quadrant, step=3) and
                  status(d, s, at, 'a-quad.png  42 x 30, PNG  (1 of 7)'), 'guiapps-img-files.png', timeout=40)
        for name, colour, w, h in (('b-plain.gif', gif, 23, 17), ('c-rgb24.bmp', lambda px, py: pattern(px, py, 23, 17), 23, 17)):
            d.click(FILES_BAR[0], FILES_BAR[1])     # Files again, by its title bar (wview has the keys)
            d.move(*PARK)
            d.visible(f'a click on its title bar gives Files the focus again, for {name}', suite.files_front(d))
            suite.select(d, rows[name])
            d.key('ret')
            d.visible(f'Files: Enter on {name} shows it in the Image Viewer\'s window, in place of the one there',
                      lambda s: titled(d, s, at, f'{name} - Image Viewer', True) and picture(d, s, at, w, h, colour, step=2),
                      timeout=40)
        d.key('o')
        d.visible('O asks the desktop\'s chooser: its own title, Open a file',
                  lambda s: media.open_chooser(d, s), 'guiapps-img-chooser.png')
        media.choose(d, ['usb1p1', 'img', 'd-base444.jpg'], 'wview')
        d.visible('the chooser\'s JPEG in the same window, fitted, the status bar saying its place',
                  lambda s: titled(d, s, at, 'd-base444.jpg - Image Viewer', True) and
                  picture(d, s, at, 40, 30, lambda px, py: pattern(px, py, 40, 30), tolerance=12, step=3) and
                  status(d, s, at, 'd-base444.jpg  40 x 30, JPEG  (4 of 7)'), 'guiapps-img-chosen.png', timeout=30)
        d.key('ctrl-w')
        d.visible('Ctrl+W ends wview, and its window goes; of three pictures opened from Files, no other viewer is left',
                  lambda s: not any(titled(d, s, at, f'{name} - Image Viewer')
                                    for name in ('a-quad.png', 'b-plain.gif', 'c-rgb24.bmp', 'd-base444.jpg')),
                  'guiapps-img-one-viewer.png', timeout=20)
        d.open_app('Image Viewer')
        d.visible('Apps > Image Viewer starts wview with no picture, where the Image Viewer stands: only Open can be used',
                  lambda s: titled(d, s, at, 'Image Viewer', True) and
                  d.text(s, x + 1 + 16, y + TITLE_H + ROOM_Y + 16, 'Open a picture', FOAM, NAVY) and
                  toolbar(d, s, at, (True,) + (False,) * (len(TOOLS) - 1)),
                  'guiapps-img-apps.png', timeout=30)
        d.key('ctrl-w')
        d.visible('and Ctrl+W ends it', lambda s: not titled(d, s, at, 'Image Viewer'), timeout=20)
        d.key('ctrl-b')
        d.visible('Ctrl+B brings Files back', suite.files_front(d))
        d.click(*suite.EJECT)
        d.visible('Eject in Files lets the stick go', suite.files_status(d, 'usb1: safe to remove'))
        state['mounted'] = False
        d.move(*PARK)
        d.key('ctrl-w')
        d.visible('Files closes', lambda s: not suite.files_front(d)(s))
        state['files'] = False
    except AssertionError:
        if d.outdir:
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-img-desktop-at-failure.png'))
        raise
    finally:
        if state['files'] or state['mounted']:
            for _ in range(3):                          # a chooser left open: cancelled
                if not media.dialog_open(d, d.vm.screen()):
                    break
                d.key('esc')
                d.vm.pump(0.3)
            s = d.vm.screen()
            for title in [f'{name} - Image Viewer' for name in FOLDER] + ['Image Viewer']:
                if titled(d, s, at, title):
                    d.click(at[0] + FRAME_W - 16, at[1] + 14)
                    d.vm.wait_screen(lambda s: not titled(d, s, at, title), 10, interval=0.2)
                    break
            d.key('ctrl-b')
            d.vm.pump(0.5)
            if state['mounted']:
                d.click(*suite.EJECT)
                d.vm.wait_screen(suite.files_status(d, 'usb1: safe to remove'), 15, interval=0.2)
            d.move(*PARK)
            d.key('ctrl-w')


def checks(d):
    terminal(d)
    desktop(d)
