#!/usr/bin/env python3
"""
SurfCPU serial tool: the Arty's UART from the Mac, with Python's standard library only
File: uart.py Date: 10/16/26 (SurfCPU, the os track)

Nothing to install: termios, tty, select and os are in every Python, the Mac's own
/usr/bin/python3 (3.9) included. It runs on Linux too (the tests, and the RTL system's
pseudo-terminal: surfsim --uart-pty).

  uart.py list                                     the candidate ports, and which one is the UART
                                                   (PORT below may be auto: the only Arty UART there)
  uart.py console PORT [--log FILE]                a terminal; Ctrl-] quits
  uart.py capture PORT --until REGEX [--timeout S] --out FILE
                                                   record what arrives, line by line with the time
  uart.py script PORT FILE [--timeout S] [--log FILE]
                                                   the expect/send steps of FILE (see below)

The port is set to 115200 8N1, raw: no echo, no line editing, no CR or LF translation, no
flow control (the Arty's UART has no RTS/CTS wired), CLOCAL (the modem lines are ignored)
and not HUPCL (closing the port does not drop DTR). Bytes that arrived before the port was
opened are thrown away (--no-flush keeps them).

DTR and RTS. The system raises both when a program opens the port, and uart.py leaves them
alone unless asked (--dtr on|off, --rts on|off). On the Arty they reach nothing: the FT2232's
DTR drives the board's RESET net only through jumper JP2, which stays open (WIRING.md U7).
With JP2 shorted, opening the port could reset the SoC.

Typing. What uart.py types goes out one byte at a time with a pause between bytes (--pace,
2 ms: about 500 characters a second, a fifth of the line's 11,520), so a ROM or kernel that
polls its 64-byte receive FIFO between other work does not lose characters. A line is ended
with CR, as a terminal sends Enter.

Which port: the Arty's FT2232 shows two ports, /dev/cu.usbserial-<serial>0 (channel A:
JTAG, for openFPGALoader) and /dev/cu.usbserial-<serial>1 (channel B: the UART). On Linux,
/dev/serial/by-id/...-if00-port0 is channel A and ...-if01-port0 channel B. `list` says
which is which; a port that shows garbled bytes or nothing at all is usually channel A, or
the wrong baud rate.

A script file has one step per line, written as surfsim's options are (sim/soc/surfsim):

  --expect REGEX         wait for REGEX; every --expect must match, in order, each searched
                         after the previous one's match: the script passes when the last one did
  --send REGEX=TEXT      once REGEX matched (the sends in their own order), type TEXT; TEXT
                         takes \\r \\n \\t \\e \\b \\\\ and \\xHH
  --fail-on REGEX        fail at once when REGEX appears
  --timeout S            the whole script's time limit (default 60 s)

Blank lines and lines starting with # are skipped. REGEX is Python's, matched against the
text as received with escape sequences and CR removed. Exit status: 0 when every --expect
matched, 1 on a --fail-on match or a port that went away, 2 when the time ran out first.
"""

import argparse
import codecs
import errno
import glob
import os
import re
import select
import sys
import termios
import threading
import time
import tty

BAUDS = {9600: termios.B9600, 19200: termios.B19200, 38400: termios.B38400, 57600: termios.B57600,
         115200: termios.B115200}
PACE = 0.002                # seconds between typed bytes
WINDOW = 65536              # patterns are searched over at most this much of the newest text


class PortGone(Exception):
    """the port went away: the USB cable unplugged, the board unpowered, surfsim ended"""


