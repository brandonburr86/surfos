#!/usr/bin/env python3
"""
SurfOS riscv32 on QEMU's virt machine: the automated kernel test
File: test_os.py Date: 10/15/26 (SurfCPU)

Boots the machine-mode kernel (build/riscv32/qemu_virt/surfos.elf) in qemu-system-riscv32,
with a FAT32 disk image as a RAM disk (-device loader at 0x88000000: tools/mkimage.py's
"foreign" stick, long and accented names, folders and a 1 MB file, made by mkfs.fat and
mtools), and drives the kernel shell over the serial console: the banner, help, the
tasks running and sleeping, the timer, interrupts, the self test (registers across traps,
stack alignment, tasks, locks), faults that kill a shell which init restarts, and FAT
read and write. At the end the RAM disk is saved through QEMU's monitor (pmemsave) and
checked on the host: fsck.fat finds it clean and mtools reads back what SurfOS wrote.

With --package it then boots the kernel package itself, SURFOS.BIN, the way SurfBoot hands
over (BOOT-ABI.md section 4): a stand-in for SurfBoot's last step (boards/qemu_virt/stub)
copies the payload to its load address and jumps to the entry with a1 pointing at a
struct surf_boot_info the harness made (its command line, RAM size and CRC), and the
kernel must take it; a second boot with a corrupted CRC must refuse the block and carry on;
a third passes the word sdsim, which makes the RAM disk sd0, through the Arty's SD driver
(boards/arty_a7/sdcard.c) and a simulated card, for FAT32 over the real SD code path.

QEMU's virt machine is not SurfCPU: this tests the portable kernel and the qemu_virt HAL
(boards/qemu_virt), not the Arty's hardware.

  test_os.py --kernel build/riscv32/qemu_virt/surfos.elf [--qemu qemu-system-riscv32]
             [--package build/riscv32/qemu_virt/SURFOS.BIN --stub build/riscv32/qemu_virt/surfboot_stub.elf]
             [--evidence surfcpu/docs/evidence/os/test-os-O2.txt] [--transcript FILE] [--verbose]

Exit status 0 when every check passed.
"""

import argparse
import os
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mkimage  # noqa: E402  (tools/mkimage.py: the foreign stick, FOREIGN_FILES, foreign_big)
sys.path.insert(0, os.path.join(ROOT, 'arch', 'riscv32'))
import surfimg_tmp  # noqa: E402  (the constants of the generated soc.h, the package checks)

RAMDISK_ADDR = 0x88000000
# QEMU's rv32 CPU with everything SurfCPU lacks switched off (surfcpu/docs/ISA.md): misa 0x40001100,
# RV32IM, machine mode only (no S or U, no MMU, no PMP), no A, C, F, D or later extensions; an
# instruction the kernel should never use traps here as it would on SurfCPU
SURFCPU_LIKE = ('rv32,a=false,c=false,f=false,d=false,s=false,u=false,h=false,mmu=false,pmp=false,zawrs=false,'
                'zfa=false,zba=false,zbb=false,zbc=false,zbs=false,zicond=false,zihintntl=false,zihintpause=false,'
                'zacas=false,zcb=false,zfh=false,zfhmin=false,sstc=false,svadu=false')
SGR = rb'(?:\x1b\[[0-9;]*m)*'
PROMPT = re.compile(rb'(?:\xe2\x89\x8b|~)' + SGR + rb'[!#@?]?' + SGR + rb'>')
MORE = b'--More--'
PART_OFFSET = mkimage.PART_START * 512


