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
desktop as they found it (a dialog left open closed, wkit ended, the Terminal closed), also when one fails.
11/05/26 (stage 3: the toolkit on the desktop's own calls, track desk's 2c5afd30): wkit's menus File and View stand
in the bar in place of File and Edit, greyed while a message is open over the window; keys no widget takes reach the program with their modifiers (Alt+X with no Esc
before it, Ctrl+0, Ctrl+Enter, Ctrl+Esc), Ctrl+Backspace deletes a word, Ctrl+Tab moves the focus; Copy pastes back
the window's own copy at once; Ctrl+O and File > Open in the bar (Ctrl+G) ask the desktop's chooser; View > Speed > Normal is a submenu's item, Help > About the toolkit the program's item in the
desktop's Help; SurfOS > Ocean / dusk colors tells the program the theme and back; Ctrl+Shift+O keeps wkit busy while
presses of F12 fill its event queue, and the chooser it asks for comes all the same, the events lost said with its
answer; a paste longer than a field's room (the notes' lines) gives the field the first line. A chooser is known by
its own title, after the name of whom it is for, which desk's fix round changes from the window's to the program's."""
import os
import re

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
CORAL = bytes.fromhex('ee906d')
from gui_apps_win import answered
from gui_apps_media import dialog_open

W, H = 600, 360                     # wkit's content (user/wkit/demo.h)
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
SHEET = bytes.fromhex('fffdf5')
SHELL = bytes.fromhex('dee3d8')
LINE = bytes.fromhex('c9cfc4')
PARK = (900, 480)                   # the pointer clear of the windows and of the Terminal's text
BAR_FILE_X, BAR_VIEW_X = 8 + 88 + 4, 8 + 88 + 56 + 4   # a program's first two menus in the bar, after "\xf7 SurfOS"
CHOOSER = (220, 81)                 # the desktop's chooser's top left at 960x540 (shell/guidialog.c's layout)
EDIT_X, PASTE_Y = 160, 149          # the Edit menu's title, and its Paste item's middle (gui_apps_win.py's)
STATUS_Y = H - 22                   # the status bar, in the content
TABS = {'Controls': 8, 'Lists': 8 + 78 + 4, 'Drawing': 8 + 78 + 4 + 54 + 4, 'More': 8 + 78 + 4 + 54 + 4 + 70 + 4}   # each tab's left in the strip