class Port:
    """a serial port (or a pseudo-terminal) in raw 8N1"""

    def __init__(self, path, baud=115200, flush=True, dtr=None, rts=None):
        self.path = path
        try:
            self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            raise PortGone('%s: %s' % (path, e.strerror)) from None
        try:
            a = termios.tcgetattr(self.fd)
            a[0] = 0                                        # iflag: no CR/LF mapping, no XON/XOFF, no parity marks
            a[1] = 0                                        # oflag: no output processing
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL   # 8N1, receiver on, modem lines ignored, no HUPCL
            a[3] = 0                                        # lflag: no echo, no line editing, no signals
            a[4] = a[5] = BAUDS[baud]
            a[6][termios.VMIN] = 0
            a[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, a)
            if flush:
                termios.tcflush(self.fd, termios.TCIFLUSH)  # what arrived before we looked
            self.modem(dtr, rts)
        except (termios.error, OSError) as e:
            os.close(self.fd)
            raise PortGone('%s: %s' % (path, e)) from None

    def modem(self, dtr=None, rts=None):
        """set DTR and RTS (None: leave as they are)"""
        import fcntl
        import struct
        for on, bit in ((dtr, getattr(termios, 'TIOCM_DTR', 0x002)), (rts, getattr(termios, 'TIOCM_RTS', 0x004))):
            if on is None:
                continue
            req = getattr(termios, 'TIOCMBIS' if on else 'TIOCMBIC', None)
            if req is None:
                raise PortGone('this system cannot set the modem lines')
            fcntl.ioctl(self.fd, req, struct.pack('I', bit))

    def read(self, timeout):
        """what arrived within timeout seconds (b'' if nothing); PortGone if the port went away"""
        try:
            r, _, x = select.select([self.fd], [], [self.fd], timeout)
        except (OSError, ValueError) as e:
            raise PortGone('%s: %s' % (self.path, e)) from None
        if not r and not x:
            if not os.path.exists(self.path):
                raise PortGone('%s no longer exists' % self.path)
            return b''
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            return b''
        except OSError as e:
            raise PortGone('%s: %s' % (self.path, e.strerror or e)) from None   # EIO, ENXIO: unplugged
        if not data:
            raise PortGone('%s hung up' % self.path)
        return data

    def write(self, data, pace=0.0):
        """data, one byte at a time with pace seconds between bytes (0: all at once)"""
        chunks = [data[i:i + 1] for i in range(len(data))] if pace else [data]
        for c in chunks:
            while c:
                try:
                    n = os.write(self.fd, c)
                    c = c[n:]
                except BlockingIOError:
                    select.select([], [self.fd], [], 1.0)
                except OSError as e:
                    raise PortGone('%s: %s' % (self.path, e.strerror or e)) from None
            if pace:
                time.sleep(pace)

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None


class Text:
    """what arrived, as text: UTF-8, with bytes that are not and control characters a console
    does not send shown as U+FFFD; CSI and two-byte escape sequences, CR, bell and backspace
    removed; kept incrementally (a sequence or a character may be split between reads); each
    line with the time its first character arrived"""

    def __init__(self):
        self.dec = codecs.getincrementaldecoder('utf-8')(errors='replace')
        self.esc = ''                   # an escape sequence not finished yet
        self.text = ''                  # everything, cleaned
        self.raw = 0                    # bytes received
        self.bad = 0                    # bytes that were not printable text (garbage on the line?)
        self.line_times = []            # (offset in text, time) of each line's start
        self.at_line_start = True

    def add(self, data, when):
        self.raw += len(data)
        out = []
        for ch in self.dec.decode(data):
            if ch == '�' or (ch < ' ' and ch not in '\t\n\r\x1b\x08\x07') or ch == '\x7f':
                self.bad += 1               # not UTF-8, or a control character no console sends
                ch = '�'
            elif ch in '\x07\x08':
                continue                    # the bell; a backspace (the line was edited)
            if self.esc:
                self.esc += ch
                if len(self.esc) == 2 and ch != '[':
                    self.esc = ''               # ESC x: two characters
                elif len(self.esc) > 2 and '\x40' <= ch <= '\x7e':
                    self.esc = ''               # CSI ... final byte
                elif len(self.esc) > 32:
                    self.esc = ''               # not a sequence after all
                continue
            if ch == '\x1b':
                self.esc = ch
                continue
            if ch == '\r':
                continue
            if self.at_line_start:
                self.line_times.append((len(self.text) + len(out), when))
                self.at_line_start = False
            out.append(ch)
            if ch == '\n':
                self.at_line_start = True
        self.text += ''.join(out)

    def lines(self, start=0):
        """[(time, line)] of the text from offset start on"""
        res = []
        for i, (off, t) in enumerate(self.line_times):
            end = self.line_times[i + 1][0] if i + 1 < len(self.line_times) else len(self.text)
            if end <= start:
                continue
            res.append((t, self.text[max(off, start):end].rstrip('\n')))
        return res

    def garbled(self):
        """mostly unprintable bytes: the wrong port or the wrong baud rate"""
        return self.raw >= 32 and self.bad * 4 > self.raw


