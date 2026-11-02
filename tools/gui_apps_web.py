"""The guiapps scenario's checks of the web browser in a window (wundertow, docs/superpowers/plans/2026-10-25-browser.md;
the Driver is gui_apps_scenario.py's). 10/25/26: started in the Terminal on a page of the data stick, wundertow opens a
window in front titled after the page (without the focus, as a program started with & does); a click on its title bar
gives it the focus; its text is where the layout puts it, in the page's colours, its heading at twice the size and
bold; a click on a link opens the next page, Backspace goes back to the first, Page Down scrolls a screen and Home
goes back to the top; its corner dragged makes it smaller and the page is laid out again at the new width; the close
button ends it. The pages are this module's own, on the stick (STICK); the stick is mounted in the Terminal and
unmounted again, and the Terminal closed, also when a check fails.

11/1/26 (milestone 2, stage 2.7): on a page of the stick a PNG, a GIF, a BMP and a JPEG show where their pixels should be,
each compared with what it was made from, and a broken one keeps its frame; Ctrl+F finds a word and shows it, Enter
and Shift+Enter go from place to place, Escape closes it; Tab reaches a link, Shift+Tab goes back, Enter follows it,
and it then has the visited colour; the scroll bar's thumb dragged moves the page by the rule of its track.
What needs a server is gui_apps_webnet.py's, on a boot with a network."""
import os

import gui_apps_term as term
from gui_apps_scenario import PAPER, TEAL, MUTED

MUTED_TEXT = bytes.fromhex('6c888c')    # an image's alt text in its frame

LINES = 60
STICK = {
    'web/one.html': ('<!DOCTYPE html><title>One</title><h1>Wave one</h1>'
                     '<p>First paragraph with <a href="two.html">a link to page two</a>. It goes on, wrapped when '
                     'narrower.' + ''.join(f'<p>Line {i}' for i in range(1, LINES + 1))).encode(),
    'web/two.html': b'<!DOCTYPE html><title>Two</title><h1>Wave two</h1><p>Page two, reached by its link.',
    'web/pics.html': (b'<!DOCTYPE html><title>Pics</title><p>Pictures<p><img src=rgb8.png><p><img src=plain.gif>'
                      b'<p><img src=rgb24.bmp><p><img src=base444.jpg><p><img src=broken.png alt=broken><p>After them'),
}
IMAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tests', 'fixtures', 'wundertow', 'images')
for name in ('rgb8.png', 'plain.gif', 'rgb24.bmp', 'base444.jpg'):
    with open(os.path.join(IMAGES, name), 'rb') as f:
        STICK['web/' + name] = f.read()
STICK['web/broken.png'] = STICK['web/rgb8.png'][:200]
PARK = (900, 480)           # the pointer clear of the text checked: right of the page's left 320 pixels wherever
                            # the window is (beside it when it is the desktop's first, 72 + 762 across)
TITLE_H = 28
WIDTH = 760                 # the window wundertow asks for, 760 x 460: the desktop makes it 457 high at 960x540
FRAME_W = WIDTH + 2         # a pixel of border at each side
CONTENT_H = 540 - 28 - 26 - TITLE_H - 1     # 457: the room between the menu bar and the taskbar, less the title bar
TOOLBAR, STATUS = 31, 22    # user/wundertow/browser.h
INK, SHEET, LINK = bytes.fromhex('153d49'), bytes.fromhex('fffdf5'), bytes.fromhex('197d83')
VISITED, FOUND, BAND = bytes.fromhex('6c4f8c'), bytes.fromhex('ffd34d'), bytes.fromhex('e7e2d2')
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


def heading_at(d, s, at, y, text, x=8):
    """text at twice the size and bold on the page, as user/wundertow/paint.c draws it: each pixel of the font lit,
    and the one after it in its row, as 2 x 2; every other pixel of its cells the page's"""
    x0, y0 = at[0] + 1 + x, at[1] + TITLE_H + TOOLBAR + y
    for index, char in enumerate(text):
        for row, bits in enumerate(d.font[ord(char)]):
            bits |= bits >> 1
            for col in range(8):
                colour = INK if bits & (0x80 >> col) else SHEET
                px, py = x0 + index * 16 + col * 2, y0 + row * 2
                if any(d.pixel(s, px + a, py + b) != colour for a in (0, 1) for b in (0, 1)):
                    return False
    return True


