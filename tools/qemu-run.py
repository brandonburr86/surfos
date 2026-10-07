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
import io
import threading
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import tarfile
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


# 10/6/26: the 2026 shell sh (user/bin/sh.c) in the root directory, which init runs in a boot where it is surfsh
SH_PROMPT = re.compile(rb'/ \$ ')


def own_shell_killed(q):
    """the line that has sh run kcmd kill on sh itself: its pid from its latest banner"""
    pids = re.findall(r'SurfOS user shell \(pid (\d+)\)', q.text())
    return 'kcmdtest run - kill ' + (pids[-1] if pids else '0')


def surf_prompt(crumbs, state):
    """10/6/26: the SurfOS prompt as a shell writes it to the serial port: the breadcrumbs (bytes), the wave and
    the state character, colour sequences between"""
    return re.compile(re.escape(crumbs) + rb' \xe2\x89\x8b' + _SGR + re.escape(state) + _SGR + rb'>')


# (command, strings that must appear, in order, before the next prompt)
SMOKE = [
    ('tick',    ['Ticks:']),
    ('memstat', ['Physical memory:', 'Free physical pages:']),
    ('ps',      ['Kernel Idle Task', 'init', 'surfsh (ring 3)']),                 # 10/6/26: surfsh on every console
    ('help',    ['surfsh 1.0, the SurfOS shell', 'lsblk', '(C)2004-2026 Brandon Burr.']),   # paged: Space from the harness
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
    # 10/6/26 (the telnet server, track N): network connections for programs (surfos/sock.h), with nettest: the
    # interface; an echo server the host reaches through the forwarded port 23, and a megabyte the host reads slowly;
    # an HTTP GET from the harness's server; a refused connection, and one to an address that does not answer ARP;
    # the calls' errors and limits; a kill and Ctrl+C while accept() waits. Port 23 is free again at the end.
    ('nettest info', ['eth0: 52:54:00:12:34:56, link up', 'inet 10.0.2.15']),
    ('@nettest-host', []),
    ('nettest get 10.0.2.2 {http} /nettest', ['HTTP/1.0 200 OK', 'SurfOS HTTP test OK: /nettest', 'bytes received']),
    ('nettest refuse 10.0.2.2 1', ['connect to 10.0.2.2 port 1: Connection refused']),
    ('nettest connect 10.0.2.99 23', ['connect to 10.0.2.99 port 23: No route to host']),
    ('nettest limits', ['NETTEST PASS']),
    ('@nettest-kills', []),
    # TCP against an other end that makes its segments by hand (tools/qemu-tcp.py, tools/rawnet.py): a boot of its
    # own whose NIC is on a raw link, shared by these steps; the scenarios are listed in tools/qemu-tcp.py
    ('@tcp-raw scan', []), ('@tcp-raw flood', []), ('@tcp-raw reset', []), ('@tcp-raw greet', []),
    ('@tcp-raw window', []), ('@tcp-raw fin', []), ('@tcp-raw keepalive', []), ('@tcp-raw churn', []),
    ('@tcp-raw reserve', []), ('@tcp-raw sws', []), ('@tcp-raw linger', []), ('@tcp-raw pollscan', []),
    ('@tcp-raw pollstall', []), ('@tcp-raw end', []),
    ('run hello a b', ['Hello from user mode', 'argv[2] = "b"', 'exit status 42']),
    ('crash null', ['killed by "Page Fault at 0x00000000', 'killed]']),
    ('crash cli', ['killed by "General Protection Fault', 'killed]']),
    ('run crash efault', ['= -1 (errno 14: Bad address)', 'exit status 0']),   # run: the kernel says the status
    ('count 3 10', ['counted to 3']),
    ('ls /dev', ['console', 'null']),
    ('sh', ['$ '], [('hello from-sh', ['Hello from user mode', 'argv[1] = "from-sh"', '$ ']),
                    ('cat /initrd/etc/version', ['SurfOS 1.0 (ai-dev)', '$ ']),
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
    # after q, a question the program asks before it reads still shows, and so does its crash report
    ('@pager-q-ask', []),
    ('@pager-q-crash', []),
    # a job in the background that turned paging on does not keep the shell's next command from being paged
    ('@pager-background', []),
    # what is typed for a kernel command whose output goes to a file is shown, and is not in the file
    ('@kcmd-echo', []),
    # `ascii` on the kernel command line: TTY_CAPS says colour only, and surfsh draws its prompt in ASCII
    ('@boot ascii', [re.compile(rb'SURF ~' + _SGR + rb'>')], [('pagetest caps', ['TTY_CAPS 2: colour'])], []),
    ('selftest', ['SELFTEST PASS']),
    ('cat /hda1/SELFTEST.TXT', ['SurfOS wrote this file']),
    # 10/6/26 (surfsh, track K): a kernel command's status, 0 success, 1 failure (a refusal too), 2 a usage
    # error; $? in the ring-0 shell; 127 for no such command; a program's exit status through run
    ('echo status $?', ['status 0']),
    ('ls /hda1/no-such-file', ['No such file or directory']),
    ('echo status $?', ['status 1']),
    ('fdisk hda new mbr', ['hda will not be changed']),
    ('echo status $?', ['status 1']),
    ('fdisk', ['fdisk <disk> new mbr|gpt']),
    ('echo status $?', ['status 2']),
    ('no-such-command', ['surfsh: no-such-command: command not found']),
    ('echo status $?', ['status 127']),
    ('run hello', ['exit status 42']),
    ('echo status $?', ['status 42']),
    # the bridge (SYS_kcmd, SYS_kcmdinfo) from user space: statuses, errors, run, redirection, cut output, the table
    ('run kcmdtest /hda1', ['KCMDTEST PASS', 'exit status 0']),
    # one self test or heaptest at a time, since they share state: four heaptests at once from four processes;
    # a self test run through kcmd by a process with a child of its own leaves that child to it
    ('run kcmdtest together /hda1', ['four heaptests at once', 'KCMDTEST PASS', 'exit status 0']),
    ('run kcmdtest child-selftest /hda1/KSELF.TXT',
     ["kcmd selftest: status 0; this process's own child: collected, exit status 42", 'exit status 0']),
    # a question while the output goes to a file: seen and answered on the console; the rest is in the file
    ('run kcmdtest run /hda1/FDISK.TXT fdisk rd1 new gpt', [CONFIRM_RD1], [('nope', ['kcmdtest: fdisk: status 1'])]),
    ('cat /hda1/FDISK.TXT', ['rd1: scratch.img, 64M', 'change: ', 'Cancelled']),
    ('rm /hda1/FDISK.TXT', []),
    # three pings at once from three processes take turns: every request is answered
    ('run kcmdtest pings /hda1 10.0.2.2', ['three pings at once: every request answered', 'KCMDTEST PASS', 'exit status 0']),
    # a report that is not fatal (crashint's unexpected vector) reaches the screen with the output going to a file
    ('run kcmdtest run /hda1/CRASHINT.TXT crashint', ['Unexpected trap', 'int 0x50 returned', 'kcmdtest: crashint: status 0']),
    ('rm /hda1/CRASHINT.TXT', []),
    # a kernel command that faults kills the process that ran it, with its fault report on the screen even though
    # its output went to a file; the kernel lets go of that file, so the volume unmounts and mounts again
    ('mount rd1p1 /kcmd', ['rd1p1 on /kcmd: fat16']),
    ('run kcmdtest crash /kcmd/CRASH.TXT', ['General Protection Fault', 'killed]']),
    ('umount /kcmd', []),
    ('mount rd1p1 /kcmd', ['rd1p1 on /kcmd: fat16']),
    ('umount /kcmd', []),
    # the question answered as asked, with the output going to a file: the change is made, and the file says so
    ('run kcmdtest run /hda1/FDISK.TXT fdisk rd1 new mbr', [CONFIRM_RD1], [('rd1', ['kcmdtest: fdisk: status 0'])]),
    ('cat /hda1/FDISK.TXT', ['change: a new, empty MBR', 'rd1: written, flushed and verified', 'rd1 now has no partitions']),
    ('rm /hda1/FDISK.TXT', []),
    # 10/6/26 (the kill that waits): a kill of a process inside a kernel command waits for the command to end; the
    # command gives up where it waits and lets go of what it holds. kcmdtest kills: a file held open, fdisk at its
    # question, a sleep, a ping, a self test (which lets go of its claim), each ended at once; a sleep nothing cuts
    # short, killed at once after 2 s with a kernel log line. The volume of the file held open unmounts afterwards.
    ('run kcmdtest kills /hda1 10.0.2.2', ['cp /dev/tty', 'KCMDTEST PASS', 'exit status 0']),
    ('umount /hda1', []),
    ('mount hda1 /hda1', ['hda1 on /hda1: fat16']),
    # Ctrl+C on the serial line: a kernel command surfsh runs (ping) is cut short, a program (count) killed
    ('@ctrl-c', []),
    # the prompt's breadcrumbs, whichever shell init started on F1
    ('cd /initrd/bin', [re.compile(rb'initrd \xe2\x96\xb8 bin \xe2\x89\x8b')], [('cd /', [])]),
    # 10/6/26 (the telnet server's track P): pseudo-terminals, poll(), the process list, detach() and /dev/kmsg,
    # from user space (user/bin/ptytest.c). A program on a pty and what its master read; surfsh driven through
    # one (a command, Ctrl+C on a kernel command and on a program, dmesg in the kernel's colour, exit: the master
    # reads 0); PTY_SETSIZE and what TTY_GETSIZE and TTY_CAPS answer there; the pager asking in the stream at
    # 80x10; a program that waits for room in the master's buffer; the hangup; poll(); procinfo(); detach();
    # a line written to /dev/kmsg is in dmesg and on no console
    ('run ptytest run hello pty', ['Hello from user mode', 'argv[1] = "pty"', 'ptytest: exit status 42',
                                   'PTYTEST run PASS', 'exit status 0']),
    ('run ptytest surfsh', ['ptytest: typed "echo hello"', 'ptytest: Ctrl+C: sleep cut short, count killed',
                            'in the kernel\'s colour', 'ptytest: exit typed: the master read 0', 'PTYTEST surfsh PASS',
                            'exit status 0']),
    ('run ptytest size 100 30', ['sizes out of range refused', 'TTY_GETSIZE 100x30', 'TTY_CAPS 3: UTF-8 colour',
                                 'PTYTEST size PASS', 'exit status 0']),
    ('run ptytest pager', ['the lines between them: 1-9 10-18 19-27 28-36 37-45 46-54 55-60', 'PTYTEST pager PASS',
                           'exit status 0']),
    ('run ptytest flood', ['lines 1-1000, all of them, in order', 'PTYTEST flood PASS', 'exit status 0']),
    ('run ptytest hangup', ['count: gone', 'pagetest at --More--: gone', 'pagetest waiting for room: gone',
                            'each pty is free again', 'PTYTEST hangup PASS', 'exit status 0']),
    # 10/6/26 (the review): the hangup's kill waits for a kernel command however long; a kernel thread started in a
    # session is on F1; a second session starts clean; a master's write never waits (a full queue: a short count,
    # EAGAIN, POLLOUT again), and Ctrl+C drops what was typed ahead; Ctrl+C does not reach a detached process; a
    # session is kept apart from F1 (its output, F1's keys) and poll() sees a key queued on the console
    ('run ptytest hangup-call', ['still there', 'the kernel log says the kill waited', 'PTYTEST hangup-call PASS',
                                 'exit status 0']),
    ('@pty-tcpecho', []),
    ('run ptytest reuse', ['line mode, and only its own keys', 'opened again: 80x24', 'PTYTEST reuse PASS', 'exit status 0']),
    ('run ptytest fullqueue', ['the input queue is full', 'EAGAIN, and no POLLOUT', 'POLLOUT came back after',
                               'what was typed ahead never ran', 'PTYTEST fullqueue PASS', 'exit status 0']),
    ('run ptytest detach-ctrlc', ['detached from surfsh', 'the detached sleeper lived on', 'PTYTEST detach-ctrlc PASS',
                                  'exit status 0']),
    ('@pty-isolate', []),
    ('run ptytest poll', ['timed out after', 'POLLNVAL', 'POLLHUP at once', '(POLLIN | POLLHUP)', 'two masters and the console',
                          'killed: gone after', 'PTYTEST poll PASS', 'exit status 0']),
    ('run ptytest ps', ['init', 'ptytest', 'PTYTEST ps PASS', 'exit status 0']),
    ('run ptytest detach', ['detached: parent 1', 'ECHILD', 'PTYTEST detach PASS', 'exit status 0']),
    ('@kmsg', []),
    # boots of their own (@boot: kernel command-line words; no-initrd; surfsh=<program or file in the initrd>, an
    # initrd whose bin/surfsh is that), for init's choice of shell. shell=kernel: the ring-0 shell on every console
    # on purpose, elevated (#), no recovery line, and started again elevated after crashgp; no initrd: no
    # /initrd/bin/surfsh, a recovery console (?) at once with init's line; a surfsh that ends at once (hello: 42):
    # started again twice, then the recovery console; one that is not an executable: the recovery console at once;
    # one that runs (sh): an exit after 2 s with status 3 is said, with 0 not, a kill is said ('@sleep n' waits)
    # 10/6/26: Ctrl+C there cuts a command short with status 130: a sleep, and a program it runs (killed)
    ('@boot shell=kernel', [surf_prompt(b'SURF', b'#')], [('cd /initrd/bin', [surf_prompt(b'initrd \xe2\x96\xb8 bin', b'#')]),
                                                          (b'sleep 10000\r', []), ('@sleep 2', []),
                                                          (b'\x03', ['sleep cut short', surf_prompt(b'bin', b'#')]),
                                                          ('echo status $?', ['status 130']),
                                                          (b'count 100 100\r', [' 5 ']),
                                                          (b'\x03', ['killed]', surf_prompt(b'bin', b'#')]),
                                                          ('echo status $?', ['status 130']),
                                                          ('crashgp', ['General Protection Fault', 'init: restarted the shell',
                                                                       surf_prompt(b'SURF', b'#')])],
     ['recovery console']),
    ('@boot no-initrd', ['init: /initrd/bin/surfsh: No such file or directory; recovery console', surf_prompt(b'SURF', b'?')],
     [('cd /dev', [surf_prompt(b'dev', b'?')]), ('ls /nothing', ['No such file']), ('echo status $?', ['status 1'])], []),
    ('@boot surfsh=hello', ['Hello from user mode', 'init: surfsh exited at once (status 42); restarted the shell',
                            'init: surfsh exited at once (status 42); restarted the shell',
                            'init: surfsh exited at once 3 times in 30 s (status 42); recovery console',
                            surf_prompt(b'SURF', b'?')], [('ps', ['Shell 0'])], []),
    ('@boot surfsh=etc/version', ['init: /initrd/bin/surfsh could not be loaded; recovery console', surf_prompt(b'SURF', b'?')],
     [], ['restarted the shell']),
    ('@boot surfsh=sh', ['SurfOS user shell', SH_PROMPT],
     [('@sleep 2.5', []), ('exit 3', ['init: surfsh exited (status 3); restarted the shell', 'SurfOS user shell', SH_PROMPT]),
      ('@sleep 2.5', []), ('exit', ['SurfOS user shell', SH_PROMPT]),
      (own_shell_killed, ['init: surfsh was killed; restarted the shell', 'SurfOS user shell', SH_PROMPT])],
     ['exited (status 0)', 'recovery console']),
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
                    ('exit', [])]),                                  # status 0: surfsh says nothing
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
                    ('exit', [])]),
    ('run edit /hda1/CRLF.TXT', ['Read 2 lines (CRLF line ends)', '^S Save'],
     [(b'\x1b[F!\x13', ['Wrote 2 lines']),
      (b'\x11', ['exit status 0'])]),
    ('run hexdump /hda1/CRLF.TXT', ['66 69 72 73 74 21 0d 0a  73 65 63 6f 6e 64 0d 0a', '|first!..second..|']),
    # 10/6/26: surfsh, the SurfOS shell, started inside the one init runs on F1. Raw mode passes the bytes on as
    # they are: Tab, Up (ESC [ A), Ctrl-C. The prompt and its state, $?, Tab for a command (the kernel's too) and a
    # path (a second Tab lists), history, a line over the 1023-byte limit (in pieces the console's input queue
    # takes), a kernel command's status and its redirection, < refused for it, a builtin and a program redirected,
    # & reported at the next prompt, a killed program, help paged (the harness answers --More--), set, exit.
    ('surfsh', [shown('SURF \u224b> ')],
     [('cd /initrd/bin', [shown('initrd \u25b8 bin \u224b> ')]),
      ('nosuchcommand', ['surfsh: nosuchcommand: command not found', shown('initrd \u25b8 bin \u224b!> ')]),
      ('echo status $? "$?" \'$?\'', ['\r\nstatus 127 127 $?\r\n', shown('initrd \u25b8 bin \u224b> ')]),
      ('pwd', ['\r\n/initrd/bin\r\n', shown('initrd \u25b8 bin \u224b> ')]),
      (b'ech\tcompleted-command\n', ['\r\ncompleted-command\r\n', shown('bin \u224b> ')]),
      ('echo history-check', ['\r\nhistory-check\r\n', shown('bin \u224b> ')]),
      (b'\x1b[A\n', ['\r\nhistory-check\r\n', shown('bin \u224b> ')]),
      (b'echo ' + b'w' * 150 + b'1', ['1']), (b'w' * 150 + b'2', ['2']), (b'w' * 150 + b'3', ['3']),
      (b'w' * 150 + b'4', ['4']), (b'w' * 150 + b'5', ['5']), (b'w' * 150 + b'6', ['6']),
      (b'w' * 150 + b'\n', ['surfsh: the line is longer than 1023 bytes: it was not run', shown('bin \u224b!> ')]),
      (b'cd /hd\t\n', [shown('hda1 \u224b> ')]),
      (b'c\t\t', ['cpuinfo', shown('hda1 \u224b> ')]),
      (b'\x03', ['^C', shown('hda1 \u224b> ')]),
      ('ls no-such-file', ['No such file or directory', shown('hda1 \u224b!> ')]),
      ('echo status $?', ['\r\nstatus 1\r\n', shown('hda1 \u224b> ')]),
      (b'lsbl\t\n', ['part mbr FAT16', shown('hda1 \u224b> ')]),
      ('lsblk > LSBLK.TXT; echo status $?', ['\r\nstatus 0\r\n', shown('hda1 \u224b> ')]),
      ('cat LSBLK.TXT', ['rd0', 'part mbr FAT16', shown('hda1 \u224b> ')]),
      ('lsblk < LSBLK.TXT', ['surfsh: lsblk is a kernel command: it reads only the console', shown('hda1 \u224b!> ')]),
      ('ls < LSBLK.TXT', ['use /initrd/bin/ls for that', shown('hda1 \u224b!> ')]),
      ('echo hello > /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/echo more >> /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/cat < /hda1/SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('cat SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('/initrd/bin/count 1 200 > /dev/null &', ['] started', shown('hda1 \u224b> ')]),
      ('sleep 1000', ['slept 1000 ms', '] done', shown('hda1 \u224b> ')]),      # reported before the next prompt
      ('crash null', ['killed by "Page Fault', 'killed]', shown('hda1 \u224b!> ')]),
      ('echo status $?', ['\r\nstatus 137\r\n', shown('hda1 \u224b> ')]),
      ('help', ['surfsh 1.0, the SurfOS shell', "the kernel's commands", '--More--', 'lsblk', 'programs in /initrd/bin',
                'fstest', '$? is the one expansion', '(C)2004-2026 Brandon Burr.', shown('hda1 \u224b> ')]),
      ('set prompt.unicode off', [shown('hda1 ~> ')]),
      ('set prompt.unicode=on', [shown('hda1 \u224b> ')]),
      ('exit 7', ['exit status 7'])]),
    ('@surfsh-corner', []),                                 # surfsh draws only where its line is
    # 10/6/26: telnetd, the telnet server (user/telnetd/), through the forwarded port 23 with the harness's telnet
    # client (Telnet, below). Not running at first; on; on again (running already, status 1); ports that are none
    # and --server typed by hand (status 2); a session: the banner, a command, cd, Tab, help paged, Ctrl+C, exit; two sessions at once, status,
    # off ending both, on again; four, and a fifth refused; a client that stops reading while a second session is
    # served; a client whose keys flood a session that reads none; telnetd off typed in a session; on while nettest has
    # the port; off when not running (status 1); the server's log.
    ('telnetd', ['telnetd: not running (telnetd on starts it)']),
    ('telnetd on', ['telnetd: listening on 10.0.2.15 port 23']),
    ('run telnetd on', ['telnetd: running already (pid', 'exit status 1']),
    ('run telnetd on 65536', ['telnetd: 65536 is no port (1-65535)', 'exit status 2']),
    ('run telnetd on +23', ['telnetd: +23 is no port (1-65535)', 'exit status 2']),
    ('run telnetd --server 23', ['telnetd: --server is how telnetd on starts the server', 'exit status 2']),
    ('@telnet-session', []),
    ('@telnet-two', []),
    ('@telnet-full', []),
    ('@telnet-stalled', []),
    ('@telnet-flood', []),
    ('@telnet-off-inside', []),
    ('@telnet-port-busy', []),
    ('run telnetd off', ['telnetd: not running', 'exit status 1']),
    ('dmesg', ['telnetd: listening on port 23', 'telnetd: session 1 from 10.0.2.2:', 'telnetd: session 1 ended (the shell',
               'telnetd: telnetd off stops the server (pid', ', 2 session shells)', 'refused: 4 sessions are open',
               'ended (the client left)', 'telnetd: telnetd off stops the server (pid', ', 1 session shell)']),
    ('umount /hda1', []),                                   # marks it clean for fsck.fat on the host
]
# steps that legitimately take longer than the 10 s default (by command, or by its first word)
TIMEOUTS = {'selftest': 120, 'run fstest /hda1/FST': 60, 'fsbench': 60, 'run kcmdtest /hda1': 60,
            'run kcmdtest together /hda1': 60, 'run kcmdtest child-selftest /hda1/KSELF.TXT': 180,
            'run kcmdtest pings /hda1 10.0.2.2': 60, 'run kcmdtest kills /hda1 10.0.2.2': 180}
