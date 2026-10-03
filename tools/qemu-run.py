#!/usr/bin/env python3
"""Boot SurfOS under QEMU with no display and talk to it.

    tools/qemu-run.py --kernel build/O0/surfos.bin --test            smoke test (what `make test` runs)
    tools/qemu-run.py --kernel build/O0/surfos.bin --shell ps tick   run shell commands, print the transcript
    tools/qemu-run.py --kernel build/O0/surfos.bin --screen --keys 'help\\n<f2>'
                                                                     type on the PS/2 keyboard, dump the VGA screen

The serial console (driver/serial.c) is the main channel: it mirrors the active
console and feeds received bytes into the keyboard queue. The VGA dump and --keys
go through the QEMU monitor (pmemsave / sendkey), so they keep working when the
serial path itself is what is broken.
"""
import argparse
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PROMPT = 'SurfOS*>'

# (command, strings that must appear, in order, before the next prompt)
SMOKE = [
    ('tick',    ['Ticks:']),
    ('memstat', ['Physical memory:', 'Free physical pages:']),
    ('ps',      ['Kernel Idle Task', 'init', 'Shell 0']),
    ('help',    ['SurfOS ring0 Debug Shell']),
    ('test',    ['linear is using physical']),
    ('dmesg',   ['Booting SurfOS Kernel']),
    ('crashgp', ['General Protection Fault', 'Backtrace:', 'restarted the shell']),
    ('crashnull', ['Page Fault at 0x00000000', 'restarted the shell']),
    ('heaptest', ['HEAPTEST PASS']),
    ('lspci',   ['Ethernet', '82540EM']),
    ('lsblk',   ['rd0', 'hda', 'hda1']),
    ('hexdump hda 0', ['55 aa']),
    ('mount',   ['rd0', '/initrd', 'tarfs']),
    ('ls /initrd', ['etc/', 'motd']),
    ('cat /initrd/motd', ['Welcome to SurfOS']),
    ('ls /hda1', ['DOCS/', 'README', 'motd']),
    ('cat /hda1/DOCS/README.md', ['SurfOS']),
    ('uptime',  ['tasks']),
    ('date',    ['CMOS clock']),
    ('selftest', ['SELFTEST PASS']),
    ('cat /hda1/SELFTEST.TXT', ['SurfOS wrote this file']),
]
# anything that means the kernel fell over
BAD = ['Kernel Wipeout', "Woah.. this ain't", 'SYSTEM HALTED', 'HALTING']


