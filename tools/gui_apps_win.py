"""The guiapps scenario's checks of programs' windows (milestone M12, docs/superpowers/plans/2026-10-25-app-windows.md);
the Driver is gui_apps_scenario.py's. 10/25/26 (stage C4): wintest show, typed in the Terminal, opens a window in
front with its title and its four quadrants at 2x; a key typed reaches the program (its title says which); its close
button sends CLOSE and the program ends, and its window goes. (stage C5) wdemo, through libgui: its colour bars and
its size as text at 2x, a key shown as its last event, and its close button. 10/25/26 (the review): a window comes in
front without the focus (its title bar muted), and a key typed meanwhile stays the Terminal's; a click on its title bar
gives it the focus. The checks leave the desktop as they found it (the windows and the Terminal closed), also when one
fails."""
import gui_apps_term as term
from gui_apps_scenario import PAPER, TEAL, MUTED

CORAL, TEALQ, SKY, SAND = (bytes.fromhex('ee906d'), bytes.fromhex('197d83'), bytes.fromhex('659ecb'),
                           bytes.fromhex('d0b056'))         # user/bin/wintest.c's quadrants
FIRST = (72, 52)            # where shell/guiclient.c opens the first program's window of a desktop (its cascade)
SECOND = (96, 72)           # and the second
TITLE_H = 28
NAVY, FOAM, PAPER_TEXT = bytes.fromhex('102e3b'), bytes.fromhex('b8e9d9'), bytes.fromhex('f4f1e6')
BARS = (bytes.fromhex('ee906d'), bytes.fromhex('d0b056'), bytes.fromhex('7ed48c'), bytes.fromhex('197d83'),
        bytes.fromhex('659ecb'), bytes.fromhex('9b5fa7'), bytes.fromhex('f4f1e6'), bytes.fromhex('153d49'))


def titled(d, s, text, focused=True):
    """the first program's window's title bar shows text (its start)"""
    return d.text(s, FIRST[0] + 30, FIRST[1] + 6, text, PAPER, TEAL if focused else MUTED)


def quadrants(d, s, w=320, h=200):
    """wintest's four quadrants in the first program's window, w x h"""
    x, y = FIRST[0] + 1, FIRST[1] + TITLE_H
    return (d.pixel(s, x + 10, y + 10) == CORAL and d.pixel(s, x + w - 10, y + 10) == TEALQ and
            d.pixel(s, x + 10, y + h - 10) == SKY and d.pixel(s, x + w - 10, y + h - 10) == SAND)


def wdemo_shown(d, s, last=None):
    """wdemo's window, the second program's of the desktop: its bars, its size, and its last event if given"""
    x, y = SECOND[0] + 1, SECOND[1] + TITLE_H
    ok = all(d.pixel(s, x + 45 * i + 20, y + 80) == BARS[i] for i in range(8)) and \
        d.text(s, x + 8, y + 168, 'wdemo 360x240', FOAM, NAVY)
    return ok and (last is None or d.text(s, x + 8, y + 188, last, PAPER_TEXT, NAVY))


def checks(d):
    grid = term.Grid()
    x, y = FIRST
    try:
        d.move(900, 480)
        d.key('ctrl-t')
        d.visible('Ctrl+T opens the Terminal, for a program with a window', d.title('Terminal'))
        d.require('the Terminal shows its prompt', term.wait_rows(d, grid, term.prompted, 30) is not None)
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
        d.move(900, 480)
        d.keys('a')
        d.visible('a key typed reaches the program: its title says "key 97"',
                  lambda s: titled(d, s, 'wintest: key 97'))
        d.click(x + 322 - 16, y + 14)               # its close button
        d.visible('its close button ends the program, and its window goes',
                  lambda s: not quadrants(d, s) and not titled(d, s, 'wintest'), timeout=15)
        # wdemo, through libgui
        d.move(900, 480)
        d.keys('wdemo &\n')
        d.visible('wdemo opens a window through libgui: its colour bars and its size as text at 2x',
                  lambda s: wdemo_shown(d, s, 'last: nothing yet'), 'guiapps-wdemo.png', timeout=20)
        d.click(SECOND[0] + 150, SECOND[1] + 14)    # its title bar: the focus, which wdemo is told
        d.visible('a click on its title bar gives wdemo the focus, its last event',
                  lambda s: wdemo_shown(d, s, 'last: focus 1'))
        d.move(900, 480)
        d.keys('a')
        d.visible('a key typed is wdemo\'s last event', lambda s: wdemo_shown(d, s, 'last: key 97'))
        d.click(SECOND[0] + 362 - 16, SECOND[1] + 14)
        d.visible('its close button ends wdemo', lambda s: not wdemo_shown(d, s), timeout=15)
    finally:
        if wdemo_shown(d, d.vm.screen()):           # wdemo left over: its close button
            d.click(SECOND[0] + 362 - 16, SECOND[1] + 14)

        def there(s):
            return titled(d, s, 'wintest') or titled(d, s, 'wintest', False)
        if there(d.vm.screen()):                    # left over (a check failed): CLOSE, then End if it stays
            d.click(x + 322 - 16, y + 14)
            if d.vm.wait_screen(lambda s: not there(s), 5, interval=0.2) is None:
                d.click(x + 322 - 16, y + 14)       # "End the program?", Cancel first: End
                d.key('right')
                d.key('ret')
        term.close_terminal(d)
