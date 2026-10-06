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
import http.server
import threading
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time

# The shell's prompt: the ring-0 shell's "SurfOS*> " of old, or the SurfOS prompt (10/6/26,
# shell/prompt.c): breadcrumbs, the wave (~ in ASCII), a state character, "> ", with colour
# sequences in between when the shell colours it. The space after ">" is not required: a
# console repainted on the serial terminal (F1-F4) ends its rows at their last character. expect() takes the compiled pattern.
_SGR = rb'(?:\x1b\[[0-9;]*m)*'
PROMPT = re.compile(rb'SurfOS\*>|(?:\xe2\x89\x8b|~)' + _SGR + rb'[!#@?]?' + _SGR + rb'>')
# the pager's question after a screenful (TTY_MORE in include/surfos/tty.h): answered with Space
MORE = b'--More--'
# fdisk's question before it writes the scratch ramdisk rd1 (a 64 MB blank module, see --scratch)
CONFIRM_RD1 = 'Type rd1 to write the new partition table to rd1 (scratch.img, 64M): '
# A VGA cell's byte is a code page 437 character (driver/cp437.c, 10/6/26): the graphic set,
# with SurfOS's ▸ ≋ … at 0x10, 0xF7 and 0xF9 where VGA draws ► ≈ ∙. The empty cell reads as a space.
CP437 = (' ☺☻♥♦♣♠•◘○◙♂♀♪♫☼▸◄↕‼¶§▬↨↑↓→←∟↔▲▼' + ''.join(map(chr, range(0x20, 0x7f))) + '⌂' +
         bytes(range(0x80, 0xf7)).decode('cp437') + '≋°…' + bytes(range(0xfa, 0x100)).decode('cp437'))
assert len(CP437) == 256
# what `pagetest utf8` prints, as the console draws it: the euro sign is not in code page 437
UTF8_SAMPLE = 'UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ ? END'


def cells_text(cells):
    """VGA character bytes (every other byte of a text page) as the text the console shows"""
    return ''.join(CP437[b] for b in cells)


def answered(cmd, question, answer, expects):
    """a step whose command asks first (mkfs): wait for the question, type the answer, then expect"""
    return (cmd, [question], [(answer, expects)])


