"""The guiapps scenario's checks of the web browser in a window (wundertow, docs/superpowers/plans/2026-10-25-browser.md;
the Driver is gui_apps_scenario.py's). 10/25/26: started in the Terminal on a page of the data stick, wundertow opens a
window in front titled after the page (without the focus, as a program started with & does); a click on its title bar
gives it the focus; its text is where the layout puts it, in the page's colours, its heading in the 16x32 font and
bold; a click on a link opens the next page, Backspace goes back to the first, Page Down scrolls a screen and Home
goes back to the top; its corner dragged makes it smaller and the page is laid out again at the new width; the close
button ends it. The pages are this module's own, on the stick (STICK); the stick is mounted in the Terminal and
unmounted again, and the Terminal closed, also when a check fails."""
import os
import re

import gui_apps_term as term
from gui_apps_scenario import PAPER, TEAL, MUTED

LINES = 60
STICK = {
    'web/one.html': ('<!DOCTYPE html><title>One</title><h1>Wave one</h1>'
                     '<p>First paragraph with <a href="two.html">a link to page two</a>. It goes on, wrapped when '
                     'narrower.' + ''.join(f'<p>Line {i}' for i in range(1, LINES + 1))).encode(),
    'web/two.html': b'<!DOCTYPE html><title>Two</title><h1>Wave two</h1><p>Page two, reached by its link.',
}
PARK = (900, 480)           # the pointer clear of the text checked: right of the page's left 320 pixels wherever
                            # the window is (beside it when it is the desktop's first, 72 + 762 across)
TITLE_H = 28
WIDTH = 760                 # the window wundertow asks for, 760 x 460: the desktop makes it 457 high at 960x540
FRAME_W = WIDTH + 2         # a pixel of border at each side
CONTENT_H = 540 - 28 - 26 - TITLE_H - 1     # 457: the room between the menu bar and the taskbar, less the title bar
TOOLBAR, STATUS = 31, 22    # user/wundertow/browser.h
INK, SHEET, LINK = bytes.fromhex('153d49'), bytes.fromhex('fffdf5'), bytes.fromhex('197d83')
# where the layout puts the text (user/wundertow/layout.c at 748 pixels, the content less the scroll bar: an 8-pixel
# margin, h1 32 high with 16 above and below, paragraphs 16 high and 16 apart)
H1_Y = 8 + 16                           # 24: the heading
P1_Y = H1_Y + 32 + 16                   # 72: the first paragraph, one line of 75 characters at 748 pixels
LINK_X = 8 + len('First paragraph with ') * 8
SCREEN = CONTENT_H - TOOLBAR - STATUS   # 404: what Page Down scrolls is this less 32
LINE10_Y = P1_Y + 32 * 10 - (SCREEN - 32)   # Line 10's place after one Page Down
# the corner dragged 180 left and 100 up: 580 x 357, laid out at 568 pixels, 69 characters a line: the first
# paragraph's last word goes to a line of its own
DRAG = (180, 100)
# a program's window has its top at the menu bar while it is as tall as the room, 6 pixels below it once it is less
# tall (gui_place's top_limit, shell/guicore.c): where the drag leaves it when wundertow draws at the new size
TOP, SHORT_TOP = 28, 28 + 6


def place(k, top=TOP):
    """where the desktop puts its k-th program window (from 0) when it is this tall: the cascade's place
    (shell/guiclient.c), kept on the screen by gui_place: as high as a window may be, at most 960 - FRAME_W across
    (made less tall, it keeps its left side and has its top at SHORT_TOP)"""
    return min(72 + 24 * (k % 8), 960 - FRAME_W), top


def titled(d, s, at, text, focused=None):
    """the window at at has text (its start) in its title bar, in front with the focus or without (None: either)"""
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, text, PAPER, bar) for bar in bars)


def find(d, s, text):
    for k in range(8):
        for top in (TOP, SHORT_TOP):
            if titled(d, s, place(k, top), text):
                return place(k, top)
    return None


def text_at(d, s, at, y, text, colour=INK, x=8):
    """text on the page, x and y from the page's top left (below the toolbar)"""
    return d.text(s, at[0] + 1 + x, at[1] + TITLE_H + TOOLBAR + y, text, colour, SHEET if colour == INK else None)


def font_rows(name):
    """{character: its rows} of a console font, driver/font<name>.c, each row an integer whose top bit of 16 is the
    leftmost pixel, as user/wundertow/font.c gives them"""
    width = int(name.split('x')[0])
    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'driver', f'font{name}.c')
    rows = {}
    with open(path, encoding='utf-8') as f:
        for index, values in re.findall(r'\[0x([0-9a-fA-F]{2})\] = \{ ([^}]*) \}', f.read()):
            rows[int(index, 16)] = [int(v, 16) << (8 if width == 8 else 0) for v in values.split(',')]
    return rows


FONT32 = font_rows('16x32')


