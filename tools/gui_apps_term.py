"""The guiapps scenario's checks of track TERM (docs/superpowers/plans/2026-10-17-gui-suite.md); the Driver
is gui_apps_scenario.py's. 10/17/26: the Terminal. Ctrl+T opens it with surfsh's banner and prompt; a command's
output; the console's escape sequences (the kernel's ansi command: moves, erases, inverse, colours, the corner
without a scroll, and the size it sees), UTF-8 (pagetest utf8) and the pager's question (pagetest 40, q); edit's
full screen with the arrow keys; Ctrl+C stopping sleep; after maximize the size the shell sees (pagetest size);
exit and the ended state, then a new session; closing with a session running asks first (Cancel keeps it, Close
ends it and the program it runs); and a new session's ps shows that nothing of the earlier ones is left. The window's
text is read back from the screenshot, cell by cell, with the console's font; the checks leave the desktop as they
found it (the Terminal closed), also when one fails.
10/18/26 (the review): the pointer is moved off the text after every click on the window (its sprite would read as
'?'); exit ends a session at once although a program it left in the background runs on (the hangup ends that too);
the checks close the Terminal by its own title bar's x, as it opened or maximized, never by keys sent blind."""
import os
import re

from gui_apps_scenario import window, PAPER, INK, TEAL, MUTED, BAND

NAVY, SHELL = bytes.fromhex('102e3b'), bytes.fromhex('dee3d8')      # the Terminal's background and text
BLUE, YELLOW = bytes.fromhex('3c6eb4'), bytes.fromhex('f0d27a')     # its palette's blue and bright yellow
FOAM = bytes.fromhex('b8e9d9')                                       # light cyan: the Terminal's own lines
TITLE_H, STATUS_H = 28, 22
# shell/main.c's ansi command, as tools/qemu-usb.py reads it on the console (row: text)
ANSI_ROWS = {0: 'ANSI-TEST top left', 2: '         INVERSE normal', 3: '         YELLOW-ON-BLUE', 4: '         KEEP',
             6: '         D C   E', 7: '         A', 8: '          B', 10: '         UNKNOWN-IGNORED'}
UTF8_SAMPLE = 'UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ ? END'           # pagetest utf8, as the console draws it
EDIT_HELP = '^S Save  ^Q Quit  ^F Find  ^K Cut line  ^U Paste  ^G Go to line'
PROMPT = re.compile(r'[≋~][!#@?]?>\s*$')
ENDED = '[The session has ended. Enter starts a new one.]'
GLYPHS = None


def glyphs():
    """{glyph rows: character} of the console's font, as qemu-usb.py's load_font() reads it"""
    global GLYPHS
    if GLYPHS is None:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'driver', 'font8x16.c')
        src = open(path, encoding='utf-8').read()
        entries = re.findall(r'\[0x([0-9a-fA-F]{2})\] = \{ ([^}]*) \}, /\* U\+([0-9A-Fa-f]{4,6})', src)
        if len(entries) != 256:
            raise ValueError('could not read the font')
        GLYPHS = {}
        for index, rows, code in sorted(entries, key=lambda e: (not 0x20 <= int(e[0], 16) <= 0x7e, int(e[0], 16))):
            GLYPHS.setdefault(tuple(int(x, 16) for x in rows.split(',')), chr(int(code, 16)))
        GLYPHS[(0,) * 16] = ' '
    return GLYPHS


