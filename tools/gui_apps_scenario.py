#!/usr/bin/env python3
"""The suite's applications on the screen (docs/superpowers/plans/2026-10-17-gui-suite.md), for qemu-usb.py's
guiapps scenario: one boot at 1920x1080 (960x540 logical pixels), the desktop, then each track's checks from a
file of its own (gui_apps_term.py, gui_apps_sys.py, gui_apps_tools.py, gui_apps_media.py), so the tracks never
edit the same file. Each track's checks(d) gets a Driver: it drives the guest only through the keyboard and the
mouse and reads the framebuffer's pixels, as gui_scenario.py does, and records results with d.require (which stops
that track's checks at its first failure) or d.check (which does not). A track's checks leave the desktop as they
found it, with its windows and dialogs closed, also when one fails (try/finally), so the next track's start clean.

A FAT32 data stick (usb1, label APPSTICK, not mounted until a check mounts it) is attached when qemu-usb.py passes
its helpers: its files are the tracks' own, each track module's optional STICK ({'path': bytes}, paths unique
across the tracks). After the guest has stopped, each module's optional readback(d, stick, helpers) reads it on
the host (helpers['read_fat32'](stick, 'path')), and, when every check passed, independent FAT checkers look it over.

10/25/26: gui_apps_win.py, programs' windows (docs/superpowers/plans/2026-10-25-app-windows.md), before the suite's;
its last check leaves the desktop (d.prompt: the serial console's prompt) and starts it again. gui_apps_web.py, the
web browser in a window (docs/superpowers/plans/2026-10-25-browser.md), comes after it, on pages of the stick.
10/17/26 (the integration): gui_apps_suite.py's checks come last: Files opening a picture in the Image Viewer and a
binary file in Hex, with track MEDIA's files. For development, GUIAPPS_ONLY=gui_apps_suite (module names, by commas)
runs only those modules' checks and readbacks; the stick still holds every module's files.
"""
import importlib
import os
import re
import shutil
import tempfile

import gui_scenario as base

PAPER, INK, NAVY, FOAM, OCEAN, TEAL, MUTED, BAND = (base.PAPER, base.INK, base.NAVY, base.FOAM, base.OCEAN, base.TEAL,
                                                    base.MUTED, base.BAND)
# the suite's windows in the order of shell/guicore.c's table, and where gui_init() puts them at 960x540
APPS = ['Terminal', 'System Monitor', 'Disks', 'Log', 'Clock', 'Image Viewer', 'Hex', 'Character Map']
SIZES = {'Terminal': (640, 400), 'System Monitor': (540, 360), 'Disks': (580, 320), 'Log': (600, 380),
         'Clock': (380, 300), 'Image Viewer': (560, 400), 'Hex': (620, 380), 'Character Map': (460, 360)}
MENU_H, TITLE_H = 28, 28
TRACKS = ('gui_apps_term', 'gui_apps_sys', 'gui_apps_tools', 'gui_apps_media', 'gui_apps_win', 'gui_apps_web',
          'gui_apps_suite')
STICK_LABEL, STICK_SERIAL = 'APPSTICK', 0x6A11A995      # a serial of its own: GRUB's bootvol= names the boot stick's


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


def kernel_heap_used(vm, PROMPT):
    """10/25/26 (robustness, B1): the kernel heap's bytes in use, as memstat on the serial line says them, or None. The
    console drops what is typed until a pause after the desktop gives the keyboard back (TTY_SKIP): a quiet second
    first, and memstat once more if its lines did not come"""
    for _ in range(2):
        vm.pump(1)
        start = len(vm.transcript)
        vm.send('memstat\n')
        if vm.expect('kernel heap:', 15) and vm.expect(PROMPT, 30):
            m = re.search(rb'kernel heap: [^\n]*\n\s*used: (\d+) bytes', vm.transcript[start:])
            if m:
                return int(m.group(1))
    return None


