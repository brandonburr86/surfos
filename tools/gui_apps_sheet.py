"""The guiapps scenario's checks of the Spreadsheet, wsheet (user/wsheet/; the desktop applications, track sheet:
docs/superpowers/plans/2026-11-05-desktop-apps.md), on the stick's sheet/budget.csv. Files mounts the data stick, and
Enter on budget.csv opens it in the Spreadsheet (the registry's rootfs/apps/wsheet.app: csv), started with its path:
its title names the file, and its grid shows the CSV's text at the left of its cells, its numbers at the right, and the
active cell A1 framed. Ctrl+L goes to C2, where formulas typed one a line (Enter moving down) are worked out at once:
=B2*2, =B3*2, a SUM of them, 1/3, 0.1+0.2, ROUND(2.675, 2), SQRT(2). Ctrl+L C2:C3 selects a range, and the status bar
sums it. Ctrl+Shift+S saves it as total.sheet through the desktop's Save as chooser, which starts in the stick's folder
and adds the extension; a cell typed then and Ctrl+S save it again, replacing the file as saveutil.h does; Save as
values.csv asks first, in the window, since CSV keeps no formulas, and Enter saves the values. Ctrl+W closes the window,
everything saved. Files ejects the stick. On the host, total.sheet holds the formulas, values.csv the values SurfOS
worked out, digit for digit what the host's tests compute (the doubles are SSE2's on both), and budget.csv is as it was.
The checks leave the desktop as they found it (the window closed, the stick ejected, Files closed), also after a
failure."""
import os

from gui_apps_scenario import PAPER, INK, TEAL, MUTED, BAND
from gui_apps_suite import files_front, files_status, files_row, select, EJECT
from gui_apps_desk import files_open

BUDGET = b'Item,Cost\r\nRent,500\r\nFood,250.5\r\n'
STICK = {'sheet/budget.csv': BUDGET}
W, H = 760, 440                     # the window's content (user/wsheet/app.h)
TITLE_H, FRAME_W, FRAME_H = 28, 2, 29
GRID_Y = 30                         # the grid under the formula bar, in the content
HEAD_H, ROW_H, ROWHEAD_W, COL_W, PAD = 20, 20, 48, 80, 3    # user/wsheet/view.h
SHEET = bytes.fromhex('fffdf5')
CHOOSER = (220, 81)                 # a chooser's top left at 960x540 (shell/guidialog.c's layout)
BOX = ((W - 470) // 2, (H - 146) // 2)   # a question over the window (the toolkit's)
PARK = (900, 500)
FORMULAS = ['=B2*2', '=B3*2', '=SUM(C2:C3)', '=1/3', '=0.1+0.2', '=ROUND(2.675,2)', '=SQRT(2)']
VALUES = (b'Item,Cost,,note\r\nRent,500,1000,\r\nFood,250.5,501,\r\n,,1501,\r\n,,0.3333333333333333,\r\n'
          b',,0.30000000000000004,\r\n,,2.68,\r\n,,1.4142135623730951,\r\n')


def places():
    """where shell/guiclient.c opens a desktop's k-th program window, kept on a 960x540 screen (gui_place(): a window
    this wide also kept from its right edge)"""
    for k in range(8):
        yield min(72 + 24 * k, 960 - (W + FRAME_W)), min(52 + 20 * k, 540 - 26 - 2 - (H + FRAME_H))


def titled_at(d, s, at, title, focused=None):
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, title, PAPER, bar) for bar in bars)


def find(d, s, title, focused=None):
    for at in places():
        if titled_at(d, s, at, title, focused):
            return at
    return None


