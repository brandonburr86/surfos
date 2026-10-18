#!/usr/bin/env python3
"""
SurfOS riscv32 on QEMU's virt machine: the automated kernel test
File: test_os.py Date: 10/15/26 (SurfCPU)

Boots the machine-mode kernel (build/riscv32/qemu_virt/surfos.elf) in qemu-system-riscv32,
with a FAT32 disk image as a RAM disk (-device loader at 0x88000000: tools/mkimage.py's
"foreign" stick, long and accented names, folders and a 1 MB file, made by mkfs.fat and
mtools, with /USERS/DOCUMENTS/ added by mtools: not an 8.3 name, so a long-name entry with
the alias DOCUME~1, as the Mac makes it), and drives the kernel shell over the serial
console: the banner, help, the tasks running and sleeping, the timer, interrupts, the self
test (registers across traps, stack alignment, tasks, locks), faults that kill a shell which
init restarts, and FAT read and write. At the end the RAM disk is saved through QEMU's
monitor (pmemsave) and checked on the host: fsck.fat finds it clean and mtools reads back
what SurfOS wrote.

10/16/26: a second boot runs QEMU slowed with -icount to about SurfCPU's pace for scrolltest:
two kernel tasks print at once while the console scrolls, and every row must stay whole (the
console lock, kernel/console.c, audit C10; the console before it fails here).

With --stub it then tests the boot ABI's handoff (BOOT-ABI.md section 4): a stand-in for
SurfBoot's last step (boards/qemu_virt/stub) enters the kernel with a0 = 0, a1 pointing at
a struct surf_boot_info the harness made (its command line, RAM size and CRC) and a2 = the
magic, and the kernel must take it; a second boot with a corrupted CRC must refuse the
block and carry on; a third passes the word sdsim, which makes the RAM disk sd0, through
the Arty's SD driver (boards/arty_a7/sdcard.c) and a simulated card, for FAT32 over the real
SD code path, where SurfOS also makes /USERS/DOCUMENTS itself. These boots put surfos.elf in
place with QEMU's loader, so they need no packer.

Where the fw track's tools are in the tree, two more parts run (each says when it is
skipped): SURFOS.BIN (packed by tools/surfimg.py, --package) checked with surfimg.py verify
and booted through the stand-in, which copies its payload to the load address; and a card
image made by tools/mkcard.py (--mkcard: MBR, FAT32, the standard directories of BOOT-ABI.md
section 5, the package in /BOOT) mounted through the SD driver, /USERS/DOCUMENTS found by its
long name and written to.

10/16/26 (milestone 8, files that persist): on the same simulated card the kernel's sdsim
command arms the card model's faults (boards/arty_a7/tests/sdmodel.c, after the io track's
SdFaults): a block answered "write error", a card that refuses every block or answers no
command, the card taken out and put back, and taken out right after the nth block of a new
file's write, an rm, an mv and an overwrite. Checked: what the kernel and the command say,
that nothing hangs, that eject keeps a volume it could not write, what fsck.fat -n finds on
the card as it was left (with the block cache's write order, audit B3, nothing that points
at a free cluster or at bytes not written), and, booting the pulled card again, that the
files read back whole or not at all. And a full volume: the stick made nearly full on the
host, a copy that does not fit says "No space left on device" and keeps what fitted (audit
B4: no cluster is left past its end), a write and a mkdir fail the same way, rm gives the
space back; fsck.fat and mtools check the result.

QEMU's virt machine is not SurfCPU: this tests the portable kernel and the qemu_virt HAL
(boards/qemu_virt), not the Arty's hardware.

  test_os.py --kernel build/riscv32/qemu_virt/surfos.elf [--qemu qemu-system-riscv32]
             [--stub build/riscv32/qemu_virt/surfboot_stub.elf] [--package build/riscv32/qemu_virt/SURFOS.BIN]
             [--surfimg surfcpu/tools/surfimg.py] [--mkcard surfcpu/tools/mkcard.py]
             [--evidence surfcpu/docs/evidence/os/test-os-O2.txt] [--transcript FILE] [--verbose]

Exit status 0 when every check that ran passed.
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
SOC_H = os.path.join(ROOT, 'surfcpu', 'firmware', 'include', 'surfcpu', 'soc.h')


def soc_constants(path=SOC_H):
    """the SURF_* numbers of the generated soc.h: the boot information is built from them, never typed here"""
    consts = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'#define\s+(SURF_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u?\b', line)
            if m:
                consts[m.group(1)] = int(m.group(2), 0)
    return consts


C = soc_constants()

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
    def __init__(self, qemu, boot, disk, verbose=False, loads=(), elf=None):
        """boot: QEMU's arguments for what runs first (-bios none -kernel ELF, or -bios the stub);
        loads: (file, address) pairs put into RAM with -device loader, as the disk image is;
        elf: an ELF the loader puts at its own addresses (the kernel, for the stub to enter)"""
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
        if elf:
            shutil.copyfile(elf, os.path.join(self.workdir, 'kernel.elf'))
            extra += ['-device', 'loader,file=kernel.elf']
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
PACKAGE_HEADER = struct.Struct('<IHHIIIIIIIIII12sI')     # BOOT-ABI.md section 2: the 64-byte SIMG header


def bootinfo_blob(cmdline, ram_size, image_base, image_mem, corrupt=False):
    """struct surf_boot_info, BOOT-ABI.md section 4, as SurfBoot fills it (this harness plays SurfBoot)"""
    head = struct.pack('<IHH16I', C['SURF_BOOTINFO_MAGIC'], C['SURF_BOOTINFO_VERSION'], 256,
                       0, C['SURF_BOARD_SIM'], C['SURF_SOC_VERSION'], C['SURF_MAP_VERSION'], C['SURF_SYS_HZ'],
                       10000000, C['SURF_DDR_BASE'], ram_size, image_base, image_mem, C['SURF_BOOT_DEVICE_SD'], 2048, 0,
                       0x000001, 0x53545542, 0)
    blob = bytearray(head + bytes(56) + cmdline.encode().ljust(128, b'\0'))
    assert len(blob) == 256
    struct.pack_into('<I', blob, 0x44, zlib.crc32(bytes(blob)) & 0xFFFFFFFF)
    if corrupt:
        blob[0x90] ^= 0x01                                  # one bit of the command line
    return bytes(blob)


def package_header(path):
    """the fields of SURFOS.BIN's header the boot information repeats (surfimg.py verify does the checking)"""
    with open(path, 'rb') as f:
        f = PACKAGE_HEADER.unpack(f.read(64))
    return {'magic': f[0], 'load': f[5], 'payload': f[6], 'mem': f[7], 'entry': f[8], 'crc': f[9], 'build': f[12]}