class Link:
    """a port read by a thread into a Text, so nothing is lost while the caller waits for a
    person or types; it can open the port again when it comes back (a power cycle)"""

    def __init__(self, path, baud=115200, flush=True, pace=PACE, log=None):
        self.path, self.baud, self.pace, self.log = path, baud, pace, log
        self.t0 = time.time()
        self.text = Text()
        self.cond = threading.Condition()
        self.gone = None                # why the port went away (None: it is there)
        self.gone_at = None
        self.reopen_until = 0.0         # while time() < this, a port that went away is opened again
        self.reopened = 0
        self.events = []                # (time, what): the port going away and coming back
        self.last_fail = None           # the match of the fail_on pattern that ended the last wait
        self.stop = False
        self.port = Port(path, baud, flush=flush)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def now(self):
        return time.time() - self.t0

    def _run(self):
        while not self.stop:
            port = self.port
            if port is None:
                if time.time() < self.reopen_until:
                    try:
                        p = Port(self.path, self.baud, flush=False)
                    except PortGone:
                        time.sleep(0.1)
                        continue
                    with self.cond:
                        self.port, self.gone, self.gone_at = p, None, None
                        self.reopened += 1
                        self.events.append((self.now(), 'the port is back: opened again'))
                        self.cond.notify_all()
                    continue
                time.sleep(0.05)
                continue
            try:
                data = port.read(0.1)
            except PortGone as e:
                port.close()
                with self.cond:
                    self.port = None
                    self.gone, self.gone_at = str(e), self.now()
                    self.events.append((self.gone_at, 'the port went away (%s)' % e))
                    self.cond.notify_all()
                continue
            if data:
                with self.cond:
                    self.text.add(data, self.now())
                    if self.log:
                        self.log.write(data)
                        self.log.flush()
                    self.cond.notify_all()

    def allow_reopen(self, seconds):
        """a port that goes away within the next seconds is opened again when it is back"""
        with self.cond:
            self.reopen_until = time.time() + seconds
            self.cond.notify_all()

    def wait(self, pattern, start, timeout, fail_on=()):
        """the first match of pattern in the text after offset start, within timeout seconds:
        (match, None), or (None, why): 'timeout', 'gone: ...' or 'fail-on: PATTERN'"""
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        fails = [re.compile(f) if isinstance(f, str) else f for f in fail_on]
        deadline = time.time() + timeout
        with self.cond:
            while True:
                text = self.text.text
                lo = max(start, len(text) - WINDOW)
                m = rx.search(text, lo)
                for f in fails:
                    fm = f.search(text, max(start, len(text) - WINDOW))
                    if fm:
                        self.last_fail = fm
                        return None, 'fail-on: %s' % fm.group(0)
                if m:
                    return m, None
                if self.port is None and time.time() >= self.reopen_until:
                    return None, 'gone: %s' % self.gone
                left = deadline - time.time()
                if left <= 0:
                    return None, 'timeout'
                self.cond.wait(min(left, 0.2))

    def type(self, text):
        """text typed on the port, paced"""
        port = self.port
        if port is None:
            raise PortGone(self.gone or '%s is not open' % self.path)
        port.write(text.encode('utf-8'), self.pace)

    def close(self):
        self.stop = True
        self.thread.join(2)
        if self.port:
            self.port.close()


def unescape(s):
    """surfsim's TEXT escapes: \\r \\n \\t \\e \\b \\\\ \\xHH"""
    out, i = [], 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n in 'rnteb\\':
                out.append({'r': '\r', 'n': '\n', 't': '\t', 'e': '\x1b', 'b': '\b', '\\': '\\'}[n])
                i += 2
                continue
            if n == 'x' and re.match(r'[0-9a-fA-F]{2}', s[i + 2:i + 4]):
                out.append(chr(int(s[i + 2:i + 4], 16)))
                i += 4
                continue
        out.append(c)
        i += 1
    return ''.join(out)


# ------------------------------------------------------------------------------------ list

def candidates():
    """[(path, what)] for the serial ports that could be the Arty's"""
    res = []
    mac = sorted(glob.glob('/dev/cu.usbserial-*'))
    for p in mac:
        what = 'FT2232 channel B: the UART' if p.endswith('1') else \
            'FT2232 channel A: JTAG (openFPGALoader), not the UART' if p.endswith('0') else 'a USB serial port'
        res.append((p, what))
    for p in sorted(glob.glob('/dev/serial/by-id/*')):
        what = 'channel B: the UART' if '-if01' in p else 'channel A: JTAG, not the UART' if '-if00' in p else ''
        res.append(('%s -> %s' % (p, os.path.realpath(p)), what))
    if not res:
        for p in sorted(glob.glob('/dev/ttyUSB*')):
            res.append((p, 'a USB serial port (ttyUSB1 is usually the FT2232\'s UART)'))
    return res