def shown(text):
    """text as the terminal gets it from surfsh: colour sequences may come between its characters"""
    return re.compile(_SGR.join(re.escape(ch.encode()) for ch in text))


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
    ('lsusb',   ['no USB host controller']),             # the USB/UEFI tests (tools/qemu-usb.py) attach USB devices
    ('lsblk',   ['rd0', 'ram', 'mounted', 'hda', 'ata', 'disk mbr', 'internal: never formatted', 'hda1', 'part mbr FAT16', 'mounted']),
    ('lsblk -s', ['READ', 'rd0', 'hda1']),
    # fdisk on the scratch ramdisk rd1; a step's third item answers its question
    ('fdisk hda new mbr', ['hda will not be changed', 'not a USB drive']),    # an internal disk
    ('fdisk rd0 new mbr', ['rd0 will not be changed', 'mounted']),             # the initrd, on /initrd
    ('fdisk rd1', ['rd1: scratch.img, 64M', 'no partition table', 'fdisk and mkfs may change rd1']),
    ('fdisk rd1 new mbr', ['change: a new, empty MBR', CONFIRM_RD1], [('rd1', ['written, flushed and verified', 'no partitions'])]),
    ('fdisk rd1 add', ['change: add partition 1: blocks 2048-131071', CONFIRM_RD1], [('rd1', ['verified', 'rd1p1 (63M)'])]),
    ('lsblk', ['rd1', 'disk mbr', 'rd1p1', 'part mbr FAT32 LBA']),
    ('fdisk rd1', ['MBR partition table', 'rd1p1', '2048', '131071', '0c FAT32 LBA', 'free: none']),
    ('fdisk rd1p1 new gpt', ['rd1p1 is a partition']),
    ('fdisk rd1 delete 1', [CONFIRM_RD1], [('rd1', ['verified', 'no partitions'])]),
    ('fdisk rd1 new gpt', [CONFIRM_RD1], [('nope', ['Cancelled'])]),         # a wrong answer writes nothing
    ('fdisk rd1', ['MBR partition table']),
    ('fdisk rd1 new gpt', [CONFIRM_RD1], [('rd1', ['verified'])]),
    ('fdisk rd1 add 16M', ['add partition 1: blocks 2048-34815', CONFIRM_RD1], [('rd1', ['verified', 'rd1p1 (16M)'])]),
    ('fdisk rd1 add', [CONFIRM_RD1], [('rd1', ['verified', 'rd1p1 (16M), rd1p2 (46M)'])]),
    ('fdisk rd1 type 2 linux', [CONFIRM_RD1], [('rd1', ['verified'])]),
    ('lsblk', ['rd1', 'disk gpt', 'rd1p1', 'part gpt basic data', 'SurfOS data', 'rd1p2', 'part gpt Linux']),
    ('fdisk rd1', ['GPT partition table, usable blocks 34-131038', 'rd1p1', 'Microsoft basic data', 'SurfOS data',
                   'rd1p2', 'Linux filesystem', 'free: none']),
    ('hexdump hda 0', ['55 aa']),
    ('mount',   ['rd0', '/initrd', 'tarfs']),
    ('ls /initrd', ['etc/', 'motd']),
    ('cat /initrd/motd', ['Welcome to SurfOS']),
    ('ls /hda1', ['DOCS/', 'README', 'motd']),
    ('cat /hda1/DOCS/README.md', ['SurfOS']),
    # 10/5/26 (USB storage): on the GPT fdisk left on rd1, FAT16 on rd1p1 and FAT32 (forced) on rd1p2,
    # used, remounted and ejected; then the whole disk as one volume, and the refusals
    answered('mkfs rd1p1 TESTVOL', 'Type rd1p1 to erase rd1p1 (16 MB) and create a FAT16 file system: ', 'rd1p1',
             ['rd1p1: FAT16, 2048-byte clusters', 'label TESTVOL']),
    ('mount rd1p1 /scratch', ['rd1p1 on /scratch: fat16, label TESTVOL', 'read-write']),
    ('write /scratch/hello.txt Hello from SurfOS', []),
    ('cp /initrd/motd /scratch/motd', ['bytes copied']),
    ('write /scratch/a-much-longer-file-name.text long names work', []),
    ('mkdir /scratch/docs', []),
    ('mv /scratch/hello.txt /scratch/docs', []),
    ('mv /scratch/docs /scratch/papers', []),
    ('ls /scratch/papers', ['hello.txt', '1 entries']),
    ('ls /scratch', ['papers/', 'motd', 'a-much-longer-file-name.text', '3 entries']),
    ('df', ['rd1p1   fat16', '/scratch', 'TESTVOL']),
    ('fsbench /scratch 4', ['read, 64 KB pieces:', 'fsbench: ok']),
    ('cd /scratch/papers', []),
    ('umount rd1p1', ['/scratch is busy: it holds the current directory']),
    ('cd /', []),
    ('umount rd1p1', []),
    ('mount rd1p1 /scratch', ['rd1p1 on /scratch: fat16, label TESTVOL']),
    ('cat /scratch/papers/hello.txt', ['Hello from SurfOS']),
    ('cat /scratch/a-much-longer-file-name.text', ['long names work']),
    ('cat /scratch/motd', ['Welcome to SurfOS']),
    ('umount /scratch', []),
    ('mount -r rd1p1 /scratch', ['rd1p1 on /scratch: fat16, label TESTVOL', 'read-only']),
    ('write /scratch/new.txt nope', ['Read-only file system']),
    ('mount rd1p2', ['rd1p2: no file system found']),
    ('umount /scratch', []),
    ('mkfs -t fat32 rd1p2 BIG32', ["the partition's type (Linux) becomes basic data", 'Type rd1p2 to erase rd1p2 ('],
     [('rd1p2', ['rd1p2: FAT32, 512-byte clusters', '(1 MiB aligned)', 'label BIG32', 'rd1p2: partition type: basic data'])]),
    ('lsblk', ['rd1p2', 'part gpt basic data']),
    ('mount rd1p2', ['rd1p2 on /rd1p2: fat32, label BIG32']),
    ('mount rd1p2 /again', ['rd1p2 is mounted already']),
    ('mount rd1 /whole', ['rd1 is mounted already, or its disk or a partition of it is']),
    ('write /rd1p2/fat32.txt FAT32 works', []),
    ('mkdir /rd1p2/dir', []),
    ('mv /rd1p2/fat32.txt /rd1p2/dir/renamed.txt', []),
    ('cat /rd1p2/dir/renamed.txt', ['FAT32 works']),
    ('df', ['rd1p2   fat32', '/rd1p2', 'BIG32']),
    ('eject rd1', ['/rd1p2 unmounted', 'rd1: safe to remove']),
    ('mount rd1p2', ['(FSInfo)', 'rd1p2 on /rd1p2: fat32, label BIG32']),     # cleanly unmounted: FSInfo is trusted
    ('cat /rd1p2/dir/renamed.txt', ['FAT32 works']),
    ('mount rd1p1 /scratch', ['rd1p1 on /scratch: fat16']),
    ('eject rd1p1', ['/rd1p2 unmounted', '/scratch unmounted', 'rd1: safe to remove']),   # the whole disk goes
    ('mkfs rd1 WHOLE', ['its partitions, which will be gone: rd1p1 rd1p2', 'Type rd1 to erase rd1 (64 MB) and create a FAT16'],
     [('rd1', ['rd1: FAT16', 'label WHOLE', 'rd1 was read again: no partitions now'])]),
    ('mount rd1p1', ["no block device 'rd1p1'"]),
    ('mount rd1 /whole', ['rd1 on /whole: fat16, label WHOLE']),
    ('write /whole/x.txt a whole-disk volume', []),
    ('cat /whole/x.txt', ['a whole-disk volume']),
    ('eject rd1', ['/whole unmounted', 'rd1: safe to remove']),
    ('fdisk rd1 new mbr', [CONFIRM_RD1], [('rd1', ['verified'])]),                  # an MBR partition of type 0c,
    ('fdisk rd1 add 32M', [CONFIRM_RD1], [('rd1', ['verified', 'rd1p1 (32M)'])]),  # small enough for FAT16
    ('mkfs rd1p1', ["the partition's type (FAT32 LBA) becomes 0e FAT16 LBA", 'Type rd1p1 to erase rd1p1 (32 MB)'],
     [('rd1p1', ['rd1p1: FAT16', 'rd1p1: partition type: FAT16 LBA'])]),
    ('lsblk', ['rd1p1', 'part mbr FAT16 LBA']),
    ('mkfs hda1', ['mkfs: hda1: it is not a USB drive']),
    ('mkfs rd0', ['mkfs: rd0: it, its disk or one of its partitions is mounted']),
    answered('mkfs rd1', 'Type rd1 to erase rd1', 'rd2', ['Cancelled']),
    ('uptime',  ['tasks']),
    ('date',    ['CMOS clock']),
    ('bootinfo', ['multiboot:', 'profile emulator', 'screen: VGA text mode']),
    ('drivers', ['parport   ok', 'usb       not present', 'console   VGA text mode', 'timer     PIT channel 0']),
    ('cpuinfo', ['physical address bits', 'from the boot loader: CR0']),
    ('acpi',    ['read with profile=laptop']),         # the USB/UEFI tests (tools/qemu-usb.py) cover the laptop profile
    ('himem',   ['no RAM above 4 GB']),
    ('dhcp',    ['lease 10.0.2.15', 'inet 10.0.2.15', 'gateway 10.0.2.2']),
    ('ping 10.0.2.2 2', ['64 bytes from 10.0.2.2', '2 packets transmitted, 2 received']),
    ('arp',     ['10.0.2.2']),
    ('httpget 10.0.2.2 {http} /smoke', ['200 OK', 'SurfOS HTTP test OK: /smoke', 'bytes received']),
    ('tcpecho 7', ['tcpecho server started']),
    ('@host-echo', []),
    ('netstat', ['tcpecho: connection from 10.0.2.2', 'tcpecho: closed, 39 bytes echoed', 'tcp  LISTEN']),
    ('run hello a b', ['Hello from user mode', 'argv[2] = "b"', 'exit status 42']),
    ('crash null', ['killed by "Page Fault at 0x00000000', 'killed]']),
    ('crash cli', ['killed by "General Protection Fault', 'killed]']),
    ('crash efault', ['= -1 (errno 14: Bad address)', 'exit status 0']),
    ('count 3 10', ['counted to 3']),
    ('ls /dev', ['console', 'null']),
    ('sh', ['$ '], [('hello from-sh', ['Hello from user mode', 'argv[1] = "from-sh"', '$ ']),
                    ('cat /initrd/etc/version', ['SurfOS 0.007', '$ ']),
                    ('exit 3', ['exit status 3'])]),
    # raw mode (tty.h): the terminal's bytes arrive unchanged, an arrow key as ESC [ A
    ('rawkeys', ['raw mode (80x25)'], [('\x1b[A\x7f\x03q', ['1b 5b 41 7f 03 71'])]),
    # escape sequences (tty.h): the test pattern, then read back from VGA memory with its colours
    ('ansi', ['ANSI-TEST done, 80x25']),
    ('@ansi-screen', []),
    ('@console-switch', []),                                 # F2, F1: the serial terminal repainted
    # 10/6/26 (the console's character set, the pager): a line of UTF-8 reaches the serial port as it
    # was printed, and VGA memory as code page 437 cells
    ('run pagetest utf8', ['UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ € END', 'exit status 0']),
    ('@utf8-screen', []),
    ('run pagetest caps', ['TTY_CAPS 3: UTF-8 colour', 'exit status 0']),     # no `ascii` on the command line
    # the pager (TTY_PAGE): --More-- after a screenful less one row, 24 lines at 80x25; answered by the
    # harness with Space, then with Enter and q, then on the PS/2 keyboard and read back from VGA memory
    ('@pager-space', []),
    ('@pager-enter-q', []),
    ('@pager-keys', []),
    ('selftest', ['SELFTEST PASS']),
    ('cat /hda1/SELFTEST.TXT', ['SurfOS wrote this file']),
    # 10/5/26: the user C library and its programs. fstest checks the file API from user space;
    # sh redirects < > >> through spawnfd(); the file tools work on files with known content.
    ('run fstest /hda1/FST', ['FSTEST PASS', 'exit status 0']),
    ('run echo one  two', ['one two', 'exit status 0']),
    ('run ls -l /hda1', ['d          0 ', 'DOCS/', 'hello', 'motd', 'README', 'entries']),
    ('sh', ['$ '], [('echo hello > /hda1/R.TXT', ['$ ']),
                    ('cat < /hda1/R.TXT', ['hello', '$ ']),
                    ('echo more >> /hda1/R.TXT', ['$ ']),
                    ('cat /hda1/R.TXT', ['hello\r\nmore', '$ ']),
                    ('cat < /hda1/R.TXT > /hda1/R2.TXT', ['$ ']),
                    ('cat /hda1/R2.TXT', ['hello\r\nmore', '$ ']),
                    ('cat < /hda1/NOPE', ['sh: /hda1/NOPE: No such file or directory', '$ ']),
                    ('wc /hda1/R.TXT', ['      2       2      11 /hda1/R.TXT', '$ ']),
                    ('mkdir -p /hda1/T/a/b', ['$ ']),
                    ('cp /hda1/R.TXT /hda1/T/a/b/C.TXT', ['$ ']),
                    ('cp -r /hda1/T/a /hda1/T/z', ['$ ']),
                    ('cat /hda1/T/z/b/C.TXT', ['hello\r\nmore', '$ ']),
                    ('mv /hda1/T/z/b/C.TXT /hda1/T/M.TXT', ['$ ']),
                    ('rmdir /hda1/T/z/b', ['$ ']),
                    ('touch /hda1/T/E.TXT', ['$ ']),
                    ('ls -l /hda1/T', ['d          0 ', 'a/', '-          0 ', 'E.TXT', '-         11 ', 'M.TXT', 'z/', '4 entries', '$ ']),
                    ('head -n 1 /hda1/T/M.TXT', ['hello\r\n/ $ ']),
                    ('hexdump /hda1/R.TXT', ['00000000  68 65 6c 6c 6f 0a 6d 6f  72 65 0a', '|hello.more.|', '0000000b', '$ ']),
                    ('rm -r /hda1/T', ['$ ']),
                    ('ls /hda1/T', ['No such file or directory', '$ ']),
                    ('df', ['Mounted on', 'rootfs', 'devfs', 'rd0', '/initrd', 'hda1', '/hda1', '$ ']),
                    ('exit', ['exit status 0'])]),
    # the text editor over the serial line (raw mode, keys as bytes): type with Enter, Backspace,
    # Home, arrows and an insertion, save, quit, check the file; reopen it, change a line, quit
    # and answer the question; a file with CRLF line ends keeps them
    ('run edit /hda1/NOTE.TXT', ['New file', '^S Save'],
     [(b'Alpha one\rGamma threex', ['Col 13']),
      (b'\x7f\x1b[HBeta two\r', ['Ln 3/3']),
      (b'\x1b[A\x1b[A\x1b[F!', ['Alpha one!']),
      (b'\x13', ['Wrote 3 lines']),
      (b'\x11', ['exit status 0'])]),
    ('cat /hda1/NOTE.TXT', ['Alpha one!\r\nBeta two\r\nGamma three\r\n']),
    ('run wc /hda1/NOTE.TXT', ['      3       6      32 /hda1/NOTE.TXT']),
    ('run edit /hda1/NOTE.TXT', ['Read 3 lines', '^S Save'],
     [(b'\x1b[B\x1b[F\x7f\x7f\x7f2', ['Beta 2']),
      (b'\x11', ['Save changes? (y/n/^C)']),
      (b'y', ['exit status 0'])]),
    ('cat /hda1/NOTE.TXT', ['Alpha one!\r\nBeta 2\r\nGamma three\r\n']),
    ('run ls /hda1/NOTE.TXT.tmp', ['No such file or directory']),
    ('sh', ['$ '], [('echo -e "first\\r\\nsecond\\r" > /hda1/CRLF.TXT', ['$ ']),
                    ('exit', ['exit status 0'])]),
    ('run edit /hda1/CRLF.TXT', ['Read 2 lines (CRLF line ends)', '^S Save'],
     [(b'\x1b[F!\x13', ['Wrote 2 lines']),
      (b'\x11', ['exit status 0'])]),
    ('run hexdump /hda1/CRLF.TXT', ['66 69 72 73 74 21 0d 0a  73 65 63 6f 6e 64 0d 0a', '|first!..second..|']),
    # 10/6/26: surfsh, the SurfOS shell, started from the ring-0 shell. Raw mode passes the bytes on as they
    # are: Tab, Up (ESC [ A), Ctrl-C. The prompt and its state, Tab for a command and a path (a second Tab
    # lists), history, a line over the 1023-byte limit (in pieces the console's input queue takes),
    # redirection of a builtin and of a program, & and a killed program, help, set, exit with a status.
    ('surfsh', [shown('SURF \u224b> ')],
     [('cd /initrd/bin', [shown('initrd \u25b8 bin \u224b> ')]),
      ('nosuchcommand', ['surfsh: nosuchcommand: command not found', shown('initrd \u25b8 bin \u224b!> ')]),
      ('pwd', ['\r\n/initrd/bin\r\n', shown('initrd \u25b8 bin \u224b> ')]),
      (b'ech\tcompleted-command\n', ['\r\ncompleted-command\r\n', shown('bin \u224b> ')]),
      ('echo history-check', ['\r\nhistory-check\r\n', shown('bin \u224b> ')]),
      (b'\x1b[A\n', ['\r\nhistory-check\r\n', shown('bin \u224b> ')]),
      (b'echo ' + b'w' * 150 + b'1', ['1']), (b'w' * 150 + b'2', ['2']), (b'w' * 150 + b'3', ['3']),
      (b'w' * 150 + b'4', ['4']), (b'w' * 150 + b'5', ['5']), (b'w' * 150 + b'6', ['6']),
      (b'w' * 150 + b'\n', ['surfsh: the line is longer than 1023 bytes: it was not run', shown('bin \u224b!> ')]),
      (b'cd /hd\t\n', [shown('hda1 \u224b> ')]),
      (b'c\t\t', ['count', 'crash', shown('hda1 \u224b> ')]),
      (b'\x03', ['^C', shown('hda1 \u224b> ')]),
      ('echo hello > /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/echo more >> /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/cat < /hda1/SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('cat SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('/initrd/bin/cat /dev/null &', ['] started', shown('hda1 \u224b> ')]),
      ('/initrd/bin/count 2 100', ['counted to 2', '] done', shown('hda1 \u224b> ')]),
      ('crash null', ['killed by "Page Fault', 'killed]', shown('hda1 \u224b!> ')]),
      ('help', ['surfsh 1.0, the SurfOS shell', 'history', "the kernel's commands", 'programs in /initrd/bin',
                'fstest', 'no variables', '(C)2004-2026 Brandon Burr.', shown('hda1 \u224b> ')]),
      ('set prompt.unicode off', [shown('hda1 ~> ')]),
      ('set prompt.unicode on', [shown('hda1 \u224b> ')]),
      ('exit 7', ['exit status 7'])]),
    ('umount /hda1', []),                                   # marks it clean for fsck.fat on the host
]
# steps that legitimately take longer than the 10 s default (by command, or by its first word)
TIMEOUTS = {'selftest': 120, 'run fstest /hda1/FST': 60, 'fsbench': 60}
# anything that means the kernel fell over
BAD = ['Kernel Wipeout', "Woah.. this ain't", 'SYSTEM HALTED', 'HALTING']