# anything that means the kernel fell over
BAD = ['Kernel Wipeout', "Woah.. this ain't", 'SYSTEM HALTED', 'HALTING']

# 10/6/26: steps that need every echo request answered through QEMU's user-mode network, which now and then
# loses one when several QEMUs on the host ping at once: such a step runs once more when it failed and the
# kernel's ping said a request timed out, and fails only if the second run fails too
ICMP_STEPS = {'ping 10.0.2.2 2', 'run kcmdtest pings /hda1 10.0.2.2'}
LOST_PING = b'request timeout for icmp_seq'


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
        # user-mode network: 10.0.2.0/24, gateway 10.0.2.2; the host reaches guest ports 7 and
        # (10/6/26, the telnet server) 23 through forwarded ports, and the guest reaches a small HTTP
        # server on the host (10.0.2.2) whose port travels on the kernel command line
        self.ports = {}
        self.http = None
        if net:
            self.ports['echo'] = free_port()
            self.ports['telnet'] = free_port()
            self.ports['http'] = free_port()
            boot += ['-netdev', f"user,id=net0,hostfwd=tcp:127.0.0.1:{self.ports['echo']}-:7,"
                                f"hostfwd=tcp:127.0.0.1:{self.ports['telnet']}-:23",
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


# 10/6/26: the prompt in the root directory as a repaint draws it from a console's cells: a console that does not
# draw UTF-8 yet keeps no wave there (SURF #>), one that does keeps it in one cell
REPAINTED_PROMPT = re.compile(rb'SURF \xe2\x89\x8b[!#@?]?>')


def console_switch_check(q):
    """F2 and back to F1 on the PS/2 keyboard, after `ansi`: the serial terminal is repainted
    from the console's buffer, down to its last row in use (Z in the corner, row 25: surfsh
    keeps to its own rows), and the cursor goes back up to the prompt on row 14, just after it
    (the column the screen shows)"""
    q.sendkeys('<f2>')
    ok = q.expect("Brandon Burr's SurfOS", 10) and q.expect(REPAINTED_PROMPT, 10)
    q.sendkeys('<f1>')
    ok = ok and q.expect('ANSI-TEST top left', 10)
    screen = q.screen()
    rows = screen.split('\n')
    column = len(rows[13]) + 1 if len(rows) > 13 else 0          # the prompt and the space after it
    ok = ok and q.expect(f'Z\r\x1b[11A\x1b[{column}C', 10)
    ok = ok and 'ANSI-TEST done, 80x25' in screen
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
            ok = q.expect(PROMPT, 20)                       # the rest, run's status line too, is not shown
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
            ok = q.expect(PROMPT, 20)
        ok = ok and 'PAGETEST DONE' not in q.transcript[start:].decode(errors='replace')
    finally:
        q.more_at, q.auto_more = len(q.transcript), True
    return ('pager on the PS/2 keyboard: --More-- in inverse video on the last row, Space, Enter, q', ok)


def pager_quit_ask_check(q):
    """q at --More--, then the program asks and reads (pagetest ask): its question still shows, the last line
    q dropped, and what it prints after the answer shows too"""
    q.auto_more = False
    try:
        start = len(q.transcript)
        q.send('pagetest ask 30\n')
        ok = q.expect('--More--', 20)
        if ok:
            q.send('q')
            ok = q.expect('pagetest question: ', 20)
        if ok:
            q.send('yes\n')
            ok = q.expect('pagetest answer: yes', 20) and q.expect('PAGETEST DONE', 10) and q.expect(PROMPT, 10)
        out = q.transcript[start:].decode(errors='replace')
        ok = ok and 'pagetest line 024' in out and 'pagetest line 025' not in out
    finally:
        q.more_at, q.auto_more = len(q.transcript), True
    return ('pager: after q, the question a program asks before it reads still shows', ok)


def pager_quit_crash_check(q):
    """q at --More--, then the program crashes (pagetest crash): the kernel's report still shows"""
    q.auto_more = False
    try:
        start = len(q.transcript)
        q.send('pagetest crash 30\n')
        ok = q.expect('--More--', 20)
        if ok:
            q.send('q')
            ok = q.expect('killed by "Page Fault', 20) and q.expect(PROMPT, 10)
        ok = ok and 'pagetest line 025' not in q.transcript[start:].decode(errors='replace')
    finally:
        q.more_at, q.auto_more = len(q.transcript), True
    return ('pager: after q, a crash report still shows', ok)


def pager_background_check(q):
    """a program in the background holds the pager (pagetest hold, started with &): the shell's next command is
    paged all the same, the shell takes the pager over; the job ends, and is collected with the next program"""
    q.send('pagetest hold 4000 &\n')
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10)
    if ok:
        start = len(q.transcript)
        q.send('pagetest 30\n')
        ok = q.expect('PAGETEST DONE', 20) and q.expect(PROMPT, 10)
        parts = q.transcript[start:].decode(errors='replace').split(MORE.decode())
        ok = ok and [pagetest_lines(p) for p in parts] == [list(range(1, 25)), list(range(25, 31))]
    ok = ok and q.expect('PAGETEST HELD', 15)
    if ok:
        q.send('pagetest caps\n')
        ok = q.expect('] done', 10) and q.expect(PROMPT, 10)
    return ('pager: a command after a background job that holds the pager is paged all the same', ok)


