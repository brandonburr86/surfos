#!/usr/bin/env python3
"""Visible desktop integration checks, called by qemu-usb.py's GUI scenario.

Only public keyboard, mouse and shell interfaces drive the guest. Expected
application state is read from framebuffer pixels, not guest memory or hooks.
"""
import os
import re


PAPER = bytes.fromhex('f4f1e6')
INK = bytes.fromhex('153d49')
NAVY = bytes.fromhex('102e3b')
FOAM = bytes.fromhex('b8e9d9')
OCEAN = bytes.fromhex('245a67')
CURSOR = (
    'X', 'XX', 'X.X', 'X..X', 'X...X', 'X....X', 'X.....X',
    'X......X', 'X.......X', 'X........X', 'X.....XXXXX',
    'X..X..X', 'X.X X..X', 'XX  X..X', 'X    XX',
)


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
    return any(re.search(r'(?:SurfOS\*>|[\u224b~][!#@?]?>)\s*$', row) for row in screen.rows)


def run(Vm, PROMPT, args, image, firmware, outdir):
    """Return qemu-usb's list of (description, passed), saving PNG evidence."""
    results = []
    font = _font()
    vm = Vm('gui', args, image, firmware, mem='512', serial=True, outdir=outdir,
            extra=['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080'])
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

    def desktop():
        position[:] = [480, 270]
        vm.send('gui\n')
        return visible('desktop renders the ocean menu bar at 1920x1080',
                       lambda s: (s.w, s.h) == (1920, 1080) and
                       _pixel(s, 0, 0) == PAPER and _pixel(s, 900, 400) == OCEAN and
                       _text(s, font, 16, 6, '~ SurfOS', INK, PAPER) and
                       _text(s, font, 128, 6, 'Apps', INK, PAPER))

    def restored(name):
        require(name + ': shell responds', vm.expect('text console restored', 15) and vm.expect(PROMPT, 15))
        visible(name + ': framebuffer text and margins restored',
                lambda s: _prompt(s) and 'text console restored' in s.text() and s.pixel(0, 0) == b'\0\0\0')

    def display(value, x=688, y=50):
        return lambda s: _text(s, font, x + 28, y + 58, str(value) + '  ', FOAM, INK)

    try:
        require('GUI boot reaches the shell', vm.expect(PROMPT, args.boot_timeout))
        for command in ('gui', 'pointerinfo'):
            vm.send(command + ' extra\n')
            require(command + ' rejects extra arguments with usage',
                    vm.expect('usage: ' + command, 15) and vm.expect(PROMPT, 15))
        vm.send('pointerinfo\n')
        require('native PS/2 mouse is detected before GUI launch',
                vm.expect('PS/2 three-button mouse', 15) and vm.expect(PROMPT, 15))
        desktop()
        vm.screen('gui-desktop.png')

        click(150, 14)
        visible('PS/2 click opens the Apps menu',
                lambda s: _text(s, font, 132, 36, 'Calculator  Ctrl+K', PAPER), 'gui-menu.png')
        click(220, 44)
        visible('Apps menu opens Calculator',
                lambda s: _text(s, font, 700, 56, 'Calculator', PAPER) and display(0)(s))
        for x, y, expected in [(728, 251, 1), (782, 251, 12), (890, 291, 12),
                               (836, 251, 3), (782, 291, 30), (836, 291, 42)]:
            click(x, y)
            visible(f'calculator mouse entry renders {expected}', display(expected))
        vm.screen('gui-calculator-42.png')

        key('ctrl-d')
        visible('Ctrl+D opens Sketch', lambda s: _text(s, font, 276, 70, 'Sketch', PAPER))
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
                lambda s: _text(s, font, 336, 120, 'Sketch', PAPER) and
                _pixel(s, 265, 65) == OCEAN, 'gui-window-moved.png')
        click(740, 128)
        visible('window close control reveals the desktop', lambda s: _pixel(s, 325, 115) == OCEAN)
        key('ctrl-d')
        visible('reopening Sketch retains its moved position and cleared canvas',
                lambda s: _text(s, font, 336, 120, 'Sketch', PAPER) and _pixel(s, 400, 240) == PAPER)
        click(230, 14)
        visible('Options menu opens', lambda s: _text(s, font, 204, 36, 'Pointer & desktop', PAPER))
        click(800, 400)
        visible('clicking outside a menu closes it', lambda s: _pixel(s, 194, 32) == OCEAN)
        key('ctrl-k')
        visible('Ctrl+K restores Calculator with its answer intact', display(42))

        key('ctrl-p')
        visible('Ctrl+P opens pointer settings with one native device',
                lambda s: _text(s, font, 292, 78, 'Pointer & desktop', PAPER) and
                _text(s, font, 296, 116, '1 pointing device connected', INK, PAPER))
        vm.mon.cmd('device_add usb-mouse,id=guimouse,bus=xhci.0')
        visible('USB boot mouse hotplug appears in pointer settings',
                lambda s: _text(s, font, 296, 116, '2 pointing devices connected', INK, PAPER),
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
        click(150, 14)
        visible('native USB mouse movement and click open Apps',
                lambda s: _text(s, font, 132, 36, 'Calculator  Ctrl+K', PAPER))
        click(220, 44)
        visible('native USB mouse selects Calculator', display(42))
        move(860, 64)
        button(True)
        move(870, 74)
        visible('native USB held button drags the window',
                lambda s: _text(s, font, 710, 66, 'Calculator', PAPER))
        vm.mon.cmd('device_del guimouse')
        key('ctrl-p')
        visible('unplugging the held USB mouse removes its device',
                lambda s: _text(s, font, 296, 116, '1 pointing device connected', INK, PAPER), timeout=30)
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
        vm.cursor = len(vm.transcript)
        vm.keys('<f2>')
        visible('F2 revokes graphics and switches to a text console',
                lambda s: _prompt(s) and s.pixel(0, 0) == b'\0\0\0')
        desktop()
        key('ctrl-p')
        visible('the second console can own a new desktop session',
                lambda s: _text(s, font, 292, 78, 'Pointer & desktop', PAPER))
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
        visible('F1 returns to the restored original console',
                lambda s: _prompt(s) and 'text console restored' in s.text() and s.pixel(0, 0) == b'\0\0\0')
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
    return results