class Qemu:
    def __init__(self, kernel, qemu='qemu-system-i386', mem='64', extra=(), int_log=None, iso=None,
                 disk=None, initrd=None, disk_inplace=False, net=True, append='', scratch_mb=0):
        # UNIX socket paths are limited to 108 bytes, so keep the work directory short
        base = tempfile.gettempdir()
        if len(base) > 50 and os.path.isdir('/tmp'):
            base = '/tmp'
        self.workdir = tempfile.mkdtemp(prefix='surfos-', dir=base)
        self.mon_path = os.path.join(self.workdir, 'mon.sock')
        self.ser_path = os.path.join(self.workdir, 'ser.sock')
        boot = ['-cdrom', os.path.abspath(iso), '-boot', 'd'] if iso else ['-kernel', os.path.abspath(kernel)]
        modules = []
        if initrd and not iso:
            # a Multiboot module: the ramdisk rd0. QEMU splits -initrd at spaces (module arguments),
            # so a project path such as "SurfOS Restoration Project" needs a copy with a plain name.
            shutil.copyfile(initrd, os.path.join(self.workdir, 'initrd.tar'))
            modules.append('initrd.tar')
        if scratch_mb and not iso:
            # a blank module after it: ramdisk rd1, RAM only, which fdisk and mkfs may write
            with open(os.path.join(self.workdir, 'scratch.img'), 'wb') as f:
                f.truncate(int(scratch_mb) << 20)
            modules.append('scratch.img')
        if modules:
            boot += ['-initrd', ','.join(modules)]
        cmdline = append.split()
        self.disk = None
        if disk:
            # the guest writes to the disk, so work on a copy unless asked not to
            if disk_inplace:
                self.disk = os.path.abspath(disk)
            else:
                self.disk = os.path.join(self.workdir, 'disk.img')
                shutil.copyfile(disk, self.disk)
            boot += ['-drive', f'file={self.disk},format=raw,if=ide,index=0,media=disk']  # primary master: hda
        # user-mode network: 10.0.2.0/24, gateway 10.0.2.2; the host reaches guest port 7 through a
        # forwarded port, and the guest reaches a small HTTP server on the host (10.0.2.2) whose port
        # travels on the kernel command line
        self.ports = {}
        self.http = None
        if net:
            self.ports['echo'] = free_port()
            self.ports['http'] = free_port()
            boot += ['-netdev', f"user,id=net0,hostfwd=tcp:127.0.0.1:{self.ports['echo']}-:7",
                     '-device', 'e1000,netdev=net0,mac=52:54:00:12:34:56']
            cmdline.append(f"httpport={self.ports['http']}")
            self.http = http.server.HTTPServer(('127.0.0.1', self.ports['http']), TestHandler)
            threading.Thread(target=self.http.serve_forever, daemon=True).start()
        if cmdline and not iso:
            boot += ['-append', ' '.join(cmdline)]
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
        self.more_at, self.auto_more = 0, True

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
                self.answer_more()

    def answer_more(self):
        """the pager waits for a key after a screenful: Space, unless a test turned this off"""
        while self.auto_more:
            i = self.transcript.find(MORE, self.more_at)
            if i < 0:
                return
            self.more_at = i + len(MORE)
            self.ser.sendall(b' ')

    def expect(self, text, timeout=10):
        """wait until text (a string, or a compiled bytes pattern such as PROMPT) shows up in
        serial output after the previous match"""
        deadline = time.time() + timeout
        while True:
            if isinstance(text, re.Pattern):
                m = text.search(self.transcript, self.cursor)
                if m:
                    self.cursor = m.end()
                    return True
            else:
                needle = text.encode()
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
        return re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', t)

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

    def vga_memory(self):
        """the 80x25 VGA text page: character and attribute bytes, None if the dump fails.
        The file name is relative on purpose: the monitor treats a leading '/' as a format
        specifier."""
        path = os.path.join(self.workdir, 'vga.bin')
        if os.path.exists(path):
            os.remove(path)
        self.monitor('pmemsave 0xb8000 4000 vga.bin')
        for _ in range(50):
            if os.path.exists(path) and os.path.getsize(path) == 4000:
                return open(path, 'rb').read()
            time.sleep(0.1)
        return None

    def screen(self):
        """the 80x25 VGA text screen"""
        d = self.vga_memory()
        if d is None:
            return '(screen dump failed)'
        rows = [cells_text(d[r * 160:(r + 1) * 160:2]).rstrip() for r in range(25)]
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
        if self.http:
            self.http.shutdown()
        shutil.rmtree(self.workdir, ignore_errors=True)


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