def auto_port():
    """the Arty's UART when exactly one is plugged in: (path, None), or (None, why)"""
    uarts = sorted(glob.glob('/dev/cu.usbserial-*1')) or sorted(glob.glob('/dev/serial/by-id/*-if01*'))
    if len(uarts) == 1:
        return uarts[0], None
    if not uarts:
        return None, 'no Arty UART (/dev/cu.usbserial-<serial>1): is the board plugged in? uart.py list shows the ports'
    return None, 'more than one candidate (%s): name one with --port' % ', '.join(uarts)


def cmd_list(a):
    found = candidates()
    if not found:
        print('uart.py: no /dev/cu.usbserial-* port: is the Arty plugged in (its USB cable powers it)?')
        return 1
    for p, what in found:
        print('%-48s %s' % (p, what))
    if any(p.endswith('1') for p, _ in found):
        print('use the port ending in 1; `make -C surfcpu doctor` also lists them')
    return 0


# ------------------------------------------------------------------------------------ console

def cmd_console(a):
    log = open(a.log, 'ab') if a.log else None
    try:
        port = Port(a.port, a.baud, flush=not a.no_flush, dtr=a.dtr, rts=a.rts)
    except PortGone as e:
        print('uart.py: %s' % e, file=sys.stderr)
        return 1
    print('uart.py: %s at %d 8N1; Ctrl-] quits' % (a.port, a.baud), file=sys.stderr)
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    status = 0
    try:
        if saved:
            tty.setraw(stdin)
        while True:
            r, _, _ = select.select([port.fd, stdin], [], [], 0.5)
            if port.fd in r or not r:
                try:
                    data = port.read(0)
                except PortGone as e:
                    sys.stderr.write('\r\nuart.py: the port went away: %s\r\n' % e)
                    status = 1
                    break
                if data:
                    os.write(sys.stdout.fileno(), data)
                    if log:
                        log.write(data)
                        log.flush()
            if stdin in r:
                keys = os.read(stdin, 1024)
                if not keys or b'\x1d' in keys:
                    keys = keys.split(b'\x1d')[0]
                    if keys:
                        port.write(keys)
                    break
                port.write(keys)
    finally:
        if saved:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        port.close()
        if log:
            log.close()
    if status == 0:
        print('\nuart.py: closed', file=sys.stderr)
    return status


# ------------------------------------------------------------------------------------ capture

def write_lines(f, link, start=0, prefix=''):
    for t, line in link.text.lines(start):
        f.write('%s+%9.3f | %s\n' % (prefix, t, line))


def cmd_capture(a):
    try:
        link = Link(a.port, a.baud, flush=not a.no_flush)
    except PortGone as e:
        print('uart.py: %s' % e, file=sys.stderr)
        return 1
    m, why = link.wait(a.until, 0, a.timeout)
    link.close()
    with open(a.out, 'w') as f:
        f.write('# uart.py capture %s at %d 8N1, %s UTC; until %r: %s\n' % (
            a.port, a.baud, time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(link.t0)), a.until,
            'matched at +%.3f s' % link.now() if m else why))
        write_lines(f, link)
    if m:
        print('uart.py: %s: matched %r; %d lines in %s' % (a.port, m.group(0), len(link.text.lines()), a.out))
        return 0
    print('uart.py: %s: %s waiting for %r%s; what arrived is in %s' % (
        a.port, why, a.until, ' (the bytes look garbled: the wrong port or baud rate?)' if link.text.garbled() else '',
        a.out), file=sys.stderr)
    return 2 if why == 'timeout' else 1


# ------------------------------------------------------------------------------------ script

def parse_script(path):
    """(expects, sends, fail_ons, timeout) from a script file"""
    expects, sends, fails, timeout = [], [], [], 60.0
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\r\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        opt, _, arg = line.strip().partition(' ')
        try:
            if opt == '--expect':
                expects.append(re.compile(arg))
            elif opt == '--send':
                pat, eq, text = arg.partition('=')
                if not eq:
                    raise ValueError('--send needs REGEX=TEXT')
                sends.append((re.compile(pat), unescape(text)))
            elif opt == '--fail-on':
                fails.append(re.compile(arg))
            elif opt == '--timeout':
                timeout = float(arg)
            else:
                raise ValueError('unknown step %s' % opt)
        except (re.error, ValueError) as e:
            raise SystemExit('uart.py: %s line %d: %s' % (path, n, e))
    return expects, sends, fails, timeout


