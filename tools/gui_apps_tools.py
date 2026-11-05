"""The guiapps scenario's checks of track TOOLS (docs/superpowers/plans/2026-10-17-gui-suite.md): Log, Clock and
Character Map on the screen, driven only by the keyboard, the mouse and QEMU's monitor (a USB mouse plugged in and
out, so that the kernel logs something while the Log follows), and read from the framebuffer's pixels. The Driver is
gui_apps_scenario.py's. Each part leaves the desktop as it found it, also when one of its checks fails: its window
closed, Notepad closed without saving, no dialog left open, the USB mouse it plugged in unplugged again.
10/26/26 (stage C12 of docs/superpowers/plans/2026-10-25-app-windows.md): the Clock is a program (user/wclock/),
started by the Apps menu, its window where the desktop's Clock's stood and drawn as it was, so the same checks read
it. At zero its window comes back in front without the focus, "Time's up!" in its title and on its page, where a
message of the desktop's came up before (a check changed with the behaviour); a click in it answers, once the half
second in which a press on a window that came up by itself is nobody's has passed.
10/26/26: the Character Map is a program too (user/wcharmap/), read by the same checks; Notepad comes from the Apps
menu, for Ctrl+E is the program's key while its window has the focus (a check changed with the behaviour)."""
import calendar
import time

from gui_apps_scenario import window, PAPER, INK, TEAL, MUTED, BAND, FOAM

CORAL = bytes.fromhex('ee906d')         # 10/26/26: the Clock's alarm

SHEET = bytes.fromhex('fffdf5')         # the Log's text and Notepad's
MATCH = bytes.fromhex('e8d08a')         # the Log's match found
TOP = 28 + 28 + 4                       # a window's page below its title bar and toolbar
NOTEPAD = (296, 38, 640, 420)           # where Notepad opens at 960x540 (gui_scenario.py)
LOG, CLOCK, CHARMAP = window('Log'), window('Clock'), window('Character Map')
MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October',
          'November', 'December']
WEEKDAYS = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday']   # time.struct_time's


# ---- the screen