def kcmd_echo_check(q):
    """typed input's echo is shown and never kept with a kernel command's output: fdisk's question with its
    output on a file (kcmdtest run), answered on the console. The interactive commands that read without a
    question of the shell's (demo, hanoi, rawkeys) refuse a redirection, so fdisk is the one left to show it"""
    start = len(q.transcript)
    q.send('kcmdtest run /hda1/ECHO.TXT fdisk rd1 new gpt\n')
    ok = q.expect(CONFIRM_RD1, 10)
    if ok:
        q.send('nope-typed\n')
        ok = q.expect('nope-typed', 10) and q.expect('kcmdtest: fdisk: status 1', 10) and q.expect(PROMPT, 10)
    if ok:
        start = len(q.transcript)
        q.send('cat /hda1/ECHO.TXT\n')
        ok = q.expect('Cancelled', 10) and q.expect(PROMPT, 10) and b'nope-typed' not in q.transcript[start:]
    q.send('rm /hda1/ECHO.TXT\n')
    ok = q.expect(PROMPT, 10) and ok
    return ('a kernel command with its output on a file: the typed answer is shown, and not kept in the file', ok)


# 10/6/26: surfsh's prompt after a command that failed: the state character ! (shell/prompt.c)
FAILED_PROMPT = re.compile(rb'(?:\xe2\x89\x8b|~)' + _SGR + rb'!' + _SGR + rb'>')