def pattern(x, y, w, h):
    """tools/wundertow-images.py's: what the test pictures hold"""
    return bytes((x * 255 // max(w - 1, 1), y * 255 // max(h - 1, 1), (x + y) * 255 // max(w + h - 2, 1)))


def picture_at(d, s, at, y, w, h, colour, tolerance=0):
    """a picture of w x h on the page at (8, y), each pixel what colour(px, py) says, every fifth one of it"""
    x0, y0 = at[0] + 1 + 8, at[1] + TITLE_H + TOOLBAR + y
    for py in range(0, h, 5):
        for px in range(0, w, 5):
            got, want = d.pixel(s, x0 + px, y0 + py), colour(px, py)
            if any(abs(a - b) > tolerance for a, b in zip(got, want)):
                return False
    return True


def opened(d, name, title):
    """wundertow on a page of the stick, its window given the focus: where it is"""
    d.keys(f'wundertow /usb1p1/web/{name} &\n')
    s = d.visible(f'wundertow opens {name}', lambda s: find(d, s, title) is not None, timeout=40)
    at = find(d, s, title)
    d.click(at[0] + 150, at[1] + 14)
    d.move(*PARK)
    d.visible(f'{title}: the focus', lambda s: titled(d, s, at, title, True), timeout=20)
    return at


def close_window(d, at, title):
    d.click(at[0] + FRAME_W - 16, at[1] + 14)
    d.visible(f'{title}: its close button ends it', lambda s: find(d, s, title) is None, timeout=20)


def pictures(d):
    """the acceptance test's pictures: paragraphs 16 apart from y 24: its text, then each picture at its own size"""
    def gif(x, y):
        i = (x * 3 + y * 5) % 16
        return bytes((i * 255 // 15, 255 - i * 255 // 15, i * 37 & 255))
    at = opened(d, 'pics.html', 'Pics - Undertow')
    png_y, gif_y, bmp_y, jpg_y, frame_y = 56, 89, 122, 155, 201
    d.visible('a PNG, a GIF, a BMP and a JPEG show where their pixels should be, and a broken one keeps its frame',
              lambda s: picture_at(d, s, at, png_y, 23, 17, lambda x, y: pattern(x, y, 23, 17)) and
              picture_at(d, s, at, gif_y, 23, 17, gif) and
              picture_at(d, s, at, bmp_y, 23, 17, lambda x, y: pattern(x, y, 23, 17)) and
              picture_at(d, s, at, jpg_y, 40, 30, lambda x, y: pattern(x, y, 40, 30), tolerance=12) and
              text_at(d, s, at, frame_y + 4, 'broken', MUTED_TEXT, 8 + 4) and
              text_at(d, s, at, frame_y + 24 + 16, 'After them'), 'guiapps-web-pictures.png', timeout=60)
    close_window(d, at, 'Pics - Undertow')


def tinted(c):
    """a colour under find's mark"""
    return bytes(a * b // 255 for a, b in zip(c, FOUND))


def status_right(d, s, at, text):
    """find's count, at the right of the status bar"""
    x = at[0] + 1 + WIDTH - 24 - 8 * len(text)
    return d.text(s, x, at[1] + TITLE_H + CONTENT_H - STATUS + 3, text, MUTED_TEXT, BAND)


def marked(d, s, at, y, text, x=8):
    """text on the page under find's mark"""
    return d.text(s, at[0] + 1 + x, at[1] + TITLE_H + TOOLBAR + y, text, tinted(INK), tinted(SHEET))


def reading(d):
    """find, links by keyboard, a visited link, the thumb, on the page of 60 lines: see user/wundertow/browser.c's
    show_rows(): what is out of view comes to a quarter of the view from its edge"""
    at = opened(d, 'one.html', 'One - Undertow')
    line = lambda k: P1_Y + 32 * k
    d.key('ctrl-f')
    d.keys('Line 4')
    d.visible('Ctrl+F: the word is found as it is typed, and shows', lambda s: status_right(d, s, at, '1 of 11') and
              marked(d, s, at, line(4), 'Line 4'), 'guiapps-web-find.png', timeout=20)
    d.key('ret')
    scroll = line(40) + 16 - SCREEN + SCREEN // 4
    d.visible('Enter: the next place comes into view', lambda s: status_right(d, s, at, '2 of 11') and
              marked(d, s, at, line(40) - scroll, 'Line 4'), timeout=20)
    d.key('shift-ret')
    scroll = line(4) - SCREEN // 4
    d.visible('Shift+Enter: the one before', lambda s: status_right(d, s, at, '1 of 11') and
              marked(d, s, at, line(4) - scroll, 'Line 4'), timeout=20)
    d.key('esc')
    d.visible('Escape closes it, and the mark goes', lambda s: text_at(d, s, at, line(4) - scroll, 'Line 4'), timeout=20)
    d.key('home')
    d.key('tab')
    frame = (at[0] + 1 + LINK_X + 8, at[1] + TITLE_H + TOOLBAR + P1_Y - 1)
    d.visible('Tab reaches the link, drawn focused', lambda s: d.pixel(s, *frame) == INK, 'guiapps-web-tab.png', timeout=20)
    d.key('tab')
    d.key('shift-tab')
    d.visible('Tab goes round the only link, and Shift+Tab stays on it', lambda s: d.pixel(s, *frame) == INK, timeout=20)
    d.key('ret')
    d.visible('Enter follows it', lambda s: titled(d, s, at, 'Two - Undertow'), timeout=30)
    d.key('backspace')
    d.visible('back, the link has the visited colour', lambda s: titled(d, s, at, 'One - Undertow') and
              text_at(d, s, at, P1_Y, 'a link to page two', VISITED, LINK_X), 'guiapps-web-visited.png', timeout=20)
    # the page is 2016 high and shows 404: the thumb is 80 of the track's 404, and 324 of track are 1612 of page
    x, y = at[0] + 1 + WIDTH - 6, at[1] + TITLE_H + TOOLBAR + 20
    d.move(x, y)
    d.button(True)
    d.move(x, y + 100)
    d.button(False)
    d.move(*PARK)
    scroll = 100 * 1612 // 324
    d.visible('the scroll bar\'s thumb dragged 100 pixels moves the page by the rule of its track',
              lambda s: text_at(d, s, at, line(14) - scroll, 'Line 14'), 'guiapps-web-thumb.png', timeout=20)
    close_window(d, at, 'One - Undertow')


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
        d.check('the heading at twice the size and bold', heading_at(d, s, at, H1_Y, 'Wave one'))
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
        pictures(d)
        reading(d)
    except AssertionError:
        if d.outdir:                                        # the screen as the check found it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-web-at-failure.png'))
        raise
    finally:
        for title in ('One - Undertow', 'Two - Undertow', 'Pics - Undertow'):
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