def _big(d, s, x, y, text, fg, scale):
    """text drawn at scale: the middle of every lit pixel's square is fg"""
    for i, ch in enumerate(text):
        for row, bits in enumerate(d.font[ord(ch)]):
            for col in range(8):
                if bits & (0x80 >> col) and \
                        d.pixel(s, x + (i * 8 + col) * scale + scale // 2, y + row * scale + scale // 2) != fg:
                    return False
    return True


def _read(d, s, x, y, n, fg, scale=1, chars='0123456789:.'):
    """the n characters drawn at x, y in fg at scale, each one of chars (lit pixels fg, the others not): None if not"""
    out = ''
    for i in range(n):
        for ch in chars:
            if all((d.pixel(s, x + (i * 8 + col) * scale + scale // 2, y + row * scale + scale // 2) == fg) ==
                   bool(bits & (0x80 >> col)) for row, bits in enumerate(d.font[ord(ch)]) for col in range(8)):
                out += ch
                break
        else:
            return None
    return out


def _glyph(d, s, x, y, ch, fg, bg, cols=8):
    """one character's cell: its lit pixels fg and the others bg, in its first cols columns (a text cursor may follow)"""
    return all(d.pixel(s, x + col, y + row) == (fg if bits & (0x80 >> col) else bg)
               for row, bits in enumerate(d.font[ord(ch)]) for col in range(cols))


def _taskbar(d, s, text, fg):
    """a taskbar button's title begins with text, wherever the button is"""
    for n in range(1, 16):
        w = min(156, (960 - 96) // n - 4)
        if any(d.text(s, 6 + i * (w + 4) + 24, 540 - 26 + 5, text, fg) for i in range(n)):
            return True
    return False


def _dialog(d, s):
    """the dialog in front (its ≋ and title), or None: a message or question, a text box, a chooser"""
    for x, y in ((245, 198), (245, 191), (220, 81)):
        if d.text(s, x + 12, y + 6, '\xf7', FOAM, TEAL):
            for title in ('Unsaved changes', 'Close the window?', 'Timer'):
                if d.text(s, x + 28, y + 6, title, PAPER, TEAL):
                    return title
            return '?'
    return None


def _answer(d):
    """no dialog left in front: Don't save, Close (it was ours to close), OK, or Cancel"""
    for _ in range(4):
        d.vm.pump(0.3)
        title = _dialog(d, d.vm.screen())
        if title is None:
            return
        for chord in {'Unsaved changes': ('tab', 'ret'), 'Close the window?': ('ret',), 'Timer': ('ret',)}.get(title, ('esc',)):
            d.key(chord)


def _shown(d, s, name):
    """the window is open: its title where it opened (in front or not), or its button in the taskbar"""
    if name == 'Notepad':
        return d.text(s, NOTEPAD[0] + 30, NOTEPAD[1] + 6, 'Untitled', PAPER) or \
            d.text(s, NOTEPAD[0] + 30, NOTEPAD[1] + 6, '*Untitled', PAPER)
    return d.title(name)(s) or d.title(name, focused=False)(s) or _taskbar(d, s, name, FOAM) or _taskbar(d, s, name, MUTED)


def _shut(d, name):
    """the window closed, however it was left: any question answered, the window brought up by the Apps menu (or
    opened), then Ctrl+W; again when a dialog came up meanwhile and took the keys (the timer's alarm, say)"""
    for _ in range(3):
        try:
            _answer(d)
            if name == 'Notepad':
                d.key('ctrl-e')
            else:
                d.open_app(name)
            d.vm.pump(0.5)
            if _dialog(d, d.vm.screen()):
                continue
            d.key('ctrl-w')
            _answer(d)
            if not _shown(d, d.vm.screen(), name):
                return
        except Exception:                   # the next part still runs; its own checks say what is wrong
            pass


# ---- Log

def _log_row(d, s, row, text, bg=SHEET):
    return d.text(s, LOG[0] + 8, LOG[1] + TOP + row * 16, text, INK, bg)


def _log_rows(d, s, rows, text, bg=SHEET):
    return any(_log_row(d, s, r, text, bg) for r in rows)


def _wrapped(d, s):
    """the Log's status bar says that the kernel log has wrapped round: "<n> lines, the newest 16 KB" """
    return any(d.text(s, LOG[0] + 10 + digits * 8, LOG[1] + LOG[3] - 23 + 3, ' lines, the newest 16 KB', INK, BAND)
               for digits in range(1, 6))


def _monitor(d, name, line):
    """a QEMU monitor command, recorded as a check: HMP says Error when it fails"""
    reply = d.vm.mon.cmd(line)
    if 'rror' in reply:
        print(f'{line}: {reply.strip()}', flush=True)
    return d.check(name, 'rror' not in reply)


def _usb_line(d, s, rows, after):
    """a USB mouse's +USB: line (any port) in those rows: plugged in (after ', ') or unplugged"""
    return any(_log_rows(d, s, rows, f'+USB: port {port}: 0627:0001 QEMU {after}') for port in range(1, 9))


def log(d):
    x, y, w, h = LOG
    status_y = y + h - 23 + 3
    plugged = False
    try:
        d.move(900, 470)                        # the pointer off the window, whose rows are read
        d.open_app('Log')
        d.visible('Log: opens from the Apps menu following the kernel log (Follow lit, "Follow on")',
                  lambda s: d.title('Log')(s) and d.text(s, x + 8 + 7, y + 28 + 7, 'Follow', PAPER, TEAL) and
                  d.text(s, x + w - 24 - 9 * 8, status_y, 'Follow on', TEAL, BAND), 'guiapps-log.png')
        plugged = _monitor(d, 'Log: QEMU plugs in a USB mouse (device_add)', 'device_add usb-mouse,id=toolsmouse,bus=xhci.0')
        if not plugged:
            raise AssertionError('device_add')
        d.visible('Log: a USB mouse plugged in, its +USB: line comes in at the end (Follow)',
                  lambda s: _usb_line(d, s, range(12, 18), 'QEMU USB Mouse, '), 'guiapps-log-follow.png', timeout=30)
        plugged = not _monitor(d, 'Log: QEMU unplugs it (device_del)', 'device_del toolsmouse')
        d.visible('Log: and unplugged, the next line comes in below it',
                  lambda s: _usb_line(d, s, range(15, 18), 'USB Mouse unplugged') and
                  _usb_line(d, s, range(12, 17), 'QEMU USB Mouse, '), timeout=30)
        d.key('home')
        d.visible('Log: Home shows the kernel log from its first line, as dmesg does (or the newest 16 KB of it, when it '
                  'has wrapped round); Follow stops',
                  lambda s: (_wrapped(d, s) or _log_row(d, s, 0, 'SurfOS: serial console on COM1 (115200 8N1)') and
                             _log_row(d, s, 1, 'GDT Initialization')) and
                  d.text(s, x + w - 24 - 15 * 8, status_y, 'Follow off: End', MUTED, BAND), 'guiapps-log-home.png')
        d.key('ctrl-f')
        d.keys('unplugged')
        d.key('ret')
        d.visible('Log: Ctrl+F finds a word: its line comes into view, selected, the word marked',
                  lambda s: any(d.text(s, x + 8, y + TOP + r * 16, f'+USB: port {port}: 0627:0001 QEMU USB Mouse ', INK) and
                                d.text(s, x + 8 + 39 * 8, y + TOP + r * 16, 'unplugged', INK, MATCH)
                                for r in range(16) for port in range(1, 9)),
                  'guiapps-log-find.png')
        d.key('esc')
        d.visible('Log: Esc closes the find bar', lambda s: d.title('Log')(s) and
                  not d.text(s, x + 10, y + h - 23 - 28 + 6, 'Find:', INK, BAND))
        d.close('Log')
        d.visible('Log: closes', lambda s: not d.title('Log')(s) and not d.title('Log', focused=False)(s))
    finally:
        if plugged:
            d.vm.mon.cmd('device_del toolsmouse')
        if d.vm.wait_screen(lambda s: d.title('Log')(s) or d.title('Log', focused=False)(s), 1, interval=0.2):
            _shut(d, 'Log')


# ---- Clock

def _centred(text, scale, width=380):
    return (width - len(text) * 8 * scale) // 2


def clock(d):
    x, y, w, h = CLOCK
    big_y = y + TOP + 8
    try:
        d.open_app('Clock')
        d.visible('Clock: opens from the Apps menu', d.title('Clock'))
        found = {}

        def same_time(s):
            """the large time is the taskbar clock's, with its seconds; the date below it is today's (UTC)"""
            now = _read(d, s, 960 - 10 - 40, 540 - 20, 5, FOAM)
            shown = _read(d, s, x + _centred('00:00:00', 4), y + TOP + 10, 8, INK, 4)
            if not now or not shown or shown[:5] != now:
                return False
            for t in (time.gmtime(), time.gmtime(time.time() - 120)):
                date = f'{MONTHS[t.tm_mon - 1]} {t.tm_mday}, {t.tm_year}'
                day = WEEKDAYS[t.tm_wday]
                if _big(d, s, x + _centred(day, 2), y + TOP + 88, day, TEAL, 2) and \
                        _big(d, s, x + _centred(date, 2), y + TOP + 124, date, INK, 2):
                    found['date'] = t
                    return True
            return False
        d.visible('Clock: the time large, as the taskbar\'s clock shows it, and today\'s weekday and date',
                  same_time, 'guiapps-clock.png')
        t = found['date']
        d.key('2')
        month = f'{MONTHS[t.tm_mon - 1]} {t.tm_year}'
        cell = (calendar.weekday(t.tm_year, t.tm_mon, 1) + 1) % 7 + t.tm_mday - 1      # weeks from Sunday
        cx = x + (w - 7 * 48) // 2 + cell % 7 * 48
        cy = y + TOP + 62 + cell // 7 * 24
        d.visible(f'Clock: the calendar shows {month}, today ({t.tm_mday}) marked',
                  lambda s: _big(d, s, x + 70 + (w - 72 - 4 - 70 - len(month) * 16) // 2, y + TOP + 5, month, INK, 2) and
                  d.pixel(s, cx + 8, cy + 3) == TEAL and
                  d.text(s, cx + (48 - len(str(t.tm_mday)) * 8) // 2, cy + 4, str(t.tm_mday), PAPER),
                  'guiapps-clock-calendar.png')
        d.key('3')
        d.visible('Clock: the stopwatch at 00:00.0', lambda s: _big(d, s, x + _centred('00:00.0', 4), big_y, '00:00.0', INK, 4))
        d.key('spc')
        seen = []

        def counting(s):
            v = _read(d, s, x + _centred('00:00.0', 4), big_y, 7, TEAL, 4)
            if v and v != '00:00.0' and (not seen or v != seen[-1]):
                seen.append(v)
            return len(seen) >= 2 and seen[-1] > seen[0]
        d.visible('Clock: Space starts the stopwatch, and it counts', counting, 'guiapps-clock-stopwatch.png')
        d.key('spc')
        stopped = []

        def holds(s):
            v = _read(d, s, x + _centred('00:00.0', 4), big_y, 7, INK, 4)
            if v:
                stopped.append((v, time.time()))
            return len(stopped) >= 2 and stopped[-1][0] == stopped[0][0] and stopped[-1][1] - stopped[0][1] > 1
        d.visible('Clock: Space stops it, and it holds its time', holds)
        d.check(f'Clock: the stopwatch counted from 00:00.0 ({seen[0]}, {seen[-1]}, then {stopped[0][0]})',
                stopped[0][0] >= seen[-1] > seen[0] > '00:00.0')
        d.key('r')
        d.visible('Clock: R resets it', lambda s: _big(d, s, x + _centred('00:00.0', 4), big_y, '00:00.0', INK, 4))
        d.key('4')
        d.visible('Clock: the timer, set to 05:00', lambda s: _big(d, s, x + _centred('05:00', 4), big_y, '05:00', INK, 4))
        d.key('home')
        d.key('pgup')
        d.key('pgup')
        d.visible('Clock: Home and Page Up set it to twenty seconds',
                  lambda s: _big(d, s, x + _centred('00:20', 4), big_y, '00:20', INK, 4))
        d.key('spc')
        d.visible('Clock: Space starts it: the title counts down',
                  lambda s: d.text(s, x + 30, y + 6, 'Clock - 00:', PAPER, TEAL) and
                  _read(d, s, x + _centred('00:20', 4), big_y, 5, TEAL, 4) not in (None, '00:20'))
        d.click(x + w - 40, y + 14)             # minimize: it keeps running, tucked away in the taskbar
        d.visible('Clock: the timer runs, the window tucked away: its taskbar button counts down',
                  lambda s: not d.title('Clock')(s) and _taskbar(d, s, 'Clock - 00:0', MUTED))
        # 10/26/26 (C12): the program asks to be seen (WIN_ALERT): its window in front, the focus where it was
        d.visible("Clock: at zero the window comes back in front, without the focus: Time's up! in its title and on "
                  "its page", lambda s: d.text(s, x + 30, y + 6, "Time's up! - Clock", PAPER, MUTED) and
                  _big(d, s, x + _centred('00:00', 4), big_y, '00:00', CORAL, 4) and
                  d.text(s, x + _centred("Time's up!", 1), y + TOP + 78, "Time's up!", CORAL),
                  'guiapps-clock-alarm.png', timeout=60)
        time.sleep(0.6)                         # a press on a window that came up by itself is nobody's for 500 ms;
        d.click(x + 60, y + TOP + 180)          # then a click in it: the focus, and the alarm answered
        d.visible('Clock: a click answers it, the timer ready again',
                  lambda s: d.title('Clock')(s) and _big(d, s, x + _centred('00:20', 4), big_y, '00:20', INK, 4))
        d.key('home')
        for _ in range(5):
            d.key('up')
        d.visible('Clock: the timer set back to 05:00', lambda s: _big(d, s, x + _centred('05:00', 4), big_y, '05:00', INK, 4))
        d.key('1')
        d.close('Clock')
        d.visible('Clock: closes', lambda s: not d.title('Clock')(s) and not _taskbar(d, s, 'Clock', MUTED) and
                  not _taskbar(d, s, 'Clock', FOAM))
    finally:
        s = d.vm.screen()
        if _dialog(d, s) or d.title('Clock')(s) or d.title('Clock', focused=False)(s) or \
                _taskbar(d, s, 'Clock', MUTED) or _taskbar(d, s, 'Clock', FOAM) or _taskbar(d, s, "Time's up!", MUTED):
            _end_clock(d)


def _end_clock(d):
    """10/26/26: the Clock's program ended however it was left: its window brought up (Apps > Clock), its close
    button, and when it asks (a timer or the stopwatch running) the close button again: the desktop's End"""
    x, y, w, _ = CLOCK
    for _ in range(2):
        try:
            _answer(d)
            d.open_app('Clock')
            d.vm.pump(1.0)
            d.click(x + w - 16, y + 14)
            d.vm.pump(1.0)
            if _shown(d, d.vm.screen(), 'Clock'):       # it asked in its window: 11/05/26, pressed again it is asked
                for _ in range(3):                      # again, and the third press in 3 s is the desktop's question
                    d.click(x + w - 16, y + 14)
                d.vm.pump(0.5)
                if _dialog(d, d.vm.screen()) == '?':      # "End the program?", Cancel first: End
                    d.key('right')
                    d.key('ret')
            d.vm.pump(1.0)
            if not _shown(d, d.vm.screen(), 'Clock'):
                return
        except Exception:
            pass


# ---- Character Map

def charmap(d):
    x, y, w, h = CHARMAP
    status_y = y + h - 23 + 3
    notepad = False
    try:
        d.open_app('Character Map')
        d.visible('Character Map: opens from the Apps menu, the 256 characters in a grid',
                  lambda s: d.title('Character Map')(s) and d.text(s, x + 26 + 7 * 17 + 5, y + 50 + 15 * 17 + 1, '\xf7', INK) and
                  d.text(s, x + 26 + 1 * 17 + 5, y + 50 + 4 * 17 + 1, 'A', INK, SHEET))
        d.click(x + 26 + 7 * 17 + 8, y + 50 + 15 * 17 + 8)
        d.move(900, 470)                        # the pointer off the cell, which is read next (and off Notepad)
        d.visible('Character Map: a click chooses ≋ (0xF7, SurfOS\'s own): large, with its code, U+224B and its UTF-8',
                  lambda s: d.text(s, x + 26 + 7 * 17 + 5, y + 50 + 15 * 17 + 1, '\xf7', PAPER, TEAL) and
                  d.text(s, x + 312 + 64, y + 148, '247', INK) and d.text(s, x + 312 + 64, y + 148 + 18, '0xF7', INK) and
                  d.text(s, x + 312 + 64, y + 148 + 36, 'U+224B', INK) and
                  d.text(s, x + 312 + 64, y + 148 + 54, 'E2 89 8B', INK) and
                  _big(d, s, x + 312 + (138 - 66) // 2 + 9, y + 34 + 5, '\xf7', INK, 6), 'guiapps-charmap.png')
        d.key('ctrl-c')
        d.visible('Character Map: Ctrl+C copies it as UTF-8 (the status bar says so)',
                  lambda s: d.text(s, x + 10, status_y, 'Copied \xf7 (3 bytes of UTF-8): Ctrl+V pastes it', INK, BAND))
        d.key('ctrl-g')                         # 10/26/26: Apps > Notepad (Ctrl+E is the program's own key)
        for _ in range(3):
            d.key('right')
        d.key('ret')
        notepad = True
        d.visible('Character Map: Apps > Notepad opens Notepad',
                  lambda s: d.text(s, NOTEPAD[0] + 30, NOTEPAD[1] + 6, 'Untitled - Notepad', PAPER, TEAL))
        d.key('ctrl-v')
        d.visible('Character Map: Ctrl+V pastes ≋ in Notepad: one character (Col 2), three bytes',
                  lambda s: _glyph(d, s, NOTEPAD[0] + 44, NOTEPAD[1] + 60, '\xf7', INK, SHEET, 7) and
                  d.text(s, NOTEPAD[0] + 30, NOTEPAD[1] + 6, '*Untitled - Notepad', PAPER, TEAL) and
                  d.text(s, NOTEPAD[0] + 10, NOTEPAD[1] + NOTEPAD[3] - 23 + 3, 'Ln 1, Col 2', INK, BAND) and
                  d.text(s, NOTEPAD[0] + NOTEPAD[2] - 24 - 11 * 8, NOTEPAD[1] + NOTEPAD[3] - 23 + 3, '3 bytes  LF', MUTED, BAND),
                  'guiapps-charmap-notepad.png')
        d.key('ctrl-w')
        d.visible('Character Map: closing Notepad asks about the unsaved note', lambda s: _dialog(d, s) == 'Unsaved changes')
        d.key('tab')
        d.key('ret')
        notepad = False
        d.visible('Character Map: Don\'t save closes Notepad; the Character Map is in front again',
                  lambda s: _dialog(d, s) is None and not d.text(s, NOTEPAD[0] + 30, NOTEPAD[1] + 6, '*Untitled', PAPER) and
                  d.title('Character Map')(s))
        d.close('Character Map')
        d.visible('Character Map: closes', lambda s: not d.title('Character Map')(s) and
                  not d.title('Character Map', focused=False)(s))
    finally:
        if notepad:
            _shut(d, 'Notepad')
        s = d.vm.screen()
        if _dialog(d, s) or d.title('Character Map')(s) or d.title('Character Map', focused=False)(s):
            _shut(d, 'Character Map')


def checks(d):
    """each part on its own: one that fails still leaves the desktop clean for the next"""
    failed = None
    for part in (log, clock, charmap):
        try:
            part(d)
        except AssertionError as e:
            failed = failed or e
            d.vm.screen(f'guiapps-tools-{part.__name__}-failed.png')
    if failed:
        raise failed
