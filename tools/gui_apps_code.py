"""The guiapps scenario's checks of the Code Editor (wcode, user/wcode/; the desktop applications, track code:
docs/superpowers/plans/2026-11-05-desktop-apps.md), on a C file of the data stick; the Driver is gui_apps_scenario.py's.

The Terminal mounts the stick and starts `wcode /usb1p1/code/hello.c` in the foreground, so its window comes up with
the focus: titled after the file, its tab lit, the line numbers in the gutter (the caret's line in ink), the C in colour
(main in ink, int a type, return a keyword, 0 a number), and its menus in the desktop's bar in place of File and Edit.
Ctrl+End, Enter and a comment typed show in the comment's colour, and the title and the tab say the file has changed;
Ctrl+S saves it (the status bar says so, the star goes). Ctrl+F opens the find bar: return typed and Enter select the
match, Esc closes the bar. Ctrl+Shift+F opens find in files with what was found last, in the file's folder: Enter finds
return in hello.c and says so; Esc closes it. Ctrl+N gives a new tab, something is typed in it, and Ctrl+Q asks about
it (the window's own question: Save, Don't save, Cancel): Don't save ends the program, and the Terminal's prompt comes
back; ps finds no wcode. Then Enter in Files on the stick's notes.md, a type rootfs/apps/wcode.app names, starts a new
wcode with it, whose close button ends it at once (nothing changed). After the guest stops, the host reads hello.c back
as it was saved, with no hello.c.tmp beside it. The checks leave the desktop as they found it (wcode ended, Files
closed, the stick unmounted, the Terminal closed), also when one fails."""
import os

import gui_apps_term as term
from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
from gui_apps_win import answered
from gui_apps_suite import files_front, files_row, select

HELLO = b'int main(void)\n{\n    return 0;\n}\n'
STICK = {'code/hello.c': HELLO, 'code/notes.md': b'# Notes\n\nA *needle* here.\n'}
SAVED = HELLO + b'\n// edited'

W, H = 800, 440                     # wcode's content as it opens (user/wcode/window.h): the desktop gives it at 960x540
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
TABS_H, GUTTER, CW, CH = 30, 40, 8, 16  # the tab strip; the line numbers' 5 cells; a cell
STATUS_Y = H - 22                   # the status bar, in the content
PARK = (900, 480)                   # the pointer clear of what is checked (the text's right end at most)
TYPE = bytes.fromhex('1e5c96')      # the colours of user/wcode/window.c's class_color()
KEYWORD = bytes.fromhex('6b3a8e')
NUMBER = bytes.fromhex('a44a22')
PICK = bytes.fromhex('bfe3dd')      # the selection (the theme's)
# where the toolkit lays the bands out at 800x440 (gk.h's sizes; measured on the fake desktop, tests/host/gk_fake.c):
FIND_Y = STATUS_Y - 32 + 8          # the find bar's label, over the status bar
IN_Y, IN_X, FOUND_Y = 252, 78, 395  # find in files' In: (its field's text) and what it says of its search
# the bar's titles and their x, after "\xf7 SurfOS" (shell/guicore.c: from 8, a title's cells * 8 + 24, its text at +4)
MENUS = (('File', 100), ('Edit', 156), ('Search', 212), ('View', 284), ('Apps', 340))


def places():
    """where shell/guiclient.c opens a desktop's k-th program window, kept on a 960x540 screen (gui_place())"""
    seen = []
    for k in range(8):
        at = (min(72 + 24 * k, 960 - (W + FRAME_W)), min(52 + 20 * k, 540 - 26 - 2 - (H + FRAME_H)))
        if at not in seen:
            seen.append(at)
            yield at


def titled_at(d, s, at, title, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, title, PAPER, bar) for bar in bars)


def find(d, s, title, focused=None):
    for at in places():
        if titled_at(d, s, at, title, focused):
            return at
    return None


class Win:
    """wcode's window at at: its content's top left, its text's cells"""
    def __init__(self, d, at):
        self.d, self.at = d, at
        self.x, self.y = at[0] + 1, at[1] + TITLE_H

    def text(self, s, x, y, text, fg, bg=None):
        return self.d.text(s, self.x + x, self.y + y, text, fg, bg)

    def row(self, s, r, c, text, fg, bg=None):
        """text on the text's row r from its column c"""
        return self.text(s, GUTTER + c * CW, TABS_H + r * CH, text, fg, bg)

    def status(self, text):
        return lambda s: self.text(s, 10, STATUS_Y + 3, text, INK, BAND)



