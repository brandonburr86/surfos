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
import ftplib
import hashlib
import http.client
import http.server
import io
import threading
import os
import random
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import zlib

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


def autonet_telnet(q):
    """10/9/26 (the development loop): in a boot with autonet, a telnet session through the forwarded port 23 (the
    banner, the session's prompt, exit); the line to type afterwards says how it went"""
    try:
        t = Telnet(q.ports['telnet'], timeout=20)
    except OSError:
        return 'echo autonet-telnet-FAILED'
    try:
        ok = t.expect("Brandon Burr's SurfOS", 20) and t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return 'echo autonet-telnet-ok' if ok else 'echo autonet-telnet-FAILED'


# 11/01/26 (the command-line suite, track net): what the block of telnet and ftp in SMOKE needs. Its lines are functions
# of the QEMU, as in an @boot block: they know the forwarded ports and start the scripted servers. The password is made up
NET_PASSWORD = 'made-up-test-password-77'
NET = {}                                                    # the block's servers, from the line that starts one to its check


class ScriptedFtp:
    """An FTP server of this host for the guest to reach as 10.0.2.2:self.port, as hostile as its script. replies: a verb
    and what it gets: bytes, or a list used up in turn (the last stays), where {pasv} is the data listener's port as
    PASV's two numbers. transfers: a whole command line, or a verb, and (data, ending): after the data 'ok' closes and
    says 226; 'hold' keeps it open and silent until ABOR; 'reset' resets it, and waits for ABOR a little: QEMU's user network
    may hand the guest that reset as a plain end of the data (seen under Linux, not on the Mac), and a client then rightly
    waits for a final reply, so 426 comes by itself. self.got has every command."""

    def __init__(self, replies, transfers=None, greeting=b'220 scripted\r\n'):
        self.listener, self.port = host_listener()
        self.data_listener, self.data_port = host_listener()
        self.replies, self.transfers, self.greeting = replies, transfers or {}, greeting
        self.got, self.error = [], None
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def say(self, c, word):
        text = self.replies.get(word, b'502 no\r\n')
        if isinstance(text, list):
            text = text.pop(0) if len(text) > 1 else text[0]
        c.sendall(text.replace(b'{pasv}', b'%d,%d' % (self.data_port >> 8, self.data_port & 255)))

    def transfer(self, c, buf, how):
        data, ending = how
        self.data_listener.settimeout(20)
        d, _ = self.data_listener.accept()
        c.sendall(b'150 here\r\n')
        d.sendall(data)
        if ending == 'reset':
            d.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\0\0\0\0\0\0\0')
            d.close()
            c.settimeout(3)
        if ending in ('reset', 'hold'):
            try:
                while b'ABOR\r\n' not in buf:
                    more = c.recv(4096)
                    if not more:
                        break
                    buf += more
            except socket.timeout:
                c.settimeout(60)
                c.sendall(b'426 cut\r\n')
                return buf
            c.settimeout(60)
            self.got.append(b'ABOR')
            d.close()
            c.sendall(b'426 stopped\r\n226 abort done\r\n')
            buf = buf.split(b'ABOR\r\n', 1)[-1]
        else:
            d.close()
            c.sendall(b'226 done\r\n')
        return buf

    def serve(self):
        try:
            c, _ = self.listener.accept()
            c.settimeout(60)
            c.sendall(self.greeting)
            buf = b''
            while True:
                while b'\r\n' not in buf:
                    more = c.recv(4096)
                    if not more:
                        return
                    buf += more
                line, buf = buf.split(b'\r\n', 1)
                self.got.append(line)
                word = line.split(b' ')[0].decode('latin-1')
                how = self.transfers.get(line.decode('latin-1')) or self.transfers.get(word)
                if how:
                    buf = self.transfer(c, buf, how)
                else:
                    self.say(c, word)
                if word == 'QUIT':
                    c.close()
                    return
        except OSError as e:
            self.error = e

    def close(self):
        self.thread.join(5)
        self.listener.close()
        self.data_listener.close()


def net_telnet_line(q):
    """the line for a telnet session on SurfOS's own telnetd, by the hairpin through its forwarded port"""
    return f"telnet 10.0.2.2 {q.ports['telnet']}"


def net_kill_telnet(q):
    """a telnet is in raw mode on the console: a session of this host finds its pid and kills it, as nobody at that console
    could. The line to type afterwards reaches a shell only if raw mode ended with the program"""
    t = Telnet(q.ports['telnet'], timeout=20)
    try:
        t.expect(TELNET_ROOT, 15)
        t.send('kernel ps\n')
        t.expect(TELNET_ROOT, 15)
        m = re.search(rb'^\s*(\d+)\b.*\btelnet \(ring 3\)\s*$', t.data.replace(b'\r', b''), re.M)
        if m:
            t.send(f'kill {m.group(1).decode()}\n')
            t.expect(TELNET_ROOT, 15)
    finally:
        t.close()
    q.expect(PROMPT, 15)
    return 'echo raw-mode-gone'


def net_talk_line(q):
    """telnet to a host server that speaks no telnet (its port is not 23, so telnet says nothing first): the first thing it
    gets must be the line typed, whole, and then it leaves a string open and resets the connection"""
    srv, port = host_listener()
    box = NET['talk'] = {}

    def serve():
        try:
            c, _ = srv.accept()
            c.settimeout(15)
            c.sendall(b'220 mail here\r\n')
            line = b''
            while not line.endswith(b'\n'):
                d = c.recv(100)
                if not d:
                    break
                line += d
            box['line'] = line
            c.sendall(b'\x1b]0;a title never ended')
            c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\0\0\0\0\0\0\0')
            c.close()
        except OSError as e:
            box['error'] = e
        srv.close()
    box['thread'] = threading.Thread(target=serve, daemon=True)
    box['thread'].start()
    return f'telnet 10.0.2.2 {port}'


def net_ftpd_on(q):
    """what the talk's server got, said as a line; telnetd off, ftpd on, and a folder of files for it"""
    box = NET['talk']
    box['thread'].join(10)
    said = 'echo net-talk-ok' if box.get('line') == b'HELO surfos\r\n' else f"echo net-talk-FAILED {box.get('line')!r}"
    if 'the connection was lost' not in q.screen():     # the string it left open would have swallowed them on the screen
        said = 'echo net-talk-FAILED the last words are not on the screen'
    return (f"{said}; telnetd off; ftpd on {q.ports['ftp_pasv_range']}; mkdir /hda1/NET; cp /initrd/README /hda1/NET/UP.TXT; "
            "cd /hda1/NET")


def net_ftp_files(q):
    """the first script, through SurfOS's ftpd by the hairpin; what came back is what went (the count of different sums)"""
    return (f"ftp 10.0.2.2 {q.ports['ftp']} < S1.TXT; echo status $?; "
            "sha256sum UP.TXT BACK.TXT REMOTE/UP3.TXT | cut -c1-64 | uniq | wc -l")


def net_ftp_more(q):
    """the second script: reget on from a part, whole already; mget; text mode back through mput; mget again, which leaves
    what is there: status 1, and the script stops there, so its last delete does not run. What is left is one file four
    times"""
    return (f"ftp 10.0.2.2 {q.ports['ftp']} < S2.TXT; echo status $?; "
            "sha256sum UP.TXT PART.TXT UP3.TXT LF.TXT | cut -c1-64 | uniq | wc -l; ls REMOTE/UP3.TXT; cd /hda1; rm -r NET; cd /")


def net_login_line(q):
    """ftp to the scripted server: EPSV refused, PASV naming another address (10.0.2.3, QEMU's own: a client that believed
    it would reach nothing), then a low port, then the right port again"""
    good = b'227 ok (10,0,2,3,{pasv})\r\n'
    NET['ftp'] = ScriptedFtp(
        {'USER': b'331 password please\r\n', 'PASS': b'230 in\r\n', 'TYPE': b'200 ok\r\n', 'SIZE': b'550 unknown\r\n',
         'NOOP': b'200 ok\r\n',
         'PASV': [good, b'227 ok (10,0,2,2,0,25)\r\n', good], 'QUIT': b'221 bye\r\n'},
        {'NLST': (b'nice.txt\r\n\x1b[31mred\x1b[0m\r\n', 'ok'),
         'RETR big.bin': (b'x' * 20000, 'hold'),
         'RETR cut.bin': (b'y' * 5000, 'reset'),
         'RETR nice.txt': (b'nice', 'ok'),
         'RETR climb.txt': (b'up', 'ok')},
        b'220-scripted\r\n220 ready\r\n')
    return f"ftp -n 10.0.2.2 {NET['ftp'].port}"


def net_script_line(q):
    """ftpd off; a script on standard input, a pipe, and a login in it: the password can only come from the terminal"""
    NET['script'] = ScriptedFtp({'USER': b'331 password please\r\n', 'PASS': b'230 in\r\n', 'QUIT': b'221 bye\r\n'})
    return f"ftpd off; printf 'user bob\\nquit\\n' | ftp -n 10.0.2.2 {NET['script'].port}"


def net_detached_line(q):
    """a script that a detached task runs (the kernel's spawn, as the desktop's Run program... starts one): its ftp is not
    detached itself, but nobody types for it, so it must not ask for a password on the console"""
    NET['detached'] = ScriptedFtp({'USER': b'331 password please\r\n', 'QUIT': b'221 bye\r\n'})
    return (f"echo \"printf 'user bob\\\\nquit\\\\n' | /initrd/bin/ftp -n 10.0.2.2 {NET['detached'].port} > /hda1/D.OUT 2>&1\" "
            "> /hda1/D.SH; kernel spawn /initrd/bin/surfsh /hda1/D.SH")


def net_detached_result(q):
    """what that ftp said, once the server has its QUIT (had it asked, the line typed next would have been its password)"""
    deadline = time.time() + 20
    while b'QUIT' not in NET['detached'].got and time.time() < deadline:
        q.pump(0.1)
    return 'cat /hda1/D.OUT; rm /hda1/D.SH /hda1/D.OUT'


def net_names(q):
    """the session lives on; mget's listing from then on: a name that climbs, and two that are not safe here"""
    NET['ftp'].transfers['NLST'] = (b'nice.txt\r\n../../climb.txt\r\nbad|name.txt\r\nx\x1b[2J.txt\r\n', 'ok')
    return 'status\nmget *.txt'


def net_ctrl_c(q):
    """a transfer that begins and holds; then Ctrl+C, as bytes, once the server has its command"""
    q.send('get big.bin NET.BIN\n')
    deadline = time.time() + 15
    while b'RETR big.bin' not in NET['ftp'].got and time.time() < deadline:
        q.pump(0.1)
    return b'\x03'


def net_ftp_result(q):
    """nothing left on the disk by a transfer that failed, what the server got, and the password shown nowhere on the
    console"""
    srv = NET['ftp']
    srv.close()
    NET['script'].close()
    NET['detached'].close()
    bad = []
    if (b'PASS ' + NET_PASSWORD.encode()) not in NET['script'].got:
        bad.append('no PASS from the script')
    if (b'PASS ' + NET_PASSWORD.encode()) not in srv.got:
        bad.append('no PASS')
    if any(line.startswith(b'PASS') for line in NET['detached'].got):
        bad.append('the detached script asked for a password')
    if NET_PASSWORD.encode() in q.transcript:
        bad.append('the password was shown')
    if b'ABOR' not in srv.got:
        bad.append('no ABOR')
    if b'EPSV' not in srv.got:
        bad.append('no EPSV')
    if b'RETR climb.txt' not in srv.got or any(b'..' in line for line in srv.got):
        bad.append('a name climbed')
    if srv.error:
        bad.append(repr(srv.error))
    said = 'echo net-ftp-ok' if not bad else 'echo net-ftp-FAILED ' + ', '.join(bad)
    return f'ls /hda1/NET.BIN /hda1/CUT.BIN; ls /hda1 | grep -c "\\.tmp$"; {said}; rm /hda1/nice.txt /hda1/climb.txt'


def surf_prompt(crumbs, state):
    """10/6/26: the SurfOS prompt as a shell writes it to the serial port: the breadcrumbs (bytes), the wave and
    the state character, colour sequences between"""
    return re.compile(re.escape(crumbs) + rb' \xe2\x89\x8b' + _SGR + re.escape(state) + _SGR + rb'>')


# 10/6/26 (track N): the scenarios of tools/qemu-tcp.py the smoke test runs, as '@tcp-raw <name>' steps, in a boot
# of their own (tcp_raw_check() below); 'end' ends that boot
TCP_RAW = ('scan', 'flood', 'reset', 'greet', 'window', 'fin', 'keepalive', 'churn', 'reserve', 'sws', 'linger', 'lost',
           'latency', 'zwprobe', 'evict', 'backlog', 'ackahead', 'orphans', 'closing', 'shrink', 'pollscan', 'pollstall', 'halfclose',
           'nowait', 'finwait', 'ncstuck', 'mdns', 'end')

# 10/9/26 (the development loop, track D): the guest port rx listens on in the smoke test (forwarded: q.ports['rx'])
RX_PORT = 5555

# 10/24/26 (ftpd): the passive ports the smoke test forwards, each to the same port of the guest (q.ports['ftp_pasv'] is
# the first; ftpd is started with q.ports['ftp_pasv_range'])
FTP_PASV_PORTS = 4

# 10/17/26 (one stick): with the scratch ramdisk, the command line names a boot volume as GRUB would (bootvol=, the
# serial number of the FAT volume it was started from); the self test writes a FAT32 boot sector with this serial
# number onto rd1 (shell/selftest.c, test_boot_volume). No other disk of the smoke test has it.
SELFTEST_BOOTVOL = '5E1F-7E57'

# 10/9/26 (Wi-Fi, track S): in an '@boot' block, (name, CHECK) runs BOOT_CHECKS[name] on that boot's QEMU. The simulated
# Wi-Fi network's passphrase (driver/wifi/wlansim.c's WLANSIM_PASSPHRASE: a test network's, never a real one), and a wrong one
CHECK = object()
WLANSIM_PASSPHRASE = 'surfsim-wpa2-test-passphrase'
WLANSIM_WRONG = 'not-the-surfsim-passphrase'

# 10/17/26 (the network manager, kernel/netman.c): boots from a stick that keeps the owner's settings. The boot word
# settingsdisk=<name> gives a boot a FAT32 disk (the IDE primary master, hda) whose serial number its command line
# names as GRUB would (bootvol=), so SurfOS reads and writes surfos-config/ there. The disk is kept from one '@boot' to
# the next of a run (each boot writes it in place), and '@settings-disk' reads it here afterwards. SETTINGS_FILES:
# what a disk holds at first. SURFSIM_PSK: the key SurfOS must store for the test network (PBKDF2 of the passphrase
# and the SSID, as wpa_supplicant's psk=), never the passphrase itself
SETTINGS_SERIAL = 0x5E77C0F6
SETTINGS_BOOTVOL = '5E77-C0F6'
OWNER_NOTE = b"the owner's own file: SurfOS leaves it as it is\n"
SETTINGS_FILES = {
    # the owner's stick: a file of theirs, and a networks.conf written by hand on another computer: the test network
    # with its password (line 3), and one whose key is no key (line 7)
    'owner': {'notes.txt': OWNER_NOTE,
              'surfos-config/networks.conf': b'# written by hand on the Mac\nnetwork = "SurfSimWPA"\npassword = "' +
                                             WLANSIM_PASSPHRASE.encode() +
                                             b'"\n\nnetwork = "Broken"\nsecurity = wpa2\npsk = 1234\n'},
    # a stick that asks for DHCP on Ethernet and nothing else
    'ether': {'surfos-config/settings.conf': b'ethernet = dhcp\n'},
    # a hidden network saved by hand: no security line (and nothing else not understood), so open
    'hidden': {'surfos-config/networks.conf': b'network = "SurfSimHidden"\nhidden = yes\n'},
    # 10/25/26 (robustness, L22-L24): a stick for the shutdown boots, mounted read-write as SurfOS's own volume
    'shutdown': {'notes.txt': OWNER_NOTE},
    # 10/25/26 (track A, A10): surfsh's history kept, and the owner's startup file, which moves HOME for ~
    'history': {'surfos-config/settings.conf': b'history = on\n',
                'surfos-config/surfshrc': b'# the owner\'s startup file\nHOME=/hda1\necho "surfshrc ran: HOME $HOME"\n'},
    # 10/25/26 (the programs' review of track A, U5 and U8): a startup file written on Windows (CR LF) that sleeps when
    # asked to (RC_SLEEP) and then exits
    'rc': {'surfos-config/surfshrc': b'HOME=/hda1\r\necho "rc one: HOME $HOME."\r\n[ -n "$RC_SLEEP" ] && sleep 20000\r\n'
                                     b'echo "rc two"\r\nexit 3\r\necho "rc not reached"\r\n'},
}
SURFSIM_PSK = hashlib.pbkdf2_hmac('sha1', WLANSIM_PASSPHRASE.encode(), b'SurfSimWPA', 4096, 32).hex()
SETTINGS_DISKS = {}                                         # name: its image file, made the first time a boot asks
SETTINGS_DIR = []                                           # the directory they are in, once made

# 10/17/26 (httpd): a file of 1.5 MB or more on the FAT disk, four copies of a program, for @httpd-slow (the program cat,
# which copies bytes as they are; the kernel's cat prints)
BIG_BIN = '/initrd/bin/cat ' + ' '.join(['/initrd/bin/undertow'] * 4) + ' > /hda1/BIG.BIN'

# (command, strings that must appear, in order, before the next prompt)
SMOKE = [
    ('tick',    ['Ticks:']),
    ('memstat', ['Physical memory:', 'Free physical pages:']),
    ('ps',      ['Kernel Idle Task', 'init', 'surfsh (ring 3)']),                 # 10/6/26: surfsh on every console
    ('@kill-kthread', []),                  # 10/25/26 (L15): kill <the net thread's pid> is refused (kill_kthread_check)
    ('help',    ['surfsh 1.0, the SurfOS shell', 'lsblk', '(C)2004-2026 Brandon Burr.']),   # paged: Space from the harness
    ('kernel test', ['linear is using physical']),      # 10/25/26: test is surfsh's own now; kernel runs the kernel's
    ('dmesg',   ['Booting SurfOS Kernel']),
    ('crashgp', ['General Protection Fault', 'Backtrace:', 'restarted the shell']),
    ('crashnull', ['Page Fault at 0x00000000', 'restarted the shell']),
    # 10/18/26 (audit B56): a program cannot fault inside a file system, and crashfs does not give it a way (it stops
    # the system: its own boot below, in the kernel shell)
    ('crashfs', ['crashfs stops the system: only in the kernel shell (shell=kernel), not from a program']),
    ('crashlock', ['crashlock stops the system: only in the kernel shell (shell=kernel), not from a program']),
    ('heaptest', ['HEAPTEST PASS']),
    ('lspci',   ['Ethernet', '82540EM']),
    # 10/7/26 (the Wi-Fi report): the ids with the subsystem, command, interrupt; one device in full, with its bytes
    ('lspci -v', ['ids 8086:1237, subsystem 1af4:1100, class 060000', 'status: no capability list',
                  'ids 8086:100e, subsystem 1af4:1100, class 020000', 'command: I/O, memory, bus master', 'interrupt: pin A']),
    ('lspci 00:00.0', ['ids 8086:1237', 'configuration space:', '        00: 86 80 37 12', '        f0: 00']),
    ('lspci 00:1f.7', ['lspci: no PCI device at 00:1f.7']),
    ('lspci zz', ['usage: lspci [-v] [bus:device.function]']),
    ('wifiinfo', ['Wi-Fi report', 'chipset: 00:00.0 8086:1237', 'none: no PCI device is a wireless network controller',
                  'Other network controllers:', '00:03.0 8086:100e', 'USB: no xHCI controller', 'hold Option']),
    # 10/9/26 (Wi-Fi, track S): no Wi-Fi device here (the simulated one needs the word wlansim: its @boot block below)
    ('wifi', ['usage: wifi up | down | scan | join <ssid> [bssid] | status | leave']),
    ('wifi status', ['wifi: no Wi-Fi device']),
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
    ('hexdump hda 0', ['55 aa']),           # 10/25/26: the program, which hands a disk to the kernel's hexdump
    ('mount',   ['rd0', '/initrd', 'tarfs']),
    ('ls /initrd', ['etc/', 'motd']),
    ('cat /initrd/motd', ['Welcome to SurfOS']),
    ('ls /hda1', ['DOCS/', 'motd', 'README']),         # 10/25/26: the program's listing, in its sorted order
    ('@readme-cat', []),                    # cat /hda1/DOCS/README.md, every line of it (readme_cat_check)
    # 10/25/26 (track A): a program on PATH runs before a kernel command of the same name. ls -l is the program's long
    # listing; a program reads a pipe and < file (the kernel's cat read files only); type says what each name runs and
    # what it hides (a function, surfsh's own, a program, a kernel command; none: status 1); kernel NAME runs the
    # kernel's command
    ('ls -l /initrd', ['bin/', 'etc/', 'motd', 'README', ' entries']),
    ('echo x | cat', ['\r\nx\r\n']),
    ('cat < /initrd/motd', ['Welcome to SurfOS']),
    ('type ls', ["ls is the program /initrd/bin/ls; it hides the kernel's ls (kernel ls runs it)"]),
    ('f() { :; }; type f echo lsblk nosuch; echo status $?; unset -f f',
     ['f is a function', "echo is surfsh's own command; it hides the program /initrd/bin/echo and the kernel's echo "
      '(kernel echo runs it)', 'lsblk is a kernel command', 'surfsh: type: nosuch: not found', 'status 1']),
    ('kernel ls /initrd', ['motd', ' entries, ']),
    # 10/5/26 (USB storage): on the GPT fdisk left on rd1, FAT16 on rd1p1 and FAT32 (forced) on rd1p2,
    # used, remounted and ejected; then the whole disk as one volume, and the refusals
    answered('mkfs rd1p1 TESTVOL', 'Type rd1p1 to erase rd1p1 (16 MB) and create a FAT16 file system: ', 'rd1p1',
             ['rd1p1: FAT16, 2048-byte clusters', 'label TESTVOL']),
    ('mount rd1p1 /scratch', ['rd1p1 on /scratch: fat16, label TESTVOL', 'read-write']),
    ('write /scratch/hello.txt Hello from SurfOS', []),
    ('kernel cp /initrd/motd /scratch/motd', ['bytes copied']),     # 10/25/26: the kernel's (cp is a program's)
    ('write /scratch/a-much-longer-file-name.text long names work', []),
    ('mkdir /scratch/docs', []),
    ('mv /scratch/hello.txt /scratch/docs', []),
    ('mv /scratch/docs /scratch/papers', []),
    ('kernel ls /scratch/papers', ['hello.txt', '1 entries']),     # 10/25/26: the kernel's ls counts the entries
    ('kernel ls /scratch', ['papers/', 'motd', 'a-much-longer-file-name.text', '3 entries']),
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
    ('pointerinfo', ['PS/2', 'pointing devices:']),
    ('pointerinfo extra', ['usage: pointerinfo']),
    ('touchpad', ['not present on this platform']),
    ('touchpad up', ['not present on this platform']),
    ('touchpad report', ['last HID input: 0 bytes']),
    ('touchpad descriptor', ['HID report descriptor: 0 bytes']),
    ('touchpad invalid', ['usage: touchpad']),
    ('gui', ['gui needs the active local UEFI framebuffer console']),
    ('gui extra', ['usage: gui']),
    ('gfxinfo', ['software 2D', 'no framebuffer']),
    ('gfxtest', ['graphics unavailable']),
    ('gfxtest 4294967301', ['usage: gfxtest']),
    ('gfxtest 0', ['usage: gfxtest']),
    ('drivers', ['parport   ok', 'usb       not present', 'iwlwifi   skipped', 'console   VGA text mode',
                 'timer     PIT channel 0']),   # in the order printed; 10/10/26: iwlwifi only with profile=laptop
    ('cpuinfo', ['physical address bits', 'FPU      x87, FXSR, SSE, SSE2: a program\'s registers are kept with fxsave',
                 'from the boot loader: CR0']),
    # 10/25/26 (robustness, L10-L13): a program's floating point: the control words it starts with, results computed
    # with the x87 and with SSE2; its registers held across a sleep (two races at once: the self test's test_fpu)
    ('fputest', ['control words at the start: x87 0x037f, MXCSR 0x1f80', 'the stack is 16-aligned at main',
                 'x87: 1.5 + 2.25 = 3.75, * 4 = 15, / 8 = 1.875, sqrt(2.25) = 1.5, (int)-2.5 = -2',
                 'SSE2: 1.5 + 2.25 = 3.75, * 4 = 15, / 8 = 1.875, sqrt(2.25) = 1.5, (int)-2.5 = -2', 'FPUTEST PASS']),
    ('fputest race', ['8 x87 registers and 8 XMM registers across 300 ms of sleep: the same bits', 'FPUTEST race PASS']),
    ('fputest printf', ['22 formats, 0 wrong; 1/3 as %.6f is 0.333333, as %g 0.333333, as %e 3.333333e-01',
                        'FPUTEST printf PASS']),                     # 10/25/26 (L14): libsurf's %f %e %g
    # 10/25/26 (robustness, L1-L9): out of memory is a program's, never the machine's: memhog takes all a program may
    # have (the kernel keeps its reserve) and gives it back; with its stack unable to grow it is ended, and says why
    ('memhog', ['blocks of 64 KB before malloc failed', 'memhog: given back']),
    ('memhog stack', ['KB taken; now a stack that cannot grow', 'killed by "out of memory: its stack could not grow"']),
    # acpi and acpidump DSDT check that this legacy boot read no ACPI tables: poweroff -n (below) reads them, so it
    # must stay after these two
    ('acpi',    ['read with profile=laptop']),         # the USB/UEFI tests (tools/qemu-usb.py) cover the laptop profile
    ('acpidump DSDT', ['not found']),
    ('acpidump MSDM', ['usage: acpidump']),
    ('acpidump SSDT -1', ['usage: acpidump']),
    ('acpidump DSDT 1', ['usage: acpidump']),
    # 10/25/26 (robustness, L20-L24): what poweroff would do: end the shells, write out and unmount /hda1, and soft
    # off with \_S5_'s values from QEMU's DSDT. It must stay after acpi and acpidump above: it reads the ACPI tables
    # now, quietly (this boot read none), and from then on acpi lists them and acpidump finds the DSDT, so moved
    # before them it makes both fail
    ('poweroff -n', ['poweroff -n: what poweroff would do now', 'end 4 programs and shells: ', "'surfsh' (pid ",
                     'write out and unmount /hda1 (fat on hda1)',
                     'soft off (ACPI S5): SLP_TYPa 0 and SLP_TYPb 0 from \\_S5_, into PM1a at port 0x',
                     # 10/26/26 (L21b): and before it, as ACPICA does, PIIX4's GPE0 off and cleared and its PM1 status
                     'before the sleep, as ACPICA does: GPE0 at port 0xafe0 off and cleared (2 bytes each); '
                     'the PM1 status cleared at port 0x600']),
    ('acpinvs 0 16', ['refused']),
    ('acpinvs 1000 257', ['usage: acpinvs']),
    ('himem',   ['no RAM above 4 GB']),
    ('dhcp',    ['lease 10.0.2.15', 'inet 10.0.2.15', 'gateway 10.0.2.2']),
    ('ping 10.0.2.2 2', ['64 bytes from 10.0.2.2', '2 packets transmitted, 2 received']),
    ('arp',     ['10.0.2.2']),
    ('httpget 10.0.2.2 {http} /smoke', ['200 OK', 'SurfOS HTTP test OK: /smoke', 'bytes received']),
    ('tcpecho 7', ['tcpecho server started']),
    ('@host-echo', []),
    ('netstat', ['tcpecho: connection from 10.0.2.2', 'tcpecho: closed, 39 bytes echoed', 'tcp  LISTEN']),
    # 10/25/26 (L15): kill no longer ends a kernel thread: tcpecho stops when asked, its port free again
    ('tcpecho stop', ['tcpecho: stopped, port 7 is free', 'tcpecho: 1 server stopped']),
    ('tcpecho 7; sleep 500', ['tcpecho server started', 'tcpecho: listening on port 7']),
    # 10/25/26: mdns, this computer as surfos.local on its network (net/mdns.c): not running; a name that is none;
    # off when it is not running. It is not started here: QEMU's user network sends a guest's UDP on through a socket
    # of this host's, so its probes and announcements would go out onto this host's own network. On the raw link,
    # tools/qemu-tcp.py's mdns scenario ('@tcp-raw mdns' below) starts it and plays the other hosts
    ('mdns', ['mdns: not running (mdns on answers for this computer']),
    ('mdns on my_pc', ['mdns: my_pc is no name here: 1 to 63 letters, digits and hyphens']),
    ('mdns off', ['mdns: not running']),
    # 10/6/26 (the telnet server, track N): network connections for programs (surfos/sock.h), with nettest: the
    # interface; an echo server the host reaches through the forwarded port 23, and a megabyte the host reads slowly;
    # an HTTP GET from the harness's server; a refused connection, and one to an address that does not answer ARP;
    # the calls' errors and limits; a kill and Ctrl+C while accept() waits. Port 23 is free again at the end.
    ('nettest entropy unsupported', ['ENTROPY PASS']),
    ('@entropy-supported', []),
    ('nettest resolve 192.0.2.1', ['resolve 192.0.2.1: 192.0.2.1']),
    ('@dns-bounded', []),
    # 10/11/26 (Undertow, the web browser, user/undertow/): the program is in the initrd and answers; its own
    # end-to-end tests against web fixtures are tools/qemu-web.py (make test-web)
    ('undertow --help', ['usage: undertow [ADDRESS]', 'undertow --dump ADDRESS']),
    # 10/11/26 (track F): a page of the harness's HTTP server over HTTP, by --dump (the text, the status line on stderr,
    # exit status 0), then in the browser (Loading, the view, the prompt, q). QEMU's default CPU has no RDRAND, so no
    # HTTPS here: tools/qemu-web.py (make test-web) tests the rest against its fixtures
    ('undertow --dump http://10.0.2.2:{http}/undertow-dump', ['SurfOS HTTP test OK: /undertow-dump',
                                                              'HTTP 200 OK, HTTP unencrypted']),
    ('echo status $?', ['status 0']),
    ('undertow http://10.0.2.2:{http}/undertow-view', ['Loading http://10.0.2.2:', '[HTTP unencrypted]',
                                                       'SurfOS HTTP test OK: /undertow-view', 'undertow> '], [('q', [])]),
    # -- track S (the session, the screen, the commands): the welcome screen, what HTTPS needs (this CPU has no
    # RDRAND), the undertow> prompt and quit; bad usage (exit 2); an address that is refused (exit 1, and in the
    # browser, a message under "Loading ..."; the connection to the host's port 1 fails, or the stand-ins refuse it)
    ('undertow', [shown('Undertow 1.0 - the SurfOS web browser'), 'Clock  ', 'HTTPS  ', 'help: all commands',
                  'undertow> '], [('quit', [])]),
    ('undertow --dump', ['undertow: --dump takes one address', 'usage: undertow [ADDRESS]', '[exit status 2]']),
    ('undertow -x', ['undertow: no option -x', '[exit status 2]']),
    ('undertow --dump ftp://10.0.2.2/', ['undertow: cannot open ftp://10.0.2.2/: unsupported URL scheme',
                                         '[exit status 1]']),
    ('undertow --dump http://10.0.2.2:1/', ['undertow: http://10.0.2.2:1/: ', '[exit status 1]']),
    ('undertow http://10.0.2.2:1/', ['Loading http://10.0.2.2:1/ ...', 'undertow: ', 'undertow> '], [('quit', [])]),
    # 10/25/26 (wundertow, the web browser in a window, user/wundertow/): a page over HTTP from the harness's server
    # laid out (Undertow's transport), the sample site from a file (its headings at twice the size, bold), a page
    # painted into a BMP picture, bad usage (a width past 4096 too), a file that is not there. Its window is the
    # desktop's (the screen tests); 10/26/26: here, where no desktop runs, it says there is none, and loads nothing.
    ('wundertow --layout http://10.0.2.2:{http}/wundertow-layout', ['address: http://10.0.2.2:', 'status: 200',
                                                                    'SurfOS HTTP test OK: /wundertow-layout']),
    ('wundertow --layout /initrd/www/index.html', ['address: file:///initrd/www/index.html', 'title: SurfOS',
                                                    ' s32 b "SurfOS"', ' s24 b "This page came from SurfOS."']),
    ('wundertow --render /initrd/www/index.html /hda1/WEB.BMP', ['wundertow: /hda1/WEB.BMP: 640x',
                                                                 'pixels of file:///initrd/www/index.html ("SurfOS")']),
    ('rm /hda1/WEB.BMP', []),
    ('wundertow --layout', ['usage: wundertow [ADDRESS]', '[exit status 2]']),
    ('wundertow --render /initrd/www/index.html /hda1/WEB.BMP 99999', ['WIDTH: 64 to 4096 pixels (640)',
                                                                       '[exit status 2]']),
    ('wundertow --layout /initrd/nowhere.html', ['wundertow: /initrd/nowhere.html: ', '[exit status 1]']),
    ('wundertow /initrd/www/index.html', ["wundertow: no window: this terminal is no desktop's", '[exit status 1]']),
    # 11/1/26 (stage 2.3): as the window loads, through a helper process and a pipe; what it could not load comes back
    # as a sentence; no task is left of them
    ('wundertow --load http://10.0.2.2:{http}/wundertow-layout', ['address: http://10.0.2.2:', 'status: 200',
                                                                  'SurfOS HTTP test OK: /wundertow-layout']),
    # 11/01/26: with its linked sheet, /initrd/www/style.css, as a page's sheets load behind it
    ('wundertow --load /initrd/www/index.html', ['address: file:///initrd/www/index.html', 'title: SurfOS',
                                                 ' s32 b color=ffffff "SurfOS"']),
    ('wundertow --load /initrd/nowhere.html', ['wundertow: /initrd/nowhere.html: ', '[exit status 1]']),
    ('wundertow --load http://10.0.2.2:1/', ['wundertow: 10.0.2.2', '[exit status 1]']),
    ('ps', ['surfsh (ring 3)'], [], ['wundertow']),
    ('nettest info', ['eth0: 52:54:00:12:34:56, link up', 'inet 10.0.2.15']),
    ('@nettest-host', []),
    ('nettest get 10.0.2.2 {http} /nettest', ['HTTP/1.0 200 OK', 'SurfOS HTTP test OK: /nettest', 'bytes received']),
    ('nettest refuse 10.0.2.2 1', ['connect to 10.0.2.2 port 1: Connection refused']),
    ('nettest connect 10.0.2.99 23', ['connect to 10.0.2.99 port 23: No route to host']),
    ('nettest limits', ['NETTEST PASS']),
    ('@nettest-kills', []),
    # 10/9/26 (the development loop, track D): rx, with which tools/devloop.py pushes builds onto the laptop's data
    # stick, through the forwarded port RX_PORT: 300 KB and their CRC-32, then a smaller file in their place; a
    # connection reset in the middle (the file before stays, nothing partial is left); its refusals (no port, a
    # missing directory, a read-only file system, a directory, a port in use). The initrd's build id.
    ('run rx', ['usage: rx <port> <path>', 'exit status 2']),
    ('run rx 0 /hda1/RX.BIN', ['rx: 0 is no port (1-65535)', 'exit status 2']),
    (f'run rx {RX_PORT} /hda1/nodir/RX.BIN', ['rx: /hda1/nodir/RX.BIN.part: No such file or directory', 'exit status 1']),
    (f'run rx {RX_PORT} /initrd/RX.BIN', ['rx: /initrd/RX.BIN.part: Read-only file system', 'exit status 1']),
    (f'run rx {RX_PORT} /hda1', ['rx: /hda1: Is a directory', 'exit status 1']),
    ('@rx-push', []),
    ('@rx-reset', []),
    ('@rx-busy', []),
    ('cat /initrd/etc/build-id', [re.compile(rb'\n(?:[0-9a-f]{7,40}|unknown)(?:-dirty)? \d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ\r?\n')]),
    # TCP against an other end that makes its segments by hand (tools/qemu-tcp.py, tools/rawnet.py): a boot of its
    # own whose NIC is on a raw link, shared by these steps (TCP_RAW above; tools/qemu-tcp.py says what each does)
    *[('@tcp-raw ' + name, []) for name in TCP_RAW],
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
    # 10/25/26 (track A, A4): on the PS/2 keyboard, the modifiers as xterm sends them: Ctrl+Left ESC[1;5D, Shift+PgUp
    # ESC[5;2~, F5 ESC[15~ (dropped until then), Shift+Tab ESC[Z, Alt+x ESC x
    ('@rawkeys-mods', []),
    # 10/25/26 (track A, A5): surfsh's line editor on the PS/2 keyboard, where Alt reaches it as ESC and the key
    # together: Alt+., Alt+B and Alt+D, Ctrl+Left, Ctrl+R
    ('@editor-keys', []),
    # escape sequences (tty.h): the test pattern, then read back from VGA memory with its colours
    ('ansi', ['ANSI-TEST done, 80x25']),
    ('@ansi-screen', []),
    ('@console-switch', []),                                 # F2, F1: the serial terminal repainted
    # 10/25/26 (track A, A8): the alternate screen, left by the program or for it by the console when it is killed
    ('@alt-screen', []),
    # 10/25/26 (track A, A9): less, on the alternate screen with the keys from /dev/tty
    ('@less', []),
    # 10/25/26 (track A): a TAB reaches the serial terminal as it was printed (the screen: test_pager, qemu-usb's fullhd)
    ("printf 'a\\tb|\\n'", ['a\tb|']),
    # 10/25/26 (track A, A3): time, how long a command took (its status is the command's)
    ('time sleep 200; time false; echo status $?', ['slept 200 ms', re.compile(rb'real [0-9]+\.[0-9]{2} s'),
                                                   re.compile(rb'real [0-9]+\.[0-9]{2} s'), 'status 1']),
    # 10/6/26 (the console's character set, the pager): a line of UTF-8 reaches the serial port as it
    # was printed, and VGA memory as code page 437 cells
    ('run pagetest utf8', ['UTF8: SURF ▸ ≋ … café ÄÖÜ ñ ½ ° ± ┌─┐ € END', 'exit status 0']),
    ('@utf8-screen', []),
    ('run pagetest caps', ['TTY_CAPS 7: UTF-8 colour alternate-screen', 'exit status 0']),   # no `ascii`; (A8) ESC[?1049h
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
    ('@boot ascii', [re.compile(rb'SURF ~' + _SGR + rb'>')], [('pagetest caps', ['TTY_CAPS 6: colour alternate-screen'])], []),
    # 10/17/26 (one stick): test_boot_volume must run here (bootvol= and rd1), not print its skip line
    # 10/24/26 (storage, milestone 1): and test_fat_refused, a stick that refuses writes: close says EIO, umount gives up
    ('selftest', ['ok   the volume GRUB was started from (bootvol=)', 'ok   with no partition table the disk itself',
                  "ok   refused writes: a save's new file goes into the cache, and its close says EIO",
                  'ok   refused writes: read again, the stick holds the old file, whole, and says not cleanly unmounted',
                  # 11/09/26 (track desk, the second review): the ends kept for the desktop (kernel/task.c); in the order
                  # the selftest prints (test_told_ends, after test_tasks)
                  "ok   a program's end kept for the desktop that started it, given once",
                  "ok   kernel stacks: mapped in full over an unmapped guard; a thread's unmapped when it is collected",
                  'ok   Ctrl+Alt+Del: a key held down is one press, a press after its break another; Delete repeats',
                  'SELFTEST PASS']),
    ('cat /hda1/SELFTEST.TXT', ['SurfOS wrote this file']),
    # 10/6/26 (surfsh, track K): a kernel command's status, 0 success, 1 failure (a refusal too), 2 a usage
    # error; $? in the ring-0 shell; 127 for no such command; a program's exit status through run. 10/25/26: kernel ls,
    # since the program ls comes first
    ('echo status $?', ['status 0']),
    ('kernel ls /hda1/no-such-file', ['No such file or directory']),
    ('echo status $?', ['status 1']),
    ('fdisk hda new mbr', ['hda will not be changed']),
    ('echo status $?', ['status 1']),
    ('fdisk', ['fdisk <disk> new mbr|gpt']),
    ('echo status $?', ['status 2']),
    ('no-such-command', ['surfsh: no-such-command: command not found']),
    ('echo status $?', ['status 127']),
    ('run hello', ['exit status 42']),
    ('echo status $?', ['status 42']),
    # 10/17/26 (the network manager): the settings with no stick to keep them (the command line names a volume, rd1,
    # which the self test has just put back as it was): the defaults; a key explained; a change in force but not
    # saved, saying why (status 1); keys and values it does not take (status 2); the eth0 lease of the dhcp step
    # renewed (QEMU's server answers), and ifconfig showing it; no Wi-Fi device, no simulated chip
    ('settings', ['settings: none read: the stick SurfOS was started from was not found at startup',
                  'telnetd      = off       (the default)', 'hostname     = surfos    (the default)',
                  '0 saved Wi-Fi networks (wifi saved)']),
    ('settings telnetd', ['telnetd = off   (the default; it takes on | off)',
                          'the telnet server at startup, on Ethernet or a WPA2 network (no password: your own '
                          'networks only)']),
    ('settings hostname smoke-test', ['hostname = smoke-test in force until SurfOS restarts, but not saved: the stick '
                                      'SurfOS was started from is not there (lsblk)']),
    ('echo status $?', ['status 1']),
    ('settings telnetd on', ['settings: telnetd = on not saved: the stick SurfOS was started from is not there (lsblk); it '
                             'would act at the next start only, so nothing changes (telnetd on starts the server now)']),
    ('echo status $?', ['status 1']),
    ('settings telnetd', ['telnetd = off   (the default; it takes on | off)']),
    ('settings', ['hostname     = smoke-test']),
    ('settings frobnicate on', ['settings: no setting called frobnicate (settings lists them)']),
    ('echo status $?', ['status 2']),
    ('settings telnetd.port = 0', ['settings: telnetd.port is 1-65535, not 0']),
    ('settings a b c d', ['usage: settings [key [value]]']),
    ('settings hostname=surfos', ['hostname = surfos in force until SurfOS restarts, but not saved']),
    ('dhcp renew', ['dhcp: eth0: lease 10.0.2.15 renewed for 86400 s']),
    ('ifconfig', ['inet 10.0.2.15', 'DHCP lease from 10.0.2.2: ', ' of 86400 left']),
    ('dhcp renew wlan7', ['no interface']),
    ('dhcp renew eth0 extra', ['usage: dhcp [renew] [interface]']),
    ('wifi saved', ['wifi: no Wi-Fi device']),
    ('wlansim off SurfSim', ['wlansim: no simulated Wi-Fi chip (the kernel word wlansim)']),
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
    ('run ptytest size 100 30', ['sizes out of range refused', 'TTY_GETSIZE 100x30', 'TTY_CAPS 7: UTF-8 colour alternate-screen',
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
                               'what was typed ahead never ran', 'cursor keys filled the queue',
                               'Ctrl+C ended the cut sequence', 'PTYTEST fullqueue PASS', 'exit status 0']),
    ('run ptytest paste', ["TTY_SKIP: of 41 pasted lines only", "raw mode's end: of 41 pasted lines only",
                           'Ctrl+C in the middle of the skip cut sleep short', 'a line typed after a pause, while sleep ran',
                           'PTYTEST paste PASS', 'exit status 0']),
    ('run ptytest detach-ctrlc', ['detached from surfsh', 'the detached sleeper lived on', 'PTYTEST detach-ctrlc PASS',
                                  'exit status 0']),
    ('@pty-isolate', []),
    ('run ptytest poll', ['timed out after', 'POLLNVAL', 'POLLHUP at once', '(POLLIN | POLLHUP)', 'two masters and the console',
                          'killed: gone after', 'PTYTEST poll PASS', 'exit status 0']),
    ('run ptytest ps', ['init', 'ptytest', 'PTYTEST ps PASS', 'exit status 0']),
    ('run ptytest detach', ['detached: parent 1', 'ECHILD', 'PTYTEST detach PASS', 'exit status 0']),
    # 10/26/26 (CODE-AUDIT T14): a pty's session shell sets the foreground job; a program in no job that is no shell
    # of the terminal (spawned so, or detached from a job in the background) may not
    ('run ptytest jobfg', ["the session's shell sets the foreground job and gives it back", 'spawned so or detached: EACCES',
                           'PTYTEST jobfg PASS', 'exit status 0']),
    # on a console, neither may a program the kernel's run started for the login shell, nor a $( )'s (both in no job,
    # their parent a shell that is a process)
    ('run ptytest jobfg-try none', ['JOBFG none: EACCES', 'exit status 0']),
    ('echo $(ptytest jobfg-try none)', ['JOBFG none: EACCES']),
    # a surfsh whose shell above it ends (killed while it sits at its prompt) is in no job and no shell of the pty's:
    # it goes on without job control, head -1 reads the pty
    ('run ptytest jobstrand', ['goes on without job control', 'PTYTEST jobstrand PASS', 'exit status 0']),
    ('@kmsg', []),
    # 10/24/26 (pipelines and scripting in surfsh): the kernel's pipes from user space (user/bin/pipetest.c): bytes
    # through and the end of the input; a stream from a child and one write() to a child; a full pipe and POLLOUT; two
    # writers' blocks of PIPE_BUF never mixed; a broken pipe (status 141, or EPIPE when asked); poll(); EMFILE,
    # ENFILE at PIPE_MAX and no pipe lost after 200; a kill cuts a reader's and a writer's wait short; the end held
    # back by a dup2() copy and by a child
    ('run pipetest basic', ['bytes through, the end of the input, EBADF, ESPIPE, pipefs, EFAULT', 'PIPETEST basic PASS',
                            'exit status 0']),
    ('run pipetest stream', ['262144 bytes from a child, in order', 'bytes to a child in one write()',
                             'PIPETEST stream PASS', 'exit status 0']),
    ('run pipetest full', ['8192 bytes go in at once; POLLOUT only with 4096 bytes of room',
                           'a write into a full pipe waited', 'PIPETEST full PASS', 'exit status 0']),
    ('run pipetest atomic', ['128 writes of 4096 bytes from two children, each whole', 'PIPETEST atomic PASS',
                             'exit status 0']),
    ('run pipetest broken', ['ended with status 141', 'with broken_pipe_exit(0) a write says EPIPE',
                             'PIPETEST broken PASS', 'exit status 0']),
    ('run pipetest poll', ['POLLIN, POLLHUP, POLLOUT, POLLERR', "poll() waited", 'PIPETEST poll PASS', 'exit status 0']),
    ('run pipetest limits', ['then EMFILE', 'held by five children and 2 here, then ENFILE',
                             '200 pipes made and closed; 32 there again', 'PIPETEST limits PASS', 'exit status 0']),
    ('run pipetest kill', ['a reader waiting on an empty pipe killed in', 'a writer waiting on a full pipe killed in',
                           'PIPETEST kill PASS', 'exit status 0']),
    ('run pipetest share', ['a dup2() copy holds the end of the input back', 'until it ended', 'PIPETEST share PASS',
                            'exit status 0']),
    # 10/24/26: spawnx(): an environment, a descriptor 3, a closed descriptor, spawnfd() passing environ on, the errors
    ('run pipetest spawnx', ["a child's environment and its descriptor 3", 'a child with descriptor 0 closed',
                             'spawnfd() passes environ on', 'E2BIG, EINVAL, EBADF, EFAULT', 'PIPETEST spawnx PASS',
                             'exit status 0']),
    # 10/25/26: waitx(): a child that ended by itself, one that was killed, WNOHANG, the errors
    ('run pipetest waitx', ['waitx says a child ended by itself (status 0), one was killed; WNOHANG while it runs',
                            'waitx: ECHILD, EINVAL, EFAULT', 'PIPETEST waitx PASS', 'exit status 0']),
    # 10/25/26 (windows for programs, user/bin/wintest.c): where no desktop runs, the window calls' refusals: the
    # arguments first (EFAULT, EINVAL), then ENODEV; winctl on what is no window. wintest pty: the same on a pty of
    # its own, @telnet-win: in a telnet session, both with no desktop running here (nodesk checks: the refusal while
    # a desktop runs is the guiapps scenario's, tools/gui_apps_win.py)
    ('run wintest nodesk', ['EFAULT: no description, one below the program, one where nothing is mapped',
                            'EINVAL: version 2, an unknown flag, no width, limits that leave no width, a colour',
                            'ENODEV: a good window, on a terminal that is no desktop', 'EBADF, EINVAL: winctl',
                            'WINTEST nodesk PASS', 'exit status 0']),
    ('run wintest pty', ['wintest nodesk on pty', 'ENODEV, as on a console where no desktop runs', 'WINTEST pty PASS',
                         'exit status 0']),
    # boots of their own (@boot: kernel command-line words; no-initrd; surfsh=<program or file in the initrd>, an
    # initrd whose bin/surfsh is that), for init's choice of shell. shell=kernel: the ring-0 shell on every console
    # on purpose, elevated (#), no recovery line, and started again elevated after crashgp; no initrd: no
    # /initrd/bin/surfsh, a recovery console (?) at once with init's line; a surfsh that ends at once (hello: 42):
    # started again twice, then the recovery console; one that is not an executable: the recovery console at once;
    # one that runs (sh): an exit after 2 s with status 3 is said, with 0 not, a kill is said ('@sleep n' waits)
    # 10/6/26: Ctrl+C there cuts a command short with status 130: a sleep, and a program it runs (killed)
    # 10/25/26 (robustness, L21): and poweroff there, the ACPI tables read at boot: ACPI S5 stops QEMU (no disk: the
    # laptop profile skips ATA)
    ('@boot profile=laptop mouse=off', [PROMPT], [('pointerinfo', ['disabled (mouse=off)', 'pointing devices: 0']), ('echo keyboard-survives', ['keyboard-survives']),
                                                  ('shutdown-laptop', CHECK)], ['still on 3 s', 'safe to turn off']),
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
    # 10/26/26 (CODE-AUDIT T14): a surfsh typed at the ring-0 shell, in no job, still makes its jobs the terminal's
    # foreground (the ring-0 shell has no jobs of its own): head -1 reads the terminal, Ctrl+Z stops a count, fg brings
    # it back, kill %1 ends it, exit returns to the ring-0 shell; nothing stops for reading from the background
    ('@boot shell=kernel', [surf_prompt(b'SURF', b'#')], [(b'surfsh\r', [PROMPT]),
                                                          (b'head -1\r', []), ('@sleep 1', []),
                                                          (b'typed-at-head\r', ['typed-at-head', PROMPT]),
                                                          (b'count 1000 100\r', [' 3 ']), (b'\x1a', ['stopped']),
                                                          ('@sleep 1', []),
                                                          (b'fg\r', ['count 1000 100']), ('@sleep 1', []),
                                                          (b'\x1a', ['stopped']), ('@sleep 1', []),
                                                          ('kill %1', []), ('@sleep 1', []),
                                                          ('echo after-kill', ['after-kill']), ('@sleep 1', []),
                                                          (b'exit\r', [surf_prompt(b'SURF', b'#')])],
     ['reading the terminal']),
    # 10/18/26 (audit B56): a fault in kernel mode while the task is inside a file system (crashfs: as in a device
    # request) stops the system, saying why: nothing could give back what the task held there. The boot ends halted
    # ('halts': its wipeout is what is checked, not the absence of one)
    ('@boot shell=kernel', [surf_prompt(b'SURF', b'#')],
     [('crashfs', ['Kernel Wipeout: Page Fault', 'inside a file system, the block cache or a device request',
                   re.compile(rb'SYSTEM HALTED')])],
     ['init: restarted the shell'], 'halts'),
    # 10/18/26 (audit B62): so does one while the task holds a lock (crashlock: a mutex of its own), which would stay
    # with the dead task for good
    ('@boot shell=kernel', [surf_prompt(b'SURF', b'#')],
     [('crashlock', ['Kernel Wipeout: Page Fault', 'holding a lock (1 held)', re.compile(rb'SYSTEM HALTED')])],
     ['init: restarted the shell'], 'halts'),
    ('@boot no-initrd', ['init: /initrd/bin/surfsh: No such file or directory; recovery console', surf_prompt(b'SURF', b'?')],
     [('cd /dev', [surf_prompt(b'dev', b'?')]), ('ls /nothing', ['No such file']), ('echo status $?', ['status 1'])], []),
    ('@boot surfsh=hello', ['Hello from user mode', 'init: surfsh exited at once (status 42); restarted the shell',
                            'init: surfsh exited at once (status 42); restarted the shell',
                            'init: surfsh exited at once 3 times in 30 s (status 42); recovery console',
                            surf_prompt(b'SURF', b'?')], [('ps', ['Shell 0'])], []),
    ('@boot surfsh=etc/version', ['init: /initrd/bin/surfsh could not be loaded; recovery console', surf_prompt(b'SURF', b'?')],
     [], ['restarted the shell']),
    # 10/25/26 (the review of B1): a shell whose loading fails for want of memory (toobig: 1000 MB of data) is tried
    # again every second, never given up for the recovery console as one that is no program is
    ('@boot surfsh=toobig', ['/initrd/bin/surfsh: out of memory for a segment',
                             'init: /initrd/bin/surfsh: Out of memory; trying again'],
     [('@sleep 6', [])], ['recovery console', 'toobig: loaded', 'could not be loaded']),
    # 10/9/26 (the development loop, track D): the kernel word autonet, in a boot with QEMU's network (the word gives
    # the boot a network): the link, a lease, telnetd started on F1 with nobody typing; a telnet session gets in
    ('@boot autonet', ['+AUTONET: waiting up to 90 s for a network link', '+AUTONET: eth0: link up after',
                       'dhcp: eth0: lease 10.0.2.15', 'telnetd: listening on 10.0.2.15 port 23',
                       '+AUTONET: done: telnet 10.0.2.15'],
     [(autonet_telnet, ['autonet-telnet-ok']), ('telnetd', ['telnetd: running (pid', '0 session shells'])], []),
    ('@boot surfsh=sh', ['SurfOS user shell', SH_PROMPT],
     [('@sleep 2.5', []), ('exit 3', ['init: surfsh exited (status 3); restarted the shell', 'SurfOS user shell', SH_PROMPT]),
      ('@sleep 2.5', []), ('exit', ['SurfOS user shell', SH_PROMPT]),
      (own_shell_killed, ['init: surfsh was killed; restarted the shell', 'SurfOS user shell', SH_PROMPT])],
     ['exited (status 0)', 'recovery console']),
    # 10/25/26 (robustness, L16-L18): crashstack uses up surfsh's kernel stack (a kernel command runs on it) until it
    # runs into the guard below; the double fault's task gate reports it with surfsh's name and a backtrace, and halts.
    # crashstack near: a trap within KSTACK_LOW of the bottom halts with "nearly full", reported from the same task.
    # The kernel words are only these boots' names: nothing reads them. Boots that halt on purpose: the step expects
    # SYSTEM HALTED, so boot_step() does not count it as a fall
    ('@boot crashstack', [PROMPT],
     [('crashstack', ["kernel stack overflow in 'surfsh' (pid ", 'its stack: 0x9', 'Backtrace:', 'crash_down',
                      'more times)', 'cmd_crashstack+0x', re.compile(rb'SYSTEM HALTED')])],
     ['Kernel Wipeout', 'Kernel panic', 'did not stop it']),
    ('@boot crashstack near', [PROMPT],
     [('crashstack near', ["Kernel panic: the kernel stack of 'surfsh' (pid ", 'is nearly full: a trap frame ',
                           'vector 64 (yield)', 'crash_near', re.compile(rb'SYSTEM HALTED')])],
     ['Kernel Wipeout', 'stack overflow', 'did not stop it']),
    # 10/25/26 (robustness, L22-L24): three boots with the same stick (settingsdisk=shutdown), mounted read-write on
    # /hda1 as SurfOS's own volume, each writing a file there and then shutting down: reboot; poweroff (settings=off:
    # the network manager leaves the stick alone); Ctrl+Alt+Del through the keyboard's interrupt (the monitor's
    # sendkey; kbd=irq only names that boot: the legacy profile's keyboard is on its IRQ anyway). Each says its steps
    # and stops QEMU (shutdown_boot_check()); the next boot's mount finds the volume cleanly unmounted, and
    # @shutdown-disk reads the stick after the last. poweroff under profile=laptop: '@boot profile=laptop' above
    ('@boot settingsdisk=shutdown', [PROMPT], [('shutdown-reboot', CHECK)],
     ['not unmounted cleanly', 'still on 3 s', 'safe to turn off']),
    ('@boot settingsdisk=shutdown settings=off', [PROMPT], [('shutdown-poweroff', CHECK)],
     ['not unmounted cleanly', 'still on 3 s', 'safe to turn off']),
    ('@boot settingsdisk=shutdown kbd=irq', [PROMPT], [('shutdown-key', CHECK)],
     ['not unmounted cleanly', 'still on 3 s', 'safe to turn off']),
    ('@shutdown-disk', []),
    # 10/9/26 (Wi-Fi, track S): the station layer, the wifi command and two interfaces on one network, with the simulated
    # chip (`wlansim`, driver/wifi/wlansim.c) and QEMU's user network (`net`: eth0, which the simulated access points are
    # bridged to). Down at first; up; a scan listing its six networks (the hidden one as such); the ones SurfOS does not
    # join refused with the reason; a hidden network joined by name and left; the open SurfSim joined by its BSSID (an
    # unknown one is no access point in range); eth0 and wlan0 each leased their own address by QEMU's DHCP server, pings
    # through each, the default route eth0 (Ethernet before Wi-Fi); 10.0.2.77 on the access point's own network answering
    # each ping twice more as fragments, which wlan0 drops (review L5: "4 fragments dropped" after two pings), and
    # ARP with two A-MSDUs of 64 replies, which wlan0 takes without losing one (wlan-no-drops: its share of the receive
    # queue, review L1); ARP per interface, wlan0's own broadcasts, which the access point sends back, dropped and never
    # learned from (wlan-own-frames); TCP out; telnet to each interface's
    # address answered through that interface (BOOT_CHECKS: wlansim-telnet); wifi leave while a telnet session to wlan0's
    # address streams through it, and no frame reaches the chip after leave() (wlansim-tx-race; the chip would say so);
    # a scan cut short by Ctrl+C, whose end the chip reports during the next scan, which lists the networks all the same
    # (review L2), and wifi up refused as busy while a telnet session scans (wlansim-scan-abort); Ctrl+C at the password
    # prompt on the console and over telnet joins nothing (wlansim-ctrl-c);
    # WPA2 typed at the hidden prompt (wlansim-wpa:
    # refused until track C's supplicant is in); a network that deauthenticates the station once, then loses its beacons
    # for 4 s: joining it forgets the address of the network before, and its own lease survives the link going down twice
    # and coming back up on its own; leaving it forgets the address and the ARP entries; down. A telnet session to eth0's
    # address is held open from before that join to after wifi down and answers all along (eth0-held). The passphrases typed
    # never show. 10/25/26 (the programs' review of track A, U3): a password typed ahead during a join that never asks for
    # one is dropped on the join's way out: it never runs nor shows, and the history holds no such line; a line typed
    # ahead while a command (sleep) runs still runs after it, as in bash.
    ('@boot wlansim net', ['wlansim: simulated Wi-Fi chip registered as wlan0', PROMPT],
     [('wifi status', ['wlan0: down (wifi up starts it)', 'wlan0: MAC 00:00:00:00:00:00, link down']),
      ('wifi scan', ['wifi: wlan0 is down (wifi up first)']),
      ('wifi up', ['wlansim: the simulated chip is on: 6 access points, bridged to eth0', 'wlan0: up, MAC 52:54:00:12:34:57',
                   'wlan0: up, not associated']),
      ('wifi scan', ['SSID', 'BSSID', 'SECURITY', 'SurfSim ', '02:53:49:4d:00:01', '6  2.4 GHz', '-45 dBm', 'open',
                     'SurfSimWPA', '36  5 GHz', 'WPA2-Personal', 'SurfSimFlaky', 'SurfSimSAE', 'WPA3-Personal',
                     '(hidden network)', '02:53:49:4d:00:03', 'SurfSimWEP', 'WEP', '6 networks']),
      ('wifi join SurfSimWEP', ['SurfSimWEP uses WEP, which SurfOS does not support']),
      ('wifi join SurfSimSAE', ['SurfSimSAE is WPA3-Personal only (SAE): not supported yet']),
      ('wifi join NoSuchNetwork', ['no network named NoSuchNetwork in range']),
      (b'wifi join NoSuchNetwork\r', []), ('@sleep 0.3', []),
      (b'typed-ahead-secret\r', ['no network named NoSuchNetwork in range', PROMPT]),
      (b'sleep 1500\r', []), ('@sleep 0.4', []),
      (b'echo typed-ahead-ran\r', ['slept 1500 ms', '\r\ntyped-ahead-ran\r\n', PROMPT]),
      ("history | grep -c 'typed-ahead-secre[t]'", ['\r\n0\r\n']),
      ('wifi join SurfSimHidden', ['link up: "SurfSimHidden" (02:53:49:4d:00:03), channel 11', 'joined SurfSimHidden']),
      ('wifi leave', ['wlan0: left "SurfSimHidden"', 'wlan0: not associated']),
      ('wifi join SurfSim 02:53:49:4d:00:09', ['no access point 02:53:49:4d:00:09 named SurfSim in range']),
      ('wifi join SurfSim 02:53:49:4d:00:01', ['joining SurfSim (open)', 'wlansim: SurfSim: the station is associated (QoS)',
                             'wlan0: link up: "SurfSim" (02:53:49:4d:00:01), channel 6, 2.4 GHz, -45 dBm, open',
                             'wlan0: associated with "SurfSim"', 'joined SurfSim: dhcp wlan0 gets an address']),
      ('dhcp', ['dhcp: eth0: lease 10.0.2.15']),
      ('dhcp wlan0', ['dhcp: wlan0: lease 10.0.2.16', 'wlan0: 52:54:00:12:34:57, link up', 'inet 10.0.2.16']),
      ('ping 10.0.2.2 2 wlan0', ['PING 10.0.2.2 from 10.0.2.16 (wlan0)', '64 bytes from 10.0.2.2',
                                 '2 packets transmitted, 2 received']),
      ('ping 10.0.2.2 2 eth0', ['PING 10.0.2.2 from 10.0.2.15 (eth0)', '2 packets transmitted, 2 received']),
      ('ping 10.0.2.2 1', ['PING 10.0.2.2 from 10.0.2.15 (eth0)', '1 received']),
      ('ping 10.0.2.2 1 wlan9', ['ping: no interface wlan9']),
      ('ping 10.0.2.2 wlan0', ['PING 10.0.2.2 from 10.0.2.16 (wlan0)', '4 packets transmitted, 4 received']),
      ('ping 10.0.2.2 wlan9', ['usage: ping <address|name> [count] [interface]']),
      ('ping 10.0.2.77 2 wlan0', ['PING 10.0.2.77 from 10.0.2.16 (wlan0)', '2 packets transmitted, 2 received']),
      ('wlan-no-drops', CHECK),
      ('wifi status', ['frames:', ' 4 fragments dropped, ']),
      ('arp', ['IFACE', '10.0.2.2', 'wlan0', '10.0.2.2', 'eth0']),
      ('wlan-own-frames', CHECK),
      ('httpget 10.0.2.2 {http} /wlan', ['SurfOS HTTP test OK: /wlan', 'bytes received']),
      ('telnetd on', ['telnetd: listening on 10.0.2.15 port 23']),
      ('wlansim-telnet', CHECK),
      ('wifi status', ['wlan0: associated with "SurfSim"', 'frames:', 'wlan0: MAC 52:54:00:12:34:57, link up, inet 10.0.2.16']),
      ('wlansim-tx-race', CHECK),
      ('wlansim-scan-abort', CHECK),
      ('wlansim-ctrl-c', CHECK),
      ('wifi join SurfSim', ['joined SurfSim']),
      ('dhcp wlan0', ['dhcp: wlan0: lease 10.0.2.16']),
      ('wlansim-wpa', CHECK),
      ('eth0-held', CHECK),
      ('wifi join SurfSimFlaky', ['wlan0: address 10.0.2.16 forgotten', 'wlan0: link up: "SurfSimFlaky"',
                                  'joined SurfSimFlaky']),
      ('dhcp wlan0', ['dhcp: wlan0: lease 10.0.2.16']),
      ('eth0-held', CHECK),
      ('@sleep 8', []),
      ('eth0-held', CHECK),
      ('@sleep 8', []),
      ('eth0-held', CHECK),
      ('@sleep 8', []),
      ('dmesg', ['link down: "SurfSimFlaky": deauthenticated by the access point: reason 4 inactivity',
                 'link up again: "SurfSimFlaky"', 'SurfSimFlaky goes silent for 4 s',
                 'link down: "SurfSimFlaky": the access point is gone (beacons lost)',
                 'joining "SurfSimFlaky" again failed (try 1 of 5): no such network in range; next try in 6 s',
                 'link up again: "SurfSimFlaky"']),
      ('wifi status', ['wlan0: associated with "SurfSimFlaky"', 'inet 10.0.2.16']),
      ('ping 10.0.2.2 1 wlan0', ['PING 10.0.2.2 from 10.0.2.16 (wlan0)', '1 received']),
      ('wifi leave', ['wlan0: left "SurfSimFlaky" (wifi leave)', 'wlan0: address 10.0.2.16 forgotten',
                      'wlan0: not associated']),
      ('wlan-forgotten', CHECK),
      ('eth0-held', CHECK),
      ('wifi down', ['wlan0: down']),
      ('wifi status', ['wlan0: down (wifi up starts it)']),
      ('eth0-held-end', CHECK)],
     [WLANSIM_PASSPHRASE, WLANSIM_WRONG, 'handed over after leave() or stop()', 'carries Ethernet padding',
      'typed-ahead-secret']),
    # 10/9/26 (the review of track S, M1): the laptop's order. The simulated chip registers wlan0 at boot and the USB
    # Ethernet adapter is plugged in later (QEMU's usb-net on an xHCI, `usbnet-later`): eth0 comes after wlan0. wlan0
    # leases its address first, while eth0 has none (10.0.2.15), eth0 then 10.0.2.16; a ping with no interface, netinfo
    # (telnetd's address) and the rest take eth0 all the same (Ethernet before Wi-Fi), and telnet to each address is
    # answered through its own interface. (Review M2:) leaving forgets wlan0's address and ARP entries, and joining another
    # network does not bring the old address back; a telnet session to eth0's address held open meanwhile answers all along.
    # Two dhcp at once (one port 68): wlan0's on SurfSimHidden's island, where no server answers, and eth0's, which waits
    # for it and then gets its lease (dhcp-one-at-a-time)
    ('@boot wlansim usbnet-later', ['wlansim: simulated Wi-Fi chip registered as wlan0', PROMPT],
     [('usbnet-plug', CHECK),
      # (11/1/26: QEMU's usb-net reports the address it was given, but with 40 as its first byte on some versions,
      # as tools/qemu-usb.py's USBNET_MAC says)
      ('ifconfig', ['wlan0: 00:00:00:00:00:00', re.compile(rb'eth0: (?:52|40):54:00:12:34:56'), PROMPT]),
      ('wifi up', ['bridged to eth0', 'wlan0: up, not associated']),
      ('wifi join SurfSim', ['joined SurfSim']),
      ('dhcp wlan0', ['dhcp: wlan0: lease 10.0.2.15']),
      ('dhcp', ['dhcp: eth0: lease 10.0.2.16']),
      ('ping 10.0.2.2 2', ['PING 10.0.2.2 from 10.0.2.16 (eth0)', '2 packets transmitted, 2 received']),
      ('ping 10.0.2.2 2 wlan0', ['PING 10.0.2.2 from 10.0.2.15 (wlan0)', '2 packets transmitted, 2 received']),
      ('telnetd on', ['telnetd: listening on 10.0.2.16 port 23']),
      ('wlansim-telnet', CHECK),
      ('eth0-held', CHECK),
      ('wifi leave', ['wlan0: address 10.0.2.15 forgotten', 'wlan0: not associated']),
      ('wlan-forgotten', CHECK),
      ('eth0-held', CHECK),
      ('wifi join SurfSimHidden', ['joined SurfSimHidden']),
      ('wifi status', ['wlan0: associated with "SurfSimHidden"', 'inet 0.0.0.0']),
      ('ping 10.0.2.2 1', ['PING 10.0.2.2 from 10.0.2.16 (eth0)', '1 received']),
      ('eth0-held-end', CHECK),
      ('dhcp-one-at-a-time', CHECK)],
     ['handed over after leave() or stop()', 'carries Ethernet padding']),
    # 10/17/26 (the network manager): boots from the owner's stick (settingsdisk=owner: a FAT32 disk the command line
    # names as GRUB would), kept from one boot to the next. 1: the networks.conf written by hand is read: the bad network
    # told by its line, left out and kept in the file, the test network's password made into its key, the card started,
    # the network joined and its address leased with nobody typing, and no telnetd (not asked for). settings.conf is
    # made by the first change (the template, the line set), with telnetd's warning; after a scan, a network in range
    # saved by its name (open: no question), one SurfOS cannot join refused; forget; wifi save of the network joined
    # puts it first again; the lease renewed; the files as SurfOS wrote them (the key, never the password; the bad
    # network kept as it was, after the others), nothing left half written
    ('@boot wlansim net settingsdisk=owner',
     ['+NET: networks.conf line 7: psk is 64 hexadecimal digits: the network "Broken" is left out (and kept in the '
      'file as it is)',
      '+NET: settings from hda1 (surfos-config/): telnetd off, wifi on, ethernet off, hostname surfos; 1 saved Wi-Fi '
      'network (some lines were not understood: above)',
      '+NET: wlan0: starting the Wi-Fi card to join a saved network',
      '+NET: wlan0: SurfSimWPA is in range (-52 dBm): joining it', 'wlan0: link up: "SurfSimWPA"',
      '+NET: wlan0: joined SurfSimWPA; asking for an address (DHCP)', 'dhcp: wlan0: lease 10.0.2.15'],
     [('settings', ['settings: surfos-config/settings.conf on hda1, the stick SurfOS was started from (no such file '
                    'yet: the defaults)', 'telnetd      = off       (the default)', '1 saved Wi-Fi network (wifi saved)']),
      ('wifi saved', ['saved Wi-Fi networks on hda1 (surfos-config/networks.conf), tried in this order:',
                      ' 1  SurfSimWPA', 'WPA2  joined now', 'surfos-config/networks.conf also has 1 part SurfOS did '
                      'not understand, kept there as it was']),
      ('telnetd', ['telnetd: not running']),
      ('settings telnetd on', ['telnetd = on, saved on hda1 (surfos-config/settings.conf): at the next start '
                               '(telnetd on starts it now)', 'telnetd has no password and no encryption',
                               'SurfOS joins no open Wi-Fi network by itself']),
      ('settings hostname = smoketest', ['hostname = smoketest, saved on hda1 (surfos-config/settings.conf): in the '
                                         'next DHCP request']),
      ('settings wifi maybe', ['settings: wifi is on | off, not maybe']),
      ('settings aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=1', ['settings: no setting called aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa '
                                                     '(settings lists them)']),
      ('wifi scan', ['SurfSimFlaky', 'SurfSimSAE']),
      ('wifi save SurfSim', ['saved SurfSim (open) on hda1: SurfOS joins it by itself at startup, the first of 2 saved '
                             'networks it tries']),
      ('wifi save SurfSimSAE', ['SurfSimSAE is WPA3-Personal only (SAE): not supported yet']),
      ('wifi forget NoSuchNet', ['wifi: NoSuchNet is not a saved network (wifi saved lists them)']),
      ('wifi forget', ['usage: wifi forget <ssid>']),
      ('wifi save SurfSimFlaky', ['saved SurfSimFlaky (open) on hda1', 'the first of 3 saved networks it tries']),
      ('wifi forget SurfSimFlaky', ['forgot SurfSimFlaky: SurfOS no longer joins it by itself (2 saved networks left)']),
      ('wifi save', ['saved SurfSimWPA (WPA2) on hda1 again: SurfOS joins it by itself at startup, the first of 2 saved '
                     'networks it tries']),
      ('wifi saved', [' 1  SurfSimWPA', 'WPA2  joined now', ' 2  SurfSim ', 'open  in range, -45 dBm']),
      ('dhcp renew wlan0', ['dhcp: wlan0: lease 10.0.2.15 renewed for 86400 s']),
      ('dhcp renew eth0', ['dhcp: eth0 has no lease to renew (dhcp eth0 gets one)']),
      ('cat /hda1/surfos-config/settings.conf', ['# SurfOS settings, read at every start', 'telnetd = on',
                                                 '#telnetd.port = 23', '#wifi = on', 'hostname = smoketest']),
      ('cat /hda1/surfos-config/networks.conf', ['# Wi-Fi networks SurfOS joins by itself', 'network = "SurfSimWPA"',
                                                 'security = wpa2', 'psk = ' + SURFSIM_PSK, 'network = "SurfSim"',
                                                 'security = open', '# SurfOS did not understand the lines below',
                                                 'network = "Broken"', 'security = wpa2', 'psk = 1234']),
      ('kernel ls /hda1/surfos-config', ['networks.conf', 'settings.conf', '2 entries']),   # 10/25/26: the kernel's count
      # 10/18/26: each of these boots ends with the stick unmounted, as the owner ejects it before pulling it out:
      # mounted read-write (here at boot, as /hda1) a FAT volume is marked as not cleanly unmounted until then
      # (fs/fat.c), and QEMU's quit is a pull, which fsck.fat on the host would report in @settings-disk
      ('umount /hda1', [])],
     [WLANSIM_PASSPHRASE, '.new', 'starting telnetd on F1 (the settings', 'umount: /hda1']),
    # 2: the same stick again: the settings and both networks read (the bad one told again); the first saved network
    # joined, its lease, telnetd started by the setting once there is an address on that WPA2 network (that address on
    # its line), a telnet session through it. wifi leave is left alone by the keeper; wifi connect joins again. The
    # access point switched off: the station gives up after its five tries, and the keeper does not join SurfSim by
    # itself, an open network, while the telnet server runs; once it is stopped, the keeper's next look joins SurfSim
    # and gets its address
    ('@boot wlansim net settingsdisk=owner',
     ['psk is 64 hexadecimal digits: the network "Broken" is left out (and kept in the file as it is)',
      '+NET: settings from hda1 (surfos-config/): telnetd on, wifi on, ethernet off, hostname smoketest; 2 saved Wi-Fi '
      'networks (some lines were not understood: above)', '+NET: wlan0: SurfSimWPA is in range (-52 dBm): joining it',
      'wlan0: link up: "SurfSimWPA"', '+NET: wlan0: joined SurfSimWPA; asking for an address (DHCP)',
      '+NET: wlan0: 10.0.2.15; starting telnetd on F1 (the settings: telnetd = on)',
      'telnetd: listening on 10.0.2.15 port 23'],
     [('wifi status', ['wlan0: associated with "SurfSimWPA"', 'inet 10.0.2.15']),
      (autonet_telnet, ['autonet-telnet-ok']),
      ('wifi leave', ['wlan0: left "SurfSimWPA" (wifi leave)', 'wlan0: not associated']),
      ('@sleep 4', []),
      ('wifi status', ['wlan0: up, not associated']),
      ('wifi connect', ['wlan0: SurfSimWPA is in range (-52 dBm): joining it', 'wlan0: link up: "SurfSimWPA"',
                        'wlan0: joined SurfSimWPA; asking for an address (DHCP)', 'dhcp: wlan0: lease 10.0.2.15']),
      ('wifi connect', ['wlan0 is joined to SurfSimWPA already']),
      ('wlansim off SurfSimWPA', ['wlansim: SurfSimWPA is off']),
      ('keeper-skips-open', CHECK),
      # 10/24/26: an FTP server keeps SurfSim aside too (no logins, files written), once telnetd is off
      ('ftpd on', ['ftpd: serving / ']),
      ('telnetd off', ['telnetd: stopped']),
      ('wifi connect', ['wlan0: SurfSim is in range, but it is open and an FTP server (ftpd) runs: not joined by itself '
                        '(wifi join SurfSim joins it)']),
      ('ftpd off', ['ftpd: stopped']),
      ('keeper-joins-open', CHECK),
      ('wifi status', ['wlan0: associated with "SurfSim"', 'inet 10.0.2.15']),
      ('wifi saved', [' 1  SurfSimWPA', ' 2  SurfSim ', 'open  joined now']),
      ('wlansim on SurfSimWPA', ['wlansim: SurfSimWPA is on again']),
      ('umount /hda1', [])],
     [WLANSIM_PASSPHRASE, 'not mounted', 'stays mounted', 'umount: /hda1']),
    # 3: settings=off (the menu's entry without the settings): nothing read, nothing joined, no telnetd. A change is
    # saved for the next start and changes nothing now (set back afterwards); a network saved and forgotten so too
    ('@boot wlansim net settingsdisk=owner settings=off',
     ['+NET: settings=off: the settings on the stick are not read at this start', PROMPT],
     [('settings', ['settings: not read at this start (settings=off): the defaults until SurfOS restarts; a change is '
                    'saved on hda1 for the next start', 'telnetd      = off       (the default)', '0 saved Wi-Fi networks']),
      ('wifi saved', ['the saved Wi-Fi networks are not read at this start (settings=off)']),
      ('wifi status', ['wlan0: down (wifi up starts it)']),
      ('telnetd', ['telnetd: not running']),
      ('settings hostname offtest', ['hostname = offtest, saved on hda1 (surfos-config/settings.conf): from the next '
                                     'start (this one does not use the settings: settings=off)']),
      ('settings', ['hostname     = surfos    (the default)']),
      ('settings hostname smoketest', ['hostname = smoketest, saved on hda1']),
      ('wifi save SurfSimFlaky', [re.compile(rb'Password for SurfSimFlaky \(nothing for an open network\): ')]),
      ('', ['saved SurfSimFlaky (open, hidden) on hda1, the first of 3 saved networks it tries: SurfOS joins it by '
            'itself from the next start', 'the last scan did not hear SurfSimFlaky by its name']),
      ('wifi saved', ['the saved Wi-Fi networks are not read at this start (settings=off)']),
      ('wifi forget SurfSimFlaky', ['forgot SurfSimFlaky: SurfOS no longer joins it by itself (2 saved networks left)']),
      ('umount /hda1', [])],
     ['+NET: wlan0', '+NET: settings from', 'umount: /hda1']),
    # 4: a hidden network saved by hand (hidden = yes, no security line: open): joined by a probe for its name; no DHCP
    # server answers on its island, so the keeper says it asks again later. wifi save of it keeps it hidden (the station
    # heard its beacons without a name); with settings wifi off, a network the last scan did not hear by its name is
    # saved as hidden, and the save says SurfOS joins none by itself
    ('@boot wlansim net settingsdisk=hidden',
     ['+NET: settings from hda1 (surfos-config/): telnetd off, wifi on, ethernet off, hostname surfos; 1 saved Wi-Fi '
      'network', '+NET: wlan0: looking for the hidden network SurfSimHidden', 'wlan0: link up: "SurfSimHidden"',
      '+NET: wlan0: joined SurfSimHidden; asking for an address (DHCP)'],
     [('hidden-no-address', CHECK),
      ('wifi status', ['wlan0: associated with "SurfSimHidden"', 'inet 0.0.0.0']),
      ('wifi save', ['saved SurfSimHidden (open, hidden) on hda1 again: SurfOS joins it by itself at startup, its only '
                     'saved network']),
      ('settings wifi off', ['wifi = off, saved on hda1 (surfos-config/settings.conf): no saved network is joined by '
                             'itself, from now on']),
      ('wifi save NotHereNet', [re.compile(rb'Password for NotHereNet \(nothing for an open network\): ')]),
      ('', ['saved NotHereNet (open, hidden) on hda1, the first of 2 saved networks it tries; SurfOS joins none by '
            'itself while settings wifi is off', 'the last scan did not hear NotHereNet by its name']),
      ('wifi forget NotHereNet', ['forgot NotHereNet: SurfOS no longer joins it by itself (1 saved network left)']),
      ('cat /hda1/surfos-config/networks.conf', ['network = "SurfSimHidden"', 'security = open', 'hidden = yes'])],
     ['none of the', 'not in range']),
    # 5: ethernet = dhcp, and every lease shortened to 8 s (dhcplease=): eth0 leased at boot, renewed at half the time
    # by the server that granted it, again and again; its link down and up again: the address confirmed (INIT-REBOOT)
    ('@boot net settingsdisk=ether dhcplease=8',
     ['+NET: settings from hda1 (surfos-config/): telnetd off, wifi on, ethernet dhcp, hostname surfos; 0 saved Wi-Fi '
      'networks', '+NET: eth0: link up; asking for an address (DHCP)', 'dhcp: eth0: lease 10.0.2.15 for 86400 s',
      'dhcp: eth0: lease 10.0.2.15 renewed for 86400 s'],
     [('ifconfig', ['DHCP lease from 10.0.2.2: ', ' of 8 left']),
      ('renewed-again', CHECK),
      ('link-bounce', CHECK)],
     ['given up', 'no lease']),
    # what those boots left on the owner's stick, read here
    ('@settings-disk', []),
    # 10/25/26 (track A, A10): surfsh's startup file and history on the owner's stick (settingsdisk=history: history = on
    # and a surfshrc), twice. 1: surfshrc runs at the interactive start (its HOME for ~, a quoted ~ left), the lines
    # typed kept but the one that starts with a space, history -w, the file as written, /dev/config, $HOSTNAME
    ('@boot settingsdisk=history',
     ['+NET: settings from hda1 (surfos-config/): telnetd off, wifi on, ethernet off, hostname surfos, history on; 0 '
      'saved Wi-Fi networks', 'surfshrc ran: HOME /hda1'],
     [('echo ~ ~/notes "~"', ['/hda1 /hda1/notes ~']),
      ('echo history-marker-one', ['history-marker-one']),
      (' echo kept-out', ['kept-out']),
      ('history -w', ['in surfos-config/history on the stick']),
      ('grep -c marker-one /hda1/surfos-config/history; grep -c kept-out /hda1/surfos-config/history', ['1\r\n0\r\n']),
      # 10/25/26 (the kernel review of track A, 4): a file of history opened before a save (here by a background job,
      # which writes to it after the save) is not written back over it: its close is refused (EAGAIN), and the
      # save's lines stay
      ('echo marker-two', ['marker-two']),
      ("{ sleep 4000; echo stale-snapshot; } >> /dev/config/history & sleep 1500; history -w; wait",
       ['in surfos-config/history on the stick']),
      ("grep -c '^echo marker-two$' /hda1/surfos-config/history; grep -c '^stale-snapshot$' /hda1/surfos-config/history",
       ['\r\n1\r\n0\r\n']),
      ('ls /dev/config', ['history', 'hostname', 'surfshrc']),
      ('echo host $HOSTNAME', ['host surfos']),
      ('umount /hda1', [])],
     ['was not saved']),
    # 2: the same stick again: surfshrc again, the history recalled; history off: history -w says it is not kept, the
    # file stays
    ('@boot settingsdisk=history',
     ['surfshrc ran: HOME /hda1'],
     [("history | grep -c '^ *[0-9]*  echo history-marker-one'", ['\r\n1\r\n']),
      ('settings history off', ['history = off, saved on hda1']),
      ('history -w', ['history: not kept on the stick: the history setting is off']),
      ('cat /hda1/surfos-config/history', ['echo history-marker-one']),
      ('umount /hda1', [])],
     ['was not saved']),
    # 3: the same stick, started without the settings (settings=off): neither file is read (no surfshrc, ~ is /, no
    # history), and no surfsh waited for the stick before its first prompt (the kernel log says when one does)
    ('@boot settingsdisk=history settings=off',
     ['+NET: settings=off: the settings on the stick are not read at this start'],
     [('echo home ~', ['home /\r\n']),
      ("history | grep -c '^ *[0-9]*  echo history-marker-one'", ['\r\n0\r\n']),
      ("dmesg | grep -c 'for the settings to be read'", ['\r\n0\r\n']),
      ('umount /hda1', [])],
     ['surfshrc ran']),
    # 10/25/26 (the programs' review of track A, U5 and U8): a startup file with CR LF line ends runs as one with LF (HOME
    # without a CR), and its exit leaves the file and is said, the shell going on (no restart, no recovery console); a
    # surfsh started inside it runs the file again, and a Ctrl+C in its sleep skips the rest of the file
    ('@boot settingsdisk=rc',
     ['rc one: HOME /hda1.', 'rc two', 'exit leaves the startup file, as return does; the shell goes on', PROMPT],
     [('echo alive $HOME', ['alive /hda1']),
      ('RC_SLEEP=1 surfsh', [re.compile(rb'rc one: HOME /hda1\.')]),
      ('@sleep 0.5', []),
      (b'\x03', ['sleep cut short', 'the rest of the startup file was skipped (Ctrl+C)', PROMPT]),
      ('exit', []),
      ('umount /hda1', [])],
     ['rc not reached', 'surfsh exited', 'recovery console']),
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
    # 10/25/26 (track A, A9): ^Z and ^Y in edit (a run of typing is one change), and UTF-8 typed over the serial line
    # as a terminal sends it: one character, one column, and Backspace takes all of it
    ('run edit /hda1/UNDO.TXT', ['New file', '^S Save'],
     [(b'one two', ['Col 8']),
      (b'\x1a', ['Undone (0 more to undo, 1 to redo)', 'Col 1']),
      (b'\x19', ['Done again (1 more to undo, 0 to redo)', 'Col 8']),
      (b' caf\xc3\xa9', ['Col 13']),
      (b'\x7f', ['Col 12']),
      (b'\x13', ['Wrote 1 line']),
      (b'\x11', ['exit status 0'])]),
    ('cat /hda1/UNDO.TXT; rm /hda1/UNDO.TXT', ['one two caf\r\n']),
    # 10/6/26: surfsh, the SurfOS shell, started inside the one init runs on F1. Raw mode passes the bytes on as
    # they are: Tab, Up (ESC [ A), Ctrl-C. The prompt and its state, $?, Tab for a command (the kernel's too) and a
    # path (a second Tab lists), history, a line over the 1023-byte limit (in pieces the console's input queue
    # takes), a kernel command's status and its redirection, < refused for it, a builtin and a program redirected,
    # & reported at the next prompt, a killed program, help paged (the harness answers --More--), set, exit.
    # 10/25/26: the kernel's ls as kernel ls (the program ls comes first); < refused for a kernel command where PATH
    # has no program of its name (a subshell with PATH=/), with the hint that names the one in /initrd/bin
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
      ('kernel ls no-such-file', ['No such file or directory', shown('hda1 \u224b!> ')]),
      ('echo status $?', ['\r\nstatus 1\r\n', shown('hda1 \u224b> ')]),
      (b'lsbl\t\n', ['part mbr FAT16', shown('hda1 \u224b> ')]),
      ('lsblk > LSBLK.TXT; echo status $?', ['\r\nstatus 0\r\n', shown('hda1 \u224b> ')]),
      ('cat LSBLK.TXT', ['rd0', 'part mbr FAT16', shown('hda1 \u224b> ')]),
      ('lsblk < LSBLK.TXT', ['surfsh: lsblk is a kernel command: it reads only the console', shown('hda1 \u224b!> ')]),
      ('( PATH=/; ls < LSBLK.TXT )', ['use /initrd/bin/ls for that', shown('hda1 \u224b!> ')]),
      ('echo hello > /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/echo more >> /hda1/SURFSH.TXT', [shown('hda1 \u224b> ')]),
      ('/initrd/bin/cat < /hda1/SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('cat SURFSH.TXT', ['hello\r\nmore\r\n', shown('hda1 \u224b> ')]),
      ('/initrd/bin/count 1 200 > /dev/null &', ['] started', shown('hda1 \u224b> ')]),
      ('sleep 1000', ['slept 1000 ms', '] done', shown('hda1 \u224b> ')]),      # reported before the next prompt
      ('crash null', ['killed by "Page Fault', 'killed]', shown('hda1 \u224b!> ')]),
      ('echo status $?', ['\r\nstatus 137\r\n', shown('hda1 \u224b> ')]),
      ('help', ['surfsh 1.0, the SurfOS shell', "the kernel's commands", '--More--', 'lsblk', 'programs in /initrd/bin',
                'fstest', 'the names that match: * any characters', '(C)2004-2026 Brandon Burr.', shown('hda1 \u224b> ')]),
      ('set prompt.unicode off', [shown('hda1 ~> ')]),
      ('set prompt.unicode=on', [shown('hda1 \u224b> ')]),
      ('exit 7', ['exit status 7'])]),
    ('@surfsh-corner', []),                                 # surfsh draws only where its line is
    # 10/25/26 (track A, A5): the line editor, in a surfsh of its own. History expansion: !$ !! !str !-n, each line
    # shown as it runs; a line that starts with a space is not kept; ! left as typed where bash leaves it, and in
    # double quotes; a reference to nothing refused. Ctrl+R: the row under the line says what it looks for, Enter
    # runs the line found, Ctrl+R again an older one, Esc the line back. Ctrl+Left as a terminal sends it.
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo one two', ['\r\none two\r\n', shown('SURF \u224b> ')]),
      ('echo !$', ['\r\necho two\r\ntwo\r\n', shown('SURF \u224b> ')]),
      ('!!', ['\r\necho two\r\ntwo\r\n', shown('SURF \u224b> ')]),
      ('!echo o', ['\r\necho two o\r\ntwo o\r\n', shown('SURF \u224b> ')]),
      (' echo unkept', ['\r\nunkept\r\n', shown('SURF \u224b> ')]),
      ('!!', ['\r\necho two o\r\ntwo o\r\n', shown('SURF \u224b> ')]),
      ('!-3', ['\r\necho one two\r\none two\r\n', shown('SURF \u224b> ')]),
      ('echo "#!/x" \'!!\' ! != x!', ['\r\n#!/x !! ! != x!\r\n', shown('SURF \u224b> ')]),
      ('echo !nothing-like-it', ['surfsh: !nothing-like-it: no such line in the history', shown('SURF \u224b!> ')]),
      ('\x12one', ['search: one', '\r\none two\r\n', shown('SURF \u224b> ')]),
      ('\x12two\x12', ['search: two', '\r\ntwo o\r\n', shown('SURF \u224b> ')]),
      (b'echo kept\x12one\x1b', ['search: one']),
      (b'\n', ['\r\nkept\r\n', shown('SURF \u224b> ')]),
      ('echo world\x1b[1;5Dhello ', ['\r\nhello world\r\n', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 10/24/26 (pipelines and scripting in surfsh): pipelines and lists, in a surfsh of their own. A program into a
    # program; a kernel command and a builtin into one (a new surfsh runs them); five stages; the last stage in this
    # shell (its cd stays), another stage in a new surfsh (its cd does not); && || ! { } ( ); 2>&1 and 2> file; a
    # here-document into a pipeline, over lines after the continuation prompt; a line that goes on after |, and ^C
    # there; a broken pipe ends its writer; a kernel command after | refused; a mistake; a job in the background, a
    # pipeline, reported by its number. 10/25/26: the kernel's ls into a program as kernel ls (ls is the program)
    ('surfsh', [shown('SURF \u224b> ')],
     [('/initrd/bin/cat /initrd/etc/version | /initrd/bin/wc -c', ['     20 ', shown('SURF \u224b> ')]),
      ('kernel ls /initrd/bin | /initrd/bin/head -2', [re.compile(rb'  [a-z0-9]+\r\n[^\r\n]*  [a-z0-9]+\r\n'), shown('SURF \u224b> ')]),
      # 10/31/26: a line of its own at the log's end, not the boot's at its start: by here the steps before have filled
      # the 16 KB ring (KLOG_SIZE), and the boot's first lines are gone
      ("printf 'dmesg-%s\\n' piped > /dev/kmsg; dmesg | /initrd/bin/tail -n 1", ['dmesg-piped', shown('SURF \u224b> ')]),
      ('help | /initrd/bin/head -2', ['surfsh 1.0, the SurfOS shell', shown('SURF \u224b> ')]),
      ('echo one two | /initrd/bin/wc -w', ['      2 ', shown('SURF \u224b> ')]),
      ('echo abc | /initrd/bin/cat | /initrd/bin/cat | /initrd/bin/cat | /initrd/bin/wc -c', ['      4 ', shown('SURF \u224b> ')]),
      ('echo x | cd /initrd/bin', [shown('initrd \u25b8 bin \u224b> ')]),
      ('cd / | /initrd/bin/cat; pwd', ['\r\n/initrd/bin\r\n', shown('bin \u224b> ')]),
      ('cd /', [shown('SURF \u224b> ')]),
      ('echo a && echo b || echo c; echo end', ['\r\na\r\nb\r\nend\r\n', shown('SURF \u224b> ')]),
      ('ls /nonexistent && echo yes || echo no', ['No such file or directory', '\r\nno\r\n', shown('SURF \u224b> ')]),
      ('! ls /initrd > /dev/null; echo status $?', ['\r\nstatus 1\r\n', shown('SURF \u224b> ')]),
      ('{ echo one; echo two; } > /hda1/GROUP.TXT; cat /hda1/GROUP.TXT', ['one\r\ntwo\r\n', shown('SURF \u224b> ')]),
      ('( cd /initrd; pwd ); pwd', ['\r\n/initrd\r\n/\r\n', shown('SURF \u224b> ')]),
      ('/initrd/bin/ls /nonexistent 2>&1 | /initrd/bin/wc -l', ['      1 ', shown('SURF \u224b> ')]),
      ('/initrd/bin/ls /nonexistent 2> /hda1/ERR.TXT; cat /hda1/ERR.TXT',
       ['[exit status 1]', 'ls: /nonexistent: No such file or directory', shown('SURF \u224b> ')]),
      ('/initrd/bin/cat <<END | /initrd/bin/wc -l', [shown('\u2026 \u224b> ')]),
      ('one', [shown('\u2026 \u224b> ')]),
      ('two $?', [shown('\u2026 \u224b> ')]),
      ('END', ['      2 ', shown('SURF \u224b> ')]),
      ('echo three |', [shown('\u2026 \u224b> ')]),
      ('/initrd/bin/wc -c', ['      6 ', shown('SURF \u224b> ')]),
      ('echo unfinished |', [shown('\u2026 \u224b> ')]),
      (b'\x03', ['^C', shown('SURF \u224b> ')]),
      ('/initrd/bin/hexdump /dev/zero | /initrd/bin/head -1; echo status $?',
       ['00000000  00 00 00 00', '\r\nstatus 0\r\n', shown('SURF \u224b> ')]),
      ('echo x | lsblk', ['surfsh: lsblk is a kernel command: it reads only the console, not the pipe before it',
                          shown('SURF \u224b!> ')]),
      ('echo (', ['surfsh: unexpected (', shown('SURF \u224b!> ')]),
      ('/initrd/bin/count 2 50 | /initrd/bin/wc -w &', ['[1] started (pid ', shown('SURF \u224b> ')]),
      ('sleep 600', ['slept 600 ms', '[1] done  /initrd/bin/count 2 50 | /initrd/bin/wc -w', shown('SURF \u224b> ')]),
      ('rm /hda1/GROUP.TXT /hda1/ERR.TXT', [shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 10/24/26 (variables and expansions in surfsh), in a surfsh of their own: a variable, quoted and not; export into a
    # program's environment (env), x=1 cmd for cmd alone, env -i; unset, readonly; $( ) and ` `, a line of assignments
    # alone with the status of its last $( ); $(( )); ${ } forms; splitting; set --, $#, shift; a variable that is not
    # exported, in a stage that runs in a new surfsh; wildcards, quoted, matching nothing; ${x:?} and $((1 / 0))
    ('surfsh', [shown('SURF \u224b> ')],
     [('x=hello; echo $x ${x}! "$x" \'$x\'', ['\r\nhello hello! hello $x\r\n', shown('SURF \u224b> ')]),
      ('export GREETING="hi there"; env', ['\r\nGREETING=hi there\r\n', 'PATH=/initrd/bin\r\n', 'PWD=/\r\n',
                                            'SHELL=/initrd/bin/surfsh\r\n', shown('SURF \u224b> ')]),
      ('LANG=C env | /initrd/bin/head -1; echo ${LANG-unset}', ['\r\nLANG=C\r\nunset\r\n', shown('SURF \u224b> ')]),
      ('env -i ONLY=1 env', ['\r\nONLY=1\r\n', shown('SURF \u224b> ')]),
      ('unset x; echo "[${x-gone}]"', ['\r\n[gone]\r\n', shown('SURF \u224b> ')]),
      ('readonly r=1; r=2; echo $? $r', ['surfsh: r: read-only', '\r\n1 1\r\n', shown('SURF \u224b> ')]),
      ('v=$(cat /initrd/etc/version); echo "[$v]"', ['\r\n[SurfOS 1.0 (ai-dev)]\r\n', shown('SURF \u224b> ')]),
      ('echo "a`echo b`c" $(/initrd/bin/echo d)', ['\r\nabc d\r\n', shown('SURF \u224b> ')]),
      ('v=$(/initrd/bin/ls /nonexistent 2> /dev/null); echo status $?', ['\r\nstatus 1\r\n', shown('SURF \u224b> ')]),
      ('i=6; echo $((i * 7)) $((i += 1)) $i $((0x10 + 010))', ['\r\n42 7 7 24\r\n', shown('SURF \u224b> ')]),
      ('f=/hda1/notes.txt; echo ${f##*/} ${f%.txt} ${#f} ${u:-default}',
       ['\r\nnotes.txt /hda1/notes 15 default\r\n', shown('SURF \u224b> ')]),
      ('sp="a  b"; set -- $sp "$sp"; echo $# "$3"', ['\r\n3 a  b\r\n', shown('SURF \u224b> ')]),
      ('set -- one "two words" three; shift; echo $# "$1"', ['\r\n2 two words\r\n', shown('SURF \u224b> ')]),
      ('z=inside; { echo $z $1; } | /initrd/bin/cat', ['\r\ninside two words\r\n', shown('SURF \u224b> ')]),
      ('echo /initrd/etc/*.conf', ['\r\n/initrd/etc/surfsh.conf /initrd/etc/undertow.conf\r\n', shown('SURF \u224b> ')]),
      ('echo "/initrd/etc/*.conf" /initrd/etc/v?rsion /initrd/etc/none*',
       ['\r\n/initrd/etc/*.conf /initrd/etc/version /initrd/etc/none*\r\n', shown('SURF \u224b> ')]),
      ('echo ${nothing:?is not set}', ['surfsh: nothing: is not set', shown('SURF \u224b!> ')]),
      ('echo $((1 / 0))', ['division by 0', shown('SURF \u224b!> ')]),
      ('exit', [])]),
    # 10/25/26 (scripts in surfsh), in a surfsh of their own: if, elif and else; while with $(( )); for over words and
    # wildcards; case; a function with its arguments, local and return; one that calls itself; continue and break;
    # printf; read in the last stage of a pipeline, and a while read loop there; eval; a . file with arguments; kernel;
    # test and [, and a test that is no test; set -e, -u and -x in surfsh -c; a script over several lines (a here-
    # document), run by surfsh and by its name (#!), one without #! (run as a script), one whose #! names nothing;
    # exec with redirections alone and with a command; unset -f; $- and set -o; a function in a pipeline and in ( );
    # an if typed over lines; mistakes
    ('surfsh', [shown('SURF \u224b> ')],
     [('if false; then echo no; elif [ -d /initrd ]; then echo elif-yes; else echo no; fi',
       ['\r\nelif-yes\r\n', shown('SURF \u224b> ')]),
      ('i=0; while [ $i -lt 3 ]; do printf "i=%d " $i; i=$((i + 1)); done; echo', ['\r\ni=0 i=1 i=2 \r\n', shown('SURF \u224b> ')]),
      ('for x in a "b c" /initrd/etc/*.conf; do echo "<$x>"; done',
       ['\r\n<a>\r\n<b c>\r\n</initrd/etc/surfsh.conf>\r\n</initrd/etc/undertow.conf>\r\n', shown('SURF \u224b> ')]),
      ('for w in apple Bob 42 x.txt; do case $w in [a-z]*.txt) echo "$w: a file";; [A-Z]*) echo "$w: a name";; '
       '*[0-9]) echo "$w: a number";; *) echo "$w: a word";; esac; done',
       ['\r\napple: a word\r\nBob: a name\r\n42: a number\r\nx.txt: a file\r\n', shown('SURF \u224b> ')]),
      ('greet() { local who=${1:-nobody}; echo "hello $who ($#)"; return 3; }; greet world x; echo status $? ${who-unset}',
       ['\r\nhello world (2)\r\nstatus 3 unset\r\n', shown('SURF \u224b> ')]),
      ('fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }; fact 6',
       ['\r\n720\r\n', shown('SURF \u224b> ')]),
      ('for i in 1 2 3 4 5 6; do [ $i = 2 ] && continue; [ $i = 5 ] && break; printf "%s " $i; done; echo',
       ['\r\n1 3 4 \r\n', shown('SURF \u224b> ')]),
      ('printf "%-5s|%05d|%x|%c|%b\\n" ab 42 255 xyz "x\\0101y"', ['\r\nab   |00042|ff|x|xAy\r\n', shown('SURF \u224b> ')]),
      ('echo "one two three" | { read a b; echo "a=$a b=$b"; }', ['\r\na=one b=two three\r\n', shown('SURF \u224b> ')]),
      ('printf "x 1\\ny 2\\n" | while read k v; do echo "$k=$v"; done', ['\r\nx=1\r\ny=2\r\n', shown('SURF \u224b> ')]),
      ('eval "e1=10; e2=\\$((e1 * 2))"; echo $e2', ['\r\n20\r\n', shown('SURF \u224b> ')]),
      ('echo "echo dot \\$# \\$1; dotvar=yes" > /hda1/DOT.SH; . /hda1/DOT.SH first second; echo dotvar=$dotvar',
       ['\r\ndot 2 first\r\ndotvar=yes\r\n', shown('SURF \u224b> ')]),
      ('kernel echo the kernel says', ['\r\nthe kernel says\r\n', shown('SURF \u224b> ')]),
      ('[ -f /initrd/etc/version ] && test ! -d /initrd/etc/version && [ "a b" = "a b" ] && [ 10 -gt 9 ] && echo tests-pass',
       ['\r\ntests-pass\r\n', shown('SURF \u224b> ')]),
      ('[ 1 -eq x ]; echo status $?', ['surfsh: test: x: not a number', '\r\nstatus 2\r\n', shown('SURF \u224b> ')]),
      ('surfsh -c "set -e; echo before; false; echo not-reached"; echo status $?',
       ['before\r\n[exit status 1]\r\nstatus 1\r\n', shown('SURF \u224b> ')]),
      ('surfsh -c "set -u; echo \\$undefined; echo not-reached"', ['surfsh: undefined: not set', '[exit status 1]',
                                                                   shown('SURF \u224b!> ')]),
      ('surfsh -c "set -x; v=1; echo \\$v"', ['+ v=1', '+ echo 1', '\r\n1\r\n', shown('SURF \u224b> ')]),
      ("/initrd/bin/cat > /hda1/T.SH << 'EOF'", [shown('\u2026 \u224b> ')]),
      ('#!/initrd/bin/surfsh', [shown('\u2026 \u224b> ')]),
      ('total=0', [shown('\u2026 \u224b> ')]),
      ('for n in "$@"; do', [shown('\u2026 \u224b> ')]),
      ('  total=$((total + n))', [shown('\u2026 \u224b> ')]),
      ('done', [shown('\u2026 \u224b> ')]),
      ('echo "$0: $# numbers, total $total"', [shown('\u2026 \u224b> ')]),
      ('EOF', [shown('SURF \u224b> ')]),
      ('surfsh /hda1/T.SH 1 2 3', ['/hda1/T.SH: 3 numbers, total 6\r\n', shown('SURF \u224b> ')]),
      ('/hda1/T.SH 10 20', ['/hda1/T.SH: 2 numbers, total 30\r\n', shown('SURF \u224b> ')]),
      ('echo "echo no hashbang \\$1" > /hda1/P.SH; /hda1/P.SH ok', ['no hashbang ok\r\n', shown('SURF \u224b> ')]),
      ('echo "#!/bin/sh" > /hda1/B.SH; /hda1/B.SH; echo status $?',
       ['surfsh: /hda1/B.SH: its #! line names /bin/sh, which cannot run', '\r\nstatus 126\r\n', shown('SURF \u224b> ')]),
      ('exec 3> /hda1/FD3.TXT; echo via-three >&3; exec 3>&-; cat /hda1/FD3.TXT', ['\r\nvia-three\r\n', shown('SURF \u224b> ')]),
      ('surfsh -c "exec /initrd/bin/echo exec-ran; echo not-reached"; echo status $?',
       ['exec-ran\r\nstatus 0\r\n', shown('SURF \u224b> ')]),
      ('unset -f greet; greet; echo status $?', ['surfsh: greet: command not found', '\r\nstatus 127\r\n', shown('SURF \u224b> ')]),
      ('echo opts=$-; set -o | /initrd/bin/head -1', ['\r\nopts=i\r\n', 'errexit    off\r\n', shown('SURF \u224b> ')]),
      ('piped() { echo from-a-function; }; piped | /initrd/bin/wc -c; ( piped )',
       ['     16 ', 'from-a-function\r\n', shown('SURF \u224b> ')]),
      ('if true', [shown('\u2026 \u224b> ')]),
      ('then echo over-lines', [shown('\u2026 \u224b> ')]),
      ('fi', ['\r\nover-lines\r\n', shown('SURF \u224b> ')]),
      ('break; echo status $?', ['surfsh: break: not in a loop', '\r\nstatus 1\r\n', shown('SURF \u224b> ')]),
      ('if true; then fi', ['surfsh: a command is missing before fi', shown('SURF \u224b!> ')]),
      ('rm /hda1/DOT.SH /hda1/T.SH /hda1/P.SH /hda1/B.SH /hda1/FD3.TXT', [shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 10/25/26 (what a review of the scripting found, in a surfsh of its own): a script that redirects descriptor 3 reads
    # on (its own file is on one of surfsh's); a pipeline whose last command is not found is 127; $? in eval's text is
    # the one before it; eval and a subshell in a condition under set -e; a script's wait for a job that ended before;
    # exit alone is $?'s; { ...; } > file holds the command's output, not surfsh's report; printf's widest width; jobs
    # in a pipeline and in $( ); x=1 x=2 prog; exec of a typo at the prompt; here-documents of more than a pipe holds
    # into : and read, and into a stage whose output another stage reads; a surfsh reading the terminal into a file, a
    # line at a time; tail -n of a huge count
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo "exec 3< /initrd/etc/version; read v <&3; echo got-\\$v; exec 3<&-; echo after-close" > /hda1/E3.SH; '
       'surfsh /hda1/E3.SH', ['\r\ngot-SurfOS', 'after-close\r\n', shown('SURF \u224b> ')]),
      ('echo hi | nosuchcmd; echo status $?', ['nosuchcmd: command not found', '\r\nstatus 127\r\n', shown('SURF \u224b> ')]),
      ("false; eval 'echo was $?'", ['\r\nwas 1\r\n', shown('SURF \u224b> ')]),
      ('surfsh -c "set -e; eval false || echo handled; if (false; echo in-sub); then echo then-part; fi; echo end"',
       ['\r\nhandled\r\nin-sub\r\nthen-part\r\nend\r\n', shown('SURF \u224b> ')]),
      ('echo "sleep 100 & p=\\$!; sleep 400; wait \\$p; echo waited \\$?" > /hda1/W.SH; surfsh /hda1/W.SH',
       ['\r\nwaited 0\r\n', shown('SURF \u224b> ')]),
      ('surfsh -c "false; exit"; echo status $?', ['\r\nstatus 1\r\n', shown('SURF \u224b> ')]),
      ('{ /initrd/bin/grep nothing /initrd/etc/version; } > /hda1/G.TXT; /initrd/bin/wc -c < /hda1/G.TXT',
       ['[exit status 1]', '      0', shown('SURF \u224b> ')]),
      ("printf '%1500000000s' x; echo status $?",
       ['a width or a precision above 65536', '\r\nstatus 2\r\n', shown('SURF \u224b> ')]),
      ('sleep 60000 &', ['[1] started (pid ', shown('SURF \u224b> ')]),
      ('jobs | /initrd/bin/cat; n=$(jobs); echo "<${n%% *}>"', ['[1]  running  sleep 60000\r\n', '<[1]>\r\n',
                                                               shown('SURF \u224b> ')]),
      ('kill %1; sleep 300', ['[1] killed  sleep 60000', shown('SURF \u224b> ')]),
      ('x=1 x=2 /initrd/bin/env | /initrd/bin/grep "^x="', ['\r\nx=2\r\n', shown('SURF \u224b> ')]),
      ('x=1 x=2 /initrd/bin/env | /initrd/bin/grep -c "^x="', ['\r\n1\r\n', shown('SURF \u224b> ')]),
      ('exec nosuchcmd; echo alive', ['nosuchcmd: command not found', '\r\nalive\r\n', shown('SURF \u224b> ')]),
      ('{ echo ": <<END"; /initrd/bin/seq 3000; echo END; echo "read first <<END"; /initrd/bin/seq 3000; echo END; '
       'echo "echo first=\\$first"; } > /hda1/H.SH; surfsh /hda1/H.SH', ['\r\nfirst=1\r\n', shown('SURF \u224b> ')]),
      ('{ echo "/initrd/bin/tr 0-9 a-j <<END | /initrd/bin/tail -n 1"; /initrd/bin/seq 3000; echo END; } > /hda1/T2.SH; '
       'surfsh /hda1/T2.SH', ['\r\ndaaa\r\n', shown('SURF \u224b> ')]),
      ('echo ready; surfsh > /hda1/LOG.TXT', ['\r\nready\r\n']),
      ('echo hello-log', []),
      ('echo second-line', []),
      (b'\x04', [shown('SURF \u224b> ')]),
      ('/initrd/bin/cat /hda1/LOG.TXT', ['\r\nhello-log\r\nsecond-line\r\n', shown('SURF \u224b> ')]),
      ('/initrd/bin/seq 5 | /initrd/bin/tail -n 1073741825 | /initrd/bin/wc -l', ['      5', shown('SURF \u224b> ')]),
      ('rm /hda1/E3.SH /hda1/W.SH /hda1/G.TXT /hda1/H.SH /hda1/T2.SH /hda1/LOG.TXT', [shown('SURF \u224b> ')]),
      ('exit', [])]),
    ('@surfsh-ctrl-c', []),                                 # Ctrl+C in surfsh's loops, read, a surfsh in a surfsh
    ('@surfsh-jobs', []),                                   # 10/25/26: job control: Ctrl+Z, jobs, fg, bg, kill %1, Ctrl+D
    # 11/01/26 (the command-line suite, track archive): gzip, zcat and gunzip on FAT, the input going and coming back; a file
    # cut short and an output that exists, refused with nothing lost; tar czf with -C and --exclude, tz, xz, x over a file
    # that exists, and through a pipe; unzip of a zip written out here (a stored member, a deflated one, one named ../up),
    # by -l, -t, -p and unpacking, the bad name refused and the others taken; a tar with the same name likewise. The depth
    # is in tests/host/test_deflate.c, test_gztool.c, test_tar.c and test_unzip.c.
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-archive', ['cli-archive\r\n', shown('SURF \u224b> ')]),
      ('seq 1 3000 > /hda1/AR.TXT; sha256sum /hda1/AR.TXT | cut -d \' \' -f 1 > /hda1/AR.SUM; gzip -9 /hda1/AR.TXT; test -e /hda1/AR.TXT || echo input-gone',
       ['input-gone\r\n', shown('SURF \u224b> ')]),
      ('gzip -t /hda1/AR.TXT.gz; echo status $?; zcat /hda1/AR.TXT.gz | tail -n 1',
       ['status 0\r\n', '3000\r\n', shown('SURF \u224b> ')]),
      ('gunzip /hda1/AR.TXT.gz; test "$(cat /hda1/AR.SUM)" = "$(sha256sum /hda1/AR.TXT | cut -d \' \' -f 1)" && echo same-bytes; test -e /hda1/AR.TXT.gz || echo gz-gone',
       ['same-bytes\r\n', 'gz-gone\r\n', shown('SURF \u224b> ')]),
      ('printf \'\\037\\213\\010\\000\' > /hda1/CUT.GZ; gunzip /hda1/CUT.GZ; echo status $?; test -e /hda1/CUT || echo no-output; test -e /hda1/CUT.GZ && echo input-kept',
       ['the data ends too early', 'status 2\r\n', 'no-output\r\n', 'input-kept\r\n', shown('SURF \u224b> ')]),
      ('echo mine > /hda1/AR.TXT.gz; gzip /hda1/AR.TXT; echo status $?; cat /hda1/AR.TXT.gz; rm /hda1/AR.TXT.gz',
       ['it already exists', 'status 2\r\n', 'mine\r\n', shown('SURF \u224b> ')]),
      ('tar czf /hda1/ETC.TGZ --exclude=ssl -C /initrd etc; echo status $?; tar tzf /hda1/ETC.TGZ | grep -c ssl; tar tzf /hda1/ETC.TGZ | grep -c motd',
       ['status 0\r\n', '0\r\n', '1\r\n', shown('SURF \u224b> ')]),
      ('mkdir /hda1/AROUT; tar xzf /hda1/ETC.TGZ -C /hda1/AROUT; echo status $?; test "$(cat /hda1/AROUT/etc/motd.txt)" = "$(cat /initrd/etc/motd.txt)" && echo same-file',
       ['status 0\r\n', 'same-file\r\n', shown('SURF \u224b> ')]),
      ('echo old > /hda1/AROUT/etc/motd.txt; tar xzf /hda1/ETC.TGZ -C /hda1/AROUT; test "$(cat /hda1/AROUT/etc/motd.txt)" = "$(cat /initrd/etc/motd.txt)" && echo replaced; ls /hda1/AROUT/etc | grep -c tmp; true',
       ['replaced\r\n', '0\r\n', shown('SURF \u224b> ')]),
      ('tar cf - -C /initrd/etc ssl | tar tvf - | grep -c README',
       ['1\r\n', shown('SURF \u224b> ')]),
      ('printf \'PK\\003\\004\\024\\0\\0\\0\\0\\0\\0\\0a\\135\\027\\2268\\206\\007\\0\\0\\0\\007\\0\\0\\0\\002\\0\\0\\0okzip ok\\012PK\\003\\004\\024\\0\\0\\0\\010\\0\\0\\0a\\135\\346\\374\\043\\254\\017\\0\\0\\0\\265\\0\\0\\0\\001\\0\' > /hda1/EV.ZIP',
       [shown('SURF \u224b> ')]),
      ('printf \'\\0\\0dKIM\\313I\\054IMQH\\031:\\014.\\0PK\\003\\004\\024\\0\\0\\0\\0\\0\\0\\0a\\135z\\315\\077\\267\\005\\0\\0\\0\\005\\0\\0\\0\\005\\0\\0\\0../upevil\\012PK\\001\\002\\024\\0\\024\\0\\0\\0\\0\\0\\0\\0a\\135\\027\' >> /hda1/EV.ZIP',
       [shown('SURF \u224b> ')]),
      ('printf \'\\2268\\206\\007\\0\\0\\0\\007\\0\\0\\0\\002\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0okPK\\001\\002\\024\\0\\024\\0\\0\\0\\010\\0\\0\\0a\\135\\346\\374\\043\\254\\017\\0\\0\\0\\265\\0\\0\\0\\001\\0\\0\\0\\0\\0\\0\\0\' >> /hda1/EV.ZIP',
       [shown('SURF \u224b> ')]),
      ('printf \'\\0\\0\\0\\0\\0\\0\\047\\0\\0\\0dPK\\001\\002\\024\\0\\024\\0\\0\\0\\0\\0\\0\\0a\\135z\\315\\077\\267\\005\\0\\0\\0\\005\\0\\0\\0\\005\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0\\0U\\0\\0\\0../upPK\\005\\006\\0\\0\\0\\0\\003\\0\\003\' >> /hda1/EV.ZIP',
       [shown('SURF \u224b> ')]),
      ('printf \'\\0\\222\\0\\0\\0\\175\\0\\0\\0\\0\\0\' >> /hda1/EV.ZIP',
       [shown('SURF \u224b> ')]),
      ('unzip -l /hda1/EV.ZIP',
       ['  ok\r\n', '  d\r\n', '  ../up\r\n', '3 files', shown('SURF \u224b> ')]),
      ('unzip -tq /hda1/EV.ZIP; echo status $?; unzip -p /hda1/EV.ZIP d | wc -c',
       ['status 0\r\n', '181', shown('SURF \u224b> ')]),
      ('unzip -q /hda1/EV.ZIP -d /hda1/AROUT/Z; echo status $?; cat /hda1/AROUT/Z/ok; test -e /hda1/AROUT/up || echo not-above',
       ['the name leaves the folder', 'status 2\r\n', 'zip ok\r\n', 'not-above\r\n', shown('SURF \u224b> ')]),
      ('z() { i=0; while test $i -lt $1; do printf \'\\0\'; i=$((i+1)); done; }',
       [shown('SURF \u224b> ')]),
      ('printf \'../up\' > /hda1/EV.TAR; z 95 >> /hda1/EV.TAR; printf \'0000644\\0000000000\\0000000000\\00000000000005\\00014000000000\\000006527\\0 0\' >> /hda1/EV.TAR',
       [shown('SURF \u224b> ')]),
      ('z 100 >> /hda1/EV.TAR; printf \'ustar\\00000\' >> /hda1/EV.TAR; z 247 >> /hda1/EV.TAR; printf \'evil\\012\' >> /hda1/EV.TAR; z 507 >> /hda1/EV.TAR',
       [shown('SURF \u224b> ')]),
      ('printf \'ok\' >> /hda1/EV.TAR; z 98 >> /hda1/EV.TAR; printf \'0000644\\0000000000\\0000000000\\00000000000007\\00014000000000\\000006303\\0 0\' >> /hda1/EV.TAR',
       [shown('SURF \u224b> ')]),
      ('z 100 >> /hda1/EV.TAR; printf \'ustar\\00000\' >> /hda1/EV.TAR; z 247 >> /hda1/EV.TAR; printf \'tar ok\\012\' >> /hda1/EV.TAR; z 505 >> /hda1/EV.TAR',
       [shown('SURF \u224b> ')]),
      ('z 1024 >> /hda1/EV.TAR',
       [shown('SURF \u224b> ')]),
      ('rm /hda1/AROUT/Z/ok; tar xf /hda1/EV.TAR -C /hda1/AROUT/Z; echo status $?; cat /hda1/AROUT/Z/ok; test -e /hda1/AROUT/up || echo not-above',
       ['the name leaves the folder', 'status 2\r\n', 'tar ok\r\n', 'not-above\r\n', shown('SURF \u224b> ')]),
      ('rm /hda1/AR.TXT /hda1/AR.SUM /hda1/CUT.GZ /hda1/ETC.TGZ /hda1/EV.ZIP /hda1/EV.TAR; rm -r /hda1/AROUT; ls /hda1 | grep -c AR; true',
       ['0\r\n', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track sys): top's plain reports, each counted, the one that ends when its reader goes,
    # and a program's memory (memhog's, big enough to be no one else's) read back from the kernel; its live view, every order
    # and q, and on a pseudo-terminal; the manual: a page, a shared one, the names a file cannot have, a name that would lead out of it, man -k, a page
    # through less on the terminal; watch: the command with its title row, a kernel command with -t, a pipeline that never
    # ends, gone with all its processes on q; help's pointer to man. Depth in tests/host/test_top.c, test_watch.c, test_manutil.c
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-sys', ['cli-sys\r\n', shown('SURF \u224b> ')]),
      ('top -n 2 -d 0.2 | grep -c "PID  PPID"; top -d 0.1 | head -1; echo ended',
       ['\r\n2\r\n', 'top: up ', ' tasks, ', '% busy\r\n', 'ended\r\n', shown('SURF \u224b> ')]),
      ('top -n 1 -s pid | head -5 | tail -1', [re.compile(rb'\n    0     - [rR] \S+ +[0-9]+\.[0-9] +[0-9:]+ +- \[Kernel Idle Tas\]\r'),
                                               shown('SURF \u224b> ')]),
      ('memhog hold 1000 > /dev/null & top -n 3 -d 1 -s mem | grep memhog; wait',
       [re.compile(rb' [0-9]{5,} memhog\r\n'), shown('SURF \u224b> ')]),
      ('top -x; echo status $?; top -d 0; echo status $?; top -n 0 | cat; top -s name; echo status $?',
       ['usage: top [', 'status 2', 'top: 0: no time to wait', 'status 2', 'top: 0: no count of reports',
        'top: name: no order', 'status 2', shown('SURF \u224b> ')]),
      ('top -d 0.3', ['\x1b[?1049h', 'top: up ', 'Memory: ', '%CPU', 'by CPU', 'q: end']),
      (b'm', ['by memory']),
      (b'p', ['by pid']),
      (b'c', ['by CPU']),
      (b'q', ['\x1b[?1049l', shown('SURF \u224b> ')]),
      ('top -n 1 -d 0.1', ['\x1b[?1049h', 'q: end', '\x1b[?1049l', shown('SURF \u224b> ')]),
      ('ptytest run top -n 1 -d 0.1', ['\\e[?1049h', 'q: end', '\\e[?1049l', 'PTYTEST run PASS', shown('SURF \u224b> ')]),
      ('man ls | head -1; man crashdiv | head -1; man . | head -1; man "[" | head -1; man : | head -1',
       ['ls - list directories\r\n', 'crash - misbehave on purpose\r\n', 'source - run a file', 'test - check files',
        'true - do nothing', shown('SURF \u224b> ')]),
      ('man nosuch; echo status $?; man ../etc/motd; echo status $?', ['man: nosuch: no such page', 'status 1',
                                                                       'man: ../etc/motd: no such page', 'status 1',
                                                                       shown('SURF \u224b> ')]),
      ('man -k DISKS; man -k nosuchword; echo status $?; man; echo status $?',
       ['lsblk - disks, partitions and ramdisks\r\n', 'status 1', 'usage: man name', 'status 2', shown('SURF \u224b> ')]),
      ('man top', ['/initrd/man/top.txt  lines 1-']),
      (b'q', ['\x1b[?1049l', shown('SURF \u224b> ')]),
      ('watch -n 0.2 echo tick; echo status $?', ['\x1b[?1049h', 'Every 0.2s: echo tick', 'tick']),
      (b'q', ['\x1b[?1049l', 'status 0', shown('SURF \u224b> ')]),
      ('watch -t -n 0.2 uptime', ['\x1b[?1049h', ' tasks, ']),
      (b'\x03', ['\x1b[?1049l', shown('SURF \u224b> ')]),
      ("watch 'seq 1 3; cat /dev/zero | grep x'", ['\x1b[?1049h', 'Every 2s: seq 1 3;', '3']),
      (b'q', ['\x1b[?1049l', shown('SURF \u224b> ')]),
      ('sleep 300; ps | grep -c " cat"; true', ['\r\n0\r\n', shown('SURF \u224b> ')]),
      # (the review of 11/01/26) what a run left in the background goes at the end of that run, and of every run: the two
      # titles after 'started' are redraws, so a run has ended before q; without JOB_KILL there each leaves a count
      ("watch -n 0.2 'count 100000 1000 > /dev/null & echo started'", ['\x1b[?1049h', 'started', 'Every 0.2s', 'Every 0.2s']),
      (b'q', ['\x1b[?1049l', shown('SURF \u224b> ')]),
      ('sleep 300; ps | grep -c " count"; true', ['\r\n0\r\n', shown('SURF \u224b> ')]),
      ('watch date | cat; watch; echo status $?', ['watch: standard output is no terminal', 'usage: watch [', 'status 2',
                                                   shown('SURF \u224b> ')]),
      ('ptytest procsize', ['size 0 wrote 36 bytes', 'PTYTEST procsize PASS', shown('SURF \u224b> ')]),
      ('help | grep -c "man -k WORD"', ['\r\n1\r\n', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track lang): bc: numbers of any length, scale, the bases, a long number's lines; -l;
    # a script in a file, and a file then standard input; a mistake at a terminal costs its line, and in a pipe the status
    # is 2; a number the machine cannot hold is said at once; a file that is not there; a reader that goes away
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-lang', ['cli-lang\r\n', shown('SURF \u224b> ')]),
      ("echo '2^100; 2^500' | bc", ['\r\n1267650600228229401496703205376\r\n',
                                    '32733906078961418700131896968275991522166420460430647894832913680961\\\r\n',
                                    '053328527589376\r\n', shown('SURF \u224b> ')]),
      ("echo 'scale=20; 1/7; scale=2; 7.555*2; -7%3' | bc", ['.14285714285714285714\r\n', '15.110\r\n', '-.01\r\n',
                                                             shown('SURF \u224b> ')]),
      ("echo 'obase=16; 255; obase=2; 5.25; obase=25; 1024' | bc", ['FF\r\n', '101.0100000\r\n', ' 01 15 24\r\n',
                                                                    shown('SURF \u224b> ')]),
      ("echo 'ibase=16; FF.8; ibase=A; sqrt(10^40-1); length(1.50)' | bc",
       ['255.5\r\n', '99999999999999999999\r\n', '3\r\n', shown('SURF \u224b> ')]),
      ("echo 'e(1); 4*a(1); l(10); s(1); c(1); j(1,1)' | bc -l",
       ['2.71828182845904523536\r\n', '3.14159265358979323844\r\n', '2.30258509299404568401\r\n',
        '.84147098480789650665\r\n', '.54030230586813971740\r\n', '.44005058574493351595\r\n',
        shown('SURF \u224b> ')]),
      ("printf 'define f(n) { auto a[]; a[0]=n; if(n<2) return 1; return a[0]*f(n-1) }\\nf(25)\\nquit\\n' > /hda1/F.BC; "
       "bc /hda1/F.BC", ['15511210043330985984000000\r\n', shown('SURF \u224b> ')]),
      ("echo 'x=41' > /hda1/F.BC; echo 'x+1' | bc /hda1/F.BC", ['42\r\n', shown('SURF \u224b> ')]),
      # (bc says nothing when it starts: the file's answer says that it has got to the terminal)
      ("echo 'x+1' > /hda1/F.BC; bc /hda1/F.BC", ['\r\n1\r\n']),
      ('x=6', []),
      ('x*7', ['42\r\n']),
      ('1/0', ['bc: (standard input): line 3: divide by zero\r\n']),
      ('x', ['6\r\n']),
      ('quit', [shown('SURF \u224b> ')]),
      ('rm /hda1/F.BC', [shown('SURF \u224b> ')]),
      ("echo '1/0' | bc; echo status $?; echo '1+' | bc; echo status $?",
       ['divide by zero\r\n', 'status 2\r\n', 'unexpected end of line\r\n', 'status 2\r\n', shown('SURF \u224b> ')]),
      # (a digit above the base counts at its own value, and else may stand after a ; or on a later line)
      ("printf 'ibase=2\\n12\\nibase=1010\\n1E5\\nif(0)2;else 3\\nif(0)4\\nelse 5\\n' | bc; echo status $?",
       ['\r\n4\r\n245\r\n3\r\n5\r\nstatus 0\r\n', shown('SURF \u224b> ')]),
      ("echo '10^1000000000' | bc; echo status $?", ['out of memory\r\n', 'status 2\r\n', shown('SURF \u224b> ')]),
      ('bc /hda1/NONE.BC; echo status $?', ['bc: /hda1/NONE.BC: No such file or directory\r\n', 'status 2\r\n',
                                            shown('SURF \u224b> ')]),
      ("echo 'while(1) 7' | bc | head -2; echo done", ['7\r\n7\r\ndone\r\n', shown('SURF \u224b> ')]),
      ('head -1 /initrd/man/bc.txt', ['bc - ', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track lang): awk: fields, patterns, -F and -v, UTF-8 counted as characters, printf,
    # recursion, arrays and for-in through a pipe to sort, sub and match, getline from a command, system, a file written,
    # added to and read back, exit, a mistake and a file that is not there, memory running out, a reader that goes away
    ('surfsh', [shown('SURF ≋> ')],
     [('echo cli-lang', ['cli-lang\r\n', shown('SURF ≋> ')]),
      ("printf 'a b c\\nd e f\\n' | awk '{ print NF, $2; $2 = \"x\"; print }'",
       ['\r\n3 b\r\n', 'a x c\r\n', '3 e\r\n', 'd x f\r\n', shown('SURF ≋> ')]),
      ("seq 10 | awk '$1 % 2 == 0 { s += $1 } NR == 2, NR == 3 { n++ } END { print s, NR, n }'", ['30 10 2\r\n', shown('SURF ≋> ')]),
      ("printf 'x:1\\ny:2\\n' | awk -F: -v n=10 '{ print $1, $2 * n }'", ['x 10\r\n', 'y 20\r\n', shown('SURF ≋> ')]),
      ("echo héllo wörld | awk '{ print length($1), substr($2, 2, 2), index($0, \"o\"), toupper($1) }'",
       ['5 ör 5 HéLLO\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { printf \"%5.2f|%-4d|%s|%c|%x\\n\", 3.14159, 42, \"s\", 65, 255; print 2 ^ 10, 1 / 3, int(3.9), 1e6 }'",
       [' 3.14|42  |s|A|ff\r\n', '1024 0.333333 3 1000000\r\n', shown('SURF ≋> ')]),
      # (libsurf's printf would put a sign on what has none, which a host's printf does not: only here does it show)
      ("awk 'BEGIN { printf \"%+x|% o|%+u|% x|%+d\\n\", 255, 8, 3, 15, 5 }'", ['ff|10|3|f|+5\r\n', shown('SURF ≋> ')]),
      ("awk 'function f(n) { return n < 2 ? 1 : n * f(n - 1) } BEGIN { print f(10), sqrt(16), exp(0), length(\"\") }'",
       ['3628800 4 1 0\r\n', shown('SURF ≋> ')]),
      ("printf 'a\\nb\\na\\n' | awk '{ c[$1]++ } END { for (k in c) print k, c[k] | \"sort\" }'",
       ['a 2\r\n', 'b 1\r\n', shown('SURF ≋> ')]),
      ("echo hello | awk '{ n = gsub(/l+/, \"[&]\"); print n, $0, match($0, /o/), RSTART, RLENGTH }'",
       ['1 he[ll]o 7 7 1\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { while ((\"seq 3\" | getline x) > 0) s += x; print s, close(\"seq 3\"), system(\"exit 7\") }'",
       ['6 0 7\r\n', shown('SURF ≋> ')]),
      # (what awk has read of a command, a file or a pipe survives a system() and an fflush(), which flush stdio's streams)
      ("awk 'BEGIN { c = \"seq 3000\"; while ((c | getline x) > 0) { n++; s += x; if (n == 10) system(\"true\"); "
       "if (n == 20) fflush() } print n, s, close(c), close(c) }'", ['3000 4501500 0 -1\r\n', shown('SURF \u224b> ')]),
      ("seq 3000 > /hda1/S.TXT; awk 'BEGIN { f = \"/hda1/S.TXT\"; while ((getline x < f) > 0) { n++; if (n == 10) "
       "system(\"true\") } print n, close(f) }'; rm /hda1/S.TXT", ['3000 0\r\n', shown('SURF \u224b> ')]),
      ("seq 3000 | awk 'NR == 10 { system(\"true\") } END { print NR }'", ['\r\n3000\r\n', shown('SURF \u224b> ')]),
      ("awk 'BEGIN { f = \"/hda1/A.TXT\"; print \"one\" > f; close(f); print \"two\" >> f }'; awk '{ print NR, $0 }' /hda1/A.TXT; rm /hda1/A.TXT",
       ['1 one\r\n', '2 two\r\n', shown('SURF ≋> ')]),
      ("dmesg | awk '/FAT16: hda1/ { print $2, (NF > 3) }' | head -1", ['hda1: 1\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { exit 5 }'; echo status $?; awk 'BEGIN { print 1 / 0 }'; echo status $?",
       ['status 5\r\n', 'division by zero\r\n', 'status 2\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { print ) }'; echo status $?; awk '{ print }' /hda1/NONE.TXT; echo status $?",
       ["syntax error near ')'\r\n", 'status 2\r\n', 'awk: /hda1/NONE.TXT: No such file or directory\r\n', 'status 2\r\n',
        shown('SURF ≋> ')]),
      ("awk 'BEGIN { s = \"x\"; while (1) s = s s }'; echo status $?", ['out of memory\r\n', 'status 2\r\n', shown('SURF ≋> ')]),
      # (a write that fails when the file is closed, as on a full stick: this file opens, and refuses every write)
      ("awk 'BEGIN { print \"x\" >> \"/dev/config/hostname\" }'; echo status $?",
       ['awk: /dev/config/hostname: Read-only file system\r\n', 'status 2\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { f = \"/dev/config/hostname\"; print \"x\" >> f; close(f); print \"no\" }'; echo status $?",
       ['awk: line 1: /dev/config/hostname: Read-only file system\r\n', 'status 2\r\n', shown('SURF ≋> ')]),
      # (a mistake waits for a command, as the end of a program does: what it writes comes before the prompt)
      ("awk 'BEGIN { print \"sent\" | \"cat\"; print 1 / 0 }'; echo status $?", ['division by zero\r\n', 'sent\r\n', 'status 2\r\n', shown('SURF ≋> ')]),
      # (at a terminal a pattern as RS gives a record as soon as the next byte is there: the person does not type 64 KB)
      ("awk 'BEGIN { RS = \"[0-9]\"; print \"ready\" } { print \"<\" $0 \">\" } NR == 2 { exit }'", ['ready\r\n']),
      ('a1b2c', ['<a>\r\n', '<b>\r\n', shown('SURF ≋> ')]),
      # (and when RS can match the newline, the line's last record comes with the line, not with the next one)
      ("awk 'BEGIN { RS = \"[0-9\\n]\"; print \"ready\" } { print \"<\" $0 \">\" } NR == 2 { exit }'", ['ready\r\n']),
      ('a1b', ['<a>\r\n', '<b>\r\n', shown('SURF ≋> ')]),
      ("awk 'BEGIN { while (1) print 7 }' | head -2; echo done", ['7\r\n7\r\ndone\r\n', shown('SURF ≋> ')]),
      ('head -1 /initrd/man/awk.txt', ['awk - ', shown('SURF ≋> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track text): what libsurf gained, inside SurfOS: libtest (user/bin/libtest.c) checks
    # regcomp and regexec, strtod, sscanf's floats, getopt, system, popen and pclose, and that the mathematical functions
    # give the same bits here as on the host (user/bin/libm_grid.h), at -O0 and at -O2, on x87
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-text', ['cli-text\r\n', shown('SURF \u224b> ')]),
      ('libtest regex', ['LIBTEST regex PASS', shown('SURF \u224b> ')]),
      ('libtest numbers', ['LIBTEST numbers PASS', shown('SURF \u224b> ')]),
      ('libtest math', ['LIBTEST math PASS', shown('SURF \u224b> ')]),
      ('libtest getopt', ['LIBTEST getopt PASS', shown('SURF \u224b> ')]),
      ('libtest system', ['seen by a child', 'LIBTEST system PASS', shown('SURF \u224b> ')]),
      ('libtest popen', ['LIBTEST popen PASS', shown('SURF \u224b> ')]),
      ('libtest closed', ['LIBTEST closed PASS', shown('SURF \u224b> ')]),
      ('libtest order | cat', ['first\r\nsecond\r\nthird\r\n', shown('SURF \u224b> ')]),
      # stage 2: grep on the library, with -E, and sed: addresses, s with groups, a file replaced through saveutil.h with its
      # backup and nothing left beside it, w, what q says, a mistake, a reader that goes, a whole UTF-8 character
      ('printf "apple\\nBanana\\ncherry\\n" | grep -E "an|rr"', ['Banana\r\ncherry\r\n', shown('SURF \u224b> ')]),
      ('printf "aa\\nab\\n" | grep "\\(a\\)\\1"; printf "x\\n" | grep -E "a("; echo status $?',
       ['aa\r\n', 'grep: a(: ', 'status 2', shown('SURF \u224b> ')]),
      ('seq 5 | sed -n "2,3p;\\$="', ['2\r\n3\r\n5\r\n', shown('SURF \u224b> ')]),
      ("echo hello world | sed 's/\\(hello\\) \\(world\\)/\\2 \\1/'; echo aaa | sed -E 's/a+/<&>/'",
       ['world hello\r\n', '<aaa>\r\n', shown('SURF \u224b> ')]),
      ("printf 'a\\nb\\n' > /hda1/SED.TXT; sed -i.BAK 's/a/X/' /hda1/SED.TXT; echo status $?; cat /hda1/SED.TXT /hda1/SED.TXT.BAK; "
       "ls /hda1 | grep -c -i 'SED.TXT.TMP'; rm /hda1/SED.TXT /hda1/SED.TXT.BAK",
       ['status 0\r\n', 'X\r\nb\r\na\r\nb\r\n0\r\n', shown('SURF \u224b> ')]),
      ("seq 3 | sed -n '2w /hda1/SEDW.TXT'; cat /hda1/SEDW.TXT; rm /hda1/SEDW.TXT", ['\r\n2\r\n', shown('SURF \u224b> ')]),
      ("seq 3 | sed -n 'w /dev/null'; echo status $?", ['status 0\r\n', shown('SURF \u224b> ')]),
      ("seq 9 | sed 2q7; echo status $?; echo x | sed k; echo status $?", ['1\r\n2\r\n', 'status 7\r\n', 'sed: script line 1: ', 'status 2',
                                                                          shown('SURF \u224b> ')]),
      # names on FAT: case and paths differ, the file is one. w's two names are one output (a new file: its folder and name; the root
      # folder's number is 0); with a suffix one file twice is refused; so is a suffix that makes the working name. The file stays
      ("cd /hda1; printf 'a\\nb\\n' > SEDC.TXT; sed -n -e '1w /hda1/SEDO.TXT' -e '2w ./sedo.txt' SEDC.TXT; cat SEDO.TXT",
       ['a\r\nb\r\n', shown('hda1 \u224b> ')]),
      ("sed -i.BAK 's/a/X/' SEDC.TXT ./sedc.txt; echo status $?; sed -i.TMP 's/a/XX/' SEDC.TXT; echo status $?",
       ['are one file', 'status 2\r\n', 'in the way', 'status 2\r\n', shown('hda1 \u224b> ')]),
      ("cat SEDC.TXT; rm SEDC.TXT SEDO.TXT; ls | grep -c -i '^SED'; cd /", ['a\r\nb\r\n', '0\r\n', shown('SURF \u224b> ')]),
      # D, through libsurf's malloc: the pattern space is freed from where it began
      ("seq 3000 | sed -n ':a;$!{N;ba};P;D' | tail -1", ['\r\n3000\r\n', shown('SURF \u224b> ')]),
      ('seq 200000 | sed p | head -1', ['\r\n1\r\n', shown('SURF \u224b> ')]),
      ("echo h\u00e9llo | sed 's/./x/2;y/l/L/'", ['hxLLo\r\n', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track diff): on the FAT disk, where saving goes through saveutil.h's rename: diff in its
    # forms and its status, patch through it and back with -R, a file made and one refused, a .rej, cmp, then one line each of
    # the small tools. tests/host/test_diff.c and tests/test_cli_diff.py hold the depth; only here is the file system real
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-diff', ['cli-diff\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; printf "a\\nb\\nc\\n" > D1.TXT; printf "a\\nB\\nc\\nd\\n" > D2.TXT; diff D1.TXT D2.TXT; echo status $?; cd /',
       ['2c2\r\n< b\r\n---\r\n> B\r\n3a4\r\n> d\r\n', 'status 1\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; diff -u D1.TXT D2.TXT > D.DIF; head -4 D.DIF | tail -2; patch D1.TXT D.DIF; cmp D1.TXT D2.TXT; echo same $?; cd /',
       ['@@ -1,3 +1,4 @@\r\n a\r\n', 'patching file D1.TXT\r\n', 'same 0\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; patch -R D1.TXT D.DIF; cmp D1.TXT D2.TXT; echo differ $?; ls D1.TXT.tmp; cd /', ['patching file D1.TXT\r\n',
       'D1.TXT D2.TXT differ: byte 3, line 2\r\n', 'differ 1\r\n', 'No such file', shown('SURF \u224b> ')]),
      ('cd /hda1; diff -c D1.TXT D2.TXT | tail -5; diff -q D1.TXT D1.TXT; echo status $?; cd /',
       ['--- 1,4 ----\r\n  a\r\n! B\r\n  c\r\n+ d\r\n', 'status 0\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; diff -u /dev/null D2.TXT > N.DIF; rm D2.TXT; patch -p0 -i N.DIF; cat D2.TXT; echo status $?; cd /',
       ['patching file D2.TXT\r\n', 'a\r\nB\r\nc\r\nd\r\n', 'status 0\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; printf "x\\n" > D3.TXT; patch D3.TXT D.DIF; echo status $?; head -3 D3.TXT.rej | tail -1; cd /',
       ['1 out of 1 hunk FAILED -- saving rejects to file D3.TXT.rej\r\n', 'status 1\r\n', '@@ -1,3 +1,4 @@\r\n', shown('SURF \u224b> ')]),
      ('printf "%s\\n" "--- /x/y" "+++ /x/y" "@@ -1 +1 @@" "-a" "+b" | patch -p0; echo status $?', ['refused', 'status 2\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; seq 5000 > S1.TXT; seq 5000 | grep -v 777 > S2.TXT; diff S1.TXT S2.TXT | grep -c d; rm S1.TXT S2.TXT D.DIF N.DIF D1.TXT D2.TXT D3.TXT D3.TXT.rej; cd /',
       ['5\r\n', shown('SURF \u224b> ')]),
      # the review: FAT will not replace a file with a descriptor open, so a patch naming its own file fails and leaves it; a
      # failed -o leaves the user's file; an offset keeps the old file in .orig
      ('cd /hda1; printf "%s\\n" "--- SELF.DIF" "+++ SELF.DIF" "@@ -1 +1 @@" "---- SELF.DIF" "+--- CHANGED" > SELF.DIF; patch -p0 -i SELF.DIF; echo status $?; head -1 SELF.DIF; ls SELF.DIF.tmp; rm SELF.DIF; cd /',
       ['busy', 'status 2\r\n', '--- SELF.DIF\r\n', 'No such file', shown('SURF \u224b> ')]),
      ('cd /hda1; printf "%s\\n" "--- G.TXT" "+++ G.TXT" "@@ -1 +1 @@" "-a" "+b" > G.DIF; printf "x\\n" > K.TXT; printf "KEEP\\n" > O.TXT; patch -o O.TXT K.TXT G.DIF; echo status $?; cat O.TXT; cd /',
       ['not written', 'status 1\r\n', 'KEEP\r\n', shown('SURF \u224b> ')]),
      ('cd /hda1; printf "top\\na\\n" > M.TXT; patch M.TXT G.DIF; echo status $?; tail -1 M.TXT; tail -1 M.TXT.orig; rm G.DIF K.TXT K.TXT.rej O.TXT M.TXT M.TXT.orig; cd /',
       ['Hunk #1 succeeded at 2 (offset 1 line).\r\n', 'status 0\r\n', 'b\r\na\r\n', shown('SURF \u224b> ')]),
      ('printf "a\\nb\\n" > /hda1/C1.TXT; printf "b\\nc\\n" > /hda1/C2.TXT; comm /hda1/C1.TXT /hda1/C2.TXT; comm -12 /hda1/C1.TXT /hda1/C2.TXT',
       ['a\r\n\t\tb\r\n\tc\r\nb\r\n', shown('SURF \u224b> ')]),
      ('printf "1 x\\n2 y\\n" > /hda1/J1.TXT; printf "1 p\\n3 q\\n" > /hda1/J2.TXT; join /hda1/J1.TXT /hda1/J2.TXT; join -v 2 /hda1/J1.TXT /hda1/J2.TXT',
       ['1 x p\r\n3 q\r\n', shown('SURF \u224b> ')]),
      ('rm /hda1/C1.TXT /hda1/C2.TXT /hda1/J1.TXT /hda1/J2.TXT; seq 4 | paste - -; seq 3 | paste -s -d ,', ['1\t2\r\n3\t4\r\n1,2,3\r\n', shown('SURF \u224b> ')]),
      ('echo abcdefgh | fold -w 3; printf "a\\n\\nb\\n" | nl; echo stressed | rev',
       ['abc\r\ndef\r\ngh\r\n', '     1\ta\r\n       \r\n     2\tb\r\n', 'desserts\r\n', shown('SURF \u224b> ')]),
      ('basename /hda1/www/index.html .html; dirname /hda1/www/index.html', ['index\r\n/hda1/www\r\n', shown('SURF \u224b> ')]),
      ('echo hi | od -c; echo hi | od -A x -t x1; echo hello | base64; echo aGVsbG8K | base64 -d',
       ['0000000   h   i  \\n\r\n0000003\r\n', '000000 68 69 0a\r\n000003\r\n', 'aGVsbG8K\r\nhello\r\n', shown('SURF \u224b> ')]),
      ('printf "\\001\\002hello world\\003ab\\004" | strings; printf "\\001abc\\002" | strings -n 3', ['hello world\r\nabc\r\n', shown('SURF \u224b> ')]),
      ('file /initrd/bin/hello /initrd/motd /initrd/etc', ['/initrd/bin/hello: ELF 32-bit LSB executable, Intel 80386\r\n',
       '/initrd/motd: ', ' text', '/initrd/etc: directory\r\n', shown('SURF \u224b> ')]),
      ('tree -d /initrd/etc | tail -3; cal 11 2026; cal | head -2 | tail -1', ['ssl\r\n\r\n1 directory\r\n', '   November 2026\r\nSu Mo Tu We Th Fr Sa\r\n 1  2  3',
       '29 30\r\nSu Mo Tu We Th Fr Sa\r\n', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track fsck): the kernel command fsck, as far as a guest can go: it has no way to
    # damage a volume (tools/qemu-usb.py's fsck damages one on the host). Its refusals, each with its status, -r among
    # them (11/02/26: it only reads); a fresh volume clean, with files on it, read-only mounted. rd1 ends as it began
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-fsck', ['cli-fsck\r\n', shown('SURF \u224b> ')]),
      ('fsck; echo status $?', ['usage: fsck', 'status 2', shown('SURF \u224b> ')]),
      ('fsck nosuch; echo status $?', ["fsck: no block device 'nosuch'", 'status 2', shown('SURF \u224b> ')]),
      ('fsck hda1; echo status $?', ['hda1 is mounted read-write on /hda1', 'status 2', shown('SURF \u224b> ')]),
      ('fsck -r hda1; echo status $?', ['repairs are not available in this version', 'status 2', shown('SURF \u224b> ')]),
      ('mkfs rd1 FSCKTEST', ['Type rd1 to erase rd1']),
      ('rd1', ['rd1: FAT16', shown('SURF \u224b> ')]),
      ('fsck rd1; echo status $?', ['FAT16: ', 'clean', 'status 0', shown('SURF \u224b> ')]),
      ('mount rd1 /fsk', ['rd1 on /fsk: fat16, label FSCKTEST', shown('SURF \u224b> ')]),
      ('mkdir /fsk/dir; echo one > /fsk/dir/a-long-file-name.txt; seq 1 500 > /fsk/NUMBERS.TXT', [shown('SURF \u224b> ')]),
      ('fsck rd1; echo status $?', ['rd1 is mounted read-write on /fsk', 'status 2', shown('SURF \u224b> ')]),
      ('umount rd1', [shown('SURF \u224b> ')]),
      ('fsck -v rd1; echo status $?', ['2 directories, 2 files', 'clean', 'status 0', shown('SURF \u224b> ')]),
      ('mount -r rd1 /fsk', ['read-only', shown('SURF \u224b> ')]),
      ('fsck rd1; echo status $?', ['clean', 'status 0', shown('SURF \u224b> ')]),
      ('fsck -r rd1; echo status $?', ['repairs are not available in this version', 'status 2', shown('SURF \u224b> ')]),
      ('umount rd1', [shown('SURF \u224b> ')]),
      ('fsck -rv rd1; echo status $?', ['repairs are not available in this version, and the stick can be repaired on another computer', 'status 2', shown('SURF \u224b> ')]),
      ('fdisk rd1 new mbr', [CONFIRM_RD1]),
      ('rd1', ['verified', shown('SURF \u224b> ')]),
      ('fsck rd1; echo status $?', ['no FAT file system', 'status 2', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 11/01/26 (the command-line suite, track net): the clients, user/telnet/ and user/ftp/. The depth is in test_telnetc,
    # test_ftpc and test_ftpcs; here the real kernel, sockets and terminal. The refusals. telnet by the hairpin to SurfOS's
    # own telnetd (10.0.2.2 and the forwarded port): keys, the window as the session sees it, Ctrl+]'s status and send ayt,
    # the session's exit; quit from a full-screen program, the terminal back; a kill from another session, raw mode gone.
    # telnet to a scripted server that speaks no telnet: silent, a line typed sent whole, then a reset. ftp by the hairpin to
    # SurfOS's ftpd, scripted from standard input: folders, listings, put and get byte for byte, reget, mget, mput, text
    # mode. ftp to a scripted server (ScriptedFtp): a password typed with no echo and shown nowhere, and none asked for by a
    # script that a detached task runs (the kernel's spawn); EPSV refused and a PASV
    # naming another address, the data still from the server's own; an escape sequence in a name, a low port refused;
    # Ctrl+C in a transfer, the session alive; names that climb or are not safe; a reset partway, nothing left behind
    ('surfsh', [shown('SURF \u224b> ')],
     [('echo cli-net', ['\r\ncli-net\r\n', shown('SURF \u224b> ')]),
      ('telnet; telnet 10.0.2.2 0; ftp -x; echo status $?',
       ['usage: telnet host [port]', 'telnet: 0 is no port (1-65535)', 'usage: ftp [-n] [host [port]]', 'status 2', shown('SURF \u224b> ')]),
      ('telnetd on', ['telnetd: listening on 10.0.2.15 port 23', shown('SURF \u224b> ')]),
      (net_telnet_line, ['telnet: connected to 10.0.2.2 port', 'Ctrl+] gives a prompt', shown('SURF \u224b@> ')]),
      (b'pagetest size\r', ['TTY_GETSIZE 80x25', shown('SURF \u224b@> ')]),
      (b'\x1dstatus\r', ['telnet> ', 'The other end echoes what is typed; keys go one at a time',
                         'The other end knows the window: 80 by 25', 'Binary: sending yes, receiving no', 'bytes received']),
      (b'\x1dsend ayt\rexit\r', ['[SurfOS: yes]', 'telnet: the other end closed the connection', shown('SURF \u224b> ')]),
      (net_telnet_line, ['telnet: connected to 10.0.2.2 port', shown('SURF \u224b@> ')]),
      (b'less /initrd/README\r', ['\x1b[?1049h']),
      (b'\x1dquit\r', ['telnet> ', '\x1b[?1049l', shown('SURF \u224b> ')]),
      (net_telnet_line, ['telnet: connected to 10.0.2.2 port', shown('SURF \u224b@> ')]),
      (net_kill_telnet, ['\r\nraw-mode-gone\r\n', shown('SURF \u224b> ')]),
      (net_talk_line, ['telnet: connected to 10.0.2.2 port', '220 mail here']),
      (b'HELO surfos\r', ['HELO surfos', 'telnet: the connection was lost', PROMPT]),
      (net_ftpd_on, ['\r\nnet-talk-ok\r\n', 'telnetd: stopped', 'ftpd: serving / at ftp://10.0.2.15/ (read-write)', PROMPT]),
      ("printf 'cd /hda1/NET\\nmkdir REMOTE\\ncd REMOTE\\npwd\\nput UP.TXT UP2.TXT\\nsize UP2.TXT\\nls\\ndir\\nget UP2.TXT BACK.TXT\\n"
       "ascii\\nget UP2.TXT LF.TXT\\nbinary\\nrename UP2.TXT UP3.TXT\\n' > S1.TXT", [PROMPT]),
      ("printf 'cd /hda1/NET/REMOTE\\nreget UP3.TXT PART.TXT\\nreget UP3.TXT PART.TXT\\nmget *.TXT\\nmput LF.*\\ndelete LF.TXT\\n"
       "mget *.TXT\\ndelete UP3.TXT\\n' > S2.TXT; head -n 1 UP.TXT > PART.TXT", [PROMPT]),
      (net_ftp_files, ['/hda1/NET/REMOTE\r\n', 'UP2.TXT: ', 'BACK.TXT: ', 'status 0', re.compile(rb'\n +1 *\r'), PROMPT]),
      (net_ftp_more, ['PART.TXT: ', 'PART.TXT: whole already', 'UP3.TXT: ', 'LF.TXT: ', 'UP3.TXT is there already',
                      'the rest of the script does not run', 'status 1', re.compile(rb'\n +1 *\r'), shown('SURF \u224b> ')]),
      (net_script_line, ['ftpd: stopped', 'Password for bob: ']),
      (NET_PASSWORD.encode() + b'\r', ['230 in', '221 bye', shown('SURF \u224b> ')]),
      (net_detached_line, ['started]', PROMPT]),
      (net_detached_result, ['bob: a password is needed, and there is no terminal to ask on', PROMPT]),
      (net_login_line, ['220-scripted', '220 ready', 'ftp> ']),
      ('lcd /hda1\nuser alice', ['The local folder is /hda1', 'Password for alice: ']),
      (NET_PASSWORD.encode() + b'\r', ['230 in', 'ftp> ']),
      ('ls\ndir', ['nice.txt', '?[31mred?[0m', 'the server names port 25', 'ftp> ']),
      (net_ctrl_c, ['stopped', 'ftp> ']),
      (net_names, ['Connected to 10.0.2.2', 'nice.txt: 4 bytes', 'climb.txt: 2 bytes', 'not safe here: left out: bad|name.txt',
                   'not safe here: left out: x?[2J.txt', 'ftp> ']),
      ('get cut.bin CUT.BIN\nquit', [re.compile(rb'the data connection: |cut\.bin: 426 cut'), '221 bye', re.compile(rb'\[exit status [12]\]'), PROMPT]),
      (net_ftp_result, ['No such file', '\r\n0\r\n', '\r\nnet-ftp-ok\r\n', PROMPT]),
      ('exit', [])]),
    # 10/25/26 (the filters, user/bin): sort and its -u -n -r, tr's ranges, -d and -s, uniq -c, seq and tail -n, grep's
    # patterns and -i -n -v -c -F (status 1 when nothing matches), tee into a file, and dmesg through grep and head
    ('surfsh', [shown('SURF \u224b> ')],
     [('printf "b\\na\\nc\\na\\n" | sort', ['a\r\na\r\nb\r\nc\r\n', shown('SURF \u224b> ')]),
      ('printf "b\\na\\nc\\na\\n" | sort -u | tr a-z A-Z', ['A\r\nB\r\nC\r\n', shown('SURF \u224b> ')]),
      ('printf "10\\n9\\n100\\n" | sort -n; printf "10\\n9\\n100\\n" | sort -rn',
       ['9\r\n10\r\n100\r\n', '100\r\n10\r\n9\r\n', shown('SURF \u224b> ')]),
      ('printf "x\\nx\\ny\\nx\\n" | uniq -c', ['      2 x\r\n      1 y\r\n      1 x\r\n', shown('SURF \u224b> ')]),
      ('seq 1 3 10 | tail -n 2; seq 10 -3 1 | tr "\\n" " "; echo', ['7\r\n10\r\n', '10 7 4 1 \r\n', shown('SURF \u224b> ')]),
      ('printf "apple\\nBanana\\ncherry\\n" | grep -i an', ['Banana\r\n', shown('SURF \u224b> ')]),
      ('printf "apple\\nBanana\\ncherry\\n" | grep -n e', ['1:apple\r\n3:cherry\r\n', shown('SURF \u224b> ')]),
      ('printf "apple\\nBanana\\ncherry\\n" | grep "^c.*y$"; seq 5 | grep -v 3 | tr -d "\\n"; echo',
       ['cherry\r\n', '1245\r\n', shown('SURF \u224b> ')]),
      ('seq 20 | grep -c "1"; printf "a.p\\nap\\n" | grep -F "a.p"; printf "x\\n" | grep -F y; echo status $?',
       ['11\r\n', 'a.p\r\n', 'status 1\r\n', shown('SURF \u224b> ')]),
      ('echo "hello    world" | tr -s " "', ['hello world\r\n', shown('SURF \u224b> ')]),
      ('seq 3 | tee /hda1/TEE.TXT | wc -l; cat /hda1/TEE.TXT; rm /hda1/TEE.TXT',
       ['      3 ', '1\r\n2\r\n3\r\n', shown('SURF \u224b> ')]),
      ('dmesg | grep -i "fat16: hda1" | head -1', ['+FAT16: hda1: label SURFOS', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 10/25/26 (track A, A9): find (-name -iname -type -maxdepth -print0, a path that is not there), du (-a -s -h),
    # xargs (-n, quotes, -0, a kernel command, a command that is nowhere), cut (-d -f, -c with UTF-8, -b) and
    # sha256sum (and -c: OK, FAILED)
    ('surfsh', [shown('SURF \u224b> ')],
     [("find /initrd -name '*.conf'", ['/initrd/etc/surfsh.conf\r\n/initrd/etc/undertow.conf\r\n', shown('SURF \u224b> ')]),
      ('find /initrd -maxdepth 1 -type f', ['/initrd/README\r\n/initrd/files-help.txt\r\n/initrd/motd\r\n',
                                            shown('SURF \u224b> ')]),
      ("find /initrd/etc -type d -iname 'S*'; find /nowhere; echo status $?",
       ['/initrd/etc/ssl\r\n', 'find: /nowhere: No such file or directory', 'status 1', shown('SURF \u224b> ')]),
      ('du -a /initrd/etc/ssl; du -sh /initrd/bin', [re.compile(rb'\n[0-9]+\t/initrd/etc/ssl/README\r'),
                                                    re.compile(rb'\n[0-9]+\t/initrd/etc/ssl\r'),
                                                    re.compile(rb'\n[0-9.]+M\t/initrd/bin\r'), shown('SURF \u224b> ')]),
      ('echo a b c | xargs -n 2 echo x', ['x a b\r\nx c\r\n', shown('SURF \u224b> ')]),
      ('echo "\'a b\' c" | xargs -n 1 echo got', ['got a b\r\ngot c\r\n', shown('SURF \u224b> ')]),
      ("find /initrd/etc -name '*.conf' -print0 | xargs -0 echo",
       ['/initrd/etc/surfsh.conf /initrd/etc/undertow.conf\r\n', shown('SURF \u224b> ')]),
      ('echo 100 | xargs sleep; echo x | xargs nosuchcommand; echo status $?',
       ['slept 100 ms', 'xargs: nosuchcommand: not found', 'status 127', shown('SURF \u224b> ')]),
      # 10/25/26 (the programs' review of track A): U1, a command of 127 words, the most a program is given (xargs's own
      # name is the 128th), takes one word a run; U9, a pattern that ends in [ or [! takes the [ as it is; U10, du -h
      # rounds up
      ("echo x y | xargs echo $(seq 1 126) | cut -d' ' -f126-", ['\r\n126 x\r\n126 y\r\n', shown('SURF \u224b> ')]),
      ("echo x > '/hda1/ss['; find /hda1 -maxdepth 1 -name 'ss['; find /hda1 -maxdepth 1 -name 's[!' | grep -c .; "
       "rm '/hda1/ss['", ['\r\n/hda1/ss[\r\n0\r\n', shown('SURF \u224b> ')]),
      # (22 copies of seq 10000, 48,894 bytes: 1,051 clusters of 1 KB, 1.03 MB: 1.1M, not 1.0M; a short line, as the
      # serial line's input queue holds 256 bytes)
      ('seq 10000 > /hda1/A.TXT; { for i in $(seq 22); do cat /hda1/A.TXT; done; } > /hda1/B.TXT; du -h /hda1/B.TXT; '
       'rm /hda1/A.TXT /hda1/B.TXT', ['\r\n1.1M\t/hda1/B.TXT\r\n', shown('SURF \u224b> ')]),
      ("echo a:b:c | cut -d: -f2,3; echo h\u00e9llo | cut -c2-3; printf 'a\\tb\\tc\\n' | cut -f1,3; echo abcdef | cut -b-2,5-",
       ['b:c\r\n', '\u00e9l\r\n', 'a\tc\r\n', 'abef\r\n', shown('SURF \u224b> ')]),
      ('echo abc | sha256sum', ['edeaaff3f1774ad2888673770c6d64097e391bc362d7d6fb34982ddf0efd18cb  -\r\n', shown('SURF \u224b> ')]),
      ('sha256sum /initrd/motd > /hda1/SUMS.TXT; sha256sum -c /hda1/SUMS.TXT; rm /hda1/SUMS.TXT',
       ['/initrd/motd: OK\r\n', shown('SURF \u224b> ')]),
      ("printf '%064d  /initrd/motd\\n' 0 | sha256sum -c; echo status $?",
       ['/initrd/motd: FAILED\r\n', 'status 1', shown('SURF \u224b> ')]),
      ('exit', [])]),
    # 10/6/26: telnetd, the telnet server (user/telnetd/), through the forwarded port 23 with the harness's telnet
    # client (Telnet, below). Not running at first; on; on again (running already, status 1); ports that are none and
    # --server typed by hand (status 2); a session: the banner, a command, cd, Tab, help paged, Ctrl+C, exit; two
    # sessions at once, status, off ending both, on again; four, and a fifth refused; a client that stops reading while
    # a second session is served; a client whose keys flood a session that reads none; a paste into edit, whole; keys
    # and a Ctrl+C in one segment; a pasted block at the prompt, its first line only; the rest of the protocol in one
    # session that ends with a reset; a client in binary mode; telnetd off typed in a session; on while nettest has the
    # port; off when not running (status 1); the server's log.
    ('telnetd', ['telnetd: not running (telnetd on starts it)']),
    ('telnetd on', ['telnetd: listening on 10.0.2.15 port 23']),
    ('run telnetd on', ['telnetd: running already (pid', 'exit status 1']),
    ('run telnetd on 65536', ['telnetd: 65536 is no port (1-65535)', 'exit status 2']),
    ('run telnetd on +23', ['telnetd: +23 is no port (1-65535)', 'exit status 2']),
    ('run telnetd --server 23', ['telnetd: --server is how telnetd on starts the server', 'exit status 2']),
    ('@telnet-session', []),
    ('@memhog-hold', []),                   # 10/25/26 (L1-L9): memhog hold, and a telnet login after (memhog_hold_check)
    ('@telnet-two', []),
    ('@telnet-full', []),
    ('@telnet-stalled', []),
    ('@telnet-flood', []),
    ('@telnet-paste', []),
    ('@telnet-ctrlc-keys', []),
    ('@telnet-paste-lines', []),
    ('@surfsh-crlf', []),
    ('@telnet-protocol', []),
    ('@telnet-binary', []),
    ('@telnet-win', []),                    # 10/25/26: before off-inside stops the server
    ('@telnet-off-inside', []),
    ('@telnet-port-busy', []),
    ('run telnetd off', ['telnetd: not running', 'exit status 1']),
    ('dmesg', ['telnetd: listening on port 23', 'telnetd: session 1 from 10.0.2.2:', 'telnetd: session 1 ended (the shell',
               'telnetd: telnetd off stops the server (pid', ', 2 session shells)', 'refused: 4 sessions are open',
               'ended (the client left)', 'telnetd: telnetd off stops the server (pid', ', 1 session shell)']),
    # 10/17/26: httpd, the web server (user/httpd/), from this host through the forwarded ports 80 and 8080 with
    # http.client and raw sockets (the checks below). Not running at first; the commands' refusals (status 1 and 2);
    # on: the sample site, /initrd/www; on again (running already); the site's files, HEAD, 304, 404s; what the
    # protocol refuses; keep-alive and pipelining; six places; the timeouts; off. The initrd's root on port 8080:
    # its listings, a 301, a large file. A site made on the FAT disk, served by httpd serve, which prints each
    # request, until Ctrl+C; then the port is free again, and httpd on serves a 1.5 MB file to a client that stops
    # reading while others are answered. The kernel's log.
    ('httpd', ['httpd: not running (httpd on starts it)']),
    ('run httpd on /initrd/nowhere', ['httpd: /initrd/nowhere: no such folder', 'exit status 1']),
    ('run httpd on /initrd/README', ['httpd: /initrd/README is a file, not a folder', 'exit status 1']),
    ('run httpd on 0', ['httpd: 0 is no port (1-65535)', 'exit status 2']),
    ('run httpd on 80 81', ['usage: httpd [on [folder] [port]', 'exit status 2']),
    ('run httpd on -p 80', ['httpd: no option -p', 'exit status 2']),
    ('run httpd --server 80 /initrd/www', ['httpd: --server is how httpd on starts the server', 'exit status 2']),
    ('run httpd off', ['httpd: not running', 'exit status 1']),
    ('httpd on', ['httpd: serving /initrd/www at http://10.0.2.15/']),
    ('run httpd on /initrd', ['httpd: running already (pid', 'exit status 1']),
    ('httpd status', ['httpd: running (pid']),
    ('run httpd serve 80', ['httpd: port 80 is in use (the server httpd on started, pid', 'exit status 1']),
    ('@httpd-site', []),
    ('@httpd-protocol', []),
    ('@httpd-keepalive', []),
    ('@httpd-places', []),
    ('@httpd-timeouts', []),
    ('httpd off', ['httpd: stopped']),
    ('httpd', ['httpd: not running']),
    ('cd /initrd/etc', []),
    ('httpd on .. 8080', ['httpd: serving /initrd at http://10.0.2.15:8080/']),
    ('cd /', []),
    ('@httpd-initrd', []),
    ('httpd off', ['httpd: stopped']),
    ('mkdir /hda1/WWW', []),
    ("echo '<h1>FAT site</h1>' > /hda1/WWW/INDEX.HTM", []),
    ('echo a space > "/hda1/Hello World.txt"', []),
    ('echo hidden > /hda1/.secret', []),
    (BIG_BIN, []),
    ('@httpd-serve', []),
    ('httpd on /hda1', ['httpd: serving /hda1 at http://10.0.2.15/']),     # Ctrl+C freed the port
    ('@httpd-slow', []),
    ('httpd off', ['httpd: stopped']),
    ('rm /hda1/WWW/INDEX.HTM "/hda1/Hello World.txt" /hda1/.secret /hda1/BIG.BIN', []),
    ('rmdir /hda1/WWW', []),
    ('dmesg', ['httpd: serving /initrd/www on port 80', 'httpd: httpd off stops the server (pid',
               'httpd: serving /initrd on port 8080', 'httpd: serving /hda1 on port 80']),
    # 10/24/26: ftpd, the file transfer server (user/ftpd/), from this host through the forwarded port 21 and its
    # passive range (q.ports['ftp'], q.ports['ftp_pasv_range'], each passive port forwarded to the same port of the
    # guest) with Python's ftplib and raw sockets (the checks below). Not running at first; the commands' refusals
    # (status 1 and 2); on: every mounted volume; on again; a session (login, folders, listings, the initrd's files
    # byte for byte); uploads to the FAT disk read back, append, resume, unique names, renames, folders, hidden files;
    # text mode; active mode; ABOR and commands during a transfer; the protocol; six places; the data connection's
    # time; off. Then the FAT disk alone, read-only; ftpd serve, a screenful of commands and more, Ctrl+C; the log.
    ('ftpd', ['ftpd: not running (ftpd on starts it)']),
    ('run ftpd on /initrd/nowhere', ['ftpd: /initrd/nowhere: no such folder', 'exit status 1']),
    ('run ftpd on /initrd/README', ['ftpd: /initrd/README is a file, not a folder', 'exit status 1']),
    ('run ftpd on /dev', ['ftpd: /dev is not for serving (the devices)', 'exit status 1']),
    ('run ftpd on 0', ['ftpd: 0 is no port (1-65535)', 'exit status 2']),
    ('run ftpd on 21 22', ['usage: ftpd [on [-r] [-a address]', 'exit status 2']),
    ('run ftpd on -x', ['ftpd: no option -x', 'exit status 2']),
    ('run ftpd on 9-2', ['ftpd: 9-2 is no range of ports', 'exit status 2']),
    ('run ftpd on 50010', ['ftpd: port 50010 is inside the passive range 50000-50999', 'exit status 2']),
    ('run ftpd on -a 300.1.1.1', ['ftpd: -a needs an IPv4 address', 'exit status 2']),
    ('run ftpd --server 21 / rw 50000-50999', ['ftpd: --server is how ftpd on starts the server', 'exit status 2']),
    ('run ftpd off', ['ftpd: not running', 'exit status 1']),
    ('ftpd on {ftp_pasv_range}', ['ftpd: serving / at ftp://10.0.2.15/ (read-write)']),
    ('run ftpd on /hda1', ['ftpd: running already (pid', 'exit status 1']),
    ('ftpd status', ['ftpd: running (pid']),
    ('run ftpd serve 21', ['ftpd: port 21 is in use (the server ftpd on started, pid', 'exit status 1']),
    ('@ftpd-session', []),
    ('@ftpd-upload', []),
    ('@ftpd-text', []),
    ('@ftpd-active', []),
    (BIG_BIN, []),
    ('@ftpd-abort', []),
    ('@ftpd-descriptors', []),
    ('rm /hda1/BIG.BIN', []),
    ('@ftpd-protocol', []),
    ('@ftpd-places', []),
    ('@ftpd-data-time', []),
    ('ftpd off', ['ftpd: stopped']),
    ('ftpd', ['ftpd: not running']),
    ('ftpd on -r /hda1 {ftp_pasv_range}', ['ftpd: serving /hda1 at ftp://10.0.2.15/ (read-only)']),
    ('@ftpd-readonly', []),
    ('ftpd off', ['ftpd: stopped']),
    ('@ftpd-serve', []),
    ('dmesg', ['ftpd: serving / on port 21 (read-write, passive ports', 'ftpd: session 1 from 10.0.2.2:',
               'ftpd: session 1 ended (the client left): 4 files sent, 0 received',
               'ftpd: a session from 10.0.2.2 refused: 6 are open', 'ftpd: ftpd off stops the server (pid',
               'ftpd: serving /hda1 on port 21 (read-only, passive ports']),
    # 10/24/26: nc, the netcat of SurfOS (user/bin/nc.c), with this host as the Mac's nc: the refusals; a file of
    # 200000 random bytes received (nc -l on guest port RX_PORT, forwarded) and sent back whole with -v (the input's
    # end said with a half-close, the answer after it printed); a line typed, sent with CR LF (-c); -z on an open
    # and a closed port
    ('run nc', ['usage: nc [-vc] [-w seconds] host port', 'exit status 2']),
    ('run nc -q 10.0.2.2 1', ['nc: no option -q', 'exit status 2']),
    ('run nc 10.0.2.2 0', ['nc: 0 is no port (1-65535)', 'exit status 2']),
    ('run nc -z 10.0.2.2 9-1', ['nc: 9-1 is no port (1-65535) or range (first-last)', 'exit status 2']),
    ('run nc -z 10.0.2.2 1-2000', ['nc: 2000 ports: at most 1000 at once', 'exit status 2']),
    ('run nc -w 0 -l 5555', ['nc: -w 0: a time in seconds, 1-86400', 'exit status 2']),
    ('run nc a..b 80', ['nc: a..b: not a host name', 'exit status 1']),
    ('run nc 10.0.2.2 1', ['nc: 10.0.2.2 port 1: Connection refused', 'exit status 1']),
    ('run nc -w 2 -l %d' % RX_PORT, ['nc: listening on 10.0.2.15 port %d' % RX_PORT, 'nc: no connection within 2 s',
                                    'exit status 1']),
    ('@nc-receive', []),
    ('run wc -c /hda1/NC.BIN', [' 200000 /hda1/NC.BIN']),
    ('@nc-send', []),
    ('@nc-talk', []),
    ('@nc-scan', []),
    ('rm /hda1/NC.BIN', []),
    # 10/24/26: fetch, the downloader (user/fetch/), from this host's HTTP server (TestHandler's /fetch/ paths) onto
    # the FAT disk: the refusals; a file, then again (there already: -f replaces it, -c finds it whole); one cut
    # short and gone on with (-c: a Range request for the rest); a server that ignores Range (its 200 refused: -f);
    # the rest in parts, a part longer than its range, a 204; chunked;
    # the end of the connection, to standard output; a redirect (the first name kept), a loop, a 404 (no file); a
    # name with spaces; -o a folder, a device; the initrd (read-only); the files read back through httpd. HTTPS and
    # the fixtures' strict framing: tools/qemu-web.py's fetch scenarios.
    ('run fetch', ['usage: fetch [-o file|folder|-] [-c] [-f] [-q] [-t ', 'exit status 2']),
    # the user shell's quotes keep the time one word (the kernel's run would split it)
    ("fetch -t '2026-13-01 00:00:00' http://10.0.2.2/; echo status $?",
     ['fetch: -t: 2026-13-01 00:00:00: the month must be 01 to 12', 'status 2']),
    ('run fetch ftp://10.0.2.2/x', ['fetch: ftp://10.0.2.2/x: the address starts with http:// or https://',
                                    'exit status 2']),
    ('run fetch -x http://10.0.2.2/', ['fetch: there is no option -x', 'exit status 2']),
    ('run fetch -c -o - http://10.0.2.2/', ['fetch: -c goes on with a file, not with standard output', 'exit status 2']),
    ('run fetch http://10.0.2.2:1/x', ['fetch: ', 'refused', 'exit status 1']),
    ('cd /hda1', []),
    ('fetch http://10.0.2.2:{http}/fetch/data.bin', ['fetch: 292.9 KB in', '-> data.bin']),
    ('run fetch http://10.0.2.2:{http}/fetch/data.bin', ['fetch: data.bin is there already: -f replaces it, -c goes '
                                                         'on with it', 'exit status 1']),
    ('fetch -c http://10.0.2.2:{http}/fetch/data.bin', ['fetch: data.bin is whole already (300000 bytes)']),
    ('fetch -f -q http://10.0.2.2:{http}/fetch/data.bin', []),
    ('run fetch http://10.0.2.2:{http}/fetch/cut.bin', ['fetch: 10.0.2.2 closed the connection after 100000 of 300000 '
                                                        'bytes; fetch -c goes on with cut.bin', 'exit status 1']),
    ('fetch -c http://10.0.2.2:{http}/fetch/cut.bin', ['fetch: 195.3 KB in', '-> cut.bin (the rest of it)']),
    ('run fetch -o norange.bin http://10.0.2.2:{http}/fetch/cut.bin', ['after 100000 of 300000 bytes',
                                                                       'exit status 1']),
    ('run fetch -c http://10.0.2.2:{http}/fetch/norange.bin', ['fetch: 10.0.2.2 sent all of it (200 OK), not the rest: '
                                                               'norange.bin is kept as it was; fetch -f downloads it '
                                                               'anew', 'exit status 1']),
    ('fetch -f http://10.0.2.2:{http}/fetch/norange.bin', ['-> norange.bin']),
    # the rest a part at a time (part.bin: 100000 bytes a Range), a part longer than its range, a 204 (nothing to save)
    ('run fetch -o part.bin http://10.0.2.2:{http}/fetch/cut.bin', ['after 100000 of 300000 bytes', 'exit status 1']),
    ('run fetch -c -o part.bin http://10.0.2.2:{http}/fetch/badpart.bin',
     ['fetch: 10.0.2.2 sent a part of 200010 bytes for a range of 200000 (bytes 100000 to 299999): part.bin is kept '
      'as it was', 'exit status 1']),
    ('run fetch -c -o part.bin http://10.0.2.2:{http}/fetch/empty',
     ['fetch: 10.0.2.2 answered 204 No Content: there is nothing to save, and the file there is kept as it was',
      'exit status 1']),
    ('run fetch -c http://10.0.2.2:{http}/fetch/part.bin', ['fetch: 10.0.2.2 sent bytes 100000 to 199999 of 300000, not '
                                                            'all of the rest; fetch -c goes on with part.bin',
                                                            'exit status 1']),
    ('fetch -c http://10.0.2.2:{http}/fetch/part.bin', ['-> part.bin (the rest of it)']),
    ('run fetch http://10.0.2.2:{http}/fetch/empty', ['fetch: 10.0.2.2 answered 204 No Content: there is nothing to save',
                                                      'exit status 1']),
    ("fetch -o '' http://10.0.2.2/; echo status $?", ['fetch: -o names nothing', 'status 2']),
    ('fetch http://10.0.2.2:{http}/fetch/chunked.txt', ['-> chunked.txt']),
    ('fetch -o - http://10.0.2.2:{http}/fetch/close.txt', ['the end of the connection ends this body']),
    ('fetch http://10.0.2.2:{http}/fetch/redirect', ['fetch: 302 Found: to http://10.0.2.2:', '/fetch/data.bin',
                                                     '-> redirect']),
    ('run fetch http://10.0.2.2:{http}/fetch/loop', ['fetch: the address redirects in a loop (back to http://10.0.2.2:',
                                                     'exit status 1']),
    ('run fetch http://10.0.2.2:{http}/fetch/missing', ['/fetch/missing: 404 Not Found', 'exit status 1']),
    ('fetch http://10.0.2.2:{http}/fetch/name%20with%20space.txt', ['-> name with space.txt']),
    ('mkdir /hda1/DL', []),
    ('fetch -o /hda1/DL http://10.0.2.2:{http}/fetch/data.bin', ['-> /hda1/DL/data.bin']),
    ('fetch -o /dev/null http://10.0.2.2:{http}/fetch/data.bin', ['-> /dev/null']),
    ('run fetch -c -o /dev/null http://10.0.2.2:{http}/fetch/data.bin', ['fetch: -c goes on with a file, not with the '
                                                                         'device /dev/null', 'exit status 1']),
    ('cd /initrd', []),
    ('run fetch http://10.0.2.2:{http}/fetch/data.bin', ['fetch: data.bin: ', 'exit status 1']),
    ('cd /', []),
    ('@fetch-files', []),
    ('rm /hda1/data.bin /hda1/cut.bin /hda1/norange.bin /hda1/part.bin /hda1/chunked.txt /hda1/redirect '
     '"/hda1/name with space.txt" /hda1/DL/data.bin', []),
    ('rmdir /hda1/DL', []),
    ('umount /hda1', []),                                   # marks it clean for fsck.fat on the host
]
# steps that legitimately take longer than the 10 s default (by command, or by its first word)
TIMEOUTS = {BIG_BIN: 60, 'selftest': 120, 'run pipetest stream': 60, 'run pipetest atomic': 60, 'run pipetest limits': 60, 'run fstest /hda1/FST': 60, 'fsbench': 60, 'run kcmdtest /hda1': 60,
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
        # 10/26/26: the serial port over TCP on 127.0.0.1, no longer a UNIX socket. QEMU writes it a byte at a time,
        # and a Linux UNIX socket holds only some 277 bytes unread so (each byte costs a buffer's overhead). While the
        # harness looked elsewhere for a few hundred ms, QEMU stopped taking bytes, the guest's bounded wait for the
        # UART gave up, and each byte after that overwrote the oldest of the 16 in its FIFO: a banner came out as
        # "SurfOS#>" (B4's gate, @boot shell=kernel at -O2). A TCP connection keeps a hundred kilobytes and more
        self.ser_port = free_port()
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
        if scratch_mb and not iso:
            cmdline.append('bootvol=' + SELFTEST_BOOTVOL)
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
        # server on the host (10.0.2.2) whose port travels on the kernel command line. 10/9/26 (the
        # development loop): guest port RX_PORT too, where rx listens in the smoke test
        # 10/18/26 (round fs-fix-11): a forwarded port is written {port:NAME} (here and in extra) and picked afresh for
        # each try below: QEMU binds it, and a port free_port() found free may be taken before QEMU starts
        self.ports = {}
        self.http = None
        if net:
            # 10/24/26: ftpd, the file transfer server, on port 21, and its passive range: FTP_PASV_PORTS ports
            # forwarded to the same ports of the guest (EPSV gives a client the port alone), its first one
            # ports['ftp_pasv'] ('ftpd on ... {ftp_pasv_range}' in a step). 10/9/26: telnet_wlan is port 23 of 10.0.2.16,
            # the simulated wlan0's lease; 10/17/26: www and www2 are httpd, the web server, on ports 80 and 8080
            self.ports['ftp_pasv'] = free_port_range(FTP_PASV_PORTS)
            self.ports['ftp_pasv_range'] = f"{self.ports['ftp_pasv']}-{self.ports['ftp_pasv'] + FTP_PASV_PORTS - 1}"
            pasv = ''.join(f",hostfwd=tcp:127.0.0.1:{p}-:{p}"
                           for p in range(self.ports['ftp_pasv'], self.ports['ftp_pasv'] + FTP_PASV_PORTS))
            boot += ['-netdev', "user,id=net0,hostfwd=tcp:127.0.0.1:{port:echo}-:7,"
                                "hostfwd=tcp:127.0.0.1:{port:telnet}-:23,"
                                "hostfwd=tcp:127.0.0.1:{port:rx}-:%d,"
                                "hostfwd=tcp:127.0.0.1:{port:telnet_wlan}-10.0.2.16:23,"
                                "hostfwd=tcp:127.0.0.1:{port:www}-:80,"
                                "hostfwd=tcp:127.0.0.1:{port:www2}-:8080,"
                                "hostfwd=tcp:127.0.0.1:{port:ftp}-:21" % RX_PORT + pasv,
                     '-device', 'e1000,netdev=net0,mac=52:54:00:12:34:56']
            # (10/9/26: telnet_wlan is port 23 of 10.0.2.16, the simulated wlan0's lease.) The test's HTTP server binds
            # its port itself, so it keeps it (10/18/26: before, free_port()'s, which another program could take first)
            self.http = http.server.HTTPServer(('127.0.0.1', 0), TestHandler)
            self.ports['http'] = self.http.server_address[1]
            cmdline.append(f"httpport={self.ports['http']}")
            threading.Thread(target=self.http.serve_forever, daemon=True).start()
        if cmdline and not iso:
            boot += ['-append', ' '.join(cmdline)]
        for attempt in range(1, PORT_TRIES + 1):
            ports = dict(self.ports)
            cmd = [qemu, '-m', str(mem)] + pick_ports(boot, ports) + [
                   '-display', 'none', '-no-reboot', '-no-shutdown',
                   '-monitor', f'unix:{self.mon_path},server,nowait',
                   '-chardev', f'socket,id=ser0,host=127.0.0.1,port={self.ser_port},server=on,wait=off,nodelay=on',
                   '-serial', 'chardev:ser0'] + pick_ports(list(extra), ports)
            if int_log:
                cmd += ['-d', 'int,guest_errors,cpu_reset', '-D', os.path.abspath(int_log)]
            self.cmd = cmd
            self.proc = subprocess.Popen(cmd, cwd=self.workdir, stdin=subprocess.DEVNULL,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                self.ser = self._connect(self.ser_port)
            except RuntimeError as e:
                picked = {ports[n] for n in ports if n not in self.ports}   # (10/19/26, B78: not the caller's own)
                if attempt < PORT_TRIES and picked_port_taken(str(e), picked):
                    retry_note(attempt, str(e))
                    self.proc.stdout.close()
                    if os.path.exists(self.mon_path):
                        os.remove(self.mon_path)
                    continue
                raise
            self.ports = ports
            break
        self.ser.setblocking(False)
        self.transcript = b''
        self.cursor = 0
        self.more_at, self.auto_more = 0, True

    def _connect(self, port, timeout=10):
        deadline = time.time() + timeout
        while True:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.connect(('127.0.0.1', port))
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)     # keys go at once, as they did
                return s
            except OSError:
                s.close()
                if self.proc.poll() is not None or time.time() > deadline:
                    s.close()
                    raise RuntimeError('QEMU did not start: ' + ' '.join(self.cmd) + '\n' + self.qemu_output())
                time.sleep(0.05)

    def alive(self):
        return self.proc.poll() is None

    def wait(self, seconds):
        """sleep while draining the serial port, so QEMU never back-pressures the UART. 10/11/26: in
        slices no longer than what is left, so a short wait is short (wait(.03) once took up to 0.1 s,
        past the 100 ms a program allows between the bytes of an escape sequence)"""
        deadline = time.time() + seconds
        while True:
            left = deadline - time.time()
            if left <= 0:
                break
            self.pump(min(0.1, left))

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

    def text(self, skip=()):
        """the transcript as text; 10/25/26: without the byte ranges in skip (boot_expect()'s look at dmesg)"""
        t = self.transcript
        for a, b in sorted(skip, reverse=True):
            t = t[:a] + t[b:]
        t = t.decode(errors='replace').replace('\r', '')
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
                 'home': 'home', 'end': 'end', 'pgup': 'pgup', 'pgdn': 'pgdn',
                 'spc': 'spc', 'space': 'spc', 'ret': 'ret'}    # 10/25/26: for <ctrl-spc>, <alt-ret> (A4)
        named.update({f'f{i}': f'f{i}' for i in range(1, 13)})
        for tok in re.findall(r'<[^>]+>|.', spec.encode().decode('unicode_escape'), re.S):
            if tok.startswith('<'):             # 10/25/26 (track A, A4): <ctrl-left>, <shift-tab>, <alt-x> too
                *mods, name = tok[1:-1].lower().split('-')
                key = named.get(name) or keymap.get(name)
                if key and mods:
                    key = '-'.join(mods + [key]) if set(mods) <= {'shift', 'ctrl', 'alt'} else None
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


PORTS_GIVEN = set()     # 10/25/26: every port free_port() and free_port_range() have handed out in this run


def free_port():
    """a port free here, never one handed out before in this run. 10/25/26: the port is let go again for QEMU to
    bind, so the system may give the same one to the next call, and two forwards of one QEMU on one port stop it
    starting ("QEMU did not start", seen on @boot autonet)"""
    for _ in range(100):
        s = socket.socket()
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
        s.close()
        if port not in PORTS_GIVEN:
            PORTS_GIVEN.add(port)
            return port
    raise RuntimeError('no free port that was not handed out already')


def console_keep(q):
    """10/25/26: what the console has printed, read now. A check that makes the guest print a burst of lines (a
    server's line for each of many requests) reads them as they come: QEMU writes the serial port to its socket a
    byte at a time, and under Linux the socket holds only some hundred bytes unread (each byte costs a buffer's
    overhead); beyond that QEMU drops what the guest writes, and the guest's bounded wait for the UART gives up.
    macOS holds kilobytes, so a check that read the console only afterwards passed there and lost lines in Docker"""
    q.pump(0)        # 10/26/26: still read early, though the serial port's TCP connection keeps far more now


def free_port_range(n, avoid=()):
    """10/24/26 (ftpd): the first of n ports in a row that are free here, from 20000 up to 59999: forwarded to the
    same ports of the guest, which listens on them for passive data connections. 10/25/26 (review): none of the
    ports in avoid, which free_port() gave the other forwards (closed again, so they look free here too), nor any
    other port handed out in this run; free_port() then hands out none of these"""
    for _ in range(200):
        base = random.randrange(20000, 60000 - n)
        if any(base <= p < base + n for p in avoid) or any(p in PORTS_GIVEN for p in range(base, base + n)):
            continue
        socks = []
        try:
            for p in range(base, base + n):
                s = socket.socket()
                socks.append(s)
                s.bind(('127.0.0.1', p))
            PORTS_GIVEN.update(range(base, base + n))
            return base
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    raise RuntimeError(f'no {n} free ports in a row')


# 10/24/26 (fetch, user/fetch/): what fetch downloads from this host, TestHandler's /fetch/ paths. FETCH_DATA has
# a Content-Length and takes a Range (bytes=N-): 206 and the rest, 416 at its end; cut.bin is FETCH_DATA cut after
# FETCH_CUT bytes (the end of the connection) unless a Range asks for the rest; norange.bin never takes a Range;
# part.bin sends FETCH_CUT bytes of the rest at most; badpart.bin a part 10 bytes longer than its range; empty is a
# 204; chunked.txt is FETCH_TEXT in chunks with a trailer; close.txt ends with the connection; redirect goes to
# data.bin; loop to itself; missing is a 404. FETCH_SEEN: (path, Range, User-Agent) of every request.
FETCH_DATA = b''.join(b'fetch %06d ' % i + bytes(range(i % 11, 250, 11)) for i in range(9000))[:300000]
FETCH_CUT = 100000
FETCH_TEXT = b''.join(b'line %03d of the chunked text\n' % i for i in range(300))
FETCH_CLOSE = b'the end of the connection ends this body\n'
FETCH_SEEN = []


# 10/18/26 (round fs-fix-11): free_port() lets its port go so that QEMU can bind it, and another program may take it
# first: QEMU then stops at once ("Could not set up host forwarding rule ...", or for a socket of its own "Address
# already in use"). A launch with ports written {port:NAME} (picked here) tries again with fresh ones, PORT_TRIES times
# in all, and says so; every other failure to start stays fatal (tests/test_qemu_ports.py). 10/19/26 (audit B78): only
# when the port QEMU names is one picked here: a port the caller wrote itself, taken, stays fatal at once, also in a
# launch that has picked ones too (before, any taken port was tried again whenever the launch had picked ports)
PORT_TRIES = 5
PORT_TAKEN = ('Could not set up host forwarding rule', 'Address already in use')


def pick_ports(args, ports):
    """args with each {port:NAME} replaced by a port: the one in ports, or a fresh one (free_port()) put there"""
    out = []
    for a in args:
        for name in re.findall(r'\{port:(\w+)\}', a):
            if name not in ports:
                ports[name] = free_port()
            a = a.replace('{port:%s}' % name, str(ports[name]))
        out.append(a)
    return out


def port_taken(output):
    """did QEMU stop because a port it was to bind was taken?"""
    return any(m in output for m in PORT_TAKEN)


def taken_ports(output):
    """10/19/26 (audit B78): the ports QEMU says it could not bind: the forwarding rule's ('tcp:127.0.0.1:PORT-...'),
    or a datagram socket's (its -netdev, which QEMU's message quotes, says local.port=PORT)"""
    found = []
    for line in output.splitlines():
        m = re.search(r"Could not set up host forwarding rule '[a-z]+:[0-9.]*:(\d+)-", line)
        if m:
            found.append(int(m.group(1)))
        elif 'Address already in use' in line:
            found += [int(p) for p in re.findall(r'local\.port=(\d+)', line)]
    return found


def picked_port_taken(output, picked):
    """10/19/26 (audit B78): did QEMU stop because a port picked for it (in picked) was taken, and only such a one?"""
    taken = taken_ports(output)
    return bool(taken) and port_taken(output) and all(p in picked for p in taken)


def retry_note(attempt, output):
    """one line in the log for a launch tried again with fresh ports"""
    why = next((l.strip() for l in output.splitlines() if port_taken(l)), 'a port was taken')
    print(f'QEMU did not start: a port picked for it was taken by another program meanwhile ({why}); starting it '
          f'again with fresh ports (try {attempt + 1} of {PORT_TRIES})', flush=True)


class TestHandler(http.server.BaseHTTPRequestHandler):
    """what the guest's httpget and self test fetch; 10/24/26: and fetch (the /fetch/ paths)"""
    def do_GET(self):
        if self.path.startswith('/fetch/'):
            FETCH_SEEN.append((self.path, self.headers.get('Range'), self.headers.get('User-Agent')))
            return self.fetch_path(self.path[len('/fetch/'):])
        body = f'SurfOS HTTP test OK: {self.path}\n'.encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def raw(self, head, body=b''):
        """a response as it is written here (status line and fields, one per line), then the end of the connection"""
        self.close_connection = True
        try:
            self.wfile.write(head.replace('\n', '\r\n').encode() + b'Connection: close\r\n\r\n' + body)
        except (BrokenPipeError, ConnectionResetError):
            pass                        # 10/25/26: fetch refuses a response it does not want, its body unread

    def ranged(self, data, ranges=True, cap=None, extra=b''):
        """data, or the rest of it from byte N when a Range (bytes=N-) asks for it: 206 (cap bytes of it at most;
        extra: bytes more than the range, counted in the Content-Length), or 416 at or past its end"""
        m = re.fullmatch(r'bytes=(\d+)-', self.headers.get('Range') or '')
        if not (m and ranges):
            return self.raw(f'HTTP/1.1 200 OK\nContent-Length: {len(data)}\n', data)
        start = int(m.group(1))
        if start >= len(data):
            return self.raw(f'HTTP/1.1 416 Range Not Satisfiable\nContent-Range: bytes */{len(data)}\n'
                            f'Content-Length: 0\n')
        end = len(data) if cap is None else min(len(data), start + cap)
        return self.raw(f'HTTP/1.1 206 Partial Content\nContent-Range: bytes {start}-{end - 1}/{len(data)}\n'
                        f'Content-Length: {end - start + len(extra)}\n', data[start:end] + extra)

    def fetch_path(self, name):
        if name == 'data.bin':
            self.ranged(FETCH_DATA)
        elif name == 'cut.bin' and not self.headers.get('Range'):
            self.raw(f'HTTP/1.1 200 OK\nContent-Length: {len(FETCH_DATA)}\n', FETCH_DATA[:FETCH_CUT])
        elif name == 'cut.bin':
            self.ranged(FETCH_DATA)
        elif name == 'norange.bin':
            self.ranged(FETCH_DATA, ranges=False)
        elif name == 'part.bin':
            self.ranged(FETCH_DATA, cap=FETCH_CUT)
        elif name == 'badpart.bin':
            self.ranged(FETCH_DATA, extra=b'0123456789')
        elif name == 'empty':
            self.raw('HTTP/1.1 204 No Content\n')
        elif name == 'chunked.txt':
            body = b''.join(b'%x;n=%d\r\n%s\r\n' % (len(FETCH_TEXT[i:i + 333]), i, FETCH_TEXT[i:i + 333])
                            for i in range(0, len(FETCH_TEXT), 333))
            self.raw('HTTP/1.1 200 OK\nTransfer-Encoding: chunked\n', body + b'0\r\nX-Trailer: yes\r\n\r\n')
        elif name == 'close.txt':
            self.raw('HTTP/1.1 200 OK\nContent-Type: text/plain\n', FETCH_CLOSE)
        elif name == 'redirect':
            self.raw('HTTP/1.1 302 Found\nLocation: /fetch/data.bin\nContent-Length: 0\n')
        elif name == 'loop':
            self.raw('HTTP/1.1 302 Found\nLocation: /fetch/loop\nContent-Length: 0\n')
        elif name == 'name%20with%20space.txt':
            self.raw('HTTP/1.1 200 OK\nContent-Length: 19\n', b'a name with spaces\n')
        else:
            self.raw('HTTP/1.1 404 Not Found\nContent-Length: 14\n', b'no such thing\n')

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
             12: 'ANSI-TEST done, 80x25',
             # 10/25/26 (track A, A8): CHA and ECH; ESC 7/8, ESC[s/u and VPA; SGR 22, 92, 39, 44, 49, 38 dropped
             5: '         COL       TWENTY    X   XX', 9: '         CB        FE        VPA', 11: '         BNGDU.S'}


def ansi_screen_check(q):
    """after `ansi`: its pattern in VGA memory, characters and attributes, and Z in the corner"""
    d = q.vga_memory() or bytes(4000)

    def row(r):
        return cells_text(d[r * 160:(r + 1) * 160:2]).rstrip()

    def attr(c, r):
        return d[(r * 80 + c) * 2 + 1]
    ok = all(row(r) == text for r, text in ANSI_ROWS.items()) and d[(24 * 80 + 79) * 2] == ord('Z')
    ok = ok and attr(9, 2) == 0x70 and attr(16, 2) == 0x07 and attr(9, 3) == 0x1E   # inverse, normal, bright yellow on blue
    # 10/25/26 (A8): bright yellow, bright off, bright green, the writer's, blue behind, behind off, 38;5;1 not bold
    ok = ok and [attr(c, 11) for c in range(9, 16)] == [0x0E, 0x06, 0x0A, 0x07, 0x17, 0x07, 0x07]
    return ('escape sequences on the VGA screen: position, moves, erase, inverse, colours, the corner; '
            'G d s u X, ESC 7 and 8, SGR 22 39 49 92 and 38', ok)


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


def rawkeys_mods_check(q):
    """10/25/26 (track A, A4): raw mode on the PS/2 keyboard: special keys with Shift, Alt and Ctrl as xterm sends them,
    F5, Shift+Tab, and Alt on a letter as ESC before it (rawkeys prints the bytes it reads, q ends it)"""
    q.send('rawkeys\n')
    ok = q.expect('q ends', 10)
    if ok:
        q.sendkeys('<ctrl-left><shift-pgup><f5><shift-tab><alt-x>q')
        ok = q.expect('1b 5b 31 3b 35 44 1b 5b 35 3b 32 7e 1b 5b 31 35 7e 1b 5b 5a 1b 78 71', 15) and q.expect(PROMPT, 10)
    return ('raw mode on the PS/2 keyboard: Ctrl+Left ESC[1;5D, Shift+PgUp ESC[5;2~, F5 ESC[15~, Shift+Tab ESC[Z, '
            'Alt+x ESC x', ok)


def editor_keys_check(q):
    """10/25/26 (track A, A5): surfsh's line editor on the PS/2 keyboard, where the console sends Alt+key as ESC and the
    key together (A4): Alt+. puts the last word of the line before at the cursor, Alt+B goes back a word and Alt+D
    deletes it, Ctrl+Left goes back a word, Ctrl+R finds an earlier line and Enter runs it"""
    steps = [('echo alpha beta\n', 'alpha beta'), ('echo <alt-.>\n', 'beta'), ('echo gamma delta<alt-b><alt-d>x\n', 'gamma x'),
             ('echo two<ctrl-left>one \n', 'one two'), ('<ctrl-r>alpha\n', 'alpha beta')]
    ok = True
    for keys, want in steps:
        q.sendkeys(keys)
        ok = ok and q.expect(f'\r\n{want}\r\n', 10) and q.expect(PROMPT, 10)
    return ('the line editor on the PS/2 keyboard: Alt+., Alt+B, Alt+D, Ctrl+Left, Ctrl+R', ok)


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


def surfsh_ctrl_c_check(q):
    """10/25/26 (scripts in surfsh): Ctrl+C in surfsh's own commands. A loop of builtins at the prompt stops with ^C,
    the prompt saying so and $? 130, and what came after it on the line does not run; a loop that runs a program, and
    one that runs a kernel command (sleep), stop as a whole, not just their round; read at the terminal ends with ^C
    and 130; a surfsh typed at the prompt lives through a Ctrl+C of its own loop, and the shell outside says nothing of
    it after its exit; a script (surfsh -c) is stopped as a whole: [pid n killed]"""
    def interrupt(line, wait, expects):
        start = len(q.transcript)
        q.send(line + '\n')
        q.wait(wait)
        q.ser.sendall(b'\x03')
        ok = all(q.expect(e, 15) for e in expects)
        return ok and b'\nnot-reached' not in q.transcript[start:]       # only in the line as typed

    def status(want):
        q.send('echo status $?\n')
        return q.expect('\r\nstatus %d\r\n' % want, 10) and q.expect(PROMPT, 10)

    results = []
    results.append(interrupt('while :; do :; done; echo not-reached', 1.0, ['^C', shown('\u224b!> ')]) and status(130))
    results.append(interrupt('for i in 1 2 3; do /initrd/bin/count 100 100; done; echo not-reached', 1.5,
                             ['^C', shown('\u224b!> ')]) and status(130))
    # 10/25/26: sleeps of 300 ms, so that the Ctrl+C a second after the line comes in the middle of one, not where
    # one ends and the next begins (with 100 ms it did, a second being ten of them, and then sometimes between two)
    results.append(interrupt('while :; do sleep 300; done; echo not-reached', 1.0, ['sleep cut short', shown('\u224b!> ')]))
    results.append(interrupt('read line; echo not-reached', 0.5, ['^C', shown('\u224b!> ')]) and status(130))
    q.send('surfsh\n')
    ok = q.expect(PROMPT, 10)
    ok = ok and interrupt('while :; do :; done; echo not-reached', 1.0, ['^C', shown('\u224b!> ')]) and status(130)
    start = len(q.transcript)
    q.send('exit\n')
    ok = ok and q.expect(PROMPT, 10)
    q.pump(0.5)
    results.append(ok and b'^C' not in q.transcript[start:])
    results.append(interrupt('surfsh -c "while :; do :; done"; echo not-reached', 1.0, ['killed]', shown('\u224b!> ')]))
    q.send('echo surfsh lives on\n')
    ok = q.expect('\r\nsurfsh lives on\r\n', 10) and q.expect(PROMPT, 10)
    return ('surfsh: Ctrl+C stops loops of builtins, programs and kernel commands, read, a script; a surfsh in a surfsh '
            'lives on (%d of %d)' % (sum(results), len(results)), all(results) and ok)

def surfsh_jobs_check(q):
    """10/25/26 (job control in surfsh): Ctrl+Z stops a program: [1] stopped, $? 148, jobs and ps say so; bg lets it go
    on, fg brings it back, Ctrl+Z again, kill %1 ends it ([1] killed before a prompt); a pipeline stops as one job; Ctrl+Z
    on a loop of builtins does nothing (Ctrl+C ends it); a program in the background that reads the terminal stops
    (reading the terminal), fg gives it the keys and Ctrl+D ends its input; cat > file ended by Ctrl+D; the stopped job
    of a surfsh that exits is killed. A program waiting for a key stops off the terminal's queue of readers (the shell's
    next line is the shell's), also when a key comes in the same burst as the Ctrl+Z; a program that a kernel command
    starts (run, in a surfsh typed at the prompt) is in no job: Ctrl+Z leaves it alone. Two surfshs inside each other:
    the inner one's job stopped, and its end, leave the middle one the terminal. Ctrl+Z leaves an until loop. 10/26/26
    (CODE-AUDIT T14): a surfsh that run started is no shell of the terminal's, so it runs without job control: head -1
    reads the terminal, nothing stops, fg says so"""
    count = '/initrd/bin/count 100 100'
    stopped = shown('\u224b!> ')

    def step(line, expects, keys=None, wait=1.0):
        start = len(q.transcript)
        if line is not None:
            q.send(line + '\n')
        if keys is not None:
            q.wait(wait)
            q.ser.sendall(keys)
        ok = all(q.expect(e, 15) for e in expects)
        return ok, q.transcript[start:]

    results = []
    ok, _ = step(count, ['[1] stopped  ' + count, stopped], b'\x1a')
    ok = ok and step('echo status $?', ['\r\nstatus 148\r\n', PROMPT])[0]
    ok = ok and step('jobs', ['[1]  stopped  ' + count, PROMPT])[0]
    ok = ok and step('ps', [re.compile(rb'stopped +\S+ +\d+ +\d+ +\d+ +count'), PROMPT])[0]
    results.append(ok)
    ok = step('bg', ['[1] ' + count + ' &', PROMPT])[0]
    ok = ok and step('fg', ['\r\n' + count + '\r\n'])[0]
    ok = ok and step(None, ['[1] stopped  ' + count, stopped], b'\x1a')[0]
    ok = ok and step('kill %1; sleep 300', ['[1] killed  ' + count, PROMPT])[0]
    results.append(ok)
    pipeline = count + ' | /initrd/bin/cat'
    ok = step(pipeline, ['[1] stopped  ' + pipeline, stopped], b'\x1a')[0]
    ok = ok and step('kill %1; sleep 300', ['[1] killed  ' + pipeline, PROMPT])[0]
    results.append(ok)
    ok, out = step('while :; do :; done', [], b'\x1a')
    q.wait(1)
    ok, out = step(None, ['^C', stopped], b'\x03', 0)
    results.append(ok and b'stopped' not in out)
    ok = step('/initrd/bin/cat &', ['[1] started (pid ', PROMPT])[0]
    ok = ok and step('sleep 500', ['[1] stopped (reading the terminal)  /initrd/bin/cat', PROMPT])[0]
    ok = ok and step('fg', ['\r\n/initrd/bin/cat\r\n'])[0]
    ok = ok and step('hello-from-fg', ['hello-from-fg', 'hello-from-fg'])[0]
    ok = ok and step(None, [shown('\u224b> ')], b'\x04', 0.5)[0]
    results.append(ok)
    ok = step('/initrd/bin/cat > /hda1/CATD.TXT', [])[0]
    q.wait(0.5)
    ok = step('line one', [])[0]
    ok = step(None, [shown('\u224b> ')], b'\x04', 0.5)[0]
    ok = ok and step('cat /hda1/CATD.TXT; rm /hda1/CATD.TXT', ['line one\r\n', PROMPT])[0]
    results.append(ok)
    ok = step('surfsh', [PROMPT])[0]
    ok = ok and step(count, ['[1] stopped  ' + count, stopped], b'\x1a')[0]
    ok = ok and step('exit; sleep 300', [PROMPT])[0]
    ok2, out = step('ps', [PROMPT])
    results.append(ok and ok2 and b' count' not in out)
    # a program waiting for a key stops off the queue of readers: the shell's next line is the shell's
    ok = step('/initrd/bin/cat', ['[1] stopped  /initrd/bin/cat', stopped], b'\x1a', 0.5)[0]
    ok = ok and step('echo shell-got-it', ['\r\nshell-got-it\r\n', PROMPT])[0]
    ok = ok and step('fg', ['\r\n/initrd/bin/cat\r\n'])[0]
    ok = ok and step('to-cat', ['to-cat', 'to-cat'])[0]
    ok = ok and step(None, [shown('\u224b> ')], b'\x04', 0.5)[0]
    results.append(ok)
    # a key and Ctrl+Z in one burst: the stop is said all the same (a key the program has not read yet stays for
    # whoever reads next, here the shell: a space)
    ok = step('/initrd/bin/cat', [], b' \x1a', 0.5)[0]
    ok = step(None, ['[1] stopped  /initrd/bin/cat', stopped])[0]
    ok = ok and step('kill %1; sleep 300', ['[1] killed  /initrd/bin/cat', PROMPT])[0]
    results.append(ok)
    # two surfshs inside each other: the inner one's job stopped, and its end, leave the middle one the terminal
    ok = step('surfsh', [PROMPT])[0]
    ok = ok and step('surfsh', [PROMPT])[0]
    ok = ok and step(count, ['[1] stopped  ' + count, stopped], b'\x1a')[0]
    ok = ok and step('exit 0', [PROMPT])[0]
    ok = ok and step('echo middle-reads', ['\r\nmiddle-reads\r\n', PROMPT])[0]
    ok = ok and step('exit', [PROMPT])[0]
    results.append(ok)
    # 10/25/26: Ctrl+Z leaves an until loop as it does a while loop, with the prompt at once (no rounds without end)
    ok = step('until false; do ' + count + '; done', ['[1] stopped  ' + count, stopped], b'\x1a')[0]
    ok = ok and step('kill %1; sleep 300', ['[1] killed  ' + count, PROMPT])[0]
    results.append(ok)
    # a program the kernel's run starts is in no job: Ctrl+Z leaves it alone, Ctrl+C ends it
    ok = step('surfsh', [PROMPT])[0]
    ok, out = step('run ' + count, [], b'\x1a')
    q.wait(0.5)
    ok2, out2 = step(None, [PROMPT], b'\x03', 0)
    ok = ok2 and b'stopped' not in out + out2
    ok = ok and step('exit', [PROMPT])[0]
    results.append(ok)
    # 10/26/26 (T14): a surfsh that the kernel's run starts for the login shell may not move jobs into the foreground:
    # it runs without job control, what it starts reads the terminal, and fg says there is none
    start = len(q.transcript)
    ok = step('run surfsh', [PROMPT])[0]
    ok = ok and step('head -1', [])[0]
    q.wait(0.5)
    ok = ok and step('typed-at-head', ['typed-at-head\r\ntyped-at-head\r\n', PROMPT])[0]
    ok = ok and step('fg', ['no job control in this shell', PROMPT])[0]
    ok = ok and step('exit 0', ['exit status 0', PROMPT])[0]
    results.append(ok and b'stopped' not in q.transcript[start:])
    return ('surfsh: job control: Ctrl+Z, jobs, bg, fg, kill %%1, a pipeline, a reader in the background, Ctrl+D '
            '(%d of %d)' % (sum(results), len(results)), all(results))

def readme_cat_check(q):
    """cat of a file of several clusters on the FAT16 disk (docs/README.md as DOCS/README.md, tools/mkimage.py), through
    the pager: every line of it arrives in order, and the step ends at the prompt after its last line. 10/6/26: the file
    shows prompts as text (`SURF ≋@>`), which PROMPT alone takes for the end: the next command was then typed while cat
    was still paging, and the pager took its first key"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'README.md')
    with open(path, encoding='utf-8') as f:
        want = [line.strip() for line in f.read().splitlines() if line.strip()]
    start = len(q.transcript)
    q.send('cat /hda1/DOCS/README.md\n')
    deadline, text, end = time.time() + 30, '', -1
    while time.time() < deadline and q.alive():
        q.pump(0.2)
        text = q.transcript[start:].decode('utf-8', 'replace').replace('\r', '')
        text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', text)
        text = re.sub(r'\n?--More-- \([^)]*\)', '', text)       # the pager's questions, answered with Space
        end = text.find(want[-1])
        if end >= 0 and PROMPT.search(text[end + len(want[-1]):].encode()):
            break
    at, ok = 0, end >= 0
    for line in want:                                   # every line, in order
        i = text.find(line, at)
        if i < 0:
            ok = False
            print(f'\n---- @readme-cat: missing line: {line[:70]!r}')
            break
        at = i + len(line)
    ok = ok and PROMPT.search(text[at:].encode()) is not None
    q.cursor = len(q.transcript)                       # the next step starts after this prompt
    return (f'cat /hda1/DOCS/README.md: all {len(want)} lines, through the pager, then the prompt', ok)


# the steps of the console's character set and the pager (10/6/26) that the host carries out
CONSOLE_CHECKS = {'@readme-cat': readme_cat_check, '@rawkeys-mods': rawkeys_mods_check, '@editor-keys': editor_keys_check, '@utf8-screen': utf8_screen_check, '@pager-space': pager_space_check,
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


def net_thread_pids(raw):
    """10/25/26: the pids ps lists for the net thread in this part of the transcript (bytes)"""
    text = re.sub(r'\x1b\[[0-9;?]*[A-Za-z]', '', raw.decode(errors='replace').replace('\r', ''))
    return re.findall(r'^ *(\d+) +\S+ +\S+ +\d+ +\d+ +-?\d+ +net$', text, re.M)


def kill_kthread_check(q):
    """10/25/26 (robustness, L15): kill refuses a kernel thread: the net thread's pid from ps, kill says what it is and
    kills nothing, and the next ps lists the thread with the same pid"""
    what = 'kill: the net thread is a kernel thread, refused, and it runs on'
    start = len(q.transcript)
    q.send('ps\n')
    ok = q.expect(PROMPT, 30)
    pids = net_thread_pids(q.transcript[start:])
    if not ok or not pids:
        return (what, False)
    start = len(q.transcript)
    q.send(f'kill {pids[-1]}\n')
    ok = q.expect(f"'net' (pid {pids[-1]}) is a kernel thread: kill ends programs and shells", 10) and q.expect(PROMPT, 10)
    ok = ok and b'killing' not in q.transcript[start:]
    start = len(q.transcript)
    q.send('ps\n')
    ok = ok and q.expect(PROMPT, 30) and net_thread_pids(q.transcript[start:]) == pids[-1:]
    return (what, ok)


CONSOLE_CHECKS['@kill-kthread'] = kill_kthread_check


def settings_disk(name):
    """10/17/26: the settings disk of that name, made (SETTINGS_FILES) the first time a boot asks for it"""
    if name not in SETTINGS_DISKS:
        import mkimage
        if not SETTINGS_DIR:
            SETTINGS_DIR.append(tempfile.mkdtemp(prefix='surfos-settings-'))
        path = os.path.join(SETTINGS_DIR[0], f'settings-{name}.img')
        mkimage.make_fat32(path, SETTINGS_FILES[name], size_mb=40, label='OWNERSTICK', cluster_bytes=512,
                           serial=SETTINGS_SERIAL)
        SETTINGS_DISKS[name] = path
    return SETTINGS_DISKS[name]


def settings_disk_check(q):
    """10/17/26: the owner's stick after the settings boots, read here with tools/mkusb.py's FAT32 reader: settings.conf
    the template with the two lines set; networks.conf the two networks in their order, the test network's key (never
    its password), the bad network kept as it was written, at the end; the owner's file as it was; no file left half
    written; fsck.fat (if the host has it) finds no error"""
    import mkusb
    path = SETTINGS_DISKS.get('owner')
    what = 'settings: the owner\'s stick holds what SurfOS saved, the key and never the password, and their own file'
    if not path:
        return (what, False)
    try:
        with open(path, 'rb') as f:
            fat = mkusb.Fat32(f, 2048 * 512)
            settings = fat.read('surfos-config/settings.conf')
            nets = fat.read('surfos-config/networks.conf')
            note = fat.read('notes.txt')
            folder = next(first for name, is_dir, first, _, _ in fat.entries(fat.root) if is_dir and name == 'surfos-config')
            names = sorted(name for name, _, _, _, _ in fat.entries(folder) if name not in ('.', '..'))
    except Exception as e:
        print(f'\n@settings-disk: {e}')
        return (what, False)
    blocks = [b for b in nets.decode().split('\n\n') if b.startswith('network = ')]
    ok = (settings.startswith(b'# SurfOS settings') and b'\ntelnetd = on\n' in settings and
          b'\nhostname = smoketest\n' in settings and b'\n#wifi = on\n' in settings and
          blocks == ['network = "SurfSimWPA"\nsecurity = wpa2\npsk = ' + SURFSIM_PSK, 'network = "SurfSim"\nsecurity = open'] and
          b'\n# SurfOS did not understand the lines below' in nets and
          nets.endswith(b'\nnetwork = "Broken"\nsecurity = wpa2\npsk = 1234\n') and
          WLANSIM_PASSPHRASE.encode() not in nets and note == OWNER_NOTE and names == ['networks.conf', 'settings.conf'])
    if not ok:
        print(f'\n@settings-disk: settings.conf {settings!r}\nnetworks.conf {nets!r}\nnotes.txt {note!r}\nfolder {names}')
    fsck = fsck_check(path, 2048 * 512, (40 << 20) - 2048 * 512)
    return (what, ok and all(r for _, r in fsck))


CONSOLE_CHECKS['@settings-disk'] = settings_disk_check


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


def alt_screen_check(q):
    """10/25/26 (track A, A8): the alternate screen (ESC[?1049h and l) in VGA memory: `pagetest alt` asks its question
    on a blank screen, and the shell's screen comes back after it with the cursor where it was; a program killed there
    (Ctrl+C at its question) gets the main screen back from the console, which tells the serial terminal too"""
    def rows():
        q.wait(0.3)
        d = q.vga_memory() or bytes(4000)
        return [cells_text(d[r * 160:(r + 1) * 160:2]).rstrip() for r in range(25)]

    def alt_gone(screen):
        return not any('ALT-SCREEN' in r or 'alternate question' in r for r in screen)
    q.send('clear\n')
    ok = q.expect(PROMPT, 10)
    q.send('echo main screen kept\n')
    ok = ok and q.expect('main screen kept\r\n', 10) and q.expect(PROMPT, 10)
    q.send('run pagetest alt\n')
    ok = ok and q.expect('alternate question: ', 10)
    alt = rows()
    ok = ok and alt[0] == 'PAGETEST ALT-SCREEN' and alt[1] == 'alternate question:' and not any(alt[2:])
    q.send('yes\n')
    ok = ok and q.expect('PAGETEST MAIN SCREEN: yes', 10) and q.expect(PROMPT, 10)
    main = rows()
    ok = ok and main[1] == 'main screen kept' and main[2].endswith('run pagetest alt') and \
        main[3] == 'PAGETEST MAIN SCREEN: yes' and main[4].endswith('exit status 0]') and alt_gone(main)
    q.send('run pagetest alt\n')
    ok = ok and q.expect('alternate question: ', 10)
    start = len(q.transcript)
    q.ser.sendall(b'\x03')
    ok = ok and q.expect('killed]', 10) and q.expect(PROMPT, 10) and b'\x1b[?1049l' in q.transcript[start:]
    killed = rows()
    ok = ok and killed[:5] == main[:5] and killed[5].endswith('run pagetest alt') and alt_gone(killed)
    return ('the alternate screen: a program\'s question on a blank screen, the shell\'s screen back after it, and back '
            'from the console when the program is killed there (the serial terminal told)', ok)


CONSOLE_CHECKS['@alt-screen'] = alt_screen_check


def less_check(q):
    """10/25/26 (track A, A9): less on the alternate screen, its text from a pipe and its keys from /dev/tty: the first
    screenful and where it is, Space, a search (the line found on top, its match in inverse video), G and (END), and q
    giving the shell's screen back as it was; a file that fits on the screen, at its end at once; through a pipe, a
    copy as cat makes"""
    def rows():
        q.wait(0.5)
        d = q.vga_memory() or bytes(4000)
        return d, [cells_text(d[r * 160:(r + 1) * 160:2]).rstrip() for r in range(25)]
    q.send('clear\n')
    ok = q.expect(PROMPT, 10)
    q.send('echo before less\n')
    ok = ok and q.expect('before less\r\n', 10) and q.expect(PROMPT, 10)
    q.send('seq 1 200 | less\n')
    ok = ok and q.expect('lines 1-24 of 200', 15)
    _, shown = rows()
    ok = ok and shown[0] == '1' and shown[23] == '24' and shown[24] == 'standard input  lines 1-24 of 200  12%  h: keys'
    q.ser.sendall(b' ')
    ok = ok and q.expect('lines 25-48 of 200', 10)
    q.ser.sendall(b'/150\r')
    ok = ok and q.expect('lines 150-173 of 200', 10)
    d, shown = rows()
    ok = ok and shown[0] == '150' and [d[c * 2 + 1] for c in range(4)] == [0x70, 0x70, 0x70, 0x07] and d[161] == 0x07
    q.ser.sendall(b'G')
    ok = ok and q.expect('lines 177-200 of 200  (END)', 10)
    q.ser.sendall(b'q')
    ok = ok and q.expect(PROMPT, 10)
    _, shown = rows()
    ok = ok and shown[1] == 'before less' and shown[2].endswith('seq 1 200 | less') and \
        not any('standard input' in r for r in shown)
    q.send('less /initrd/etc/ssl/README\n')
    ok = ok and q.expect('/initrd/etc/ssl/README  lines 1-23 of 23  (END)', 15)
    q.ser.sendall(b'q')
    ok = ok and q.expect(PROMPT, 10)
    q.send('pagetest utf8 | less\n')                     # UTF-8 as the console's characters, one cell each
    ok = ok and q.expect('lines 1-1 of 1  (END)', 15)
    _, shown = rows()
    ok = ok and shown[0] == UTF8_SAMPLE
    q.ser.sendall(b'q')
    ok = ok and q.expect(PROMPT, 10)
    q.send("printf 'a\\tb\\001c\\n' | less\n")             # a TAB to column 8, ^A in inverse video
    ok = ok and q.expect('lines 1-1 of 1  (END)', 15)
    d, shown = rows()
    ok = ok and shown[0] == 'a       b^Ac' and [d[c * 2 + 1] for c in range(8, 12)] == [0x07, 0x70, 0x70, 0x07]
    q.ser.sendall(b'q')
    ok = ok and q.expect(PROMPT, 10)
    q.send('echo through less | less | cat\n')
    ok = ok and q.expect('through less\r\n', 10) and q.expect(PROMPT, 10)
    return ('less: a pipeline\'s text with the keys from the terminal, Space, a search shown in inverse video, G, and q '
            'giving the shell\'s screen back; a file that fits; UTF-8, a TAB and ^A; a copy into a pipe', ok)


CONSOLE_CHECKS['@less'] = less_check
CONSOLE_CHECKS['@surfsh-ctrl-c'] = surfsh_ctrl_c_check    # 10/25/26
CONSOLE_CHECKS['@surfsh-jobs'] = surfsh_jobs_check        # 10/25/26


# ---- 10/6/26: telnetd, the telnet server ------------------------------------------------------------------------
TN_IAC, TN_DONT, TN_DO, TN_WONT, TN_WILL, TN_SB, TN_SE = 255, 254, 253, 252, 251, 250, 240
TO_BINARY, TO_ECHO, TO_SGA, TO_NAWS = 0, 1, 3, 31


class Telnet:
    """A telnet client for the tests that answers as a terminal's client does (RFC 854, 1143): it lets the
    server echo and suppress go-ahead (DO), suppresses go-ahead itself and tells its window size when asked
    (WILL, then NAWS), and refuses anything else once. What the session printed is kept in self.data with
    the IAC sequences taken out (IAC IAC is the byte 255, CR NUL a CR); self.raw has everything as it came.
    Return is sent as CR NUL, as telnet clients send it; --More-- is answered with Space unless auto_more is off.
    With binary, the client agrees to send in binary mode (WILL BINARY), as Apple's telnet does, and then sends
    Return as a CR alone."""

    def __init__(self, port, cols=80, rows=24, host='127.0.0.1', timeout=10, rcvbuf=None, binary=False):
        self.sock = self._connect(host, port, timeout, rcvbuf)
        self.binary = binary
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
            ours = (TO_SGA, TO_NAWS) + ((TO_BINARY,) if self.binary else ())
            wanted = opt in ((TO_ECHO, TO_SGA) if side == 'him' else ours)
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
            enter = b'\r' if ('us', TO_BINARY) in self.agreed else b'\r\0'     # binary mode: a CR alone
            keys = keys.encode().replace(b'\xff', b'\xff\xff').replace(b'\n', enter)
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


def memstat_free(raw):
    """10/25/26: (free, total) pages from each memstat in this part of the transcript (bytes); 10/25/26 (the review of
    B1): free less those promised to the kernel heap (memstat's Reserve line), which the heap may take any time"""
    free = re.findall(rb'Free physical pages: (\d+) of (\d+)', raw)
    promised = re.findall(rb'; (\d+) promised to the heap;', raw)
    if len(promised) != len(free):
        return []
    return [(int(a) - int(p), int(b)) for (a, b), p in zip(free, promised)]


# 10/25/26 (the review of B1): surfsh's own prompt on the console, after a command that worked or one that failed;
# never a recovery console's (?) or the ring-0 shell's (#)
SURFSH_PROMPT = re.compile(rb' \xe2\x89\x8b' + _SGR + rb'!?' + _SGR + rb'>')


def memhog_hold_check(q):
    """10/25/26 (robustness, L1-L9): memhog holds all the memory a program may have for 3 s: meanwhile a program
    either runs or says it is out of memory, and ps (a kernel command) works; once memhog has given it back, a telnet
    login works (telnetd on came before) and memstat's free pages are back within 1%"""
    what = 'memory: while memhog holds it all a program runs or is refused and ps works; then telnet, free pages back'
    start = len(q.transcript)
    q.send('memstat\n')
    ok = q.expect(PROMPT, 30)
    before = memstat_free(q.transcript[start:])
    held = len(q.transcript)
    q.send('memhog hold 3000 &\n')
    ok = ok and q.expect('started (pid', 20) and q.expect('KB taken; holding them 3000 ms', 60)
    q.send('/initrd/bin/ls /initrd\n')
    # (11/1/26: or it starts and then cannot load: "out of memory for a segment", "for the stack")
    ok = ok and q.expect(re.compile(rb'motd|surfsh: /initrd/bin/ls: Out of memory|/initrd/bin/ls: out of memory for '), 20) \
        and q.expect(SURFSH_PROMPT, 20)
    q.send('ps\n')
    ok = ok and q.expect('Kernel Idle Task', 20) and q.expect(SURFSH_PROMPT, 20)
    ok = ok and q.expect('memhog hold: given back', 60)
    if b'init:' in q.transcript[held:]:                  # the shells lived on: init started none, said nothing
        print('  memhog hold: init spoke while memory was held')
        ok = False
    if ok:
        t = Telnet(q.ports['telnet'])
        try:
            ok = t.expect("Brandon Burr's SurfOS", 15) and t.expect(TELNET_ROOT, 10)
            if ok:
                t.send('exit\n')
                ok = t.wait_closed(10)
        finally:
            t.close()
    start = len(q.transcript)
    q.send('memstat\n')
    ok = q.expect(PROMPT, 30) and ok
    after = memstat_free(q.transcript[start:])
    ok = ok and len(before) == 1 and len(after) == 1 and abs(after[0][0] - before[0][0]) * 100 <= before[0][1]
    if not ok:
        print(f'  memhog hold: free pages before {before}, after {after}')
    return (what, ok)


CONSOLE_CHECKS['@memhog-hold'] = memhog_hold_check


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
    stays full (netstat: out 8192), and the server holds that session back (and keeps it: a stalled session is ended
    only when a new connection needs its place) and serves a second session all the same. When the first client
    reads again its session goes on: the hexdump is killed from the console, and an Enter in the session brings the
    report of it and a prompt; every line arrived, in order. Then the client goes."""
    a, b = Telnet(q.ports['telnet'], rcvbuf=4096), None
    full = re.compile(rb'local 23\s+remote [0-9.]+:[0-9]+\s+rx\s+[0-9]+\s+tx\s+[0-9]+\s+retrans\s+[0-9]+\s+'
                      rb'buf\s+in\s+[0-9]+\s+out\s+8192')
    line = re.compile(rb'([0-9a-f]{8})  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  \|\.{16}\|\r\n')
    why = ''
    try:
        ok = a.expect(TELNET_ROOT, 15)
        if ok:
            a.send('/initrd/bin/hexdump /dev/zero &\n')
            ok = a.expect(re.compile(rb'\] started \(pid (\d+)\)'), 10)     # a reads nothing more from here on
            m = re.search(rb'\] started \(pid (\d+)\)', a.data)     # 10/24/26: "[1] started (pid 42)"
            pid = int(m.group(1)) if m else 0
            # 10/26/26: said as what it is: the shell's line about the job did not come, and nothing below was tried
            # (it was "the send buffer never filled": the shell stopped at --More-- behind the job's rows, until
            # kernel/console.c's page_row())
            why = '' if ok else " (the job's start line never came within 10 s)"
        # 10/25/26: 120 s, was 45: before the guest's buffer fills, hexdump fills QEMU's and the host's socket buffers
        # through a slow guest, and on a loaded machine (two suites at once, builds) that took longer than 45 s
        stalled, deadline = False, time.time() + 120    # seconds here; the server ends no session for a stall
        while ok and not stalled and time.time() < deadline:    # unless a new connection needs its place
            start = len(q.transcript)
            q.send('netstat\n')
            ok = q.expect(PROMPT, 10)
            stalled = full.search(q.transcript[start:]) is not None
            if not stalled:
                q.wait(1)
        why = why or ('' if stalled else ' (the send buffer never filled)')
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
            # every line, in order: the job's lines can come while surfsh still writes "[1] started (pid n)" and its
            # prompt, which then cut a line or three of them; past those, no line may be missing. 10/25/26: wherever
            # they are (& is a job of surfsh's now, which starts at once, and the shell's line waits for room in the
            # session's output behind the job's): a gap is allowed only where the shell's own text is between the lines
            found = list(line.finditer(a.data))
            offsets = [int(m.group(1), 16) for m in found]
            gaps = [(i, offsets[i + 1] - offsets[i]) for i in range(len(offsets) - 1) if offsets[i + 1] != offsets[i] + 16]
            shell = [i for i, _ in gaps
                     if re.search(rb'started \(pid [0-9]+\)|\xe2\x89\x8b', a.data[found[i].end():found[i + 1].start()])]
            ok = ok and len(offsets) > 2000 and offsets[0] < 0x400 and all(i in shell and 16 < d <= 64 for i, d in gaps)
            # 10/26/26: and what came between the two lines of each of the first gaps: nothing (a line lost whole),
            # part of a line (a piece of the stream lost: the next line is found inside it), or a line with something
            # else in it (the regex finds the line after). A one-line gap was seen twice on 10/25/26, cause unknown
            between = [bytes(a.data[found[i].end():found[i + 1].start()][:120]) for i, _ in gaps[:3]]
            why = why or ('' if ok else ' (%d lines, gaps %s, between their lines %s)' % (len(offsets), gaps[:3], between))
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
    still gets through at once, ends the sleep, and drops what was typed ahead: its next command runs as typed. Then
    400 cursor keys (ESC [ A) fill the queue, and a Ctrl+C after them still ends another sleep"""
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
        if ok:                                      # cursor keys fill the queue, the rest waits: Ctrl+C still ends it
            a.send('sleep 100000\n')
            ok = a.expect('sleep 100000', 10)
        if ok:
            time.sleep(0.5)
            a._out(b'\x1b[A' * 400)
            time.sleep(1.5)
            a.cursor = len(a.data)
            a._out(b'\x03')
            ok = a.expect(PROMPT, 15)
            why = why or ('' if ok else ' (a Ctrl+C after cursor keys did not end the sleep)')
        if ok:
            a.send('echo after-arrows\n')
            ok = a.expect('\r\nafter-arrows\r\n', 10) and a.expect(TELNET_ROOT, 10)
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


# a BSD telnet's first bytes: DO SGA, WILL TERMINAL-TYPE, NAWS, TERMINAL-SPEED, TOGGLE-FLOW-CONTROL, LINEMODE,
# NEW-ENVIRON, DO STATUS
TN_BSD_OPENING = bytes([255, 253, 3, 255, 251, 24, 255, 251, 31, 255, 251, 32, 255, 251, 33, 255, 251, 34, 255, 251, 39,
                        255, 253, 5])


def telnet_server_pid(q):
    """the running server's pid, as telnetd status on the console says it"""
    start = len(q.transcript)
    q.send('telnetd status\n')
    q.expect(PROMPT, 10)
    m = re.search(r'running \(pid (\d+)\)', q.transcript[start:].decode(errors='replace'))
    return int(m.group(1)) if m else None


def telnet_zombies(q, server):
    """the pids of the shells that are zombies under the server, as ps on the console lists them"""
    start = len(q.transcript)
    q.send('ps\n')
    q.expect(PROMPT, 10)
    text = q.transcript[start:].decode(errors='replace')
    return re.findall(r'(?m)^\s*(\d+)\s+zombie\s+\S+\s+\d+\s+\d+\s+%d\s+surfsh' % server, text)


def telnet_protocol_check(q):
    """one session for the rest of the protocol: a BSD telnet's opening (each option it offers that the server does not
    do is refused once), the window size the client tells (NAWS 100x30) as a program sees it, AYT answered, DO
    TIMING-MARK refused, Enter as CR LF, DEL and BS, IAC EC and IAC EL, UTF-8 typed, IAC IP on a running count, the byte
    255 in a program's output (cat of a program file, in the background) as IAC IAC; then the client resets the
    connection: the session ends, and its shell is collected at once (no zombie under the server)"""
    import struct
    server = telnet_server_pid(q)
    t = Telnet(q.ports['telnet'], cols=100, rows=30)
    ok = server is not None
    try:
        t._out(TN_BSD_OPENING)
        ok = ok and t.expect(TELNET_ROOT, 15)
        refusals = [bytes([255, 254, o]) for o in (24, 32, 33, 34, 39)] + [bytes([255, 252, 5])]
        ok = ok and all(t.raw.count(r) == 1 for r in refusals)
        steps = [('pagetest size\n', ['TTY_GETSIZE 100x30', TELNET_ROOT]),
                 (b'\xff\xf6', ['[SurfOS: yes]']),
                 (b'echo crlf-enter\r\n', ['\r\ncrlf-enter\r\n', TELNET_ROOT]),
                 (b'echo abc\x7f\x7fd\r\0', ['\r\nad\r\n', TELNET_ROOT]),
                 (b'echo xyz\x08\x08w\r\0', ['\r\nxw\r\n', TELNET_ROOT]),
                 (b'echo pqX\xff\xf7r\r\0', ['\r\npqr\r\n', TELNET_ROOT]),
                 (b'not this line\xff\xf8echo el-ok\r\0', ['\r\nel-ok\r\n', TELNET_ROOT]),
                 ('echo café\n', ['\r\ncafé\r\n', TELNET_ROOT]),
                 ('count 100 100\n', [' 5 ']),
                 (b'\xff\xf4', ['killed]', TELNET_ROOT])]
        for keys, expects in steps:
            if not ok:
                break
            t.send(keys)
            ok = all(t.expect(e, 20) for e in expects)
        if ok:                                      # DO TIMING-MARK: WONT
            mark = len(t.raw)
            t._out(b'\xff\xfd\x06')
            deadline = time.time() + 10
            while b'\xff\xfc\x06' not in t.raw[mark:] and not t.closed and time.time() < deadline:
                t.pump(0.1)
            ok = b'\xff\xfc\x06' in t.raw[mark:]
        if ok:                                      # 255 in the output: cat of a program, then a prompt says done
            mark = len(t.data)
            t.send('/initrd/bin/cat /initrd/bin/hello &\n')
            ok = t.expect('] started', 10)
            deadline = time.time() + 60
            while ok and b'] done' not in t.data[mark:] and time.time() < deadline:
                t.send('\n')
                t.expect(TELNET_ROOT, 10)
                t.pump(0.5)
            ok = ok and b'] done' in t.data[mark:] and t.data[mark:].count(b'\xff') >= 100
        if ok:                                      # a reset: the session ends, its shell is collected
            t.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            t.close()
            ok = telnet_no_sessions(q)
            deadline, zombies = time.time() + 5, ['?']
            while zombies and time.time() < deadline:
                zombies = telnet_zombies(q, server)
                if zombies:
                    q.wait(0.5)
            ok = ok and not zombies
    finally:
        t.close()
    return ('telnet: a BSD opening, NAWS 100x30, AYT, TIMING-MARK, CR LF, DEL, BS, EC, EL, UTF-8, IP, 255 in output, '
            'a reset; no zombie after', ok)


def telnet_binary_check(q):
    """a client that agrees to send in binary mode (DO BINARY is in the server's opening: Apple's telnet otherwise
    strips the eighth bit of what is typed) and then sends Return as a CR alone: UTF-8 typed arrives whole, the
    command runs"""
    t = Telnet(q.ports['telnet'], binary=True)
    try:
        ok = t.expect(TELNET_ROOT, 15) and bytes([TN_IAC, TN_DO, TO_BINARY]) in t.raw and ('us', TO_BINARY) in t.agreed
        if ok:
            t.send('echo caf\u00e9-ok\n')
            ok = t.expect('\r\ncaf\u00e9-ok\r\n', 10) and t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet: a client in binary mode: UTF-8 typed, and Return as a CR alone, run the command', ok)


def telnet_quiet(t, quiet, limit):
    """read until the session has printed nothing for quiet seconds (at most limit); True if it did go quiet"""
    deadline, since = time.time() + limit, time.time()
    while time.time() < deadline and not t.closed:
        n = len(t.data)
        t.pump(0.1)
        if len(t.data) != n:
            since = time.time()
        elif time.time() - since >= quiet:
            return True
    return False


def telnet_paste_check(q):
    """a paste of 60 lines, 3000 bytes in one go, into a program that reads every key (edit, in raw mode): while the
    editor takes keys the server reads the client only as far as its key buffer has room, so TCP holds the rest at
    the client and nothing is dropped; saved, the file has the 60 lines"""
    t = Telnet(q.ports['telnet'], rows=1000)
    lines = ['line %02d ' % i + 'x' * 41 for i in range(60)]
    try:
        ok = t.expect(TELNET_ROOT, 15)
        if ok:
            t.send('/initrd/bin/rm /hda1/PASTE.TXT\n')    # from a run before, if there was one
            ok = t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('/initrd/bin/edit /hda1/PASTE.TXT\n')
            ok = telnet_quiet(t, 1.0, 20)               # the editor's screen
        if ok:
            t._out(''.join(line + '\r' for line in lines).encode())
            ok = telnet_quiet(t, 2.0, 120)              # every key taken and drawn
        if ok:
            t._out(b'\x13')                             # ^S: save
            telnet_quiet(t, 1.0, 20)
            t._out(b'\x11')                             # ^Q: quit
            ok = t.expect(TELNET_ROOT, 20)
        if ok:
            mark = len(t.data)
            t.send('/initrd/bin/cat /hda1/PASTE.TXT\n')
            ok = t.expect(TELNET_ROOT, 20)
            got = [int(x) for x in re.findall(rb'line (\d\d) x{41}\r\n', t.data[mark:])]
            ok = ok and got == list(range(60))
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet: a paste of 60 lines (3000 bytes) into edit arrives whole', ok)


def telnet_ctrlc_keys_check(q):
    """keys and a Ctrl+C in one segment: those before the Ctrl+C are not lost. At the prompt, "echo RAWKEYS-1",
    Return and a Ctrl+C sent together run the command; in edit, where ^C is a key, "hello" and ^C sent together leave
    hello in the file"""
    t = Telnet(q.ports['telnet'], rows=1000)
    try:
        ok = t.expect(TELNET_ROOT, 15)
        if ok:
            t._out(b'echo RAWKEYS-1\r\0\x03')
            ok = t.expect('\r\nRAWKEYS-1\r\n', 10) and t.expect(TELNET_ROOT, 10)
            telnet_quiet(t, 0.5, 10)                    # the ^C after the Enter was the rest of a "paste": the
        if ok:                                          # terminal skips what comes until a pause (TTY_SKIP)
            t.send('/initrd/bin/rm /hda1/CTRLC.TXT\n')     # from a run before, if there was one
            ok = t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('/initrd/bin/edit /hda1/CTRLC.TXT\n')
            ok = telnet_quiet(t, 1.0, 20)
        if ok:
            t._out(b'hello\x03')                        # ^C in edit: where the cursor is
            telnet_quiet(t, 1.0, 20)
            t._out(b'\x13')                             # ^S: save
            telnet_quiet(t, 1.0, 20)
            t._out(b'\x11')                             # ^Q: quit
            ok = t.expect(TELNET_ROOT, 20)
        if ok:
            t.send('/initrd/bin/cat /hda1/CTRLC.TXT\n')
            ok = t.expect('\r\nhello\r\n', 10) and t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet: keys sent with a Ctrl+C in one segment are kept: a command at the prompt, text in edit', ok)


def surfsh_crlf_check(q):
    """Return as CR LF on the serial line, as a serial terminal sends it: the LF right after the CR is part of the
    Enter, not the rest of a paste, so it starts no skip and a command typed at once after the prompt runs"""
    q.ser.sendall(b'echo CRLF-A\r\n')
    ok = q.expect('CRLF-A\r\n', 10) and q.expect(PROMPT, 10)
    q.ser.sendall(b'echo CRLF-B\r\n')
    ok = q.expect('CRLF-B\r\n', 10) and q.expect(PROMPT, 10) and ok
    return ('surfsh: Return as CR LF on the serial line starts no skip: the next command, typed at once, runs', ok)


def telnet_paste_lines_check(q):
    """a pasted block of 40 lines at the prompt runs its first line and nothing else (surfsh drops the rest of what it
    read, the terminal skips the rest of the paste); a command typed after a pause runs"""
    t = Telnet(q.ports['telnet'], rows=1000)
    try:
        ok = t.expect(TELNET_ROOT, 15)
        if ok:
            mark = len(t.data)
            t._out(b'echo ONE\r' + b''.join(b'echo LINE%02d\r' % i for i in range(2, 41)))
            ok = t.expect('\r\nONE\r\n', 10) and t.expect(TELNET_ROOT, 10)
            telnet_quiet(t, 1.0, 10)
            ok = ok and b'not found' not in t.data[mark:] and b'\r\nLINE' not in t.data[mark:]
        if ok:
            t.send('echo TWO\n')
            ok = t.expect('\r\nTWO\r\n', 10) and t.expect(TELNET_ROOT, 10)
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet: a pasted block of 40 lines runs its first line only; a command after a pause runs', ok)


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


def telnet_win_check(q):
    """10/25/26 (windows for programs): wintest nodesk in a telnet session, with no desktop running: refused
    (ENODEV) as on a console where none runs (a nodesk check; with a desktop running, tools/gui_apps_win.py)"""
    t = Telnet(q.ports['telnet'])
    try:
        ok = t.expect(TELNET_ROOT, 15)
        if ok:
            t.send('wintest nodesk\n')
            ok = t.expect('ENODEV: a good window', 20) and t.expect('WINTEST nodesk PASS', 20) and \
                t.expect(TELNET_ROOT, 20)
        if ok:
            t.send('exit\n')
            ok = t.wait_closed(10)
    finally:
        t.close()
    return ('telnet, no desktop running: wintest nodesk in a session is refused (ENODEV)', ok)


CONSOLE_CHECKS.update({'@telnet-session': telnet_session_check, '@telnet-two': telnet_two_check,
                       '@telnet-full': telnet_full_check, '@telnet-stalled': telnet_stalled_check,
                       '@telnet-flood': telnet_flood_check, '@telnet-paste': telnet_paste_check,
                       '@telnet-ctrlc-keys': telnet_ctrlc_keys_check,
                       '@telnet-paste-lines': telnet_paste_lines_check, '@surfsh-crlf': surfsh_crlf_check,
                       '@telnet-protocol': telnet_protocol_check,
                       '@telnet-binary': telnet_binary_check,
                       '@telnet-off-inside': telnet_off_inside_check, '@telnet-port-busy': telnet_port_busy_check,
                       '@telnet-win': telnet_win_check})


# 10/17/26: httpd, the web server (user/httpd/), from this host through the forwarded ports 80 (q.ports['www']) and
# 8080 (q.ports['www2']). The files it should send are read from the very initrd this QEMU has.
def initrd_file(q, name):
    """the bytes of the initrd's file name (www/index.html), as the guest has them"""
    with tarfile.open(os.path.join(q.workdir, 'initrd.tar')) as t:
        return t.extractfile(name).read()


_WWW_PACE = threading.Lock()
_WWW_LAST = [0.0]


def www_connect(port, timeout=10, rcvbuf=None):
    """a connection to a forwarded guest port, at least 50 ms after the one before. QEMU's user network takes the
    connections to a forwarded port one at a time: of two made in the same instant it resets one on this host
    before the guest sees it (10/17/26: every time, whatever the guest's backlog), so every check connects here;
    tools/qemu-tcp.py's backlog scenario tests the guest's own backlog"""
    with _WWW_PACE:
        wait = _WWW_LAST[0] + 0.05 - time.time()
        if wait > 0:
            time.sleep(wait)
        s = socket.socket()
        try:
            if rcvbuf:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            s.settimeout(timeout)
            s.connect(('127.0.0.1', port))
        except OSError:
            s.close()
            raise
        finally:
            _WWW_LAST[0] = time.time()
        return s


def www_get(port, path, method='GET', headers=None, timeout=10):
    """one request on a connection of its own (http.client): status, header fields (names in lower case), body"""
    c = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        c.sock = www_connect(port, timeout)
        c.request(method, path, headers=headers or {})
        r = c.getresponse()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, r.read()
    finally:
        c.close()


def www_raw(port, data, timeout=5):
    """data sent as it is on a connection of its own; what comes back until the server closes (None if it does not
    close within timeout, with what came in www_raw.got)"""
    s = www_connect(port, timeout)
    got, deadline = b'', time.time() + timeout
    try:
        s.sendall(data)
        while time.time() < deadline:
            try:
                d = s.recv(65536)
            except socket.timeout:
                break
            if not d:
                return got
            got += d
        www_raw.got = got
        return None
    finally:
        s.close()


def www_responses(data):
    """the responses one behind the other in data: [(status, header fields, body)] (no HEAD: bodies by Content-Length)"""
    out = []
    while data:
        head, sep, rest = data.partition(b'\r\n\r\n')
        if not sep:
            break
        lines = head.decode('latin-1').split('\r\n')
        fields = {}
        for l in lines[1:]:
            k, _, v = l.partition(':')
            fields[k.strip().lower()] = v.strip()
        n = int(fields.get('content-length', '0'))
        out.append((int(lines[0].split()[1]), fields, rest[:n]))
        data = rest[n:]
    return out


def www_recv(s, timeout):
    """what comes on s within timeout, until it closes: (bytes, closed)"""
    got, deadline = b'', time.time() + timeout
    while time.time() < deadline:
        s.settimeout(max(deadline - time.time(), 0.05))
        try:
            d = s.recv(65536)
        except socket.timeout:
            break
        except OSError:
            return got, True
        if not d:
            return got, True
        got += d
    return got, False


def httpd_site_check(q):
    """the sample site (httpd on: /initrd/www) from this host: the page, its style sheet and picture, byte for byte
    with their types, the server's fields, the query ignored; HEAD; If-None-Match with the ETag (304); a name that
    is not there (a 404 page; hidden names are @httpd-serve's, on the FAT disk)"""
    port, results = q.ports['www'], []
    try:
        page, css, svg = (initrd_file(q, 'www/' + n) for n in ('index.html', 'style.css', 'wave.svg'))
        st, h, body = www_get(port, '/')
        results.append(st == 200 and body == page and h.get('content-type') == 'text/html; charset=utf-8' and
                       h.get('server') == 'SurfOS-httpd/1.0' and h.get('cache-control') == 'no-cache' and
                       h.get('connection') == 'keep-alive' and h.get('content-length') == str(len(page)) and
                       b'This page came from SurfOS.' in body)
        tag = h.get('etag', '')
        st, h, body = www_get(port, '/style.css')
        results.append(st == 200 and body == css and h.get('content-type') == 'text/css; charset=utf-8')
        st, h, body = www_get(port, '/wave.svg')
        results.append(st == 200 and body == svg and h.get('content-type') == 'image/svg+xml')
        st, h, body = www_get(port, '/index.html?from=smoke#top')
        results.append(st == 200 and body == page and h.get('etag') == tag)
        st, h, body = www_get(port, '/', 'HEAD')
        results.append(st == 200 and body == b'' and h.get('content-length') == str(len(page)) and h.get('etag') == tag)
        st, h, body = www_get(port, '/', headers={'If-None-Match': tag})
        results.append(re.fullmatch(r'"[0-9a-f]+-[0-9a-f]+"', tag) is not None and st == 304 and body == b'' and
                       h.get('etag') == tag)
        st, h, body = www_get(port, '/', headers={'If-None-Match': '"0-0"'})
        results.append(st == 200 and body == page)
        st, h, body = www_get(port, '/nothing-here.html')
        results.append(st == 404 and b'404 Not Found' in body and h.get('content-type') == 'text/html; charset=utf-8')
    except (OSError, http.client.HTTPException, KeyError) as e:
        print(f'  httpd site: {e!r}')
        results.append(False)
    return ('httpd: the sample site: page, style sheet, picture, HEAD, 304, 404s (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def httpd_protocol_check(q):
    """what httpd refuses, each on a connection of its own that it then closes: no Host in HTTP/1.1, .. plain and
    encoded, POST with a body (405 and Allow), HTTP/2.0 (505), a target too long (414), a header too large (431);
    and what it takes: the absolute form, HTTP/1.0 (answered, then closed)"""
    port, results = q.ports['www'], []

    def refused(data, status, *needles):
        got = www_raw(port, data)
        r = www_responses(got or b'')
        ok = got is not None and len(r) == 1 and r[0][0] == status and r[0][1].get('connection') == 'close' and \
            all(n in got for n in needles)
        if not ok:
            print(f'  httpd protocol: {data[:40]!r}: {got[:200] if got else got!r}')
        return ok
    try:
        results.append(refused(b'GET / HTTP/1.1\r\n\r\n', 400))
        results.append(refused(b'GET /../etc/version HTTP/1.1\r\nHost: s\r\n\r\n', 400))
        results.append(refused(b'GET /%2e%2e/%2e%2e/etc/version HTTP/1.1\r\nHost: s\r\n\r\n', 400))
        results.append(refused(b'POST / HTTP/1.1\r\nHost: s\r\nContent-Length: 5\r\n\r\nhello', 405, b'Allow: GET, HEAD\r\n'))
        results.append(refused(b'GET / HTTP/2.0\r\nHost: s\r\n\r\n', 505))
        results.append(refused(b'GET /' + b'a' * 5000 + b' HTTP/1.1\r\nHost: s\r\n\r\n', 414))
        results.append(refused(b'GET / HTTP/1.1\r\nHost: s\r\nX-Big: ' + b'b' * 5000 + b'\r\n\r\n', 431))
        got = www_raw(port, b'GET http://anything/style.css HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n')
        r = www_responses(got or b'')
        results.append(len(r) == 1 and r[0][0] == 200 and r[0][2] == initrd_file(q, 'www/style.css'))
        got = www_raw(port, b'GET / HTTP/1.0\r\n\r\n')
        r = www_responses(got or b'')
        results.append(got is not None and len(r) == 1 and r[0][0] == 200 and r[0][1].get('connection') == 'close')
    except OSError as e:
        print(f'  httpd protocol: {e!r}')
        results.append(False)
    return ('httpd: refusals (400 400 400 405 505 414 431), the absolute form, HTTP/1.0 (%d of %d)' %
            (sum(results), len(results)), all(results))


def httpd_keepalive_check(q):
    """one connection for several requests (keep-alive), three requests sent at once (pipelining) answered in order,
    a 404 that keeps the connection, Connection: close that ends it, and a request with a body: answered, then
    closed, what came behind it never answered"""
    port, results = q.ports['www'], []
    try:
        c = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        c.sock = www_connect(port)
        statuses, socks = [], set()
        for path in ('/', '/style.css', '/nothing', '/wave.svg'):
            c.request('GET', path)
            r = c.getresponse()
            r.read()
            statuses.append(r.status)
            socks.add(c.sock.getsockname() if c.sock else None)     # http.client drops a connection that closes
        c.close()
        results.append(statuses == [200, 200, 404, 200] and len(socks) == 1 and None not in socks)
        s = www_connect(port)
        try:
            s.sendall(b'GET / HTTP/1.1\r\nHost: s\r\n\r\nGET /style.css HTTP/1.1\r\nHost: s\r\n\r\n'
                      b'GET /missing HTTP/1.1\r\nHost: s\r\n\r\n')
            got, closed = www_recv(s, 3)
            r = www_responses(got)
            results.append(not closed and [x[0] for x in r] == [200, 200, 404] and
                           r[1][2] == initrd_file(q, 'www/style.css'))
            s.sendall(b'GET /wave.svg HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n')
            got, closed = www_recv(s, 5)
            r = www_responses(got)
            results.append(closed and [x[0] for x in r] == [200] and r[0][1].get('connection') == 'close')
        finally:
            s.close()
        got = www_raw(port, b'GET /style.css HTTP/1.1\r\nHost: s\r\nContent-Length: 3\r\n\r\nabc'
                            b'GET / HTTP/1.1\r\nHost: s\r\n\r\n')
        r = www_responses(got or b'')
        results.append(got is not None and [x[0] for x in r] == [200] and r[0][1].get('connection') == 'close')
    except (OSError, http.client.HTTPException) as e:
        print(f'  httpd keep-alive: {e!r}')
        results.append(False)
    return ('httpd: keep-alive, pipelining, close, a request with a body (%d of %d)' % (sum(results), len(results)),
            all(results))


def httpd_places_check(q):
    """six idle connections fill every place: a seventh is served all the same (the one idle longest makes room);
    six requests begun but not finished: a seventh waits until one of them is finished and answered; eight clients
    side by side, three pages each on connections of their own, all answered whole (connections 50 ms apart at
    least: www_connect()); six connections just made keep their places from a seventh"""
    port, results, socks = q.ports['www'], [], []
    css = initrd_file(q, 'www/style.css')
    try:
        for _ in range(6):
            socks.append(www_connect(port))
            time.sleep(0.15)                # an order of idleness
        time.sleep(0.5)
        st, _, body = www_get(port, '/style.css')
        results.append(st == 200 and body == css)
        socks[0].settimeout(3)
        try:
            results.append(socks[0].recv(1) == b'')     # the one idle longest was closed for it
        except OSError:
            results.append(False)
        for s in socks:
            s.close()
        socks = []
        q.pump(0.5)
        for _ in range(6):
            s = www_connect(port)
            s.sendall(b'GET /wave.svg HTTP/1.1\r\nHost: s\r\n')          # not finished: not idle
            socks.append(s)
        time.sleep(0.5)
        late = www_connect(port)
        socks.append(late)
        late.sendall(b'GET /style.css HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n')
        got, _ = www_recv(late, 1.5)
        results.append(got == b'')                      # it waits: every place is taken
        socks[2].sendall(b'\r\n')                       # one request finished: answered, then idle
        got, _ = www_recv(socks[2], 2)
        results.append([x[0] for x in www_responses(got)] == [200])
        got, closed = www_recv(late, 5)
        r = www_responses(got)
        results.append(closed and len(r) == 1 and r[0][0] == 200 and r[0][2] == css)
        for s in socks:
            s.close()
        socks = []
        q.pump(0.5)
        answers, files = [], {'/': initrd_file(q, 'www/index.html'), '/style.css': css,
                              '/wave.svg': initrd_file(q, 'www/wave.svg')}

        def client(n):
            for path, want in files.items():
                try:
                    st, _, body = www_get(port, path, headers={'Connection': 'close'}, timeout=20)
                    answers.append(st == 200 and body == want)
                except (OSError, http.client.HTTPException) as e:
                    print(f'  httpd places: client {n}, {path}: {e!r}')
                    answers.append(False)
        threads = [threading.Thread(target=client, args=(n,)) for n in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        results.append(len(answers) == 24 and all(answers))
        # 11/1/26 (audit B90): six connections made a moment ago, their requests still to come, and a seventh that
        # has sent its own. None of the six is idle yet, so none gives up its place for it: all seven are answered
        q.pump(0.5)
        for _ in range(7):
            socks.append(www_connect(port))
        request = b'GET /style.css HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n'
        socks[6].sendall(request)
        time.sleep(0.2)                     # time for the server to take the seventh, were one of them idle
        for s in socks[:6]:
            s.sendall(request)
        got = [www_recv(s, 5) for s in socks]
        results.append(all(closed and [x[0] for x in www_responses(data)] == [200] for data, closed in got))
    except (OSError, http.client.HTTPException) as e:
        print(f'  httpd places: {e!r}')
        results.append(False)
    finally:
        for s in socks:
            s.close()
    if not all(results):
        print(f'  httpd places: {results} (served, idle one closed, a new one waits, one finished, the new one, eight, '
              'young ones keep their places)')
    return ('httpd: six places: idle ones make room, busy and young ones make new ones wait, eight clients (%d of %d)' %
            (sum(results), len(results)), all(results))


def httpd_timeouts_check(q):
    """a connection that sends nothing is closed after 10 s; one whose request did not finish gets 408 and is closed"""
    port, ok = q.ports['www'], False
    idle = www_connect(port)
    part = www_connect(port)
    try:
        part.sendall(b'GET / HTTP/1.1\r\nHost: s\r\n')
        start = time.time()
        got_idle, closed_idle = www_recv(idle, 16)
        took = time.time() - start
        got_part, closed_part = www_recv(part, 3)
        r = www_responses(got_part)
        ok = closed_idle and got_idle == b'' and 8 <= took <= 14 and closed_part and len(r) == 1 and r[0][0] == 408
        if not ok:
            print(f'  httpd timeouts: idle {closed_idle} {got_idle!r} after {took:.1f} s; part {closed_part} {got_part[:80]!r}')
    except OSError as e:
        print(f'  httpd timeouts: {e!r}')
    finally:
        idle.close()
        part.close()
    return ('httpd: an idle connection closed after 10 s, an unfinished request answered 408', ok)


def httpd_initrd_check(q):
    """httpd on /initrd 8080: the listing of the initrd's root (folders first), a folder asked without its slash
    (301 to it), the sample site as a folder of it, the programs' folder, a large file (cert.pem, 188 KB) byte for
    byte, HEAD of it"""
    port, results = q.ports['www2'], []
    try:
        pem = initrd_file(q, 'etc/ssl/cert.pem')
        st, h, body = www_get(port, '/')
        text = body.decode('utf-8', 'replace')
        results.append(st == 200 and '<title>Index of /</title>' in text and 'href="../"' not in text and
                       0 <= text.find('href="bin/"') < text.find('href="etc/"') < text.find('href="www/"') <
                       text.find('href="README"'))
        st, h, body = www_get(port, '/www')
        results.append(st == 301 and h.get('location') == '/www/' and body == b'')
        st, h, body = www_get(port, '/www/')
        results.append(st == 200 and body == initrd_file(q, 'www/index.html'))
        st, h, body = www_get(port, '/bin/')
        results.append(st == 200 and b'href="httpd">httpd</a>' in body and b'href="../"' in body)
        st, h, body = www_get(port, '/etc/ssl/cert.pem', timeout=30)
        results.append(st == 200 and body == pem and h.get('content-type') == 'application/octet-stream')
        st, h, body = www_get(port, '/etc/ssl/cert.pem', 'HEAD')
        results.append(st == 200 and h.get('content-length') == str(len(pem)) and body == b'')
    except (OSError, http.client.HTTPException, KeyError) as e:
        print(f'  httpd initrd: {e!r}')
        results.append(False)
    return ('httpd on /initrd 8080: listings, 301, a large file, HEAD (%d of %d)' % (sum(results), len(results)),
            all(results))


def httpd_serve_check(q):
    """httpd serve /hda1 on this console (the FAT disk, a site made by the steps before): each request printed as it
    comes; the folder's INDEX.HTM, a name with a space, a hidden name neither listed nor sent, nor reached through
    its 8.3 alias (fs/fat.c makes SECRET~1 of .secret; a lookup matches only the long name); 10/24/26: forty more,
    more than a screen holds lines, answered at once (no --More-- holds the server); Ctrl+C ends it"""
    port, results = q.ports['www'], []
    q.send('httpd serve /hda1\n')
    results.append(q.expect('httpd: serving /hda1 at http://10.0.2.15/', 10) and q.expect('Ctrl+C stops the server', 10))
    try:
        st, h, body = www_get(port, '/WWW/')
        results.append(st == 200 and b'<h1>FAT site</h1>' in body and h.get('content-type') == 'text/html; charset=utf-8')
        results.append(q.expect('10.0.2.2 GET /WWW/ 200 ', 10))
        st, h, body = www_get(port, '/Hello%20World.txt')
        results.append(st == 200 and body.startswith(b'a space') and h.get('content-type') == 'text/plain; charset=utf-8')
        results.append(q.expect('10.0.2.2 GET /Hello%20World.txt 200 ', 10))
        st, h, body = www_get(port, '/')
        results.append(st == 200 and b'href="WWW/"' in body and b'href="Hello%20World.txt"' in body and
                       b'href="README"' in body and b'secret' not in body.lower())
        st, h, body = www_get(port, '/.secret')
        results.append(st == 404 and q.expect('10.0.2.2 GET /.secret 404 ', 10))
        st, h, body = www_get(port, '/SECRET~1')
        results.append(st == 404 and q.expect('10.0.2.2 GET /SECRET~1 404 ', 10))
        # 10/24/26: more requests than a screen holds lines, answered at once: the user shell pages every command,
        # and the server waited at --More-- after a screenful (22 requests). The console is read as they go
        start, t0, heads = len(q.transcript), time.time(), []
        for _ in range(40):
            heads.append(www_get(port, '/README', 'HEAD')[0] == 200)
            console_keep(q)
        results.append(all(heads) and time.time() - t0 < 20)
        q.pump(1)
        results.append(b'--More--' not in q.transcript[start:] and
                       q.transcript[start:].count(b'10.0.2.2 HEAD /README 200 ') == 40)
    except (OSError, http.client.HTTPException) as e:
        print(f'  httpd serve: {e!r}')
        results.append(False)
    q.ser.sendall(b'\x03')
    results.append(q.expect(PROMPT, 10))
    return ('httpd serve /hda1: a site on FAT, every request printed, a screenful and more, Ctrl+C (%d of %d)' %
            (sum(results), len(results)), all(results))


def httpd_slow_check(q):
    """httpd on /hda1: a client that asks for BIG.BIN (four copies of /initrd/bin/undertow, 1.5 MB or more) and reads
    nothing, while three other requests are answered at once; then it reads it all, byte for byte. The file is far
    larger than the guest, QEMU and this host can hold on the way, so the server's connection really waits for room"""
    port, results = q.ports['www'], []
    try:
        big = initrd_file(q, 'bin/undertow') * 4
        slow = www_connect(port, 60, rcvbuf=4096)
        try:
            slow.sendall(b'GET /BIG.BIN HTTP/1.1\r\nHost: s\r\nConnection: close\r\n\r\n')
            time.sleep(2)
            start = time.time()
            served = all(www_get(port, '/README')[0] == 200 for _ in range(3))
            results.append(served and time.time() - start < 5)
            got, closed = www_recv(slow, 60)
            r = www_responses(got)
            results.append(len(big) > 1500000 and closed and len(r) == 1 and r[0][0] == 200 and r[0][2] == big)
            if not results[-1]:
                print(f'  httpd slow: {len(got)} bytes, closed {closed}, {[x[0] for x in r]}, file {len(big)}')
        finally:
            slow.close()
    except (OSError, http.client.HTTPException, KeyError) as e:
        print(f'  httpd slow: {e!r}')
        results.append(False)
    return ('httpd on /hda1: a client that stops reading a large file holds up no other (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


CONSOLE_CHECKS.update({'@httpd-site': httpd_site_check, '@httpd-protocol': httpd_protocol_check,
                       '@httpd-keepalive': httpd_keepalive_check, '@httpd-places': httpd_places_check,
                       '@httpd-timeouts': httpd_timeouts_check, '@httpd-initrd': httpd_initrd_check,
                       '@httpd-serve': httpd_serve_check, '@httpd-slow': httpd_slow_check})


# 10/24/26: ftpd, the file transfer server (user/ftpd/), from this host through the forwarded port 21
# (q.ports['ftp']) and its passive range, each port of it forwarded to the same port of the guest. Python's ftplib
# takes a passive reply's address from its own control connection (127.0.0.1: trust_server_pasv_ipv4_address
# False), so the guest's 10.0.2.15 in the reply does no harm here. The FAT disk's README is the repository's.
REPO_README = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'README')


def ftp_open(q, login=True, timeout=15):
    """an ftplib session with ftpd, its control connection made by www_connect() (50 ms after the one before)"""
    f = ftplib.FTP(timeout=timeout)
    f.trust_server_pasv_ipv4_address = False
    f.sock = www_connect(q.ports['ftp'], timeout)
    f.af = f.sock.family
    f.file = f.sock.makefile('r', encoding=f.encoding)
    f.welcome = f.getresp()
    if login:
        f.login()
    return f


def ftp_get(f, path):
    """the bytes of RETR path (TYPE I)"""
    out = io.BytesIO()
    f.retrbinary('RETR ' + path, out.write)
    return out.getvalue()


def ftp_reply(f, cmd):
    """the reply to cmd whatever its code (ftplib raises for 4xx and 5xx), its lines joined by newlines"""
    f.putcmd(cmd)
    return f.getmultiline()


def ftp_drain(conn, timeout=20):
    """what a data connection sends until it closes"""
    got, deadline = b'', time.time() + timeout
    conn.settimeout(timeout)
    while time.time() < deadline:
        d = conn.recv(65536)
        if not d:
            break
        got += d
    return got


def ftp_close(f):
    try:
        f.quit()
    except (OSError, EOFError, ftplib.Error):
        f.close()


def ftp_replies(s, n, timeout=8):
    """the codes of the next n replies on a raw control connection s (a reply's last line is "ddd text"), and
    whether s then closed: ([codes], closed)"""
    codes, buf, deadline, closed = [], b'', time.time() + timeout, False
    while len(codes) < n and time.time() < deadline:
        while b'\n' in buf and len(codes) < n:
            line, buf = buf.split(b'\n', 1)
            if re.match(rb'^\d{3} ', line):
                codes.append(line[:3].decode())
        if len(codes) == n:
            break
        s.settimeout(max(deadline - time.time(), 0.05))
        try:
            d = s.recv(4096)
        except socket.timeout:
            break
        if not d:
            closed = True
            break
        buf += d
    if not closed and len(codes) == n:
        s.settimeout(1)
        try:
            closed = s.recv(4096) == b''
        except socket.timeout:
            closed = False
        except OSError:
            closed = True
    return codes, closed


def ftpd_session_check(q):
    """a session: the welcome, a login with any name and password, SYST, FEAT (no MLST or MDTM: SurfOS has no UTC),
    PWD and CWD (.. held at /), the listing of / (the mount points, never /dev), the initrd's listings in each form
    (LIST, NLST with and without a folder, MLSD, MLST, STAT), its files byte for byte (the sample site's page, a
    program, cert.pem) and SIZE, a download through EPSV, QUIT"""
    results = []
    try:
        readme, pem = initrd_file(q, 'README'), initrd_file(q, 'etc/ssl/cert.pem')
        f = ftp_open(q, login=False)
        results.append(f.welcome.startswith('220 SurfOS ftpd: the files of /, to read and write'))
        results.append(f.sendcmd('USER surfer').startswith('331') and f.sendcmd('PASS anything').startswith('230'))
        results.append(f.sendcmd('SYST') == '215 UNIX Type: L8')
        feat = f.sendcmd('FEAT')
        results.append(all(x in feat for x in (' EPSV', ' PASV', ' SIZE', ' REST STREAM', ' UTF8')) and
                       'MLST' not in feat and 'MDTM' not in feat)
        results.append(f.pwd() == '/')
        f.cwd('/initrd/bin')
        results.append(f.pwd() == '/initrd/bin')
        f.cwd('../../../..')
        results.append(f.pwd() == '/')
        lines = []
        f.retrlines('LIST', lines.append)
        names = [l.split()[-1] for l in lines]
        results.append('initrd' in names and 'hda1' in names and 'dev' not in names and all(l[:1] in 'd-' for l in lines))
        results.append(sorted(f.nlst('/initrd/www')) == ['/initrd/www/index.html', '/initrd/www/style.css',
                                                         '/initrd/www/wave.svg'])
        f.cwd('/initrd/www')
        results.append(sorted(f.nlst()) == ['index.html', 'style.css', 'wave.svg'])
        facts = dict(f.mlsd('/initrd'))
        results.append(facts.get('bin', {}).get('type') == 'dir' and facts.get('README', {}).get('size') == str(len(readme))
                       and facts['README'].get('perm') == 'r' and 'modify' not in facts['README'])
        mlst = f.sendcmd('MLST /initrd/README')
        results.append(mlst.startswith('250-') and (' type=file;size=%d;perm=r; /initrd/README' % len(readme)) in mlst)
        stat = ftp_reply(f, 'STAT /initrd/www')
        results.append(stat.startswith('213-Status of /initrd/www:') and ' index.html' in stat and
                       stat.endswith('213 End of status (3 entries)'))
        for name in ('www/index.html', 'bin/cat', 'etc/ssl/cert.pem'):
            results.append(ftp_get(f, '/initrd/' + name) == initrd_file(q, name))
        results.append(f.size('/initrd/etc/ssl/cert.pem') == len(pem))
        m = re.search(r'\(\|\|\|(\d+)\|\)', f.sendcmd('EPSV'))
        data = socket.create_connection(('127.0.0.1', int(m.group(1))), 10)
        f.voidcmd('TYPE I')
        f.putcmd('RETR /initrd/README')
        first = f.getresp()
        got = ftp_drain(data)
        data.close()
        results.append(first.startswith('150 Opening BINARY mode') and got == readme and f.voidresp().startswith('226'))
        results.append(f.quit().startswith('221'))
    except (OSError, EOFError, ftplib.Error, AttributeError, KeyError) as e:
        print(f'  ftpd session: {e!r}')
        results.append(False)
    if not all(results):
        print(f'  ftpd session: {results}')
    return ('ftpd: a session: login, FEAT, folders, every listing, the initrd byte for byte, SIZE, EPSV (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def ftpd_upload_check(q):
    """uploads to the FAT disk (/hda1), each read back: 300 KB of random bytes, a file replaced by a shorter one (no
    old tail left), APPE, a resumed download and upload (REST: the upload cut at its offset), STOU's new name, a name
    with a space, RNFR and RNTO (a folder into itself refused), DELE, MKD and RMD; a hidden file: left out of LIST
    and NLST, shown by LIST -a and MLSD, and named in RMD's refusal of its folder"""
    results = []
    blob = os.urandom(300000)
    try:
        f = ftp_open(q)
        f.storbinary('STOR /hda1/FTP.BIN', io.BytesIO(blob))
        results.append(ftp_get(f, '/hda1/FTP.BIN') == blob and f.size('/hda1/FTP.BIN') == len(blob))
        f.storbinary('STOR /hda1/FTP.BIN', io.BytesIO(b'short'))
        results.append(ftp_get(f, '/hda1/FTP.BIN') == b'short')
        f.storbinary('APPE /hda1/FTP.BIN', io.BytesIO(b' and more'))
        results.append(ftp_get(f, '/hda1/FTP.BIN') == b'short and more')
        out = io.BytesIO()
        f.retrbinary('RETR /hda1/FTP.BIN', out.write, rest=6)
        results.append(out.getvalue() == b'and more')
        f.storbinary('STOR /hda1/FTP.BIN', io.BytesIO(b'-resumed'), rest=5)
        results.append(ftp_get(f, '/hda1/FTP.BIN') == b'short-resumed')
        f.voidcmd('TYPE I')
        with f.transfercmd('STOU /hda1/FTP.BIN') as c:
            c.sendall(b'unique')
        m = re.search(r', as (\S+)$', f.voidresp())
        results.append(m is not None and m.group(1) == 'FTP.BIN.1' and ftp_get(f, '/hda1/FTP.BIN.1') == b'unique')
        f.storbinary('STOR /hda1/Two Words.txt', io.BytesIO(b'spaced\n'))
        results.append(ftp_get(f, '/hda1/Two Words.txt') == b'spaced\n' and '/hda1/Two Words.txt' in f.nlst('/hda1'))
        f.rename('/hda1/Two Words.txt', '/hda1/Renamed.txt')
        results.append(ftp_get(f, '/hda1/Renamed.txt') == b'spaced\n' and '/hda1/Two Words.txt' not in f.nlst('/hda1'))
        # 10/25/26 (audit R1): a folder renamed into itself under another case of its name is refused (FAT wrote it
        # into itself and took it out of /hda1), and stays where it was
        f.mkd('/hda1/LOOPDIR')
        results.append(ftp_reply(f, 'RNFR /hda1/LOOPDIR').startswith('350') and
                       ftp_reply(f, 'RNTO /hda1/loopdir/inside').startswith('553') and
                       f.rmd('/hda1/LOOPDIR').startswith('250'))
        f.mkd('/hda1/FTPDIR')
        f.cwd('/hda1/FTPDIR')
        f.storbinary('STOR .hidden', io.BytesIO(b'h'))
        f.storbinary('STOR shown', io.BytesIO(b's'))
        lines, every = [], []
        f.retrlines('LIST', lines.append)
        f.retrlines('LIST -a', every.append)
        results.append([l.split()[-1] for l in lines] == ['shown'] and sorted(l.split()[-1] for l in every) ==
                       ['.hidden', 'shown'] and sorted(n for n, _ in f.mlsd()) == ['.hidden', 'shown'] and
                       f.nlst() == ['shown'])
        f.delete('shown')
        results.append(ftp_reply(f, 'RMD /hda1/FTPDIR').startswith('550 /hda1/FTPDIR is not empty: it holds 1 hidden file'))
        f.delete('.hidden')
        f.cwd('/hda1')
        results.append(f.rmd('/hda1/FTPDIR').startswith('250'))
        for name in ('FTP.BIN', 'FTP.BIN.1', 'Renamed.txt'):
            f.delete('/hda1/' + name)
        left = f.nlst('/hda1')
        results.append(not any(n in left for n in ('/hda1/FTP.BIN', '/hda1/FTP.BIN.1', '/hda1/Renamed.txt', '/hda1/FTPDIR')))
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error, AttributeError) as e:
        print(f'  ftpd upload: {e!r}')
        results.append(False)
    if not all(results):
        print(f'  ftpd upload: {results}')
    return ('ftpd: uploads read back, replace, APPE, REST both ways, STOU, rename, folders, hidden files (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def ftpd_text_check(q):
    """text mode (TYPE A): an LF goes out as CR LF, a CR LF as it is, a CR alone too; CR LF comes in as LF, also when
    a CR and its LF arrive apart; a CR alone stays; listings end their lines with CR LF in either mode"""
    results = []
    try:
        f = ftp_open(q)
        f.storbinary('STOR /hda1/TEXT.TXT', io.BytesIO(b'one\ntwo\r\nthree\rfour\n'))
        f.voidcmd('TYPE A')
        with f.transfercmd('RETR /hda1/TEXT.TXT') as c:
            wire = ftp_drain(c)
        results.append(wire == b'one\r\ntwo\r\nthree\rfour\r\n' and f.voidresp().startswith('226'))
        f.voidcmd('TYPE A')
        with f.transfercmd('STOR /hda1/TEXT.TXT') as c:
            c.sendall(b'a\r\nb\r')
            time.sleep(0.3)                                  # the CR and its LF in pieces of their own
            c.sendall(b'\nc\rd\r\n')
        results.append(f.voidresp().startswith('226') and ftp_get(f, '/hda1/TEXT.TXT') == b'a\nb\nc\rd\n')
        f.voidcmd('TYPE I')
        with f.transfercmd('LIST /hda1') as c:
            listing = ftp_drain(c)
        f.voidresp()
        results.append(listing.endswith(b'\r\n') and listing.count(b'\n') == listing.count(b'\r\n') > 2)
        f.delete('/hda1/TEXT.TXT')
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd text: {e!r}')
        results.append(False)
    if not all(results):
        print(f'  ftpd text: {results}')
    return ('ftpd: text mode both ways, a CR LF in pieces, listings in CR LF (%d of %d)' % (sum(results), len(results)),
            bool(results) and all(results))


def ftpd_active_check(q):
    """active mode: PORT and EPRT with this host's address as the guest sees it (10.0.2.2, which QEMU's user network
    takes to this host's 127.0.0.1): a file sent and one received; a PORT to another address and to a port below
    1024 refused (no FTP bounce), EPRT for IPv6 and EPSV 2 told the one protocol, a PORT that is none; EPSV ALL
    leaves EPSV alone"""
    results = []
    srv = socket.socket()
    try:
        srv.bind(('127.0.0.1', 0))
        srv.listen(2)
        srv.settimeout(10)
        port = srv.getsockname()[1]
        f = ftp_open(q)
        f.voidcmd('TYPE I')
        results.append(f.sendcmd('PORT 10,0,2,2,%d,%d' % (port >> 8, port & 255)).startswith('200'))
        f.putcmd('RETR /initrd/README')
        first = f.getresp()
        c, _ = srv.accept()
        got = ftp_drain(c)
        c.close()
        results.append(first.startswith('150') and got == initrd_file(q, 'README') and f.voidresp().startswith('226'))
        results.append(f.sendcmd('EPRT |1|10.0.2.2|%d|' % port).startswith('200'))
        f.putcmd('STOR /hda1/ACTIVE.TXT')
        first = f.getresp()
        c, _ = srv.accept()
        c.sendall(b'sent the other way\n')
        c.close()
        results.append(first.startswith('150') and f.voidresp().startswith('226') and
                       ftp_get(f, '/hda1/ACTIVE.TXT') == b'sent the other way\n')
        closed = free_port()                                # nobody listens: refused at once, the loop not held
        f.sendcmd('PORT 10,0,2,2,%d,%d' % (closed >> 8, closed & 255))
        f.putcmd('RETR /initrd/README')
        first = f.getresp()
        results.append(first.startswith('150') and
                       f.getmultiline() == '425 No data connection to 10.0.2.2 port %d: Connection refused' % closed)
        results.append(ftp_reply(f, 'PORT 10,0,2,99,4,1').startswith('504 A data connection to 10.0.2.99 is refused'))
        results.append(ftp_reply(f, 'PORT 10,0,2,2,0,21').startswith('504 A data connection to port 21 is refused'))
        results.append(ftp_reply(f, 'EPRT |2|::1|5000|').startswith('522') and ftp_reply(f, 'EPSV 2').startswith('522'))
        results.append(ftp_reply(f, 'PORT 1,2,3').startswith('501'))
        results.append(ftp_reply(f, 'EPSV ALL').startswith('200') and ftp_reply(f, 'PASV').startswith('503') and
                       ftp_reply(f, 'PORT 10,0,2,2,%d,%d' % (port >> 8, port & 255)).startswith('503') and
                       ftp_reply(f, 'EPSV').startswith('229'))
        f.delete('/hda1/ACTIVE.TXT')
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd active: {e!r}')
        results.append(False)
    finally:
        srv.close()
    if not all(results):
        print(f'  ftpd active: {results}')
    return ('ftpd: active mode both ways, no bounce, IPv6 told, EPSV ALL (%d of %d)' % (sum(results), len(results)),
            bool(results) and all(results))


def ftp_slow_transfer(f, cmd, rcvbuf=4096):
    """cmd through PASV on a data connection with a small receive buffer, read by nobody yet: (its 150 reply, the
    connection). The file (BIG.BIN, 2.5 MB) is far more than the guest, QEMU and this host hold on the way, so the
    server's transfer waits for room until the connection is read, as httpd's @httpd-slow has it"""
    f.voidcmd('TYPE I')
    m = re.search(r'\((?:\d+,){4}(\d+),(\d+)\)', f.sendcmd('PASV'))
    c = socket.socket()
    c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    c.settimeout(20)
    c.connect(('127.0.0.1', int(m.group(1)) * 256 + int(m.group(2))))
    f.putcmd(cmd)
    return f.getresp(), c


def ftpd_abort_check(q):
    """ABOR during the download of a large file (BIG.BIN on the FAT disk, read slowly), as Python's ftplib abort()
    sends it (426 for the transfer, then 226 for ABOR), the session in step after it; STAT during a transfer answered
    at once; a NOOP sent during a transfer answered after its 226, in order; ABOR with no transfer (225)"""
    results = []
    try:
        big = initrd_file(q, 'bin/undertow') * 4
        f = ftp_open(q, timeout=30)
        first, c = ftp_slow_transfer(f, 'RETR /hda1/BIG.BIN')
        c.recv(1000)
        aborted = f.abort()
        c.close()
        results.append(first.startswith('150') and aborted.startswith('426') and f.voidresp().startswith('226') and
                       f.sendcmd('NOOP').startswith('200'))
        first, c = ftp_slow_transfer(f, 'RETR /hda1/BIG.BIN')
        time.sleep(0.5)                                     # what the way holds is full: the transfer waits
        f.putcmd('STAT')
        stat = f.getline()
        got = ftp_drain(c, 60)
        c.close()
        results.append(stat.startswith('213 RETR /hda1/BIG.BIN: ') and got == big and f.voidresp().startswith('226'))
        first, c = ftp_slow_transfer(f, 'RETR /hda1/BIG.BIN')
        f.putcmd('NOOP')
        time.sleep(0.3)
        got = ftp_drain(c, 60)
        c.close()
        results.append(got == big and f.getresp().startswith('226') and f.getresp().startswith('200'))
        # 10/25/26 (review): what comes behind a held command is still read: the NOOP is held, STAT and ABOR are
        # answered at once (an ABOR behind a held NOOP waited for the end of the transfer, here the 60 s stall:
        # the data connection is left unread, as ftplib's abort() leaves it), the NOOP's 200 after them
        first, c = ftp_slow_transfer(f, 'RETR /hda1/BIG.BIN')
        time.sleep(0.3)
        t0 = time.time()
        f.sock.sendall(b'NOOP\r\nSTAT\r\nABOR\r\n')
        codes = [f.getmultiline()[:3] for _ in range(4)]
        c.close()
        results.append(first.startswith('150') and codes == ['213', '426', '226', '200'] and time.time() - t0 < 10)
        results.append(ftp_reply(f, 'ABOR').startswith('225'))
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error, KeyError) as e:
        print(f'  ftpd abort: {e!r}')
        results.append(False)
    if not all(results):
        print(f'  ftpd abort: {results}')
    return ('ftpd: ABOR during a transfer, STAT during one, commands after it in order, ABOR behind them (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def ftpd_protocol_check(q):
    """the control connection as it may come, on a raw socket: commands before a login (NOOP yes, PWD 530), Telnet's
    IP and Synch and an option before commands, several commands in one segment and a line ended by LF alone
    (answered in order), a NUL in a path (501), a line of 600 bytes (500) with the session going on; then the
    replies a client gets wrong: unknown (500), not implemented (502), no argument (501), REST that is no offset (501)
    or past the end (554), RNTO without RNFR (503), a transfer without PASV (425), a folder for a file and the other
    way round, the initrd written to, /dev, a folder there already, MODE and TYPE that are not served, REIN"""
    results = []
    s = None
    try:
        s = www_connect(q.ports['ftp'])
        results.append(ftp_replies(s, 1)[0] == ['220'])
        s.sendall(b'NOOP\r\nPWD\r\n')
        results.append(ftp_replies(s, 2)[0] == ['200', '530'])
        s.sendall(b'\xff\xf4\xff\xf2USER a\r\nPASS b\r\n\xff\xfb\x01SYST\nPWD\r\n')
        results.append(ftp_replies(s, 4)[0] == ['331', '230', '215', '257'])
        s.sendall(b'CWD a\x00b\r\n' + b'X' * 600 + b'\r\nNOOP\r\n')
        results.append(ftp_replies(s, 3)[0] == ['501', '500', '200'])
        s.close()
        s = None
        f = ftp_open(q)
        want = [('XYZZY', '500'), ('AUTH TLS', '502'), ('MDTM /initrd/README', '502'), ('CWD', '501'),
                ('REST abc', '501'), ('RNTO /x', '503'), ('RETR /initrd/README', '425'), ('CWD /initrd/README', '550'),
                ('SIZE /initrd', '550'), ('MKD /initrd/x', '550'), ('CWD /dev', '550'), ('DELE /hda1', '550'),
                ('RMD /', '550'), ('MKD /hda1/DOCS', '550'), ('TYPE X', '501'), ('MODE B', '504'), ('STRU R', '504'),
                ('SITE CHMOD 644 x', '502'), ('OPTS UTF8 ON', '200'), ('ALLO 10', '202'), ('HELP', '214'),
                ('STAT', '211'), ('ACCT x', '202')]
        got = [(cmd, ftp_reply(f, cmd)[:3]) for cmd, _ in want]
        results.append(got == want)
        if got != want:
            print(f'  ftpd protocol: {[(c, r) for (c, r), (_, w) in zip(got, want) if r != w]}')
        for cmd, code in (('RETR /initrd', '550'), ('STOR /initrd/x', '550'), ('RETR /dev/console', '550')):
            f.sendcmd('PASV')
            results.append(ftp_reply(f, cmd)[:3] == code)
        f.sendcmd('REST 999999')
        f.sendcmd('PASV')
        results.append(ftp_reply(f, 'RETR /initrd/README').startswith('554 REST 999999 is past the end'))
        results.append(ftp_reply(f, 'REIN').startswith('220') and ftp_reply(f, 'PWD').startswith('530'))
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd protocol: {e!r}')
        results.append(False)
    finally:
        if s:
            s.close()
    if not all(results):
        print(f'  ftpd protocol: {results}')
    return ('ftpd: the control connection: Telnet sequences, commands together, long lines, NUL; every refusal (%d of %d)'
            % (sum(results), len(results)), bool(results) and all(results))


def ftpd_pasv_port(f):
    """PASV on an ftplib session: the port its reply names"""
    m = re.search(r'\((?:\d+,){4}(\d+),(\d+)\)', f.sendcmd('PASV'))
    return int(m.group(1)) * 256 + int(m.group(2))


def ftpd_descriptors_check(q):
    """10/25/26 (review): the server's 16 descriptors with six sessions open. Three transfers whose clients send RETR
    before they connect (each waits holding its listener alone; the file is opened and the 150 given once the
    connection is there) move at once (the third got its 150, then accept() found no descriptor: 425), and so do
    three whose clients connect first (ftplib, curl); a fourth is told to try again (425) each time, and gets its
    file once they are done"""
    results, fs = [], []
    time.sleep(0.5)                                         # the sessions of the checks before have ended
    try:
        readme, big = initrd_file(q, 'README'), initrd_file(q, 'bin/undertow') * 4
        for _ in range(6):
            fs.append(ftp_open(q, timeout=30))
        for f in fs:
            f.voidcmd('TYPE I')
        ports = [ftpd_pasv_port(f) for f in fs[:3]]
        for f in fs[:3]:
            f.putcmd('RETR /hda1/BIG.BIN')                  # no reply yet: it waits for its connection
        slow = []
        for f, port in zip(fs[:3], ports):
            c = socket.socket()
            c.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            c.settimeout(20)
            c.connect(('127.0.0.1', port))
            slow.append((f.getresp(), c))
        results.append(all(first.startswith('150') for first, c in slow) and ftp_reply(fs[3], 'PASV')[:3] == '425')
        for (first, c), f in zip(slow, fs[:3]):
            results.append(ftp_drain(c, 60) == big and f.voidresp().startswith('226'))
            c.close()
        slow = [ftp_slow_transfer(f, 'RETR /hda1/BIG.BIN') for f in fs[:3]]
        results.append(all(first.startswith('150') for first, c in slow) and ftp_reply(fs[3], 'PASV')[:3] == '425')
        for (first, c), f in zip(slow, fs[:3]):
            results.append(ftp_drain(c, 60) == big and f.voidresp().startswith('226'))
            c.close()
        results.append(ftp_get(fs[3], '/initrd/README') == readme)
    except (OSError, EOFError, ftplib.Error, AttributeError) as e:
        print(f'  ftpd descriptors: {e!r}')
        results.append(False)
    finally:
        for f in fs:
            ftp_close(f)
    if not all(results):
        print(f'  ftpd descriptors: {results}')
    return ('ftpd: six sessions, three transfers at once in either order, the next told to try again (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def ftpd_places_check(q):
    """six sessions fill every place: a seventh is told 421 and closed; once one has left, a new one is taken; a
    client that sends its commands and closes its side (as nc does when its input ends) gets every reply, QUIT's
    among them, and so does one that sends no QUIT"""
    results, socks = [], []
    time.sleep(0.5)                                         # the sessions of the checks before have ended
    try:
        for _ in range(6):
            s = www_connect(q.ports['ftp'])
            socks.append(s)
            results.append(ftp_replies(s, 1)[0] == ['220'])
        seventh = www_connect(q.ports['ftp'])
        socks.append(seventh)
        codes, closed = ftp_replies(seventh, 1)
        results.append(codes == ['421'] and closed)
        socks[0].sendall(b'QUIT\r\n')
        results.append(ftp_replies(socks[0], 1) == (['221'], True))
        time.sleep(0.5)
        f = ftp_open(q)
        results.append(f.pwd() == '/')
        ftp_close(f)
        for s in socks:
            s.close()
        socks = []
        time.sleep(0.5)
        s = www_connect(q.ports['ftp'])
        socks.append(s)
        s.sendall(b'USER a\r\nPASS b\r\nPWD\r\nNOOP\r\nQUIT\r\n')
        s.shutdown(socket.SHUT_WR)
        results.append(ftp_replies(s, 6) == (['220', '331', '230', '257', '200', '221'], True))
        s = www_connect(q.ports['ftp'])
        socks.append(s)
        s.sendall(b'USER a\r\nPASS b\r\nCWD /initrd\r\nPWD\r\n')
        s.shutdown(socket.SHUT_WR)
        results.append(ftp_replies(s, 5) == (['220', '331', '230', '250', '257'], True))
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd places: {e!r}')
        results.append(False)
    finally:
        for s in socks:
            s.close()
    if not all(results):
        print(f'  ftpd places: {results}')
    return ('ftpd: six places, a seventh refused, half-closed clients answered whole (%d of %d)' %
            (sum(results), len(results)), bool(results) and all(results))


def ftpd_data_time_check(q):
    """a transfer whose passive data connection never comes is given up after 30 s (425, its only reply: 10/25/26,
    the 150 comes once the connection is there), and the session goes on"""
    ok = False
    try:
        f = ftp_open(q, timeout=45)
        f.sendcmd('PASV')
        start = time.time()
        f.putcmd('RETR /initrd/README')
        last = f.getmultiline()
        took = time.time() - start
        ok = last.startswith('425 No data connection came within 30 s') and 28 <= took <= 36 and \
            f.sendcmd('NOOP').startswith('200')
        if not ok:
            print(f'  ftpd data time: {last!r} after {took:.1f} s')
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd data time: {e!r}')
    return ('ftpd: a data connection that never comes: 425 after 30 s, the session goes on', ok)


def ftpd_readonly_check(q):
    """ftpd on -r /hda1: the FAT disk is all a client sees, read-only: its files listed (r--) and sent, .. held at
    its top, and every write refused (STOR, APPE, STOU, DELE, MKD, RMD, RNFR) with the disk unchanged"""
    results = []
    try:
        with open(REPO_README, 'rb') as fh:
            readme = fh.read()
        f = ftp_open(q)
        results.append('read-only' in f.welcome)
        lines = []
        f.retrlines('LIST', lines.append)
        results.append(any(l.split()[-1] == 'README' and l.startswith('-r--r--r--') for l in lines) and
                       not any(l.split()[-1] in ('initrd', 'hda1') for l in lines))
        results.append(ftp_get(f, '/README') == readme and dict(f.mlsd('/'))['README'].get('perm') == 'r')
        f.cwd('..')
        results.append(f.pwd() == '/')
        before = sorted(f.nlst())
        refusals = [ftp_reply(f, c) for c in ('DELE /README', 'MKD /X', 'RMD /DOCS', 'RNFR /README', 'STOR /X.TXT',
                                               'APPE /README', 'STOU')]
        results.append(all(r.startswith('550') and 'served read-only' in r for r in refusals))
        results.append(sorted(f.nlst()) == before)
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error, KeyError) as e:
        print(f'  ftpd read-only: {e!r}')
        results.append(False)
    if not all(results):
        print(f'  ftpd read-only: {results}')
    return ('ftpd on -r /hda1: the disk read-only, every write refused (%d of %d)' % (sum(results), len(results)),
            bool(results) and all(results))


def ftpd_serve_check(q):
    """ftpd serve -a 127.0.0.1 /hda1 on this console: each command printed with its reply's code (PASS as ****), a
    transfer with its bytes, a passive reply naming -a's address; sixty commands, more than a screen holds, answered
    at once (the user shell pages every command, and a server held at --More-- would answer none until a key came);
    Ctrl+C ends it"""
    results = []
    q.send(f"ftpd serve -a 127.0.0.1 /hda1 {q.ports['ftp_pasv_range']}\n")
    results.append(q.expect('ftpd: serving /hda1 at ftp://10.0.2.15/ (read-write)', 10) and
                   q.expect('Ctrl+C stops the server', 10))
    start = len(q.transcript)
    try:
        with open(REPO_README, 'rb') as fh:
            readme = fh.read()
        f = ftp_open(q)
        results.append(re.match(r'227 Entering Passive Mode \(127,0,0,1,\d+,\d+\)\.$', f.sendcmd('PASV')) is not None)
        results.append(ftp_get(f, '/README') == readme)
        t0, noops = time.time(), []
        for _ in range(60):
            noops.append(f.sendcmd('NOOP').startswith('200'))
            console_keep(q)                                 # the console read as they go
        results.append(all(noops) and time.time() - t0 < 20)
        ftp_close(f)
    except (OSError, EOFError, ftplib.Error) as e:
        print(f'  ftpd serve: {e!r}')
        results.append(False)
    q.pump(1)
    out = q.transcript[start:]
    results.append(b'10.0.2.2 USER anonymous -> 331' in out and b'10.0.2.2 PASS **** -> 230' in out and
                   b'10.0.2.2 RETR /README -> 226, %d bytes' % len(readme) in out and
                   out.count(b'NOOP -> 200') == 60 and b'--More--' not in out)
    q.ser.sendall(b'\x03')
    results.append(q.expect(PROMPT, 10))
    if not all(results):
        print(f'  ftpd serve: {results}')
    return ('ftpd serve: every command printed, -a in PASV, a screenful and more without --More--, Ctrl+C (%d of %d)' %
            (sum(results), len(results)), all(results))


# 10/24/26: nc, the netcat of SurfOS (user/bin/nc.c): files both ways with this host, which plays the Mac's nc (it
# closes its side when its input ends and reads on until the other end closes: shutdown(SHUT_WR)). The guest
# listens on RX_PORT (forwarded: q.ports['rx']) and reaches this host's 127.0.0.1 as 10.0.2.2.
NC_SENT = []                                                # the bytes @nc-receive sent, for @nc-send


def host_listener():
    """a listening socket of this host for the guest to reach as 10.0.2.2: (socket, port)"""
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 0))
    s.listen(4)
    s.settimeout(20)
    return s, s.getsockname()[1]


def nc_receive_check(q):
    """nc -l RX_PORT > /hda1/NC.BIN: where it listens and who came are said; 200000 random bytes from this host,
    which closes its side when they are all sent, as the Mac's nc does; nc then ends, so the connection closes and
    the prompt comes (the file's size is the next step's)"""
    results = []
    blob = os.urandom(200000)
    NC_SENT[:] = [blob]
    q.send(f'nc -l {RX_PORT} > /hda1/NC.BIN\n')
    results.append(q.expect(f'nc: listening on 10.0.2.15 port {RX_PORT}', 10))
    try:
        s = www_connect(q.ports['rx'], 30)
        try:
            results.append(q.expect('nc: connection from 10.0.2.2 port ', 10))
            s.sendall(blob)
            s.shutdown(socket.SHUT_WR)
            got, closed = www_recv(s, 30)
            results.append(got == b'' and closed)
        finally:
            s.close()
    except OSError as e:
        print(f'  nc receive: {e!r}')
        results.append(False)
    results.append(q.expect(PROMPT, 30))
    if not all(results):
        print(f'  nc receive: {results}')
    return ('nc -l: a file of 200000 bytes from this host, ended by its half-close (%d of %d)' %
            (sum(results), len(results)), all(results))


def nc_send_check(q):
    """nc -v 10.0.2.2 <port> < /hda1/NC.BIN, to a listener of this host: both ends of the connection said, the
    file's bytes arrive whole, then nc's FIN (its input has ended: shutdown()); what this host answers after that
    still reaches nc, which prints it and ends when this host closes, with status 0. Then the same to a listener
    that half-closes at once, as the Mac's nc -l does when its own input is at its end (< /dev/null, or no
    terminal): nc sends the rest of its file all the same, and ends when both ways have"""
    results, box = [], {}
    srv, port = host_listener()

    def serve(early):
        try:
            c, _ = srv.accept()
            c.settimeout(30)
            if early:
                c.shutdown(socket.SHUT_WR)
            data = b''
            while True:
                d = c.recv(65536)
                if not d:
                    break
                data += d
            box['data'] = data
            if not early:
                c.sendall(b'thanks: %d bytes\n' % len(data))
            c.close()
        except OSError as e:
            box['error'] = e
    t = threading.Thread(target=serve, args=(False,), daemon=True)
    t.start()
    q.send(f'nc -v 10.0.2.2 {port} < /hda1/NC.BIN; echo status $?\n')
    results.append(q.expect(f'nc: connected to 10.0.2.2 port {port} (this end: 10.0.2.15 port ', 15))
    results.append(q.expect('nc: the input has ended: the other end is told so', 30))
    results.append(q.expect('thanks: 200000 bytes', 30))
    results.append(q.expect('nc: the other end has sent all it will', 10) and q.expect('nc: both ways have ended', 10))
    results.append(q.expect('status 0', 10) and q.expect(PROMPT, 10))
    t.join(30)
    results.append(bool(NC_SENT) and box.get('data') == NC_SENT[0])
    box.clear()
    t = threading.Thread(target=serve, args=(True,), daemon=True)
    t.start()
    q.send(f'nc 10.0.2.2 {port} < /hda1/NC.BIN; echo status $?\n')
    results.append(q.expect('status 0', 30) and q.expect(PROMPT, 10))
    t.join(30)
    srv.close()
    results.append(bool(NC_SENT) and box.get('data') == NC_SENT[0])
    if not all(results):
        print(f'  nc send: {results} {box.get("error")!r} {len(box.get("data", b""))} bytes')
    return ('nc: the file sent back whole, the half-close, the answer after it; to one that half-closed first '
            '(%d of %d)' % (sum(results), len(results)), all(results))


def nc_talk_check(q):
    """nc -c 10.0.2.2 <port>, typed to on the console: what this host says is printed, the line typed goes out with
    CR LF (-c), and when this host closes, nc ends"""
    results, box = [], {}
    srv, port = host_listener()

    def serve():
        try:
            c, _ = srv.accept()
            c.settimeout(15)
            c.sendall(b'220 the host says hello\r\n')
            line = b''
            while not line.endswith(b'\n'):
                d = c.recv(100)
                if not d:
                    break
                line += d
            box['line'] = line
            c.sendall(b'250 bye\r\n')
            c.close()
        except OSError as e:
            box['error'] = e
    t = threading.Thread(target=serve, daemon=True)
    t.start()
    q.send(f'nc -c 10.0.2.2 {port}\n')
    results.append(q.expect('220 the host says hello', 10))
    q.send('HELO surfos\n')
    results.append(q.expect('250 bye', 10) and q.expect(PROMPT, 10))
    t.join(10)
    srv.close()
    results.append(box.get('line') == b'HELO surfos\r\n')
    if not all(results):
        print(f'  nc talk: {results} {box!r}')
    return ('nc -c: what the other end says printed, a line typed sent with CR LF (%d of %d)' %
            (sum(results), len(results)), all(results))


def nc_scan_check(q):
    """nc -z: a port of this host that listens is open, one that does not is closed (status 1); an open one alone
    gives status 0"""
    srv, open_port = host_listener()
    closed_port = free_port()
    q.send(f'nc -z 10.0.2.2 {open_port} {closed_port}; echo status $?\n')
    results = [q.expect(f'10.0.2.2 port {open_port}: open', 15), q.expect(f'10.0.2.2 port {closed_port}: closed', 15),
               q.expect('status 1', 10) and q.expect(PROMPT, 10)]
    q.send(f'nc -z 10.0.2.2 {open_port}; echo status $?\n')
    results.append(q.expect(f'10.0.2.2 port {open_port}: open', 15) and q.expect('status 0', 10) and q.expect(PROMPT, 10))
    srv.close()
    return ('nc -z: an open port and a closed one (%d of %d)' % (sum(results), len(results)), all(results))


CONSOLE_CHECKS.update({'@nc-receive': nc_receive_check, '@nc-send': nc_send_check, '@nc-talk': nc_talk_check,
                       '@nc-scan': nc_scan_check})


def fetch_files_check(q):
    """10/25/26: what fetch wrote on the FAT disk, read back byte for byte through httpd on /hda1: data.bin (fetched,
    then replaced with -f), cut.bin (cut short, then the rest asked for with a Range), norange.bin (its rest refused,
    then downloaded anew with -f), part.bin (the rest in two parts, a bad part and a 204 between), redirect (the
    first address's name), DL/data.bin, chunked.txt (the chunked coding undone), the name with spaces; nothing for
    the 404 or the 204; the Range requests fetch sent and its User-Agent"""
    port, results = q.ports['www'], []
    q.send('httpd on /hda1\n')
    results.append(q.expect('httpd: serving /hda1 at http://10.0.2.15/', 10) and q.expect(PROMPT, 10))
    try:
        for path in ('/data.bin', '/cut.bin', '/norange.bin', '/part.bin', '/redirect', '/DL/data.bin'):
            st, h, body = www_get(port, path, timeout=30)
            results.append(st == 200 and body == FETCH_DATA)
        st, h, body = www_get(port, '/chunked.txt')
        results.append(st == 200 and body == FETCH_TEXT)
        st, h, body = www_get(port, '/name%20with%20space.txt')
        results.append(st == 200 and body == b'a name with spaces\n')
        results.append(www_get(port, '/missing')[0] == 404 and www_get(port, '/empty')[0] == 404)
    except (OSError, http.client.HTTPException) as e:
        print(f'  fetch files: {e!r}')
        results.append(False)
    seen = [(p, r) for p, r, a in FETCH_SEEN]
    results.append(('/fetch/cut.bin', 'bytes=100000-') in seen and ('/fetch/norange.bin', 'bytes=100000-') in seen and
                   ('/fetch/data.bin', 'bytes=300000-') in seen and ('/fetch/badpart.bin', 'bytes=100000-') in seen and
                   ('/fetch/part.bin', 'bytes=100000-') in seen and ('/fetch/part.bin', 'bytes=200000-') in seen and
                   all(a == 'fetch/1.0 (SurfOS 1.0)' for p, r, a in FETCH_SEEN))
    q.send('httpd off\n')
    results.append(q.expect('httpd: stopped', 10) and q.expect(PROMPT, 10))
    if not all(results):
        print(f'  fetch files: {results}')
    return ('fetch: the files read back through httpd, the Range requests (%d of %d)' % (sum(results), len(results)),
            all(results))


CONSOLE_CHECKS['@fetch-files'] = fetch_files_check
TIMEOUTS['fetch'] = 30                  # the downloads (by the first word), 300000 bytes each at -O0
TIMEOUTS['memhog'] = 60                 # 10/25/26: it writes every page programs may have, 200 MB or more


CONSOLE_CHECKS.update({'@ftpd-session': ftpd_session_check, '@ftpd-upload': ftpd_upload_check,
                       '@ftpd-text': ftpd_text_check, '@ftpd-active': ftpd_active_check,
                       '@ftpd-abort': ftpd_abort_check, '@ftpd-protocol': ftpd_protocol_check,
                       '@ftpd-descriptors': ftpd_descriptors_check,
                       '@ftpd-places': ftpd_places_check, '@ftpd-data-time': ftpd_data_time_check,
                       '@ftpd-readonly': ftpd_readonly_check, '@ftpd-serve': ftpd_serve_check})


# 10/10/26 (Undertow): a private DNS peer on a QEMU raw link. No public DNS,
# host resolver, privileged port or physical network is involved.
class DnsPeer:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        self.qport = None           # 10/18/26 (round fs-fix-11): QEMU's end, its Qemu's ports['dns'] once it has started
        self.running = True
        self.pending = []
        self.ids = []
        self.lookups = []           # 10/15/26 (review M1): (source port, ID) of each lookup, copies once
        self.floods = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def args(self):
        return ['-netdev', 'dgram,id=dns,local.type=inet,local.host=127.0.0.1,local.port={port:dns},'
                          f'remote.type=inet,remote.host=127.0.0.1,remote.port={self.port}',
                '-device', 'e1000,netdev=dns,mac=52:54:00:12:34:56']

    @staticmethod
    def checksum(p):
        if len(p) & 1:
            p += b'\0'
        n = sum(struct.unpack('!%dH' % (len(p) // 2), p))
        while n >> 16:
            n = (n & 65535) + (n >> 16)
        return (~n) & 65535

    def send(self, frame, payload, protocol=17, source=b'\x0a\0\x02\x03'):
        dest = frame[26:30]
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), 1, 0, 64, protocol, 0, source, dest)
        ip = ip[:10] + struct.pack('!H', self.checksum(ip)) + ip[12:]
        self.sock.sendto(frame[6:12] + b'\x52\x54\0\x12\x34\x02\x08\0' + ip + payload,
                         ('127.0.0.1', self.qport))

    def reply(self, frame, query, flags=0x8180, port=53, source=b'\x0a\0\x02\x03', address=b'\xc0\0\x02\x07'):
        answer = b'\xc0\x0c' + struct.pack('!HHIH', 1, 1, 30, 4) + address
        data = query[:2] + struct.pack('!HHHHH', flags, 1, int(flags == 0x8180), 0, 0) + query[12:]
        if flags == 0x8180:
            data += answer
        udp = struct.pack('!HHHH', port, struct.unpack('!H', frame[34:36])[0], 8 + len(data), 0) + data
        self.send(frame, udp, source=source)

    def flood(self, frame, query):
        bad = struct.pack('!H', (struct.unpack('!H', query[:2])[0] + 1) & 65535) + query[2:]
        until = time.monotonic() + 6
        while self.running and time.monotonic() < until:
            self.reply(frame, bad)
            time.sleep(0.005)

    def run(self):
        self.sock.settimeout(0.1)
        while self.running:
            try:
                frame, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(frame) < 42 or not self.qport:
                continue
            if frame[12:14] == b'\x08\x06':
                if frame[20:22] == b'\0\x01':
                    mac = b'\x52\x54\0\x12\x34\x02'
                    arp = frame[6:12] + mac + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x800, 6, 4, 2)
                    self.sock.sendto(arp + mac + frame[38:42] + frame[22:28] + frame[28:32],
                                     ('127.0.0.1', self.qport))
                continue
            if frame[12:14] != b'\x08\0':
                continue
            if frame[23] == 1 and frame[34] == 8:  # ICMP echo for ping's DNS path
                msg = bytearray(frame[34:]); msg[0] = 0; msg[2:4] = b'\0\0'
                msg[2:4] = struct.pack('!H', self.checksum(bytes(msg)))
                self.send(frame, msg, 1, frame[30:34])
                continue
            if frame[23] != 17 or frame[36:38] != b'\0\x35':
                continue
            query = frame[42:]
            if len(query) < 17:
                continue
            parts, off = [], 12
            while query[off]:
                n = query[off]; parts.append(query[off + 1:off + n + 1]); off += n + 1
            host = b'.'.join(parts)
            key = (struct.unpack('!H', frame[34:36])[0], struct.unpack('!H', query[:2])[0])
            if key not in self.lookups:
                self.lookups.append(key)
            if host == b'flood.test':
                t = threading.Thread(target=self.flood, args=(frame, query), daemon=True)
                self.floods.append(t); t.start()
            elif host.startswith(b'concurrent'):
                # 10/11/26: the guest asks again after 1 s; a copy changes nothing. 10/15/26 (review M1): a
                # copy has the same source port and ID; the IDs are random, so two lookups may share one
                if key in self.ids:
                    continue
                self.pending.append((frame, query)); self.ids.append(key)
                if len(self.pending) == 2:
                    for f, q in reversed(self.pending):
                        self.reply(f, q)
                    self.pending.clear()
            elif host == b'missing.test':
                self.reply(frame, query, 0x8183)
            else:
                # Even matching IDs must not accept these before the real peer's answer.
                self.reply(frame, query, source=b'\x0a\0\x02\x63', address=b'\xcb\0\x71\x42')
                self.reply(frame, query, port=54, address=b'\xcb\0\x71\x42')
                wrong = query[:13] + b'X' + query[14:]
                self.reply(frame, wrong, address=b'\xcb\0\x71\x42')
                self.reply(frame, query)

    def close(self):
        self.running = False
        self.thread.join(timeout=1)
        for t in self.floods:
            t.join(timeout=1)
        self.sock.close()


def dns_bounded_check(q):
    peer = DnsPeer()
    guest = None
    ok = False
    try:
        kernel = q.cmd[q.cmd.index('-kernel') + 1]
        initrd = os.path.join(q.workdir, q.cmd[q.cmd.index('-initrd') + 1].split(',')[0])
        guest = Qemu(kernel, q.cmd[0], '128', peer.args(), initrd=initrd, net=False)
        peer.qport = guest.ports['dns']
        ok = guest.expect(PROMPT, 60)

        def command(line, *expected, timeout=10):
            guest.cursor = len(guest.transcript)
            guest.send(line + '\n')
            return all(guest.expect(e, timeout) for e in expected) and guest.expect(PROMPT, timeout)

        ok = command('ifconfig eth0 10.0.2.15 255.255.255.0 0.0.0.0 8.8.8.8') and ok
        ok = command('nettest resolve offroute.test', 'errno=101') and ok
        ok = command('ifconfig eth0 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3') and ok
        ok = command('nettest resolve answer.test', 'resolve answer.test: 192.0.2.7') and ok
        ok = command('nslookup answer.test', '192.0.2.7') and ok
        ok = command('ping answer.test 1', '1 received') and ok
        ok = command('nettest resolve missing.test', 'errno=2') and ok
        guest.cursor = len(guest.transcript)
        guest.send('nettest resolve concurrent1.test &\n')
        ok = guest.expect('] started', 10) and guest.expect(PROMPT, 10) and ok
        guest.send('nettest resolve concurrent2.test\n')
        resolved = re.compile(rb'resolve concurrent[12]\.test: 192\.0\.2\.7')
        ok = guest.expect(resolved, 10) and guest.expect(resolved, 10) and ok
        ok = len(peer.ids) == 2 and peer.ids[0] != peer.ids[1] and ok
        guest.expect(PROMPT, 10)
        guest.cursor = len(guest.transcript)
        start = time.monotonic()
        flood_start = len(guest.transcript)
        guest.send('nettest resolve flood.test\n')
        ok = guest.expect('errno=110', 8) and guest.expect(PROMPT, 3) and ok
        elapsed = time.monotonic() - start
        # Assert guest ticks, too: wall time includes emulator scheduling.
        duration = re.search(r'resolve flood.test:.*in (\d+) ms', guest.transcript[flood_start:].decode(errors='replace'))
        ok = elapsed < 7 and duration is not None and 5000 <= int(duration[1]) <= 5100 and ok
        ok = command('nettest resolve answer.test', 'resolve answer.test: 192.0.2.7') and ok
        # 10/15/26 (review M1): the IDs and source ports are random, not one more each lookup (as from 0x3a7c
        # and 49152 before); a counter passes neither test, random values fail it about once in 2^100
        steps = list(zip(peer.lookups, peer.lookups[1:]))
        ok = len(peer.lookups) >= 7 and all(port >= 49152 for port, _ in peer.lookups) and ok
        ok = any((b[1] - a[1]) & 65535 != 1 for a, b in steps) and any((b[0] - a[0]) & 65535 != 1 for a, b in steps) and ok
    finally:
        if guest:
            if not ok:
                print('\n---- bounded DNS guest transcript ----\n' + tail(guest.text(), 35))
                print('DNS lookups (source port, ID): ' + ', '.join('%d/%04x' % k for k in peer.lookups))
            guest.close()
        peer.close()
    return ('DNS: isolated peer, source/question filters, concurrent IDs, 5-second flood deadline, legacy commands, '
            'random IDs and ports', ok)


# 10/6/26 (the telnet server, track N): nettest (user/bin/nettest.c) and the host
def entropy_supported_check(q):
    """Positive entropy CPU is explicit and isolated from the default baseline.
    Two user processes request entropy while a third joins the simulated WPA2 AP.
    """
    guest = None
    try:
        kernel = q.cmd[q.cmd.index('-kernel') + 1]
        initrd = os.path.join(q.workdir, q.cmd[q.cmd.index('-initrd') + 1].split(',')[0])
        guest = Qemu(kernel, q.cmd[0], '128', ['-cpu', 'max,+rdrand'],
                     initrd=initrd, append='wlansim')
        ok = guest.expect(PROMPT, 30)
        guest.send('wifi up\n')
        ok = guest.expect('wlan0: up, not associated', 10) and guest.expect(PROMPT, 10) and ok
        start = len(guest.transcript)
        guest.send('nettest entropy supported &\n')
        ok = guest.expect('entropy workers running', 15) and ok
        guest.send('wifi join SurfSimWPA\n')
        ok = guest.expect('Password for SurfSimWPA: ', 15) and ok
        guest.send(WLANSIM_PASSPHRASE + '\n')
        ok = guest.expect('joined SurfSimWPA', 30) and ok
        if b'ENTROPY PASS' not in guest.transcript[start:]:
            ok = guest.expect('ENTROPY PASS', 20) and ok
        guest.pump(0.5)
        transcript = guest.transcript[start:]
        running, joined, finished = (transcript.find(t) for t in
                                    (b'entropy workers running', b'joining SurfSimWPA', b'ENTROPY PASS'))
        ok = (0 <= running < joined < finished and b'ENTROPY FAIL' not in transcript
              and not any(bad in guest.text() for bad in BAD) and guest.alive() and ok)
        if not ok:
            print('\n---- entropy positive CPU transcript ----\n' + guest.text())
        return ('entropy: explicit RDRAND CPU, concurrent callers and WPA2 activity', ok)
    finally:
        if guest:
            guest.close()


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
    pids = re.findall(r'\] started \(pid (\d+)\)', q.transcript[start:].decode(errors='replace'))   # 10/24/26: jobs
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


CONSOLE_CHECKS['@dns-bounded'] = dns_bounded_check
CONSOLE_CHECKS['@entropy-supported'] = entropy_supported_check
CONSOLE_CHECKS['@nettest-host'] = nettest_host_check
CONSOLE_CHECKS['@nettest-kills'] = nettest_kills_check
TIMEOUTS.update({'nettest connect 10.0.2.99 23': 30, 'nettest limits': 60})

# 10/9/26 (the development loop, track D): rx (user/bin/rx.c) and the host, through the forwarded port RX_PORT
RX_FIRST = b''.join(b'rx test %06d ' % i + bytes(range(i % 7, 240, 7)) for i in range(7000))[:300000]
RX_SECOND = b'the second push, in place of the first\n' * 25


def rx_connection(q, timeout=10):
    """the host's connection to rx through the forwarded port (rx listens already)"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', q.ports['rx']), timeout=3)
            s.settimeout(20)
            return s
        except OSError:
            q.pump(0.2)
    return None


def rx_send(q, path, data, reset_after=None):
    """run rx <port> <path> on the console and send it data (or, with reset_after, that much and then a reset);
    True once rx listened and took the connection"""
    q.send(f'run rx {RX_PORT} {path}\n')
    if not q.expect(f'rx: listening on port {RX_PORT} for {path}', 10):
        return False
    s = rx_connection(q)
    if not s:
        return False
    try:
        if reset_after is None:
            s.sendall(data)
            s.shutdown(socket.SHUT_WR)
            while s.recv(4096):                 # rx closes once the file has its name
                pass
        else:
            s.sendall(data[:reset_after])
            q.wait(1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    except OSError:
        pass
    s.close()
    return True


def rx_said(data, path):
    return f'rx: {len(data)} bytes, crc32 {zlib.crc32(data) & 0xFFFFFFFF:08x} -> {path}'


def rx_push_check(q):
    """300 KB into /hda1/RX.BIN, their length and CRC-32 said; then a smaller file in its place; no .part left"""
    ok = rx_send(q, '/hda1/RX.BIN', RX_FIRST) and q.expect(rx_said(RX_FIRST, '/hda1/RX.BIN'), 60)
    ok = ok and q.expect('exit status 0', 10) and q.expect(PROMPT, 10)
    q.send('stat /hda1/RX.BIN\n')
    ok = q.expect(f'file, {len(RX_FIRST)} bytes', 10) and q.expect(PROMPT, 10) and ok
    ok = ok and rx_send(q, '/hda1/RX.BIN', RX_SECOND) and q.expect(rx_said(RX_SECOND, '/hda1/RX.BIN'), 60)
    ok = ok and q.expect('exit status 0', 10) and q.expect(PROMPT, 10)
    q.send('stat /hda1/RX.BIN\n')
    ok = q.expect(f'file, {len(RX_SECOND)} bytes', 10) and q.expect(PROMPT, 10) and ok
    q.send('ls /hda1/RX.BIN.part\n')
    ok = q.expect('No such file or directory', 10) and q.expect(PROMPT, 10) and ok
    return ('rx: 300 KB through the forwarded port with their CRC-32, then another file in their place', ok)


def rx_reset_check(q):
    """a connection reset after 50 KB: rx says so (status 1), the file before is as it was, nothing partial"""
    ok = rx_send(q, '/hda1/RX.BIN', RX_FIRST, reset_after=50000)
    ok = ok and q.expect('rx: the connection, after ', 20) and q.expect('Connection reset', 10)
    ok = ok and q.expect('exit status 1', 10) and q.expect(PROMPT, 10)
    q.send('stat /hda1/RX.BIN\n')
    ok = q.expect(f'file, {len(RX_SECOND)} bytes', 10) and q.expect(PROMPT, 10) and ok
    q.send('ls /hda1/RX.BIN.part\n')
    ok = q.expect('No such file or directory', 10) and q.expect(PROMPT, 10) and ok
    return ('rx: a connection reset in the middle: status 1, the file before kept, nothing partial left', ok)


def rx_busy_check(q):
    """rx on a port another program listens on (nettest echo): in use, status 1, nothing left behind; the host's
    connection to nettest then lets it end"""
    start = len(q.transcript)
    q.send(f'nettest echo {RX_PORT} 1 &\n')
    ok = q.expect('] started', 10) and q.expect(PROMPT, 10)
    # 10/31/26: "started" and the prompt come before nettest has listened: rx could take the port first
    listening = f'nettest: listening on port {RX_PORT}'.encode()
    deadline = time.time() + 10
    while ok and listening not in q.transcript[start:] and time.time() < deadline:
        q.pump(0.1)
    ok = ok and listening in q.transcript[start:]
    q.send(f'run rx {RX_PORT} /hda1/BUSY.BIN\n')
    ok = q.expect(f'rx: port {RX_PORT} is in use', 10) and q.expect('exit status 1', 10) and q.expect(PROMPT, 10) and ok
    q.send('ls /hda1/BUSY.BIN.part\n')
    ok = q.expect('No such file or directory', 10) and q.expect(PROMPT, 10) and ok
    s = rx_connection(q)
    if s:
        s.close()
    ok = ok and s is not None and q.expect('0 bytes echoed', 10)
    q.send('ls /hda1/RX.BIN\n')                # surfsh collects nettest from the background
    ok = q.expect(PROMPT, 10) and ok
    return ('rx: a port in use is refused (status 1), nothing left behind', ok)


CONSOLE_CHECKS.update({'@rx-push': rx_push_check, '@rx-reset': rx_reset_check, '@rx-busy': rx_busy_check})

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


for _name in TCP_RAW:
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
               ('the file rx received last is exact (mtools)', read('RX.BIN') == RX_SECOND),     # 10/9/26
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


# ---- 10/9/26 (Wi-Fi, track S): the simulated Wi-Fi network (@boot wlansim net) ------------------------------------

def tx_packets(q, dev):
    """dev's TX packet count, as netstat says it"""
    start = len(q.transcript)
    q.send('netstat\n')
    if not q.expect(PROMPT, 10):
        return None
    m = re.search(dev + r': .*?TX packets (\d+)', q.transcript[start:].decode(errors='replace'), re.S)
    return int(m.group(1)) if m else None


def telnet_echo(port, words):
    """a telnet session through a forwarded port: the banner and the prompt, echo each word, exit; True if all came"""
    try:
        t = Telnet(port)
    except OSError as e:
        print(f'telnet: {e}')
        return False
    try:
        ok = t.expect("Brandon Burr's SurfOS", 15) and t.expect(TELNET_ROOT, 10)
        for w in words:
            t.send(f'echo {w}\n')
            ok = ok and t.expect(f'\r\n{w}\r\n', 10) and t.expect(TELNET_ROOT, 10)
        t.send('exit\n')
        return ok and t.wait_closed(10)
    finally:
        t.close()


def wlansim_telnet_check(q):
    """eth0 (10.0.2.15) and wlan0 (10.0.2.16) on one network, telnetd listening: a session made to wlan0's address works,
    so its segments leave through wlan0 from wlan0's address (through eth0, the first interface, they would come from
    10.0.2.15 and the connection would never open); one made to eth0's address works while wlan0 sends nothing, as the
    integrator's telnet session to the laptop keeps to eth0 whatever wlan0 does"""
    via_wlan = telnet_echo(q.ports['telnet_wlan'], ['through-wlan0-1', 'through-wlan0-2'])
    q.wait(1)                                           # the connection's last segments
    before = tx_packets(q, 'wlan0')
    via_eth = telnet_echo(q.ports['telnet'], ['through-eth0-1', 'through-eth0-2', 'through-eth0-3'])
    after = tx_packets(q, 'wlan0')
    quiet = before is not None and before == after
    if not (via_wlan and via_eth and quiet):
        print(f'\nwlansim-telnet: to wlan0 {via_wlan}, to eth0 {via_eth}, wlan0 TX packets {before} then {after}')
    return via_wlan and via_eth and quiet


def wlansim_wpa_check(q):
    """WPA2 with SurfSimWPA: until track C's supplicant (net/wlan/wpa.c) and crypto are in the kernel, the join is
    refused saying so, without asking for a password. With them: a wrong password at the hidden prompt fails (the access
    point never takes message 2), the right one joins, a lease and a ping through wlan0, then the access point's group
    key renewal 3 s later; no passphrase ever shows (the block's absent list)"""
    start = len(q.transcript)
    q.send('wifi join SurfSimWPA\n')
    if not q.expect(re.compile(rb'Password for SurfSimWPA: |does not have yet'), 30):
        print('\nwlansim-wpa: neither the password prompt nor the refusal')
        return False
    if b'does not have yet' in q.transcript[start:]:
        ok = q.expect(PROMPT, 10)
        print('  wlansim-wpa: this kernel has no WPA2 supplicant yet: the join is refused, as it should be')
        return ok
    q.send(WLANSIM_WRONG + '\n')
    ok = q.expect('the password may be wrong', 40) and q.expect(PROMPT, 10)
    q.send('wifi join SurfSimWPA\n')
    ok = ok and q.expect('Password for SurfSimWPA: ', 30)
    q.send(WLANSIM_PASSPHRASE + '\n')
    ok = ok and q.expect('wlan0: link up: "SurfSimWPA" (02:53:49:4d:00:02), channel 36, 5 GHz', 40) and \
        q.expect('joined SurfSimWPA', 10) and q.expect(PROMPT, 10)
    for cmd, exp in (('dhcp wlan0', ['dhcp: wlan0: lease 10.0.2.16']),
                     ('ping 10.0.2.2 2 wlan0', ['from 10.0.2.16 (wlan0)', '2 packets transmitted, 2 received'])):
        q.send(cmd + '\n')
        ok = ok and all(q.expect(e, 15) for e in exp) and q.expect(PROMPT, 10)
    q.wait(4)
    q.send('dmesg\n')
    ok = ok and q.expect('SurfSimWPA: 4-way handshake done', 10) and q.expect('wlan0: new group key (index 2)', 10) and \
        q.expect('group key handshake done (key 2, its message 2 protected)', 10) and q.expect(PROMPT, 10)
    if not ok:
        print('\nwlansim-wpa: the WPA2 join, its lease, ping or group key renewal failed')
    return ok


def usbnet_later_boot(kernel, qemu, mem, extra, initrd, append):
    """10/9/26 (the review of track S, M1): a boot in the laptop's order: no e1000, an xHCI whose USB Ethernet adapter
    (QEMU's usb-net on its user network) is plugged in after boot (BOOT_CHECKS usbnet-plug), so that wlan0, which the
    simulated chip registers at boot, comes before eth0. Port 23 of 10.0.2.16 is forwarded as 'telnet' (eth0's lease
    here) and of 10.0.2.15 as 'telnet_wlan' (wlan0 asks first, and gets .15)"""
    return Qemu(kernel, qemu, mem, list(extra) + ['-device', 'qemu-xhci,id=xhci', '-netdev',
                'user,id=n0,hostfwd=tcp:127.0.0.1:{port:telnet}-10.0.2.16:23,'
                'hostfwd=tcp:127.0.0.1:{port:telnet_wlan}-10.0.2.15:23'],   # (10/18/26: ports picked by Qemu: fs-fix-11)
                None, None, None, initrd, False, False, ' '.join(append), 0)


def usbnet_plug_check(q):
    """the USB adapter plugged in after boot: QEMU's usb-net on the xHCI becomes eth0, after wlan0"""
    q.monitor('device_add usb-net,bus=xhci.0,netdev=n0,id=usbnet0,mac=52:54:00:12:34:56')
    ok = q.expect('eth0, cdc-ecm', 30)
    q.wait(2)                                           # its link
    if not ok:
        print('\nusbnet-plug: the usb-net plugged in did not become eth0')
    return ok


def wlansim_tx_race_check(q):
    """(review M3) wlan0 busy sending, a telnet session to wlan0's address printing as fast as it can, while wifi leave
    takes the link away; the simulated chip says so if a frame reaches it after leave() (the block's absent list). Then
    that session's connection, whose address wlan0 forgot, sends nothing through eth0 either"""
    try:
        t = Telnet(q.ports['telnet_wlan'])
    except OSError as e:
        print(f'\nwlansim-tx-race: {e}')
        return False
    try:
        ok = t.expect(TELNET_ROOT, 15)
        t.send('count 1000000 0\n')
        ok = ok and t.expect(' 500 ', 15)               # it streams through wlan0
        q.send('wifi leave\n')
        ok = q.expect('wlan0: not associated', 15) and q.expect(PROMPT, 10) and ok
    finally:
        t.close()
    # the session's connection is still there, its address gone with wlan0's: nothing of it goes out through eth0
    # under eth0's address (ip_route() finds no interface for it)
    q.wait(1)
    before = tx_packets(q, 'eth0')
    q.wait(3)
    after = tx_packets(q, 'eth0')
    quiet = before is not None and before == after
    if not (ok and quiet):
        print(f'\nwlansim-tx-race: stream and leave {ok}; eth0 TX packets {before} then {after}')
    return ok and quiet


def wlansim_scan_abort_check(q):
    """(review L2; wifi's Ctrl+C and busy paths) the list emptied (wifi down, up); wifi scan cut short by Ctrl+C: the
    simulated chip reports the end of that aborted scan 300 ms later, during the next scan, which must not end early (it
    lists the six networks, though the aborted scan heard none). Then wifi up on the console while a telnet session's
    scan runs is told the device is busy"""
    ok = True
    for line, exp in (('wifi down', 'wlan0: down'), ('wifi up', 'wlan0: up, not associated')):
        q.send(line + '\n')
        ok = ok and q.expect(exp, 15) and q.expect(PROMPT, 10)
    q.send('wifi scan\n')
    q.wait(0.2)
    q.ser.sendall(b'\x03')
    cut = q.expect('wifi: scan cut short', 10) and q.expect(PROMPT, 10)
    start = len(q.transcript)
    q.send('wifi scan\n')
    rescan = q.expect(PROMPT, 15) and b'6 networks' in q.transcript[start:]
    busy = False
    try:
        t = Telnet(q.ports['telnet'])
        try:
            if t.expect(TELNET_ROOT, 15):
                t.send('wifi scan\n')
                time.sleep(0.15)
                q.send('wifi up\n')
                busy = q.expect('wifi: busy: another wifi command is running on this device', 10) and q.expect(PROMPT, 10)
                busy = busy and t.expect('6 networks', 15) and t.expect(TELNET_ROOT, 10)
                t.send('exit\n')
                t.wait_closed(10)
        finally:
            t.close()
    except OSError as e:
        print(f'\nwlansim-scan-abort: telnet: {e}')
    if not (ok and cut and rescan and busy):
        print(f'\nwlansim-scan-abort: down/up {ok}, cut short {cut}, the scan after it listed all {rescan}, busy {busy}')
    return ok and cut and rescan and busy


def wlansim_ctrl_c_check(q):
    """(review test gaps: wifi's Ctrl+C paths) Ctrl+C at the hidden password prompt of a WPA2 join, on the console
    and in a telnet session: the join is cut short with nothing typed, nothing is joined, and the station is as it was
    (up, not associated). Without track C's supplicant there is no prompt, and nothing to check"""
    start = len(q.transcript)
    q.send('wifi join SurfSimWPA\n')
    if not q.expect(re.compile(rb'Password for SurfSimWPA: |does not have yet'), 30):
        print('\nwlansim-ctrl-c: neither the password prompt nor the refusal')
        return False
    if b'does not have yet' in q.transcript[start:]:
        return q.expect(PROMPT, 10)
    q.wait(0.2)
    q.ser.sendall(b'\x03')
    console = q.expect('wifi: join cut short', 10) and q.expect(PROMPT, 10)
    remote = False
    try:
        t = Telnet(q.ports['telnet'])
        try:
            if t.expect(TELNET_ROOT, 15):
                t.send('wifi join SurfSimWPA\n')
                if t.expect('Password for SurfSimWPA: ', 30):
                    time.sleep(0.2)
                    t.send(b'\x03')
                    remote = t.expect('wifi: join cut short', 10) and t.expect(TELNET_ROOT, 10)
                t.send('exit\n')
                t.wait_closed(10)
        finally:
            t.close()
    except OSError as e:
        print(f'\nwlansim-ctrl-c: telnet: {e}')
    start = len(q.transcript)
    q.send('wifi status\n')
    idle = q.expect(PROMPT, 10) and b'wlan0: up, not associated' in q.transcript[start:]
    if not (console and remote and idle):
        print(f'\nwlansim-ctrl-c: on the console {console}, over telnet {remote}, not associated afterwards {idle}')
    return console and remote and idle


def wlan_own_frames_check(q):
    """(review test gaps) the simulated access point sends wlan0's broadcasts back to it, as an access point sends them
    to its whole network: wlan0 dropped them (wifi status counts them), so its own gratuitous ARP (dhcp's announcement)
    never taught the ARP table that wlan0's address 10.0.2.16 is at wlan0's own MAC address"""
    start = len(q.transcript)
    q.send('wifi status\n')
    ok = q.expect(PROMPT, 10)
    m = re.search(r'(\d+) own broadcasts the access point sent back', q.transcript[start:].decode(errors='replace'))
    own = int(m.group(1)) if m else 0
    start = len(q.transcript)
    q.send('arp\n')
    ok = ok and q.expect(PROMPT, 10)
    learned = [line for line in q.transcript[start:].decode(errors='replace').splitlines()
               if '10.0.2.16 ' in line and '52:54:00:12:34:57' in line]
    if not (ok and own >= 1 and not learned):
        print(f'\nwlan-own-frames: own broadcasts sent back and dropped {own}, ARP entries for ourselves {learned}')
    return ok and own >= 1 and not learned


def eth0_held_check(q):
    """(review test gaps) a telnet session to eth0's address, opened at the first call and held open through what wlan0
    does next (a join that forgets its old address, a lease, the link going down and up twice, wifi leave, wifi down): it
    must answer at every call, as the integrator's session to the laptop over eth0 must whatever Wi-Fi does"""
    t = getattr(q, 'held', None)
    if t is None:
        try:
            t = q.held = Telnet(q.ports['telnet'])
        except OSError as e:
            print(f'\neth0-held: telnet: {e}')
            return False
        if not (t.expect("Brandon Burr's SurfOS", 15) and t.expect(TELNET_ROOT, 10)):
            print('\neth0-held: no banner or prompt from the session to eth0')
            return False
    q.held_n = getattr(q, 'held_n', 0) + 1
    word = f'held-through-wlan0-{q.held_n}'
    t.send(f'echo {word}\n')
    ok = t.expect(f'\r\n{word}\r\n', 15) and t.expect(TELNET_ROOT, 10)
    if not ok:
        print(f'\neth0-held: the session to eth0 did not answer ({word})')
    return ok


def eth0_held_end_check(q):
    """the held session answers once more, and ends when asked"""
    t = getattr(q, 'held', None)
    if t is None:
        print('\neth0-held-end: no session held')
        return False
    ok = eth0_held_check(q)
    t.send('exit\n')
    ok = t.wait_closed(10) and ok
    t.close()
    q.held = None
    return ok


def dhcp_one_at_a_time_check(q):
    """(review nit: there is one port 68) wlan0 on SurfSimHidden's island, where no DHCP server answers: dhcp wlan0 in
    a telnet session asks for its 6 s and gets no lease; dhcp typed on the console meanwhile waits for it, says so, and
    gets eth0's lease after it (before, it failed at once: port 68 is busy)"""
    console = remote = False
    try:
        t = Telnet(q.ports['telnet'])
    except OSError as e:
        print(f'\ndhcp-one-at-a-time: telnet: {e}')
        return False
    try:
        if t.expect(TELNET_ROOT, 15):
            t.send('dhcp wlan0\n')
            time.sleep(2)
            q.send('dhcp\n')
            console = q.expect('dhcp: eth0: waiting for the dhcp on wlan0', 10) and \
                q.expect('dhcp: eth0: lease 10.0.2.16', 15) and q.expect(PROMPT, 10)
            remote = t.expect('dhcp: no lease', 15) and t.expect(TELNET_ROOT, 10)
            t.send('exit\n')
            t.wait_closed(10)
    finally:
        t.close()
    if not (console and remote):
        print(f'\ndhcp-one-at-a-time: eth0 waited, then had its lease {console}; wlan0 had none, alone {remote}')
    return console and remote


def wlan_no_drops_check(q):
    """(review L1) after 10.0.2.77's ARP bursts, two A-MSDUs of 64 replies each, more than one device's share of the
    receive queue: wlan0 lost none (netstat's RX dropped), the station's thread having waited for room"""
    start = len(q.transcript)
    q.send('netstat\n')
    ok = q.expect(PROMPT, 10)
    out = q.transcript[start:].decode(errors='replace')
    m = re.search(r'wlan0: .*?RX packets (\d+) bytes \d+ dropped (\d+)', out, re.S)
    good = ok and m is not None and int(m.group(1)) >= 128 and m.group(2) == '0'
    if not good:
        print('\nwlan-no-drops: wlan0 lost frames of the bursts, or they never came:\n' + out)
    return good


def wlan_forgotten_check(q):
    """(review M2) after wlan0 left its network: no address on it (wifi status) and no ARP entry through it"""
    start = len(q.transcript)
    q.send('arp\n')
    ok = q.expect(PROMPT, 10)
    arp = q.transcript[start:].decode(errors='replace')
    start = len(q.transcript)
    q.send('wifi status\n')
    ok = q.expect(PROMPT, 10) and ok
    status = q.transcript[start:].decode(errors='replace')
    good = ok and 'wlan0' not in arp and 'inet 0.0.0.0' in status
    if not good:
        print('\nwlan-forgotten: wlan0 kept an ARP entry or its address:\n' + arp + status)
    return good


def keeper_skips_open_check(q):
    """10/17/26 (the network manager): the saved WPA2 network switched off (wlansim off): the station gives up after its
    reconnects (about 50 s), the keeper looks for a saved network, and does not join SurfSim by itself, an open network,
    while the telnet server runs (telnetd has no password)"""
    ok = (q.expect('gave up reconnecting to "SurfSimWPA" after 5 tries', 90) and
          q.expect('+NET: wlan0: SurfSimWPA is gone: looking for a saved network', 10) and
          q.expect('+NET: wlan0: SurfSim is in range, but it is open and a telnet server runs or is to start: not joined '
                   'by itself (wifi join SurfSim joins it)', 20) and
          q.expect('+NET: wlan0: no saved network joined; looking again every 30 s', 10))
    if not ok:
        print('\nkeeper-skips-open: the keeper did not set the open network aside:\n' + tail(q.text(), 20))
    return ok


def keeper_joins_open_check(q):
    """10/17/26: with telnetd stopped, the keeper's next look (30 s after the last at most) joins SurfSim and asks for
    its address"""
    ok = (q.expect('wlan0: link up: "SurfSim"', 45) and
          q.expect('+NET: wlan0: joined SurfSim; asking for an address (DHCP)', 10) and
          q.expect('dhcp: wlan0: lease 10.0.2.15', 30))
    if not ok:
        print('\nkeeper-joins-open: the keeper did not join SurfSim once telnetd was off:\n' + tail(q.text(), 20))
    return ok


def hidden_no_address_check(q):
    """10/17/26: no DHCP server on SurfSimHidden's island: after its three tries (about 30 s) the keeper says it asks
    again later"""
    ok = q.expect('+NET: wlan0: no address on SurfSimHidden after 3 tries; asking again in 30 s, then less often (dhcp '
                  'wlan0 asks now)', 45)
    if not ok:
        print('\nhidden-no-address: the keeper did not say it asks again:\n' + tail(q.text(), 20))
    return ok


def renewed_again_check(q):
    """10/17/26: a lease of 8 s (dhcplease=8) renewed again and again, seen as it happens (two more renewals)"""
    ok = (q.expect('dhcp: eth0: lease 10.0.2.15 renewed for 86400 s', 15) and
          q.expect('dhcp: eth0: lease 10.0.2.15 renewed for 86400 s', 15))
    if not ok:
        print('\nrenewed-again: the lease was not renewed twice more:\n' + tail(q.text(), 20))
    return ok


def link_bounce_check(q):
    """10/17/26: eth0's link down and up again (QEMU's set_link) while it has a lease: the network manager asks for the
    same address again (DHCP INIT-REBOOT), and the server confirms it"""
    q.monitor('set_link net0 off')
    q.wait(3)
    q.monitor('set_link net0 on')
    ok = q.expect('dhcp: eth0: lease 10.0.2.15 still good for 86400 s', 20)
    if not ok:
        print('\nlink-bounce: the address was not confirmed after the link came back:\n' + tail(q.text(), 20))
    return ok


# ---- 10/25/26 (robustness, L22-L24): a clean reboot and poweroff -------------------------------------------------

SHUTDOWN_FILES = {'reboot': 'REBOOT.TXT', 'poweroff': 'POWEROFF.TXT', 'key': 'CADEL.TXT'}


def shutdown_boot_check(q, how):
    """a file written on the stick, then reboot, poweroff or Ctrl+Alt+Del (the monitor's sendkey): the shutdown says
    each step (the shells ended, /hda1 written out and unmounted, the reset or ACPI S5) and QEMU stops: -no-reboot and
    -no-shutdown make a reset and a power-off alike a pause, \"paused (shutdown)\". 'laptop': poweroff with no disk"""
    ok = True
    if how != 'laptop':
        q.send(f'echo {how} > /hda1/{SHUTDOWN_FILES[how]}\n')
        ok = q.expect(PROMPT, 10)
    start = len(q.transcript)
    if how == 'key':
        q.monitor('sendkey ctrl-alt-delete')
        ok = ok and q.expect('Ctrl+Alt+Del: SurfOS shuts down, then resets the machine', 10)
    else:
        q.send(('poweroff' if how == 'laptop' else how) + '\n')
    what = 'poweroff' if how in ('poweroff', 'laptop') else 'reboot'
    # the last line ("resetting the machine", "turning the machine off (ACPI S5)") may be cut by the reset or the
    # power-off itself (QEMU drops what its UART still holds): QEMU's stop, and no failure line, say it worked
    ok = (ok and q.expect(f'shutdown: {what}: the programs end, the volumes are written out', 10) and
          q.expect('programs and shells ended', 20) and
          (how == 'laptop' or q.expect('shutdown: /hda1 written out and unmounted', 20)))
    stopped, deadline = False, time.time() + 15
    while ok and not stopped and time.time() < deadline:
        q.pump(0.3)
        stopped = 'VM status: paused (shutdown)' in q.monitor('info status')
    text = q.transcript[start:].decode(errors='replace')
    good = ok and stopped and not any(s in text for s in ('still running after', 'could not', 'not cleanly', 'busy',
                                                         'still on 3 s', 'safe to turn off'))
    if not good:
        print(f'\nshutdown-{how}: {"QEMU stopped" if stopped else "QEMU did not stop"}:\n' + tail(q.text(), 25))
    return good


def shutdown_disk_check(q):
    """after the three shutdown boots, the stick read here: FAT[1] says cleanly unmounted, the file each boot wrote
    is there, the owner's file as it was, and fsck.fat (if the host has it) finds no error"""
    import mkusb
    path = SETTINGS_DISKS.get('shutdown')
    what = 'shutdown: the stick after reboot, poweroff and Ctrl+Alt+Del: cleanly unmounted, every file there'
    if not path:
        return (what, False)
    try:
        with open(path, 'rb') as f:
            f.seek(2048 * 512)
            bpb = f.read(512)
            bps, reserved = struct.unpack_from('<H', bpb, 11)[0], struct.unpack_from('<H', bpb, 14)[0]
            f.seek(2048 * 512 + reserved * bps)
            fat1 = struct.unpack_from('<I', f.read(8), 4)[0]
            fat = mkusb.Fat32(f, 2048 * 512)
            files = {how: fat.read(name) for how, name in SHUTDOWN_FILES.items()}
            note = fat.read('notes.txt')
    except Exception as e:
        print(f'\n@shutdown-disk: {e}')
        return (what, False)
    ok = bool(fat1 & 0x08000000) and all(files[how] == how.encode() + b'\n' for how in files) and note == OWNER_NOTE
    if not ok:
        print(f'\n@shutdown-disk: FAT[1] 0x{fat1:08x}, files {files}, notes.txt {note!r}')
    fsck = fsck_check(path, 2048 * 512, (40 << 20) - 2048 * 512)
    return (what, ok and all(r for _, r in fsck))


CONSOLE_CHECKS['@shutdown-disk'] = shutdown_disk_check


BOOT_CHECKS = {'wlansim-telnet': wlansim_telnet_check, 'wlansim-wpa': wlansim_wpa_check, 'usbnet-plug': usbnet_plug_check,
               'wlan-forgotten': wlan_forgotten_check, 'wlansim-tx-race': wlansim_tx_race_check,
               'wlan-no-drops': wlan_no_drops_check, 'wlansim-scan-abort': wlansim_scan_abort_check,
               'wlansim-ctrl-c': wlansim_ctrl_c_check, 'wlan-own-frames': wlan_own_frames_check,
               'eth0-held': eth0_held_check, 'eth0-held-end': eth0_held_end_check,
               'dhcp-one-at-a-time': dhcp_one_at_a_time_check, 'keeper-skips-open': keeper_skips_open_check,
               'keeper-joins-open': keeper_joins_open_check, 'renewed-again': renewed_again_check,
               'hidden-no-address': hidden_no_address_check,
               'link-bounce': link_bounce_check,
               'shutdown-reboot': lambda q: shutdown_boot_check(q, 'reboot'),         # 10/25/26 (L22-L24)
               'shutdown-poweroff': lambda q: shutdown_boot_check(q, 'poweroff'),
               'shutdown-key': lambda q: shutdown_boot_check(q, 'key'),
               'shutdown-laptop': lambda q: shutdown_boot_check(q, 'laptop')}


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


def fill_ports(line, ports):
    """10/10/26 (review nit): {name} for a forwarded port (http, telnet, ...) becomes its number; any other brace stays
    as typed (str.format() took every brace for a field of its own: a line with one failed, or lost it)"""
    return re.sub(r'\{(\w+)\}', lambda m: str(ports[m.group(1)]) if m.group(1) in ports else m.group(0), line)


def boot_expect(q, text, timeout):
    """10/25/26: text in a boot's serial output; a kernel message from the network manager (+NET:) not seen there whole
    is looked for in the kernel log (dmesg) instead. kprintf puts a message out a character at a time with no lock, and
    surfsh's banner, printed at about the same moment, can cut it in two on the console (the banner race: the line is
    missed, every later step of that boot runs without waiting, and the boots after it fail too); the kernel log holds
    it whole. The cursor stays where it was, so what follows is looked for from there as before, and the strings that
    must not appear are looked for without dmesg's output (q.dmesg_spans): the kernel log has lines the console never
    showed (the keeper's repeated tries)"""
    if q.expect(text, timeout):
        return True
    if not (isinstance(text, str) and text.startswith('+NET:')) or not q.alive():
        return False
    cursor = q.cursor
    q.cursor = start = len(q.transcript)
    q.send('dmesg\n')
    found = q.expect(PROMPT, 30) and text.encode() in q.transcript[start:]
    q.__dict__.setdefault('dmesg_spans', []).append((start, len(q.transcript)))
    q.cursor = cursor
    if found:
        print(f'\n(boot: "{text[:50]}..." was cut on the console; whole in dmesg)')
    return found


def boot_step(reboot, entry, timeout):
    """10/6/26: an '@boot' step of SMOKE: a boot of its own (reboot(words) starts it), the strings that must appear,
    lines typed then with what must follow them (and the prompt, unless that is what they end with), and strings
    that must not appear. A line may be a function of the QEMU that returns it, or '@sleep <seconds>'; bytes are sent
    as they are, with no newline and no prompt waited for after them (Ctrl+C, a command that runs on)."""
    cmd, expects, follow, absent = entry[:4]
    halts = len(entry) > 4 and entry[4] == 'halts'         # 10/18/26 (B56): a boot that ends in a wipeout on purpose
    try:
        q2 = reboot(cmd.split()[1:])
    except Exception as e:
        print(f'{cmd}: {e}')
        return (cmd, False)
    try:
        ok = all(boot_expect(q2, e, timeout) for e in expects)
        for line, exp in follow:
            if exp is CHECK:                                 # 10/9/26: a check of its own (BOOT_CHECKS)
                ok = BOOT_CHECKS[line](q2) and ok
                continue
            if isinstance(line, str) and q2.ports:
                line = fill_ports(line, q2.ports)            # 10/9/26: a boot with the network (@boot ... net)
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
        text = q2.text(getattr(q2, 'dmesg_spans', ()))
        # 10/25/26: a fall the step itself expects (crashstack's SYSTEM HALTED) is no fall
        wanted = [e.pattern if isinstance(e, re.Pattern) else e for e in expects] + \
                 [e.pattern if isinstance(e, re.Pattern) else e for _, exp in follow if exp is not CHECK for e in exp]
        bad = [b for b in BAD if not any((b.encode() if isinstance(e, bytes) else b) in e for e in wanted
                                         if isinstance(e, (str, bytes)))]
        ok = ok and not any(a in text for a in absent) and (halts or (not any(b in text for b in bad) and q2.alive()))
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
            cmd = fill_ports(cmd, q.ports) if q.ports else cmd
            limit = TIMEOUTS.get(cmd, TIMEOUTS.get(cmd.split()[0], 10))
            for attempt in (1, 2):
                start = len(q.transcript)
                q.send(cmd + '\n')
                ok = all(q.expect(e, limit) for e in expects)
                for line, exp in follow:
                    if callable(line):                       # 11/01/26: a function of the QEMU, as in an @boot block
                        line = line(q)
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
    mode.add_argument('--entropy-test', action='store_true', help='strict entropy on default and RDRAND CPUs')
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
        surfsh=<program> (the initrd with that program, or with that file of it if the name has a /, as bin/surfsh).
        10/9/26: 'net' gives the boot QEMU's network (and its forwarded ports), and so does autonet, which needs one;
        the others have none. 10/17/26: settingsdisk=<name>, the owner's stick (settings_disk())"""
        initrd, append, net, usbnet, disk = a.initrd, a.append.split(), False, False, None
        tmp = tempfile.mkdtemp(prefix='surfos-initrd-')
        try:
            for w in words:
                if w == 'net':                               # 10/9/26: QEMU's user network, with its forwards
                    net = True
                elif w.startswith('settingsdisk='):          # 10/17/26: the owner's stick, kept from boot to boot
                    disk = settings_disk(w[len('settingsdisk='):])
                    append.append('bootvol=' + SETTINGS_BOOTVOL)
                elif w == 'usbnet-later':                    # 10/9/26: the laptop's order (usbnet_later_boot())
                    usbnet = True
                elif w == 'no-initrd':
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
            if usbnet:
                return usbnet_later_boot(a.kernel, a.qemu, a.mem, a.extra.split(), initrd, append)
            # 10/9/26: autonet needs a network to bring up. 10/17/26: a settings disk is written in place
            return Qemu(a.kernel, a.qemu, a.mem, a.extra.split(), None, None, disk, initrd, disk is not None,
                        net or 'autonet' in append, ' '.join(append), 0)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)       # Qemu() has copied the initrd

    try:
        if a.entropy_test:
            baseline = q.expect(PROMPT, a.boot_timeout)
            q.send('nettest entropy unsupported\n')
            baseline = q.expect('ENTROPY PASS', 15) and q.expect(PROMPT, 10) and baseline
            results = [('entropy: default CPU fails closed', baseline), entropy_supported_check(q)]
            for name, ok in results:
                print(('PASS ' if ok else 'FAIL ') + name)
            if not baseline:
                print(q.text())
            return int(not all(ok for _, ok in results))
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
        for d in SETTINGS_DIR:                               # 10/17/26: the settings disks
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
