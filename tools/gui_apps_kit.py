"""The guiapps scenario's checks of the window toolkit (gk.h, libsurf's user/libsurf/gk*.c; the desktop applications,
track kit: docs/superpowers/plans/2026-11-05-desktop-apps.md), through its demonstration wkit (user/wkit/), started in
the Terminal; the Driver is gui_apps_scenario.py's. wkit's window comes up with its tab strip, the Controls page lit,
its labels, the name field's hint and its status bar; a click on its title bar gives it the focus, and the name field,
which has the toolkit's focus, its teal border. A name typed shows in the field and in the status bar (each change is
the program's event); Tab goes to the password field, which shows dots; a click on the check box, the switch and a
radio button changes each, as the status bar says. Greet opens a message over the window, which Enter answers; Rename
opens a line to type, whose text, Enter, becomes the name; Copy puts the name on the desktop's clipboard, which the
Terminal pastes later. Ctrl+O, File > Open's shortcut (the toolkit's: it works whether or not the desktop shows a
program's menus), asks for a chooser: the desktop's, or on a desktop without one a line to type the path in over the
window; Esc cancels either. The Lists tab shows the list with its columns: a click selects a row, Down the next, and the
wheel scrolls it. The Drawing tab shows the area the program draws, with its scroll bars: a click marks a cell, and the
wheel scrolls it by rows. The corner grip makes the window smaller, and the layout follows (the status bar at the new
bottom, the size in it). Its close button asks first, and Close ends it: ps finds no wkit. The checks leave the
desktop as they found it (wkit ended, the Terminal closed), also when one fails."""
import os
import re

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
CORAL = bytes.fromhex('ee906d')
from gui_apps_win import answered

W, H = 600, 360                     # wkit's content (user/wkit/demo.h)
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
SHEET = bytes.fromhex('fffdf5')
SHELL = bytes.fromhex('dee3d8')
LINE = bytes.fromhex('c9cfc4')
PARK = (900, 480)                   # the pointer clear of the windows and of the Terminal's text
EDIT_X, PASTE_Y = 160, 149          # the Edit menu's title, and its Paste item's middle (gui_apps_win.py's)
STATUS_Y = H - 22                   # the status bar, in the content
TABS = {'Controls': 8, 'Lists': 8 + 78 + 4, 'Drawing': 8 + 78 + 4 + 54 + 4, 'More': 8 + 78 + 4 + 54 + 4 + 70 + 4}   # each tab's left in the strip


def places(h=H):
    """where shell/guiclient.c opens the desktop's k-th program window, kept on a 960x540 screen (gui_place())"""
    for k in range(8):
        yield 72 + 24 * k, min(52 + 20 * k, 540 - 26 - 2 - (h + FRAME_H))


def titled_at(d, s, at, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, 'Toolkit', PAPER, bar) for bar in bars)


def find(d, s, focused=None):
    for at in places():
        if titled_at(d, s, at, focused):
            return at
    return None


class Win:
    """wkit's window at at: its content's top left"""
    def __init__(self, d, at):
        self.d, self.at = d, at
        self.x, self.y = at[0] + 1, at[1] + TITLE_H
        self.w, self.h = W, H

    def text(self, s, x, y, text, fg, bg=None):
        return self.d.text(s, self.x + x, self.y + y, text, fg, bg)

    def pixel(self, s, x, y):
        return self.d.pixel(s, self.x + x, self.y + y)

    def click(self, x, y):
        self.d.click(self.x + x, self.y + y)

    def status(self, text):
        """a predicate: the status bar's text, from its start"""
        return lambda s: self.text(s, 10, self.h - 22 + 3, text, INK, BAND)

    def tab(self, name):
        self.click(TABS[name] + 12, 12)


