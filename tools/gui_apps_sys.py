"""The guiapps scenario's checks of track SYS (docs/superpowers/plans/2026-10-17-gui-suite.md): System Monitor and
Disks on the screen, driven by the keyboard and the mouse only (gui_apps_scenario.py's Driver), read back from the
framebuffer's pixels: the cells of a table are matched against the console font (driver/font8x16.c), as qemu-usb.py
reads the text console.

System Monitor: the Processes page lists the four consoles' shells (surfsh, programs on F1-F4) and the kernel's
threads as ps names them; a click on a heading sorts by it, again reverses it; End task refuses the desktop's own
task (the surfsh on F1 that ran gui), and ends the shell on F2, which init starts again with a new pid. The Memory
page's bars and numbers are plausible for the 512 MB guest; the System page shows the guest's processor, UEFI, the
laptop profile, the 1920x1080 framebuffer and an uptime that goes on.

Disks: lsblk's devices (the initrd, the boot stick usb0 with its partition, the data stick usb1 with its FAT32
partition, not mounted), Mount (the shell's mount) with its line in the status bar, the volume label and a bar of the
space used (zz-sys-fill.bin, 4 MB, is on the stick for it), then Eject: the stick is unmounted again when these checks
end, as it was found, also after a failure. readback() checks on the host that the stick's file is intact.
"""
import re

import gui_scenario as base
from gui_apps_scenario import window, PAPER, INK, TEAL, MUTED, BAND, FOAM

SHEET = bytes.fromhex('fffdf5')                 # a list's background (shell/gui_sys.h SYS_SHEET)
SHELL = bytes.fromhex('dee3d8')                 # a list's headings
CORAL = bytes.fromhex('ee906d')                 # a bar from 90% used
TITLE_H, BAR_H, ROW_H, STATUS_H = 28, 28, 18, 22
HEAD_Y = TITLE_H + BAR_H + 3                    # a list's headings, window-relative
LIST_Y = HEAD_Y + 20                            # its first row
TOOL_Y = TITLE_H + 4 + 11                       # the middle of the toolbar's buttons
MON, DISKS = 'System Monitor', 'Disks'
# System Monitor: where its columns start (shell/guimon.c column_x), the pids' right edge, the toolbar's tabs
COLUMN_X = (0, 62, 196, 272, 344)
PID_RIGHT = 52
TABS = {'Processes': 8 + 43, 'Memory': 98 + 31, 'System': 164 + 31}
DETAIL_H = 66                                   # Disks' details, above its status bar (shell/guidisks.c)

FILL = bytes(range(256)) * 16384                # 4 MB: Disks' bar of used space has something to show
FILL_NAME = 'zz-sys-fill.bin'                   # at the stick's root, after every other track's file and folder
STICK = {FILL_NAME: FILL}


# ---- reading the screen ---------------------------------------------------------------------

class Reader:
    """a row of cells read back as text: each 8x16 cell's lit pixels (any of the colours given) against the font"""
    def __init__(self, d):
        self.glyphs = {rows: chr(code) for code, rows in d.font.items() if 32 <= code < 127}

    def text(self, s, x, y, cells, colours):
        out = []
        for i in range(cells):
            rows = tuple(sum(0x80 >> col for col in range(8) if base._pixel(s, x + i * 8 + col, y + row) in colours)
                         for row in range(16))
            out.append(self.glyphs.get(rows, '?'))
        return ''.join(out).rstrip()


def _status(d, name, text, colour=INK):
    """a predicate: the status bar of the window shows text at its left"""
    x, y, _, h = window(name)
    return lambda s: d.text(s, x + 10, y + h - STATUS_H - 1 + 3, text, colour, BAND)


def _park(d):
    """the pointer away from what is read (it would hide a cell): on the desktop below the windows"""
    d.move(24, 470)


# ---- System Monitor -------------------------------------------------------------------------