class TestHandler(http.server.BaseHTTPRequestHandler):
    """what the guest's httpget and self test fetch"""
    def do_GET(self):
        body = f'SurfOS HTTP test OK: {self.path}\n'.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def host_echo_check(q):
    """connect through the forwarded port to the guest's tcpecho server"""
    port = q.ports.get('echo')
    if not port:
        return ('host reaches the guest TCP echo server', False)
    msg = b'surfos echo test over a forwarded port\n'
    got = b''
    for attempt in range(10):
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=3)
            s.settimeout(5)
            s.sendall(msg)
            while len(got) < len(msg):
                d = s.recv(1024)
                if not d:
                    break
                got += d
            s.close()
            break
        except OSError:
            time.sleep(0.3)
    time.sleep(0.5)                 # let the guest print that the connection closed
    return ('host reaches the guest TCP echo server', got == msg)


# what the shell command `ansi` draws with escape sequences (row: text), 80x25
ANSI_ROWS = {0: 'ANSI-TEST top left', 2: '         INVERSE normal', 3: '         YELLOW-ON-BLUE', 4: '         KEEP',
             6: '         D C   E', 7: '         A', 8: '          B', 10: '         UNKNOWN-IGNORED',
             12: 'ANSI-TEST done, 80x25'}


def ansi_screen_check(q):
    """after `ansi`: its pattern in VGA memory, characters and attributes, and Z in the corner"""
    d = q.vga_memory() or bytes(4000)

    def row(r):
        return cells_text(d[r * 160:(r + 1) * 160:2]).rstrip()

    def attr(c, r):
        return d[(r * 80 + c) * 2 + 1]
    ok = all(row(r) == text for r, text in ANSI_ROWS.items()) and d[(24 * 80 + 79) * 2] == ord('Z')
    ok = ok and attr(9, 2) == 0x70 and attr(16, 2) == 0x07 and attr(9, 3) == 0x1E   # inverse, normal, bright yellow on blue
    return ('escape sequences on the VGA screen: position, moves, erase, inverse, colours, the corner', ok)