class Qemu:
    def __init__(self, kernel, qemu='qemu-system-i386', mem='64', extra=(), int_log=None, iso=None,
                 disk=None, initrd=None, disk_inplace=False):
        # UNIX socket paths are limited to 108 bytes, so keep the work directory short
        base = tempfile.gettempdir()
        if len(base) > 50 and os.path.isdir('/tmp'):
            base = '/tmp'
        self.workdir = tempfile.mkdtemp(prefix='surfos-', dir=base)
        self.mon_path = os.path.join(self.workdir, 'mon.sock')
        self.ser_path = os.path.join(self.workdir, 'ser.sock')
        boot = ['-cdrom', os.path.abspath(iso), '-boot', 'd'] if iso else ['-kernel', os.path.abspath(kernel)]
        if initrd and not iso:
            boot += ['-initrd', os.path.abspath(initrd)]      # a Multiboot module: the ramdisk rd0
        self.disk = None
        if disk:
            # the guest writes to the disk, so work on a copy unless asked not to
            if disk_inplace:
                self.disk = os.path.abspath(disk)
            else:
                self.disk = os.path.join(self.workdir, 'disk.img')
                shutil.copyfile(disk, self.disk)
            boot += ['-drive', f'file={self.disk},format=raw,if=ide,index=0,media=disk']  # primary master: hda
        cmd = [qemu, '-m', str(mem)] + boot + [
               '-display', 'none', '-no-reboot', '-no-shutdown',
               '-monitor', f'unix:{self.mon_path},server,nowait',
               '-chardev', f'socket,id=ser0,path={self.ser_path},server=on,wait=off',
               '-serial', 'chardev:ser0'] + list(extra)
        if int_log:
            cmd += ['-d', 'int,guest_errors,cpu_reset', '-D', os.path.abspath(int_log)]
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, cwd=self.workdir, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.ser = self._connect(self.ser_path)
        self.ser.setblocking(False)
        self.transcript = b''
        self.cursor = 0

    def _connect(self, path, timeout=10):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + timeout
        while True:
            try:
                s.connect(path)
                return s
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    raise RuntimeError('QEMU did not start: ' + ' '.join(self.cmd) + '\n' + self.qemu_output())
                time.sleep(0.05)

    def alive(self):
        return self.proc.poll() is None

    def wait(self, seconds):
        """sleep while draining the serial port, so QEMU never back-pressures the UART"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.pump(0.1)

    def qemu_output(self):
        if self.proc.poll() is None:
            return ''
        try:
            return self.proc.stdout.read().decode(errors='replace')
        except Exception:
            return ''

    # --- serial console -------------------------------------------------
    def pump(self, timeout=0.1):
        r, _, _ = select.select([self.ser], [], [], timeout)
        if r:
            try:
                data = self.ser.recv(65536)
            except BlockingIOError:
                return
            if data:
                self.transcript += data

    def expect(self, text, timeout=10):
        """wait until text shows up in serial output after the previous match"""
        needle = text.encode()
        deadline = time.time() + timeout
        while True:
            i = self.transcript.find(needle, self.cursor)
            if i >= 0:
                self.cursor = i + len(needle)
                return True
            if time.time() > deadline or not self.alive():
                return False
            self.pump(0.1)

    def send(self, text):
        self.ser.sendall(text.encode())

    def text(self):
        t = self.transcript.decode(errors='replace').replace('\r', '')
        return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', t)

    # --- monitor --------------------------------------------------------
    def monitor(self, command, wait=0.3):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(self.mon_path)
        try:
            s.recv(4096)  # banner
        except socket.timeout:
            pass
        s.sendall((command + '\n').encode())
        time.sleep(wait)
        s.settimeout(0.3)
        out = b''
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass
        s.close()
        return re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', out.decode(errors='replace'))

    def screen(self):
        """the 80x25 VGA text screen. The file name is relative on purpose: the
        monitor treats a leading '/' as a format specifier."""
        path = os.path.join(self.workdir, 'vga.bin')
        if os.path.exists(path):
            os.remove(path)
        self.monitor('pmemsave 0xb8000 4000 vga.bin')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) == 4000:
                break
            time.sleep(0.1)
        else:
            return '(screen dump failed)'
        d = open(path, 'rb').read()
        rows = []
        for r in range(25):
            cells = (d[(r * 80 + c) * 2] for c in range(80))
            rows.append(''.join(chr(b) if 32 <= b < 127 else ' ' for b in cells).rstrip())
        return '\n'.join(rows).rstrip('\n')

    def sendkeys(self, spec):
        """type on the emulated PS/2 keyboard. spec: plain text, \\n for Enter,
        <f1>..<f12>, <esc>, <bs>, <up> etc. Upper case letters are shifted."""
        keymap = {c: c for c in 'abcdefghijklmnopqrstuvwxyz0123456789'}
        keymap.update({' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash',
                       ',': 'comma', ';': 'semicolon', '=': 'equal', '\\': 'backslash',
                       '\'': 'apostrophe', '[': 'bracket_left', ']': 'bracket_right'})
        named = {'esc': 'esc', 'bs': 'backspace', 'tab': 'tab', 'up': 'up', 'down': 'down',
                 'left': 'left', 'right': 'right', 'del': 'delete', 'ins': 'insert',
                 'home': 'home', 'end': 'end', 'pgup': 'pgup', 'pgdn': 'pgdn'}
        named.update({f'f{i}': f'f{i}' for i in range(1, 13)})
        for tok in re.findall(r'<[^>]+>|.', spec.encode().decode('unicode_escape'), re.S):
            if tok.startswith('<'):
                key = named.get(tok[1:-1].lower())
            elif tok.isupper():
                key = 'shift-' + tok.lower()
            else:
                key = keymap.get(tok)
            if key:
                self.monitor('sendkey ' + key, 0.05)
                self.pump(0)

    def registers(self):
        regs = self.monitor('info registers', 0.3)
        return '\n'.join(l for l in regs.splitlines() if l.startswith(('EIP', 'EAX', 'ESP', 'CR0', 'CR2')))

    def shutdown(self):
        """stop QEMU (the disk image is complete once it has exited)"""
        if self.proc.poll() is None:
            try:
                self.monitor('quit', 0.1)
            except Exception:
                pass
            try:
                self.proc.wait(3)
            except Exception:
                self.proc.kill()

    def close(self):
        self.shutdown()
        shutil.rmtree(self.workdir, ignore_errors=True)


def host_checks(q):
    """after the guest has run: read what it wrote to the disk with the host's mtools"""
    if not q.disk or not shutil.which('mcopy'):
        return []
    q.shutdown()
    with open(q.disk, 'rb') as f:
        mbr = f.read(512)
    start = int.from_bytes(mbr[454:458], 'little') * 512     # partition 1
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    try:
        out = subprocess.run(['mcopy', '-i', f'{q.disk}@@{start}', '::/SELFTEST.TXT', '-'],
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20).stdout
    except Exception as e:
        out = str(e).encode()
    return [('FAT file written by the self test is readable with mtools', out == b'SurfOS wrote this file.\n')]


