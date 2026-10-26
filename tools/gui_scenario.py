#!/usr/bin/env python3
"""Visible desktop integration checks, called by qemu-usb.py's GUI scenario.

Only public keyboard, mouse and shell interfaces drive the guest. Expected
application state is read from framebuffer pixels, not guest memory or hooks.
10/17/26: the menus and windows of the new layout; Notepad and Files on a FAT32
data stick (usb1), which is read back on the host afterwards; the desktop waits
through a console switch and comes back on its own console.
10/26/26 (stage C8 of docs/superpowers/plans/2026-10-25-app-windows.md): the wheel, from QEMU's
monitor (mouse_move 0 0 1), of the PS/2 mouse (a wheel mouse by the IntelliMouse handshake) and of
the USB mouse: each scrolls Help three lines a notch. A right-click on the desktop opens Apps where
it was (kept on the screen), and its first item opens Notepad.
"""
import os
import re
import shutil
import tempfile


PAPER = bytes.fromhex('f4f1e6')
INK = bytes.fromhex('153d49')
NAVY = bytes.fromhex('102e3b')
FOAM = bytes.fromhex('b8e9d9')
OCEAN = bytes.fromhex('245a67')
SHEET = bytes.fromhex('fffdf5')      # Notepad's text and the Files list
BAND = bytes.fromhex('e7e2d2')       # status bars
TEAL = bytes.fromhex('197d83')
MUTED = bytes.fromhex('6c888c')
CURSOR = (
    'X', 'XX', 'X.X', 'X..X', 'X...X', 'X....X', 'X.....X',
    'X......X', 'X.......X', 'X........X', 'X.....XXXXX',
    'X..X..X', 'X.X X..X', 'XX  X..X', 'X    XX',
)
# The layout at 960x540 logical pixels (include/sys/gui.h, shell/guicore.c): the menu titles' text,
# the Apps menu's items (24 pixels apart), and where the windows open.
SURFOS_X, APPS_X, MENU_Y = 12, 212, 6
APPS_ITEM_X, ITEM_Y, ITEM_H = 218, 36, 24
CALC = (688, 50)
SKETCH = (264, 64)
SETTINGS = (280, 72)
NOTEPAD = (296, 38, 640, 420)
FILES = (24, 48, 600, 380)
HELP, HELP_MENU_X, TITLE_H = (210, 38), 340, 28  # 10/26/26: Help's window, and the Help menu's title text
APPS_W = 192                                     # 10/26/26: the Apps menu's width (its widest item, Calculator)
STICK = {'notes.txt': b'Hello from the Mac\r\nSecond line\r\n', 'Folder/inner.txt': b'inside\n'}
EDITED = b'SurfOS was here. Hello from the Mac\r\nSecond line\r\n'


def _font():
    path = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'driver', 'font8x16.c')
    with open(path, encoding='utf-8') as source:
        entries = re.findall(r'\[0x([0-9a-fA-F]{2})\] = \{ ([^}]*) \}', source.read())
    if len(entries) != 256:
        raise ValueError('could not read all 256 framebuffer glyphs')
    return {int(code, 16): tuple(int(row.strip(), 16) for row in rows.split(','))
            for code, rows in entries}


def _pixel(screen, x, y):
    """GUI logical pixels, whose origin differs from the text-console margins."""
    return screen.pixel(x * 2, y * 2)


def _text(screen, font, x, y, text, foreground, background=None):
    for index, char in enumerate(text):
        for row, bits in enumerate(font[ord(char)]):
            for col in range(8):
                lit = bits & (0x80 >> col)
                if lit or background is not None:
                    if _pixel(screen, x + index * 8 + col, y + row) != (foreground if lit else background):
                        return False
    return True


def _cursor(screen, x, y):
    return all(_pixel(screen, x + col, y + row) == (NAVY if cell == 'X' else PAPER)
               for row, cells in enumerate(CURSOR) for col, cell in enumerate(cells) if cell != ' ')


def _prompt(screen):
    return any(re.search(r'(?:SurfOS\*>|[≋~][!#@?]?>)\s*$', row) for row in screen.rows)