def run_script(link, expects, sends, fails, timeout, report=print):
    """surfsim's rules: expects in order, sends in their own order, a fail-on ends it. (status, why)"""
    deadline = time.time() + timeout
    ei = si = 0
    efrom = sfrom = 0
    while True:
        with link.cond:
            text = link.text.text
            while si < len(sends):
                m = sends[si][0].search(text, max(sfrom, len(text) - WINDOW))
                if not m:
                    break
                sfrom = m.end()
                link.cond.release()
                try:
                    report('send %d of %d: %r' % (si + 1, len(sends), sends[si][1]))
                    link.type(sends[si][1])
                finally:
                    link.cond.acquire()
                si += 1
                text = link.text.text
            while ei < len(expects):
                m = expects[ei].search(text, max(efrom, len(text) - WINDOW))
                if not m:
                    break
                efrom = m.end()
                ei += 1
                report('expect %d of %d matched at +%.3f s' % (ei, len(expects), link.now()))
            for f in fails:
                fm = f.search(text, max(0, len(text) - 4096))
                if fm:
                    return 1, 'fail-on %r matched' % fm.group(0)
            if expects and ei == len(expects):
                return 0, 'every expect matched'
            if link.port is None and time.time() >= link.reopen_until:
                return 1, 'the port went away: %s' % link.gone
            left = deadline - time.time()
            if left <= 0:
                return (2, 'the time ran out at expect %d of %d' % (ei + 1, len(expects))) if expects \
                    else (0, 'the time ran out (no expects)')
            link.cond.wait(min(left, 0.2))


def cmd_script(a):
    expects, sends, fails, timeout = parse_script(a.file)
    if a.timeout:
        timeout = a.timeout
    try:
        link = Link(a.port, a.baud, flush=not a.no_flush, pace=a.pace)
    except PortGone as e:
        print('uart.py: %s' % e, file=sys.stderr)
        return 1
    status, why = run_script(link, expects, sends, fails, timeout,
                             report=lambda s: print('uart.py: %s' % s, file=sys.stderr))
    link.close()
    if a.log:
        with open(a.log, 'w') as f:
            f.write('# uart.py script %s on %s: %s (exit %d)\n' % (a.file, a.port, why, status))
            write_lines(f, link)
    print('uart.py: %s: %s' % (a.file, why), file=sys.stderr if status else sys.stdout)
    return status


def main(argv=None):
    ap = argparse.ArgumentParser(description='the Arty\'s UART from the Mac (standard library only)')
    sub = ap.add_subparsers(dest='cmd')
    sub.add_parser('list', help='the candidate ports')

    def common(p):
        p.add_argument('port', help='the serial port, or auto: the only Arty UART plugged in')
        p.add_argument('--baud', type=int, default=115200, choices=sorted(BAUDS))
        p.add_argument('--no-flush', action='store_true', help='keep what arrived before the port was opened')
        p.add_argument('--pace', type=float, default=PACE, help='seconds between typed bytes (default 0.002)')
    p = sub.add_parser('console', help='an interactive terminal; Ctrl-] quits')
    common(p)
    p.add_argument('--log', help='append what arrives to this file')
    for name in ('--dtr', '--rts'):
        p.add_argument(name, choices=('on', 'off'), help='set the line (default: as the system leaves it)')
    p = sub.add_parser('capture', help='record what arrives until REGEX')
    common(p)
    p.add_argument('--until', required=True)
    p.add_argument('--timeout', type=float, default=60.0)
    p.add_argument('--out', required=True)
    p = sub.add_parser('script', help='run expect/send steps')
    common(p)
    p.add_argument('file')
    p.add_argument('--timeout', type=float)
    p.add_argument('--log')
    a = ap.parse_args(argv)
    if getattr(a, 'port', None) == 'auto':
        a.port, why = auto_port()
        if not a.port:
            print('uart.py: auto: %s' % why, file=sys.stderr)
            return 1
    if a.cmd == 'console':
        a.dtr = None if a.dtr is None else a.dtr == 'on'
        a.rts = None if a.rts is None else a.rts == 'on'
    return {'list': cmd_list, 'console': cmd_console, 'capture': cmd_capture, 'script': cmd_script}.get(
        a.cmd, lambda _: ap.print_help() or 2)(a)


if __name__ == '__main__':
    sys.exit(main())