class Grid:
    """where the Terminal's text is (logical pixels) and its size in cells"""
    def __init__(self, maximized=False):
        if maximized:
            x, y, w, h = 0, 28, 960, 540 - 28 - 26
        else:
            x, y, w, h = window('Terminal')
        self.x, self.y, self.w, self.h = x, y, w, h
        self.tx, self.ty = x + 8, y + TITLE_H + 2
        self.cols = (w - 2 - 12 - 8 - 2) // 8
        self.rows = (h - STATUS_H - 1 - TITLE_H - 2 - 2) // 16

    def cell(self, s, col, row):
        """the character in a cell and its (foreground, background): the most common colour is the background"""
        px, w = s.px, s.w
        x0, y0 = self.tx + col * 8, self.ty + row * 16
        pix = []
        for gy in range(16):
            at = ((2 * (y0 + gy)) * w + 2 * x0) * 3
            pix.append([px[at + 6 * gx:at + 6 * gx + 3] for gx in range(8)])
        counts = {}
        for line in pix:
            for p in line:
                counts[p] = counts.get(p, 0) + 1
        order = sorted(counts, key=lambda p: -counts[p])
        bg = order[0]
        bits = tuple(sum(0x80 >> gx for gx in range(8) if pix[gy][gx] != bg) for gy in range(16))
        return glyphs().get(bits, '?'), (order[1] if len(order) > 1 else None, bg)

    def text(self, s):
        """the rows as text, blanks at their ends left out"""
        return [''.join(self.cell(s, c, r)[0] for c in range(self.cols)).rstrip() for r in range(self.rows)]

    def status(self, d, s, text):
        """the status bar's text, from its start"""
        return d.text(s, self.x + 10, self.y + self.h - STATUS_H - 1 + 3, text, INK, BAND)


def last_row(rows):
    for r in reversed(rows):
        if r:
            return r
    return ''


def prompted(rows):
    """the prompt on the last row that has anything"""
    return bool(PROMPT.search(last_row(rows)))


def ended_after(rows, marker):
    """below the last row that matches marker: exit typed, and the line that says the session has ended"""
    for i in range(len(rows) - 1, -1, -1):
        if re.fullmatch(marker, rows[i]):
            rest = rows[i + 1:]
            return any(r.endswith('> exit') and k + 1 < len(rest) and rest[k + 1] == ENDED for k, r in enumerate(rest))
    return False


def then_prompt(rows, text):
    """the last row that holds text, and a prompt on the row after it (what is below may be older)"""
    for i in range(len(rows) - 2, -1, -1):
        if text in rows[i]:
            return bool(PROMPT.search(rows[i + 1]))
    return False


def wait_rows(d, grid, test, timeout=20):
    """the screen once the Terminal's rows pass test, or None"""
    return d.vm.wait_screen(lambda s: test(grid.text(s)), timeout, interval=0.2)


def running(d, grid, command, timeout=20):
    """the screen once a command typed at the prompt runs: below its line, the cursor waits at a row's start"""
    def test(s):
        rows = grid.text(s)
        for i in range(len(rows) - 2, -1, -1):
            if rows[i].endswith('> ' + command):
                return not rows[i + 1] and grid.cell(s, 0, i + 1)[1][1] == SHELL
        return False
    return d.vm.wait_screen(test, timeout, interval=0.2)


def run(d, grid, name, command, test, save=None, timeout=20):
    """type a command, and require what it shows"""
    d.keys(command + '\n')
    scr = wait_rows(d, grid, test, timeout)
    d.require(name, scr is not None)
    if save and d.outdir:
        scr.png(os.path.join(d.outdir, save))
    return scr


