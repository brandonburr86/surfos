#!/usr/bin/env python3
"""The development loop: builds from this computer onto the IdeaPad over the network (docs/USB-BOOT.md section 13).

    tools/devloop.py find                   where the laptop answers: --host, else a SurfOS telnet prompt on --net
    tools/devloop.py run <cmd>...           each command in a telnet session; its output printed
    tools/devloop.py log                    the kernel log (dmesg)
    tools/devloop.py deploy [--kernel F] [--initrd F] [--opt 0|2]
                                            a build onto the data stick, a reboot into it, its build id checked
    tools/devloop.py reboot                 a reboot without a push: telnet again, the build id said

The laptop runs SurfOS with telnetd (no login). GRUB's development entry, which boots the build on the data stick,
starts it by itself (the kernel word autonet); after the normal entry someone types dhcp and telnetd on.

deploy builds nothing. It takes surfos.bin and initrd.tar of a build (devenv/build-usb.sh makes both; by default
the newer of build/docker/build/O2 and O0, or build/O2 and O0 on Linux) and, in one telnet session:
  1. lsblk: the data stick is the USB disk that is not the boot drive ("protected") and whose first partition is
     FAT, the partition GRUB's development entry looks at (--stick names it when there are more); never a name or
     a size, since usb0 and usb1 change places between boots. That partition is mounted on /surfos-dev-stick (or
     used where it is mounted already), then mkdir /surfos-dev-stick/surfos-dev;
  2. each file: rx <rx-port> <dir>/<name>.new runs in the session, this computer connects and sends the file, and
     the CRC-32 that rx says must be this file's; then both files are renamed into place (mv), so the stick
     never holds half of a build;
  3. eject, reboot, telnet again within 3 minutes (--net is searched when the address changed), and the new
     system's /initrd/etc/build-id must be the pushed initrd's (uptime says it really restarted).
It never formats or partitions anything: a data stick without a FAT file system stops it with what to do. run
refuses reboot (use reboot or deploy: they wait for the laptop and check it), mkfs and fdisk's changes.

Exit status 0; 1 with the reason. Nothing here writes on this computer.
"""
import argparse
import importlib.util
import ipaddress
import os
import re
import socket
import sys
import tarfile
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_HOST = '10.0.0.4'
DEFAULT_NET = '10.0.0.0/24'
TELNET_PORT = 23
RX_PORT = 5555
STICK_MOUNT = '/surfos-dev-stick'
DEV_DIR = 'surfos-dev'
FILES = ('initrd.tar', 'surfos.bin')            # pushed in this order, renamed into place in this order
REBOOT_WAIT = 180                               # seconds for the laptop to come back
SETTLE = 8                                      # seconds before the first try: the old system goes down first
COLS, ROWS = 200, 500                           # the session's window: long lines whole, few --More--
MAKE_FAT = ('devloop never formats a disk: make the data stick FAT32 on the Mac (Disk Utility: View > Show All '
            'Devices, the stick, Erase, MS-DOS (FAT), Master Boot Record), or on the laptop with fdisk and mkfs '
            '(docs/USB-BOOT.md section 9); either erases it. Then deploy again')
NO_FAT = '{dev} has no FAT file system. ' + MAKE_FAT

_harness = None


def harness():
    """tools/qemu-run.py as a module: its telnet client (Telnet) and the prompt (PROMPT)"""
    global _harness
    if _harness is None:
        spec = importlib.util.spec_from_file_location('qemu_run', os.path.join(REPO, 'tools', 'qemu-run.py'))
        _harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(_harness)
    return _harness