def ctrl_c_check(q):
    """10/6/26 (the kill that waits): Ctrl+C on the serial line while surfsh runs a kernel command in its own task
    (ping) cuts the command short: its summary, then surfsh's prompt saying it failed (status 130); while surfsh
    waits for a program (count), the program is killed: [pid n killed], the prompt saying so; surfsh lives on"""
    q.send('ping 10.0.2.2 100\n')
    ok = q.expect(re.compile(rb'icmp_seq[= ]2\b'), 15)     # its second request, answered or not: it is pinging
    if ok:
        q.ser.sendall(b'\x03')
        ok = q.expect('packets transmitted', 10) and q.expect(FAILED_PROMPT, 10)
    if ok:
        q.send('count 100 100\n')
        ok = q.expect(' 5 ', 15)
    if ok:
        q.ser.sendall(b'\x03')
        ok = q.expect('killed]', 10) and q.expect(FAILED_PROMPT, 10)
    q.send('echo surfsh lives on\n')
    ok = q.expect('surfsh lives on\r\n', 10) and q.expect(PROMPT, 10) and ok
    return ('Ctrl+C: a ping surfsh runs is cut short, a program it waits for is killed, surfsh lives on', ok)


# the steps of the console's character set and the pager (10/6/26) that the host carries out
CONSOLE_CHECKS = {'@utf8-screen': utf8_screen_check, '@pager-space': pager_space_check,
                  '@pager-enter-q': pager_enter_quit_check, '@pager-keys': pager_keys_check,
                  '@pager-q-ask': pager_quit_ask_check, '@pager-q-crash': pager_quit_crash_check,
                  '@pager-background': pager_background_check, '@kcmd-echo': kcmd_echo_check}


# 10/6/26 (track P): what `ptytest kmsg` writes to /dev/kmsg (user/bin/ptytest.c)
KMSG_LINE = 'ptytest kmsg: a line for the kernel log'


def kmsg_check(q):
    """a line a program writes to /dev/kmsg is not shown on the console, and dmesg has it"""
    start = len(q.transcript)
    q.send('run ptytest kmsg\n')
    ok = q.expect('PTYTEST kmsg PASS', 10) and q.expect(PROMPT, 10)
    ok = ok and KMSG_LINE.encode() not in q.transcript[start:]
    q.send('dmesg\n')
    ok = ok and q.expect(KMSG_LINE, 30) and q.expect(PROMPT, 30)
    return ('/dev/kmsg: a line written there is in the kernel log (dmesg), and on no console', ok)


CONSOLE_CHECKS['@kmsg'] = kmsg_check


def pty_tcpecho_check(q):
    """a kernel thread started in a pty session (tcpecho) is on F1: the session ends with exit, the thread lives on
    (ptytest tcpecho checks those), and what it prints shows on F1 (the serial line) rather than in the session"""
    start = len(q.transcript)
    q.send('run ptytest tcpecho 2323\n')
    ok = q.expect('PTYTEST tcpecho PASS', 20) and q.expect('exit status 0', 10) and q.expect(PROMPT, 10)
    ok = ok and b'tcpecho: listening on port 2323' in q.transcript[start:]
    return ('pty: a kernel thread started in a session (tcpecho) runs on F1 and outlives the session', ok)