def chooser_titled(d, own):
    """a predicate: the desktop's chooser is up (at 960x540), its title bar ending in its own title after the name of
    whom it is for ("wkit: Open a file": desk's fix round names the program there, 2c5afd30 the window)"""
    return lambda s: any(d.text(s, CHOOSER[0] + 28 + 8 * k, CHOOSER[1] + 6, ': ' + own, PAPER, TEAL) for k in range(1, 30))


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
        d.visible('kit: its menus File and View stand in the desktop\'s bar, in place of File and Edit (WIN_MENU)',
                  lambda s: d.text(s, BAR_FILE_X, 6, 'File', INK, PAPER) and d.text(s, BAR_VIEW_X, 6, 'View', INK, PAPER))
        d.move(*PARK)
        d.keys('surf')
        d.visible('kit: a name typed shows in its field and in the status bar',
                  lambda s: w.text(s, 110, 43, 'surf', INK) and w.status('Name: surf')(s), 'guiapps-kit-typed.png')
        # keys with their modifiers (desk's keyboard): what the name field does not take is the program's, said in the
        # status bar with its code and modifiers
        for chord, said in (('alt-x', 'Key 0x78 mods 2'), ('ctrl-0', 'Key 0x30 mods 4'),
                            ('ctrl-ret', 'Key 0x10d mods 4'), ('ctrl-esc', 'Key 0x10e mods 4')):
            d.key(chord)
            d.visible(f'kit: {chord} in a field is the program\'s key, with its modifiers ({said})', w.status(said))
        d.keys(' wave')
        d.visible('kit: more typed', w.status('Name: surf wave'))
        d.key('ctrl-backspace')
        d.visible('kit: Ctrl+Backspace deletes the word before the cursor',
                  lambda s: w.status('Name: surf ')(s) and not w.status('Name: surf w')(s))
        d.key('backspace')                              # (a space is seen by what follows it)
        d.keys('x')
        d.visible('kit: Backspace the character before it: the space, so x follows surf', w.status('Name: surfx'))
        d.key('backspace')
        d.visible('kit: and the x', lambda s: w.status('Name: surf')(s) and not w.status('Name: surfx')(s))
        d.key('ctrl-tab')
        d.visible('kit: Ctrl+Tab moves the focus on, to the password field',
                  lambda s: w.pixel(s, 106, 68) == TEAL and w.pixel(s, 106, 40) != TEAL)
        d.key('shift-tab')
        d.visible('kit: Shift+Tab back to the name', lambda s: w.pixel(s, 106, 40) == TEAL)
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
        d.visible('kit: while it is open, the program\'s menus are greyed in the bar: nothing is chosen behind it',
                  lambda s: d.text(s, BAR_VIEW_X, 6, 'View', MUTED, PAPER))
        d.key('ret')
        d.visible('kit: Enter answers it, and it goes', lambda s: w.status('Answered 0')(s) and
                  not w.text(s, bx + 28, by + 6, 'Greet', PAPER, TEAL))
        d.visible('kit: and the menus are as they were', lambda s: d.text(s, BAR_VIEW_X, 6, 'View', INK, PAPER))
        w.click(154 + 38, 159 + 12)                     # Rename
        ay = (H - 160) // 2
        d.visible('kit: Rename opens a line to type, with the name in it',
                  lambda s: w.text(s, bx + 28, ay + 6, 'Rename', PAPER, TEAL) and
                  w.text(s, bx + 16 + 4, ay + 66 + 3, 'surf', INK))
        d.keys('sea\n')
        d.visible('kit: what is typed replaces it, and Enter makes it the name',
                  lambda s: w.status('Renamed: sea')(s) and w.text(s, 110, 43, 'se', INK, SHEET))
        w.click(238 + 32, 159 + 12)                     # Copy
        d.visible('kit: Copy puts the name on the desktop\'s clipboard, and a paste at once gives the window\'s own copy',
                  w.status('Copied the name, pasted back: sea'))
        # the desktop's chooser (220, 81 at 960x540), its own title after the name of whom it is for: by File > Open's
        # shortcut, the toolkit's, and by File > Open in the bar
        chooser = chooser_titled(d, 'Open a file')
        d.key('ctrl-o')
        d.visible('kit: Ctrl+O, a menu\'s shortcut, asks for the desktop\'s chooser, Open a file after whom it is for',
                  chooser, 'guiapps-kit-chooser.png')
        d.key('esc')
        d.visible('kit: Esc cancels it (WIN_EV_DIALOG 0)', w.status('Not chosen'))
        for chord in ('ctrl-g', 'right', 'ret'):
            d.key(chord)
        d.visible('kit: Ctrl+G, Right, Enter: File > Open in the bar asks for it too (WIN_EV_MENU)', chooser)
        d.key('esc')
        d.visible('kit: cancelled', w.status('Not chosen'))
        for chord in ('ctrl-g', 'right', 'right', 'down', 'down', 'down', 'down', 'down', 'right', 'down', 'ret'):
            d.key(chord)
        d.visible('kit: View > Speed > Normal, an item of a submenu in View\'s place', w.status('Speed: Normal'))
        for chord in ('ctrl-g', 'left', 'ret'):
            d.key(chord)
        d.visible('kit: Help > About the toolkit, the program\'s item first in the desktop\'s Help: a message',
                  lambda s: w.text(s, bx + 28, by + 6, 'About the toolkit', PAPER, TEAL), 'guiapps-kit-about.png')
        d.key('esc')
        d.visible('kit: Esc answers it', lambda s: not w.text(s, bx + 28, by + 6, 'About the toolkit', PAPER, TEAL))
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
        state['dusk'] = True
        d.visible('kit: SurfOS > Ocean / dusk colors: the program hears of dusk (WIN_EV_THEME)', w.status('Theme: dusk'))
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
        d.visible('kit: and of ocean again', w.status('Theme: ocean'))
        state['dusk'] = False
        # a chooser asked while the window's queue is full: wkit busy for 3 s (Ctrl+Shift+O) while presses of F12, a
        # key no widget takes, fill its queue (WIN_EVENTS, the rest lost: each press is an event, where the wheel's
        # notches merge in the mouse's reports); the desktop has no room for the answer (EAGAIN), and the toolkit asks
        # again once it has read the events: the chooser comes all the same
        d.key('ctrl-shift-o')
        for _ in range(72):
            d.key('f12')
        d.visible('kit: Ctrl+Shift+O: busy while its queue fills, and still the chooser comes', chooser, timeout=20)
        d.key('esc')
        d.visible('kit: cancelled, and events were lost in the busy spell: its queue was full when it asked',
                  lambda s: w.status('Not chosen; lost while busy: ')(s) and
                  not w.status('Not chosen; lost while busy: 0')(s))
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
        # a paste longer than a field's room (WIN_PASTE says ENOSPC, the toolkit reads it whole): the notes, with a
        # long line, copied, and pasted over the name, which takes the first line
        w.click(10 + 40, 68 + 40)
        d.move(*PARK)
        d.key('ctrl-end')
        d.keys('\n' + 'beach' * 12)
        d.visible('kit: a long line typed in the notes', w.status('Notes: 71 bytes'))
        d.key('ctrl-a')
        d.key('ctrl-c')
        w.tab('Controls')
        d.visible('kit: back to Controls', w.status('Page: Controls'))
        w.click(106 + 60, 40 + 11)                      # the name
        d.move(*PARK)
        d.key('ctrl-a')
        d.key('ctrl-v')
        d.visible('kit: pasted over the name, a field takes the clipboard\'s first line, read whole',
                  lambda s: w.status('Name: tide')(s) and w.text(s, 110, 43, 'tide', INK) and
                  not w.text(s, 110 + 4 * 8, 43, 's', INK))
        w.click(238 + 32, 159 + 12)                     # Copy, for the Terminal at the end
        d.visible('kit: Copy, the name now', w.status('Copied the name, pasted back: tide'))
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
                  term.wait_rows(d, grid, lambda rows: answered(rows, 'echo kit-tide', 'kit-tide')) is not None)   # job
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
    """a desktop dialog left open closed (a chooser, whatever happened), the theme ocean again; wkit ended (its close
    button and Close, or End when it does not answer), then the Terminal closed"""
    for _ in range(3):
        if not dialog_open(d, d.vm.screen()):
            break
        d.key('esc')
        d.vm.pump(0.5)
    if state.get('dusk'):
        for chord in ('ctrl-g', 'down', 'down', 'ret'):
            d.key(chord)
        d.vm.pump(0.5)
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
