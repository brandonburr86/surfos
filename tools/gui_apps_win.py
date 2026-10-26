"""The guiapps scenario's checks of programs' windows (milestone M12, docs/superpowers/plans/2026-10-25-app-windows.md);
the Driver is gui_apps_scenario.py's. 10/25/26 (stage C4): wintest show, typed in the Terminal, opens a window in
front with its title and its four quadrants at 2x; a key typed reaches the program (its title says which); its close
button sends CLOSE and the program ends, and its window goes. (stage C5) wdemo, through libgui: its colour bars and
its size as text at 2x, a key shown as its last event, and its close button. (stage C6) Apps > Run program...
lists /initrd/bin's programs and offers wcalc, which opens the Calculator as a program, drawn as the desktop draws its
own, with the focus (the launcher started it), and counts by keys and by clicks; Esc ends it. 10/25/26 (the review):
a window comes in front without the focus (its title bar muted), and a key typed meanwhile stays the Terminal's; a
click on its title bar gives it the focus.
(stage C7, the platform's whole check) wintest tty names the Terminal's pty; wintest pty in the Terminal is refused on
a pty of its own while this desktop runs. wdemo echoes a press and a release of the button with where they were; F2
shows a text console and F1 brings the desktop back with wdemo's window as it was; its corner dragged gives it a new
size, which it answers (its bars and its size drawn again); its close button ends it, and ps in the Terminal finds no
wdemo. wdemo typed without & is the Terminal's foreground job, a job its shell made: its window takes the focus as it
appears, and a key reaches it at once. wdemo hang pays no heed to the close button: pressed again, it asks "End the
program?", and End kills it (surfsh: killed). wdemo crash faults after it first draws: only its window goes (surfsh:
killed), and the Terminal and the desktop go on. Last, wcalc started from Run program... outlives the Terminal (closed
with its session) and still counts; wintest pty, started by the desktop once the Terminal has closed, takes the
Terminal's pty number for a pty of its own and is refused there (the review: a pty's adoption ends with it; its lines
come on the serial line); leaving the desktop asks about its window, and once the desktop has closed, ps on the serial
console finds no wcalc: it ended with the desktop. The desktop is started again for the checks that follow.
The checks leave the desktop as they found it (the windows, the dialogs and the Terminal closed, the desktop running),
also when one fails."""
import os
import re

import gui_apps_term as term
from gui_apps_scenario import APPS, PAPER, INK, FOAM, TEAL, MUTED

CORAL, TEALQ, SKY, SAND = (bytes.fromhex('ee906d'), bytes.fromhex('197d83'), bytes.fromhex('659ecb'),
                           bytes.fromhex('d0b056'))         # user/bin/wintest.c's quadrants
TITLE_H = 28
NAVY, PAPER_TEXT = bytes.fromhex('102e3b'), bytes.fromhex('f4f1e6')
BARS = (bytes.fromhex('ee906d'), bytes.fromhex('d0b056'), bytes.fromhex('7ed48c'), bytes.fromhex('197d83'),
        bytes.fromhex('659ecb'), bytes.fromhex('9b5fa7'), bytes.fromhex('f4f1e6'), bytes.fromhex('153d49'))