def pty_isolation_check(q):
    """a pty session's output is not shown on F1, a line typed on F1 goes to the program reading F1 and not to the
    session, and poll() on the console sees it queued (ptytest isolate)"""
    start = len(q.transcript)
    q.send('run ptytest isolate\n')
    ok = q.expect('type a line on the console now', 10)
    if ok:
        q.send('f1-typed-line\n')
        ok = (q.expect('POLLIN on the console', 10) and q.expect('the console read "f1-typed-line"', 10) and
              q.expect('PTYTEST isolate PASS', 10) and q.expect(PROMPT, 10))
    text = q.transcript[start:]
    ok = ok and b'pagetest question' not in text and b'pagetest answer' not in text
    return ('pty: a session prints only to its master, F1 keeps its keys, poll() sees a key on the console', ok)


CONSOLE_CHECKS['@pty-tcpecho'] = pty_tcpecho_check
CONSOLE_CHECKS['@pty-isolate'] = pty_isolation_check


def surfsh_corner_check(q):
    """10/6/26 (surfsh): after `ansi`, the next prompt and a line typed, edited (Backspace, Left, an insertion,
    Delete) and dropped with Ctrl-C change only their own rows: the pattern and the Z in the corner stay in VGA
    memory, the line reads as it was edited, and the new prompt is on the row under it"""
    q.send('ansi\n')
    ok = q.expect('ANSI-TEST done', 10) and q.expect(PROMPT, 10)
    if ok:
        q.ser.sendall(b'echo abcdef\x7f\x7f\x1b[D\x1b[DX\x1b[3~')
        q.wait(1)
        q.ser.sendall(b'\x03')
        ok = q.expect('^C', 10) and q.expect(PROMPT, 10)
        q.wait(0.5)
    d = q.vga_memory() or bytes(4000)
    rows = [cells_text(d[r * 160:(r + 1) * 160:2]).rstrip() for r in range(25)]
    ok = ok and all(rows[r] == text for r, text in ANSI_ROWS.items()) and d[(24 * 80 + 79) * 2] == ord('Z')
    ok = ok and re.fullmatch('SURF \u224b!?> echo abXd\\^C', rows[13]) is not None and rows[14].startswith('SURF \u224b')
    return ('surfsh: a prompt and a line edited after ansi leave the screen below them alone (the Z in the corner)', ok)


CONSOLE_CHECKS['@surfsh-corner'] = surfsh_corner_check


# ---- 10/6/26: telnetd, the telnet server ------------------------------------------------------------------------
TN_IAC, TN_DONT, TN_DO, TN_WONT, TN_WILL, TN_SB, TN_SE = 255, 254, 253, 252, 251, 250, 240
TO_ECHO, TO_SGA, TO_NAWS = 1, 3, 31


class Telnet:
    """A telnet client for the tests that answers as a terminal's client does (RFC 854, 1143): it lets the
    server echo and suppress go-ahead (DO), suppresses go-ahead itself and tells its window size when asked
    (WILL, then NAWS), and refuses anything else once. What the session printed is kept in self.data with
    the IAC sequences taken out (IAC IAC is the byte 255, CR NUL a CR); self.raw has everything as it came.
    Return is sent as CR NUL, as telnet clients send it; --More-- is answered with Space unless auto_more is off."""

    def __init__(self, port, cols=80, rows=24, host='127.0.0.1', timeout=10, rcvbuf=None):
        self.sock = self._connect(host, port, timeout, rcvbuf)
        self.cols, self.rows = cols, rows
        self.raw, self.data = b'', b''
        self.cursor, self.more_at, self.auto_more = 0, 0, True
        self.closed = False
        self.state, self.verb, self.sb = 'data', 0, b''
        self.after_cr = False                   # the last data byte was CR: a NUL after it is not data
        self.agreed = set()                     # (side, option): ('him', 1) the server echoes, ('us', 31) NAWS ...

    @staticmethod
    def _connect(host, port, timeout, rcvbuf):
        """the connection; QEMU's forwarded port accepts at once and closes again while nothing listens in the
        guest, so until timeout, one that ends before the server has said anything is tried again. rcvbuf: a
        small window, for a client that stops reading"""
        deadline = time.time() + timeout
        while True:
            s = socket.socket()
            if rcvbuf:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            s.settimeout(timeout)
            try:
                s.connect((host, port))
            except OSError:
                s.close()
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
                continue
            s.settimeout(1)
            try:
                alive = s.recv(1, socket.MSG_PEEK) != b''
            except socket.timeout:
                alive = True                    # open and quiet so far: the server's opening is on its way
            except OSError:
                alive = False                   # reset
            if alive or time.time() > deadline:
                s.settimeout(timeout)
                return s
            s.close()
            time.sleep(0.2)

    def _out(self, b):
        try:
            self.sock.sendall(b)
        except OSError:
            self.closed = True

    def _option(self, verb, opt):
        side = 'him' if verb in (TN_WILL, TN_WONT) else 'us'
        if verb in (TN_WILL, TN_DO):
            wanted = opt in ((TO_ECHO, TO_SGA) if side == 'him' else (TO_SGA, TO_NAWS))
            if (side, opt) in self.agreed:
                return                          # nothing changes: nothing to say
            if not wanted:
                self._out(bytes([TN_IAC, TN_DONT if verb == TN_WILL else TN_WONT, opt]))
                return
            self.agreed.add((side, opt))
            self._out(bytes([TN_IAC, TN_DO if verb == TN_WILL else TN_WILL, opt]))
            if side == 'us' and opt == TO_NAWS:
                self.resize(self.cols, self.rows)
        elif (side, opt) in self.agreed:        # WONT or DONT for something on: agreed off
            self.agreed.discard((side, opt))
            self._out(bytes([TN_IAC, TN_DONT if verb == TN_WONT else TN_WONT, opt]))

    def resize(self, cols, rows):
        """a new window size (NAWS), as a terminal's client sends it when its window changes"""
        self.cols, self.rows = cols, rows
        size = bytes([cols >> 8, cols & 255, rows >> 8, rows & 255]).replace(b'\xff', b'\xff\xff')
        self._out(bytes([TN_IAC, TN_SB, TO_NAWS]) + size + bytes([TN_IAC, TN_SE]))

    def _feed(self, chunk):
        out = bytearray()
        for c in chunk:
            if self.state == 'data':
                if c == TN_IAC:
                    self.state = 'iac'
                elif c == 0 and self.after_cr:
                    self.after_cr = False       # CR NUL: a CR alone (RFC 854)
                else:
                    out.append(c)
                    self.after_cr = c == 13
            elif self.state == 'iac':
                self.state = 'data'
                if c == TN_IAC:
                    out.append(255)
                    self.after_cr = False
                elif c in (TN_WILL, TN_WONT, TN_DO, TN_DONT):
                    self.state, self.verb = 'option', c
                elif c == TN_SB:
                    self.state, self.sb = 'sb', b''
            elif self.state == 'option':
                self.state = 'data'
                self._option(self.verb, c)
            elif self.state == 'sb':
                self.state = 'sb-iac' if c == TN_IAC else 'sb'
            elif self.state == 'sb-iac':
                self.state = 'data' if c == TN_SE else 'sb'
        self.data += bytes(out)

    def pump(self, timeout=0.1):
        if self.closed:
            time.sleep(timeout)
            return
        r, _, _ = select.select([self.sock], [], [], timeout)
        if not r:
            return
        try:
            chunk = self.sock.recv(65536)
        except OSError:
            chunk = b''
        if not chunk:
            self.closed = True
            return
        self.raw += chunk
        self._feed(chunk)
        while self.auto_more:                   # the pager's question: Space, as the console harness answers it
            i = self.data.find(MORE, self.more_at)
            if i < 0:
                break
            self.more_at = i + len(MORE)
            self._out(b' ')

    def expect(self, needle, timeout=10):
        """wait for needle (a string, or a compiled bytes pattern) in what the session printed, after the last match"""
        deadline = time.time() + timeout
        while True:
            if isinstance(needle, re.Pattern):
                m = needle.search(self.data, self.cursor)
                if m:
                    self.cursor = m.end()
                    return True
            else:
                i = self.data.find(needle.encode(), self.cursor)
                if i >= 0:
                    self.cursor = i + len(needle.encode())
                    return True
            if time.time() > deadline or self.closed:
                return False
            self.pump(0.1)

    def send(self, keys):
        """keys as typed: a string (a newline is Return, sent as CR NUL; 0xff doubled) or bytes as they are"""
        if isinstance(keys, str):
            keys = keys.encode().replace(b'\xff', b'\xff\xff').replace(b'\n', b'\r\0')
        self._out(keys)

    def wait_closed(self, timeout=10):
        deadline = time.time() + timeout
        while not self.closed and time.time() < deadline:
            self.pump(0.1)
        return self.closed

    def leave(self, timeout=10):
        """the client goes: its FIN, then the server's close once the session has ended (True)"""
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        return self.wait_closed(timeout)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.closed = True