def checks(d):
    grid = term.Grid()
    state = {'mounted': False, 'files': False}
    done = False
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('code: Ctrl+T opens the Terminal, for the Code Editor', d.title('Terminal'))
        d.require('code: the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        d.keys('mount usb1p1\n')
        d.require('code: the data stick is mounted in the Terminal',
                  term.wait_rows(d, grid, lambda rows: any('usb1p1 on /usb1p1' in r for r in rows), 30) is not None)
        state['mounted'] = True
        d.keys('wcode /usb1p1/code/hello.c\n')
        s = d.visible('code: wcode opens its window with the focus (a foreground job), titled after the file',
                      lambda s: find(d, s, 'Code Editor - hello.c', True) is not None, timeout=40)
        w = Win(d, find(d, s, 'Code Editor - hello.c'))
        s = d.visible('code: its tab lit, the line numbers (the caret\'s in ink), and the C in colour: main in ink, '
                      'return a keyword, 0 a number',
                      lambda s: w.text(s, 8 + 7, 7, 'hello.c', PAPER, TEAL) and w.text(s, 3 * CW, TABS_H, '1', INK) and
                      w.text(s, 3 * CW, TABS_H + CH, '2', MUTED) and w.row(s, 0, 4, 'main', INK) and
                      w.row(s, 2, 4, 'return', KEYWORD) and w.row(s, 2, 11, '0', NUMBER), 'guiapps-code-open.png')
        d.check('code: int, a type, in its colour (from its n: the caret is at its i)', w.row(s, 0, 1, 'nt', TYPE))
        d.check('code: its menus File, Edit, Search and View in the desktop\'s bar, then Apps',
                all(d.text(s, x, 6, name, INK, PAPER) for name, x in MENUS))
        # typing: a comment after the last line, then Ctrl+S
        d.key('ctrl-end')
        d.key('ret')
        d.keys('// edited')
        d.visible('code: Ctrl+End, Enter and a comment typed: in the comment\'s colour, and the file marked changed',
                  lambda s: w.row(s, 5, 0, '// edite', MUTED) and titled_at(d, s, w.at, 'Code Editor - hello.c *') and
                  w.text(s, 8 + 7, 7, 'hello.c *', PAPER, TEAL), 'guiapps-code-typed.png')
        d.key('ctrl-s')
        d.visible('code: Ctrl+S saves it: the status bar says so, and the star goes',
                  lambda s: w.status('Saved hello.c')(s) and not titled_at(d, s, w.at, 'Code Editor - hello.c *') and
                  titled_at(d, s, w.at, 'Code Editor - hello.c'))
        # find
        d.key('ctrl-f')
        d.visible('code: Ctrl+F opens the find bar under the text, with its box',
                  lambda s: w.text(s, 4, FIND_Y, 'Find:', INK))
        d.keys('return\n')
        d.visible('code: return typed and Enter: the match selected', lambda s: w.row(s, 2, 4, 'return', KEYWORD, PICK),
                  'guiapps-code-find.png')
        d.key('esc')
        d.visible('code: Esc closes the find bar', lambda s: not w.text(s, 4, FIND_Y, 'Find:', INK))
        # find in files, in the file's folder
        d.key('ctrl-shift-f')
        d.visible('code: Ctrl+Shift+F opens find in files, with what was found last, in the file\'s folder',
                  lambda s: w.text(s, 4, IN_Y, 'In:', INK) and w.text(s, IN_X, IN_Y, '/usb1p1/code', INK))
        d.key('ret')
        d.visible('code: Enter searches the folder: return in hello.c, one line of the two files',
                  lambda s: w.text(s, 4, FOUND_Y, '1 line found in 2 files read', INK), 'guiapps-code-files.png',
                  timeout=30)
        d.key('esc')
        d.visible('code: Esc closes it', lambda s: not w.text(s, 4, IN_Y, 'In:', INK))
        # a new tab with changes: Ctrl+Q asks, Don't save ends the program
        d.key('ctrl-n')
        d.keys('scratch')
        d.visible('code: Ctrl+N, a new tab: Untitled 2, with what was typed',
                  lambda s: titled_at(d, s, w.at, 'Code Editor - Untitled 2 *') and w.row(s, 0, 1, 'cratch', INK))
        d.key('ctrl-q')
        qx, qy = (W - 470) // 2, (H - 146) // 2
        d.visible('code: Ctrl+Q asks about the file with changes: Save, Don\'t save, Cancel',
                  lambda s: w.text(s, qx + 28, qy + 6, 'Save the changes to Untitled 2?', PAPER, TEAL),
                  'guiapps-code-guard.png')
        d.key('right')
        d.key('ret')
        d.visible('code: Don\'t save: the program ends, and its window goes',
                  lambda s: find(d, s, 'Code Editor') is None, timeout=20)
        d.require('code: the Terminal\'s prompt comes back', term.wait_rows(d, grid, term.prompted, 20) is not None)
        d.keys('ps | grep -c wcode\n')
        d.require('code: ps finds no wcode', term.wait_rows(
            d, grid, lambda rows: answered(rows, 'ps | grep -c wcode', '0')) is not None)
        # Files opens a Markdown file in a new wcode: the type wcode.app names
        files(d, state)
        done = True
    finally:
        if not done and d.outdir:                      # the screen as the failure left it, before the tidying
            d.vm.screen().png(os.path.join(d.outdir, 'guiapps-code-at-failure.png'))
        try:
            tidy(d, grid, state)
        except AssertionError:                          # what failed first is what the results say
            pass


def files(d, state):
    """Files, the stick's code folder, notes.md opened by Enter in wcode, which its close button ends"""
    for chord in ('ctrl-g', 'right', 'right', 'right', 'down', 'ret'):  # Apps > Files (the Terminal has Ctrl+B)
        d.key(chord)
    s = d.visible('code: Files, with the stick the Terminal mounted', lambda s: files_front(d)(s) and
                  files_row(d, s, 'usb1p1', 1) is not None)
    state['files'] = True
    select(d, files_row(d, s, 'usb1p1', 1))
    d.key('ret')
    s = d.visible('code: the stick opens: its code folder', lambda s: files_row(d, s, 'code', 2) is not None)
    select(d, files_row(d, s, 'code', 2))
    d.key('ret')
    s = d.visible('code: the code folder opens: notes.md', lambda s: files_row(d, s, 'notes.md', 3) is not None)
    select(d, files_row(d, s, 'notes.md', 3))
    d.key('ret')
    s = d.visible('code: Enter opens notes.md in wcode, the program its type belongs to (rootfs/apps/wcode.app)',
                  lambda s: find(d, s, 'Code Editor - notes.md') is not None, 'guiapps-code-files-open.png', timeout=40)
    w = Win(d, find(d, s, 'Code Editor - notes.md'))
    d.check('code: the Markdown heading in its colour (navy)',
            d.vm.wait_screen(lambda s: w.row(s, 0, 1, ' Notes', bytes.fromhex('102e3b')), 10, interval=0.2) is not None)
    d.click(w.at[0] + W + FRAME_W - 16, w.at[1] + 14)  # its close button: nothing changed, it ends at once
    d.move(*PARK)
    d.visible('code: its close button ends it, with nothing to ask, and Files is in front again',
              lambda s: find(d, s, 'Code Editor') is None and files_front(d)(s), timeout=20)
    d.key('ctrl-w')                                     # Files closes
    d.visible('code: Files closes', lambda s: not files_front(d)(s))
    state['files'] = False


def tidy(d, grid, state):
    """wcode ended: its close button, then its own Quit (Ctrl+Q), each answered Don't save when it asks; then the close
    button again, which on today's desktop asks End the program? (End) and after desk's fix round sends the close
    again (Don't save). Then Files closed, the stick unmounted, the Terminal closed."""
    for attempt in range(3):
        at = find(d, d.vm.screen(), 'Code Editor')
        if at is None:
            break
        if attempt == 1:
            d.click(at[0] + 250, at[1] + 14)            # its title bar: the focus
            d.key('ctrl-q')                             # File > Quit, the program's own
        else:
            d.click(at[0] + W + FRAME_W - 16, at[1] + 14)
        d.move(*PARK)
        if d.vm.wait_screen(lambda s: find(d, s, 'Code Editor') is None, 3, interval=0.2) is not None:
            continue
        d.key('right')                                  # Don't save in its question, End in the desktop's
        d.key('ret')
        d.vm.wait_screen(lambda s: find(d, s, 'Code Editor') is None, 5, interval=0.2)
    if state.get('files'):
        d.key('ctrl-w')
        d.vm.pump(0.5)
    if state.get('mounted'):
        d.keys('umount /usb1p1\n')
        term.wait_rows(d, grid, term.prompted, 20)
    term.close_terminal(d)


def readback(d, stick, helpers):
    """hello.c on the host as wcode saved it, with no new file left beside it"""
    try:
        data = helpers['read_fat32'](stick, 'code/hello.c')
    except Exception as e:
        d.check(f'code: hello.c is on the stick ({e})', False)
        return
    d.check('code: hello.c reads back on the host as saved: the comment after its last line', data == SAVED)
    try:
        helpers['read_fat32'](stick, 'code/hello.c.tmp')
        left = True
    except Exception:
        left = False
    d.check('code: no hello.c.tmp left beside it (the save renamed it over the file)', not left)
