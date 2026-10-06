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
using the same scale, grid and margins as driver/fbcore.c (1280x800: 80x25 at scale 2;
1920x1080: 120x33). That is how the keyboard-only boot (no serial port at all, like the
laptop) is driven: keys go in through the emulated PS/2 keyboard (monitor sendkey), text
comes out of the framebuffer.

--test boots the image twelve times, each a cold boot with fresh firmware variables:
  serial     512 MB: the portable checks over COM1, then the framebuffer must show the
             same boot messages and the prompt
  keyboard   4 GB, no serial port: typing (Shift, Backspace, history), raw mode, console
             switching, a set of commands and the escape sequences of `ansi`, all through
             the keyboard and the screen
  fullhd     the IdeaPad's panel size: QEMU's VGA offers 1920x1080 in its EDID and OVMF
             hands that mode over. The console fills it with 120x33 cells at scale 2: a long
             line on one row, Backspace back across a typed line that wrapped, `ansi` with
             its corner at (119,32), console switching, a line of UTF-8 read back through the
             code page 437 font (▸ ≋ … café), the pager answered on the keyboard
  highmem    6 GB: memory above 4 GB is counted, and `himem` writes RAM above 4 GB through a
             PSE-36 page; the monitor reads the same physical bytes back
  fallback   the GRUB menu's troubleshooting entry (timer=lapic kbd=poll): the local APIC
             timer ticks and polled keystrokes arrive
  kernel-shell
             the GRUB menu's kernel shell entry (shell=kernel): the kernel's ring-0 shell on
             every console instead of the user shell, its prompt elevated (#), no recovery line
  no-pit     a machine without an 8254 (a chipset can gate its clock): the delays are
             calibrated on the ACPI PM timer and the local APIC timer takes over the tick
  usbnet     a USB Ethernet adapter (QEMU's usb-net: CDC ECM, full speed) on the xHCI
             controller next to the stick: eth0 at boot, DHCP, ping, HTTP from a server on
             this host (a small page and 1 MB), the self test's network checks, then the
             adapter unplugged and plugged in again through the monitor, and finally a
             different adapter on another network (192.168.76.0/24) in its place
  usbdisk    a second emulated USB stick (a copy of the QEMU test disk: MBR + FAT16) next to the
             boot stick: it becomes usb1, is mounted by hand, read and written, unplugged while
             mounted (I/O errors, no crash, umount still works), plugged in again, and what was
             written is still there (and in the image file, checked here afterwards)
  storage    the storage work end to end, with two more sticks: a blank 1 GB one is
             partitioned (fdisk), formatted FAT32 (mkfs), mounted, used through the C library
             (fstest), sh redirection, mv and the editor, then ejected; a stick made by another
             computer (mkfs.fat + mtools: long, accented and macOS-style names, folders, 1 MB) is
             read and written; the boot drive is refused. Afterwards other FAT implementations
             check both images here: macOS's fsck_msdos (hdiutil, without mounting), fsck.fat
             (on the host or in the devenv container) and a FAT32 reader reading the files back
  no-initrd, no-kernel
             a copy of the image with that file deleted: the boot must stop with a message
             naming it (a visible failure, not a blank screen) and SurfOS must not start
Exit status 0 only when every check passed. Evidence (command lines, serial transcripts,
screenshots as PNG, results.json) goes to --output-dir.
"""
import argparse
import collections
import http.server
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
import threading
import time
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_C = os.path.join(REPO, 'driver', 'font8x16.c')
# The shell's prompt: the ring-0 shell's "SurfOS*> " of old, or the SurfOS prompt (10/6/26,
# shell/prompt.c): breadcrumbs, the wave (~ in ASCII), a state character, "> ", with colour
# sequences in between when the shell colours it. The space after ">" is not required: a
# console repainted on the serial terminal (F1-F4) ends its rows at their last character. Vm.expect() takes the compiled pattern;
# PROMPT_TEXT finds it in text without escape sequences, PROMPT_ROW at the end of a screen row.
_SGR = rb'(?:\x1b\[[0-9;]*m)*'
PROMPT = re.compile(rb'SurfOS\*>|(?:\xe2\x89\x8b|~)' + _SGR + rb'[!#@?]?' + _SGR + rb'>')
PROMPT_TEXT = re.compile(r'SurfOS\*>|(?:\u224b|~)[!#@?]?>')
# 10/6/26: on a screen whose console does not draw UTF-8 yet the wave is not drawn, and the ring-0 shell's prompt
# ends in its state alone ("SURF #>")
PROMPT_ROW = re.compile(r'(?:SurfOS\*>|(?:\u224b|~)[!#@?]?>| [!#@?]>)$')
# the pager's question after a screenful (TTY_MORE in include/surfos/tty.h): answered with Space
MORE = b'--More--'
BAD = ['Kernel Wipeout', "Woah.. this ain't", 'SYSTEM HALTED', 'HALTING', 'Kernel panic', 'BOOT PROBLEM']

# (command, strings that must appear before the next prompt) - over the serial console
PORTABLE = [
    ('help', ['SurfOS kernel shell 1.0', '(C)2004-2026 Brandon Burr.']),
    ('bootinfo', ['multiboot2', 'profile=laptop', 'firmware UEFI', 'screen: framebuffer']),
    ('drivers', ['parport   skipped', 'ramdisk   ok', 'ata       skipped', 'usb       ok', 'console   framebuffer',
                 'keyboard  8042', 'usb       1 xHCI controller']),
    ('lsusb', ['xHCI 1.00', 'QEMU USB HARDDRIVE', '-> usb0 (256 MB)']),   # the stick, at 5 Gbit/s, as a disk
    ('tick', ['Ticks:']),
    ('ps', ['Kernel Idle Task', 'init', 'Shell 0']),
    ('memstat', ['Physical memory:', 'Free physical pages:', 'page tables']),
    ('lsblk', ['rd0']),
    ('mount', ['rd0', '/initrd', 'tarfs']),
    ('cat /initrd/etc/version', ['SurfOS 1.0 (ai-dev)']),
    ('run hello a b', ['Hello from user mode', 'argv[2] = "b"', 'exit status 42']),
    ('count 3 10', ['counted to 3']),
    ('sleep 300', ['slept 300 ms']),
    ('heaptest', ['HEAPTEST PASS']),
    ('acpi', ['FADT', 'MADT', 'I/O APIC']),
    ('cpuinfo', ['physical address bits', '4 MB pages on']),
    ('selftest', ['skip FAT', 'skip network', 'ok   USB: endpoint 0 works again after a stalled request', 'SELFTEST PASS']),
]
TIMEOUTS = {'selftest': 180, 'heaptest': 60}

# (keys to type, strings that must be on the screen afterwards) - keyboard and framebuffer only
KEYBOARD = [
    ('echo Hello World\n', ['Hello World']),                         # Shift
    ('echo abcx<bs>d\n', ['abcd']),                                  # Backspace
    ('<up>\n', ['abcd']),                                            # history
    ('tick\n', ['Ticks:']),
    ('cat /initrd/etc/version\n', ['SurfOS 1.0 (ai-dev)']),
    ('run hello a b\n', ['exit status 42']),
    ('count 3 10\n', ['counted to 3']),
    ('drivers\n', ['console   framebuffer', 'serial    none', 'keyboard  8042']),
    ('memstat\n', ['Free physical pages:', 'dma heap']),
    ('help\n', ['(C)2004-2026 Brandon Burr.']),
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
    """{glyph rows: character} from the console font. 10/6/26: the font holds code page 437
    (tools/bdf2c.py), and each entry's comment names its code point (/* U+25B8 ▸ */), which
    is the character a cell with that glyph shows. The blank glyphs (the empty cell, space,
    no-break space) read as a space; were two characters to share a glyph, ASCII would win,
    then the lower index."""
    glyphs = {}
    src = open(FONT_C, encoding='utf-8').read()
    entries = re.findall(r'\[0x([0-9a-fA-F]{2})\] = \{ ([^}]*) \}, /\* U\+([0-9A-Fa-f]{4,6})', src)
    if len(entries) != 256:
        sys.exit(f'{FONT_C}: could not read the font table ({len(entries)} of 256 entries)')
    for index, rows, code in sorted(entries, key=lambda e: (not 0x20 <= int(e[0], 16) <= 0x7e, int(e[0], 16))):
        glyphs.setdefault(tuple(int(x, 16) for x in rows.split(',')), chr(int(code, 16)))
    glyphs[(0,) * 16] = ' '
    return glyphs


GLYPHS = None


FB_MAX_COLS, FB_MAX_ROWS = 240, 100            # include/sys/framebuffer.h


class Screen:
    """the console's text grid read back from a framebuffer screenshot (PPM)"""
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
        # fb_setup(): the scale at which 80x25 cells fit, then as many cells as the mode holds
        s = max(1, min(self.w // 640, self.h // 400))
        self.scale = s
        self.cols, self.nrows = min(self.w // (8 * s), FB_MAX_COLS), min(self.h // (16 * s), FB_MAX_ROWS)
        self.ox, self.oy = (self.w - self.cols * 8 * s) // 2, (self.h - self.nrows * 16 * s) // 2
        self.cursor = None
        self.colours = {}                       # (col, row): (foreground, background) as RGB bytes
        self.rows = ([''.join(self.cell(c, r) for c in range(self.cols)).rstrip() for r in range(self.nrows)]
                     if self.w >= 640 else [])

    def pixel(self, x, y):
        i = (y * self.w + x) * 3
        return self.px[i:i + 3]

    def cell(self, col, row):
        s, x0, y0 = self.scale, self.ox + col * 8 * self.scale, self.oy + row * 16 * self.scale
        grid = [[self.pixel(x0 + gx * s, y0 + gy * s) for gx in range(8)] for gy in range(16)]
        counts = collections.Counter(p for line in grid for p in line).most_common(2)
        bg = counts[0][0]
        self.colours[(col, row)] = (counts[1][0] if len(counts) > 1 else None, bg)
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
    def __init__(self, name, args, image, firmware, mem='512', serial=True, outdir=None, machine='q35', extra=()):
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
        cmd += list(extra) + args.extra.split()
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, cwd=self.work, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.mon = Monitor(self.mon_path, self.proc)
        self.ser, self.transcript, self.cursor = None, b'', 0
        self.more_at, self.auto_more = 0, True
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
                    self.answer_more()
            except BlockingIOError:
                pass

    def answer_more(self):
        """the pager waits for a key after a screenful: Space, unless a test turned this off"""
        while self.auto_more:
            i = self.transcript.find(MORE, self.more_at)
            if i < 0:
                return
            self.more_at = i + len(MORE)
            self.ser.sendall(b' ')

    def text(self):
        return strip_ansi(self.transcript.decode(errors='replace').replace('\r', ''))

    def expect(self, needle, timeout):
        """wait for needle (a string, or a compiled bytes pattern such as PROMPT) after the last match"""
        deadline = time.time() + timeout
        while True:
            if isinstance(needle, re.Pattern):
                m = needle.search(self.transcript, self.cursor)
                if m:
                    self.cursor = m.end()
                    return True
            else:
                n = needle.encode()
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


# what the shell command `ansi` draws with escape sequences (row: text); the last line and the
# corner depend on the grid
ANSI_ROWS = {0: 'ANSI-TEST top left', 2: '         INVERSE normal', 3: '         YELLOW-ON-BLUE', 4: '         KEEP',
             6: '         D C   E', 7: '         A', 8: '          B', 10: '         UNKNOWN-IGNORED'}
GREY, BLACK, BLUE, YELLOW = bytes((170, 170, 170)), bytes((0, 0, 0)), bytes((0, 0, 170)), bytes((255, 255, 85))
# 10/6/26: what `pagetest utf8` prints, as the console draws it (code page 437: no euro sign)
UTF8_SAMPLE = 'UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ ? END'
LRED, ORANGE = bytes((255, 85, 85)), bytes((170, 85, 0))


def version_h(name):
    """a string of include/surfos/version.h (10/6/26), which the banner prints"""
    m = re.search(r'#define\s+' + name + r'\s+"([^"]*)"', open(os.path.join(REPO, 'include', 'surfos', 'version.h')).read())
    return m.group(1) if m else '?'


def banner_ok(scr):
    """the banner of kernel/sys.c (10/6/26): "Brandon Burr's SurfOS" in light red, then "Version 1.0 on an x86,
    2004-2026" in light red with the version and the years in orange"""
    if scr is None:
        return False
    rows = [i for i, r in enumerate(scr.rows) if r == f"{version_h('SURFOS_AUTHOR')}'s {version_h('SURFOS_NAME')}"]
    if not rows or rows[-1] + 1 >= len(scr.rows):
        return False
    title, line = rows[-1], rows[-1] + 1
    parts = [('Version ', LRED), (version_h('SURFOS_VERSION'), ORANGE), (' on an x86, ', LRED), (version_h('SURFOS_YEARS'), ORANGE)]
    if scr.rows[line] != ''.join(text for text, _ in parts):
        return False
    cells = [(c, title, LRED) for c, ch in enumerate(scr.rows[title]) if ch != ' ']
    col = 0
    for text, colour in parts:
        cells += [(col + i, line, colour) for i, ch in enumerate(text) if ch != ' ']
        col += len(text)
    return all(scr.colours[(c, r)][0] == colour for c, r, colour in cells)


def ansi_ok(scr):
    """the `ansi` pattern: text, Z in the bottom right corner (no scroll), inverse and bright yellow on blue"""
    if scr is None or len(scr.rows) < scr.nrows:
        return False
    return (all(scr.rows[r] == text for r, text in ANSI_ROWS.items()) and
            scr.rows[12] == f'ANSI-TEST done, {scr.cols}x{scr.nrows}' and scr.rows[-1].endswith('Z') and
            len(scr.rows[-1]) == scr.cols and scr.colours[(9, 2)] == (BLACK, GREY) and
            scr.colours[(9, 3)] == (YELLOW, BLUE))


def drawn(text):
    """text as both sides of a comparison can show it: printable ASCII, no space at the ends"""
    return ''.join(c for c in text if ' ' <= c <= '~').strip()


def ends_prompt(row):
    return PROMPT_ROW.search(row.rstrip()) is not None


def prompt_on(scr):
    return any(ends_prompt(r) for r in scr.rows)


def last_prompt_row(scr):
    rows = [i for i, r in enumerate(scr.rows) if ends_prompt(r)]
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
            m = PROMPT_TEXT.search(vm.text())
            serial_boot = vm.text()[:m.end()] if m else vm.text()
            results.append(('framebuffer shows the shell prompt', prompt_on(scr)))
            # the last boot lines on the serial port must be the last ones on the screen too (what the screen
            # can draw of them: before the console draws UTF-8 the prompt's wave is not there)
            want = [drawn(l) for l in serial_boot.splitlines() if drawn(l)][-6:]
            shown = [drawn(r) for r in scr.rows if drawn(r)]
            results.append(('framebuffer and serial agree on the last boot stages', shown[-len(want):] == want))
            results.append(('framebuffer shows the banner: Brandon Burr\'s SurfOS, the version and the years in their colours',
                            banner_ok(scr)))
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
                return all(e in text for e in expects) and last_prompt_row(s) >= 0 and ends_prompt(s.rows[last_prompt_row(s)]) and text != before.text()
            got = vm.wait_screen(done, 60, interval=0.5)
            results.append((f'keyboard: {keys.strip()!r} shows {expects}', got is not None))
        # raw mode (tty.h) on the PS/2 keyboard: Up, Backspace and Enter as a terminal sends them, F5 nothing
        vm.keys('rawkeys\n')
        ok = vm.wait_screen(lambda s: 'q ends' in s.text(), 20, interval=0.5) is not None
        if ok:
            vm.keys('<up><bs>\n<f5>xq')
            ok = vm.wait_screen(lambda s: '1b 5b 41 7f 0d 78 71' in s.text() and
                                ends_prompt(s.rows[last_prompt_row(s)]), 30, interval=0.5) is not None
        results.append(('keyboard: raw mode sends ESC [ A, 7f and 0d for Up, Backspace and Enter', ok))
        # console switching: F2 shows another shell, F1 comes back
        vm.keys('<f2>')
        two = vm.wait_screen(lambda s: prompt_on(s) and 'Hello World' not in s.text(), 20, interval=0.5)
        results.append(('F2 switches to console 2 (its own shell)', two is not None))
        vm.screen('console2.png')
        vm.keys('<f1>')
        one = vm.wait_screen(lambda s: '(C)2004-2026 Brandon Burr.' in s.text(), 20, interval=0.5)
        results.append(('F1 switches back to console 1 with its screen', one is not None))
        vm.screen('console1.png')
        # escape sequences (tty.h): the test pattern of `ansi`, read back with its colours
        vm.keys('ansi\n')
        scr = vm.wait_screen(lambda s: 'ANSI-TEST done' in s.text() and prompt_on(s), 20, interval=0.5)
        results.append(('escape sequences on the framebuffer: position, moves, erase, inverse, colours, the corner',
                        ansi_ok(scr)))
        # 10/6/26: surfsh's line editor on the PS/2 keyboard, as on the laptop: Tab, Up and Enter ('\r') in raw
        # mode. The ASCII prompt first, which every framebuffer font draws (the wave needs the console's UTF-8 work).
        vm.keys('surfsh\n')
        ok = vm.wait_screen(lambda s: any(r.startswith('SURF ') for r in s.rows), 30, interval=0.5) is not None
        if ok:
            vm.keys('set prompt.unicode off\n')
            ok = vm.wait_screen(lambda s: s.rows[last_prompt_row(s)].rstrip().endswith('SURF ~>'), 30, interval=0.5) is not None
        if ok:
            vm.keys('ech<tab>keyboard-ok\n<up>\n')
            ok = vm.wait_screen(lambda s: [r.strip() for r in s.rows].count('keyboard-ok') == 2 and
                                s.rows[last_prompt_row(s)].rstrip().endswith('SURF ~>'), 30, interval=0.5) is not None
        if ok:
            vm.keys('exit 7\n')
            ok = vm.wait_screen(lambda s: 'exit status 7' in s.text() and ends_prompt(s.rows[last_prompt_row(s)]),
                                30, interval=0.5) is not None
        vm.screen('surfsh.png')
        results.append(('keyboard: surfsh completes with Tab, recalls with Up, runs with Enter and exits', ok))
        results.append(('QEMU still running', vm.proc.poll() is None))
        final = vm.screen('final.png').text()
        for bad in BAD:
            results.append((f'no "{bad}" on the screen', bad not in final))
    finally:
        vm.close()
    return results


def scenario_fullhd(args, image, firmware, outdir):
    """1920x1080, as on the laptop: more text, same letters (120x33 at scale 2, not 80x25 centred)"""
    results = []
    vm = Vm('fullhd', args, image, firmware, mem='512', serial=False, outdir=outdir,
            extra=['-global', 'VGA.xres=1920', '-global', 'VGA.yres=1080'])

    def at_prompt(s):
        return last_prompt_row(s) >= 0 and ends_prompt(s.rows[last_prompt_row(s)])

    def typed(keys, test, timeout=60):
        vm.keys(keys)
        return vm.wait_screen(lambda s: test(s) and at_prompt(s), timeout, interval=0.5)
    try:
        scr = vm.wait_screen(prompt_on, args.boot_timeout, interval=2.0)
        results.append(('UEFI boot at 1920x1080 to the prompt (keyboard and framebuffer only)', scr is not None))
        if scr is None:
            vm.screen('stuck.png')
            return results
        vm.screen('boot.png')
        results.append(('1920x1080: 120x33 cells at scale 2, 12-pixel margins above and below, the prompt at the left edge',
                        (scr.w, scr.h, scr.scale, scr.cols, scr.nrows, scr.ox, scr.oy) == (1920, 1080, 2, 120, 33, 0, 12)
                        and not scr.rows[last_prompt_row(scr)].startswith(' ')))
        got = typed('bootinfo\n', lambda s: '120x33 text at scale 2' in s.text())
        results.append(('bootinfo: framebuffer 1920x1080, 120x33 text at scale 2', got is not None))
        # a line longer than 80 columns: typed past the end of the row (the line editor wraps it),
        # 6 x Backspace back across the wrap, and echoed on one row of 105 characters
        long = 'abcdefghij' * 11
        got = typed('echo ' + long + '<bs>' * 6 + 'Z\n', lambda s: long[:104] + 'Z' in s.rows)
        results.append(('a 105-character line on one row; Backspace erases back across a wrapped line', got is not None))
        got = typed('help\n', lambda s: any(r.startswith('    die ') and r.endswith('processes use run)') for r in s.rows))
        results.append(('help: its 104-character line on one row', got is not None))
        got = typed('ansi\n', lambda s: 'ANSI-TEST done' in s.text(), 30)
        results.append(('ansi at 120x33: the pattern, Z in the corner (119,32), the size from TTY_GETSIZE', ansi_ok(got)))
        vm.keys('<f2>')
        two = vm.wait_screen(lambda s: prompt_on(s) and 'ANSI-TEST' not in s.text(), 20, interval=0.5)
        vm.keys('<f1>')
        one = vm.wait_screen(lambda s: 'ANSI-TEST done, 120x33' in s.text(), 20, interval=0.5)
        results.append(('F2 shows console 2 and F1 brings console 1 back, at 120x33', two is not None and one is not None))
        # 10/6/26 (the console's character set, the pager): UTF-8 drawn as code page 437 cells,
        # read back through the rebuilt font (driver/font8x16.c), SurfOS's ▸ and ≋ included
        got = typed('run pagetest utf8\n', lambda s: UTF8_SAMPLE in s.rows)
        if got is not None:
            vm.screen('utf8.png')
        results.append(('UTF-8 on the framebuffer: ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ read back through the font, € as ?',
                        got is not None))

        # the pager (TTY_PAGE) on the keyboard: after `clear`, `pagetest 100` asks after 32 lines
        # (120x33) in inverse video on the last row; Enter shows one more line, Space the next 32,
        # Esc drops the rest
        def asking_after(last):
            return lambda s: (s.rows[-1].startswith('--More--') and s.colours.get((0, s.nrows - 1)) == (BLACK, GREY) and
                              s.rows[:-1] == ['pagetest line %03d' % n for n in range(last - s.nrows + 2, last + 1)])
        ok = typed('clear\n', lambda s: True) is not None
        if ok:
            vm.keys('run pagetest 100\n')
            ok = vm.wait_screen(asking_after(32), 30, interval=0.5) is not None
        if ok:
            vm.keys('\n')
            ok = vm.wait_screen(asking_after(33), 30, interval=0.5) is not None
        if ok:
            vm.keys(' ')
            ok = vm.wait_screen(asking_after(65), 30, interval=0.5) is not None
        if ok:
            vm.screen('pager.png')
            vm.keys('<esc>')
            got = vm.wait_screen(lambda s: at_prompt(s) and 'exit status 0' in s.text(), 30, interval=0.5)
            ok = got is not None and 'PAGETEST DONE' not in got.text() and 'pagetest line 066' not in got.text()
        results.append(('pager on the keyboard at 120x33: --More-- after 32 lines, Enter one more, Space 32 more, '
                        'Esc drops the rest', ok))
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


def scenario_kernel_shell(args, image, firmware, outdir):
    """10/6/26: the menu's third entry, shell=kernel: the ring-0 shell on every console on purpose, elevated (≋#>),
    with the banner on the screen and no recovery line"""
    results = []
    elevated = re.compile(rb'SURF \xe2\x89\x8b' + _SGR + rb'#' + _SGR + rb'>')
    vm = Vm('kernel-shell', args, image, firmware, mem='512', serial=True, outdir=outdir)
    try:
        ok = vm.expect('Use the', args.boot_timeout)               # the GRUB menu on the firmware console
        results.append(('the GRUB menu appears', ok))
        if ok:
            time.sleep(0.5)
            vm.keys('<down><down>')
            vm.keys('\n')
            ok = vm.expect('Loading SurfOS with shell=kernel', 30) and vm.expect(elevated, args.boot_timeout)
            results.append(('the kernel shell entry boots to the ring-0 shell, elevated: SURF \u224b#>', ok))
        if ok:
            vm.pump(1)
            scr = vm.screen('kernel-shell.png')
            row = scr.rows[last_prompt_row(scr)] if last_prompt_row(scr) >= 0 else ''
            results.append(('the framebuffer shows the banner and the elevated prompt', banner_ok(scr) and row.endswith('#>')))
            results.append(('it was asked for: no recovery line', 'recovery console' not in vm.text()))
            ok, out = run_cmd(vm, 'ps', ['Shell 0', 'Shell 1', 'Shell 2', 'Shell 3'])
            results.append(('ps: the ring-0 shells on F1 to F4, no surfsh', ok and 'surfsh' not in out))
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


BIG_BODY = b''.join(b'SurfOS USB network test: line %05d of 16384, 64 bytes each....\n' % i for i in range(16384))
assert len(BIG_BODY) == 1 << 20                # the large download: 1 MB


class HttpHandler(http.server.BaseHTTPRequestHandler):
    """what the guest fetches with httpget: a line naming the path, or BIG_BODY"""
    def do_GET(self):
        if self.path == '/big':
            body = BIG_BODY
        else:
            body = f'SurfOS HTTP test OK: {self.path}\n'.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def run_cmd(vm, cmd, expects, timeout=30):
    start = len(vm.transcript)
    vm.send(cmd + '\n')
    ok = all(vm.expect(e, timeout) for e in expects) and vm.expect(PROMPT, timeout)
    return ok, vm.transcript[start:].decode(errors='replace')


USBNET_DEV = 'usb-net,bus=xhci.0,netdev=n0,id=usbnet0,mac=52:54:00:12:34:56'
# a second adapter on another network (not QEMU's 10.0.2.0/24), plugged in later
USBNET_OTHER = 'usb-net,bus=xhci.0,netdev=n1,id=usbnet1,mac=52:54:00:65:43:21'


def scenario_usbnet(args, image, firmware, outdir):
    results = []
    httpd = http.server.HTTPServer(('127.0.0.1', 0), HttpHandler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    vm = Vm('usbnet', args, image, firmware, mem='512', serial=True, outdir=outdir,
            extra=['-netdev', 'user,id=n0', '-netdev', 'user,id=n1,net=192.168.76.0/24', '-device', USBNET_DEV])
    try:
        if boot_serial(vm, results, args.boot_timeout):
            boot = vm.text()
            results.append(('the adapter is set up at boot as eth0 (CDC ECM)',
                            re.search(r'\+USB: port \d+: 0525:a4a2 .*: eth0, cdc-ecm, MAC 52:54:00:12:34:56', boot) is not None))
            for cmd, expects in [('lsusb', ['QEMU USB HARDDRIVE', 'RNDIS/QEMU USB Network Device', '-> eth0, cdc-ecm']),
                                 ('ifconfig', ['eth0: 52:54:00:12:34:56, link up']),
                                 ('dhcp', ['lease 10.0.2.15', 'gateway 10.0.2.2', 'dns 10.0.2.3']),
                                 ('ping 10.0.2.2 3', ['3 packets transmitted, 3 received']),
                                 (f'httpget 10.0.2.2 {port} /usbnet', ['200 OK', 'SurfOS HTTP test OK: /usbnet', 'bytes received'])]:
                ok, _ = run_cmd(vm, cmd, expects)
                results.append((cmd.replace(str(port), '<port>'), ok))
            ok, out = run_cmd(vm, f'httpget 10.0.2.2 {port} /big', ['line 00000 of 16384', 'bytes received'], timeout=120)
            m = re.search(r'(\d+) bytes received in (\d+) ms(.*)', out)
            got = int(m.group(1)) if m else 0
            if m:
                ms = max(int(m.group(2)), 1)
                print(f'      ({got} bytes in {ms} ms: {got / ms:.0f} KB/s)', flush=True)
            # the body and the response header: at least the body, and the end of the body seen
            results.append(('1 MB over HTTP arrives complete', ok and m is not None and got > len(BIG_BODY)
                            and 'timed out' not in m.group(3)))
            ok, _ = run_cmd(vm, 'selftest', ['ok   ping 10.0.2.2', 'ok   DHCP lease', 'SELFTEST PASS'], timeout=180)
            results.append(('selftest, with its network checks on the USB adapter', ok))
            # unplugged: the port reports it, eth0 stays but has no link
            vm.mon.cmd('device_del usbnet0')
            ok = vm.expect('unplugged (eth0, cdc-ecm', 20)
            results.append(('unplugging the adapter is noticed', ok))
            ok, _ = run_cmd(vm, 'ifconfig', ['eth0: 52:54:00:12:34:56, link down'])
            results.append(('eth0 remains, link down', ok))
            # plugged in again: the same name, and with the same MAC the same address
            vm.mon.cmd('device_add ' + USBNET_DEV)
            ok = vm.expect('eth0, cdc-ecm, MAC 52:54:00:12:34:56', 30)
            results.append(('plugged in again, it is eth0 again', ok))
            vm.expect('eth0: link up', 10)
            ok, _ = run_cmd(vm, 'ifconfig', ['link up', 'inet 10.0.2.15'])
            results.append(('and keeps its address', ok))
            ok, _ = run_cmd(vm, 'ping 10.0.2.2 2', ['2 packets transmitted, 2 received'])
            results.append(('ping after plugging it in again', ok))
            # another adapter on another network: it becomes eth0 with the old address dropped,
            # and the self test checks what holds on any network
            vm.mon.cmd('device_del usbnet0')
            vm.expect('unplugged (eth0, cdc-ecm', 20)
            vm.mon.cmd('device_add ' + USBNET_OTHER)
            ok = vm.expect('eth0, cdc-ecm, MAC 52:54:00:65:43:21', 30)
            results.append(('a different adapter becomes eth0', ok))
            vm.expect('eth0: link up', 10)
            ok, _ = run_cmd(vm, 'ifconfig', ['eth0: 52:54:00:65:43:21', 'inet 0.0.0.0'])
            results.append(('without the previous adapter\'s address', ok))
            ok, _ = run_cmd(vm, 'dhcp', ['lease 192.168.76.15', 'gateway 192.168.76.2'])
            results.append(('dhcp on the other network', ok))
            ok, _ = run_cmd(vm, 'selftest', ['ok   the gateway answers', 'QEMU-only checks skipped', 'SELFTEST PASS'],
                            timeout=180)
            results.append(('selftest on a network that is not QEMU\'s default', ok))
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
        httpd.shutdown()
    return results


DATA_STICK = 'usb-storage,bus=xhci.0,drive=data,removable=on,id=stick1'


def data_stick_args(path):
    """a second USB stick; -blockdev so the image survives device_del and can be plugged in again"""
    return ['-blockdev', f'driver=file,filename={path},node-name=datafile',
            '-blockdev', 'driver=raw,file=datafile,node-name=data', '-device', DATA_STICK]


def scenario_usbdisk(args, image, firmware, outdir):
    results = []
    test_img = os.path.join(REPO, 'build', 'docker', 'build', 'images', 'test.img')
    if not os.path.isfile(test_img):
        test_img = os.path.join(REPO, 'build', 'images', 'test.img')
    if not os.path.isfile(test_img):
        return [('the QEMU test disk image exists (make images)', False)]
    tmpd = tempfile.mkdtemp(prefix='surfos-stick-', dir='/tmp' if os.path.isdir('/tmp') else None)
    stick = os.path.join(tmpd, 'stick.img')
    shutil.copyfile(test_img, stick)
    vm = Vm('usbdisk', args, image, firmware, mem='512', serial=True, outdir=outdir, extra=data_stick_args(stick))
    note = 'written by SurfOS to a USB stick'
    try:
        if boot_serial(vm, results, args.boot_timeout):
            boot = vm.text()
            results.append(('the second stick is the disk usb1 with partition usb1p1',
                            '+USB: usb1: QEMU QEMU HARDDISK, 16 MB' in boot and '+PART: usb1p1: MBR partition 1' in boot))
            results.append(('the boot stick is recognised as the SurfOS boot drive',
                            '+PART: usb0 is the SurfOS boot drive' in boot))
            results.append(('the boot does not mount a USB drive by itself',
                            not re.search(r'\+VFS: usb\S* mounted', boot)))
            ok, _ = run_cmd(vm, 'fdisk usb0 new mbr', ['the SurfOS boot drive'])
            results.append(('fdisk refuses the boot drive', ok))
            for cmd, expects in [('lsblk', ['usb0', 'boot drive: protected', 'usb1', 'usb1p1']),
                                 ('mount usb1p1 /stick', ['+FAT16: usb1p1']),
                                 ('ls /stick', ['DOCS/', 'README']),
                                 ('cat /stick/README', ['Welcome to the SurfOS Source Code']),
                                 ('cp /stick/hello /stick/HELLO2', []),
                                 (f'write /stick/NOTE.TXT {note}', []),
                                 ('cat /stick/NOTE.TXT', [note]),
                                 ('umount /stick', []),
                                 ('mount usb1p1 /stick', ['+FAT16: usb1p1']),
                                 ('cat /stick/NOTE.TXT', [note])]:
                ok, _ = run_cmd(vm, cmd, expects)
                results.append((cmd, ok))
            # the copy has the original's size (which depends on the build)
            ok1, a = run_cmd(vm, 'stat /stick/hello', ['bytes'])
            ok2, b = run_cmd(vm, 'stat /stick/HELLO2', ['bytes'])
            sa, sb = re.search(r'file, (\d+) bytes', a), re.search(r'file, (\d+) bytes', b)
            results.append(('the copy on the stick is as long as the original', ok1 and ok2 and sa and sb
                            and sa.group(1) == sb.group(1)))
            # pulled out while mounted: errors, not a crash; umount still works
            vm.mon.cmd('device_del stick1')
            ok = vm.expect('+USB: usb1: unplugged', 20) and vm.expect('unplugged (usb1', 20)   # the port's line comes last
            results.append(('unplugging the stick is noticed', ok))
            ok, out = run_cmd(vm, 'ls /stick', [])
            results.append(('a mounted file system on the unplugged stick fails cleanly', ok and 'DOCS/' not in out))
            ok, _ = run_cmd(vm, 'umount /stick', [])
            results.append(('and can still be unmounted', ok))
            ok, out = run_cmd(vm, 'lsblk', ['usb0'])
            results.append(('usb1 is gone from lsblk', ok and not re.search(r'^\s*usb1(p\d+)?\s', out, re.M)))
            # plugged in again: the same name, the data is still there. The port's status line comes
            # after the partition scan, so usb1p1 exists by then.
            vm.mon.cmd('device_add ' + DATA_STICK)
            ok = vm.expect('+USB: usb1: QEMU QEMU HARDDISK', 30) and vm.expect(': usb1 (16 MB)', 30)
            results.append(('plugged in again it is usb1 again', ok))
            ok, _ = run_cmd(vm, 'mount usb1p1 /stick', ['+FAT16: usb1p1'])
            ok2, _ = run_cmd(vm, 'cat /stick/NOTE.TXT', [note])
            results.append(('and what was written is still there', ok and ok2))
            run_cmd(vm, 'umount /stick', [])
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    data = open(stick, 'rb').read()
    results.append(('the stick image holds the written file', note.encode() in data and b'NOTE    TXT' in data))
    shutil.rmtree(tmpd, ignore_errors=True)
    return results


def run_steps(vm, results, steps, timeout=60):
    """(command, strings to see, [follow-up (line or raw bytes, strings to see)], name): one result each"""
    for step in steps:
        cmd, expects = step[0], step[1]
        follow = step[2] if len(step) > 2 else []
        name = step[3] if len(step) > 3 else cmd
        vm.send(cmd + '\n')
        ok = all(vm.expect(e, timeout) for e in expects)
        for line, exp in follow:
            if isinstance(line, bytes):
                vm.ser.sendall(line)                  # keys as they are, for a full-screen program
            else:
                vm.send(line + '\n')
            ok = ok and all(vm.expect(e, timeout) for e in exp)
        ok = ok and vm.expect(PROMPT, timeout)
        results.append((name, ok))


def find_image(name):
    for d in (os.path.join(REPO, 'build', 'docker', 'build', 'images'), os.path.join(REPO, 'build', 'images')):
        if os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name)
    return None


def mbr_partition(path, n=1):
    """(first byte, length in bytes) of MBR partition n"""
    with open(path, 'rb') as f:
        mbr = f.read(512)
    _, _, _, _, start, count = struct.unpack_from('<B3sB3sII', mbr, 446 + 16 * (n - 1))
    return start * 512, count * 512


def sparse_extract(path, offset, length, out):
    """a partition into its own file, skipping zero runs (images are sparse)"""
    with open(path, 'rb') as src, open(out, 'wb') as dst:
        src.seek(offset)
        left = length
        while left:
            chunk = src.read(min(left, 1 << 20))
            if not chunk:
                break
            if chunk.count(0) == len(chunk):
                dst.seek(len(chunk), 1)
            else:
                dst.write(chunk)
            left -= len(chunk)
        dst.truncate(length)


def fat_checks(path, label):
    """What independent FAT checkers say about partition 1 of an image: [(what, ok, output)].
    macOS: hdiutil attaches the image without mounting it and fsck_msdos -n reads it. Elsewhere,
    or as well: fsck.fat -n, from the host or from the devenv container."""
    out = []
    if sys.platform == 'darwin' and shutil.which('hdiutil') and shutil.which('fsck_msdos'):
        r = subprocess.run(['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-readonly',
                            path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        dev = r.stdout.split()[0] if r.returncode == 0 and r.stdout.split() else None
        if dev:
            try:
                f = subprocess.run(['fsck_msdos', '-n', dev + 's1'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True)
                out.append((f'macOS fsck_msdos finds no error on the {label}', f.returncode == 0, f.stdout))
            finally:
                subprocess.run(['hdiutil', 'detach', dev], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            out.append((f'hdiutil attaches the {label} image', False, r.stdout))
    off, length = mbr_partition(path)
    tmpd = tempfile.mkdtemp(prefix='surfos-fsck-', dir='/tmp' if os.path.isdir('/tmp') else None)
    try:
        part = os.path.join(tmpd, 'part.img')
        sparse_extract(path, off, length, part)
        cmd = None
        if shutil.which('fsck.fat'):
            cmd = ['fsck.fat', '-n', part]
        else:
            docker = shutil.which('docker') or '/Applications/Docker.app/Contents/Resources/bin/docker'
            if os.path.exists(docker) and subprocess.run([docker, 'image', 'inspect', 'surfos-modern-toolchain'],
                                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0:
                cmd = [docker, 'run', '--rm', '--platform', 'linux/amd64', '-v', f'{tmpd}:/d', 'surfos-modern-toolchain',
                       'fsck.fat', '-n', '/d/part.img']
        if cmd:
            f = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            out.append((f'fsck.fat finds no error on the {label}', f.returncode == 0, f.stdout))
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return out


def read_fat32(path, name):
    """a file from partition 1 of an image, through tools/mkusb.py's FAT32 reader"""
    mk = load_mkusb()
    off, _ = mbr_partition(path)
    with open(path, 'rb') as f:
        return mk.Fat32(f, off).read(name)


EXTRA_STICKS = ['-blockdev', 'driver=file,filename={data},node-name=datafile',
                '-blockdev', 'driver=raw,file=datafile,node-name=data',
                '-device', 'usb-storage,bus=xhci.0,drive=data,removable=on,id=stick1',
                '-blockdev', 'driver=file,filename={foreign},node-name=foreignfile',
                '-blockdev', 'driver=raw,file=foreignfile,node-name=foreign',
                '-device', 'usb-storage,bus=xhci.0,drive=foreign,removable=on,id=stick2']


def scenario_storage(args, image, firmware, outdir):
    results = []
    foreign_src = find_image('foreign-fat32.img')
    if not foreign_src:
        return [('the foreign FAT32 image exists (make images)', False)]
    tmpd = tempfile.mkdtemp(prefix='surfos-storage-', dir='/tmp' if os.path.isdir('/tmp') else None)
    data, foreign = os.path.join(tmpd, 'data.img'), os.path.join(tmpd, 'foreign.img')
    with open(data, 'wb') as f:
        f.truncate(1 << 30)                       # a blank 1 GB stick (sparse)
    shutil.copyfile(foreign_src, foreign)
    vm = Vm('storage', args, image, firmware, mem='512', serial=True, outdir=outdir,
            extra=[a.format(data=data, foreign=foreign) for a in EXTRA_STICKS])
    try:
        if boot_serial(vm, results, args.boot_timeout):
            boot = vm.text()
            results.append(('the blank stick is usb1, the foreign one usb2 with partition usb2p1',
                            '+USB: usb1: QEMU QEMU HARDDISK, 1024 MB' in boot and '+PART: usb2p1:' in boot))
            steps = [
                # the safety rule
                ('mkfs usb0p1', ['the SurfOS boot drive'], [], 'mkfs refuses the boot drive'),
                ('fdisk usb0 new mbr', ['the SurfOS boot drive'], [], 'fdisk refuses the boot drive'),
                ('mount usb0p1 /bootstick', ['read-only'], [], 'the boot drive mounts read-only'),
                ('cat /bootstick/boot/BUILD-INFO.txt', ['SurfOS USB boot image', 'Source revision']),
                ('write /bootstick/X.TXT no', ['Read-only file system'], [], 'and cannot be written'),
                ('umount /bootstick', []),
                # a stick from another computer: long, accented and macOS-style names, folders, 1 MB
                ('mount usb2p1 /foreign', []),
                # (the console and its serial mirror show no bytes above 0x7e: 'cafe menu' loses its accent)
                ('run ls -a /foreign', ['._A long file name written elsewhere.txt', '.DS_Store',
                                        'A long file name written elsewhere.txt', 'BIG.BIN', 'menu.txt', 'Photos/']),
                ('cat /foreign/README.TXT', ['A plain 8.3 name.']),
                ('cat /foreign/Photos/2026/October/notes.txt', ['Three folders down.']),
                ('run wc -l /foreign/BIG.BIN', ['16384']),
                ('run head -n 2 /foreign/BIG.BIN', ['line 00000 of 16384', 'line 00001 of 16384']),
                ('sh', ['$ '], [('cat "/foreign/A long file name written elsewhere.txt"', ['Written on another computer', '$ ']),
                                ('cp /foreign/README.TXT "/foreign/Photos/2026/October/copied by SurfOS.txt"', ['$ ']),
                                ('exit', ['exit status 0'])], 'a long name through sh, a copy into a folder'),
                ('umount /foreign', []),
                # a blank stick: an MBR, one partition, FAT32, then files through the C library and the editor
                ('fdisk usb1 new mbr', ['Type usb1 to'], [('usb1', ['written, flushed and verified'])]),
                ('fdisk usb1 add', ['Type usb1 to'], [('usb1', ['written, flushed and verified', 'usb1 now has usb1p1'])]),
                ('mkfs usb1p1 SURFDATA', ['Type usb1p1 to'], [('usb1p1', ['FAT32'])]),
                ('mount usb1p1 /data', []),
                ('run fstest /data', ['FSTEST PASS']),
                ('sh', ['$ '], [('echo "hello from SurfOS" > /data/hello.txt', ['$ ']),
                                ('mkdir /data/Documents', ['$ ']),
                                ('mv /data/hello.txt "/data/Documents/Hello World.txt"', ['$ ']),
                                ('cat "/data/Documents/Hello World.txt"', ['hello from SurfOS', '$ ']),
                                ('exit', ['exit status 0'])], 'sh redirection, mkdir and mv on the stick'),
                ('run edit /data/notes.txt', ['New file', '^S Save'],
                 [(b'Written in the SurfOS editor\ron a USB stick', ['Col 15']),
                  (b'\x13', ['Wrote 2 lines']),
                  (b'\x11', ['exit status 0'])], 'the editor writes a file on the stick'),
                ('cat /data/notes.txt', ['Written in the SurfOS editor', 'on a USB stick']),
                ('df', ['/data']),
                ('umount /data', []),
                ('eject usb1', ['safe to remove']),
            ]
            run_steps(vm, results, steps, timeout=180)
        check_bad(vm, results, vm.text())
    finally:
        vm.close()
    # what other FAT implementations make of the two sticks
    try:
        for what, ok, text in fat_checks(data, 'stick SurfOS formatted') + fat_checks(foreign, 'foreign stick'):
            results.append((what, ok))
            if not ok:
                print(text, flush=True)
        results.append(('the files SurfOS wrote read back elsewhere (a FAT32 reader)',
                        read_fat32(data, 'Documents/Hello World.txt') == b'hello from SurfOS\n'
                        and read_fat32(data, 'notes.txt') == b'Written in the SurfOS editor\non a USB stick\n'
                        and read_fat32(foreign, 'Photos/2026/October/copied by SurfOS.txt') == b'A plain 8.3 name.\n'))
    except Exception as e:
        results.append((f'the sticks can be read back here ({e})', False))
    shutil.rmtree(tmpd, ignore_errors=True)
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
            results.append(('and SurfOS does not start', not PROMPT_TEXT.search(vm.text())))
            vm.screen('grub-error.png')
        finally:
            vm.close()
    finally:
        shutil.rmtree(tmpd, ignore_errors=True)
    return results


SCENARIOS = {'serial': scenario_serial, 'keyboard': scenario_keyboard, 'fullhd': scenario_fullhd,
             'highmem': scenario_highmem, 'fallback': scenario_fallback, 'kernel-shell': scenario_kernel_shell,
             'no-pit': scenario_nopit,
             'usbnet': scenario_usbnet, 'usbdisk': scenario_usbdisk, 'storage': scenario_storage,
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
            print(f'\n({scr.w}x{scr.h}, {scr.cols}x{scr.nrows} text at scale {scr.scale}, cursor {scr.cursor})')
            return 0 if scr and prompt_on(scr) else 1
        finally:
            vm.close()

    summary = {'image': os.path.abspath(a.image), 'image_sha256': sha256(a.image), 'qemu': qemu_version(a.qemu),
               'ovmf_code': firmware[0], 'ovmf_code_sha256': sha256(firmware[0]), 'ovmf_vars': firmware[1],
               'scenarios': {}}
    failed = 0
    for name in a.scenario or ['serial', 'keyboard', 'fullhd', 'highmem', 'fallback', 'kernel-shell', 'no-pit', 'usbnet',
                               'usbdisk', 'storage', 'no-initrd', 'no-kernel']:
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