class Qemu:
    def __init__(self, qemu, boot, disk, verbose=False, loads=()):
        """boot: QEMU's arguments for what runs first (-bios none -kernel ELF, or -bios the stub);
        loads: (file, address) pairs put into RAM with -device loader, as the disk image is"""
        base = '/tmp' if os.path.isdir('/tmp') else tempfile.gettempdir()
        self.workdir = tempfile.mkdtemp(prefix='surfrv-', dir=base)
        self.mon_path = os.path.join(self.workdir, 'mon.sock')
        self.ser_path = os.path.join(self.workdir, 'ser.sock')
        self.disk = os.path.join(self.workdir, 'disk.img')
        shutil.copyfile(disk, self.disk)
        extra = []
        for i, (path, addr) in enumerate(loads):
            name = 'load%d.bin' % i
            shutil.copyfile(path, os.path.join(self.workdir, name))
            extra += ['-device', 'loader,file=%s,addr=0x%x,force-raw=on' % (name, addr)]
        self.argv = [qemu, '-machine', 'virt', '-cpu', SURFCPU_LIKE, '-m', '256M', '-smp', '1'] + boot + [
                    '-display', 'none', '-no-reboot',
                    '-device', 'loader,file=disk.img,addr=0x%x,force-raw=on' % RAMDISK_ADDR] + extra + [
                    '-monitor', 'unix:%s,server,nowait' % self.mon_path,
                    '-chardev', 'socket,id=ser0,path=%s,server=on,wait=on' % self.ser_path,
                    '-serial', 'chardev:ser0']
        self.verbose = verbose
        self.proc = subprocess.Popen(self.argv, cwd=self.workdir, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.ser = self._connect(self.ser_path)
        self.ser.setblocking(False)
        self.transcript = b''
        self.cursor = 0
        self.more_at = 0

    def _connect(self, path, timeout=10):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + timeout
        while True:
            try:
                s.connect(path)
                return s
            except OSError:
                if self.proc.poll() is not None or time.time() > deadline:
                    out = self.proc.stdout.read().decode(errors='replace') if self.proc.poll() is not None else ''
                    raise RuntimeError('QEMU did not start: %s\n%s' % (" ".join(self.argv), out))
                time.sleep(0.05)

    def alive(self):
        return self.proc.poll() is None

    def pump(self, timeout=0.1):
        try:
            r, _, _ = select.select([self.ser], [], [], timeout)
        except ValueError:
            return
        if not r:
            return
        try:
            data = self.ser.recv(65536)
        except (BlockingIOError, ConnectionResetError):
            return
        if data:
            if self.verbose:
                sys.stdout.write(data.decode(errors='replace'))
                sys.stdout.flush()
            self.transcript += data
            while True:                                 # the pager waits after a screenful: Space
                i = self.transcript.find(MORE, self.more_at)
                if i < 0:
                    break
                self.more_at = i + len(MORE)
                self.ser.sendall(b' ')

    def wait(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.pump(min(0.1, max(0.0, deadline - time.time())))

    def expect(self, what, timeout=15):
        deadline = time.time() + timeout
        while True:
            if isinstance(what, re.Pattern):
                m = what.search(self.transcript, self.cursor)
                if m:
                    self.cursor = m.end()
                    return m
            else:
                i = self.transcript.find(what.encode(), self.cursor)
                if i >= 0:
                    self.cursor = i + len(what.encode())
                    return True
            if time.time() > deadline or not self.alive():
                return None
            self.pump(0.1)

    def send(self, text):
        self.ser.sendall(text.encode())

    def since(self, mark):
        t = self.transcript[mark:].decode(errors='replace').replace('\r', '')
        return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', t)

    def monitor(self, command, wait=0.5):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        s.connect(self.mon_path)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
        s.sendall((command + '\n').encode())
        time.sleep(wait)
        out = b''
        s.settimeout(0.3)
        try:
            while True:
                d = s.recv(65536)
                if not d:
                    break
                out += d
        except socket.timeout:
            pass
        s.close()
        return out.decode(errors='replace')

    def cmd(self, line, expects=(), timeout=20, forbid=()):
        """type a line, wait for each expected text in order and then the prompt; (all seen, the output)"""
        mark = len(self.transcript)
        self.cursor = mark
        self.send(line + '\r')
        ok = True
        for e in expects:
            if not self.expect(e, timeout):
                ok = False
                break
        if not self.expect(PROMPT, timeout):
            ok = False
        out = self.since(mark)
        for f in forbid:
            if (f.search(out) if isinstance(f, re.Pattern) else f in out):
                ok = False
        return ok, out

    def poweroff(self):
        """the board's poweroff command: QEMU's exit status (0: a pass), None if it did not exit"""
        self.send('poweroff\r')
        self.expect('powering off', 10)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            return None
        return self.proc.poll()

    def close(self):
        if self.proc.poll() is None:
            try:
                self.monitor('quit', 0.1)
            except OSError:
                pass
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.workdir, ignore_errors=True)


def run_mtools(args, env_extra=None):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', LANG='C.UTF-8')
    return subprocess.run(args, env=env, capture_output=True, text=True)


STUB_STAGE = 0x8D000000          # boards/qemu_virt/stub/surfboot_stub.S: where SURFOS.BIN is loaded
STUB_BOOTINFO = 0x8C000000       # and the boot information


def bootinfo_blob(cmdline, ram_size, image_base, image_mem, corrupt=False):
    """struct surf_boot_info, BOOT-ABI.md section 4, as SurfBoot fills it (this harness plays SurfBoot)"""
    c = surfimg_tmp.C
    head = struct.pack('<IHH16I', c['SURF_BOOTINFO_MAGIC'], c['SURF_BOOTINFO_VERSION'], 256,
                       0, c['SURF_BOARD_SIM'], c['SURF_SOC_VERSION'], c['SURF_MAP_VERSION'], c['SURF_SYS_HZ'],
                       10000000, c['SURF_DDR_BASE'], ram_size, image_base, image_mem, c['SURF_BOOT_DEVICE_SD'], 2048, 0,
                       0x000001, 0x53545542, 0)
    blob = bytearray(head + bytes(56) + cmdline.encode().ljust(128, b'\0'))
    assert len(blob) == 256
    struct.pack_into('<I', blob, 0x44, zlib.crc32(bytes(blob)) & 0xFFFFFFFF)
    if corrupt:
        blob[0x90] ^= 0x01                                  # one bit of the command line
    return bytes(blob)


def package_boots(args, disk, check):
    """SURFOS.BIN through the SurfBoot stand-in: with good boot information, then with a corrupted one"""
    with open(args.package, 'rb') as f:
        package = f.read()
    hdr = surfimg_tmp.check(package)                        # what SurfBoot checks before it copies anything
    check(True, 'the package passes SurfBoot\'s checks: load 0x%08x, entry 0x%08x, %d bytes, CRC %08x'
          % (hdr['load'], hdr['entry'], hdr['payload'], hdr['crc']))
    transcript = b''
    work = tempfile.mkdtemp(prefix='surfrv-pkg-', dir='/tmp' if os.path.isdir('/tmp') else None)
    try:
        for corrupt in (False, True):
            info = os.path.join(work, 'bootinfo.bin')
            with open(info, 'wb') as f:
                f.write(bootinfo_blob('ascii surftest=1', 256 << 20, hdr['load'], hdr['mem'], corrupt))
            q = Qemu(args.qemu, ['-bios', os.path.abspath(args.stub)], disk, args.verbose,
                     loads=[(args.package, STUB_STAGE), (info, STUB_BOOTINFO)])
            try:
                ok = q.expect("Brandon Burr's SurfOS", 30) and q.expect(PROMPT, 30)
                boot_log = q.since(0)
                if not corrupt:
                    check(ok and 'BOOT PROBLEM' not in boot_log, 'package: SURFOS.BIN copied to its load address boots to the shell')
                    check('SURF ~#>' in boot_log and '\u224b' not in boot_log,
                          'package: the command line from the boot information arrives (ascii: an ASCII prompt)')
                    ok, out = q.cmd('bootinfo', ['boot information: SurfBoot 0.0.1 (53545542)'])
                    check(ok and "command line: 'ascii surftest=1'" in out and 'handoff a0 0, a1 0x8c000000, a2 0x49465253' in out,
                          'package: bootinfo shows the handoff (a0 0, a1, a2 = the magic), SurfBoot\'s block and its command line')
                    check('RAM 0x80000000 + 256 MB verified; image 0x80000000 + %d bytes' % hdr['mem'] in out,
                          'package: the RAM size and the image\'s memory size come from the boot information')
                    ok, out = q.cmd('ls /rd0p1', ['README.TXT'])
                    check(ok, 'package: the RAM disk works as before')
                else:
                    ok2, out = q.cmd('bootinfo', ['boot information: none'])
                    check(ok and ok2 and "the boot information's CRC does not match" in out and '\u224b' in boot_log,
                          'package: a corrupted boot information block is refused (CRC), the kernel boots on without it')
                if not corrupt:
                    check(q.poweroff() == 0, 'package: poweroff')
                else:                               # QEMU runs with -no-reboot: a reset ends it
                    q.send('reboot\r')
                    seen = q.expect('SurfOS System Reboot Sequence', 10)
                    try:
                        q.proc.wait(10)
                    except subprocess.TimeoutExpired:
                        pass
                    check(seen and q.proc.poll() is not None, 'reboot: the reset register resets the machine (QEMU ends)')
            finally:
                transcript += q.transcript
                q.close()
        transcript += sdsim_boot(args, disk, hdr, check, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return transcript


def save_ramdisk(q, size):
    """the RAM disk image as it is in the guest now (QEMU's monitor), or None"""
    q.monitor('pmemsave 0x%x %d ramdisk.bin' % (RAMDISK_ADDR, size), 2.0)
    dump = os.path.join(q.workdir, 'ramdisk.bin')
    for _ in range(100):
        if os.path.exists(dump) and os.path.getsize(dump) == size:
            return dump
        time.sleep(0.1)
    return None


def sdsim_boot(args, disk, hdr, check, work):
    """the package again with the word sdsim: the RAM disk is sd0, through the Arty's SD driver
    (boards/arty_a7/sdcard.c) and a simulated SDHC card; FAT32 on /sd0p1, then the host checks it"""
    info = os.path.join(work, 'bootinfo-sdsim.bin')
    with open(info, 'wb') as f:
        f.write(bootinfo_blob('sdsim', 256 << 20, hdr['load'], hdr['mem']))
    q = Qemu(args.qemu, ['-bios', os.path.abspath(args.stub)], disk, args.verbose,
             loads=[(args.package, STUB_STAGE), (info, STUB_BOOTINFO)])
    big = mkimage.foreign_big()
    big_crc = zlib.crc32(big) & 0xFFFFFFFF
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        log = q.since(0)
        check(ok and '+VFS: sd0p1 mounted on /sd0p1 (fat)' in log,
              'sdsim: the RAM disk through the SD driver and a simulated card is sd0; its FAT32 partition mounted')
        ok, out = q.cmd('ls /sd0p1', ['entries'])
        check(ok and 'A long file name written elsewhere.txt' in out and 'BIG.BIN' in out, 'sdsim: ls /sd0p1 over the SD driver')
        ok, out = q.cmd('crc32 /sd0p1/BIG.BIN', ['BIG.BIN'], timeout=90)
        check(ok and re.search(r'%08x\s+1048576' % big_crc, out), 'sdsim: the 1 MB file read through the SD driver (CMD17/CMD18)')
        ok, out = q.cmd('cp /sd0p1/BIG.BIN /sd0p1/SDCOPY.BIN', ['1048576 bytes copied'], timeout=120)
        ok2, _ = q.cmd('write /sd0p1/SDSIM.TXT written through the SD driver')
        ok3, out3 = q.cmd('sync', ['synced'])
        check(ok and ok2 and ok3, 'sdsim: a 1 MB copy and a new file written through the SD driver (CMD24/CMD25), synced')
        ok, out = q.cmd('drivers', ['sdsim'])
        m = re.search(r'sdsim\s+(\d+) reads, (\d+) writes, (\d+) CRC retries, (\d+) timeouts, (\d+) errors', out)
        check(ok and m and int(m.group(1)) > 0 and int(m.group(2)) > 0 and m.group(4) == '0' and m.group(5) == '0',
              'sdsim: the driver counted %s reads and %s writes, no timeouts or errors' % (m.group(1), m.group(2)) if m else 'sdsim: counters')
        ok, out = q.cmd('umount /sd0p1')
        dump = save_ramdisk(q, os.path.getsize(disk)) if ok else None
        if check(dump, 'sdsim: umount, and the card image saved through the monitor'):
            part = dump + '@@%d' % PART_OFFSET
            r = run_mtools(['mtype', '-i', part, '::/SDSIM.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'written through the SD driver', 'sdsim: mtools reads SDSIM.TXT')
            r = run_mtools(['mcopy', '-n', '-i', part, '::/SDCOPY.BIN', os.path.join(q.workdir, 'sdcopy.bin')])
            got = open(os.path.join(q.workdir, 'sdcopy.bin'), 'rb').read() if r.returncode == 0 else b''
            check(got == big, 'sdsim: mtools reads the 1 MB copy byte for byte')
            with open(dump, 'rb') as f:
                f.seek(PART_OFFSET)
                with open(os.path.join(q.workdir, 'part.img'), 'wb') as p:
                    p.write(f.read())
            r = subprocess.run(['fsck.fat', '-n', os.path.join(q.workdir, 'part.img')], capture_output=True, text=True)
            check(r.returncode == 0, 'sdsim: fsck.fat -n finds the volume clean')
        check(q.poweroff() == 0, 'sdsim: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', default=os.path.join(ROOT, 'build/riscv32/qemu_virt/surfos.elf'))
    ap.add_argument('--qemu', default='qemu-system-riscv32')
    ap.add_argument('--package', help='SURFOS.BIN, booted through the SurfBoot stand-in (--stub)')
    ap.add_argument('--stub')
    ap.add_argument('--evidence')
    ap.add_argument('--transcript')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    work = tempfile.mkdtemp(prefix='surfrv-img-', dir='/tmp' if os.path.isdir('/tmp') else None)
    disk = os.path.join(work, 'foreign.img')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), 'foreign', disk], check=True,
                   stdout=subprocess.DEVNULL)
    big_crc = zlib.crc32(mkimage.foreign_big()) & 0xFFFFFFFF
    t_start = time.time()
    q = Qemu(args.qemu, ['-bios', 'none', '-kernel', os.path.abspath(args.kernel)], disk, args.verbose)
    try:
        cmd = q.cmd

        # boot: the banner and the prompt, with the board's lines on the way
        boot_ok = all(q.expect(t, 30) for t in ['SurfOS: serial console on the qemu_virt UART', 'Interrupts Enabled',
                                               '+RAMDISK: rd0 is the image at 0x88000000',
                                               "Brandon Burr's SurfOS", 'Version 1.0 on a RISC-V (QEMU virt), 2004-2026'])
        check(boot_ok and q.expect(PROMPT, 30), 'boot: the banner (SurfOS 1.0 on a RISC-V) and the kernel shell prompt')
        boot_log = q.since(0)
        check('+VFS: rd0p1 mounted on /rd0p1 (fat)' in boot_log, 'boot: the FAT32 partition of the RAM disk mounted on /rd0p1')
        check('Kernel panic' not in boot_log and 'Wipeout' not in boot_log, 'boot: no panic')

        ok, out = cmd('help', ['SurfOS kernel shell 1.0', 'poweroff', '(C)2004-2026 Brandon Burr.'])
        check(ok and 'selftest' in out and 'lspci' not in out, 'help: the kernel shell 1.0, portable commands, no PCI')

        ok, out = cmd('ps', ['SurfOS Task List'])
        names = ['Kernel Idle Task', 'init', 'heartbeat', 'Shell 0', 'Shell 1', 'Shell 2', 'Shell 3']
        check(ok and all(n in out for n in names), 'ps: idle, init, heartbeat and a kernel shell on each of F1-F4')

        # the tick and the sleep list: ticks at 100 Hz, a sleep of 1000 ms is 100 ticks
        ok1, out1 = cmd('tick', ['Ticks:'])
        t1 = int(re.search(r'Ticks: (\d+)', out1).group(1)) if ok1 else 0
        ok, out = cmd('sleep 1000', ['slept 1000 ms'])
        m = re.search(r'slept 1000 ms \((\d+) ticks\)', out)
        check(ok and m and 99 <= int(m.group(1)) <= 105, 'sleep 1000: the shell sleeps 100 ticks (%s; a busy host may add a few)'
              % (m.group(1) if m else '?'))
        ok2, out2 = cmd('tick', ['Ticks:'])
        t2 = int(re.search(r'Ticks: (\d+)', out2).group(1)) if ok2 else 0
        check(ok1 and ok2 and t2 - t1 >= 100, 'the tick advances: %d ticks across the sleep' % (t2 - t1))

        ok, out = cmd('uptime', ['up ', 'heartbeat'])
        m = re.search(r'heartbeat (\d+)', out)
        check(ok and m and int(m.group(1)) >= 2, 'uptime: the heartbeat task woke %s times (every 500 ms)' % (m.group(1) if m else '?'))

        ok, out = cmd('tasks', ['started task A', 'task A ran 4 rounds, task B 3'], timeout=30)
        check(ok and 'task B: round 3 of 3' in out and 'task A: round 4 of 4' in out,
              'tasks: two kernel tasks count, sleep and wake, interleaved, and end')

        ok, out = cmd('irqstat', ['timer:'])
        m = re.search(r'timer: (\d+)', out)
        check(ok and m and int(m.group(1)) > 100 and re.search(r'\s10\s+\d+', out), 'irqstat: timer interrupts and the UART (PLIC 10)')

        ok, out = cmd('cpuinfo', ['hart 0'])
        check(ok and 'rv32im (misa 0x40001100)' in out and 'rv32im_zicsr_zifencei' in out,
              'cpuinfo: the hart is RV32IM, misa 0x40001100 as on SurfCPU (QEMU restricted to its ISA)')

        ok, out = cmd('selftest', ['SurfOS self test'], timeout=120)
        check(ok and 'SELFTEST PASS' in out, 'selftest: SELFTEST PASS')
        for line in out.splitlines():
            if line.strip().startswith(('ok', 'FAIL', 'skip')):
                print('      ' + line.strip())

        ok, out = cmd('heaptest', ['heaptest:'], timeout=60)
        check(ok and 'HEAPTEST PASS' in out, 'heaptest: HEAPTEST PASS')

        # FAT: read what another computer wrote
        ok, out = cmd('lsblk', ['rd0'])
        check(ok and 'rd0p1' in out, 'lsblk: the RAM disk rd0 and its partition rd0p1')
        ok, out = cmd('df', ['/rd0p1'])
        check(ok and 'FOREIGN' in out, 'df: the FAT32 volume FOREIGN')
        ok, out = cmd('ls /rd0p1', ['entries'])
        want = ['A long file name written elsewhere.txt', 'café menu.txt', 'README.TXT', 'BIG.BIN', 'Photos/']
        check(ok and all(w in out for w in want), 'ls: long, accented and 8.3 names, a folder, BIG.BIN')
        ok, out = cmd('cat /rd0p1/README.TXT', ['A plain 8.3 name.'])
        check(ok, 'cat: a file another computer wrote')
        ok, out = cmd('cat /rd0p1/Photos/2026/October/notes.txt', ['Three folders down.'])
        check(ok, 'cat: three folders down')
        ok, out = cmd('crc32 /rd0p1/BIG.BIN', ['BIG.BIN'], timeout=60)
        check(ok and re.search(r'%08x\s+1048576' % big_crc, out),
              'crc32: the 1 MB file reads back exactly (CRC %08x, a 2048-cluster chain)' % big_crc)

        # FAT: write
        ok, out = cmd('write /rd0p1/RISCV.TXT SurfOS wrote this on riscv32')
        ok2, out2 = cmd('cat /rd0p1/RISCV.TXT', ['SurfOS wrote this on riscv32'])
        check(ok and ok2, 'write and cat: a new file')
        ok, _ = cmd('mkdir /rd0p1/rvdir')
        ok2, out2 = cmd('cp /rd0p1/BIG.BIN /rd0p1/rvdir/Copy_Of_Big_File.bin', ['1048576 bytes copied'], timeout=90)
        ok3, out3 = cmd('crc32 /rd0p1/rvdir/Copy_Of_Big_File.bin', ['Copy_Of_Big_File.bin'], timeout=60)
        check(ok and ok2 and ok3 and ('%08x' % big_crc) in out3, 'mkdir, cp 1 MB into it under a long name, same CRC')
        ok, _ = cmd('write /rd0p1/rvdir/gone.txt delete me')
        ok2, _ = cmd('mv /rd0p1/rvdir/gone.txt /rd0p1/rvdir/renamed.txt')
        ok3, out3 = cmd('rm /rd0p1/rvdir/renamed.txt')
        ok4, out4 = cmd('ls /rd0p1/rvdir', ['entries'])
        check(ok and ok2 and ok3 and ok4 and 'renamed' not in out4 and 'Copy_Of_Big_File.bin' in out4, 'mv and rm')
        ok, out = cmd('sync', ['synced'])
        check(ok, 'sync: the block cache written out')
        ok, out = cmd('cd /rd0p1/Photos/2026')
        ok2, out2 = cmd('pwd', ['/rd0p1/Photos/2026'])
        ok3, _ = cmd('cd /')
        check(ok and ok2 and ok3, 'cd and pwd')
        ok, out = cmd('fdisk rd0', ['rd0'])
        check(ok and re.search(r'FAT32', out) and 'rd0p1' in out, 'fdisk rd0 shows the MBR partition table (reading only)')
        ok, out = cmd('ansi', ['ANSI-TEST done, 80x25'])
        check(ok, 'ansi: the console parses the escape sequences of tty.h (80x25 grid)')

        # faults: a killed shell comes back (init), a breakpoint is stepped over
        ok, out = cmd('crashint', ['breakpoint at pc', 'ebreak returned'])
        check(ok, 'crashint: ebreak is reported and stepped over')
        ok, out = cmd('crashill', ['killed by "illegal instruction', 'Backtrace:', 'init: restarted the shell',
                                   "Brandon Burr's SurfOS"], timeout=20)
        check(ok and 'cmd_crashill' in out and 'startShell' in out and 'shell+' in out and 'heartbeat' not in out,
              'crashill: the shell is killed with a backtrace (cmd_crashill, startShell, shell), init restarts it')
        ok, out = cmd('crashnull', ['killed by "store access fault', 'init: restarted the shell'], timeout=20)
        check(ok, 'crashnull: a store to address 0 is an access fault that kills the shell; init restarts it')
        ok, out = cmd('crashdiv', ['does not trap'])
        check(ok and '1 / 0 = -1' in out, 'crashdiv: RISC-V division by zero gives -1 without a trap')
        ok, out = cmd('ls /rd0p1', ['RISCV.TXT'])
        check(ok, 'the file system is still there after the faults')

        # unmount, save the RAM disk and check it on the host
        ok, out = cmd('umount /rd0p1')
        check(ok and 'busy' not in out.lower(), 'umount /rd0p1')
        q.monitor('pmemsave 0x%x %d ramdisk.bin' % (RAMDISK_ADDR, os.path.getsize(disk)), 2.0)
        dump = os.path.join(q.workdir, 'ramdisk.bin')
        for _ in range(100):
            if os.path.exists(dump) and os.path.getsize(dump) == os.path.getsize(disk):
                break
            time.sleep(0.1)
        if check(os.path.exists(dump) and os.path.getsize(dump) == os.path.getsize(disk), 'the RAM disk saved through the monitor'):
            part = dump + '@@%d' % PART_OFFSET
            r = run_mtools(['mtype', '-i', part, '::/RISCV.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'SurfOS wrote this on riscv32', 'mtools reads RISCV.TXT as SurfOS wrote it')
            r = run_mtools(['mcopy', '-n', '-i', part, '::/rvdir/Copy_Of_Big_File.bin', os.path.join(q.workdir, 'big.bin')])
            got = open(os.path.join(q.workdir, 'big.bin'), 'rb').read() if r.returncode == 0 else b''
            check(got == mkimage.foreign_big(), 'mtools reads the 1 MB copy under its long name, byte for byte')
            r = run_mtools(['mdir', '-i', part, '::/rvdir'])
            check(r.returncode == 0 and 'renamed' not in r.stdout, 'mtools: the removed file is gone')
            with open(dump, 'rb') as f:
                f.seek(PART_OFFSET)
                with open(os.path.join(q.workdir, 'part.img'), 'wb') as p:
                    p.write(f.read())
            r = subprocess.run(['fsck.fat', '-n', os.path.join(q.workdir, 'part.img')], capture_output=True, text=True)
            check(r.returncode == 0, 'fsck.fat -n: the volume is clean after the riscv32 kernel wrote it')
            if r.returncode:
                print(r.stdout + r.stderr)

        ok, out = cmd('mount rd0p1', ['rd0p1 on /rd0p1: fat32'])
        check(ok and 'not unmounted cleanly' not in out, 'mount again: clean (umount set the clean bit)')

        # a sleeping task killed: off the sleep list and gone
        ok, out = cmd('ps', ['heartbeat'])
        m = re.search(r'^\s*(\d+)\s+sleeping\s+LOW\s+.*heartbeat$', out, re.M)
        if check(ok and m, 'ps: the heartbeat task sleeps between its beats'):
            ok, out = cmd('kill %s' % m.group(1), ["killing 'heartbeat'"])
            cmd('sleep 200', ['slept'])             # init collects the zombie when it next runs
            ok2, out2 = cmd('ps', ['tasks'])
            check(ok and ok2 and 'heartbeat' not in out2, 'kill: the sleeping heartbeat task is killed and collected (by init)')

        check(q.poweroff() == 0, 'poweroff: QEMU exits with status 0 (sifive_test)')
    finally:
        transcript = q.transcript
        q.close()

    if args.package:
        try:
            transcript += package_boots(args, disk, check)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    else:
        shutil.rmtree(work, ignore_errors=True)

    passed = sum(1 for ok, _ in results if ok)
    elapsed = time.time() - t_start
    summary = 'test-os qemu_virt %s: %d of %d checks passed in %.0f s' % (os.path.relpath(args.kernel, ROOT), passed,
                                                                       len(results), elapsed)
    print(summary)
    if args.transcript:
        with open(args.transcript, 'wb') as f:
            f.write(transcript)
    if args.evidence:
        os.makedirs(os.path.dirname(args.evidence), exist_ok=True)
        with open(args.evidence, 'w') as f:
            f.write('# %s\n' % summary)
            f.write('# %s, %s\n' % (subprocess.run([args.qemu, '--version'], capture_output=True, text=True).stdout.splitlines()[0],
                                    time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())))
            for ok, what in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