class Sheet:
    """the Spreadsheet's window at at, its title's top left"""
    def __init__(self, d, at):
        self.d, self.at = d, at
        self.x, self.y = at[0] + 1, at[1] + TITLE_H

    def cell(self, row, col):
        """a cell's top left on the screen (the default widths)"""
        return self.x + ROWHEAD_W + COL_W * col, self.y + GRID_Y + HEAD_H + ROW_H * row

    def left(self, row, col, text):
        """a predicate: text at a cell's left, as text sits"""
        x, y = self.cell(row, col)
        return lambda s: self.d.text(s, x + PAD, y + 2, text, INK)

    def right(self, row, col, text):
        """a predicate: text at a cell's right, as numbers sit"""
        x, y = self.cell(row, col)
        return lambda s: self.d.text(s, x + COL_W - PAD - 8 * len(text), y + 2, text, INK)

    def status(self, text):
        return lambda s: self.d.text(s, self.x + 10, self.y + H - 22 + 3, text, INK, BAND)

    def asking(self, title):
        return lambda s: self.d.text(s, self.x + BOX[0] + 28, self.y + BOX[1] + 6, title, PAPER, TEAL)


def chooser(d, title, folder):
    """a predicate: the desktop's chooser is up, its own title in its bar after whatever names the program (the window's
    title, or the registry's), and its folder"""
    return lambda s: d.text(s, CHOOSER[0] + 80, CHOOSER[1] + 38, folder, INK) and \
        any(d.text(s, CHOOSER[0] + 28 + 8 * k, CHOOSER[1] + 6, title, PAPER, TEAL) for k in range(56))


def checks(d):
    state = {'files': False, 'mounted': False, 'window': None}
    done = False
    try:
        d.move(*PARK)
        files_open(d)
        s = d.visible('sheet: Files, with the data stick usb1p1 not mounted',
                      lambda s: files_front(d)(s) and files_row(d, s, 'usb1p1', 1) is not None)
        state['files'] = True
        select(d, files_row(d, s, 'usb1p1', 1))
        d.key('ret')
        s = d.visible('sheet: Enter mounts the stick, with its sheet folder',
                      lambda s: files_status(d, 'usb1p1 on /usb1p1: fat32')(s) and
                      files_row(d, s, 'sheet', 2) is not None)
        state['mounted'] = True
        select(d, files_row(d, s, 'sheet', 2))
        d.key('ret')
        s = d.visible('sheet: the sheet folder opens', lambda s: files_row(d, s, 'budget.csv', 3) is not None)
        select(d, files_row(d, s, 'budget.csv', 3))
        d.key('ret')
        s = d.visible('sheet: Enter opens budget.csv in the Spreadsheet, the program of its type, its title naming it',
                      lambda s: find(d, s, 'budget.csv - Spreadsheet') is not None, timeout=20)
        at = find(d, s, 'budget.csv - Spreadsheet')
        state['window'] = at
        w = Sheet(d, at)
        d.click(at[0] + 300, at[1] + 14)                # its title bar: the focus, whatever the start gave it
        d.move(*PARK)
        d.visible('sheet: the CSV in the grid: text at the left, numbers at the right, A1 framed',
                  lambda s: titled_at(d, s, at, 'budget.csv - Spreadsheet', True) and w.left(0, 0, 'Item')(s) and
                  w.left(1, 0, 'Rent')(s) and w.right(1, 1, '500')(s) and w.right(2, 1, '250.5')(s) and
                  d.pixel(s, *w.cell(0, 0)) == TEAL, 'guiapps-sheet-open.png')
        d.key('ctrl-l')
        d.keys('C2\n')
        d.keys(''.join(f + '\n' for f in FORMULAS))
        d.visible('sheet: formulas typed one a line are worked out at once',
                  lambda s: w.right(1, 2, '1000')(s) and w.right(3, 2, '1501')(s) and w.right(4, 2, '0.3333333')(s) and
                  w.right(5, 2, '0.3')(s) and w.right(6, 2, '2.68')(s) and w.right(7, 2, '1.4142136')(s),
                  'guiapps-sheet-formulas.png')
        d.key('ctrl-l')
        d.keys('C2:C3\n')
        d.visible('sheet: a range selected, and the status bar sums it', w.status('Sum 1501  Average 750.5  Count 2'))
        # Save as a .sheet, through the desktop's chooser in the stick's folder
        d.key('ctrl-shift-s')
        d.visible('sheet: Ctrl+Shift+S asks the desktop\'s Save as chooser, in the file\'s folder',
                  chooser(d, 'Save as', '/usb1p1/sheet'), 'guiapps-sheet-save.png')
        d.keys('total\n')
        d.visible('sheet: saved as total.sheet, the extension the chooser\'s', lambda s: titled_at(
            d, s, at, 'total.sheet - Spreadsheet', True) and w.status('Saved total.sheet')(s))
        d.key('ctrl-l')
        d.keys('D1\nnote\n')
        d.key('ctrl-s')
        d.visible('sheet: a change, and Ctrl+S saves it over the file', w.status('Saved total.sheet'))
        # Save as values.csv: it asks first, as the formulas would be lost
        d.key('ctrl-shift-s')
        d.visible('sheet: Save as again', chooser(d, 'Save as', '/usb1p1/sheet'))
        d.keys('values.csv\n')
        d.visible('sheet: CSV keeps no formulas: it asks first, in the window', w.asking('Save the values only?'),
                  'guiapps-sheet-csv.png')
        d.key('ret')
        d.visible('sheet: Enter saves the values', lambda s: titled_at(d, s, at, 'values.csv - Spreadsheet', True) and
                  w.status('Saved the values in values.csv')(s))
        d.key('ctrl-w')
        d.visible('sheet: Ctrl+W closes it, everything saved', lambda s: find(d, s, 'values.csv - Spreadsheet') is None)
        state['window'] = None
        files_open(d)
        d.visible('sheet: Files in front again', files_front(d))
        d.click(*EJECT)
        d.visible('sheet: Eject in Files flushes the stick', files_status(d, 'usb1: safe to remove'))
        state['mounted'] = False
        d.move(*PARK)
        d.key('ctrl-w')
        d.visible('sheet: Files closes', lambda s: not files_front(d)(s))
        state['files'] = False
        done = True
    finally:
        if not done:
            if d.outdir:
                d.vm.screen().png(os.path.join(d.outdir, 'guiapps-sheet-at-failure.png'))
            try:
                tidy(d, state)
            except AssertionError:
                pass