def dialog_up(d, s, title):
    """a question of the window manager in front (shell/guidialog.c's box at 960x540)"""
    return d.text(s, (960 - 470) // 2 + 28, 28 + (540 - 28 - 26 - 146) // 2 + 6, title, PAPER, TEAL)


def shown(d, s):
    """the Terminal's title bar, where it opens or maximized, in front or not"""
    x, y, _, _ = window('Terminal')
    return any(d.text(s, tx, ty, 'Terminal', PAPER, bar) for tx, ty in ((x + 30, y + 6), (30, 28 + 6))
               for bar in (TEAL, MUTED))


def task_name(row):
    """the name in a row of ps (kernel/task.c: print_tasks()), without its "(ring 3)" """
    return row[:-len(' (ring 3)')].split()[-1] if row.endswith(' (ring 3)') else row.split()[-1]


def park(d):
    """the pointer off the Terminal's text, as it opened or maximized: on the maximized window's scroll bar (its
    text ends at x 944), with all of its 12 pixels on the screen (x 946 to 957), as the harness wants to see it"""
    d.move(946, 300)


def close_terminal(d):
    """the Terminal's own title bar x, where its title bar is (as it opened, or maximized), and Close in the question
    when a session runs; a first click may only close a menu that was open, so a second try follows"""
    x, y, w, _ = window('Terminal')
    for _ in range(3):
        s = d.vm.screen()
        for tx, ty, bx, by in ((x + 30, y + 6, x + w - 16, y + 14), (30, 28 + 6, 960 - 16, 28 + 14)):
            if any(d.text(s, tx, ty, 'Terminal', PAPER, bar) for bar in (TEAL, MUTED)):
                d.click(bx, by)
                if d.vm.wait_screen(lambda s: dialog_up(d, s, 'Close the window?'), 3, interval=0.2) is not None:
                    d.key('ret')
                break
        else:
            return
        if d.vm.wait_screen(lambda s: not shown(d, s), 5, interval=0.2) is not None:
            return


def checks(d):
    grid = Grid()
    try:
        d.move(900, 480)                            # the pointer off the Terminal's text, which is read back
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal', d.title('Terminal'))
        scr = wait_rows(d, grid, lambda rows: "Brandon Burr's SurfOS" in rows and
                        then_prompt(rows, 'Version 1.0 on an x86, 2004-2026'), 30)
        d.require("the Terminal shows surfsh's banner and its prompt", scr is not None)
        if d.outdir:
            scr.png(os.path.join(d.outdir, 'guiapps-terminal.png'))
        d.check('the Terminal says its size in the status bar', d.text(scr, grid.x + grid.w - 24 - 5 * 8,
                                                                       grid.y + grid.h - STATUS_H - 1 + 3, '77x21',
                                                                       MUTED, BAND))
        scr = run(d, grid, "a typed command's output shows in the Terminal", 'echo hello-from-the-terminal',
                  lambda rows: 'hello-from-the-terminal' in rows and then_prompt(rows, 'hello-from-the-terminal'))
        # copy and paste: a double-click takes the word, Edit > Copy, then Edit > Paste into a new line
        row = len(grid.text(scr)) - 1 - grid.text(scr)[::-1].index('hello-from-the-terminal')
        x, y = grid.tx + 3 * 8 + 4, grid.ty + row * 16 + 8
        d.click(x, y)
        d.click(x, y)
        d.move(900, 480)
        d.key('ctrl-g')
        d.key('right')
        d.key('right')
        d.key('ret')                                # Copy: the first item that is on
        d.keys('echo ')
        d.key('ctrl-g')
        d.key('right')
        d.key('right')
        d.key('down')
        d.key('ret')                                # Paste
        scr = wait_rows(d, grid, lambda rows: sum(r.endswith('> echo hello-from-the-terminal') for r in rows) == 2)
        d.require('a word selected with the mouse, copied and pasted through the Edit menu', scr is not None)
        d.keys('\n')
        d.require('and run', wait_rows(d, grid, lambda rows: rows.count('hello-from-the-terminal') == 2 and
                                      then_prompt(rows, 'hello-from-the-terminal')) is not None)
        scr = run(d, grid, "the console's escape sequences: moves, erases, the corner without a scroll, the size",
                  'ansi', lambda rows: all(rows[r] == text for r, text in ANSI_ROWS.items()) and
                  rows[12] == 'ANSI-TEST done, 77x21' and rows[20].endswith('Z') and len(rows[20]) == 77,
                  'guiapps-terminal-ansi.png')
        d.check('inverse video and bright yellow on blue, in the Terminal\'s palette',
                grid.cell(scr, 9, 2) == ('I', (NAVY, SHELL)) and grid.cell(scr, 9, 3) == ('Y', (YELLOW, BLUE)))
        run(d, grid, 'UTF-8 as the console draws it', 'pagetest utf8',
            lambda rows: UTF8_SAMPLE in rows and then_prompt(rows, UTF8_SAMPLE))
        scr = run(d, grid, "the pager's question on the last row", 'pagetest 40',
                  lambda rows: rows[-1].startswith('--More--') and 'pagetest line 020' in rows,
                  'guiapps-terminal-pager.png')
        d.check("the pager's question in inverse video", grid.cell(scr, 0, grid.rows - 1)[1] == (NAVY, SHELL))
        d.keys('q')
        scr = wait_rows(d, grid, lambda rows: prompted(rows) and not any(r.startswith('--More--') for r in rows))
        d.require('q at the question drops the rest, and the prompt comes back', scr is not None)
        # edit: its full screen, raw mode's keys (the arrows as escape sequences), Ctrl+Q its own
        run(d, grid, "edit draws its full screen in the Terminal", 'edit term.txt',
            lambda rows: rows[0].startswith(' SurfOS edit  term.txt') and rows[-1] == EDIT_HELP,
            'guiapps-terminal-edit.png')
        d.keys('abc<left><left>X')
        scr = wait_rows(d, grid, lambda rows: rows[1] == 'aXbc' and rows[0].endswith('Modified'))
        d.require("typing and the arrow keys reach edit", scr is not None)
        d.check("edit's title bar in inverse video", grid.cell(scr, 1, 0)[1] == (NAVY, SHELL))
        d.key('ctrl-q')
        d.require("Ctrl+Q is edit's: it asks", wait_rows(d, grid, lambda rows: any('Save changes?' in r for r in rows))
                  is not None)
        d.keys('n')
        d.require('edit ends and the prompt comes back', wait_rows(d, grid, prompted) is not None)
        # Ctrl+C reaches the program
        d.keys('sleep 30000\n')
        d.require('sleep 30000 runs', running(d, grid, 'sleep 30000') is not None)
        d.vm.pump(1.0)
        d.key('ctrl-c')
        scr = wait_rows(d, grid, lambda rows: then_prompt(rows, 'sleep cut short after'), 10)
        d.require('Ctrl+C stops sleep 30000 in the Terminal', scr is not None)
        # maximized, the shell sees the new size
        d.click(grid.x + grid.w - 40, grid.y + 10)
        park(d)
        big = Grid(maximized=True)
        d.require('the Terminal maximizes', wait_rows(d, big, prompted, 10) is not None)
        run(d, big, 'after maximize the shell sees the new size', 'pagetest size',
            lambda rows: then_prompt(rows, 'TTY_GETSIZE 117x26'), 'guiapps-terminal-maximized.png')
        d.click(big.x + big.w - 40, big.y + 10)
        park(d)
        run(d, grid, 'and the size it had again', 'pagetest size',
            lambda rows: then_prompt(rows, 'TTY_GETSIZE 77x21'))
        # exit: the window says so; Enter starts a new session
        d.keys('exit\n')
        scr = wait_rows(d, grid, lambda rows: ENDED in rows)
        d.require('exit: the Terminal says the session has ended', scr is not None)
        if d.outdir:
            scr.png(os.path.join(d.outdir, 'guiapps-terminal-ended.png'))
        d.check('the ended line in light cyan', grid.cell(scr, 1, grid.text(scr).index(ENDED))[1] == (FOAM, NAVY))
        d.check('and the status bar says so', grid.status(d, scr, 'The session has ended'))
        d.key('ret')
        d.require('Enter starts a new session', wait_rows(d, grid, lambda rows: prompted(rows) and
                                                           ENDED in rows) is not None)
        # a program the shell left running in the background does not keep the session: exit ends it at once,
        # and the hangup ends the program (ps below finds no pagetest)
        run(d, grid, 'a program runs in the background', 'pagetest hold 300000 &',
            lambda rows: any(re.fullmatch(r'\[\d+\] started \(pid \d+\)', r) for r in rows) and prompted(rows))
        d.keys('exit\n')
        d.require('exit ends the session at once, though that program would run on',
                  wait_rows(d, grid, lambda rows: ended_after(rows, r'\[\d+\] started \(pid \d+\)'), 15) is not None)
        d.key('ret')
        d.require('and Enter starts another', wait_rows(d, grid, lambda rows: prompted(rows) and
                                                         ended_after(rows, r'\[\d+\] started \(pid \d+\)')) is not None)
        # closing while a session runs asks first: Cancel keeps it
        d.close('Terminal')
        d.require('closing with a session running asks first',
                  d.vm.wait_screen(lambda s: dialog_up(d, s, 'Close the window?'), 10, interval=0.2) is not None)
        d.key('esc')
        d.require('Cancel keeps the window', d.vm.wait_screen(lambda s: not dialog_up(d, s, 'Close the window?') and
                                                              d.title('Terminal')(s), 10, interval=0.2) is not None)
        run(d, grid, 'and the session goes on', 'echo still-here', lambda rows: then_prompt(rows, 'still-here'))
        # Close ends it, with the program it runs (pagetest, waiting for an answer from the terminal)
        d.keys('pagetest ask 1\n')
        d.require('pagetest asks, waiting for the terminal', wait_rows(d, grid, lambda rows: last_row(rows) ==
                                                                         'pagetest question:') is not None)
        d.close('Terminal')
        d.require('the question again', d.vm.wait_screen(lambda s: dialog_up(d, s, 'Close the window?'), 10,
                                                         interval=0.2) is not None)
        d.key('ret')
        d.require('Close closes the window', d.vm.wait_screen(lambda s: not d.title('Terminal')(s) and
                                                              not dialog_up(d, s, 'Close the window?'), 10,
                                                              interval=0.2) is not None)
        # a new session: nothing of the earlier ones is left (no zombie, no pagetest, one surfsh more than F1-F4's)
        d.key('ctrl-t')
        d.visible('the Terminal opens again', d.title('Terminal'))
        d.require('with a new session', wait_rows(d, grid, prompted, 30) is not None)
        d.click(grid.x + grid.w - 40, grid.y + 10)
        park(d)
        seen = []

        def listed(rows):
            if any(r.startswith('--More--') for r in rows):
                return True
            return prompted(rows) and any(r.endswith(' tasks') for r in rows)
        d.keys('ps\n')
        for _ in range(8):                          # a screenful at a time, as the pager asks
            scr = wait_rows(d, big, lambda rows: rows != seen[-big.rows:] and listed(rows), 20)
            d.require('ps lists the tasks', scr is not None)
            rows = big.text(scr)
            seen += rows
            if d.outdir and not rows[-1].startswith('--More--'):
                scr.png(os.path.join(d.outdir, 'guiapps-terminal-ps.png'))
            if not rows[-1].startswith('--More--'):
                break
            d.keys(' ')
        tasks = {}
        for r in seen:
            if re.match(r'\s*\d+\s+(ready|running|blocked|sleeping|zombie)\s', r):
                tasks[r.split()[0]] = r                 # by pid: a row seen on two screens counts once
        d.check('ps in a new session: no zombie is left', tasks and
                not any(r.split()[1] == 'zombie' for r in tasks.values()))
        d.check("ps in a new session: the closed session's pagetest is gone", tasks and
                not any(task_name(r) == 'pagetest' for r in tasks.values()))
        shells = [r for r in tasks.values() if task_name(r) == 'surfsh']
        d.check(f"ps in a new session: five surfsh, F1-F4's and the Terminal's ({len(shells)})", len(shells) == 5)
        d.click(big.x + big.w - 40, big.y + 10)
        park(d)
    except AssertionError as e:
        d.vm.screen('guiapps-terminal-failed.png')     # as it was, before the window is closed below
        if not d.results or d.results[-1][1]:          # a stop that recorded nothing (the pointer did not
            d.check(f"the Terminal's checks went on: {e}", False)   # move, say) is a failure all the same
        raise
    finally:
        try:
            scr = d.vm.screen()
            if dialog_up(d, scr, 'Close the window?') or dialog_up(d, scr, 'Leave the desktop?'):
                d.key('esc')
            close_terminal(d)
            d.check('the Terminal is closed again', d.vm.wait_screen(lambda s: not shown(d, s), 10, interval=0.2)
                    is not None)
        except AssertionError:
            pass
