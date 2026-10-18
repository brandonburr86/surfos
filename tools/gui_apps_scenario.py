#!/usr/bin/env python3
"""The suite's applications on the screen (docs/superpowers/plans/2026-10-17-gui-suite.md), for qemu-usb.py's
guiapps scenario: one boot at 1920x1080 (960x540 logical pixels), the desktop, then each track's checks from a
file of its own (gui_apps_term.py, gui_apps_sys.py, gui_apps_tools.py, gui_apps_media.py), so the tracks never
edit the same file. Each track's checks(d) gets a Driver: it drives the guest only through the keyboard and the
mouse and reads the framebuffer's pixels, as gui_scenario.py does, and records results with d.require (which stops
that track's checks at its first failure) or d.check (which does not).
"""
import importlib
import os

import gui_scenario as base

PAPER, INK, NAVY, FOAM, OCEAN, TEAL, MUTED, BAND = (base.PAPER, base.INK, base.NAVY, base.FOAM, base.OCEAN, base.TEAL,
                                                    base.MUTED, base.BAND)
# the suite's windows in the order of shell/guicore.c's table, and where gui_init() puts them at 960x540
APPS = ['Terminal', 'System Monitor', 'Disks', 'Log', 'Clock', 'Image Viewer', 'Hex', 'Character Map']
SIZES = {'Terminal': (640, 400), 'System Monitor': (540, 360), 'Disks': (580, 320), 'Log': (600, 380),
         'Clock': (380, 300), 'Image Viewer': (560, 400), 'Hex': (620, 380), 'Character Map': (460, 360)}
MENU_H, TITLE_H = 28, 28


def window(name):
    """(x, y, w, h) of an application's window when it first opens at 960x540 (gui_init's cascade)"""
    i = APPS.index(name)
    w, h = SIZES[name]
    w, h = min(w, 960 - 48), min(h, 540 - MENU_H - 26 - 20)
    x, y = 48 + i * 24, MENU_H + 12 + i * 14
    return max(0, min(x, 960 - w)), y, w, h


class Driver:
    def __init__(self, vm, font, results, outdir):
        self.vm, self.font, self.results, self.outdir = vm, font, results, outdir
        self.position = [480, 270]

    # results
    def check(self, name, passed):
        self.results.append((name, bool(passed)))
        return bool(passed)

    def require(self, name, passed):
        if not self.check(name, passed):
            raise AssertionError(name)

    # the screen
    def text(self, s, x, y, text, foreground, background=None):
        return base._text(s, self.font, x, y, text, foreground, background)

    def pixel(self, s, x, y):
        return base._pixel(s, x, y)

    def visible(self, name, predicate, save=None, timeout=15):
        screen = self.vm.wait_screen(predicate, timeout, interval=0.1)
        self.require(name, screen is not None)
        if save and self.outdir:
            screen.png(os.path.join(self.outdir, save))
        return screen

    def title(self, name, focused=True):
        """a predicate: the window's title text in its title bar"""
        x, y, _, _ = window(name)
        return lambda s: self.text(s, *base._title(x, y), name, PAPER, TEAL if focused else MUTED)

    # the keyboard and the mouse
    def key(self, chord):
        self.vm.mon.cmd('sendkey ' + chord)

    def keys(self, text):
        self.vm.keys(text)

    def move(self, x, y):
        while self.position != [x, y]:
            dx = max(-100, min(100, x - self.position[0]))
            dy = max(-100, min(100, y - self.position[1]))
            self.vm.mon.cmd(f'mouse_move {dx} {dy}')
            self.position[0] += dx
            self.position[1] += dy
            if self.vm.wait_screen(lambda s: base._cursor(s, *self.position), 10, interval=0.1) is None:
                raise AssertionError(f'visible mouse cursor did not reach {tuple(self.position)}')

    def button(self, held):
        self.vm.mon.cmd(f'mouse_button {1 if held else 0}')
        self.vm.pump(0.05)

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        self.button(False)

    def open_app(self, name):
        """the Apps menu by the keyboard: Ctrl+G, three menus right, down to the application, Enter"""
        self.key('ctrl-g')
        for _ in range(3):
            self.key('right')
        for _ in range(5 + APPS.index(name)):      # Notepad, Files, Calculator, Sketch, Clear sketch, then the suite
            self.key('down')
        self.key('ret')

    def close(self, name):
        """the window's x button (the Terminal keeps Ctrl+W for the shell)"""
        x, y, w, _ = window(name)
        self.click(x + w - 16, y + 14)


def run(Vm, PROMPT, args, image, firmware, outdir, helpers=None):
    results = []
    font = base._font()
    vm = Vm('guiapps', args, image, firmware, mem='512', serial=True, outdir=outdir,
            extra=['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080'])
    d = Driver(vm, font, results, outdir)
    try:
        ok = vm.expect(PROMPT, args.boot_timeout)
        d.check('guiapps boot reaches the shell', ok)
        if not ok:
            return results
        vm.send('gui\n')
        d.require('the desktop draws its menu bar and the suite\'s tiles',
                  d.vm.wait_screen(lambda s: (s.w, s.h) == (1920, 1080) and d.pixel(s, 0, 0) == PAPER and
                                   d.text(s, 24 + 152 + 36, 120 + 14, 'Terminal', FOAM), 15, interval=0.1) is not None)
        for name in ('gui_apps_term', 'gui_apps_sys', 'gui_apps_tools', 'gui_apps_media'):
            try:
                importlib.import_module(name).checks(d)
            except AssertionError:
                pass                                # recorded; the next track's checks still run
        d.key('ctrl-g')
        d.key('ret')                                # SurfOS > About SurfOS, which takes the keys from any window
        vm.send('\x11')                             # Ctrl+Q
        left = vm.expect('text console restored', 20) and vm.expect(PROMPT, 15)
        if not left:                                # a busy application asks first: Leave
            vm.send('\r')
            left = vm.expect('text console restored', 20) and vm.expect(PROMPT, 15)
        d.check('the desktop closes and the text console comes back', left)
    except AssertionError:
        pass
    finally:
        vm.close()
    return results