TITLES = ('budget.csv - Spreadsheet', 'total.sheet - Spreadsheet', 'values.csv - Spreadsheet')


def tidy(d, state):
    """after a failure: the window closed without saving, the stick ejected, Files closed"""
    for _ in range(3):
        s = d.vm.screen()
        at = next((p for p in (find(d, s, t) for t in TITLES) if p is not None), None)
        if at is None:
            break
        d.click(at[0] + W + FRAME_W - 16, at[1] + 14)  # its close button
        d.move(*PARK)
        d.vm.pump(0.5)
        d.key('right')                                  # its question, if it asks: Don't save
        d.key('ret')
        d.vm.pump(1.0)
    if state['files'] or state['mounted']:
        files_open(d)
        d.vm.pump(0.5)
        if state['mounted']:
            d.click(*EJECT)
            d.vm.wait_screen(files_status(d, 'usb1: safe to remove'), 15, interval=0.2)
        d.move(*PARK)
        d.key('ctrl-w')
        d.vm.pump(0.5)


def readback(d, stick, helpers):
    """the stick's files on the host: the formulas kept, the values SurfOS worked out, the CSV opened untouched"""
    try:
        total = helpers['read_fat32'](stick, 'sheet/total.sheet')
        values = helpers['read_fat32'](stick, 'sheet/values.csv')
        budget = helpers['read_fat32'](stick, 'sheet/budget.csv')
    except Exception as e:                              # the checks stopped before they were saved
        d.check(f'sheet: the saved files are on the stick ({e})', False)
        return
    d.check('sheet: total.sheet keeps the formulas, the cell typed after the first save too',
            total.startswith(b'SurfOS sheet 1\n') and b'cell C2 general =B2*2\n' in total and
            b'cell C4 general =SUM(C2:C3)\n' in total and b'cell C8 general =SQRT(2)\n' in total and
            b'cell D1 general note\n' in total and b'cell B3 general 250.5\n' in total)
    d.check('sheet: values.csv holds the values SurfOS worked out, digit for digit the host\'s', values == VALUES)
    if values != VALUES:
        print('values.csv:', values, flush=True)
    d.check('sheet: budget.csv, only opened, is as it was', budget == BUDGET)
