"""The guiapps scenario's checks of the web browser in a window (wundertow, docs/superpowers/plans/2026-10-25-browser.md;
the Driver is gui_apps_scenario.py's). 10/25/26: started in the Terminal on a page of the data stick, wundertow opens a
window in front titled after the page (without the focus, as a program started with & does); a click on its title bar
gives it the focus; its text is where the layout puts it, in the page's colours, its heading in the 16x32 font and
bold; a click on a link opens the next page, Backspace goes back to the first, Page Down scrolls a screen and Home
goes back to the top; its corner dragged makes it smaller and the page is laid out again at the new width; the close
button ends it. The pages are this module's own, on the stick (STICK); the stick is mounted in the Terminal and
unmounted again, and the Terminal closed, also when a check fails."""
import os

import fontmetrics
import gui_apps_term as term
from gui_apps_scenario import PAPER, TEAL, MUTED

LINES = 60
# user/wundertow/font.c: a page's text is proportional, the console's fonts trimmed to their ink (tools/fontmetrics.py)
FONTS = {16: '8x16', 24: '12x24', 32: '16x32'}
CELL, BOLD = {16: 8, 24: 12, 32: 16}, {16: 1, 24: 1, 32: 2}
METRICS = {size: fontmetrics.metrics(name) for size, name in FONTS.items()}
ROWS = {size: fontmetrics.rows(name) for size, name in FONTS.items()}
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
P1_Y = H1_Y + 32 + 16                   # 72: the first paragraph, one line of 516 pixels at 748 pixels
SCREEN = CONTENT_H - TOOLBAR - STATUS   # 404: what Page Down scrolls is this less 32
LINE10_Y = P1_Y + 32 * 10 - (SCREEN - 32)   # Line 10's place after one Page Down
# the corner dragged 240 left and 100 up: 520 x 357, laid out at 508 pixels: the first paragraph's last word goes to a
# line of its own
DRAG = (240, 100)
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


def advance(char, size=16, bold=False):
    """how far on the next character is in a page's proportional text, as user/wundertow/font.c has it"""
    return METRICS[size][ord(char)][1] + (BOLD[size] if bold and char != ' ' else 0)


def text_width(text, size=16, bold=False):
    return sum(advance(char, size, bold) for char in text)


def ink(text, size=16, bold=False):
    """the pixels a page's proportional text lights, from its top left, as user/wundertow/paint.c draws it: each cell
    of its font drawn left of its place by its bearing, bold as a smear to the right"""
    lit, pen = set(), 0
    width, extra = CELL[size], BOLD[size] if bold else 0
    for char in text:
        bearing = METRICS[size][ord(char)][0]
        for row, bits in enumerate(ROWS[size][ord(char)]):
            for col in range(width):
                if bits & (1 << (width - 1 - col)):
                    lit.update((pen - bearing + col + k, row) for k in range(extra + 1))
        pen += advance(char, size, bold)
    return lit


def page_text(d, s, at, y, text, colour, background, x, size=16, bold=False):
    """the text's pixels in its colour, and with a background every other pixel of its width and height in that"""
    x0, y0 = at[0] + 1 + x, at[1] + TITLE_H + TOOLBAR + y
    lit = ink(text, size, bold)
    if any(d.pixel(s, x0 + px, y0 + py) != colour for px, py in lit):
        return False
    return background is None or all(d.pixel(s, x0 + px, y0 + py) == background or (px, py) in lit
                                     for px in range(text_width(text, size, bold)) for py in range(size))


def text_at(d, s, at, y, text, colour=INK, x=8):
    """text on the page, x and y from the page's top left (below the toolbar)"""
    return page_text(d, s, at, y, text, colour, SHEET if colour == INK else None, x)


def heading_at(d, s, at, y, text, x=8):
    """h1 on the page: the 16x32 font, bold, the page's colour everywhere else in its width"""
    return page_text(d, s, at, y, text, INK, SHEET, x, 32, True)


LINK_X = 8 + text_width('First paragraph with ')


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
        d.visible('its corner dragged 240 left and 100 up: the page laid out again at the new width, the first '
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