def tail(text, n=40):
    lines = text.splitlines()
    return '\n'.join(lines[-n:])


def run_smoke(q, boot_timeout):
    results = [('boot to prompt (serial)', q.expect(PROMPT, boot_timeout))]
    if results[0][1]:
        for cmd, expects in SMOKE:
            q.send(cmd + '\n')
            ok = all(q.expect(e, 10) for e in expects) and q.expect(PROMPT, 10)
            results.append((cmd, ok))
    q.pump(0.5)
    text = q.text()
    for bad in BAD:
        results.append((f'no "{bad}" on the console', bad not in text))
    results.append(('QEMU still running', q.alive()))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', default='build/O0/surfos.bin')
    ap.add_argument('--iso', help='boot this ISO (GRUB) instead of -kernel')
    ap.add_argument('--disk', help='raw disk image for the primary IDE master (hda)')
    ap.add_argument('--initrd', help='file to load as a Multiboot module (ramdisk rd0)')
    ap.add_argument('--disk-inplace', action='store_true', help='let the guest modify --disk itself instead of a copy')
    ap.add_argument('--qemu', default='qemu-system-i386')
    ap.add_argument('--mem', default='64')
    ap.add_argument('--extra', default='', help='extra QEMU arguments')
    ap.add_argument('--int-log', help='write the QEMU interrupt/exception log here (-d int)')
    ap.add_argument('--boot-timeout', type=float, default=20)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--test', action='store_true', help='smoke test over the serial console')
    mode.add_argument('--shell', nargs='+', metavar='CMD', help='run shell commands over serial')
    mode.add_argument('--screen', action='store_true', help='dump the VGA text screen')
    ap.add_argument('--keys', default='', help='with --screen: keys to type first (see sendkeys)')
    ap.add_argument('--wait', type=float, default=4, help='with --screen: seconds to wait for boot')
    a = ap.parse_args()

    q = Qemu(a.kernel, a.qemu, a.mem, a.extra.split(), a.int_log, a.iso, a.disk, a.initrd, a.disk_inplace)
    try:
        if a.test:
            results = run_smoke(q, a.boot_timeout)
            for name, ok in results:
                print(('PASS ' if ok else 'FAIL ') + name)
            failed = [n for n, ok in results if not ok]
            if failed:
                print('\n---- serial transcript (tail) ----\n' + tail(q.text()))
                print('\n---- VGA screen ----\n' + q.screen())
                print('\n---- CPU ----\n' + q.registers())
                print(f'\nSMOKE TEST FAILED: {len(failed)} of {len(results)} checks')
                return 1
            extra = host_checks(q)          # needs QEMU stopped, so only after the dumps above
            for name, ok in extra:
                print(('PASS ' if ok else 'FAIL ') + name)
            results += extra
            if any(not ok for _, ok in extra):
                print(f'\nSMOKE TEST FAILED: {sum(not ok for _, ok in extra)} of {len(results)} checks')
                return 1
            print(f'\nSMOKE TEST PASSED: {len(results)} checks')
            return 0
        if a.shell:
            if not q.expect(PROMPT, a.boot_timeout):
                print(q.text())
                print('\n(no shell prompt on the serial console)')
                return 1
            for cmd in a.shell:
                q.send(cmd + '\n')
                if not q.expect(PROMPT, 15):
                    break
            q.pump(0.5)
            print(q.text())
            return 0 if q.alive() else 1
        if a.screen:
            q.wait(a.wait)
            if a.keys:
                q.sendkeys(a.keys)
                q.wait(1.5)
            print(q.screen())
            print('\n---- CPU ----\n' + q.registers())
            return 0
    finally:
        q.close()


if __name__ == '__main__':
    sys.exit(main())