def checks(d):
    grid = term.Grid()
    state = {}
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('kit: Ctrl+T opens the Terminal, for wkit', d.title('Terminal'))
        d.require('kit: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('wkit &\n')
        s = d.visible('kit: wkit opens its window, without the focus: the toolkit\'s demonstration',
                      lambda s: find(d, s, False) is not None, timeout=20)
        state['open'] = True
        w = Win(d, find(d, s))
        state['win'] = w
        d.require('kit: its tab strip with Controls lit, the labels, the name\'s hint and the status bar',
                  w.pixel(s, 9, 5) == TEAL and w.text(s, 15, 7, 'Controls', PAPER, TEAL) and
                  w.text(s, TABS['Lists'] + 7, 7, 'Lists', INK, SHELL) and w.text(s, 10, 43, 'Name:', INK, PAPER) and
                  w.text(s, 110, 43, 'Type a name', MUTED, SHEET) and w.status('Ready')(s))
        if d.outdir:
            s.png(os.path.join(d.outdir, 'guiapps-kit.png'))
        d.click(w.at[0] + 250, w.at[1] + 14)            # its title bar: the focus
        d.visible('kit: a click on its title bar gives it the focus; the name field, the toolkit\'s focus, is lit',
                  lambda s: titled_at(d, s, w.at, True) and w.pixel(s, 106, 40) == TEAL)
        d.move(*PARK)
        d.keys('surf')
        d.visible('kit: a name typed shows in its field and in the status bar',
                  lambda s: w.text(s, 110, 43, 'surf', INK) and w.status('Name: surf')(s), 'guiapps-kit-typed.png')
        d.key('tab')
        d.keys('abc')
        d.visible('kit: Tab goes to the password field, which shows a dot for each character (code page 437\'s 7)',
                  lambda s: w.status('Password: 3 bytes')(s) and w.text(s, 110, 71, '\x07\x07', INK, SHEET))
        w.click(10 + 5, 96 + 11)                        # the check box
        d.visible('kit: a click on the check box turns Wi-Fi off', w.status('Wi-Fi off'))
        w.click(96 + 10, 96 + 11)                       # the switch
        d.visible('kit: a click on the switch turns it on', lambda s: w.status('Join at startup on')(s) and
                  w.pixel(s, 96 + 8, 96 + 4 + 2) == TEAL)
        w.click(106 + 52 + 16 + 68 + 16 + 10, 124 + 11)   # Fast, after Slow (52) and Normal (68), 16 apart
        d.visible('kit: a click on a radio button chooses it', w.status('Speed: Fast'))
        # boxes over the window
        bx, by = (W - 470) // 2, (H - 146) // 2
        w.click(10 + 32, 159 + 12)                      # Greet
        d.visible('kit: Greet opens a message over the window',
                  lambda s: w.text(s, bx + 28, by + 6, 'Greet', PAPER, TEAL) and
                  w.text(s, bx + 16, by + 44, 'Hello, surf.', INK, PAPER), 'guiapps-kit-message.png')
        d.key('ret')
        d.visible('kit: Enter answers it, and it goes', lambda s: w.status('Answered 0')(s) and
                  not w.text(s, bx + 28, by + 6, 'Greet', PAPER, TEAL))
        w.click(154 + 38, 159 + 12)                     # Rename
        ay = (H - 160) // 2
        d.visible('kit: Rename opens a line to type, with the name in it',
                  lambda s: w.text(s, bx + 28, ay + 6, 'Rename', PAPER, TEAL) and
                  w.text(s, bx + 16 + 4, ay + 66 + 3, 'surf', INK))
        d.keys('sea\n')
        d.visible('kit: what is typed replaces it, and Enter makes it the name',
                  lambda s: w.status('Renamed: sea')(s) and w.text(s, 110, 43, 'se', INK, SHEET))
        w.click(238 + 32, 159 + 12)                     # Copy
        d.visible('kit: Copy puts the name on the desktop\'s clipboard', w.status('Copied the name'))
        # a chooser by File > Open's shortcut: the desktop's (220, 81 at 960x540, as the launcher's), or the line the
        # toolkit opens over the window instead
        d.key('ctrl-o')
        d.visible('kit: Ctrl+O, a menu\'s shortcut, asks for a chooser: the desktop\'s, or a line to type the path in',
                  lambda s: d.text(s, 220 + 28, 81 + 6, 'Open a file', PAPER, TEAL) or
                  w.text(s, bx + 28, (H - 160) // 2 + 6, 'Open a file', PAPER, TEAL), 'guiapps-kit-chooser.png')
        d.key('esc')
        d.visible('kit: Esc cancels it', w.status('Not chosen'))
        # the Lists page
        w.tab('Lists')
        d.visible('kit: the Lists tab shows the list, with its columns', lambda s: w.status('Page: Lists')(s) and
                  w.text(s, 16, 42, 'SurfSim', INK, SHEET) and w.text(s, 16 + 112, 42, '-40 dB', INK, SHEET) and
                  w.text(s, 16 + 184, 42, 'open', INK, SHEET), 'guiapps-kit-lists.png')
        w.click(40, 41 + 18 + 8)
        d.visible('kit: a click selects a row', lambda s: w.status('Selected: SurfSimWPA')(s) and
                  w.pixel(s, 13, 41 + 18 + 2) == bytes.fromhex('bfe3dd'))
        d.move(*PARK)
        d.key('down')
        d.visible('kit: Down selects the next', w.status('Selected: Harbour'))
        d.move(w.x + 100, w.y + 100)
        d.vm.mon.cmd('mouse_move 0 0 -1')               # a notch toward the user: three rows down
        d.visible('kit: the wheel scrolls the list three rows', lambda s: w.text(s, 16, 42, 'Lighthouse', INK))
        # the Drawing page
        w.tab('Drawing')
        d.visible('kit: the Drawing tab shows the area the program draws, with its scroll bars',
                  lambda s: w.status('Page: Drawing')(s) and w.pixel(s, 5, 30 + 5) == SHEET and
                  w.pixel(s, 25, 30 + 5) == BAND and w.pixel(s, W - 6, 30 + 20) == MUTED, 'guiapps-kit-drawing.png')
        w.click(3 * 20 + 9, 30 + 2 * 20 + 9)
        d.visible('kit: a click marks a cell (teal, clear of the pointer at its top left)',
                  lambda s: w.status('Cell 3, 2, marked')(s) and w.pixel(s, 3 * 20 + 3, 30 + 2 * 20 + 3) == TEAL)
        d.move(w.x + 200, w.y + 150)
        d.vm.mon.cmd('mouse_move 0 0 -1')
        d.visible('kit: the wheel scrolls the area three rows', lambda s: w.status('Scrolled to 0, 3')(s) and
                  w.pixel(s, 3 * 20 + 3, 30 + 2 * 20 + 3) != TEAL)
        # stage 2: the More page, after the Drawing page's checks and before the smaller window
        w.tab('More')
        d.visible('kit: the More tab shows notes, a progress bar, a slider and palettes',
                  lambda s: w.status('Page: More')(s) and w.text(s, 10, 43, 'Notes:', INK, PAPER) and
                  w.pixel(s, 15, 70) == SHEET, 'guiapps-kit-more.png')
        w.click(10 + 40, 68 + 40)                       # the notes: a click, then lines typed
        d.move(*PARK)
        d.keys('tide\n')
        d.keys('swell')
        d.visible('kit: lines typed in the notes (a text of several lines)',
                  lambda s: w.status('Notes: 10 bytes')(s) and w.text(s, 15, 70, 'tide', INK) and
                  w.text(s, 15, 86, 'swel', INK))
        w.click(390 + 200 - 2, 120 + 11)                # the slider's right end
        d.visible('kit: a click at the slider\'s end sets it to 100', w.status('Volume: 100'))
        w.click(390 + 1 + 3 * 20 + 9, 176 + 1 + 9)      # the fourth colour: coral
        d.move(*PARK)                                   # (the pointer off the cell it would cover)
        d.visible('kit: a click on a colour of the palette chooses it', lambda s: w.status('Colour: #EE906D')(s) and
                  w.pixel(s, 390 + 1 + 3 * 20 + 9, 176 + 1 + 14) == CORAL)
        w.click(390 + 1 + 26 + 12, 252 + 1 + 12)        # the eraser
        d.visible('kit: a click on a tool of the palette chooses it', w.status('Tool: Eraser'))
        d.visible('kit: the progress bar counts the seconds in teal', lambda s: w.pixel(s, 390 + 2, 68 + 4) == TEAL)
        # a smaller window: laid out again
        gx, gy = w.at[0] + W + FRAME_W - 6, w.at[1] + H + FRAME_H - 6
        d.move(gx, gy)
        d.button(True)
        d.move(gx - 60, gy - 40)
        d.button(False)
        w.w, w.h = W - 60, H - 40
        d.visible('kit: its corner grip makes it smaller, and the layout follows: the status bar at the new bottom',
                  w.status('Size 540x320'), 'guiapps-kit-smaller.png')
        # the close button asks first
        d.click(w.at[0] + w.w + FRAME_W - 16, w.at[1] + 14)
        qx, qy = (w.w - 470) // 2, (w.h - 146) // 2
        d.visible('kit: its close button asks first',
                  lambda s: w.text(s, qx + 28, qy + 6, 'Close the demonstration?', PAPER, TEAL))
        d.key('ret')
        d.visible('kit: Close ends it, and its window goes', lambda s: find(d, s) is None, timeout=15)
        state['open'] = False
        d.move(*PARK)
        # the name Copy put on the clipboard, pasted into the Terminal
        d.keys('echo kit-')
        d.click(EDIT_X, 14)
        d.click(EDIT_X + 18, PASTE_Y)
        d.move(*PARK)
        d.key('ret')
        d.require('kit: the name Copy put on the clipboard, pasted into the Terminal, runs there',   # (surfsh's
                  term.wait_rows(d, grid, lambda rows: answered(rows, 'echo kit-sea', 'kit-sea')) is not None)   # job
                                                                    # notice may come between the line and the prompt)
        d.keys('ps | grep -c wkit\n')
        d.require('kit: ps finds no wkit', term.wait_rows(
            d, grid, lambda rows: answered(rows, 'ps | grep -c wkit', '0')) is not None)
        state['done'] = True
    finally:
        if not state.get('done') and d.outdir:         # the screen as the failure left it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-kit-at-failure.png'))
        try:
            tidy(d, state)
        except AssertionError:                          # what failed first is what the results say
            pass


def tidy(d, state):
    """wkit ended (its close button and Close, or End when it does not answer), then the Terminal closed"""
    if state.get('open'):
        s = d.vm.screen()
        at = find(d, s)
        if at is not None:
            for width in (W, W - 60):
                d.click(at[0] + width + FRAME_W - 16, at[1] + 14)
                if d.vm.wait_screen(lambda s: find(d, s) is None, 3, interval=0.2) is not None:
                    break
                d.key('ret')                            # its question: Close
                if d.vm.wait_screen(lambda s: find(d, s) is None, 5, interval=0.2) is not None:
                    break
                if term.dialog_up(d, d.vm.screen(), 'End the program?'):
                    d.key('right')
                    d.key('ret')
                    break
    term.close_terminal(d)