TELNET_ROOT = shown('SURF \u224b@> ')         # a session's prompt in /: the remote state (surfos/prompt.h)


def telnet_no_sessions(q, timeout=20):
    """True once telnetd status on the console says that the server runs with no session left"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        start = len(q.transcript)
        q.send('telnetd status\n')
        if not q.expect(PROMPT, 10):
            return False
        if 'running (pid' in q.transcript[start:].decode(errors='replace') and \
           ', 0 session shells' in q.transcript[start:].decode(errors='replace'):
            return True
        q.wait(0.5)
    return False


def telnet_session_check(q):
    """one session through the forwarded port 23 (telnetd on came before): the banner, the prompt in its remote
    state (\u224b@>, which a failed command does not change), a command, cd and the breadcrumbs, Tab, help paged (the
    client answers --More--), Ctrl+C killing a running count, exit, and the server closing the connection; the
    console's own prompt meanwhile has no @"""
    t = Telnet(q.ports['telnet'])
    try:
        ok = t.expect("Brandon Burr's SurfOS", 15) and t.expect(TELNET_ROOT, 10)
        steps = [('pwd\n', ['\r\n/\r\n', TELNET_ROOT]),
                 ('cd /initrd/bin\n', [shown('initrd \u25b8 bin \u224b@> ')]),
                 ('ech\ttelnet-tab\n', ['\r\ntelnet-tab\r\n', shown('bin \u224b@> ')]),
                 ('help\n', ['surfsh 1.0, the SurfOS shell', "the kernel's commands", '--More--',
                             '(C)2004-2026 Brandon Burr.', shown('bin \u224b@> ')]),
                 ('count 100 100\n', [' 5 ']),
                 (b'\x03', ['killed]', shown('bin \u224b@> ')])]
        for keys, expects in steps:
            if not ok:
                break
            t.send(keys)
            ok = all(t.expect(e, 20) for e in expects)
        if ok:
            q.send('cd /initrd\n')             # the console's prompt: no @ (F1-F4 and the serial line are not remote)
            ok = q.expect(shown('initrd \u224b> '), 10)
            q.send('cd /\n')
            ok = q.expect(shown('SURF \u224b> '), 10) and ok
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet: a session: banner, prompt \u224b@> (the console\'s has no @), a command, cd, Tab, help paged, '
            'Ctrl+C, exit closes it', ok)


def telnet_two_check(q):
    """two sessions at once, each with its own shell; telnetd status on the console lists both; telnetd off ends
    them (both connections close); telnetd on again"""
    a, b = Telnet(q.ports['telnet']), Telnet(q.ports['telnet'])
    try:
        ok = a.expect(TELNET_ROOT, 15) and b.expect(TELNET_ROOT, 15)
        if ok:
            a.send('echo from-a\n')
            b.send('echo from-b\n')
            ok = a.expect('\r\nfrom-a\r\n') and b.expect('\r\nfrom-b\r\n')
            ok = ok and b'from-b' not in a.data and b'from-a' not in b.data
        q.send('telnetd status\n')
        ok = ok and q.expect('telnetd: running (pid') and q.expect('2 session shells:') and q.expect('surfsh (pid') \
            and q.expect('surfsh (pid') and q.expect(PROMPT)
        q.send('telnetd off\n')
        ok = ok and q.expect('telnetd: stopped (2 session shells ended)', 15) and q.expect(PROMPT)
        ok = ok and a.wait_closed(10) and b.wait_closed(10)
        q.send('telnetd on\n')
        ok = ok and q.expect('telnetd: listening on 10.0.2.15 port 23') and q.expect(PROMPT)
    finally:
        a.close()
        b.close()
    return ('telnet: two sessions at once, telnetd status lists them, off ends both, on again', ok)


def telnet_full_check(q):
    """four sessions at once, a fifth connection told so and closed; the four end when their clients go"""
    clients, fifth = [], None
    try:
        ok = True
        for _ in range(4):
            t = Telnet(q.ports['telnet'])
            clients.append(t)
            ok = ok and t.expect(TELNET_ROOT, 15)
        fifth = Telnet(q.ports['telnet'])
        ok = ok and fifth.expect('4 sessions are open already', 10) and fifth.wait_closed(10)
        for t in clients:
            ok = t.leave(10) and ok
        ok = ok and telnet_no_sessions(q)
    finally:
        for t in clients + ([fifth] if fifth else []):
            t.close()
    return ('telnet: four sessions at once, a fifth refused; they end when their clients go', ok)


def telnet_stalled_check(q):
    """a client that stops reading while its session prints without end (hexdump /dev/zero in the background, which
    is not paged): however much the host's side takes first, the connection's send buffer in the guest fills and
    stays full (netstat: out 8192), and the server holds that session back and serves a second session all the
    same. When the first client reads again its session goes on: the hexdump is killed from the console, and an
    Enter in the session brings the report of it and a prompt; every line arrived, in order. Then the client goes."""
    a, b = Telnet(q.ports['telnet'], rcvbuf=4096), None
    full = re.compile(rb'local 23\s+remote [0-9.]+:[0-9]+\s+rx\s+[0-9]+\s+tx\s+[0-9]+\s+retrans\s+[0-9]+\s+'
                      rb'buf\s+in\s+[0-9]+\s+out\s+8192')
    line = re.compile(rb'([0-9a-f]{8})  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  \|\.{16}\|\r\n')
    why = ''
    try:
        ok = a.expect(TELNET_ROOT, 15)
        if ok:
            a.send('/initrd/bin/hexdump /dev/zero &\n')
            ok = a.expect(re.compile(rb'\[(\d+)\] started'), 10)     # a reads nothing more from here on
            m = re.search(rb'\[(\d+)\] started', a.data)
            pid = int(m.group(1)) if m else 0
        stalled, deadline = False, time.time() + 90
        while ok and not stalled and time.time() < deadline:
            start = len(q.transcript)
            q.send('netstat\n')
            ok = q.expect(PROMPT, 10)
            stalled = full.search(q.transcript[start:]) is not None
            if not stalled:
                q.wait(1)
        why = '' if stalled else ' (the send buffer never filled)'
        ok = ok and stalled
        if ok:
            b = Telnet(q.ports['telnet'])
            ok = b.expect(TELNET_ROOT, 15)
        if ok:
            b.send('echo second-session\n')
            ok = b.expect('\r\nsecond-session\r\n', 15) and b.expect(TELNET_ROOT, 10)
        if ok:
            b.send('exit\n')
            ok = b.wait_closed(10)
        if ok:                                      # a reads again; the hexdump is stopped from the console
            a.pump(1)
            q.send('kill %d\n' % pid)
            ok = q.expect("killing 'hexdump'", 10) and q.expect(PROMPT, 10)
        if ok:                                      # everything it printed, then an Enter: the report and a prompt
            quiet_since = time.time()
            deadline = time.time() + 120
            while time.time() - quiet_since < 2 and time.time() < deadline and not a.closed:
                n = len(a.data)
                a.pump(0.2)
                if len(a.data) != n:
                    quiet_since = time.time()
            a.send('\n')
            ok = a.expect('] killed', 20) and a.expect(TELNET_ROOT, 10)
            # the lines from the prompt after "[pid] started" on: the job's first lines may come before that notice
            # and the prompt, which then cut one of them
            started = re.search(rb'\] started', a.data)
            prompt = TELNET_ROOT.search(a.data, started.end()) if started else None
            offsets = [int(x, 16) for x in line.findall(a.data, prompt.end() if prompt else 0)]
            ok = ok and len(offsets) > 2000 and offsets[0] < 0x10000 and \
                offsets == list(range(offsets[0], offsets[0] + 16 * len(offsets), 16))
            why = why or ('' if ok else ' (%d lines, not all in order)' % len(offsets))
            ok = a.leave(10) and ok
        ok = ok and telnet_no_sessions(q)
    finally:
        a.close()
        if b:
            b.close()
    return ('telnet: a client that stops reading holds back only its own session, which goes on later' + why, ok)