CALC_KEYS = '789/456*123-C0=+'
RUN_DIALOG = (220, 81, 520, 380)    # the launcher's chooser at 960x540 (shell/guidialog.c's layout)
PARK = (900, 480)                   # the pointer clear of the windows and of the Terminal's text
ABOUT = ((960 - 432) // 2, (540 - 234) // 2)    # the About window's place at 960x540 (gui_init)
ANSI = re.compile(r'\x1b\[[0-9;?]*[ -/]*[@-~]')


def cascade(k):
    """where shell/guiclient.c opens a desktop's k-th program window (from 0): a cascade, 24 by 20 apart, 8 places"""
    return 72 + 24 * (k % 8), 52 + 20 * (k % 8)


FIRST, SECOND, CALC, FG, HANG = cascade(0), cascade(1), cascade(2), cascade(3), cascade(4)


def titled_at(d, s, at, text, focused=None):
    """the title bar of the window at at shows text (its start), in front or behind (focused None: either)"""
    bars = (TEAL, MUTED) if focused is None else (TEAL,) if focused else (MUTED,)
    return any(d.text(s, at[0] + 30, at[1] + 6, text, PAPER, bar) for bar in bars)


def find(d, s, text):
    """the place of the program's window titled text, at any of the cascade's places, or None"""
    for k in range(8):
        if titled_at(d, s, cascade(k), text):
            return cascade(k)
    return None


def titled(d, s, text, focused=True):
    """the first program's window's title bar shows text (its start)"""
    return titled_at(d, s, FIRST, text, focused)


def quadrants(d, s, w=320, h=200):
    """wintest's four quadrants in the first program's window, w x h"""
    x, y = FIRST[0] + 1, FIRST[1] + TITLE_H
    return (d.pixel(s, x + 10, y + 10) == CORAL and d.pixel(s, x + w - 10, y + 10) == TEALQ and
            d.pixel(s, x + 10, y + h - 10) == SKY and d.pixel(s, x + w - 10, y + h - 10) == SAND)


def wdemo_at(d, s, at, w=360, h=240, last=None):
    """wdemo's window at at, its content w x h: its bars over two thirds, its size below, and its last event if given"""
    x, y, bars = at[0] + 1, at[1] + TITLE_H, h * 2 // 3
    ok = all(d.pixel(s, x + (2 * i + 1) * w // 16, y + bars // 2) == BARS[i] for i in range(8)) and \
        d.text(s, x + 8, y + bars + 8, f'wdemo {w}x{h}', FOAM, NAVY)
    return ok and (last is None or d.text(s, x + 8, y + bars + 28, last, PAPER_TEXT, NAVY))


def wdemo_shown(d, s, last=None):
    """wdemo's window, the second program's of the desktop, at its first size"""
    return wdemo_at(d, s, SECOND, last=last)


def calc_shows(d, s, value, at=CALC):
    """wcalc's display shows value, where the desktop's own Calculator shows it in a window at the same place"""
    return d.text(s, at[0] + 28, at[1] + 58, str(value) + '  ', FOAM, INK)


def calc_titled(d, s, at=CALC):
    return d.text(s, at[0] + 30, at[1] + 6, 'Calculator', PAPER, TEAL)


def calc_click(d, key, at=CALC):
    """a click on wcalc's button for key (its grid: shell/guicore.c's Calculator's)"""
    i = CALC_KEYS.index(key)
    d.click(at[0] + 1 + 15 + i % 4 * 54 + 24, at[1] + TITLE_H + 76 + i // 4 * 40 + 17)


def run_titled(d, s):
    """the launcher's chooser is open: its title"""
    return d.text(s, RUN_DIALOG[0] + 28, RUN_DIALOG[1] + 6, 'Run a program', PAPER, TEAL)


def run_dialog(d, s):
    """the launcher's chooser: its title, wcalc in its list, and the label of the line to run, clear of the list"""
    x, y, _, h = RUN_DIALOG
    return run_titled(d, s) and any(d.text(s, x + 44, y + 64 + 18 * row + 1, 'wcalc', INK) for row in range(12)) and \
        d.text(s, x + 16, y + h - 74 - 18, 'The program, and its arguments:', MUTED, PAPER)


def run_program(d):
    """Apps > Run program... by the keyboard: Ctrl+G, three menus right, past the suite's applications, Enter"""
    d.key('ctrl-g')
    for _ in range(3):
        d.key('right')
    for _ in range(5 + len(APPS)):                  # Notepad, Files, Calculator, Sketch, Clear sketch, the suite
        d.key('down')
    d.key('ret')


def term_run(d, grid, name, command, test, timeout=20):
    """term.run(); when the rows do not come, the Terminal's rows go to the log and its screen to the evidence"""
    d.keys(command + '\n')
    scr = term.wait_rows(d, grid, test, timeout)
    if scr is None:
        s = d.vm.screen()
        print(f'{name}: the Terminal shows', *grid.text(s), sep='\n    ', flush=True)
        if d.outdir:
            s.png(os.path.join(d.outdir, 'guiapps-win-terminal.png'))
    d.require(name, scr is not None)
    return scr


def answered(rows, command, answer):
    """below the last row where command was typed: a row that is answer, then the prompt"""
    for i in range(len(rows) - 1, -1, -1):
        if rows[i].endswith('> ' + command):
            for k, r in enumerate(rows[i + 1:]):
                if term.PROMPT.search(r):
                    return answer in rows[i + 1:i + 1 + k]
            return False
    return False


def desktop_up(d, s):
    """the desktop's first screen, as gui_apps_scenario.run() waits for it: its menu bar and the suite's tiles"""
    return (s.w, s.h) == (1920, 1080) and d.pixel(s, 0, 0) == PAPER and \
        d.text(s, 24 + 152 + 36, 120 + 14, 'Terminal', FOAM)


def serial_lines(d, command, timeout=15):
    """command on the serial console, the desktop's own (free once the desktop has closed): what it printed, by
    lines, or None when its prompt did not come back"""
    start = len(d.vm.transcript)
    d.vm.cursor = start
    d.vm.send(command + '\n')
    if not d.vm.expect(d.prompt, timeout):
        return None
    return [line.strip() for line in
            ANSI.sub('', d.vm.transcript[start:].decode(errors='replace')).replace('\r', '').split('\n')]


def leave(d):
    """SurfOS > About SurfOS in front (the desktop's own window: Ctrl+Q is then the desktop's, where a program's
    window would take it), then Ctrl+Q, all by the keyboard, in order; the question about programs' windows, and the
    wait for the text console, are the caller's"""
    d.key('ctrl-g')
    d.key('ret')
    d.vm.wait_screen(lambda s: titled_at(d, s, ABOUT, 'About SurfOS', True), 10, interval=0.2)
    d.vm.cursor = len(d.vm.transcript)
    d.key('ctrl-q')


def start_again(d):
    """the desktop started again on the serial console, as gui_apps_scenario.run() starts it; a new desktop puts the
    pointer in the middle of the screen"""
    d.vm.send('gui\n')
    up = d.vm.wait_screen(lambda s: desktop_up(d, s), 15, interval=0.1) is not None
    if up:
        d.position[:] = [480, 270]
    return up


def end_window(d, at, widths):
    """a program's window at at closed: its close button (its frame one of widths wide), and End when it stays"""
    def there(s):
        return any(titled_at(d, s, at, t) for t in ('wintest', 'wdemo', 'Calculator'))
    for w in widths:
        d.click(at[0] + w - 16, at[1] + 14)
        if d.vm.wait_screen(lambda s: not there(s), 5, interval=0.2) is not None:
            return
        d.click(at[0] + w - 16, at[1] + 14)       # "End the program?", Cancel first: End
        if d.vm.wait_screen(lambda s: term.dialog_up(d, s, 'End the program?'), 5, interval=0.2) is not None:
            d.key('right')
            d.key('ret')
            return


def tidy(d, state):
    """after the checks, or a failure: the desktop running again, its dialogs and the programs' windows closed, then
    the Terminal"""
    if state.get('left'):                           # the desktop was left: a new one has none of these
        if not state.get('back'):
            if term.dialog_up(d, d.vm.screen(), 'Leave the desktop?'):
                d.key('ret')                        # still asking: Leave
            d.vm.expect('text console restored', 10)
            d.vm.expect(d.prompt, 10)
            start_again(d)
        return
    s = d.vm.screen()
    if run_titled(d, s) or term.dialog_up(d, s, 'End the program?') or term.dialog_up(d, s, 'Leave the desktop?'):
        d.key('esc')
    for title, widths in (('wdemo', (362, 462)), ('Calculator', (248,)), ('wintest', (322,))):
        for _ in range(3):                          # each of its windows, wherever the cascade put it
            at = find(d, d.vm.screen(), title)
            if at is None:
                break
            end_window(d, at, widths)
    term.close_terminal(d)


def checks(d):
    grid = term.Grid()
    x, y = FIRST
    state = {}
    try:
        d.move(*PARK)
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal, for a program with a window', d.title('Terminal'))
        d.require('the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
        scr = term_run(d, grid, 'wintest tty names the Terminal\'s pty', 'wintest tty',
                       lambda rows: any(re.fullmatch(r'WINTEST tty: pty\d+', r) for r in rows) and term.prompted(rows))
        state['pty'] = [r for r in grid.text(scr) if re.fullmatch(r'WINTEST tty: pty\d+', r)][-1].split()[-1]
        term_run(d, grid, 'wintest pty in the Terminal: refused on a pty of its own, while this desktop runs',
                 'wintest pty', lambda rows: term.then_prompt(rows, 'WINTEST pty PASS'), timeout=30)
        # wintest, with the bare calls
        d.keys('wintest show &\n')
        d.visible('wintest show opens a window in front of the Terminal, without the focus: its title and its '
                  'quadrants at 2x', lambda s: titled(d, s, 'wintest', False) and quadrants(d, s), 'guiapps-wintest.png',
                  timeout=20)
        d.keys('z')                                 # the Terminal's still: the program hears nothing of it
        d.check('a key typed meanwhile stays the Terminal\'s', d.vm.wait_screen(
            lambda s: titled(d, s, 'wintest: key', False) or titled(d, s, 'wintest: key'), 2, interval=0.2) is None)
        d.key('backspace')                          # and leaves the Terminal's line as it was
        d.click(x + 150, y + 14)                    # its title bar: the focus
        d.visible('a click on its title bar gives it the focus', lambda s: titled(d, s, 'wintest'))
        d.move(*PARK)
        d.keys('a')
        d.visible('a key typed reaches the program: its title says "key 97"',
                  lambda s: titled(d, s, 'wintest: key 97'))
        d.click(x + 322 - 16, y + 14)               # its close button
        d.visible('its close button ends the program, and its window goes',
                  lambda s: not quadrants(d, s) and not titled(d, s, 'wintest'), timeout=15)
        # wdemo, through libgui
        d.move(*PARK)
        d.keys('wdemo &\n')
        d.visible('wdemo opens a window through libgui: its colour bars and its size as text at 2x',
                  lambda s: wdemo_shown(d, s, 'last: nothing yet'), 'guiapps-wdemo.png', timeout=20)
        d.click(SECOND[0] + 150, SECOND[1] + 14)    # its title bar: the focus, which wdemo is told
        d.visible('a click on its title bar gives wdemo the focus, its last event',
                  lambda s: wdemo_shown(d, s, 'last: focus 1'))
        d.move(*PARK)
        d.keys('a')
        d.visible('a key typed is wdemo\'s last event', lambda s: wdemo_shown(d, s, 'last: key 97'))
        cx, cy = SECOND[0] + 1, SECOND[1] + TITLE_H
        d.move(cx + 100, cy + 60)
        d.button(True)
        d.visible('a press of the button over wdemo is its last event, where it was',
                  lambda s: wdemo_shown(d, s, 'last: pointer 100,60 buttons 1'))
        d.button(False)
        d.visible('and the release', lambda s: wdemo_shown(d, s, 'last: pointer 100,60 buttons 0'))
        d.key('f2')
        d.visible('F2 shows a text console; the desktop waits', lambda s: d.pixel(s, 0, 0) == b'\0\0\0' and
                  not wdemo_shown(d, s))
        d.key('f1')
        d.visible('F1 brings the desktop back, wdemo\'s window as it was',
                  lambda s: wdemo_shown(d, s, 'last: pointer 100,60 buttons 0'), 'guiapps-wdemo-f1.png')
        d.position[:] = [480, 270]                  # a desktop that has the screen again puts the pointer in the middle
        gx, gy = SECOND[0] + 362 - 6, SECOND[1] + 240 + TITLE_H + 1 - 6
        d.move(gx, gy)                              # the corner grip, the desktop's own
        d.button(True)
        d.move(gx + 100, gy + 60)
        d.button(False)
        d.visible('its corner dragged 100 by 60: wdemo answers the new size, its bars and its size drawn again',
                  lambda s: wdemo_at(d, s, SECOND, 460, 300, 'last: size 460x300'), 'guiapps-wdemo-resized.png')
        d.click(SECOND[0] + 462 - 16, SECOND[1] + 14)
        d.visible('its close button ends wdemo', lambda s: not wdemo_at(d, s, SECOND, 460, 300) and
                  find(d, s, 'wdemo') is None, timeout=15)
        d.move(*PARK)
        term_run(d, grid, 'surfsh tells that wdemo is done (and collects it)', '',
                 lambda rows: term.then_prompt(rows, 'done  wdemo'))
        term_run(d, grid, 'ps in the Terminal: no wdemo is left', 'ps | grep -c wdemo',
                 lambda rows: answered(rows, 'ps | grep -c wdemo', '0'))
        # wcalc, from the launcher
        run_program(d)
        d.visible('Apps > Run program... lists /initrd/bin\'s programs, with a line to run', lambda s: run_dialog(d, s),
                  'guiapps-run.png')
        d.key('ret')                                # wcalc, offered
        d.visible('Run starts wcalc: the Calculator as a program, drawn as the desktop draws its own',
                  lambda s: calc_titled(d, s) and calc_shows(d, s, 0) and not run_titled(d, s), 'guiapps-wcalc.png',
                  timeout=20)
        d.keys('12+30=')
        d.visible('typed, 12+30= makes 42 in wcalc', lambda s: calc_shows(d, s, 42))
        for key, value in (('C', 0), ('7', 7), ('*', 7), ('6', 6), ('=', 42)):
            calc_click(d, key)
            d.visible(f'a click on wcalc\'s {key} shows {value}', lambda s, v=value: calc_shows(d, s, v))
        d.key('esc')
        d.visible('Esc ends wcalc, and its window goes', lambda s: not calc_titled(d, s) and not calc_shows(d, s, 42),
                  timeout=15)
        # the focused Terminal's foreground job: a job its shell made, so its window takes the focus as it appears
        d.move(*PARK)
        d.keys('wdemo\n')                           # without &
        d.visible('wdemo typed without & (the Terminal\'s foreground job) opens its window with the focus',
                  lambda s: titled_at(d, s, FG, 'wdemo  ', True) and wdemo_at(d, s, FG, last='last: focus 1'),
                  'guiapps-wdemo-fg.png', timeout=20)
        d.keys('b')
        d.visible('a key typed reaches it at once, its last event', lambda s: wdemo_at(d, s, FG, last='last: key 98'))
        d.click(FG[0] + 362 - 16, FG[1] + 14)       # its close button: it ends, and its shell is back
        d.visible('its close button ends it, and the Terminal has the focus again',
                  lambda s: find(d, s, 'wdemo') is None and d.title('Terminal')(s), timeout=15)
        d.move(*PARK)
        term_run(d, grid, 'the Terminal\'s prompt is back', 'echo fg-done', lambda rows: term.then_prompt(rows, 'fg-done'))
        # a program that pays no heed to the close button
        d.move(*PARK)
        d.keys('wdemo hang &\n')
        d.visible('wdemo hang opens its window', lambda s: titled_at(d, s, HANG, 'wdemo hang') and wdemo_at(d, s, HANG),
                  timeout=20)
        d.click(HANG[0] + 150, HANG[1] + 14)        # its title bar: the focus (wdemo's last event), then its close button
        d.vm.wait_screen(lambda s: wdemo_at(d, s, HANG, last='last: focus 1'), 10, interval=0.2)
        d.click(HANG[0] + 362 - 16, HANG[1] + 14)
        d.visible('its close button: the program is told (its last event) and the window stays',
                  lambda s: wdemo_at(d, s, HANG, last='last: close'))
        d.click(HANG[0] + 362 - 16, HANG[1] + 14)
        d.visible('pressed again, the desktop asks "End the program?"',
                  lambda s: term.dialog_up(d, s, 'End the program?'), 'guiapps-end.png')
        d.key('right')                              # Cancel first: End
        d.key('ret')
        d.visible('End kills it, and its window goes', lambda s: not term.dialog_up(d, s, 'End the program?') and
                  find(d, s, 'wdemo') is None, timeout=15)
        d.move(*PARK)
        term_run(d, grid, 'surfsh says the job was killed', '', lambda rows: term.then_prompt(rows, 'killed  wdemo hang'))
        # a program that faults
        d.keys('wdemo crash &\n')
        for _ in range(6):                          # surfsh tells of the job's end at a prompt: Enter until it has
            if term.wait_rows(d, grid, lambda rows: term.then_prompt(rows, 'killed  wdemo crash'), 3) is not None:
                break
            d.keys('\n')
        d.require('wdemo crash faults after it first draws: surfsh says it was killed',
                  term.wait_rows(d, grid, lambda rows: term.then_prompt(rows, 'killed  wdemo crash'), 5) is not None)
        d.check('the crash loses only its window: none is left', find(d, d.vm.screen(), 'wdemo') is None)
        term_run(d, grid, 'the Terminal and the desktop go on', 'echo still-here',
                 lambda rows: term.then_prompt(rows, 'still-here'))
        # a program the desktop started: it outlives the Terminal and ends with the desktop
        run_program(d)
        d.visible('Run program... again', lambda s: run_dialog(d, s))
        d.key('ret')
        scr = d.visible('wcalc starts again', lambda s: find(d, s, 'Calculator') is not None and
                        calc_shows(d, s, 0, find(d, s, 'Calculator')), timeout=20)
        at = find(d, scr, 'Calculator')
        term.close_terminal(d)
        d.visible('the Terminal closes with its session, and wcalc stays: it is not the session\'s',
                  lambda s: not term.shown(d, s) and calc_shows(d, s, 0, at))
        d.click(at[0] + 100, at[1] + 14)            # its title bar: in front
        d.move(*PARK)
        d.keys('6*7=')
        d.visible('and it still counts: 6*7= makes 42', lambda s: calc_shows(d, s, 42, at))
        # the Terminal's pty, free again, is no desktop's: a pty of its own that takes its number is refused
        d.vm.cursor = len(d.vm.transcript)
        run_program(d)
        d.visible('Run program... for wintest pty', lambda s: run_dialog(d, s))
        d.keys('wintest pty\n')                     # on the desktop's console: its lines on the serial line
        d.check(f'wintest pty, started by the desktop, takes the closed Terminal\'s {state["pty"]} and is refused there',
                d.vm.expect(f'wintest nodesk on {state["pty"]}: ENODEV', 20) and d.vm.expect('WINTEST pty PASS', 10))
        d.click(at[0] + 100, at[1] + 14)            # wcalc in front again, for the leaving
        d.move(*PARK)
        leave(d)
        state['left'] = True
        d.visible('leaving the desktop asks first: a program has a window open',
                  lambda s: term.dialog_up(d, s, 'Leave the desktop?'), 'guiapps-leave.png')
        d.key('ret')                                # Leave
        d.require('Leave closes the desktop, and the text console comes back',
                  d.vm.expect('text console restored', 20) and d.vm.expect(d.prompt, 15))
        for _ in range(3):
            lines = serial_lines(d, 'ps | grep -c wcalc')
            if lines and '0' in lines:
                break
            d.vm.pump(1.0)
        d.check('wcalc ended with the desktop: ps on the serial console finds none', lines and '0' in lines)
        state['back'] = start_again(d)
        d.require('the desktop starts again for the checks that follow', state['back'])
    finally:
        try:
            tidy(d, state)
        except AssertionError:                      # what failed first is what the results say
            pass