def run(Vm, PROMPT, args, image, firmware, outdir, helpers=None):
    results = []
    font = base._font()
    modules = [importlib.import_module(name) for name in TRACKS]
    only = [name for name in os.environ.get('GUIAPPS_ONLY', '').split(',') if name]
    files = {}
    for m in modules:
        for path, data in getattr(m, 'STICK', {}).items():
            if path in files:
                raise ValueError(f'{m.__name__}: {path} is already on the stick')
            files[path] = data
    tmpd = tempfile.mkdtemp(prefix='surfos-guiapps-', dir='/tmp' if os.path.isdir('/tmp') else None)
    stick = os.path.join(tmpd, 'stick.img')
    extra = ['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080']
    if helpers:
        helpers['mkimage'].make_fat32(stick, files, size_mb=64, label=STICK_LABEL, cluster_bytes=512,
                                      serial=STICK_SERIAL)
        extra += ['-drive', f'if=none,id=appstick,format=raw,file={stick}',
                  '-device', 'usb-storage,bus=xhci.0,drive=appstick,removable=on']
    vm = Vm('guiapps', args, image, firmware, mem='512', serial=True, outdir=outdir, extra=extra)
    d = Driver(vm, font, results, outdir)
    d.prompt = PROMPT                               # 10/25/26: the serial console's, for checks that leave the desktop
    try:
        ok = vm.expect(PROMPT, args.boot_timeout)
        d.check('guiapps boot reaches the shell', ok)
        if not ok:
            return results
        # 10/25/26 (B1): the desktop gives back all it took. 10/31/26: counted from two readings that agree: at -O2 the
        # first prompt can come before the boot's last startup work is done, the heap 16-40 KB above where it settles
        heap_before = kernel_heap_used(vm, PROMPT)
        for _ in range(8):
            again = kernel_heap_used(vm, PROMPT)
            if again == heap_before:
                break
            heap_before = again
        vm.send('gui\n')
        d.require('the desktop draws its menu bar and the suite\'s tiles',
                  d.vm.wait_screen(lambda s: (s.w, s.h) == (1920, 1080) and d.pixel(s, 0, 0) == PAPER and
                                   d.text(s, 24 + 152 + 36, 120 + 14, 'Terminal', FOAM), 15, interval=0.1) is not None)
        for m in modules:
            if only and m.__name__ not in only:
                continue
            try:
                m.checks(d)
            except AssertionError as e:             # recorded; the next track's checks still run (each track's
                vm.screen(f'guiapps-{m.__name__[len("gui_apps_"):]}-failed.png')   # leave the desktop as they found
                if not results or results[-1][1]:   # 10/18/26: stopped with no failed check (a move that gave up):
                    results.append((f'{m.__name__}: its checks stopped early ({e})', False))   # one now
        d.key('ctrl-g')
        d.key('ret')                                # SurfOS > About SurfOS, which takes the keys from any window
        vm.send('\x11')                             # Ctrl+Q
        left = vm.expect('text console restored', 20) and vm.expect(PROMPT, 15)
        if not left:                                # a busy application asks first: Leave
            vm.send('\r')
            left = vm.expect('text console restored', 20) and vm.expect(PROMPT, 15)
        d.check('the desktop closes and the text console comes back', left)
        if left:
            heap_after = kernel_heap_used(vm, PROMPT)
            d.check(f'the kernel heap holds what it held before gui ({heap_before} bytes in use, then {heap_after})',
                    heap_before is not None and heap_after == heap_before)
    except AssertionError:
        pass
    finally:
        vm.close()
    if helpers:
        for m in modules:
            if hasattr(m, 'readback') and not (only and m.__name__ not in only):
                try:
                    m.readback(d, stick, helpers)
                except AssertionError:
                    pass
                except Exception as e:
                    results.append((f'{m.__name__}: the stick can be read back here ({e})', False))
        if results and all(ok for _, ok in results):
            try:
                for what, ok, text in helpers['fat_checks'](stick, 'guiapps stick'):
                    results.append((what, ok))
                    if not ok:
                        print(text, flush=True)
            except Exception as e:
                results.append((f'the guiapps stick can be checked here ({e})', False))
    shutil.rmtree(tmpd, ignore_errors=True)
    return results