def _title(x, y):
    """where a window's title text starts: after its icon"""
    return x + 30, y + 6


def _files_row(row, depth):
    """the name of row (from the top of the Files list) at depth"""
    return FILES[0] + 8 + depth * 16 + 30, FILES[1] + 79 + row * 18 + 1


def run(Vm, PROMPT, args, image, firmware, outdir, helpers=None):
    """Return qemu-usb's list of (description, passed), saving PNG evidence."""
    results = []
    font = _font()
    tmpd = tempfile.mkdtemp(prefix='surfos-gui-', dir='/tmp' if os.path.isdir('/tmp') else None)
    stick = os.path.join(tmpd, 'stick.img')
    extra = ['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080']
    if helpers:
        helpers['mkimage'].make_fat32(stick, STICK, size_mb=64, label='GUISTICK', cluster_bytes=512, serial=0x6A11F11E)
        extra += ['-drive', f'if=none,id=guistick,format=raw,file={stick}',
                  '-device', 'usb-storage,bus=xhci.0,drive=guistick,removable=on']
    vm = Vm('gui', args, image, firmware, mem='512', serial=True, outdir=outdir, extra=extra)
    position = [480, 270]

    def require(name, passed):
        results.append((name, bool(passed)))
        if not passed:
            raise AssertionError(name)

    def visible(name, predicate, save=None, timeout=15):
        screen = vm.wait_screen(predicate, timeout, interval=0.1)
        require(name, screen is not None)
        if save and outdir:
            screen.png(os.path.join(outdir, save))
        return screen

    def move(x, y):
        # Avoid device saturation and prove each step reached the visible cursor.
        # Coordinates stay below the right/bottom clipping edges of its bitmap.
        while position != [x, y]:
            dx = max(-100, min(100, x - position[0]))
            dy = max(-100, min(100, y - position[1]))
            vm.mon.cmd(f'mouse_move {dx} {dy}')
            position[0] += dx
            position[1] += dy
            if vm.wait_screen(lambda s: _cursor(s, *position), 10, interval=0.1) is None:
                raise AssertionError(f'visible mouse cursor did not reach {tuple(position)}')

    def button(held):
        vm.mon.cmd(f'mouse_button {1 if held else 0}')
        # Pacing for the hardware sample interval; application assertions below
        # prove the effects rather than treating elapsed time as evidence.
        vm.pump(0.05)

    def click(x, y):
        move(x, y)
        button(True)
        button(False)

    def key(chord):
        vm.mon.cmd('sendkey ' + chord)

    def help_from(line):
        """a predicate: Help shows its text from line 0 (its heading, The desktop) or 3 on"""
        x, y = HELP[0] + 14, HELP[1] + TITLE_H + 8
        if line == 0:
            return lambda s: _text(s, font, x + 16, y, 'The desktop', TEAL)
        return lambda s: _text(s, font, x, y, 'Ctrl+O opens a file in Notepad', INK)

    def right_click_menu():
        """10/26/26 (stage C8): a right-click on the desktop opens Apps where it was, moved left to stay on the
        screen, its first item chosen; a click on it opens Notepad, and Ctrl+W closes Notepad again"""
        move(880, 60)
        vm.mon.cmd('mouse_button 2')
        vm.pump(0.05)
        vm.mon.cmd('mouse_button 0')
        x = 960 - APPS_W
        visible('a right-click on the desktop opens the Apps menu there, kept on the screen',
                lambda s: _text(s, font, x + 14, 60 + 8, 'Notepad', PAPER, TEAL) and
                _text(s, font, x + 14, 60 + 8 + 2 * ITEM_H, 'Calculator', INK) and
                not _text(s, font, APPS_ITEM_X, ITEM_Y, 'Notepad', PAPER), 'gui-right-click.png')
        click(x + 20, 60 + 16)
        visible('its first item opens Notepad', notepad_title('Untitled'))
        key('ctrl-w')
        visible('Ctrl+W closes it again', lambda s: _pixel(s, 900, 300) != SHEET)

    def help_wheel(mouse):
        """10/26/26 (stage C8): Help > Keyboard and mouse, the wheel over Help (a notch toward the user is three lines
        down, one away from the user three lines up), then Esc closes it"""
        click(HELP_MENU_X + 10, 14)
        click(HELP_MENU_X + 10, ITEM_Y + 8)
        visible(f'{mouse}: Help > Keyboard and mouse opens Help at its first line', help_from(0))
        move(HELP[0] + 270, HELP[1] + 200)
        vm.mon.cmd('mouse_move 0 0 -1')
        visible(f'{mouse}: a notch of the wheel toward the user scrolls Help three lines down', help_from(3))
        vm.mon.cmd('mouse_move 0 0 1')
        visible(f'{mouse}: a notch away from the user, three lines back up', help_from(0))
        key('esc')
        visible(f'{mouse}: Esc closes Help', lambda s: not help_from(0)(s) and not help_from(3)(s))

    def desktop():
        position[:] = [480, 270]
        vm.send('gui\n')
        return visible('desktop renders the ocean menu bar at 1920x1080',
                       lambda s: (s.w, s.h) == (1920, 1080) and
                       _pixel(s, 0, 0) == PAPER and _pixel(s, 900, 400) == OCEAN and
                       _text(s, font, SURFOS_X, MENU_Y, '\xf7 SurfOS', INK, PAPER) and
                       _text(s, font, APPS_X, MENU_Y, 'Apps', INK, PAPER))

    def restored(name):
        require(name + ': shell responds', vm.expect('text console restored', 15) and vm.expect(PROMPT, 15))
        visible(name + ': framebuffer text and margins restored',
                lambda s: _prompt(s) and 'text console restored' in s.text() and s.pixel(0, 0) == b'\0\0\0')

    def display(value, x=CALC[0], y=CALC[1]):
        return lambda s: _text(s, font, x + 28, y + 58, str(value) + '  ', FOAM, INK)

    def apps_item(i):
        return APPS_X + 48, ITEM_Y + ITEM_H * i + 6

    def notepad_title(name):
        return lambda s: _text(s, font, *_title(*NOTEPAD[:2]), name + ' - Notepad', PAPER, TEAL)

    def notepad_status(text):
        return lambda s: _text(s, font, NOTEPAD[0] + 10, NOTEPAD[1] + NOTEPAD[3] - 23 + 3, text, INK, BAND)

    def notepad_ends(text):
        """the right of Notepad's status bar: the size and the line ends (CRLF only when the conversion happened)"""
        return lambda s: _text(s, font, NOTEPAD[0] + NOTEPAD[2] - 24 - len(text) * 8, NOTEPAD[1] + NOTEPAD[3] - 23 + 3,
                               text, MUTED, BAND)

    def files_status(text):
        return lambda s: _text(s, font, FILES[0] + 10, FILES[1] + FILES[3] - 23 + 3, text, INK, BAND)

    def notes_and_files():
        """Files mounts the stick and opens a file in Notepad; Save, Save as, the unsaved-changes question, Eject"""
        key('ctrl-b')
        visible('Ctrl+B opens Files: the root, then the sticks that are not mounted',
                lambda s: _text(s, font, *_title(*FILES[:2]), 'Files', PAPER, TEAL) and
                _text(s, font, *_files_row(0, 0), 'SurfOS', INK) and
                _text(s, font, *_files_row(1, 1), 'usb0p1', INK) and _text(s, font, *_files_row(2, 1), 'usb1p1', INK),
                'gui-files.png')
        for chord in ('home', 'down', 'down', 'ret'):
            key(chord)
        visible('Enter on the data stick mounts it (the shell\'s mount) and opens it',
                lambda s: files_status('usb1p1 on /usb1p1: fat32')(s) and
                _text(s, font, *_files_row(4, 1), 'usb1p1', INK) and
                _text(s, font, *_files_row(5, 2), 'Folder', INK) and _text(s, font, *_files_row(6, 2), 'notes.txt', INK),
                'gui-files-mounted.png')
        x, y = _files_row(6, 2)
        move(x + 120, y + 8)
        for _ in range(2):
            button(True)
            button(False)
        visible('a double-click opens the file in Notepad, its CRLF line ends seen as lines',
                lambda s: notepad_title('notes.txt')(s) and
                _text(s, font, NOTEPAD[0] + 44 + 8, NOTEPAD[1] + 60, 'ello from the Mac  ', INK, SHEET) and
                _text(s, font, NOTEPAD[0] + 44, NOTEPAD[1] + 76, 'Second line', INK, SHEET) and
                notepad_ends('31 bytes  CRLF')(s),
                'gui-notepad-open.png')
        vm.keys('SurfOS was here. ')
        key('ctrl-s')
        visible('typing and Ctrl+S save the file (CRLF again: 50 bytes)',
                lambda s: notepad_status('Ln 1, Col 18  Saved notes.txt (50 bytes)')(s) and
                _text(s, font, NOTEPAD[0] + 44, NOTEPAD[1] + 60, 'SurfOS was here.', INK, SHEET) and
                notepad_ends('48 bytes  CRLF')(s),
                'gui-notepad-saved.png')
        for chord in ('ctrl-g', 'right', 'down', 'down', 'down', 'ret'):
            key(chord)
        visible('File > Save as (by the keyboard) opens the chooser in the file\'s folder',
                lambda s: _text(s, font, 248, 87, 'Save as', PAPER, TEAL) and _text(s, font, 300, 119, '/usb1p1', INK),
                'gui-save-as.png')
        vm.keys('copy.txt\n')
        visible('Save as writes a second file, which Notepad then edits', notepad_title('copy.txt'))
        vm.keys('x')
        key('ctrl-q')
        visible('Ctrl+Q with unsaved changes asks first',
                lambda s: _text(s, font, 273, 204, 'Unsaved changes', PAPER, TEAL), 'gui-unsaved.png')
        key('esc')
        visible('Cancel keeps the desktop and the change', notepad_title('*copy.txt'))
        key('ctrl-z')
        visible('Ctrl+Z undoes the change: nothing is unsaved', notepad_title('copy.txt'))
        key('ctrl-b')
        click(FILES[0] + 502 + 27, FILES[1] + 32 + 11)
        visible('Eject in Files unmounts the stick and flushes it (the shell\'s eject)',
                lambda s: files_status('usb1: safe to remove')(s) and _text(s, font, *_files_row(2, 1), 'usb1p1', INK),
                'gui-ejected.png')
        key('ctrl-w')
        key('ctrl-w')
        visible('Ctrl+W closes Files and Notepad', lambda s: _pixel(s, 30, 300) != SHEET and _pixel(s, 900, 300) != SHEET)

    try:
        require('GUI boot reaches the shell', vm.expect(PROMPT, args.boot_timeout))
        for command in ('gui', 'pointerinfo'):
            vm.send(command + ' extra\n')
            require(command + ' rejects extra arguments with usage',
                    vm.expect('usage: ' + command, 15) and vm.expect(PROMPT, 15))
        vm.send('pointerinfo\n')
        require('native PS/2 mouse is detected before GUI launch, a wheel mouse (10/26/26: the IntelliMouse handshake)',
                vm.expect('PS/2 wheel mouse', 15) and vm.expect(PROMPT, 15))
        desktop()
        vm.screen('gui-desktop.png')
        right_click_menu()

        click(APPS_X + 18, 14)
        visible('PS/2 click opens the Apps menu',
                lambda s: _text(s, font, APPS_ITEM_X, ITEM_Y, 'Notepad', PAPER) and
                _text(s, font, APPS_ITEM_X, ITEM_Y + 2 * ITEM_H, 'Calculator', INK), 'gui-menu.png')
        click(*apps_item(2))
        visible('Apps menu opens Calculator',
                lambda s: _text(s, font, *_title(*CALC), 'Calculator', PAPER) and display(0)(s))
        for x, y, expected in [(728, 251, 1), (782, 251, 12), (890, 291, 12),
                               (836, 251, 3), (782, 291, 30), (836, 291, 42)]:
            click(x, y)
            visible(f'calculator mouse entry renders {expected}', display(expected))
        vm.screen('gui-calculator-42.png')

        key('ctrl-d')
        visible('Ctrl+D opens Sketch', lambda s: _text(s, font, *_title(*SKETCH), 'Sketch', PAPER))
        move(330, 190)
        button(True)
        move(450, 190)
        button(False)
        move(750, 400)
        stroke = lambda s: all(_pixel(s, x, 190) == INK for x in range(330, 451))
        visible('PS/2 drag paints a continuous Sketch stroke', stroke, 'gui-sketch.png')
        move(380, 190)
        move(750, 400)
        visible('moving the cursor over a stroke preserves the drawing', stroke)
        click(640, 114)
        visible('Sketch Clear removes the stroke',
                lambda s: all(_pixel(s, x, 190) == PAPER for x in range(330, 451)))

        move(300, 76)
        button(True)
        move(360, 126)
        button(False)
        move(800, 400)
        visible('title-bar drag moves the Sketch window',
                lambda s: _text(s, font, *_title(SKETCH[0] + 60, SKETCH[1] + 50), 'Sketch', PAPER) and
                _pixel(s, 265, 65) == OCEAN, 'gui-window-moved.png')
        click(740, 128)
        visible('window close control reveals the desktop', lambda s: _pixel(s, 325, 115) == OCEAN)
        key('ctrl-d')
        visible('reopening Sketch retains its moved position and cleared canvas',
                lambda s: _text(s, font, *_title(SKETCH[0] + 60, SKETCH[1] + 50), 'Sketch', PAPER) and
                _pixel(s, 400, 240) == PAPER)
        click(40, 14)
        visible('the SurfOS menu opens', lambda s: _text(s, font, 18, ITEM_Y, 'About SurfOS', PAPER) and
                _text(s, font, 18, ITEM_Y + ITEM_H, 'Pointer & desktop', INK, PAPER))
        click(800, 400)
        visible('clicking outside a menu closes it', lambda s: _pixel(s, 194, 32) == OCEAN)
        help_wheel('PS/2 wheel')
        key('ctrl-k')
        visible('Ctrl+K restores Calculator with its answer intact', display(42))

        key('ctrl-p')
        visible('Ctrl+P opens pointer settings with one native device',
                lambda s: _text(s, font, *_title(*SETTINGS), 'Pointer & desktop', PAPER) and
                _text(s, font, SETTINGS[0] + 16, SETTINGS[1] + 44, '1 pointing device connected', INK, PAPER))
        if helpers:
            notes_and_files()
        vm.mon.cmd('device_add usb-mouse,id=guimouse,bus=xhci.0')
        key('ctrl-p')
        visible('USB boot mouse hotplug appears in pointer settings',
                lambda s: _text(s, font, SETTINGS[0] + 16, SETTINGS[1] + 44, '2 pointing devices connected', INK, PAPER),
                'gui-usb-connected.png', timeout=30)
        mice = vm.mon.cmd('info mice')
        if outdir:
            with open(os.path.join(outdir, 'gui-mice.txt'), 'w', encoding='utf-8') as output:
                output.write(mice)
        usb = re.search(r'Mouse #(\d+):[^\n]*(?:USB|HID)[^\n]*Mouse', mice, re.I)
        require('QEMU exposes the hotplugged USB mouse for native input', usb is not None)
        vm.mon.cmd('mouse_set ' + usb.group(1))
        selected = vm.mon.cmd('info mice')
        require('QEMU routes mouse events to the USB device',
                re.search(r'\*\s+Mouse #' + usb.group(1) + r':', selected) is not None)
        click(APPS_X + 18, 14)
        visible('native USB mouse movement and click open Apps',
                lambda s: _text(s, font, APPS_ITEM_X, ITEM_Y, 'Notepad', PAPER))
        click(*apps_item(2))
        visible('native USB mouse selects Calculator', display(42))
        help_wheel('USB wheel')
        move(860, 64)
        button(True)
        move(870, 74)
        visible('native USB held button drags the window',
                lambda s: _text(s, font, *_title(CALC[0] + 10, CALC[1] + 10), 'Calculator', PAPER))
        vm.mon.cmd('device_del guimouse')
        key('ctrl-p')
        visible('unplugging the held USB mouse removes its device',
                lambda s: _text(s, font, SETTINGS[0] + 16, SETTINGS[1] + 44, '1 pointing device connected', INK, PAPER),
                timeout=30)
        vm.cursor = len(vm.transcript)
        key('ctrl-q')
        restored('Ctrl+Q closes the desktop')
        vm.send('pointerinfo\n')
        require('unplugging a held USB mouse releases its buttons',
                vm.expect('pointing devices: 1', 15) and vm.expect('buttons=0', 15) and vm.expect(PROMPT, 15))
        button(False)  # Reset the host's physical state only after proving removal released the guest.
        vm.screen('gui-normal-exit.png')

        desktop()
        vm.cursor = len(vm.transcript)
        vm.send('\x03')
        restored('Ctrl+C closes the desktop')
        desktop()
        # 10/25/26: frames drawn (the menu opened and closed again), timed for gfxinfo on another console
        key('ctrl-g')
        visible('Ctrl+G opens the first menu', lambda s: _text(s, font, SURFOS_X, MENU_Y, '\xf7 SurfOS', PAPER, TEAL))
        key('esc')
        visible('Esc closes it again', lambda s: _text(s, font, SURFOS_X, MENU_Y, '\xf7 SurfOS', INK, PAPER))
        vm.cursor = len(vm.transcript)
        vm.keys('<f2>')
        visible('F2 switches to a text console; the desktop waits on its own',
                lambda s: _prompt(s) and s.pixel(0, 0) == b'\0\0\0')
        vm.cursor = len(vm.transcript)
        vm.send('gfxinfo\n')
        require('gfxinfo names the desktop waiting on F1 and its frame times',
                vm.expect('desktop on F1: 960x540 at 2x', 15) and vm.expect('the last: composing', 15) and
                vm.expect(PROMPT, 15))
        desktop()
        key('ctrl-p')
        visible('the second console can own a new desktop session',
                lambda s: _text(s, font, *_title(*SETTINGS), 'Pointer & desktop', PAPER))
        key('2')
        visible('the second desktop selects pointer speed 2',
                lambda s: _text(s, font, 420, 179, '2 / Medium', PAPER))
        vm.mon.cmd('mouse_move 10 0')
        position[:] = [500, 270]
        visible('the second desktop applies speed 2 as 20 visible pixels for 10 mouse units',
                lambda s: _cursor(s, *position), 'gui-second-console.png')
        vm.cursor = len(vm.transcript)
        key('ctrl-q')
        restored('the second desktop exits cleanly')
        vm.keys('<f1>')
        visible('F1 brings the first desktop back, drawn again',
                lambda s: _text(s, font, SURFOS_X, MENU_Y, '\xf7 SurfOS', INK, PAPER) and _pixel(s, 900, 400) == OCEAN,
                'gui-first-again.png')
        vm.cursor = len(vm.transcript)
        key('ctrl-q')
        restored('the first desktop then exits as usual')
        vm.keys('echo gui-keyboard-ok\n')
        visible('physical keyboard remains usable after GUI recovery',
                lambda s: 'gui-keyboard-ok' in s.rows and _prompt(s), 'gui-final.png')
        require('QEMU remains running after GUI input and recovery checks', vm.proc.poll() is None)
    except AssertionError as error:
        if not results or results[-1] != (str(error), False):
            results.append((str(error), False))
        vm.screen('gui-failed.png')
    finally:
        vm.close()
    if helpers and results and all(ok for _, ok in results):
        try:
            for what, ok, text in helpers['fat_checks'](stick, 'stick Notepad wrote'):
                results.append((what, ok))
                if not ok:
                    print(text, flush=True)
            results.append(('the files Notepad saved read back on the host, CRLF and all',
                            helpers['read_fat32'](stick, 'notes.txt') == EDITED and
                            helpers['read_fat32'](stick, 'copy.txt') == EDITED and
                            helpers['read_fat32'](stick, 'Folder/inner.txt') == b'inside\n'))
        except Exception as e:
            results.append((f'the stick can be read back here ({e})', False))
    shutil.rmtree(tmpd, ignore_errors=True)
    return results