def _tasks(d, r, s):
    """the Processes page's rows: [(pid, name, state, console, kind)], stopping at the first empty row"""
    x, y, _, h = window(MON)
    rows = []
    for row in range((h - STATUS_H - 1 - LIST_Y) // ROW_H):
        py = y + LIST_Y + row * ROW_H + 1
        pid = r.text(s, x + PID_RIGHT - 6 * 8, py, 6, (MUTED,)).strip()
        if not pid:
            break
        rows.append((int(pid) if pid.isdigit() else -1,
                     r.text(s, x + COLUMN_X[1] + 8, py, 15, (INK,)),
                     r.text(s, x + COLUMN_X[2] + 8, py, 8, (MUTED, TEAL)),
                     r.text(s, x + COLUMN_X[3] + 8, py, 7, (MUTED,)),
                     r.text(s, x + COLUMN_X[4] + 8, py, 13, (INK, MUTED))))
    return rows


def _wait_tasks(d, r, what, test, save=None, timeout=15):
    """the first screen whose rows pass test(rows), and those rows"""
    found = []

    def ok(s):
        rows = _tasks(d, r, s)
        if rows and test(rows):
            found[:] = rows
            return True
        return False
    d.visible(what, ok, save, timeout)
    return found


def _select(d, row):
    """the row of the list, by the keys (Home, then Down)"""
    d.key('home')
    for _ in range(row):
        d.key('down')


def _number(text):
    """'4.5 MB' (or '512 bytes') as kilobytes; None for anything else"""
    m = re.fullmatch(r'([\d.]+) (bytes?|KB|MB|GB)', text)
    return float(m.group(1)) * {'KB': 1, 'MB': 1024, 'GB': 1024 * 1024}.get(m.group(2), 1 / 1024) if m else None


def monitor(d):
    r = Reader(d)
    x, y, w, h = window(MON)
    d.open_app(MON)
    d.visible('System Monitor opens from the Apps menu', d.title(MON), 'guiapps-system-monitor.png')
    _park(d)                                                   # before any cell is read
    rows = _wait_tasks(d, r, 'System Monitor lists the consoles\' shells and the kernel\'s threads, in pid order',
                       lambda rows: [p for p, *_ in rows] == sorted(p for p, *_ in rows) and
                       {('surfsh', 'F1'), ('surfsh', 'F2'), ('surfsh', 'F3'), ('surfsh', 'F4')} <=
                       {(n, c) for _, n, _, c, _ in rows} and {'init', 'usb', 'net'} <= {n for _, n, *_ in rows})
    d.check('the shells are programs, init, usb and net kernel threads (as ps has them)',
            all(k == 'program' for _, n, _, _, k in rows if n == 'surfsh') and
            all(k == 'kernel thread' for _, n, _, _, k in rows if n in ('init', 'usb', 'net')) and
            all(s in ('ready', 'running', 'blocked', 'sleeping') for _, _, s, _, _ in rows))
    # the order of the rows shown (a task may come or go meanwhile): by name, ties by pid; then reversed (ties still by
    # pid); neither is the pid order, since init (pid 1) comes before the idle task (pid 0) by name
    def by_name(rows):
        return sorted(rows, key=lambda t: (t[1].lower(), t[0]))

    def by_pid(rows):
        return [p for p, *_ in rows] == sorted(p for p, *_ in rows)
    d.click(x + COLUMN_X[1] + 40, y + HEAD_Y + 10)             # the Name heading
    _park(d)
    _wait_tasks(d, r, 'a click on the Name heading sorts the tasks by name',
                lambda rows: rows == by_name(rows) and not by_pid(rows))
    d.click(x + COLUMN_X[1] + 40, y + HEAD_Y + 10)
    _park(d)
    _wait_tasks(d, r, 'a second click reverses the order',
                lambda rows: rows == sorted(sorted(rows, key=lambda t: t[0]), key=lambda t: t[1].lower(), reverse=True)
                and not by_pid(rows), 'guiapps-monitor-sorted.png')
    d.click(x + 20, y + HEAD_Y + 10)                           # the PID heading: back to pid order
    _park(d)
    rows = _wait_tasks(d, r, 'the PID heading sorts by pid again', by_pid)

    # End task: never the desktop's own task, the shell on F1 that ran gui
    own = [i for i, (_, n, _, c, k) in enumerate(rows) if n == 'surfsh' and c == 'F1' and k == 'program']
    d.require('the desktop\'s own shell (surfsh on F1) is listed', len(own) == 1)
    pid = rows[own[0]][0]
    _select(d, own[0])
    d.visible('the selected task is described in the status bar',
              _status(d, MON, f'surfsh (pid {pid}): a program on F1, the console\'s shell'))
    d.key('delete')
    d.visible('End task asks first', lambda s: d.text(s, 273, 204, 'End task', PAPER, TEAL) and
              d.text(s, 261, 242, f'End surfsh (pid {pid}) on F1?', INK, PAPER), 'guiapps-monitor-end-task.png')
    d.key('tab')                                               # Cancel is the default: to End task
    d.key('ret')
    d.visible('End task refuses the desktop\'s own task, and says why in the status bar',
              _status(d, MON, f'surfsh (pid {pid}) runs this desktop: Ctrl+Q leaves it'), 'guiapps-monitor-refused.png')
    _wait_tasks(d, r, 'the desktop\'s own task is still there', lambda rows: any(p == pid for p, *_ in rows))

    # a real End task: the shell on F2, which init starts again (a new pid on F2)
    rows = _wait_tasks(d, r, 'the shell on F2 is listed',
                       lambda rows: any(n == 'surfsh' and c == 'F2' for _, n, _, c, _ in rows))
    row = next(i for i, (_, n, _, c, _) in enumerate(rows) if n == 'surfsh' and c == 'F2')
    old = rows[row][0]
    _select(d, row)
    d.key('delete')
    d.visible('End task asks about the shell on F2',
              lambda s: d.text(s, 261, 242, f'End surfsh (pid {old}) on F2?', INK, PAPER) and
              d.text(s, 261, 264, 'A console\'s shell: init starts a new one there.', MUTED, PAPER))
    d.key('esc')
    d.visible('Esc answers Cancel: nothing ended', _status(d, MON, f'surfsh (pid {old}): a program on F2'))
    d.key('delete')
    d.key('tab')
    d.key('ret')
    d.visible('End task ends the program: kill\'s words in the status bar',
              _status(d, MON, f'ended surfsh (pid {old})'))
    _wait_tasks(d, r, 'its pid is gone, and init has started a new shell on F2',
                lambda rows: all(p != old for p, *_ in rows) and
                any(n == 'surfsh' and c == 'F2' and p > old for p, n, _, c, _ in rows), 'guiapps-monitor-ended.png')

    # Memory: three meters with plausible numbers for a 512 MB guest
    d.click(x + TABS['Memory'], y + TOOL_Y)
    _park(d)
    top = y + HEAD_Y + 10
    found = {}

    def memory(s):
        if not all(d.text(s, x + 16, top + 72 * k, title, TEAL) for k, title in
                   enumerate(('Physical memory', 'Kernel heap', 'DMA heap'))):
            return False
        found['physical'] = r.text(s, x + 16, top + 44, (w - 32) // 8, (INK,))
        found['heap'] = r.text(s, x + 16, top + 72 + 44, (w - 32) // 8, (INK,))
        found['bars'] = [(base._pixel(s, x + 18, top + 72 * k + 30), base._pixel(s, x + w - 20, top + 72 * k + 30))
                         for k in range(3)]
        return True
    d.visible('the Memory tab shows physical memory, the kernel heap and the DMA heap', memory,
              'guiapps-monitor-memory.png')
    m = re.fullmatch(r'(.+) used, (.+) free, of (.+) in pages of 4 KB', found['physical'])
    used, free, total = (_number(m.group(k)) if m else None for k in (1, 2, 3))
    d.check(f'physical memory is plausible for 512 MB ({found["physical"]!r})',
            None not in (used, free, total) and 0 < used < total and 0 < free < total and
            300 * 1024 <= total <= 512 * 1024 and abs(used + free - total) <= total / 50)
    m = re.fullmatch(r'(.+) used, (.+) free', found['heap'])
    heap = (_number(m.group(1)), _number(m.group(2))) if m else (None, None)
    d.check(f'the kernel heap is in use ({found["heap"]!r})', None not in heap and heap[0] > 64 and heap[1] >= 0)
    d.check('each bar shows what is used (teal, coral from 90%), and physical memory\'s what is free',
            all(left in (TEAL, CORAL) for left, _ in found['bars']) and found['bars'][0][1] == SHEET)

    # System: the guest's machine
    d.click(x + TABS['System'], y + TOOL_Y)
    _park(d)
    values = {}

    def system(s):
        labels = ('Processor', 'Firmware', 'Profile', 'Display', 'Build', 'Uptime', 'Tasks', 'Timers')
        if not all(d.text(s, x + 16, top + 24 * k, label, MUTED) for k, label in enumerate(labels)):
            return False
        for k, label in enumerate(labels):
            values[label] = r.text(s, x + 16 + 104, top + 24 * k, (w - 144) // 8, (INK,))
        return True
    d.visible('the System tab shows the machine', system, 'guiapps-monitor-system.png')
    d.check(f'System: a QEMU processor, UEFI, the laptop profile, the 1920x1080 framebuffer ({values})',
            'QEMU' in values.get('Processor', '') and values.get('Firmware') == 'UEFI' and
            values.get('Profile') == 'laptop' and
            values.get('Display', '').startswith('1920x1080x32 (the desktop: 960x540)'))
    d.check('System: an uptime as h:mm:ss, some tasks and timers',
            re.fullmatch(r'\d+:\d\d:\d\d', values.get('Uptime', '')) is not None and
            values.get('Tasks', '').isdigit() and int(values['Tasks']) >= 8 and values.get('Timers', '').isdigit())
    first = values.get('Uptime', '')
    d.visible('the uptime goes on (read again every second)', lambda s: system(s) and values['Uptime'] != first,
              timeout=10)


# ---- Disks ------------------------------------------------------------------------------------

# the test image's devices as lsblk lists them: name, partition, size, type, model or label, status (the data stick
# not mounted)
DEVICES = [('rd0', False, None, 'disk', 'initrd.tar', 'mounted'),
           ('usb0', False, '256M', 'disk gpt', 'QEMU QEMU HARDDISK', 'boot drive: protected'),
           ('usb0p1', True, '254M', 'part gpt EFI system', 'SURFOSUSB', 'boot drive: protected'),
           ('usb1', False, '64M', 'disk mbr', 'QEMU QEMU HARDDISK', ''),
           ('usb1p1', True, '63M', 'part mbr FAT32 LBA', '', '')]


def _columns(d, s):
    """where Disks' columns are on this screen, from its headings: the columns are as wide as what they hold
    (shell/guidisks.c's layout(): the longest size string, type and so on), so they are found, not assumed;
    (size_r, bus_x, type_x, status_x) window-relative, or None"""
    x, y, w, _ = window(DISKS)
    found = {}
    for word in ('Size', 'Bus', 'Type', 'Status'):
        for at in range(40, w - 40):
            if d.text(s, x + at, y + HEAD_Y + 2, word, MUTED, SHELL):
                found[word] = at
                break
        else:
            return None
    return found['Size'] + 32, found['Bus'], found['Type'], found['Status']


def _row_named(d, s, name, part):
    """the row of Disks' list that shows this device, or None"""
    x, y, _, h = window(DISKS)
    for i in range((h - STATUS_H - 1 - DETAIL_H - LIST_Y) // ROW_H):
        if d.text(s, x + 28 + (16 if part else 0), y + LIST_Y + i * ROW_H + 1, name, INK):
            return i
    return None


def disks(d, state):
    r = Reader(d)
    x, y, w, h = window(DISKS)
    detail_y = y + h - STATUS_H - 1 - DETAIL_H
    cols = {}

    def row_y(i):
        return y + LIST_Y + i * ROW_H + 1

    def listed(s):
        found = _columns(d, s)
        if not found:
            return False
        size_r, bus_x, type_x, status_x = cols['at'] = found
        for i, (name, part, size, kind, _, status) in enumerate(DEVICES):
            if not d.text(s, x + 28 + (16 if part else 0), row_y(i), name, INK):
                return False
            if size and not d.text(s, x + size_r - len(size) * 8, row_y(i), size, MUTED):
                return False
            if not d.text(s, x + type_x, row_y(i), kind, MUTED):
                return False
            if not d.text(s, x + bus_x, row_y(i), 'ram' if i == 0 else 'usb', MUTED):
                return False
            if status and not d.text(s, x + status_x, row_y(i), status, TEAL if status == 'mounted' else MUTED):
                return False
        return True
    d.open_app(DISKS)
    d.visible('Disks opens from the Apps menu', d.title(DISKS))
    _park(d)                                                   # before any cell is read
    d.visible('Disks lists the devices as lsblk does: the initrd, the boot stick and the data stick, partitions under them',
              lambda s: d.title(DISKS)(s) and listed(s), 'guiapps-disks.png')
    status_x = cols['at'][3]
    stick = next(i for i, dev in enumerate(DEVICES) if dev[0] == 'usb1p1')
    _select(d, stick)
    d.visible('the data stick\'s partition: its details, not mounted',
              lambda s: d.text(s, x + 10, detail_y + 8, 'usb1p1', TEAL) and
              d.text(s, x + 10 + 48, detail_y + 8, ': MBR partition of usb1, 63M (FAT32 LBA)', INK) and
              d.text(s, x + 10, detail_y + 45, 'Not mounted: Mount (or Enter) opens it at /usb1p1', MUTED) and
              _status(d, DISKS, 'Mount (M) or Enter mounts usb1p1')(s), 'guiapps-disks-stick.png')
    state['mounted'] = True                                    # from here on, the end makes sure it is ejected
    d.key('m')
    d.visible('Mount mounts it (the shell\'s mount: its line in the status bar)',
              lambda s: _status(d, DISKS, 'usb1p1 on /usb1p1: fat32, label APPSTICK, ')(s) and
              d.text(s, x + status_x, row_y(stick), 'mounted', TEAL), 'guiapps-disks-mounted.png')
    found = {}

    def mounted(s):
        if not d.text(s, x + 10, detail_y + 26, 'Mounted on /usb1p1: fat32, read-write, volume APPSTICK', INK):
            return False
        bar = (w - 20) // 3
        found['bar'] = (base._pixel(s, x + 10 + 2, detail_y + 46 + 7),
                        base._pixel(s, x + 10 + bar - 4, detail_y + 46 + 7))
        found['words'] = r.text(s, x + 10 + bar + 10, detail_y + 45, (w - 20 - bar - 10) // 8, (MUTED,))
        return True
    d.visible('its details: where it is mounted, its volume label and a bar of the space used', mounted)
    m = re.fullmatch(r'(.+) used, (.+) free, of (.+) \((\d+)%\)', found.get('words', ''))
    used, free, total = (_number(m.group(k)) if m else None for k in (1, 2, 3))
    d.check(f'the bar and its numbers: 4 MB of the stick\'s 62 MB used, at least ({found.get("words")!r})',
            found.get('bar') == (TEAL, SHEET) and None not in (used, free, total) and 4 * 1024 <= used < total and
            60 * 1024 <= total <= 64 * 1024 and abs(used + free - total) <= 1024)
    d.key('e')
    d.visible('Eject unmounts it and flushes the stick (the shell\'s eject)',
              lambda s: _status(d, DISKS, 'usb1: safe to remove')(s) and
              not d.text(s, x + status_x, row_y(stick), 'mounted', TEAL) and
              d.text(s, x + 10, detail_y + 45, 'Not mounted: Mount (or Enter) opens it at /usb1p1', MUTED),
              'guiapps-disks-ejected.png')
    state['mounted'] = False


# ---- the track's checks ------------------------------------------------------------------------

MENU_LEFT = (8, 96, 152, 208, 264, 336)       # where each menu's title starts (shell/guicore.c menu_left())


def _menu_open(d, s):
    """a menu of the menu bar is open: its title is drawn on teal"""
    return any(base._pixel(s, left - 2, 4) == TEAL for left in MENU_LEFT)


def _dialog_open(d, s):
    """a dialog is in front: the desktop's dialog box (a message, a question, a line to type) has the wave in its
    title bar, where shell/guidialog.c centres it on the 960x540 desktop"""
    return any(d.text(s, 245 + 12, top + 6, '\xf7', FOAM, TEAL) for top in (198, 191))


def _leave(d, name):
    """the window closed, and a menu or a dialog in front of it first. Esc is sent only to close a menu or a dialog
    that is there: with nothing in front, Esc closes the window in front, and with no window open at all it would
    leave the desktop"""
    s = d.vm.screen()
    if _menu_open(d, s) or _dialog_open(d, s):
        d.key('esc')
        d.vm.wait_screen(lambda s: not _menu_open(d, s) and not _dialog_open(d, s), 5, interval=0.2)
    if d.vm.wait_screen(lambda s: d.title(name)(s) or d.title(name, False)(s), 2, interval=0.2) is not None:
        d.close(name)
        d.vm.wait_screen(lambda s: not d.title(name)(s) and not d.title(name, False)(s), 5, interval=0.2)


def _eject_stick(d):
    """as the stick was found: not mounted. Disks in front, the data stick's partition found by its name, Eject"""
    d.open_app(DISKS)
    if d.vm.wait_screen(d.title(DISKS), 10, interval=0.2) is None:
        return
    _park(d)
    s = d.vm.wait_screen(lambda s: _row_named(d, s, 'usb1p1', True) is not None, 10, interval=0.2)
    if s is None:
        return
    _select(d, _row_named(d, s, 'usb1p1', True))
    d.key('e')
    d.vm.wait_screen(_status(d, DISKS, 'usb1: safe to remove'), 10, interval=0.2)


def checks(d):
    state = {'mounted': False}
    try:
        monitor(d)
    finally:
        _leave(d, MON)
    try:
        disks(d, state)
    finally:
        if state['mounted']:
            _eject_stick(d)
        _leave(d, DISKS)


def readback(d, stick, helpers):
    d.check('the data stick\'s file is intact after Mount and Eject in Disks',
            helpers['read_fat32'](stick, FILL_NAME) == FILL)