def elf_image(path):
    """where an ELF's loadable segments go: the lowest address and the bytes to the end of the last one in
    memory (BSS included), what SurfBoot reports for a package as load_addr and mem_size"""
    with open(path, 'rb') as f:
        d = f.read()
    phoff, = struct.unpack_from('<I', d, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, _, _, p_paddr, _, p_memsz = struct.unpack_from('<6I', d, phoff + i * phentsize)
        if p_type == 1 and p_memsz:                         # PT_LOAD
            segs.append((p_paddr, p_memsz))
    base = min(a for a, _ in segs)
    return base, max(a + n for a, n in segs) - base


def stub_boot(args, disk, work, cmdline, package=None, corrupt=False, ram_mb=256):
    """the kernel entered as SurfBoot hands over, by the stand-in (boards/qemu_virt/stub): SURFOS.BIN copied from
    STUB_STAGE to its load address or, without a package, surfos.elf put in place by QEMU's loader; a1 points at a
    boot information block with this command line and RAM size. (QEMU, the image's base and memory size)"""
    if package:
        hdr = package_header(package)
        base, mem, loads, elf = hdr['load'], hdr['mem'], [(package, STUB_STAGE)], None
    else:
        (base, mem), loads, elf = elf_image(args.kernel), [], args.kernel
    info = os.path.join(work, 'bootinfo.bin')
    with open(info, 'wb') as f:
        f.write(bootinfo_blob(cmdline, ram_mb << 20, base, mem, corrupt))
    q = Qemu(args.qemu, ['-bios', os.path.abspath(args.stub)], disk, args.verbose,
             loads=loads + [(info, STUB_BOOTINFO)], elf=elf)
    return q, base, mem


def handoff_boots(args, disk, check, work):
    """the boot ABI's handoff: a0, a1 and a2 and a boot information block with a command line, then a corrupted one"""
    transcript = b''
    for corrupt in (False, True):
        # 128 MiB of DDR in the boot information: the heap ends 16 MiB below it (BOOT-ABI.md's ddr_top)
        q, base, mem = stub_boot(args, disk, work, 'ascii surftest=1', corrupt=corrupt, ram_mb=128)
        try:
            ok = q.expect("Brandon Burr's SurfOS", 30) and q.expect(PROMPT, 30)
            boot_log = q.since(0)
            if not corrupt:
                check(ok and 'BOOT PROBLEM' not in boot_log,
                      'handoff: entered as SurfBoot hands over (a0, a1, a2), the kernel boots to the shell')
                check('SURF ~#>' in boot_log and '≋' not in boot_log,
                      'handoff: the command line from the boot information arrives (ascii: an ASCII prompt)')
                ok, out = q.cmd('bootinfo', ['boot information: SurfBoot 0.0.1 (53545542)'])
                check(ok and "command line: 'ascii surftest=1'" in out and 'handoff a0 0, a1 0x8c000000, a2 0x49465253' in out,
                      'handoff: bootinfo shows the handoff (a0 0, a1, a2 = the magic), SurfBoot\'s block and its command line')
                check('RAM 0x80000000 + 128 MB (calibrated, SurfBoot\'s quick test passed)' in out and
                      'image 0x%08x + %d bytes' % (base, mem) in out,
                      'handoff: the RAM size and the image\'s memory size come from the boot information')
                check(re.search(r'kernel heap 0x8[0-9a-f]{7}-0x86ffffff', boot_log),
                      'handoff: 128 MiB of DDR: the kernel heap ends at 0x87000000, 16 MiB below it (ddr_top)')
                ok, out = q.cmd('ls /rd0p1', ['README.TXT'])
                check(ok, 'handoff: the RAM disk works as before')
                check(q.poweroff() == 0, 'handoff: poweroff')
            else:
                ok2, out = q.cmd('bootinfo', ['boot information: none'])
                check(ok and ok2 and "the boot information's CRC does not match" in out and '≋' in boot_log,
                      'handoff: a corrupted boot information block is refused (CRC), the kernel boots on without it')
                q.send('reboot\r')                  # QEMU runs with -no-reboot: a reset ends it
                seen = q.expect('SurfOS System Reboot Sequence', 10)
                try:
                    q.proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass
                check(seen and q.proc.poll() is not None, 'reboot: the reset register resets the machine (QEMU ends)')
        finally:
            transcript += q.transcript
            q.close()
    return transcript


SCROLL_LINE = re.compile(r'^scroll [AB] \d{3} ')
SCROLL_WHOLE = re.compile(r'^scroll [AB] \d{3} abcdefghijklmnopqrstuvwxyz0123456789 end$')


def scroll_boot(args, disk, check):
    """scrolltest (arch/riscv32/kshell.c, audit C10): two kernel tasks print 60 lines each at once while the console
    scrolls, then the kernel checks every row of the console's cells. QEMU runs with -icount shift=7 (one instruction
    every 128 ns of virtual time, near SurfCPU's pace), so a 10 ms tick often lands inside a scroll or a line: before
    the console lock the rows came out blank, cut short or twice (the old console fails this check)."""
    q = Qemu(args.qemu, ['-bios', 'none', '-kernel', os.path.abspath(args.kernel), '-icount', 'shift=7,align=off,sleep=off'],
             disk, args.verbose)
    try:
        ok = q.expect("Brandon Burr's SurfOS", 60) and q.expect(PROMPT, 60)
        ok, out = q.cmd('scrolltest', ['scrolltest: 2 tasks'], timeout=120)
        m = re.search(r'scrolltest: (.*)', out)
        check(ok and 'SCROLLTEST PASS' in out, 'scrolltest: two tasks printing at once while the console scrolls leave '
              'every row whole, each task\'s lines in order (QEMU slowed with -icount)%s' % ('' if 'SCROLLTEST PASS' in out
                                                                                            else ': ' + (m.group(1) if m else '?')))
        lines = [l for l in out.splitlines() if SCROLL_LINE.match(l)]
        check(len(lines) == 120 and all(SCROLL_WHOLE.match(l) for l in lines),
              'scrolltest: the serial line got the same 120 lines, each whole (%d)' % len(lines))
        check(q.poweroff() == 0, 'scrolltest: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def fsfault_boot(args, disk, check):
    """10/18/26 (audit B56): a fault while the task is inside a file system, the block cache or a device request
    (crashfs: a store to address 0 between fs_enter() and fs_leave()) stops the system with a wipeout that says why:
    nothing could give back what the task held there. crashnull, the same store outside, kills only the shell"""
    q = Qemu(args.qemu, ['-bios', 'none', '-kernel', os.path.abspath(args.kernel)], disk, args.verbose)
    try:
        ok = q.expect("Brandon Burr's SurfOS", 60) and q.expect(PROMPT, 60)
        mark = len(q.transcript)
        q.cursor = mark
        q.send('crashfs\r')
        ok2 = all(q.expect(t, 20) for t in ['Kernel Wipeout: ', 'inside a file system, the block cache or a device request',
                                             'SYSTEM HALTED'])
        q.wait(1)
        out = q.since(mark)
        m = re.search(r'Kernel Wipeout: (.*)', out)
        check(ok and ok2 and 'init: restarted the shell' not in out,
              'crashfs: a store to address 0 inside a file system stops the system with a wipeout that says why (%s)'
              % (m.group(1).strip() if m else 'none'))
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def package_boot(args, disk, check, work):
    """SURFOS.BIN, packed by the fw track's tools/surfimg.py: its verify, then copied to its load address"""
    hdr = package_header(args.package)
    if args.surfimg and os.path.exists(args.surfimg):
        r = subprocess.run([sys.executable, args.surfimg, 'verify', args.package], capture_output=True, text=True)
        check(r.returncode == 0, 'package: surfimg.py verify passes SurfBoot\'s checks: load 0x%08x, entry 0x%08x, '
              '%d bytes, CRC %08x, build %08x' % (hdr['load'], hdr['entry'], hdr['payload'], hdr['crc'], hdr['build']))
        if r.returncode:
            print(r.stdout + r.stderr)
    q, base, mem = stub_boot(args, disk, work, 'surftest=1', package=args.package)
    try:
        ok = q.expect("Brandon Burr's SurfOS", 30) and q.expect(PROMPT, 30)
        check(ok and 'BOOT PROBLEM' not in q.since(0), 'package: SURFOS.BIN copied to its load address boots to the shell')
        ok, out = q.cmd('bootinfo', ['boot information: SurfBoot'])
        check(ok and 'image 0x%08x + %d bytes' % (base, mem) in out,
              'package: bootinfo shows the package\'s image (load 0x%08x, mem_size %d)' % (base, mem))
        check(q.poweroff() == 0, 'package: poweroff')
    finally:
        transcript = q.transcript
        q.close()
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


def fsck_partition(q, dump, offset):
    """fsck.fat -n on the partition of a saved image: (clean, its report)"""
    with open(dump, 'rb') as f:
        f.seek(offset)
        with open(os.path.join(q.workdir, 'part.img'), 'wb') as p:
            p.write(f.read())
    r = subprocess.run(['fsck.fat', '-n', os.path.join(q.workdir, 'part.img')], capture_output=True, text=True)
    return r.returncode == 0, r.stdout + r.stderr


def sdsim_boot(args, disk, check, work):
    """the word sdsim: the RAM disk is sd0, through the Arty's SD driver (boards/arty_a7/sdcard.c) and a simulated
    SDHC card; FAT32 on /sd0p1, /USERS/DOCUMENTS made by SurfOS, then the host checks it"""
    q, _, _ = stub_boot(args, disk, work, 'sdsim')
    big = mkimage.foreign_big()
    big_crc = zlib.crc32(big) & 0xFFFFFFFF
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        log = q.since(0)
        check(ok and '+VFS: sd0p1 mounted on /sd0p1 (fat)' in log,
              'sdsim: the RAM disk through the SD driver and a simulated card is sd0; its FAT32 partition mounted')
        ok, out = q.cmd('ls /sd0p1', ['entries'])
        check(ok and 'A long file name written elsewhere.txt' in out and 'BIG.BIN' in out, 'sdsim: ls /sd0p1 over the SD driver')
        # 10/18/26 (audit B59): the card watch is one of the kernel's own threads: kill refuses it (killed at once, it
        # left the driver's lock held, and every later request to the card waited for good)
        ok, out = q.cmd('ps', ['sdwatch'])
        m = re.search(r'^\s*(\d+)\s+\w+\s+\w+\s+.*sdwatch$', out, re.M)
        ok2, out2 = q.cmd('kill %s' % (m.group(1) if m else '0'), ['not killed']) if m else (False, '')
        ok3, out3 = q.cmd('ps', ['sdwatch'])
        check(ok and m and ok2 and "is one of the kernel's own threads" in out2 and ok3,
              'sdsim: kill refuses the card watch (one of the kernel\'s own threads), which lives on')
        ok, out = q.cmd('crc32 /sd0p1/BIG.BIN', ['BIG.BIN'], timeout=90)
        check(ok and re.search(r'%08x\s+1048576' % big_crc, out), 'sdsim: the 1 MB file read through the SD driver (CMD17/CMD18)')
        ok, out = q.cmd('cp /sd0p1/BIG.BIN /sd0p1/SDCOPY.BIN', ['1048576 bytes copied'], timeout=120)
        ok2, _ = q.cmd('write /sd0p1/SDSIM.TXT written through the SD driver')
        ok3, out3 = q.cmd('sync', ['synced'])
        check(ok and ok2 and ok3, 'sdsim: a 1 MB copy and a new file written through the SD driver (CMD24/CMD25), synced')
        # the card's user directory made by SurfOS: DOCUMENTS is not an 8.3 name, so long-name entries and an alias
        ok, _ = q.cmd('mkdir /sd0p1/USERS')
        ok2, _ = q.cmd('mkdir /sd0p1/USERS/DOCUMENTS')
        ok3, _ = q.cmd('write /sd0p1/USERS/DOCUMENTS/NOTE.TXT made by SurfOS in its own DOCUMENTS')
        ok4, out4 = q.cmd('ls /sd0p1/USERS', ['entries'])
        check(ok and ok2 and ok3 and ok4 and 'DOCUMENTS/' in out4,
              'sdsim: mkdir /sd0p1/USERS/DOCUMENTS (longer than 8.3) and a file written in it')
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
            r = run_mtools(['mdir', '-i', part, '::/USERS'])
            check(r.returncode == 0 and re.search(r'DOCUME~1\s+<DIR>.*\bDOCUMENTS\s*$', r.stdout, re.M),
                  'sdsim: mtools sees DOCUMENTS as a long name with the alias DOCUME~1 (the LFN entries SurfOS wrote)')
            if r.returncode == 0 and 'DOCUMENTS' not in r.stdout:
                print(r.stdout)
            r = run_mtools(['mtype', '-i', part, '::/USERS/DOCUMENTS/NOTE.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'made by SurfOS in its own DOCUMENTS',
                  'sdsim: mtools reads /USERS/DOCUMENTS/NOTE.TXT')
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            check(clean, 'sdsim: fsck.fat -n finds the volume clean')
            if not clean:
                print(report)
        check(q.poweroff() == 0, 'sdsim: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def card_boot(args, check, work):
    """a card image made by the fw track's tools/mkcard.py, laid out as BOOT-ABI.md section 5 says (MBR, one FAT32
    partition, /BOOT /SYSTEM /PROGRAMS /USERS/DOCUMENTS, the package in /BOOT when there is one), mounted through the
    SD driver (sdsim): /USERS/DOCUMENTS found by its long name, read and written, then checked on the host"""
    card = os.path.join(work, 'card.img')
    hello = os.path.join(work, 'hello.txt')
    notes = os.path.join(work, 'notes.txt')
    with open(hello, 'w') as f:
        f.write('Hello from the card image.\n')
    with open(notes, 'w') as f:
        f.write('Notes kept under a long name.\n')
    argv = [sys.executable, args.mkcard, card, '--size', '48M', '--standard-dirs',
            '--file', '/USERS/DOCUMENTS/HELLO.TXT=' + hello, '--file', '/USERS/DOCUMENTS/Notes-from-the-Mac.txt=' + notes]
    with_package = bool(args.package and os.path.exists(args.package))
    if with_package:
        argv += ['--file', '/BOOT/SURFOS.BIN=' + args.package]
    r = subprocess.run(argv, capture_output=True, text=True)
    if not check(r.returncode == 0, 'card: mkcard.py makes a 48 MiB card image with the standard directories%s'
                 % (' and /BOOT/SURFOS.BIN' if with_package else '')):
        print(r.stdout + r.stderr)
        return b''
    with open(card, 'rb') as f:
        part_lba, = struct.unpack_from('<I', f.read(512), 0x1C6)
    q, _, _ = stub_boot(args, card, work, 'sdsim')
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        check(ok and '+VFS: sd0p1 mounted on /sd0p1 (fat)' in q.since(0),
              'card: the mkcard.py image is sd0 through the SD driver; its FAT32 partition mounted on /sd0p1')
        ok, out = q.cmd('ls /sd0p1/USERS/DOCUMENTS', ['entries'])
        check(ok and 'HELLO.TXT' in out and 'Notes-from-the-Mac.txt' in out,
              'card: /sd0p1/USERS/DOCUMENTS found by its long name (alias DOCUME~1), both files listed')
        ok, _ = q.cmd('cat /sd0p1/USERS/DOCUMENTS/Notes-from-the-Mac.txt', ['Notes kept under a long name.'])
        ok2, _ = q.cmd('cat /sd0p1/USERS/DOCUMENTS/HELLO.TXT', ['Hello from the card image.'])
        check(ok and ok2, 'card: the files in DOCUMENTS read through the SD driver')
        if with_package:
            size = os.path.getsize(args.package)
            crc = zlib.crc32(open(args.package, 'rb').read()) & 0xFFFFFFFF
            ok, out = q.cmd('crc32 /sd0p1/BOOT/SURFOS.BIN', ['SURFOS.BIN'], timeout=60)
            check(ok and re.search(r'%08x\s+%d' % (crc, size), out), 'card: /BOOT/SURFOS.BIN on the card is the package')
        ok, _ = q.cmd('write /sd0p1/USERS/DOCUMENTS/FROMRV.TXT written on the card by SurfOS')
        ok2, _ = q.cmd('umount /sd0p1')
        dump = save_ramdisk(q, os.path.getsize(card)) if ok and ok2 else None
        if check(dump, 'card: a file written into DOCUMENTS, unmounted, the card image saved'):
            r = subprocess.run([sys.executable, args.mkcard, '--inspect', dump], capture_output=True, text=True)
            check(r.returncode == 0 and '/USERS/DOCUME~1/FROMRV.TXT' in r.stdout and '(DOCUMENTS)' in r.stdout,
                  'card: mkcard.py --inspect reads /USERS/DOCUMENTS/FROMRV.TXT back')
            r = run_mtools(['mtype', '-i', dump + '@@%d' % (part_lba * 512), '::/USERS/DOCUMENTS/FROMRV.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'written on the card by SurfOS', 'card: mtools reads it too')
            clean, report = fsck_partition(q, dump, part_lba * 512)
            check(clean, 'card: fsck.fat -n finds the card clean')
            if not clean:
                print(report)
        check(q.poweroff() == 0, 'card: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


# fsck.fat -n findings an interrupted FAT32 write may leave with the block cache's write order (audit B3): clusters
# marked used that nothing points at, the two FATs differing, a stale free count, the dirty bit. Anything else
# (an entry pointing at a free cluster, a chain shorter or longer than a size, shared clusters) would be damage.
FSCK_HARMLESS = ('Dirty bit is set', 'Automatically removing dirty bit', 'FATs differ but appear to be intact',
                 'Using first FAT', 'Reclaimed ', 'Free cluster summary wrong', 'Auto-correcting')
FSCK_NOISE = ('fsck.fat ', 'Leaving filesystem unchanged', 'part.img: ')


def fsck_findings(report):
    """fsck.fat -n's report as (harmless findings, other findings), its noise lines left out"""
    harmless, other = [], []
    for line in report.splitlines():
        s = line.strip()
        if not s or any(s.startswith(n) or n in s for n in FSCK_NOISE):
            continue
        (harmless if any(s.startswith(h) for h in FSCK_HARMLESS) else other).append(s)
    return harmless, other


def short(lines):
    return '; '.join(lines) if lines else 'nothing'


def sdsim_card(q, disk):
    """the card's blocks as they are now (the RAM disk, also while the card is out), or None"""
    return save_ramdisk(q, os.path.getsize(disk))


def seen(q, mark, text, timeout=5):
    """text anywhere in what the kernel printed since mark (before or after a prompt), waiting up to timeout"""
    deadline = time.time() + timeout
    while text.encode() not in q.transcript[mark:]:
        if time.time() > deadline or not q.alive():
            return False
        q.pump(0.1)
    return True


def fault_boot(args, disk, check, work):
    """milestone 8, under QEMU (sdsim): write faults of the card model (boards/arty_a7/tests/sdmodel.c, the io track's
    SdFaults write_error and no_response) during file writes. A refused block is written again by the next sync; a card
    that refuses everything makes the command that wrote say so, nothing hangs, eject keeps the volume mounted; once the
    card works again sync writes it all, and the host finds every other file as it was"""
    q, _, _ = stub_boot(args, disk, work, 'sdsim')
    big = mkimage.foreign_big()
    bad = re.compile(r'Kernel panic|Wipeout|killed')
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        check(ok and '+VFS: sd0p1 mounted on /sd0p1 (fat)' in q.since(0), 'faults: sdsim, /sd0p1 mounted read-write')
        ok, _ = q.cmd('write /sd0p1/KEEP.TXT written before the faults', forbid=['not all of it'])
        ok2, out = q.cmd('sdsim fault write-error 1', ['the next 1 written block is answered "write error"'])
        ok3, out3 = q.cmd('write /sd0p1/ONCE.TXT one block refused, then written again',
                          ['bcache: write of sd0p1 blocks', 'kept for the next sync', 'bcache: sd0p1 takes writes again'],
                          forbid=['not all of it', bad])
        ok4, out4 = q.cmd('cat /sd0p1/ONCE.TXT', ['one block refused, then written again'])
        check(ok and ok2 and ok3 and ok4, 'faults: one block answered "write error": the console says so, the next sync '
              '(at the file\'s close) writes it again, the command succeeds and the file reads back')
        ok, out = q.cmd('sdsim fault write-error 1000000', ['written blocks are answered "write error"'])
        t0 = time.time()
        ok2, out2 = q.cmd('write /sd0p1/STUCK.TXT written while every block is refused',
                          ['write: not all of it reached /sd0p1: I/O error'], timeout=60, forbid=[bad])
        ok3, out3 = q.cmd('cp /sd0p1/BIG.BIN /sd0p1/ERR.BIN', ['cp: not all of it reached /sd0p1: I/O error'], timeout=120,
                          forbid=[bad, 'bytes copied'])
        dt = time.time() - t0
        check(ok and ok2 and ok3 and dt < 100,
              'faults: a card that refuses every block: write and cp (1 MB, more than the cache holds) both say "not '
              'all of it reached /sd0p1: I/O error", and return (%.0f s; the cache keeps the refused blocks, no hang)' % dt)
        ok, out = q.cmd('eject sd0', ['eject: sd0: /sd0p1 could not be written: I/O error', 'eject: sd0 stays mounted'],
                        forbid=['safe to remove'])
        ok2, out2 = q.cmd('mount', ['sd0p1  on /sd0p1'])
        check(ok and ok2, 'faults: eject with blocks the card refused: "stays mounted", not "safe to remove" (audit B2)')
        ok, _ = q.cmd('sdsim fault clear', ['no faults armed'])
        ok2, out2 = q.cmd('sync', ['bcache: sd0p1 takes writes again', 'synced'], timeout=60)
        check(ok and ok2, 'faults: the card works again: sync writes everything the cache kept')
        ok, out = q.cmd('sdsim fault no-response 1000000', ['commands get no answer'])
        t0 = time.time()
        ok2, out2 = q.cmd('write /sd0p1/MUTE.TXT written while the card answers nothing',
                          [re.compile(rb'write: /sd0p1/MUTE.TXT: I/O error|write: not all of it reached /sd0p1: I/O error')],
                          timeout=60, forbid=[bad])
        dt = time.time() - t0
        ok3, _ = q.cmd('sdsim fault clear', ['no faults armed'])
        ok4, out4 = q.cmd('sync', ['synced'], timeout=60)
        m = re.search(r'(\d+) timeouts', q.cmd('sdsim')[1])
        said = re.search(r'write: .*I/O error', out2)
        check(ok and ok2 and ok3 and ok4 and dt < 30 and m and int(m.group(1)) > 0,
              'faults: a card that answers no command: the write fails at once (no response within NCR: "%s", %.1f s, %s '
              'driver timeouts), and sync works once the card answers' % (said.group(0) if said else '?', dt,
                                                                         m.group(1) if m else '?'))
        ok, out = q.cmd('crc32 /sd0p1/ERR.BIN', ['ERR.BIN'], timeout=60)
        m = re.search(r'([0-9a-f]{8})\s+(\d+)\s+/sd0p1/ERR.BIN', out)
        err_len = int(m.group(2)) if m else -1
        check(ok and m and 0 < err_len < len(big) and int(m.group(1), 16) == zlib.crc32(big[:err_len]) & 0xFFFFFFFF,
              'faults: ERR.BIN holds what cp wrote before the card refused everything: the first %d bytes of BIG.BIN' % err_len)
        ok, out = q.cmd('eject sd0', ['/sd0p1 unmounted', 'sd0: safe to remove'])
        dump = sdsim_card(q, disk) if ok else None
        if check(dump, 'faults: eject once the card works: safe to remove; the card image saved'):
            part = dump + '@@%d' % PART_OFFSET
            got = {}
            for name in ('KEEP.TXT', 'ONCE.TXT', 'STUCK.TXT', 'MUTE.TXT'):
                r = run_mtools(['mtype', '-i', part, '::/' + name])
                got[name] = r.stdout.strip() if r.returncode == 0 else None
            mute = got.pop('MUTE.TXT')
            check(got == {'KEEP.TXT': 'written before the faults', 'ONCE.TXT': 'one block refused, then written again',
                          'STUCK.TXT': 'written while every block is refused'} and
                  mute in (None, 'written while the card answers nothing'),
                  'faults: on the host every file the commands wrote is there and whole (MUTE.TXT %s: its open had failed)'
                  % ('not made' if mute is None else 'whole'))
            r = run_mtools(['mcopy', '-n', '-i', part, '::/BIG.BIN', os.path.join(q.workdir, 'big.bin')])
            r2 = run_mtools(['mcopy', '-n', '-i', part, '::/ERR.BIN', os.path.join(q.workdir, 'err.bin')])
            b1 = open(os.path.join(q.workdir, 'big.bin'), 'rb').read() if r.returncode == 0 else b''
            b2 = open(os.path.join(q.workdir, 'err.bin'), 'rb').read() if r2.returncode == 0 else b''
            check(b1 == big and b2 == big[:err_len], 'faults: BIG.BIN untouched, ERR.BIN the first %d bytes of it' % err_len)
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            harmless, other = fsck_findings(report)
            check(clean, 'faults: fsck.fat -n finds %s (audit B4: the clusters cp had taken past what it could write '
                  'went back to the free pool)' % ('the volume clean' if clean else short(other + harmless)))
            if not clean:
                print(report)
        check(q.poweroff() == 0, 'faults: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def pull_boot(args, disk, check, work):
    """milestone 8, under QEMU (sdsim): the card taken out while mounted read-write (card detect off). The kernel says
    so, refuses further writes, keeps the mount until umount; fsck.fat then finds only the dirty bit. Put back, it is
    sd0 again, mounts with a warning, and its files are there; ejected, it keeps the mark (10/17/26, audit B42)"""
    q, _, _ = stub_boot(args, disk, work, 'sdsim')
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        ok2, _ = q.cmd('write /sd0p1/KEEP.TXT written before the card came out', forbid=['not all of it'])
        mark = len(q.transcript)
        ok3, _ = q.cmd('sdsim pull', ['the card is out'])
        ok4 = seen(q, mark, '+SDSIM: sd0: the card was taken out') and \
            seen(q, mark, '+SDSIM: /sd0p1 was mounted read-write: what was not yet written to the card is lost; umount /sd0p1')
        check(ok and ok2 and ok3 and ok4, 'pull: the card taken out while mounted read-write: within half a second the '
              'kernel says so, and that what was not yet written is lost')
        ok, out = q.cmd('write /sd0p1/AFTER.TXT written after it came out', ['write: /sd0p1/AFTER.TXT: I/O error'])
        ok2, out2 = q.cmd('mount', ['sd0p1  on /sd0p1       type fat (unplugged: umount it)'])
        check(ok and ok2, 'pull: a write afterwards is refused (I/O error), the mount stays, marked "unplugged: umount it"')
        dump = sdsim_card(q, disk)
        if check(dump, 'pull: the card image as it was taken out, saved'):
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            harmless, other = fsck_findings(report)
            check(not other and any(h.startswith('Dirty bit is set') for h in harmless),
                  'pull: fsck.fat -n on it finds %s (pulled after a sync: only the mark of a volume mounted read-write)'
                  % short(harmless + other))
        ok, _ = q.cmd('umount /sd0p1')
        mark = len(q.transcript)
        ok2, _ = q.cmd('sdsim insert', ['the card is back in'])
        ok3 = seen(q, mark, '+SDSIM: sd0: a card was put in: mount sd0p1 to use it')
        ok4, out4 = q.cmd('mount sd0p1', ['sd0p1 was not unmounted cleanly', 'sd0p1 on /sd0p1: fat32'])
        ok5, _ = q.cmd('cat /sd0p1/KEEP.TXT', ['written before the card came out'])
        ok6, out6 = q.cmd('ls /sd0p1', ['entries'])
        check(ok and ok2 and ok3 and ok4 and ok5 and ok6 and 'AFTER.TXT' not in out6,
              'pull: umount, put back: sd0 again, mounted with "not unmounted cleanly", KEEP.TXT there, AFTER.TXT not')
        ok, out = q.cmd('eject sd0', ['sd0: safe to remove'])
        dump = sdsim_card(q, disk) if ok else None
        if dump:
            # 10/17/26 (audit B42): a card that was not cleanly unmounted when mounted stays marked so at its unmount
            # (the eject says so): only a checker clears the mark. Before, this eject cleared it
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            harmless, other = fsck_findings(report)
            check(not other and any(h.startswith('Dirty bit is set') for h in harmless) and 'only a check clears that' in out,
                  'pull: after eject the card stays marked as not cleanly unmounted (the eject says so), and fsck.fat -n '
                  'finds nothing else: %s' % short(harmless + other))
        check(q.poweroff() == 0, 'pull: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def pull_points(args, disk, check, work):
    """milestone 8: the card taken out after n of the blocks a new file's write sends (sdsim pull n), n = 1 to 4: its
    data, FAT copy 1, FAT copy 2, its directory entry (the block cache's write order, audit B3). fsck.fat -n on what is
    left must find nothing that points at a free cluster or at bytes not written; then the image is booted again (the
    reboot), mounted, and read back: the file there in full or not at all, the files before it untouched"""
    transcript = b''
    text = 'a new file with a long name'
    name = 'Notes-from-SurfOS.txt'
    for n in (1, 2, 3, 4):
        tag = 'pull after %d: ' % n
        q, _, _ = stub_boot(args, disk, work, 'sdsim')
        dump = None
        try:
            ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
            ok2, _ = q.cmd('write /sd0p1/KEEP.TXT written before the card came out', forbid=['not all of it'])
            ok3, _ = q.cmd('sdsim pull %d' % n, ['the card comes out right after'])
            mark = len(q.transcript)
            ok4, out4 = q.cmd('write /sd0p1/%s %s' % (name, text), ['write: not all of it reached /sd0p1'], timeout=30)
            ok5 = seen(q, mark, '+SDSIM: sd0: the card was taken out')
            if check(ok and ok2 and ok3 and ok4 and ok5, tag + 'the write says "not all of it reached /sd0p1", the card is out'):
                dump = sdsim_card(q, disk)
                if dump:
                    keep = os.path.join(work, 'pulled.img')
                    shutil.copyfile(dump, keep)
                    clean, report = fsck_partition(q, dump, PART_OFFSET)
                    harmless, other = fsck_findings(report)
                    check(not other, tag + 'fsck.fat -n finds %s' % short(harmless + other))
                    if other:
                        print(report)
                    dump = keep
        finally:
            transcript += q.transcript
            q.close()
        if not dump:
            continue
        q, _, _ = stub_boot(args, dump, work, 'sdsim')          # the reboot, from the card as it was pulled
        try:
            ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
            log = q.since(0)
            ok2, out2 = q.cmd('ls /sd0p1', ['entries'])
            present = name in out2
            ok3, out3 = q.cmd('crc32 /sd0p1/KEEP.TXT' + (' /sd0p1/' + name if present else ''), ['KEEP.TXT'])
            keep_crc = zlib.crc32(b'written before the card came out\n') & 0xFFFFFFFF
            new_ok = not present or re.search(r'%08x\s+%d\s+/sd0p1/%s' % (zlib.crc32((text + '\n').encode()) & 0xFFFFFFFF,
                                                                            len(text) + 1, re.escape(name)), out3)
            check(ok and 'not unmounted cleanly' in log and ok2 and ok3 and re.search(r'%08x\s+' % keep_crc, out3) and new_ok,
                  tag + 'rebooted from it: mounted ("not unmounted cleanly"), KEEP.TXT whole, the new file %s'
                  % ('there and whole' if present else 'not there'))
        finally:
            transcript += q.transcript
            q.close()
    return transcript


def pull_others(args, disk, check, work):
    """milestone 8: the card taken out right after the first block that rm, mv into a folder, and a write over a file
    send (sdsim pull 1): what is gone must be gone from its entry before its clusters are free (an order point, audit
    B3), and a moved file must keep at least one name. fsck.fat -n on the card as it came out"""
    transcript = b''
    cases = [('rm', ['write /sd0p1/GOES.TXT a file to delete'], 'rm /sd0p1/GOES.TXT', None),
             ('mv into a folder', ['mkdir /sd0p1/Folder', 'write /sd0p1/MOVES.TXT a file that moves'],
              'mv /sd0p1/MOVES.TXT /sd0p1/Folder/Moved-file.txt', 'MOVES.TXT|Moved-file.txt|share clusters|Truncating '
              'second|File size is \\d+ bytes, cluster chain length is 0|Truncating file to 0'),
             ('write over a file', ['write /sd0p1/OVER.TXT the first text, a long one, longer than the second'],
              'write /sd0p1/OVER.TXT second text', None)]
    for what, setup, command, name_too_many in cases:
        tag = 'pull in %s: ' % what
        q, _, _ = stub_boot(args, disk, work, 'sdsim')
        try:
            ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
            for c in setup:
                ok = q.cmd(c, forbid=['not all of it', 'I/O error'])[0] and ok
            ok2, _ = q.cmd('sdsim pull 1', ['the card comes out right after'])
            mark = len(q.transcript)
            ok3, out3 = q.cmd(command, ['not all of it reached /sd0p1'], timeout=30)
            ok4 = seen(q, mark, '+SDSIM: sd0: the card was taken out')
            dump = sdsim_card(q, disk) if ok and ok2 and ok3 and ok4 else None
            if check(dump, tag + '"%s" says "not all of it reached /sd0p1", the card is out after its first block'
                     % command.split()[0]):
                clean, report = fsck_partition(q, dump, PART_OFFSET)
                harmless, other = fsck_findings(report)
                rest = [l for l in other if not (name_too_many and re.search(name_too_many, l))]
                check(not rest, tag + 'fsck.fat -n finds %s%s' % (short(harmless + other),
                      ' (a name too many: the moved file under both names, never under none)' if other and not rest else ''))
                if rest:
                    print(report)
        finally:
            transcript += q.transcript
            q.close()
    return transcript


def fill_image(src, dst, free_left, work):
    """dst: the image src with /FILLER.BIN added by mtools so that free_left clusters stay free (mtools keeps FAT32's
    FSInfo count). (the cluster size, the filler's bytes)"""
    shutil.copyfile(src, dst)
    with open(dst, 'rb') as f:
        f.seek(PART_OFFSET)
        bs = f.read(512)
        f.seek(PART_OFFSET + 512)
        free, = struct.unpack_from('<I', f.read(512), 488)
    cluster = bs[13] * 512
    filler = os.path.join(work, 'filler.bin')
    size = (free - free_left) * cluster
    with open(filler, 'wb') as f:
        f.write(b'\x5a' * size)
    r = run_mtools(['mcopy', '-i', dst + '@@%d' % PART_OFFSET, filler, '::/FILLER.BIN'])
    os.remove(filler)
    if r.returncode:
        raise RuntimeError('mcopy of the filler: %s' % r.stderr)
    return cluster, size


def full_boot(args, disk, check, work):
    """milestone 8: a full volume under QEMU (sdsim). The stick made nearly full on the host (mtools adds /FILLER.BIN,
    3003 clusters stay free: about 1.5 MB); then a 1 MB copy fits, the next does not: cp must say "No space left on
    device" at once, the part that fitted must be the source's first bytes, a write and a mkdir on the full volume
    fail the same way, rm gives the space back, and a write then works. The host checks the card"""
    img = os.path.join(work, 'full.img')
    cluster, filler_size = fill_image(disk, img, 3003, work)
    big = mkimage.foreign_big()
    q, _, _ = stub_boot(args, img, work, 'sdsim')
    try:
        ok = q.expect('+SDSIM: sd0 is the RAM disk through the SD driver', 30) and q.expect(PROMPT, 30)
        ok2, out2 = q.cmd('df', ['sd0p1'])
        m = re.search(r'sd0p1\s+fat32\s+\S+ MB\s+\S+ \S+\s+(\S+ \S+)\s', out2)
        check(ok and ok2 and m, 'full: a stick %d clusters of %d bytes from full (mtools filled the rest): df says %s free'
              % (3003, cluster, m.group(1) if m else '?'))
        ok, out = q.cmd('cp /sd0p1/BIG.BIN /sd0p1/ONE.BIN', ['1048576 bytes copied'], timeout=60)
        t0 = time.time()
        ok2, out2 = q.cmd('cp /sd0p1/BIG.BIN /sd0p1/TWO.BIN', ['cp: write: No space left on device'], timeout=60,
                          forbid=['bytes copied', 'not all of it'])
        dt = time.time() - t0
        check(ok and ok2 and dt < 30, 'full: a 1 MB cp fits; the next one says "cp: write: No space left on device" '
              'when the space runs out (%.1f s, no hang)' % dt)
        ok, out = q.cmd('crc32 /sd0p1/TWO.BIN', ['TWO.BIN'])
        m = re.search(r'([0-9a-f]{8})\s+(\d+)\s+/sd0p1/TWO.BIN', out)
        part = int(m.group(2)) if m else -1
        check(ok and m and 0 < part < len(big) and part % cluster == 0 and
              int(m.group(1), 16) == zlib.crc32(big[:part]) & 0xFFFFFFFF,
              'full: the copy that did not fit is whole up to what fitted: the first %d bytes of BIG.BIN (%d clusters, '
              'every free one)' % (part, part // cluster if part > 0 else 0))
        ok, out = q.cmd('df', ['sd0p1'])
        ok2, out2 = q.cmd('write /sd0p1/MORE.TXT one more file', ['write: No space left on device'])
        ok3, out3 = q.cmd('mkdir /sd0p1/NEWDIR', ['mkdir: /sd0p1/NEWDIR: No space left on device'])
        ok4, out4 = q.cmd('ls /sd0p1', ['entries'])
        check(ok and re.search(r'\s0 KB\s+100%', out) and ok2 and ok3 and ok4 and 'NEWDIR' not in out4,
              'full: df says 0 KB free; a write ("write: No space left on device") and a mkdir (the same) fail at once, '
              'no NEWDIR left behind')
        ok, _ = q.cmd('rm /sd0p1/TWO.BIN', forbid=['rm:', 'not all of it'])
        ok2, _ = q.cmd('write /sd0p1/MORE.TXT written after the rm', forbid=['write:', 'not all of it'])
        ok3, out3 = q.cmd('df', ['sd0p1'])
        m = re.search(r'sd0p1\s+fat32\s+\S+ MB\s+\S+ \S+\s+(\S+ \S+)\s', out3)
        check(ok and ok2 and ok3 and m and m.group(1) != '0 KB',
              'full: rm gives the space back (df: %s free) and a write works again' % (m.group(1) if m else '?'))
        ok, _ = q.cmd('eject sd0', ['sd0: safe to remove'])
        dump = save_ramdisk(q, os.path.getsize(img)) if ok else None
        if check(dump, 'full: eject: safe to remove; the card image saved'):
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            harmless, other = fsck_findings(report)
            check(clean, 'full: fsck.fat -n finds %s' % ('the volume clean' if clean else short(harmless + other)))
            if not clean:
                print(report)
            part_img = dump + '@@%d' % PART_OFFSET
            got = {}
            for name in ('BIG.BIN', 'ONE.BIN', 'MORE.TXT', 'README.TXT'):
                path = os.path.join(q.workdir, 'got.bin')
                r = run_mtools(['mcopy', '-n', '-i', part_img, '::/' + name, path])
                got[name] = open(path, 'rb').read() if r.returncode == 0 else None
            r = run_mtools(['mcopy', '-n', '-i', part_img, '::/FILLER.BIN', os.path.join(q.workdir, 'filler.bin')])
            filler_ok = r.returncode == 0 and open(os.path.join(q.workdir, 'filler.bin'), 'rb').read() == b'\x5a' * filler_size
            r = run_mtools(['mdir', '-i', part_img, '::/'])
            check(got['BIG.BIN'] == big and got['ONE.BIN'] == big and got['MORE.TXT'] == b'written after the rm\n' and
                  got['README.TXT'] is not None and filler_ok and 'TWO' not in r.stdout,
                  'full: on the host BIG.BIN, the copy that fitted, the filler and the stick\'s own files are whole, '
                  'MORE.TXT holds what was written after the rm, TWO.BIN is gone')
        check(q.poweroff() == 0, 'full: poweroff')
    finally:
        transcript = q.transcript
        q.close()
    return transcript


def add_documents(img):
    """/USERS/DOCUMENTS/NOTES.TXT put on a disk image by mtools: DOCUMENTS is not an 8.3 name, so mtools writes a
    long-name entry and the alias DOCUME~1, as the Mac does. Returns mdir's listing of /USERS."""
    part = img + '@@%d' % PART_OFFSET
    note = os.path.join(os.path.dirname(img), 'notes.txt')
    with open(note, 'w') as f:
        f.write("The owner's files live here.\n")
    for argv in (['mmd', '-i', part, '::/USERS'], ['mmd', '-i', part, '::/USERS/DOCUMENTS'],
                 ['mcopy', '-i', part, note, '::/USERS/DOCUMENTS/NOTES.TXT']):
        r = run_mtools(argv)
        if r.returncode:
            raise RuntimeError('%s: %s' % (' '.join(argv), r.stderr))
    return run_mtools(['mdir', '-i', part, '::/USERS']).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', default=os.path.join(ROOT, 'build/riscv32/qemu_virt/surfos.elf'))
    ap.add_argument('--qemu', default='qemu-system-riscv32')
    ap.add_argument('--stub', help='boards/qemu_virt/stub: the SurfBoot stand-in, for the handoff, sdsim, package and card')
    ap.add_argument('--package', help='SURFOS.BIN, booted through the stand-in where it exists (the fw track packs it)')
    ap.add_argument('--surfimg', help="the fw track's tools/surfimg.py, to verify the package")
    ap.add_argument('--mkcard', help="the fw track's tools/mkcard.py, to make a card image (skipped where absent)")
    ap.add_argument('--evidence')
    ap.add_argument('--transcript')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    results = []
    skipped = []

    def check(ok, what):
        results.append((bool(ok), what))
        print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    def skip(what):
        skipped.append(what)
        print('skip %s' % what, flush=True)

    work = tempfile.mkdtemp(prefix='surfrv-img-', dir='/tmp' if os.path.isdir('/tmp') else None)
    disk = os.path.join(work, 'foreign.img')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), 'foreign', disk], check=True,
                   stdout=subprocess.DEVNULL)
    big_crc = zlib.crc32(mkimage.foreign_big()) & 0xFFFFFFFF
    t_start = time.time()
    users = os.path.join(work, 'users.img')                 # the same stick with /USERS/DOCUMENTS/ on it
    shutil.copyfile(disk, users)
    listing = add_documents(users)
    check(re.search(r'DOCUME~1\s+<DIR>.*\bDOCUMENTS\s*$', listing, re.M),
          'image: mtools put /USERS/DOCUMENTS on the stick as a long name with the alias DOCUME~1')
    q = Qemu(args.qemu, ['-bios', 'none', '-kernel', os.path.abspath(args.kernel)], users, args.verbose)
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

        # /USERS/DOCUMENTS (BOOT-ABI.md section 5) is not an 8.3 name: only its long-name entry says DOCUMENTS
        ok, out = cmd('ls /rd0p1/USERS', ['entries'])
        check(ok and 'DOCUMENTS/' in out and 'DOCUME~1' not in out, 'ls /rd0p1/USERS: DOCUMENTS by its long name')
        ok, out = cmd('cat /rd0p1/USERS/DOCUMENTS/NOTES.TXT', ["The owner's files live here."])
        check(ok, 'cat /rd0p1/USERS/DOCUMENTS/NOTES.TXT: a path through the long name')
        ok, out = cmd('ls /rd0p1/users/documents', ['NOTES.TXT'])
        check(ok and 'entries' in out, 'ls /rd0p1/users/documents: the long name is found in any case')
        ok, out = cmd('write /rd0p1/USERS/DOCUMENTS/FROMRV.TXT written into DOCUMENTS on riscv32')
        ok2, out2 = cmd('cat /rd0p1/USERS/DOCUMENTS/FROMRV.TXT', ['written into DOCUMENTS on riscv32'])
        check(ok and ok2, 'write /rd0p1/USERS/DOCUMENTS/FROMRV.TXT and cat it')

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
        dump = save_ramdisk(q, os.path.getsize(users))
        if check(dump, 'the RAM disk saved through the monitor'):
            part = dump + '@@%d' % PART_OFFSET
            r = run_mtools(['mtype', '-i', part, '::/RISCV.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'SurfOS wrote this on riscv32', 'mtools reads RISCV.TXT as SurfOS wrote it')
            r = run_mtools(['mcopy', '-n', '-i', part, '::/rvdir/Copy_Of_Big_File.bin', os.path.join(q.workdir, 'big.bin')])
            got = open(os.path.join(q.workdir, 'big.bin'), 'rb').read() if r.returncode == 0 else b''
            check(got == mkimage.foreign_big(), 'mtools reads the 1 MB copy under its long name, byte for byte')
            r = run_mtools(['mdir', '-i', part, '::/rvdir'])
            check(r.returncode == 0 and 'renamed' not in r.stdout, 'mtools: the removed file is gone')
            r = run_mtools(['mtype', '-i', part, '::/USERS/DOCUMENTS/FROMRV.TXT'])
            check(r.returncode == 0 and r.stdout.strip() == 'written into DOCUMENTS on riscv32',
                  'mtools reads /USERS/DOCUMENTS/FROMRV.TXT as SurfOS wrote it')
            clean, report = fsck_partition(q, dump, PART_OFFSET)
            check(clean, 'fsck.fat -n: the volume is clean after the riscv32 kernel wrote it')
            if not clean:
                print(report)

        ok, out = cmd('mount rd0p1', ['rd0p1 on /rd0p1: fat32'])
        check(ok and 'not unmounted cleanly' not in out, 'mount again: clean (umount set the clean bit)')

        # a sleeping task killed: off the sleep list and gone. ps may catch it just woken ("ready", on a busy
        # host it can wait a while for its turn): looked at again until it sleeps, a few times at most
        for _ in range(10):
            ok, out = cmd('ps', ['heartbeat'])
            m = re.search(r'^\s*(\d+)\s+sleeping\s+LOW\s+.*heartbeat$', out, re.M)
            if m or not ok:
                break
            q.wait(0.2)
        if check(ok and m, 'ps: the heartbeat task sleeps between its beats'):
            ok, out = cmd('kill %s' % m.group(1), ["killing 'heartbeat'"])
            cmd('sleep 200', ['slept'])             # init collects the zombie when it next runs
            ok2, out2 = cmd('ps', ['tasks'])
            check(ok and ok2 and 'heartbeat' not in out2, 'kill: the sleeping heartbeat task is killed and collected (by init)')

        check(q.poweroff() == 0, 'poweroff: QEMU exits with status 0 (sifive_test)')
    finally:
        transcript = q.transcript
        q.close()

    try:
        transcript += scroll_boot(args, disk, check)
        transcript += fsfault_boot(args, disk, check)
        if args.stub:
            transcript += handoff_boots(args, disk, check, work)
            transcript += sdsim_boot(args, disk, check, work)
            if args.package and os.path.exists(args.package):
                transcript += package_boot(args, disk, check, work)
            else:
                skip('package: no %s: SURFOS.BIN is packed by the fw track\'s tools/surfimg.py, which this tree does '
                     'not have' % (os.path.relpath(args.package, ROOT) if args.package else 'SURFOS.BIN'))
            if args.mkcard and os.path.exists(args.mkcard):
                transcript += card_boot(args, check, work)
            else:
                skip("card: no tools/mkcard.py (the fw track's card images) in this tree")
            transcript += fault_boot(args, disk, check, work)
            transcript += pull_boot(args, disk, check, work)
            transcript += pull_points(args, disk, check, work)
            transcript += pull_others(args, disk, check, work)
            transcript += full_boot(args, disk, check, work)
        else:
            skip('handoff, sdsim, package and card: no --stub')
    finally:
        shutil.rmtree(work, ignore_errors=True)

    passed = sum(1 for ok, _ in results if ok)
    elapsed = time.time() - t_start
    summary = 'test-os qemu_virt %s: %d of %d checks passed in %.0f s' % (os.path.relpath(args.kernel, ROOT), passed,
                                                                       len(results), elapsed)
    if skipped:
        summary += ' (%d part%s skipped)' % (len(skipped), '' if len(skipped) == 1 else 's')
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
            for what in skipped:
                f.write('skip %s\n' % what)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