def console_switch_check(q):
    """F2 and back to F1 on the PS/2 keyboard, after `ansi`: the serial terminal is repainted
    from the console's buffer, down to its last row in use (Z in the corner, row 25), and the
    cursor goes back up to the prompt on row 14, column 10"""
    q.sendkeys('<f2>')
    ok = q.expect('Kernel version', 10) and q.expect(PROMPT, 10)
    q.sendkeys('<f1>')
    ok = ok and q.expect('ANSI-TEST top left', 10) and q.expect('Z\r\x1b[11A\x1b[9C', 10)
    ok = ok and 'ANSI-TEST done, 80x25' in q.screen()
    return ('F2 and F1: the serial terminal is repainted and the cursor put back', ok)


def utf8_screen_check(q):
    """after `run pagetest utf8`: its line read back from VGA memory, cell by cell"""
    return ('UTF-8 on the VGA console: ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ as code page 437 cells, € as ?',
            UTF8_SAMPLE in q.screen().splitlines())


def pagetest_lines(text):
    """the numbers of the `pagetest N` lines in a piece of output"""
    return [int(n) for n in re.findall(r'pagetest line (\d{3})', text)]


def pager_space_check(q):
    """pagetest 60, the harness answering --More-- with Space: the question after lines 1-24,
    again after 25-48, then the rest, and paging ends with the program"""
    start = len(q.transcript)
    q.send('run pagetest 60\n')
    ok = q.expect('PAGETEST DONE', 20) and q.expect('exit status 0', 10) and q.expect(PROMPT, 10)
    parts = q.transcript[start:].decode(errors='replace').split(MORE.decode())
    ok = ok and [pagetest_lines(p) for p in parts] == [list(range(1, 25)), list(range(25, 49)), list(range(49, 61))]
    return ('pager: --More-- after 24 lines at 80x25, Space shows the next 24, the rest follows', ok)