def telnet_flood_check(q):
    """a client that types without end into a session that reads nothing (sleep 100000, a kernel command): the server
    keeps reading it, holds what the pseudo-terminal does not take and drops the rest, and a second session stays
    quick all the while (an echo, a hexdump of a program paged on a tall window); a Ctrl+C from the flooding client
    still gets through at once, ends the sleep, and drops what was typed ahead: its next command runs as typed"""
    a, b = Telnet(q.ports['telnet']), None
    stop, sent = threading.Event(), [0]

    def flood():
        chunk = b'x' * 1024
        while not stop.is_set():
            try:
                a.sock.sendall(chunk)
                sent[0] += len(chunk)
            except OSError:
                return

    flooder = threading.Thread(target=flood, daemon=True)
    ok, why = a.expect(TELNET_ROOT, 15), ''
    try:
        if ok:
            a.send('sleep 100000\n')
            ok = a.expect('sleep 100000', 10)
        if ok:
            flooder.start()
            time.sleep(1)
            b = Telnet(q.ports['telnet'], rows=1000)
            t0 = time.time()
            ok = b.expect(TELNET_ROOT, 20)
            if ok:
                b.send('echo flood-ok\n')
                ok = b.expect('\r\nflood-ok\r\n', 10) and b.expect(TELNET_ROOT, 10) and time.time() - t0 < 20
                why = '' if ok else ' (the second session was slow)'
            if ok:
                t0 = time.time()
                b.send('/initrd/bin/hexdump /initrd/bin/echo\n')
                ok = b.expect(re.compile(rb'\r\n[0-9a-f]{8}\r\n'), 60) and b.expect(TELNET_ROOT, 20)
                ok = ok and time.time() - t0 < 60 and sent[0] > 64 * 1024
                why = '' if ok else ' (the hexdump was slow, or the flood stopped: %d bytes sent)' % sent[0]
        stop.set()
        if flooder.is_alive():
            flooder.join(10)
        if ok:
            a.cursor = len(a.data)
            a.send(b'\xff\xf4')                     # IAC IP: Ctrl+C
            ok = a.expect(PROMPT, 15)
            why = why or ('' if ok else ' (Ctrl+C did not end the sleep)')
        if ok:
            a.send('echo after-flood\n')
            ok = a.expect('\r\nafter-flood\r\n', 10) and a.expect(TELNET_ROOT, 10)
            why = why or ('' if ok else ' (keys typed ahead were left)')
        if ok and b:
            b.send('exit\n')
            ok = b.wait_closed(10)
        if ok:
            a.send('exit\n')
            ok = a.wait_closed(10)
    finally:
        stop.set()
        a.close()
        if b:
            b.close()
    return ('telnet: keys flooding into a session that reads none hold back no other session; Ctrl+C still gets '
            'through' + why, ok)


def telnet_off_inside_check(q):
    """telnetd off typed in a session: the server stops, which ends every session, that one too (its connection
    closes); on the console the server is not running any more"""
    t = Telnet(q.ports['telnet'])
    try:
        ok = t.expect(TELNET_ROOT, 15)
        if ok:
            t.send('telnetd off\n')
            ok = t.wait_closed(15)
    finally:
        t.close()
    q.send('telnetd\n')
    ok = q.expect('telnetd: not running (telnetd on starts it)', 10) and q.expect(PROMPT, 10) and ok
    return ('telnet: telnetd off typed in a session stops the server and ends the session', ok)


def telnet_port_busy_check(q):
    """telnetd on while another program listens on port 23 (nettest echo 23 1 &): the port is in use (status 1), and
    no server is left behind; the host's connection to nettest then lets it end"""
    q.send('nettest echo 23 1 &\n')
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10)
    q.send('run telnetd on\n')
    ok = q.expect('telnetd: port 23 is in use', 10) and q.expect('exit status 1', 10) and q.expect(PROMPT, 10) and ok
    q.send('telnetd\n')
    ok = q.expect('telnetd: not running', 10) and q.expect(PROMPT, 10) and ok
    s = guest_connection(q) if ok else None
    if s:
        s.close()
    ok = ok and s is not None and q.expect('0 bytes echoed', 10)
    q.send('telnetd\n')                     # surfsh collects nettest from the background
    ok = q.expect(PROMPT, 10) and ok
    return ('telnet: telnetd on while nettest has port 23: in use, and no server left', ok)


CONSOLE_CHECKS.update({'@telnet-session': telnet_session_check, '@telnet-two': telnet_two_check,
                       '@telnet-full': telnet_full_check, '@telnet-stalled': telnet_stalled_check,
                       '@telnet-flood': telnet_flood_check,
                       '@telnet-off-inside': telnet_off_inside_check, '@telnet-port-busy': telnet_port_busy_check})