def heading_at(d, s, at, y, text, x=8):
    """h1 on the page as user/wundertow/paint.c draws it: the 16x32 font, bold as a smear of two pixels to the right,
    the page's colour everywhere else in its cells"""
    x0, y0 = at[0] + 1 + x, at[1] + TITLE_H + TOOLBAR + y
    for row in range(32):
        line = 0
        for index, char in enumerate(text):
            bits = FONT32[ord(char)][row] << 2
            bits |= bits >> 1
            bits |= bits >> 1
            line |= bits << (16 * (len(text) - 1 - index))
        for col in range(16 * len(text)):
            lit = line & (1 << (16 * len(text) + 2 - 1 - col))
            if d.pixel(s, x0 + col, y0 + row) != (INK if lit else SHEET):
                return False
    return True


def checks(d):
    grid = term.Grid()
    mounted = False
    at = None
    frame_w = FRAME_W
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal, for the browser', d.title('Terminal'))
        d.require('the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('mount usb1p1\n')
        mounted = d.check('the data stick is mounted in the Terminal',
                          term.wait_rows(d, grid, lambda rows: any('usb1p1 on /usb1p1' in r for r in rows), 30) is not None)
        d.keys('wundertow /usb1p1/web/one.html &\n')
        s = d.visible('wundertow opens a window in front, titled after the page',
                      lambda s: find(d, s, 'One - Undertow') is not None, timeout=40)
        at = find(d, s, 'One - Undertow')
        d.click(at[0] + 150, at[1] + 14)                    # its title bar: the focus
        s = d.visible('a click on its title bar gives it the focus; its text is where the layout puts it',
                      lambda s: titled(d, s, at, 'One - Undertow', True) and
                      text_at(d, s, at, P1_Y, 'First paragraph with '), 'guiapps-web-one.png')
        d.check('the heading in its real size and bold', heading_at(d, s, at, H1_Y, 'Wave one'))
        d.check('the link in the link colour', text_at(d, s, at, P1_Y, 'a link to page two', LINK, LINK_X))
        d.click(at[0] + 1 + LINK_X + 24, at[1] + TITLE_H + TOOLBAR + P1_Y + 8)
        d.move(*PARK)                                       # the pointer off the page: its arrow hides no text checked
        d.visible('a click on the link opens the page it names',
                  lambda s: titled(d, s, at, 'Two - Undertow') and text_at(d, s, at, P1_Y, 'Page two, reached by its link.'),
                  'guiapps-web-two.png', timeout=30)
        d.key('backspace')
        d.visible('Backspace goes back to the first page', lambda s: titled(d, s, at, 'One - Undertow') and
                  text_at(d, s, at, P1_Y, 'First paragraph with '), timeout=20)
        d.key('pgdn')
        d.visible('Page Down scrolls a screen: Line 10 where the layout and the scroll put it',
                  lambda s: text_at(d, s, at, LINE10_Y, 'Line 10') and not text_at(d, s, at, P1_Y, 'First paragraph with '),
                  'guiapps-web-scrolled.png', timeout=20)
        d.key('home')
        d.visible('Home goes back to the top', lambda s: text_at(d, s, at, P1_Y, 'First paragraph with ') and
                  not text_at(d, s, at, P1_Y + 16, 'narrower.'), timeout=20)
        gx, gy = at[0] + FRAME_W - 6, at[1] + TITLE_H + CONTENT_H + 1 - 6
        d.move(gx, gy)                                      # the corner grip, the desktop's own
        d.button(True)
        d.move(gx - DRAG[0], gy - DRAG[1])
        d.button(False)
        frame_w = FRAME_W - DRAG[0]
        at = (at[0], SHORT_TOP)                             # less tall than the room now: its top 6 pixels lower
        d.move(*PARK)
        d.visible('its corner dragged 180 left and 100 up: the page laid out again at the new width, the first '
                  'paragraph\'s last word on a line of its own',
                  lambda s: text_at(d, s, at, P1_Y, 'First paragraph with ') and text_at(d, s, at, P1_Y + 16, 'narrower.'),
                  'guiapps-web-resized.png', timeout=20)
        d.click(at[0] + frame_w - 16, at[1] + 14)           # its close button
        d.visible('the close button ends wundertow, and its window goes',
                  lambda s: find(d, s, 'One - Undertow') is None, timeout=20)
    except AssertionError:
        if d.outdir:                                        # the screen as the check found it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-web-at-failure.png'))
        raise
    finally:
        for title in ('One - Undertow', 'Two - Undertow'):
            there = find(d, d.vm.screen(), title)
            if there:
                for w in dict.fromkeys((frame_w, FRAME_W)):     # its close button: as resized, else as it was
                    d.click(there[0] + w - 16, there[1] + 14)
                    if d.vm.wait_screen(lambda s: find(d, s, title) is None, 10, interval=0.2) is not None:
                        break
                break
        if mounted:
            d.move(*PARK)
            d.keys('umount /usb1p1\n')
            term.wait_rows(d, grid, term.prompted, 20)
        term.close_terminal(d)