def pager_enter_quit_check(q):
    """the harness's Space off: Enter at --More-- shows one more line and asks again, q drops the
    rest of the program's output, and the shell's prompt comes back"""
    q.auto_more = False
    try:
        start = len(q.transcript)
        q.send('run pagetest 60\n')
        ok = q.expect('--More--', 20)
        if ok:
            q.send('\r')
            ok = q.expect('pagetest line 025', 10) and q.expect('--More--', 10)
        if ok:
            q.send('q')
            ok = q.expect('exit status 0', 20) and q.expect(PROMPT, 10)
        out = q.transcript[start:].decode(errors='replace')
        parts = out.split(MORE.decode())
        ok = ok and [pagetest_lines(p) for p in parts] == [list(range(1, 25)), [25], []] and 'PAGETEST DONE' not in out
    finally:
        q.more_at, q.auto_more = len(q.transcript), True
    return ('pager: Enter shows one more line, q drops the rest, the prompt comes back', ok)


def pager_keys_check(q):
    """the pager on the PS/2 keyboard, read back from VGA memory: after `clear`, lines 1-24 and the
    question in inverse video on the last row; Space the next 24, Enter one more, q the end"""
    def vga_until(test, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            d = q.vga_memory()
            if d and test([cells_text(d[r * 160:(r + 1) * 160:2]).rstrip() for r in range(25)], d):
                return True
            q.pump(0.3)
        return False

    def asking_after(last):
        return lambda rows, d: (rows[24].startswith('--More--') and d[24 * 160 + 1] == 0x70 and
                                rows[:24] == ['pagetest line %03d' % n for n in range(last - 23, last + 1)])
    q.auto_more = False
    try:
        start = len(q.transcript)
        q.sendkeys('clear\n')
        ok = q.expect(PROMPT, 10)
        q.sendkeys('run pagetest 60\n')
        ok = ok and vga_until(asking_after(24))
        if ok:
            q.sendkeys(' ')
            ok = vga_until(asking_after(48))
        if ok:
            q.sendkeys('\n')
            ok = vga_until(asking_after(49))
        if ok:
            q.sendkeys('q')
            ok = q.expect('exit status 0', 20) and q.expect(PROMPT, 10)
        ok = ok and 'PAGETEST DONE' not in q.transcript[start:].decode(errors='replace')
    finally:
        q.more_at, q.auto_more = len(q.transcript), True
    return ('pager on the PS/2 keyboard: --More-- in inverse video on the last row, Space, Enter, q', ok)


# the steps of the console's character set and the pager (10/6/26) that the host carries out
CONSOLE_CHECKS = {'@utf8-screen': utf8_screen_check, '@pager-space': pager_space_check,
                  '@pager-enter-q': pager_enter_quit_check, '@pager-keys': pager_keys_check}


def host_checks(q):
    """after the guest has run: read what it wrote to the disk with the host's mtools"""
    if not q.disk or not shutil.which('mcopy'):
        return []
    q.shutdown()
    with open(q.disk, 'rb') as f:
        mbr = f.read(512)
    start = int.from_bytes(mbr[454:458], 'little') * 512     # partition 1
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')

    def read(name):
        try:
            return subprocess.run(['mcopy', '-i', f'{q.disk}@@{start}', '::/' + name, '-'],
                                  env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20).stdout
        except Exception as e:
            return str(e).encode()
    results = [('FAT file written by the self test is readable with mtools', read('SELFTEST.TXT') == b'SurfOS wrote this file.\n'),
               ('the files the editor saved are exact, line ends included (mtools)',
                read('NOTE.TXT') == b'Alpha one!\nBeta 2\nGamma three\n' and read('CRLF.TXT') == b'first!\r\nsecond\r\n')]
    try:                                                    # the UTF-8 name the self test made, through mtools' UTF-16
        out = subprocess.run(['mdir', '-i', f'{q.disk}@@{start}', '::/'], env=dict(env, LC_ALL='C.UTF-8'),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20).stdout
    except Exception as e:
        out = str(e).encode()
    results.append(('mtools lists the UTF-8 name caf\u00e9.txt', 'caf\u00e9.txt'.encode() in out))
    return results + fsck_check(q.disk, start, int.from_bytes(mbr[458:462], 'little') * 512)


def fsck_check(disk, start, size):
    """fsck.fat (dosfstools) on a copy of the partition: what SurfOS wrote must be consistent"""
    if not shutil.which('fsck.fat') or not size:
        return []
    part = os.path.join(os.path.dirname(disk), 'part.img')
    with open(disk, 'rb') as f, open(part, 'wb') as o:
        f.seek(start)
        o.write(f.read(size))
    try:
        r = subprocess.run(['fsck.fat', '-n', part], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        ok = r.returncode == 0
        if not ok:
            print(r.stdout.decode(errors='replace'))
    except Exception as e:
        print(e)
        ok = False
    return [('fsck.fat finds the FAT volume SurfOS wrote consistent', ok)]


def tail(text, n=40):
    lines = text.splitlines()
    return '\n'.join(lines[-n:])


def run_smoke(q, boot_timeout):
    results = [('boot to prompt (serial)', q.expect(PROMPT, boot_timeout))]
    if results[0][1]:
        for entry in SMOKE:
            cmd, expects = entry[0], entry[1]
            follow = entry[2] if len(entry) > 2 else []      # (line, expects) pairs typed after cmd
            if cmd == '@host-echo':                          # a step the host performs while the guest runs
                results.append(host_echo_check(q))
                continue
            if cmd == '@ansi-screen':                        # the screen the previous step drew
                results.append(ansi_screen_check(q))
                continue
            if cmd == '@console-switch':
                results.append(console_switch_check(q))
                continue
            if cmd in CONSOLE_CHECKS:
                results.append(CONSOLE_CHECKS[cmd](q))
                continue
            cmd = cmd.format(**q.ports) if q.ports else cmd
            limit = TIMEOUTS.get(cmd, TIMEOUTS.get(cmd.split()[0], 10))
            q.send(cmd + '\n')
            ok = all(q.expect(e, limit) for e in expects)
            for line, exp in follow:
                if isinstance(line, bytes):                  # keys as they are, for a full-screen program
                    q.ser.sendall(line)
                else:
                    q.send(line + '\n')
                ok = ok and all(q.expect(e, limit) for e in exp)
            ok = ok and q.expect(PROMPT, limit)
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
    ap.add_argument('--no-net', action='store_true', help='no user-mode network, port forward or HTTP server')
    ap.add_argument('--qemu', default='qemu-system-i386')
    ap.add_argument('--mem', default='64')
    ap.add_argument('--extra', default='', help='extra QEMU arguments')
    ap.add_argument('--append', default='', help='kernel command line words (for example profile=laptop)')
    ap.add_argument('--int-log', help='write the QEMU interrupt/exception log here (-d int)')
    ap.add_argument('--scratch', type=int, default=0, metavar='MB',
                    help='a blank module of this many MB after the initrd: a scratch ramdisk (rd1) for fdisk/mkfs')
    ap.add_argument('--boot-timeout', type=float, default=20)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--test', action='store_true', help='smoke test over the serial console')
    mode.add_argument('--shell', nargs='+', metavar='CMD', help='run shell commands over serial')
    mode.add_argument('--screen', action='store_true', help='dump the VGA text screen')
    ap.add_argument('--keys', default='', help='with --screen: keys to type first (see sendkeys)')
    ap.add_argument('--wait', type=float, default=4, help='with --screen: seconds to wait for boot')
    a = ap.parse_args()

    q = Qemu(a.kernel, a.qemu, a.mem, a.extra.split(), a.int_log, a.iso, a.disk, a.initrd, a.disk_inplace, not a.no_net,
             a.append, a.scratch)
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