def mkusb():
    spec = importlib.util.spec_from_file_location('mkusb', os.path.join(REPO, 'tools', 'mkusb.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class DevloopError(Exception):
    pass


def say(text):
    print(f'devloop: {text}', flush=True)


# ---- what a session prints -------------------------------------------------------------------------------------

# CSI sequences, OSC strings and the other two-character escapes
ANSI = re.compile(rb'\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|\x1b[@-Z\\-_]')


def clean(data):
    """bytes a session printed as text: escape sequences gone, lines as a terminal leaves them (what a CR
    overwrote is gone: the pager's --More-- is taken away with CR ESC[K), LF line ends"""
    text = ANSI.sub(b'', data).decode('utf-8', 'replace')
    lines = []
    for line in text.split('\n'):
        line = line.rstrip('\r')
        if '\r' in line:
            line = line.rsplit('\r', 1)[1]
        lines.append(line.rstrip())
    return '\n'.join(lines)


# refused in run: deploy and reboot send reboot themselves and wait; nothing formats or partitions the laptop's disks;
# and telnetd stays on (without it nobody but the owner, at the laptop, could start it again)
REFUSED = [(re.compile(r'^(?:run\s+|spawn\s+)?reboot\b'), 'reboot: use tools/devloop.py reboot (or deploy), which wait '
                                                         'for the laptop and check it'),
           (re.compile(r'^(?:run\s+|spawn\s+)?mkfs\b'), 'mkfs: devloop never formats a disk'),
           (re.compile(r'^(?:run\s+|spawn\s+)?fdisk\s+\S+\s+\S'), 'fdisk with a change: devloop never partitions a disk'),
           (re.compile(r'^(?:run\s+|spawn\s+)?(?:/initrd/bin/)?telnetd\s+off\b'),
            'telnetd off: it would end this loop until someone at the laptop types telnetd on')]


def refused(cmd):
    """why devloop will not run cmd, or None"""
    for part in re.split(r'[;&]', cmd):
        for pattern, why in REFUSED:
            if pattern.search(part.strip()):
                return why
    return None


# ---- a telnet session with surfsh on the laptop ----------------------------------------------------------------

class Session:
    """surfsh in a telnet session (tools/qemu-run.py's Telnet: it negotiates as a terminal's client does and
    answers --More-- with Space). A command runs as "<cmd>; echo <mark> $?", so its end and its status are seen
    whatever it prints."""

    def __init__(self, host, port=TELNET_PORT, timeout=15):
        qr = harness()
        self.host, self.port, self.n = host, port, 0
        try:
            self.t = qr.Telnet(port, cols=COLS, rows=ROWS, host=host, timeout=timeout)
        except OSError as e:
            raise DevloopError(f'no telnet at {host} port {port} ({e.strerror or e})')
        if not self.t.expect(qr.PROMPT, timeout + 10):
            text = clean(self.t.data).strip()
            self.t.close()
            if 'sessions are open already' in text:
                raise DevloopError(f'{host}: telnetd has 4 sessions open already (exit one, or telnetd off and on)')
            raise DevloopError(f'{host} port {port}: no SurfOS prompt' + (f' (it said {text[-120:]!r})' if text else ''))
        self.banner = clean(self.t.data)
        self.cursor = self.t.cursor
        self.pending = None

    def close(self):
        try:
            if not self.t.closed:
                self.t.send('exit\n')
                self.t.wait_closed(3)
        finally:
            self.t.close()

    def _pump_until(self, test, timeout):
        """pump until test() is true (its value), the time is up or the session ends (None)"""
        deadline = time.time() + timeout
        while True:
            got = test()
            if got:
                return got
            if time.time() > deadline or self.t.closed:
                return None
            self.t.pump(0.1)

    def start(self, cmd):
        """type cmd (one line); finish() collects its output"""
        if '\n' in cmd or '\r' in cmd:
            raise DevloopError('one line at a time')
        self.n += 1
        mark = f'devloop-end-{os.getpid()}-{self.n}'
        self.pending = (re.compile(re.escape(mark.encode()) + rb' (\d+)\r?\n'), len(self.t.data))
        self.t.send(f'{cmd}; echo {mark} $?\n')

    def wait(self, pattern, timeout):
        """a match of pattern (bytes) in what the command printed so far, or None when it ended first"""
        end, start = self.pending
        rx = re.compile(pattern)

        def seen():
            m = rx.search(self.t.data, start)
            return m or (end.search(self.t.data, start) and 'ended')
        got = self._pump_until(seen, timeout)
        return got if got and got != 'ended' else None

    def finish(self, timeout):
        """(status, output) of the command started: status None when it did not run (surfsh refused the line:
        its next prompt came and stayed without the end mark), output without the typed line"""
        end, start = self.pending
        qr = harness()
        quiet = {'size': len(self.t.data), 'since': time.time()}

        def done():
            m = end.search(self.t.data, start)
            if m:
                return m
            if len(self.t.data) != quiet['size']:
                quiet['size'], quiet['since'] = len(self.t.data), time.time()
                return None
            line_end = self.t.data.find(b'\n', start)
            if line_end >= 0 and qr.PROMPT.search(self.t.data, line_end) and time.time() - quiet['since'] > 1.5:
                return 'refused'
            return None
        got = self._pump_until(done, timeout)
        self.pending = None
        if got is None:
            raise DevloopError('the session ended' if self.t.closed else f'no answer within {timeout:.0f} s')
        line_end = self.t.data.find(b'\n', start) + 1
        if got == 'refused':
            out, status = self.t.data[line_end:], None
            self.cursor = len(self.t.data)
        else:
            out, status = self.t.data[line_end:got.start()], int(got.group(1))
            self.cursor = got.end()
            self._pump_until(lambda: qr.PROMPT.search(self.t.data, self.cursor), 10)   # its prompt, before the next
        if status is None:
            out = qr.PROMPT.split(out)[0] if qr.PROMPT.search(out) else out
        return status, clean(out).strip('\n')

    def run(self, cmd, timeout=60):
        self.start(cmd)
        return self.finish(timeout)

    def check(self, cmd, timeout=60, ok=None):
        """run cmd; its output when its status is 0 (or ok(status, output) says so), else DevloopError"""
        status, out = self.run(cmd, timeout)
        good = ok(status, out) if ok else status == 0
        if good:
            return out
        raise DevloopError(f'{cmd}: ' + (f'status {status}' if status is not None else 'not run') +
                           (f': {out.strip()}' if out.strip() else ''))

    def uptime(self):
        """seconds since the laptop started"""
        m = re.search(r'up (\d+)\.(\d+) s', self.check('uptime'))
        if not m:
            raise DevloopError('uptime said no time')
        return int(m.group(1)) + int(m.group(2)) / 100

    def build_id(self):
        out = self.check('cat /initrd/etc/build-id', ok=lambda s, o: True).strip()
        return out if out and 'No such file' not in out else None


# ---- finding the laptop ------------------------------------------------------------------------------------

def answers(host, port, timeout=3):
    """a SurfOS prompt at host:port (the session is ended again)"""
    try:
        Session(host, port, timeout).close()
        return True
    except DevloopError:
        return False


def scan(net, port, exclude=(), timeout=1.0):
    """the first host on net (not in exclude) with a SurfOS telnet prompt on port, or None"""
    network = ipaddress.ip_network(net, strict=False)
    hosts = [str(h) for h in network.hosts()] or [str(network.network_address)]
    hosts = [h for h in hosts if h not in exclude]

    def open_port(h):
        try:
            socket.create_connection((h, port), timeout).close()
            return h
        except OSError:
            return None
    with ThreadPoolExecutor(32) as pool:
        candidates = [h for h in pool.map(open_port, hosts) if h]
    for h in candidates:
        if answers(h, port, 5):
            return h
    return None


def find(host, port, net):
    """the laptop's address: host when a SurfOS prompt answers there, else the first one found on net"""
    if answers(host, port):
        return host
    if not net:
        raise DevloopError(f'no SurfOS telnet prompt at {host} port {port}')
    say(f'nothing answers at {host} port {port}: searching {net} ...')
    found = scan(net, port, exclude={host})
    if not found:
        raise DevloopError(f'no SurfOS telnet prompt at {host} or on {net} port {port}: is the laptop on, with '
                           f'telnetd on (or booted from the development entry)?')
    say(f'found SurfOS at {found}')
    return found


def connect(host, port, net):
    return Session(find(host, port, net), port)


def wait_for_laptop(host, port, net, deadline, settle=SETTLE):
    """a session with the laptop once it is back, from host or (after a while) anywhere on net"""
    time.sleep(settle)
    next_scan = time.time() + 45
    while time.time() < deadline:
        try:
            return Session(host, port, timeout=5)
        except DevloopError:
            pass
        if net and time.time() >= next_scan:
            found = scan(net, port, exclude={host})
            if found:
                say(f'the laptop came back at {found}')
                return Session(found, port)
            next_scan = time.time() + 45
        time.sleep(2)
    return None


def reboot(session, port, net, wait=REBOOT_WAIT, settle=SETTLE):
    """reboot the laptop through session (which ends), wait for its telnet and make sure it restarted: a new
    session"""
    up = session.uptime()
    host = session.host
    say(f'rebooting {host} (up {up:.0f} s)')
    session.t.send('reboot\n')
    session._pump_until(lambda: b'rebooting' in session.t.data[session.cursor:], 5)
    session.t.close()
    sent = time.time()
    s = wait_for_laptop(host, port, net, sent + wait, settle)
    if not s:
        raise DevloopError(f'the laptop did not answer telnet within {wait} s after the reboot: look at its screen '
                           f'(the development entry, then +AUTONET: lines; the normal entry needs dhcp and telnetd on)')
    up = s.uptime()
    if up > time.time() - sent + 5:
        s.close()
        raise DevloopError(f'{s.host} did not restart: it has been up {up:.0f} s')
    say(f'{s.host} is back after {time.time() - sent:.0f} s (up {up:.0f} s)')
    return s


# ---- the data stick and the push ---------------------------------------------------------------------------

def parse_lsblk(text):
    """lsblk's table as dicts: name, part (a partition), size, bus, type (the TYPE column: "disk mbr", "part mbr
    FAT32 LBA", ...), label (a disk's model, a GPT partition's name), status"""
    lines = [l for l in text.splitlines() if l.strip()]
    head = next((i for i, l in enumerate(lines) if l.startswith('NAME ')), None)
    if head is None:
        raise DevloopError('lsblk printed no table')
    h = lines[head]
    type_at, label_at, status_at = h.find('TYPE'), h.find('MODEL/LABEL'), h.find('STATUS')
    if type_at < 0 or label_at < type_at:
        raise DevloopError('lsblk printed a table devloop does not know')
    rows = []
    for l in lines[head + 1:]:
        f = l.split()
        if len(f) < 5 or f[2] not in ('ram', 'ata', 'usb', '-'):
            continue
        rows.append({'name': f[0], 'part': l.startswith(' '), 'size': f[1], 'bus': f[2],
                     'type': l[type_at:label_at].strip(),
                     'label': l[label_at:status_at if status_at > 0 else None].strip(),
                     'status': l[status_at:].strip() if status_at > 0 and len(l) > status_at else ''})
    return rows


# a partition GRUB and SurfOS read as FAT: the MBR types of FAT12, 16 and 32, an EFI system partition, GPT's basic data
FAT_PART = re.compile(r'^part (?:mbr|gpt) (?:FAT|EFI system|basic data)')


def data_stick(rows, name=None):
    """(the disk, the device to mount), as lsblk's rows, of the data stick: the USB disk that is not the boot drive
    (lsblk says "protected") and whose first partition is FAT, the partition GRUB's development entry looks at; or,
    when no such disk is there, the one USB disk with no partition table at all (mounting it says whether it is
    FAT). Never chosen by its name or size: usb0 and usb1 change places from one boot to the next."""
    disks = [r for r in rows if not r['part'] and r['bus'] == 'usb' and 'protected' not in r['status']
             and 'read-only' not in r['status']]
    if name:
        disks = [r for r in disks if r['name'] == name]
        if not disks:
            raise DevloopError(f'{name} is not a USB disk SurfOS may write (lsblk)')
    if not disks:
        raise DevloopError('no data stick: lsblk shows no USB disk besides the boot drive (plug a FAT32 USB stick '
                           'into the laptop)')

    def first(d):
        parts = sorted((r for r in rows if r['part'] and re.fullmatch(re.escape(d['name']) + r'p\d+', r['name'])),
                       key=lambda r: int(r['name'][len(d['name']) + 1:]))
        return parts[0] if parts else None
    fat = [(d, first(d)) for d in disks if first(d) and FAT_PART.match(first(d)['type'])]
    if len(fat) > 1:
        raise DevloopError('more than one USB disk with a FAT partition besides the boot drive (%s): say which is '
                           'the data stick with --stick' % ', '.join(d['name'] for d, _ in fat))
    if fat:
        return fat[0]
    bare = [d for d in disks if first(d) is None]
    if len(bare) == 1 and len(disks) == 1:
        return bare[0], bare[0]
    seen = ', '.join(f'{d["name"]}: ' + (f'its first partition is {first(d)["type"][5:]}' if first(d) else
                                          'no partition table') for d in disks)
    raise DevloopError(f'no data stick: no USB disk besides the boot drive has a FAT partition first ({seen}). '
                       + MAKE_FAT)


def mount_stick(s, dev):
    """the stick's FAT file system, mounted read-write: where"""
    out = s.check('mount', ok=lambda st, o: True)
    m = re.search(r'^\s*%s on (\S+)\s+type (\S+)(.*)$' % re.escape(dev['name']), out, re.M)
    if m:
        if '(ro)' in m.group(3):
            raise DevloopError(f'{dev["name"]} is mounted read-only on {m.group(1)}: umount it, then deploy again')
        if not m.group(2).startswith('fat'):
            raise DevloopError(f'{dev["name"]} on {m.group(1)} is {m.group(2)}, not FAT')
        say(f'{dev["name"]} is mounted on {m.group(1)} already')
        return m.group(1)
    status, out = s.run(f'mount {dev["name"]} {STICK_MOUNT}')
    if 'no file system found' in out:
        raise DevloopError(NO_FAT.format(dev=dev['name']))
    line = re.search(r'%s on %s: (\S+?),.*' % (re.escape(dev['name']), re.escape(STICK_MOUNT)), out)
    if status != 0 or not line:
        raise DevloopError(f'mount {dev["name"]} {STICK_MOUNT}: {out.strip() or "status %s" % status}')
    if 'read-write' not in line.group(0):
        s.run(f'umount {STICK_MOUNT}')
        raise DevloopError(f'{dev["name"]} mounts only read-only ({line.group(0).strip()})')
    if not line.group(1).startswith('fat'):
        s.run(f'umount {STICK_MOUNT}')
        raise DevloopError(NO_FAT.format(dev=dev['name']))
    say(line.group(0).strip())
    return STICK_MOUNT


def push(s, data, path, rx_port, connect_host=None, timeout=None):
    """data into path on the laptop with rx; DevloopError unless rx said its length and CRC-32"""
    host = connect_host or s.host
    timeout = timeout or 60 + len(data) / 50000
    s.start(f'rx {rx_port} {path}')
    if not s.wait(rb'rx: listening on port \d+', 30):
        status, out = s.finish(15)
        raise DevloopError(f'rx did not start: {out.strip() or "status %s" % status}')
    t0, problem = time.time(), None
    try:
        c = socket.create_connection((host, rx_port), timeout=15)
    except OSError as e:
        s.t.send(b'\x03')                      # rx would wait 2 minutes for a connection that will not come
        try:
            s.finish(15)
        except DevloopError:
            pass
        raise DevloopError(f'cannot connect to rx at {host} port {rx_port} ({e.strerror or e})')
    try:
        with c:
            c.settimeout(timeout)
            c.sendall(data)
            c.shutdown(socket.SHUT_WR)
            while c.recv(4096):                 # rx closes once the file has its name
                pass
    except OSError as e:
        problem = e
    status, out = s.finish(timeout)
    m = re.search(r'rx: (\d+) bytes, crc32 ([0-9a-f]{8}) -> (.+)', out)
    want = '%08x' % (zlib.crc32(data) & 0xFFFFFFFF)
    if status != 0 or not m:
        raise DevloopError(f'rx {path}: {out.strip()}' + (f' (sending: {problem})' if problem else ''))
    if int(m.group(1)) != len(data) or m.group(2) != want:
        raise DevloopError(f'{path}: rx received {m.group(1)} bytes with crc32 {m.group(2)}, '
                           f'sent {len(data)} bytes with crc32 {want}')
    secs = max(time.time() - t0, 0.001)
    say(f'{path}: {len(data)} bytes, crc32 {want} ({len(data) / secs / 1024:.0f} KB/s)')


# ---- the build -------------------------------------------------------------------------------------------------

def default_build(opt=None):
    """(kernel, initrd) of the newest build: build/docker/build/O2 or O0, or build/O2 or O0"""
    found = []
    for root in (os.path.join(REPO, 'build', 'docker', 'build'), os.path.join(REPO, 'build')):
        for level in ([str(opt)] if opt is not None else ['2', '0']):
            k, i = (os.path.join(root, 'O' + level, n) for n in ('surfos.bin', 'initrd.tar'))
            if os.path.isfile(k) and os.path.isfile(i):
                found.append((max(os.path.getmtime(k), os.path.getmtime(i)), k, i))
    if not found:
        raise DevloopError('no build with surfos.bin and initrd.tar: devenv/build-usb.sh makes one (O=0 for the '
                           'debug build), or name the files with --kernel and --initrd')
    _, k, i = max(found)
    if os.path.getmtime(k) > os.path.getmtime(i) + 1:
        raise DevloopError(f'{i} is older than {k}: build both with devenv/build-usb.sh (or devenv/build.sh '
                           f'O=<level> usb-image)')
    return k, i


def build_id_of(initrd):
    """etc/build-id of an initrd (its bytes)"""
    import io
    try:
        with tarfile.open(fileobj=io.BytesIO(initrd)) as t:
            for name in ('etc/build-id', './etc/build-id'):
                try:
                    return t.extractfile(name).read().decode().strip()
                except KeyError:
                    continue
    except tarfile.TarError as e:
        raise DevloopError(f'the initrd is no tar archive ({e})')
    raise DevloopError('the initrd has no etc/build-id: build it again with this tree')


def load_build(kernel_path, initrd_path):
    mk = mkusb()
    try:
        mk.check_input(kernel_path, 'kernel')
        mk.check_kernel(kernel_path)
        mk.check_input(initrd_path, 'initrd')
    except mk.BuildError as e:
        raise DevloopError(str(e))
    with open(kernel_path, 'rb') as f:
        kernel = f.read()
    with open(initrd_path, 'rb') as f:
        initrd = f.read()
    return kernel, initrd, build_id_of(initrd)


def deploy(s, kernel, initrd, build_id, port=TELNET_PORT, rx_port=RX_PORT, net=None, stick=None, connect_host=None,
           wait=REBOOT_WAIT, settle=SETTLE):
    """the build onto the data stick through session s, then the reboot into it: the new session"""
    row, dev = data_stick(parse_lsblk(s.check('lsblk')), stick)
    disk = row['name']
    say(f'data stick: {disk} ({row["label"] or "no model"}, {row["size"]}), ' +
        (f'its partition {dev["name"]} ({dev["size"]})' if dev is not row else 'no partition table'))
    root = mount_stick(s, dev)
    folder = f'{root}/{DEV_DIR}'
    try:
        s.check(f'mkdir {folder}', ok=lambda st, o: st == 0 or 'File exists' in o)
        for name, data in zip(FILES, (initrd, kernel)):
            push(s, data, f'{folder}/{name}.new', rx_port, connect_host)
        for name in FILES:
            s.check(f'mv {folder}/{name}.new {folder}/{name}')
    except DevloopError:
        try:                                    # the stick left as SurfOS can find it again: unmounted, written out
            s.run(f'eject {disk}', 30)
        except DevloopError:
            pass
        raise
    out = s.check(f'eject {disk}')
    if f'{disk}: safe to remove' not in out:
        raise DevloopError(f'eject {disk}: {out.strip()}')
    say(f'{folder}: {", ".join(FILES)} in place; {disk} ejected')
    s2 = reboot(s, port, net, wait, settle)
    got = s2.build_id()
    if got != build_id:
        s2.close()
        raise DevloopError(f'the laptop came back with build {got!r}, not the pushed {build_id!r} (was the '
                           f'development entry chosen? bootinfo says the command line)')
    return s2


# ---- the commands ----------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default=DEFAULT_HOST, help=f'the laptop (default {DEFAULT_HOST})')
    ap.add_argument('--port', type=int, default=TELNET_PORT, help='its telnet port (23)')
    ap.add_argument('--net', default=DEFAULT_NET, help=f'searched when --host does not answer ({DEFAULT_NET}); '
                    '"none": never')
    ap.add_argument('--rx-port', type=int, default=RX_PORT, help=f'the port rx listens on for a push ({RX_PORT})')
    ap.add_argument('--timeout', type=float, default=120, help='run: seconds each command may take (120)')
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('find', help='where the laptop answers')
    p = sub.add_parser('run', help='run commands, print their output')
    p.add_argument('commands', nargs='+', metavar='cmd')
    sub.add_parser('log', help='the kernel log (dmesg)')
    p = sub.add_parser('deploy', help='push a build to the data stick and reboot into it')
    p.add_argument('--kernel', help='surfos.bin to push (default: the newest build)')
    p.add_argument('--initrd', help='initrd.tar to push (default: the newest build\'s)')
    p.add_argument('--opt', choices=['0', '2'], help='the build level to take (default: the newer)')
    p.add_argument('--stick', help='the data stick\'s disk (usb1), when lsblk shows more than one')
    sub.add_parser('reboot', help='reboot, wait for telnet, say the build id')
    a = ap.parse_args(argv)
    net = None if a.net == 'none' else a.net

    try:
        if a.command == 'find':
            print(find(a.host, a.port, net))
            return 0
        if a.command == 'deploy':
            if bool(a.kernel) != bool(a.initrd):
                raise DevloopError('--kernel and --initrd go together (a kernel and its own initrd)')
            kp, ip = (a.kernel, a.initrd) if a.kernel else default_build(a.opt)
            kernel, initrd, bid = load_build(kp, ip)
            say(f'build {bid}: {kp} ({len(kernel)} bytes), {ip} ({len(initrd)} bytes)')
            s = connect(a.host, a.port, net)
            s2 = deploy(s, kernel, initrd, bid, a.port, a.rx_port, net, a.stick)
            say(f'{s2.host} runs build {bid}')
            s2.close()
            return 0
        for cmd in getattr(a, 'commands', []):
            why = refused(cmd)
            if why:
                raise DevloopError(f'not run: {why}')
        s = connect(a.host, a.port, net)
        try:
            if a.command == 'reboot':
                before = s.build_id()
                s2 = reboot(s, a.port, net)
                say(f'{s2.host}: build {s2.build_id()} (before: {before})')
                s2.close()
                return 0
            commands = ['dmesg'] if a.command == 'log' else a.commands
            failed = 0
            for cmd in commands:
                status, out = s.run(cmd, a.timeout)
                if len(commands) > 1:
                    print(f'== {cmd}')
                if out:
                    print(out)
                if status != 0:
                    failed += 1
                    sys.stdout.flush()
                    print(f'({cmd}: ' + (f'status {status})' if status is not None else 'not run)'), file=sys.stderr)
            return 1 if failed else 0
        finally:
            s.close()
    except DevloopError as e:
        print(f'devloop: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