# 10/6/26 (the telnet server, track N): nettest (user/bin/nettest.c) and the host
def nettest_pattern(n):
    """what `nettest send` sends: "nettest 0000000\\n", "nettest 0000001\\n", ... cut to n bytes"""
    return b''.join(b'nettest %07d\n' % i for i in range((n + 15) // 16))[:n]


def guest_connection(q, timeout=10):
    """the host's connection to the guest's port 23 (QEMU's forwarded port): it accepts at once and closes again
    while nothing listens in the guest, so try until the guest's listener answers"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', q.ports['telnet']), timeout=3)
            s.settimeout(0.5)
            try:
                if s.recv(1, socket.MSG_PEEK) == b'':
                    s.close()
                    q.pump(0.2)
                    continue
            except socket.timeout:
                pass                        # open, nothing sent yet: the guest accepted it
            s.settimeout(20)
            return s
        except OSError:
            q.pump(0.2)
    return None


def nettest_host_check(q):
    """`nettest echo 23 1 &`: the host connects through the forwarded port 23, what it sends comes back, it
    closes, and nettest says how many bytes it echoed; `nettest send 23 1048576 &`: a megabyte the host starts
    to read late and slowly arrives whole and in order (write waits for room as long as it takes), then the end"""
    msg = b'surfos nettest: echoed through port 23\n'
    q.send('nettest echo 23 1 &\n')
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10)
    s = guest_connection(q) if ok else None
    got = b''
    if s:
        try:
            s.sendall(msg)
            while len(got) < len(msg):
                d = s.recv(1024)
                if not d:
                    break
                got += d
        except OSError:
            pass
        s.close()
    ok = ok and got == msg and q.expect('%d bytes echoed' % len(msg), 10)
    n = 1 << 20
    q.send('nettest send 23 %d &\n' % n)
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10) and ok
    s = guest_connection(q) if ok else None
    got = b''
    if s:
        q.wait(2)                           # the guest's writes wait for room meanwhile
        try:
            while True:
                d = s.recv(4096)
                if not d:
                    break
                got += d
                if len(got) < 65536:
                    time.sleep(0.01)        # slowly at first
        except OSError:
            pass
        s.close()
    ok = ok and got == nettest_pattern(n) and q.expect('%d bytes sent' % n, 20)
    q.send('nettest info\n')               # a program waited for: surfsh collects the two from the background
    ok = q.expect(PROMPT, 10) and ok
    return ('nettest: an echo server and a megabyte to the host through the forwarded port 23', ok)


def nettest_kills_check(q):
    """a program waiting in accept(): Ctrl+C kills it at once (it was started in the foreground), and so does kill
    for one in the background; nothing holds port 23 afterwards (nettest echo 23 0 listens there and stops)"""
    q.send('nettest echo 23 1\n')
    ok = q.expect('listening on port 23', 10)
    if ok:
        q.wait(0.5)
        q.ser.sendall(b'\x03')
        ok = q.expect('killed]', 5) and q.expect(FAILED_PROMPT, 5)
    start = len(q.transcript)
    q.send('nettest echo 23 1 &\n')
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10) and ok
    pids = re.findall(r'\[(\d+)\] started', q.transcript[start:].decode(errors='replace'))
    if ok and pids:
        q.wait(0.5)
        q.send('kill %s\n' % pids[-1])
        ok = q.expect("killing 'nettest'", 5) and q.expect(PROMPT, 5)
        q.wait(0.5)                         # it ends when its accept() gives up, as soon as it runs
    else:
        ok = False
    q.send('nettest echo 23 0\n')
    ok = q.expect('listening on port 23', 10) and q.expect(PROMPT, 10) and ok
    return ('nettest: Ctrl+C and kill while accept() waits end it at once, and port 23 is free again', ok)


CONSOLE_CHECKS['@nettest-host'] = nettest_host_check
CONSOLE_CHECKS['@nettest-kills'] = nettest_kills_check
TIMEOUTS.update({'nettest connect 10.0.2.99 23': 30, 'nettest limits': 60})

# 10/6/26 (track N, after its review): the scenarios of tools/qemu-tcp.py as smoke steps
_tcp_raw = {}


def tcp_raw_check(q, name):
    """one scenario of tools/qemu-tcp.py: the first step boots the kernel the smoke test runs (the same initrd) with
    its NIC on a raw link to tools/rawnet.py; '@tcp-raw end' ends that boot"""
    import importlib.util
    import types
    tcp = _tcp_raw.get('module')
    if not tcp:
        spec = importlib.util.spec_from_file_location('qemutcp', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'qemu-tcp.py'))
        tcp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tcp)
        _tcp_raw['module'] = tcp
    rig = _tcp_raw.get('rig')
    if name == 'end':
        if not rig:
            return ('TCP with hand-made segments: its boot', False)
        rig.q.pump(0.5)
        text = rig.q.text()
        ok = rig.q.alive() and not any(bad in text for bad in BAD)
        if not ok:
            print('\n---- the raw-link boot: serial transcript (tail) ----\n' + tail(text, 40))
        rig.close()
        del _tcp_raw['rig']
        return ('TCP with hand-made segments: its boot ends well', ok)
    if not rig:
        harness = types.SimpleNamespace(Qemu=Qemu, PROMPT=PROMPT, BAD=BAD, tail=tail)
        rig = tcp.Rig(harness, q.cmd[q.cmd.index('-kernel') + 1], os.path.join(q.workdir, 'initrd.tar'), q.cmd[0],
                      q.cmd[q.cmd.index('-m') + 1])
        _tcp_raw['rig'] = rig
    fn, what = [(f, w) for n, f, w in tcp.SCENARIOS if n == name][0]
    try:
        ok = rig.ok and bool(fn(rig))
    except Exception as e:
        print('        (%s: %r)' % (name, e))
        ok = False
    if not ok:
        print('\n---- the raw-link boot, %s: serial transcript (tail) ----\n%s' % (name, tail(rig.q.text(), 30)))
    rig.q.wait(0.5)
    return ('TCP %s: %s' % (name, what), ok)


for _name in ('scan', 'flood', 'reset', 'greet', 'window', 'fin', 'keepalive', 'churn', 'reserve', 'sws', 'linger', 'pollscan',
              'pollstall', 'end'):
    CONSOLE_CHECKS['@tcp-raw ' + _name] = (lambda name: lambda q: tcp_raw_check(q, name))(_name)


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


def initrd_with(src, dst, name, data):
    """10/6/26: a copy of the initrd with one file replaced (or added)"""
    with tarfile.open(src) as old, tarfile.open(dst, 'w', format=tarfile.USTAR_FORMAT) as new:
        for m in old.getmembers():
            if m.name != name:
                new.addfile(m, old.extractfile(m) if m.isfile() else None)
        info = tarfile.TarInfo(name)
        info.size, info.mode = len(data), 0o755
        new.addfile(info, io.BytesIO(data))


def boot_step(reboot, entry, timeout):
    """10/6/26: an '@boot' step of SMOKE: a boot of its own (reboot(words) starts it), the strings that must appear,
    lines typed then with what must follow them (and the prompt, unless that is what they end with), and strings
    that must not appear. A line may be a function of the QEMU that returns it, or '@sleep <seconds>'; bytes are sent
    as they are, with no newline and no prompt waited for after them (Ctrl+C, a command that runs on)."""
    cmd, expects, follow, absent = entry
    try:
        q2 = reboot(cmd.split()[1:])
    except Exception as e:
        print(f'{cmd}: {e}')
        return (cmd, False)
    try:
        ok = all(q2.expect(e, timeout) for e in expects)
        for line, exp in follow:
            if callable(line):
                line = line(q2)
            if isinstance(line, bytes):
                q2.ser.sendall(line)
                ok = ok and all(q2.expect(e, 10) for e in exp)
                continue
            if line.startswith('@sleep '):
                q2.wait(float(line.split()[1]))
                continue
            q2.send(line + '\n')
            ok = ok and all(q2.expect(e, 10) for e in exp)
            if exp and isinstance(exp[-1], re.Pattern):
                continue
            ok = ok and q2.expect(PROMPT, 10)
        q2.pump(0.5)
        text = q2.text()
        ok = ok and not any(a in text for a in absent) and not any(bad in text for bad in BAD) and q2.alive()
        if not ok:
            print(f'\n---- {cmd}: serial transcript (tail) ----\n' + tail(text, 25))
    finally:
        q2.close()
    return (cmd, ok)


def run_smoke(q, boot_timeout, reboot=None):
    results = [('boot to prompt (serial)', q.expect(PROMPT, boot_timeout))]
    if results[0][1]:
        for entry in SMOKE:
            cmd, expects = entry[0], entry[1]
            follow = entry[2] if len(entry) > 2 else []      # (line, expects) pairs typed after cmd
            if cmd.startswith('@boot'):                      # 10/6/26: a boot of its own
                results.append(boot_step(reboot, entry, boot_timeout) if reboot else (cmd, False))
                continue
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
            if cmd == '@ctrl-c':                             # 10/6/26: the kill that waits
                results.append(ctrl_c_check(q))
                continue
            cmd = cmd.format(**q.ports) if q.ports else cmd
            limit = TIMEOUTS.get(cmd, TIMEOUTS.get(cmd.split()[0], 10))
            for attempt in (1, 2):
                start = len(q.transcript)
                q.send(cmd + '\n')
                ok = all(q.expect(e, limit) for e in expects)
                for line, exp in follow:
                    if isinstance(line, bytes):              # keys as they are, for a full-screen program
                        q.ser.sendall(line)
                    else:
                        q.send(line + '\n')
                    ok = ok and all(q.expect(e, limit) for e in exp)
                ok = ok and q.expect(PROMPT, limit)
                if ok or attempt == 2 or cmd not in ICMP_STEPS or LOST_PING not in q.transcript[start:]:
                    break
                q.expect(PROMPT, limit)                      # the end of the first run
                print(f'  {cmd}: an echo request went unanswered (QEMU user network): once more')
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

    def reboot(words):
        """10/6/26: another boot of the same kernel for an '@boot' step: kernel command-line words, no-initrd, or
        surfsh=<program> (the initrd with that program, or with that file of it if the name has a /, as bin/surfsh)"""
        initrd, append = a.initrd, a.append.split()
        tmp = tempfile.mkdtemp(prefix='surfos-initrd-')
        try:
            for w in words:
                if w == 'no-initrd':
                    initrd = None
                elif w.startswith('surfsh='):
                    if not a.initrd:
                        raise RuntimeError('surfsh= needs --initrd')
                    with tarfile.open(a.initrd) as t:
                        program = t.extractfile(w[7:] if '/' in w[7:] else 'bin/' + w[7:]).read()
                    initrd = os.path.join(tmp, 'initrd.tar')
                    initrd_with(a.initrd, initrd, 'bin/surfsh', program)
                else:
                    append.append(w)
            return Qemu(a.kernel, a.qemu, a.mem, a.extra.split(), None, None, None, initrd, False, False, ' '.join(append), 0)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)       # Qemu() has copied the initrd

    try:
        if a.test:
            results = run_smoke(q, a.boot_timeout, reboot)
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
