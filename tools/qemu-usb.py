#!/usr/bin/env python3
"""Boot the SurfOS USB image the way a PC does, under QEMU + OVMF, and test it.

    tools/qemu-usb.py --image build/usb/surfos-ideapad-15iil05.img --test [--output-dir DIR]
    tools/qemu-usb.py --image IMG --shell help drivers     run commands over the serial port
    tools/qemu-usb.py --image IMG --screen --keys 'help\\n'  type on the keyboard, read the screen

Nothing is passed around the firmware: no -kernel, no -initrd, no IDE disk, no network
card and no boot entry. OVMF starts with a fresh copy of its variable store, finds the
emulated USB stick (a copy of the image on an xHCI controller, usb-storage, removable)
and runs \\EFI\\BOOT\\BOOTX64.EFI from it, the same removable-media path a laptop takes.

The screen is read back without OCR: the framebuffer is captured with the monitor's
screendump and every 8x16 cell is matched against the console font (driver/font8x16.c),
using the same scale and centring as driver/fbcore.c. That is how the keyboard-only boot
(no serial port at all, like the laptop) is driven: keys go in through the emulated
PS/2 keyboard (monitor sendkey), text comes out of the framebuffer.

--test boots the image seven times, each a cold boot with fresh firmware variables:
  serial     512 MB: the portable checks over COM1, then the framebuffer must show the
             same boot messages and the prompt
  keyboard   4 GB, no serial port: typing (Shift, Backspace, history), console switching
             and a set of commands, all through the keyboard and the screen
  highmem    6 GB: memory above 4 GB is counted, and `himem` writes RAM above 4 GB through a
             PSE-36 page; the monitor reads the same physical bytes back
  fallback   the GRUB menu's troubleshooting entry (timer=lapic kbd=poll): the local APIC
             timer ticks and polled keystrokes arrive
  no-pit     a machine without an 8254 (a chipset can gate its clock): the delays are
             calibrated on the ACPI PM timer and the local APIC timer takes over the tick
  no-initrd, no-kernel
             a copy of the image with that file deleted: the boot must stop with a message
             naming it (a visible failure, not a blank screen) and SurfOS must not start
Exit status 0 only when every check passed. Evidence (command lines, serial transcripts,
screenshots as PNG, results.json) goes to --output-dir.
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_C = os.path.join(REPO, 'driver', 'font8x16.c')
PROMPT = 'SurfOS*>'
BAD = ['Kernel Wipeout', "Woah.. this ain't", 'SYSTEM HALTED', 'HALTING', 'Kernel panic', 'BOOT PROBLEM']

# (command, strings that must appear before the next prompt) - over the serial console
PORTABLE = [
    ('help', ['SurfOS ring0 Debug Shell', '(C)2004 Brandon Burr.']),
    ('bootinfo', ['multiboot2', 'profile=laptop', 'firmware UEFI', 'screen: framebuffer']),
    ('drivers', ['parport   skipped', 'ramdisk   ok', 'ata       skipped', 'console   framebuffer', 'keyboard  8042']),
    ('tick', ['Ticks:']),
    ('ps', ['Kernel Idle Task', 'init', 'Shell 0']),
    ('memstat', ['Physical memory:', 'Free physical pages:', 'page tables']),
    ('lsblk', ['rd0']),
    ('mount', ['rd0', '/initrd', 'tarfs']),
    ('cat /initrd/etc/version', ['SurfOS 0.007']),
    ('run hello a b', ['Hello from user mode', 'argv[2] = "b"', 'exit status 42']),
    ('count 3 10', ['counted to 3']),
    ('sleep 300', ['slept 300 ms']),
    ('heaptest', ['HEAPTEST PASS']),
    ('acpi', ['FADT', 'MADT', 'I/O APIC']),
    ('cpuinfo', ['physical address bits', '4 MB pages on']),
    ('selftest', ['skip FAT', 'skip network', 'SELFTEST PASS']),
]
TIMEOUTS = {'selftest': 180, 'heaptest': 60}

# (keys to type, strings that must be on the screen afterwards) - keyboard and framebuffer only
KEYBOARD = [
    ('echo Hello World\n', ['Hello World']),                         # Shift
    ('echo abcx<bs>d\n', ['abcd']),                                  # Backspace
    ('<up>\n', ['abcd']),                                            # history
    ('tick\n', ['Ticks:']),
    ('cat /initrd/etc/version\n', ['SurfOS 0.007']),
    ('run hello a b\n', ['exit status 42']),
    ('count 3 10\n', ['counted to 3']),
    ('drivers\n', ['console   framebuffer', 'serial    none', 'keyboard  8042']),
    ('memstat\n', ['Free physical pages:', 'dma heap']),
    ('help\n', ['(C)2004 Brandon Burr.']),
]


# ---- firmware ----------------------------------------------------------------------------

FIRMWARE_PAIRS = [
    ('/opt/homebrew/share/qemu/edk2-x86_64-code.fd', '/opt/homebrew/share/qemu/edk2-i386-vars.fd'),
    ('/usr/local/share/qemu/edk2-x86_64-code.fd', '/usr/local/share/qemu/edk2-i386-vars.fd'),
    ('/usr/share/OVMF/OVMF_CODE_4M.fd', '/usr/share/OVMF/OVMF_VARS_4M.fd'),
    ('/usr/share/OVMF/OVMF_CODE.fd', '/usr/share/OVMF/OVMF_VARS.fd'),
    ('/usr/share/edk2/ovmf/OVMF_CODE.fd', '/usr/share/edk2/ovmf/OVMF_VARS.fd'),
    ('/usr/share/edk2/x64/OVMF_CODE.4m.fd', '/usr/share/edk2/x64/OVMF_VARS.4m.fd'),
    ('/usr/share/qemu/edk2-x86_64-code.fd', '/usr/share/qemu/edk2-i386-vars.fd'),
]


def find_firmware(code, vars_):
    """a matched OVMF code/variable-store pair without Secure Boot"""
    if code or vars_:
        if not (code and vars_ and os.path.isfile(code) and os.path.isfile(vars_)):
            sys.exit('--ovmf-code and --ovmf-vars must both name existing files')
        return code, vars_
    for c, v in FIRMWARE_PAIRS:
        if os.path.isfile(c) and os.path.isfile(v):
            return c, v
    # QEMU's firmware descriptors, wherever QEMU is installed
    qemu = shutil.which('qemu-system-x86_64')
    dirs = ['/usr/share/qemu/firmware']
    if qemu:
        dirs.append(os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(qemu))), 'share', 'qemu', 'firmware'))
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, '*.json'))):
            try:
                desc = json.load(open(path))
                m = desc['mapping']
                if ('uefi' in desc.get('interface-types', []) and 'secure-boot' not in desc.get('features', [])
                        and any(t.get('architecture') == 'x86_64' for t in desc.get('targets', []))
                        and m.get('device') == 'flash'):
                    c, v = m['executable']['filename'], m['nvram-template']['filename']
                    if os.path.isfile(c) and os.path.isfile(v):
                        return c, v
            except Exception:
                continue
    sys.exit('no OVMF firmware found: install it (Debian: ovmf; macOS: brew install qemu) or pass '
             '--ovmf-code and --ovmf-vars')


# ---- the screen ----------------------------------------------------------------------------

def load_font():
    glyphs = {}
    src = open(FONT_C).read()
    for m in re.finditer(r'\[(\d+)\] = \{ ([^}]*) \}', src):
        glyphs[tuple(int(x, 16) for x in m.group(2).split(','))] = chr(int(m.group(1)))
    glyphs[(0,) * 16] = ' '
    if len(glyphs) < 95:                        # printable ASCII; space is the blank cell
        sys.exit(f'{FONT_C}: could not read the font table')
    return glyphs


GLYPHS = None


class Screen:
    """an 80x25 text screen read back from a framebuffer screenshot (PPM)"""
    def __init__(self, ppm):
        global GLYPHS
        if GLYPHS is None:
            GLYPHS = load_font()
        m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+(\d+)\s', ppm)
        if not m:
            raise ValueError('not a PPM screenshot')
        self.w, self.h = int(m.group(1)), int(m.group(2))
        self.px = ppm[m.end():]
        self.ppm = ppm
        s = max(1, min(self.w // 640, self.h // 400))
        self.scale, self.ox, self.oy = s, (self.w - 640 * s) // 2, (self.h - 400 * s) // 2
        self.cursor = None
        self.rows = [''.join(self.cell(c, r) for c in range(80)).rstrip() for r in range(25)] if self.w >= 640 else []

    def pixel(self, x, y):
        i = (y * self.w + x) * 3
        return self.px[i:i + 3]

    def cell(self, col, row):
        s, x0, y0 = self.scale, self.ox + col * 8 * self.scale, self.oy + row * 16 * self.scale
        grid = [[self.pixel(x0 + gx * s, y0 + gy * s) for gx in range(8)] for gy in range(16)]
        bg = collections.Counter(p for line in grid for p in line).most_common(1)[0][0]
        bits = tuple(sum(0x80 >> gx for gx in range(8) if grid[gy][gx] != bg) for gy in range(16))
        ch = GLYPHS.get(bits)
        if ch is None and bits[14] == 0xFF and bits[15] == 0xFF:      # the underline cursor
            ch = GLYPHS.get(bits[:14] + (0, 0))
            if ch is not None:
                self.cursor = (col, row)
        return ch if ch is not None else '?'

    def text(self):
        return '\n'.join(self.rows)

    def png(self, path):
        raw = b''.join(b'\0' + self.px[y * self.w * 3:(y + 1) * self.w * 3] for y in range(self.h))

        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


# ---- QEMU -------------------------------------------------------------------------------

def strip_ansi(text):
    return re.sub(r'\x1b\[[0-9;?=]*[A-Za-z]', '', text)


class Monitor:
    """a persistent connection to the human monitor"""
    def __init__(self, path, proc):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 15
        while True:
            try:
                self.s.connect(path)
                break
            except OSError:
                if proc.poll() is not None or time.time() > deadline:
                    raise RuntimeError('QEMU did not start')
                time.sleep(0.05)
        self.buf = b''
        self._until_prompt(10)

    def _until_prompt(self, timeout):
        deadline = time.time() + timeout
        self.s.settimeout(0.2)
        while b'(qemu) ' not in self.buf and time.time() < deadline:
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
            except socket.timeout:
                pass
        out, _, self.buf = self.buf.partition(b'(qemu) ')
        return strip_ansi(out.decode(errors='replace'))

    def cmd(self, line, timeout=15):
        self.s.sendall(line.encode() + b'\n')
        return self._until_prompt(timeout)


KEYMAP = {c: c for c in 'abcdefghijklmnopqrstuvwxyz0123456789'}
KEYMAP.update({' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', ',': 'comma', ';': 'semicolon',
               '=': 'equal', '\\': 'backslash', "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right',
               '`': 'grave_accent'})
SHIFTED = dict(zip('!@#$%^&*()_+:"<>?{}|~', ['1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'minus', 'equal',
                                              'semicolon', 'apostrophe', 'comma', 'dot', 'slash', 'bracket_left',
                                              'bracket_right', 'backslash', 'grave_accent']))
NAMED = {'esc': 'esc', 'bs': 'backspace', 'tab': 'tab', 'up': 'up', 'down': 'down', 'left': 'left',
         'right': 'right', 'del': 'delete', 'home': 'home', 'end': 'end', 'ret': 'ret'}
NAMED.update({f'f{i}': f'f{i}' for i in range(1, 13)})


class Vm:
    def __init__(self, name, args, image, firmware, mem='512', serial=True, outdir=None, machine='q35'):
        self.name, self.args, self.serial_on = name, args, serial
        base = '/tmp' if os.path.isdir('/tmp') else tempfile.gettempdir()      # short: UNIX socket paths
        self.work = tempfile.mkdtemp(prefix='surfos-usb-', dir=base)
        self.outdir = outdir
        code, vars_ = firmware
        shutil.copyfile(image, os.path.join(self.work, 'usb.img'))             # the guest gets a copy
        shutil.copyfile(vars_, os.path.join(self.work, 'vars.fd'))             # fresh firmware variables
        self.mon_path = os.path.join(self.work, 'mon.sock')
        ser_path = os.path.join(self.work, 'ser.sock')
        cmd = [args.qemu, '-machine', machine, '-m', str(mem), '-smp', '2',
               '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={code}',
               '-drive', 'if=pflash,format=raw,unit=1,file=vars.fd',
               '-device', 'qemu-xhci,id=xhci',
               '-drive', 'if=none,id=usbstick,format=raw,file=usb.img',
               '-device', 'usb-storage,bus=xhci.0,drive=usbstick,removable=on',
               '-nic', 'none', '-vga', 'std', '-display', 'none', '-no-reboot',
               '-monitor', f'unix:{self.mon_path},server,nowait']
        cmd += (['-chardev', f'socket,id=ser0,path={ser_path},server=on,wait=off', '-serial', 'chardev:ser0']
                if serial else ['-serial', 'none'])
        cmd += args.extra.split()
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, cwd=self.work, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.mon = Monitor(self.mon_path, self.proc)
        self.ser, self.transcript, self.cursor = None, b'', 0
        if serial:
            self.ser = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.ser.connect(ser_path)
            self.ser.setblocking(False)
        self.shots = 0

    # serial
    def pump(self, timeout=0.1):
        if not self.ser:
            time.sleep(timeout)
            return
        import select
        r, _, _ = select.select([self.ser], [], [], timeout)
        if r:
            try:
                d = self.ser.recv(65536)
                if d:
                    self.transcript += d
            except BlockingIOError:
                pass

    def text(self):
        return strip_ansi(self.transcript.decode(errors='replace').replace('\r', ''))

    def expect(self, needle, timeout):
        n = needle.encode()
        deadline = time.time() + timeout
        while True:
            i = self.transcript.find(n, self.cursor)
            if i >= 0:
                self.cursor = i + len(n)
                return True
            if time.time() > deadline or self.proc.poll() is not None:
                return False
            self.pump(0.1)

    def send(self, text):
        self.ser.sendall(text.encode())

    # keyboard and screen
    def keys(self, spec):
        for tok in re.findall(r'<[^>]+>|.', spec.encode().decode('unicode_escape'), re.S):
            if tok.startswith('<'):
                key = NAMED.get(tok[1:-1].lower())
            elif tok in SHIFTED:
                key = 'shift-' + SHIFTED[tok]
            elif tok.isupper():
                key = 'shift-' + tok.lower()
            else:
                key = KEYMAP.get(tok)
            if not key:
                raise ValueError(f'no key for {tok!r}')
            self.mon.cmd('sendkey ' + key)
            self.pump(0.03)

    def screen(self, save=None):
        path = os.path.join(self.work, 'screen.ppm')
        if os.path.exists(path):
            os.remove(path)
        self.mon.cmd('screendump screen.ppm')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) > 15:
                break
            time.sleep(0.1)
        time.sleep(0.1)
        scr = Screen(open(path, 'rb').read())
        if save and self.outdir:
            scr.png(os.path.join(self.outdir, save))
        return scr

    def wait_screen(self, test, timeout, interval=1.0):
        deadline = time.time() + timeout
        while True:
            scr = self.screen()
            if test(scr):
                return scr
            if time.time() > deadline or self.proc.poll() is not None:
                return None
            self.pump(interval)

    def xp_bytes(self, phys, n):
        out = self.mon.cmd(f'xp /{n}xb 0x{phys:x}')
        return bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})\b', out.split(':', 1)[-1]))

    def close(self):
        if self.ser:
            self.pump(0.3)
        try:
            self.mon.cmd('quit', 2)
        except Exception:
            pass
        try:
            self.proc.wait(5)
        except Exception:
            self.proc.kill()
        if self.outdir:
            with open(os.path.join(self.outdir, 'qemu-command.txt'), 'w') as f:
                f.write(' '.join(self.cmd) + '\n')
            if self.ser:
                with open(os.path.join(self.outdir, 'serial.txt'), 'w') as f:
                    f.write(self.text())
        shutil.rmtree(self.work, ignore_errors=True)


def prompt_on(scr):
    return any(r.rstrip().endswith(PROMPT) for r in scr.rows)


def last_prompt_row(scr):
    rows = [i for i, r in enumerate(scr.rows) if r.rstrip().endswith(PROMPT)]
    return rows[-1] if rows else -1


# ---- scenarios -----------------------------------------------------------------------------

def boot_serial(vm, results, timeout):
    ok = vm.expect(PROMPT, timeout)
    results.append(('UEFI boot from the USB image to the shell prompt (serial)', ok))
    return ok


def run_portable(vm, results):
    for cmd, expects in PORTABLE:
        limit = TIMEOUTS.get(cmd.split()[0], 20)
        vm.send(cmd + '\n')
        ok = all(vm.expect(e, limit) for e in expects) and vm.expect(PROMPT, limit)
        results.append((cmd, ok))


def read_ticks(vm):
    start = len(vm.transcript)
    vm.send('tick\n')
    if not (vm.expect('Ticks:', 20) and vm.expect(PROMPT, 20)):
        return None
    m = re.search(rb'Ticks: (\d+)', vm.transcript[start:])
    return int(m.group(1)) if m else None


def tick_rate_ok(vm, seconds=3.0):
    """the tick against the wall clock: generous bounds, an emulated CPU is not exact"""
    t0, a = time.time(), read_ticks(vm)
    time.sleep(seconds)
    b, t1 = read_ticks(vm), time.time()
    if a is None or b is None:
        return False
    rate = (b - a) / (t1 - t0)
    print(f'      ({b - a} ticks in {t1 - t0:.1f} s: {rate:.0f} Hz)', flush=True)
    return 50 <= rate <= 150


def kill_while_drawing(vm):
    """A task spends most of its time drawing when it prints nonstop; killing it must not leave the
    framebuffer owned by a dead task (the screen would never change again)."""
    start = len(vm.transcript)
    vm.send('spawn count 100000 0\n')
    if not vm.expect('started]', 20):
        return False
    m = re.search(rb'\[pid (\d+) started\]', vm.transcript[start:])
    if not m:
        return False
    time.sleep(2)                                                    # let it scroll hard
    vm.send(f'kill {int(m.group(1))}\n')
    if not (vm.expect('killing', 20) and vm.expect(PROMPT, 20)):
        return False
    vm.pump(1)
    vm.send('echo SCREEN-STILL-ALIVE\n')
    if not (vm.expect('SCREEN-STILL-ALIVE', 20) and vm.expect(PROMPT, 20)):
        return False
    time.sleep(1)
    return 'SCREEN-STILL-ALIVE' in vm.screen('after-kill.png').text()


def check_bad(vm, results, text):
    for bad in BAD:
        results.append((f'no "{bad}"', bad not in text))
    results.append(('QEMU still running', vm.proc.poll() is None))


def scenario_serial(args, image, firmware, outdir):
    results = []
    vm = Vm('serial', args, image, firmware, mem='512', serial=True, outdir=outdir)
    try:
        if boot_serial(vm, results, args.boot_timeout):
            vm.pump(1)
            scr = vm.screen('boot.png')
            serial_boot = vm.text()[:vm.text().find(PROMPT) + len(PROMPT)]
            results.append(('framebuffer shows the shell prompt', prompt_on(scr)))
            # the last boot lines on the serial port must be the last ones on the screen too
            want = [l.strip() for l in serial_boot.splitlines() if l.strip()][-6:]
            shown = [r.strip() for r in scr.rows if r.strip()]
            results.append(('framebuffer and serial agree on the last boot stages', shown[-len(want):] == want))
            results.append(('framebuffer shows the banner', any("Phrite's SurfOS" in r for r in scr.rows)))
            results.append(('GRUB found its own partition through $cmdpath, no disk search',
                            'searching all disks' not in serial_boot and 'Booting `SurfOS' in serial_boot))
            run_portable(vm, results)
            results.append(('the PIT tick runs at about 100 Hz', tick_rate_ok(vm)))
            vm.screen('after-checks.png')
            results.append(('a program killed while it scrolls the screen does not freeze it', kill_while_drawing(vm)))
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    return results


def scenario_keyboard(args, image, firmware, outdir):
    results = []
    vm = Vm('keyboard', args, image, firmware, mem='4096', serial=False, outdir=outdir)
    try:
        scr = vm.wait_screen(prompt_on, args.boot_timeout, interval=2.0)
        results.append(('UEFI boot to the prompt with no serial port (framebuffer)', scr is not None))
        if scr is None:
            vm.screen('stuck.png')
            return results
        vm.screen('boot.png')
        for keys, expects in KEYBOARD:
            before = vm.screen()
            vm.keys(keys)

            def done(s):
                text = s.text()
                return all(e in text for e in expects) and last_prompt_row(s) >= 0 and s.rows[last_prompt_row(s)].rstrip().endswith(PROMPT) and text != before.text()
            got = vm.wait_screen(done, 60, interval=0.5)
            results.append((f'keyboard: {keys.strip()!r} shows {expects}', got is not None))
        # console switching: F2 shows another shell, F1 comes back
        vm.keys('<f2>')
        two = vm.wait_screen(lambda s: prompt_on(s) and 'Hello World' not in s.text(), 20, interval=0.5)
        results.append(('F2 switches to console 2 (its own shell)', two is not None))
        vm.screen('console2.png')
        vm.keys('<f1>')
        one = vm.wait_screen(lambda s: '(C)2004 Brandon Burr.' in s.text(), 20, interval=0.5)
        results.append(('F1 switches back to console 1 with its screen', one is not None))
        vm.screen('console1.png')
        results.append(('QEMU still running', vm.proc.poll() is None))
        final = vm.screen('final.png').text()
        for bad in BAD:
            results.append((f'no "{bad}" on the screen', bad not in final))
    finally:
        vm.close()
    return results


def scenario_highmem(args, image, firmware, outdir):
    results = []
    vm = Vm('highmem', args, image, firmware, mem='6144', serial=True, outdir=outdir)
    try:
        if boot_serial(vm, results, args.boot_timeout):
            vm.send('memstat\n')
            ok = vm.expect('above 4 GB, not used', 20) and vm.expect(PROMPT, 20)
            results.append(('memstat counts the RAM above 4 GB (64-bit accounting)', ok))
            start = len(vm.transcript)
            vm.send('himem\n')
            ok = vm.expect('himem: wrote', 20) and vm.expect(PROMPT, 20)
            line = vm.transcript[start:].decode(errors='replace')
            m = re.search(r'wrote "([^"]+)" at physical 0x([0-9a-f]+), read back "([^"]+)": ok', line)
            results.append(('himem: written and read back through a PSE-36 4 MB page', bool(ok and m)))
            if m:
                sig, phys = m.group(1), int(m.group(2), 16)
                got = vm.xp_bytes(phys, len(sig))
                results.append((f'the monitor finds "{sig}" at physical 0x{phys:x} (above 4 GB)',
                                phys >= 1 << 32 and got == sig.encode()))
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    return results


def scenario_fallback(args, image, firmware, outdir):
    results = []
    vm = Vm('fallback', args, image, firmware, mem='512', serial=True, outdir=outdir)
    try:
        ok = vm.expect('Use the', args.boot_timeout)               # the GRUB menu on the firmware console
        results.append(('the GRUB menu appears', ok))
        if ok:
            time.sleep(0.5)
            vm.keys('<down>')
            vm.keys('\n')
            ok = vm.expect('timer=lapic kbd=poll', 30) and vm.expect(PROMPT, args.boot_timeout)
            results.append(('the troubleshooting entry boots to the prompt', ok))
        if ok:
            vm.send('drivers\n')
            ok = (vm.expect('local APIC timer', 20) and vm.expect('keyboard  8042 (scan code set 1), polled', 20)
                  and vm.expect(PROMPT, 20))
            results.append(('drivers: local APIC timer, keyboard polled without IRQ 1', ok))
            vm.send('sleep 500\n')
            results.append(('sleep 500 wakes up on the local APIC tick', vm.expect('slept 500 ms', 20)
                            and vm.expect(PROMPT, 20)))
            results.append(('the local APIC tick runs at about 100 Hz', tick_rate_ok(vm)))
            vm.keys('tick\n')                                        # through the polled keyboard
            results.append(('typing reaches the shell through the polled 8042', vm.expect('Ticks:', 20)
                            and vm.expect(PROMPT, 20)))
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    return results


def scenario_nopit(args, image, firmware, outdir):
    """no 8254 at all (as when a chipset gates its clock): another clock calibrates, the APIC timer ticks"""
    results = []
    vm = Vm('no-pit', args, image, firmware, mem='512', serial=True, outdir=outdir, machine='q35,pit=off')
    try:
        if boot_serial(vm, results, args.boot_timeout):
            boot = vm.text()
            results.append(('delays calibrated on the ACPI PM timer or HPET, not the missing PIT',
                            'measured on the ACPI PM timer' in boot or 'measured on the HPET' in boot))
            results.append(('the PIT tick is reported missing', 'no tick from the PIT channel 0' in boot))
            vm.send('drivers\n')
            results.append(('drivers: the local APIC timer took over', vm.expect('timer     local APIC timer', 20)
                            and vm.expect(PROMPT, 20)))
            results.append(('the local APIC tick runs at about 100 Hz', tick_rate_ok(vm)))
            vm.send('run hello a b\n')
            results.append(('a user program runs', vm.expect('exit status 42', 20) and vm.expect(PROMPT, 20)))
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    return results


def load_mkusb():
    import importlib.util
    spec = importlib.util.spec_from_file_location('mkusb', os.path.join(REPO, 'tools', 'mkusb.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def scenario_missing(args, image, firmware, outdir, path, message):
    """a stick that lost a file must stop with a message saying so, not a blank screen or a hang"""
    results = []
    mk = load_mkusb()
    tmpd = tempfile.mkdtemp(prefix='surfos-usb-broken-', dir='/tmp' if os.path.isdir('/tmp') else None)
    try:
        broken = os.path.join(tmpd, 'broken.img')
        shutil.copyfile(image, broken)
        with open(broken, 'r+b') as f:
            part = mk.read_gpt(f, os.path.getsize(broken) // 512)
            mk.Fat32(f, part['first_lba'] * 512).remove(path)
        vm = Vm('missing', args, broken, firmware, mem='512', serial=True, outdir=outdir)
        try:
            ok = vm.expect(message, args.boot_timeout)
            results.append((f'without {path} the boot stops with "{message}"', ok))
            vm.pump(3)
            results.append(('and SurfOS does not start', PROMPT not in vm.text()))
            vm.screen('grub-error.png')
        finally:
            vm.close()
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return results


SCENARIOS = {'serial': scenario_serial, 'keyboard': scenario_keyboard, 'highmem': scenario_highmem,
             'fallback': scenario_fallback, 'no-pit': scenario_nopit,
             # GRUB's own error; without the kernel the bootstrap cannot recognise the stick and says that
             'no-initrd': lambda a, i, f, o: scenario_missing(a, i, f, o, '/boot/initrd.tar',
                                                             "file `/boot/initrd.tar' not found"),
             'no-kernel': lambda a, i, f, o: scenario_missing(a, i, f, o, '/boot/surfos.bin',
                                                             'SurfOS: /boot/surfos.bin and /boot/grub/grub.cfg were not found')}


def qemu_version(qemu):
    try:
        return subprocess.run([qemu, '--version'], stdout=subprocess.PIPE, text=True).stdout.splitlines()[0]
    except Exception:
        return 'unknown'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', required=True, help='the raw USB image (build/usb/surfos-ideapad-15iil05.img)')
    ap.add_argument('--ovmf-code', help='OVMF code (read-only flash)')
    ap.add_argument('--ovmf-vars', help='OVMF variable store template (copied fresh for every boot)')
    ap.add_argument('--output-dir', help='where to keep transcripts, screenshots and results.json')
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--extra', default='', help='extra QEMU arguments')
    ap.add_argument('--boot-timeout', type=float, default=240)
    ap.add_argument('--mem', default='512')
    ap.add_argument('--scenario', action='append', choices=sorted(SCENARIOS), help='with --test: only these')
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--test', action='store_true', help='the boot tests (see above)')
    mode.add_argument('--shell', nargs='+', metavar='CMD', help='boot with a serial port and run these commands')
    mode.add_argument('--screen', action='store_true', help='boot without a serial port and print the screen')
    mode.add_argument('--run', action='store_true', help='boot in a QEMU window to try it by hand (serial on '
                      'this terminal); the image itself is not changed')
    ap.add_argument('--keys', default='', help='with --screen: keys to type at the prompt first')
    a = ap.parse_args()

    if not os.path.isfile(a.image):
        sys.exit(f'{a.image}: no such image (make usb-image first)')
    if not shutil.which(a.qemu):
        sys.exit(f'{a.qemu} is not installed')
    firmware = find_firmware(a.ovmf_code, a.ovmf_vars)
    outdir = os.path.abspath(a.output_dir) if a.output_dir else None
    if outdir:
        os.makedirs(outdir, exist_ok=True)

    if a.run:
        work = tempfile.mkdtemp(prefix='surfos-usb-run-')
        try:
            shutil.copyfile(firmware[1], os.path.join(work, 'vars.fd'))
            shutil.copyfile(a.image, os.path.join(work, 'usb.img'))
            cmd = [a.qemu, '-machine', 'q35', '-m', a.mem, '-smp', '2',
                   '-drive', f'if=pflash,format=raw,unit=0,readonly=on,file={firmware[0]}',
                   '-drive', 'if=pflash,format=raw,unit=1,file=vars.fd',
                   '-device', 'qemu-xhci,id=xhci', '-drive', 'if=none,id=usbstick,format=raw,file=usb.img',
                   '-device', 'usb-storage,bus=xhci.0,drive=usbstick,removable=on',
                   '-nic', 'none', '-vga', 'std', '-serial', 'stdio'] + a.extra.split()
            print('SurfOS from a copy of', a.image, '- close the window to stop', flush=True)
            return subprocess.call(cmd, cwd=work)
        finally:
            shutil.rmtree(work, ignore_errors=True)

    if a.shell:
        vm = Vm('shell', a, a.image, firmware, mem=a.mem, serial=True, outdir=outdir)
        try:
            if not vm.expect(PROMPT, a.boot_timeout):
                print(vm.text())
                print('\n(no shell prompt on the serial port)')
                return 1
            for cmd in a.shell:
                vm.send(cmd + '\n')
                if not vm.expect(PROMPT, 60):
                    break
            vm.pump(0.5)
            print(vm.text())
            return 0
        finally:
            vm.close()

    if a.screen:
        vm = Vm('screen', a, a.image, firmware, mem=a.mem, serial=False, outdir=outdir)
        try:
            scr = vm.wait_screen(prompt_on, a.boot_timeout, interval=2.0)
            if scr and a.keys:
                vm.keys(a.keys)
                time.sleep(2)
            scr = vm.screen('screen.png')
            print(scr.text())
            print(f'\n({scr.w}x{scr.h}, text scale {scr.scale}, cursor {scr.cursor})')
            return 0 if scr and prompt_on(scr) else 1
        finally:
            vm.close()

    summary = {'image': os.path.abspath(a.image), 'image_sha256': sha256(a.image), 'qemu': qemu_version(a.qemu),
               'ovmf_code': firmware[0], 'ovmf_code_sha256': sha256(firmware[0]), 'ovmf_vars': firmware[1],
               'scenarios': {}}
    failed = 0
    for name in a.scenario or ['serial', 'keyboard', 'highmem', 'fallback', 'no-pit', 'no-initrd', 'no-kernel']:
        sdir = os.path.join(outdir, name) if outdir else None
        if sdir:
            os.makedirs(sdir, exist_ok=True)
        t0 = time.time()
        print(f'== {name}', flush=True)
        results = SCENARIOS[name](a, a.image, firmware, sdir)
        for what, ok in results:
            print(('PASS ' if ok else 'FAIL ') + what, flush=True)
        failed += sum(1 for _, ok in results if not ok)
        summary['scenarios'][name] = {'seconds': round(time.time() - t0, 1),
                                      'checks': [{'check': w, 'passed': bool(o)} for w, o in results]}
    total = sum(len(s['checks']) for s in summary['scenarios'].values())
    summary['passed'] = failed == 0
    if outdir:
        with open(os.path.join(outdir, 'results.json'), 'w') as f:
            json.dump(summary, f, indent=2)
    print(f'\nUSB/UEFI TEST {"PASSED" if not failed else "FAILED"}: {total - failed} of {total} checks'
          f'{" (evidence in " + outdir + ")" if outdir else ""}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
