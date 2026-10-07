#!/usr/bin/env python3
"""
SurfOS TCP tests with hand-made segments
--------------------
File: qemu-tcp.py   Date: 10/6/26
--------------------
TCP as a program sees it (surfos/sock.h, net/tcp.c), against an other end that does
what a test needs, right or wrong: the guest's NIC is joined to tools/rawnet.py by a
QEMU "dgram" netdev instead of QEMU's user network, and the test plays 10.0.2.2 one
segment at a time. The guest side is user/bin/nettest. One boot runs every scenario,
with keepalive made quick (tcpka=3,1: three seconds silent, then a probe a second).

  scan        five SYN, SYN-ACK, RST (a SYN scan) while the server does not accept yet:
              a real client still gets its SYN-ACK and is accepted
  flood       five SYNs never acknowledged: a real client takes a half-open one's place
  reset       a connection reset before the server accepts it: accept() hands it out,
              read() says ECONNRESET
  greet       a server that greets and closes (or resets) before the guest acknowledged
              its SYN-ACK: connect() succeeds and read() gets the greeting, then the end
  window      a reader that shuts its window and opens it again without saying so: the
              guest probes the shut window, and every byte arrives
  fin         a FIN behind more data than fits: the FIN waits for the rest
  keepalive   an other end that vanishes: ETIMEDOUT after 3 + 5 probes; one that answers
              the probes keeps its connection
  churn       40 connections made and closed in a row: none fails for want of a socket
  reserve     programs that hold every socket they can: the kernel's own connection
              (httpget) still gets one
  sws         100-byte writes to a client that keeps its window small: a reader that opens
              100 bytes at a time gets one segment a second (no silly window syndrome)
  linger      a closed connection whose other end never sends its FIN, then TIME_WAIT
              with the link down: neither keeps its buffers, TIME_WAIT ends on time
  lost        a 1-byte segment lost while bytes behind it wait for the window: it is sent
              again alone, and the bytes and the FIN follow its acknowledgement
  latency     bursts of small writes (1, 20, 30 bytes) to a client that delays its ACKs
              200 ms: each burst arrives whole at once, not held for the ACK (no Nagle)
  zwprobe     a full receive buffer probed with a byte at a time (BSD, macOS and Windows
              probe so): each probe gets an ACK saying the window is shut
  evict       a client whose half-open connection a later SYN took the place of: its ACK
              and data get a reset at once, and the server serves the next client
  ackahead    the other end's segment lost, then its bare ACK ahead of the guest: taken (it
              acknowledges the guest's data), and not answered (no ACK ping-pong)
  orphans     closed connections whose FIN is not acknowledged (FIN_WAIT_1, LAST_ACK): no
              buffers held while they finish
  pollscan    (poll() needed) the scan against a server driven by poll(), telnetd's shape
  pollstall   (poll() needed) one client stops reading: the poll() server serves another

  tools/qemu-tcp.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --test
  tools/qemu-tcp.py ... --scenario window --verbose     (the segments as they go)

tools/qemu-run.py runs the same scenarios as smoke steps ('@tcp-raw <name>').
"""

import argparse
import importlib.util
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rawnet                                       # noqa: E402
from rawnet import ACK, FIN, PSH, RST, Conn, Server   # noqa: E402


def load_harness():
    spec = importlib.util.spec_from_file_location('qemurun', os.path.join(HERE, 'qemu-run.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def pattern(n):
    """what `nettest writer` sends (the same as `nettest send`)"""
    return b''.join(b'nettest %07d\n' % i for i in range((n + 15) // 16))[:n]


class Rig:
    """one boot of the guest on the raw link, its shell typed to over the serial line"""
    def __init__(self, qr, kernel, initrd, qemu='qemu-system-i386', mem='128', verbose=False):
        self.qr = qr
        self.net = rawnet.Net(verbose)
        self.q = qr.Qemu(kernel, qemu, mem, self.net.qemu_args(), None, None, None, initrd, False, False, 'tcpka=3,1', 0)
        self.ok = self.q.expect(qr.PROMPT, 60)
        if self.ok:
            self.ok = self.run('ifconfig eth0 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3')

    def mark(self):
        m = len(self.q.transcript)
        self.q.cursor = m
        return m

    def since(self, m):
        return self.q.transcript[m:].decode(errors='replace')

    def run(self, line, *expects, timeout=15):
        """a command, what it must print (in order), then the prompt"""
        self.mark()
        self.q.send(line + '\n')
        return all(self.q.expect(e, timeout) for e in expects) and self.q.expect(self.qr.PROMPT, timeout)

    def start(self, line, first):
        """a command in the background (&) that must print first: before the shell's '] started' or after it"""
        m = self.mark()
        self.q.send(line + ' &\n')
        ok = self.q.expect('] started', 10) and self.q.expect(self.qr.PROMPT, 10)
        return ok and (first in self.since(m) or self.q.expect(first, 10))

    def expect(self, text, timeout=15):
        return self.q.expect(text, timeout)

    def finish(self, c, timeout=5.0):
        """the guest's FIN on c answered (so its socket ends at once)"""
        if c.fin is None:
            s = self.net.wait(lambda s: c.mine(s) and s.flags & FIN, timeout)
            if not s:
                return False
            c.fin = (s.seq + len(s.data)) & 0xffffffff
        c.rcv = (c.fin + 1) & 0xffffffff
        c.ack(flags=ACK | FIN)
        return True

    def close(self):
        self.q.close()
        self.net.close()


def handshakes(rig, base, n, rst):
    """n half-open connections from ports base..: SYN, the guest's SYN-ACK, and a reset when rst"""
    ok = True
    for i in range(n):
        c = Conn(rig.net, base + i, 23)
        sa = c.syn()
        ok = ok and sa is not None
        if sa and rst:
            c.rst()
    return ok


def real_client(rig, port, data=b'hello\n'):
    """a client that connects, sends data and closes; its SYN-ACK must come. The connection, or None"""
    c = Conn(rig.net, port, 23)
    if not c.connect():
        return None
    c.ack(flags=ACK | PSH, data=data)
    c.ack(flags=ACK | FIN)
    return c


def sc_scan(rig):
    ok = rig.start('nettest sleepsrv 23 4000', 'listening on port 23')
    ok = handshakes(rig, 1000, 5, True) and ok
    c = real_client(rig, 2000)
    ok = rig.expect('nettest: connection from 10.0.2.2 port 2000') and rig.expect('nettest: read 6 bytes, then EOF') and ok
    if c:
        rig.finish(c)
    return ok and c is not None


def sc_flood(rig):
    ok = rig.start('nettest sleepsrv 23 4000', 'listening on port 23')
    ok = handshakes(rig, 1100, 5, False) and ok
    c = real_client(rig, 2100)
    ok = rig.expect('nettest: connection from 10.0.2.2 port 2100') and rig.expect('nettest: read 6 bytes, then EOF') and ok
    if c:
        rig.finish(c)
    return ok and c is not None


def sc_reset(rig):
    ok = rig.start('nettest sleepsrv 23 3000', 'listening on port 23')
    c = Conn(rig.net, 2200, 23)
    ok = c.connect() is not None and ok
    c.rst()
    return rig.expect('nettest: connection from 10.0.2.2 port 2200') and \
        rig.expect('nettest: read 0 bytes, then Connection reset by peer') and ok


def sc_greet(rig):
    Server(rig.net, 2525, greeting=b'220 hello, and goodbye\r\n', close=True)
    Server(rig.net, 2526, reset=True)
    ok = rig.run('nettest recv 10.0.2.2 2525', 'connected to 10.0.2.2 port 2525', '220 hello, and goodbye',
                 'nettest: read 24 bytes, then EOF')
    ok = rig.run('nettest recv 10.0.2.2 2526', 'connected to 10.0.2.2 port 2526',
                 'nettest: read 0 bytes, then Connection reset by peer') and ok
    return ok


def sc_window(rig):
    n = 20000
    ok = rig.start('nettest writer 23 %d 4096' % n, 'listening on port 23')
    c = Conn(rig.net, 2300, 23, win=4096)
    ok = c.connect() is not None and ok
    got = c.receive(4096, win_for=lambda g: max(0, 4096 - g))     # a 4 KB buffer nobody reads: then shut
    probes, t0 = 0, time.time()
    while time.time() - t0 < 3:                     # shut: the guest probes, and hears 0
        s = rig.net.wait(lambda s: c.is_probe(s), 0.5)
        if s:
            probes += 1
            c.ack(win=0)
    # open without an update: the next probe's answer says so
    got += c.receive(n - len(got), win_for=lambda g: 8192, timeout=30)
    ok = ok and probes >= 1 and got == pattern(n)
    ok = rig.expect('nettest: wrote %d bytes in' % n, 15) and ok
    rig.finish(c)
    print('        (%d bytes, %d probes of the shut window answered)' % (len(got), probes))
    return ok


def sc_fin(rig):
    ok = rig.start('nettest reader 23', 'listening on port 23')
    c = Conn(rig.net, 2400, 23)
    ok = c.connect() is not None and ok
    data = bytes(65 + i % 26 for i in range(6 * 1460))
    base = c.snd
    for i in range(6):                              # more than the 8 KB the guest takes, the FIN on the last
        piece = data[i * 1460:(i + 1) * 1460]
        rig.net.send(c.pp, c.gp, base + i * 1460, c.rcv, ACK | PSH | (FIN if i == 5 else 0), 8192, piece)
    end = base + len(data)
    deadline = time.time() + 15
    while time.time() < deadline:                   # send again what the guest has not taken, as its window allows
        s = rig.net.wait(lambda s: c.mine(s) and s.flags & ACK, 1.0)
        if not s:
            continue
        acked = s.ack
        if acked == (end + 1) & 0xffffffff:
            break
        if s.win and not rawnet.seq_lt(end, acked):
            off = (acked - base) & 0xffffffff
            piece = data[off:off + min(1460, s.win)]
            last = off + len(piece) == len(data)
            rig.net.send(c.pp, c.gp, acked, c.rcv, ACK | PSH | (FIN if last else 0), 8192, piece)
    c.snd = end + 1
    ok = rig.expect('nettest: read %d bytes, then EOF' % len(data)) and ok
    rig.finish(c)
    return ok


def sc_keepalive(rig):
    ok = rig.start('nettest reader 23', 'listening on port 23')
    c = Conn(rig.net, 2600, 23)
    ok = c.connect() is not None and ok
    t0, probes = time.time(), 0
    ok = rig.expect('nettest: read 0 bytes, then Connection timed out', 20) and ok   # nothing answered
    probes = len([s for s in rig.net.drain(lambda s: c.is_probe(s))])
    dt = time.time() - t0
    ok = ok and probes == 5 and 6 < dt < 15
    print('        (a silent other end: given up after %.1f s and %d probes)' % (dt, probes))
    ok = rig.start('nettest reader 23', 'listening on port 23') and ok
    c = Conn(rig.net, 2601, 23)
    ok = c.connect() is not None and ok
    t0, answered = time.time(), 0
    while time.time() - t0 < 10:                    # answered: the connection stays
        s = rig.net.wait(lambda s: c.is_probe(s), 0.5)
        if s:
            answered += 1
            c.ack()
    c.ack(flags=ACK | PSH, data=b'bye\n')
    c.ack(flags=ACK | FIN)
    ok = rig.expect('nettest: read 4 bytes, then EOF') and answered >= 2 and ok
    rig.finish(c)
    print('        (an other end that answers: %d probes answered in 10 s, the connection kept)' % answered)
    return ok


def sc_churn(rig):
    Server(rig.net, 7000)
    return rig.run('nettest churn 10.0.2.2 7000 40', 'nettest: 40 connections to 10.0.2.2 port 7000, 0 failed', timeout=60)


def sc_reserve(rig):
    Server(rig.net, 8080, close=True, reply=lambda d: b'HTTP/1.0 200 OK\r\n\r\nthe reserved socket\n')
    ok = rig.start('nettest hold 5400 8000', 'then: Too many open files')     # 13 descriptors: 13 sockets
    ok = rig.start('nettest hold 5500 8000', 'then: File table overflow') and ok   # the rest a program may have
    ok = rig.run('httpget 10.0.2.2 8080 /', 'the reserved socket', 'bytes received') and ok
    rig.q.wait(8)
    return ok


def sc_sws(rig):
    n = 30000
    ok = rig.start('nettest writer 23 %d 100' % n, 'listening on port 23')
    c = Conn(rig.net, 2900, 23, win=4096)
    ok = c.connect() is not None and ok
    got, segs = b'', 0
    deadline = time.time() + 10
    while len(got) < 12000 and time.time() < deadline:   # a reader that keeps up
        s = rig.net.wait(lambda s: c.mine(s) and s.data, 1.0)
        if not s:
            continue
        if s.seq == c.rcv:
            got += s.data
            c.rcv = (c.rcv + len(s.data)) & 0xffffffff
            segs += 1
        c.ack(win=4096)
    size = len(got) // max(segs, 1)
    got += c.receive(4096, win_for=lambda g: max(0, 4096 - g))     # then it stops reading: the window shuts
    t0, small, sizes = time.time(), 0, []
    while time.time() - t0 < 4:                     # a silly reader: room for 100 bytes, never more
        s = rig.net.wait(lambda s: c.mine(s) and (s.data or c.is_probe(s)), 0.5)
        if not s:
            continue
        if s.data and s.seq == c.rcv:
            got += s.data
            c.rcv = (c.rcv + len(s.data)) & 0xffffffff
            small += 1
            sizes.append(len(s.data))
        c.ack(win=100)
    got += c.receive(n - len(got), win_for=lambda g: 8192, timeout=30)   # it reads everything again
    # no storm: a segment a second at most into the small window, each filling it (no smaller pieces)
    ok = ok and got == pattern(n) and 1 <= small <= 6 and all(x == 100 for x in sizes)
    ok = rig.expect('nettest: wrote %d bytes in' % n, 15) and ok
    rig.finish(c)
    print('        (100-byte writes: %d bytes a segment; a reader offering 100 bytes: %d segments in 4 s)' % (size, small))
    return ok


def heap_used(rig):
    """the kernel heap's bytes in use, as heaptest reports them (None when it says nothing)"""
    m = rig.mark()
    rig.run('heaptest', 'HEAPTEST', timeout=30)
    found = re.search(r'used (\d+) ->', rig.since(m))
    return int(found.group(1)) if found else None


def sc_linger(rig):
    ok = rig.start('nettest writer 23 100', 'listening on port 23')
    c = Conn(rig.net, 3000, 23)
    ok = c.connect() is not None and ok
    ok = c.receive(100) == pattern(100) and ok
    fin = rig.net.wait(lambda s: c.mine(s) and s.flags & FIN, 5)    # the guest closes: its FIN acknowledged,
    ok = fin is not None and ok                                      # none sent back (FIN_WAIT_2)
    if fin:
        c.rcv = (fin.seq + len(fin.data) + 1) & 0xffffffff
        c.ack()
    ok = rig.expect('nettest: wrote 100 bytes in') and ok
    rig.q.wait(0.5)
    ok = rig.run('netstat', 'FIN_WAIT_2') and ok    # (the program is gone by its prompt)
    held = heap_used(rig)
    c.ack(flags=ACK | FIN)                          # now the other end's FIN: TIME_WAIT, and the link goes
    t0 = time.time()
    rig.q.monitor('set_link d0 off')
    m = rig.mark()
    tw = heap_used(rig)                             # in TIME_WAIT, or across its end: the heap stays as it is
    ok = 'HEAPTEST PASS' in rig.since(m) and ok
    rig.q.wait(max(0.0, 3 - (time.time() - t0)))
    ok = rig.run('netstat', 'no TCP sockets') and ok    # 2 s, link or not
    rig.q.monitor('set_link d0 on')
    rig.q.wait(1)
    after = heap_used(rig)
    ok = ok and after is not None and held == after and tw == after
    print('        (heap in use: %s bytes in FIN_WAIT_2, %s in TIME_WAIT, %s with the connection gone)' % (held, tw, after))
    return ok


def sc_lost(rig):
    ok = rig.start('nettest burst 23 1 -1 1 2', 'listening on port 23')
    c = Conn(rig.net, 3100, 23, win=1)              # a window of one byte: the 2 bytes after it wait
    ok = c.connect() is not None and ok
    first = rig.net.wait(lambda s: c.mine(s) and s.data, 5)          # lost on the way: not acknowledged
    again = rig.net.wait(lambda s: c.mine(s) and s.data, 3)          # sent again: that byte, no more
    ok = first is not None and again is not None and again.seq == first.seq and len(again.data) == 1 and ok
    got = b''
    if again:
        got = again.data
        c.rcv = (again.seq + len(again.data)) & 0xffffffff
        c.ack(win=8192)                             # acknowledged, and the window opens
    got += c.receive(2, win_for=lambda g: 8192, timeout=5)
    ok = got == pattern(3) and rig.finish(c) and ok                  # the held bytes, then the FIN
    ok = rig.expect('nettest: burst closed') and ok
    print('        (sent again: %d byte(s); then %d more and the FIN)' % (len(again.data) if again else 0, len(got) - 1))
    return ok


def sc_latency(rig):
    sizes, bursts = (1, 20, 30), 4
    per = sum(sizes)
    ok = rig.start('nettest burst 23 %d 300 %s' % (bursts, ' '.join(map(str, sizes))), 'listening on port 23')
    c = Conn(rig.net, 3200, 23)
    ok = c.connect() is not None and ok
    got, at, ack_due = b'', [], None
    deadline = time.time() + 10
    while len(got) < bursts * per and time.time() < deadline:
        s = rig.net.wait(lambda s: c.mine(s) and s.data, 0.5 if ack_due is None else max(0.001, ack_due - time.time()))
        if s and s.seq == c.rcv:
            at += [s.t] * len(s.data)               # when each byte came
            got += s.data
            c.rcv = (c.rcv + len(s.data)) & 0xffffffff
            if ack_due is None:
                ack_due = time.time() + 0.2         # a client that delays its ACK 200 ms
        if ack_due is not None and time.time() >= ack_due:
            c.ack()
            ack_due = None
    c.ack(flags=ACK | FIN)
    ok = rig.expect('nettest: read 0 bytes, then EOF') and ok
    rig.finish(c)
    spread = [round((at[b * per + per - 1] - at[b * per]) * 1000) for b in range(bursts) if len(at) >= (b + 1) * per]
    ok = ok and got == pattern(bursts * per) and len(spread) == bursts and max(spread) < 100
    print('        (bursts of %s-byte writes, ACKs delayed 200 ms: %s ms from the first byte of each to its last)' % (
        '+'.join(map(str, sizes)), spread))
    return ok


def sc_zwprobe(rig):
    ok = rig.start('nettest sleepsrv 23 5000', 'listening on port 23')
    c = Conn(rig.net, 3300, 23)
    ok = c.connect() is not None and ok
    data = pattern(8192)
    for i in range(0, len(data), 1460):             # the guest's receive buffer, full: nobody reads yet
        c.ack(flags=ACK | PSH, data=data[i:i + 1460])
    rig.q.wait(0.5)
    rig.net.drain()
    answers = 0
    for i in range(3):                              # a byte into the shut window, not taken
        rig.net.send(c.pp, c.gp, c.snd, c.rcv, ACK | PSH, 8192, b'+')
        a = rig.net.wait(lambda s: c.mine(s) and not s.data and not c.is_probe(s) and s.ack == c.snd, 1.0)
        answers += a is not None and a.win == 0
    c.ack(flags=ACK | FIN)                          # the end, where the probes' byte was
    ok = rig.expect('nettest: read 8192 bytes, then EOF', 10) and answers == 3 and ok
    rig.finish(c)
    print('        (three 1-byte probes of a full receive buffer: %d answered, window 0)' % answers)
    return ok


def sc_evict(rig):
    ok = rig.start('nettest sleepsrv 23 3000', 'listening on port 23')
    c = Conn(rig.net, 3400, 23)
    ok = c.syn() is not None and ok                 # a client whose ACK is slow to come
    others = [Conn(rig.net, 3410 + i, 23) for i in range(4)]
    for o in others:                                # four more SYNs: the backlog (4) is full, and the oldest
        ok = o.syn() is not None and ok             # half-open entry, the slow client's, makes room
    rig.net.drain()
    c.ack(flags=ACK | PSH, data=b'hello\n')         # its handshake's ACK, and data: no connection for it now
    r = rig.net.wait(lambda s: c.mine(s), 2)
    ok = r is not None and bool(r.flags & RST) and ok
    for o in others:
        o.rst()
    d = real_client(rig, 3420)                      # the next client is served
    ok = d is not None and rig.expect('nettest: connection from 10.0.2.2 port 3420') and \
        rig.expect('nettest: read 6 bytes, then EOF') and ok
    if d:
        rig.finish(d)
    print('        (the evicted client: %s)' % ('a reset' if r is not None and r.flags & RST else 'no answer' if r is None else repr(r)))
    return ok


def sc_ackahead(rig):
    ok = rig.start('nettest burst 23 1 0 100', 'listening on port 23')
    c = Conn(rig.net, 3600, 23)
    ok = c.connect() is not None and ok
    d = rig.net.wait(lambda s: c.mine(s) and s.data, 5)          # the guest's 100 bytes arrive,
    ok = d is not None and ok
    if d:
        c.rcv = (d.seq + len(d.data)) & 0xffffffff
    c.snd += 10                                     # but 10 bytes of ours are lost on the way,
    c.ack()                                         # and our ACK of the 100 comes after them: ahead of the guest
    t0, n = time.time(), 0
    while time.time() - t0 < 2:                     # nothing comes back: no answer to it, and no resending
        s = rig.net.wait(lambda s: c.mine(s), 0.25)
        if s:
            n += 1
            if s.data or s.seq != c.rcv:            # (a strict end answers what it cannot take)
                c.ack()
    rig.net.send(c.pp, c.gp, c.snd - 10, c.rcv, ACK | PSH, 8192, b'0123456789')   # the lost bytes again
    c.ack(flags=ACK | FIN)
    ok = rig.expect('nettest: read 10 bytes, then EOF') and n == 0 and ok
    rig.finish(c)
    print('        (segments from the guest in 2 s after an ACK ahead of it: %d)' % n)
    return ok


def sc_orphans(rig):
    ok = rig.start('nettest writer 23 100', 'listening on port 23')
    a = Conn(rig.net, 3500, 23)
    ok = a.connect() is not None and ok
    ok = a.receive(100) == pattern(100) and ok      # the data acknowledged, the FIN behind it not (FIN_WAIT_1)
    fa = rig.net.wait(lambda s: a.mine(s) and s.flags & FIN, 5)
    ok = fa is not None and rig.expect('nettest: wrote 100 bytes in') and ok
    ok = rig.start('nettest reader 23', 'listening on port 23') and ok
    b = Conn(rig.net, 3501, 23)
    ok = b.connect() is not None and ok
    b.ack(flags=ACK | FIN)                          # its other end closes first, the reader after the end:
    fb = rig.net.wait(lambda s: b.mine(s) and s.flags & FIN, 5)  # its FIN not acknowledged (LAST_ACK)
    ok = fb is not None and rig.expect('nettest: read 0 bytes, then EOF') and ok
    rig.q.wait(0.5)
    m = rig.mark()
    rig.run('netstat')
    ok = 'FIN_WAIT_1' in rig.since(m) and 'LAST_ACK' in rig.since(m) and ok
    held = heap_used(rig)
    if fa:                                          # now they finish
        a.fin = (fa.seq + len(fa.data)) & 0xffffffff
        rig.finish(a)
    if fb:
        b.rcv = (fb.seq + len(fb.data) + 1) & 0xffffffff
        b.ack()
    rig.q.wait(3)
    ok = rig.run('netstat', 'no TCP sockets') and ok
    after = heap_used(rig)
    ok = ok and after is not None and held == after
    print('        (heap in use: %s bytes with the two finishing, %s with them gone)' % (held, after))
    return ok


def poll_missing(rig, m):
    return 'poll: Function not implemented' in rig.since(m)


def sc_pollscan(rig):
    m = rig.mark()
    rig.q.send('nettest pollsrv 23 1 &\n')
    if not (rig.expect('] started', 10) and rig.expect(rig.qr.PROMPT, 10)):
        return False
    rig.q.wait(1)
    if poll_missing(rig, m):
        print('        (skipped: no poll() in this kernel yet)')
        return True
    ok = 'polling on port 23' in rig.since(m) or rig.expect('polling on port 23')
    ok = handshakes(rig, 1200, 5, True) and ok
    c = Conn(rig.net, 2700, 23)
    ok = c.connect() is not None and ok
    c.ack(flags=ACK | PSH, data=b'hi\n')
    echo = c.receive(3)
    c.ack(flags=ACK | FIN)
    ok = echo == b'hi\n' and rig.expect('nettest: 1 connections ended') and ok
    rig.finish(c)
    return ok


def sc_pollstall(rig):
    m = rig.mark()
    rig.q.send('nettest pollsrv 23 2 &\n')
    if not (rig.expect('] started', 10) and rig.expect(rig.qr.PROMPT, 10)):
        return False
    rig.q.wait(1)
    if poll_missing(rig, m):
        print('        (skipped: no poll() in this kernel yet)')
        return True
    a = Conn(rig.net, 2800, 23, win=2048)
    ok = a.connect() is not None
    blob = bytes(97 + i % 26 for i in range(7000))
    for i in range(0, len(blob), 1000):             # A sends a lot and takes only 2 KB of the echo
        a.ack(flags=ACK | PSH, data=blob[i:i + 1000])
    got_a = a.receive(2048, win_for=lambda g: 2048 - g if g < 2048 else 0, timeout=5)
    b = Conn(rig.net, 2801, 23)
    ok = b.connect() is not None and ok
    b.ack(flags=ACK | PSH, data=b'hello from B\n')
    echo_b = b.receive(13, timeout=5)               # served while A stays shut
    b.ack(flags=ACK | FIN)
    got_a += a.receive(len(blob) - len(got_a), win_for=lambda g: 8192, timeout=30)   # A reads again
    a.ack(flags=ACK | FIN)
    ok = echo_b == b'hello from B\n' and got_a == blob and rig.expect('nettest: 2 connections ended', 20) and ok
    rig.finish(a)
    rig.finish(b)
    return ok


SCENARIOS = [
    ('scan', sc_scan, 'a SYN scan does not make a listener deaf'),
    ('flood', sc_flood, 'SYNs never acknowledged give their place to a real client'),
    ('reset', sc_reset, 'a connection reset before accept(): accepted, read() says ECONNRESET'),
    ('greet', sc_greet, 'a server that greets and closes (or resets) at once: connect() succeeds, read() says so'),
    ('window', sc_window, 'a shut window is probed until it opens: every byte arrives'),
    ('fin', sc_fin, 'a FIN behind data that did not fit waits for that data'),
    ('keepalive', sc_keepalive, 'a vanished other end times out, one that answers stays'),
    ('churn', sc_churn, '40 connections in a row: TIME_WAIT does not use the sockets up'),
    ('reserve', sc_reserve, "programs that take every socket leave one for the kernel's own connection"),
    ('sws', sc_sws, 'small writes and small windows make no storm of small segments'),
    ('linger', sc_linger, 'FIN_WAIT_2 and TIME_WAIT hold no buffers; TIME_WAIT ends on time with the link down'),
    ('lost', sc_lost, 'a lost small segment is sent again as it was, and what waited behind it follows'),
    ('latency', sc_latency, "small writes are not held for a client's delayed ACK"),
    ('zwprobe', sc_zwprobe, 'a byte probing a full receive buffer gets its ACK'),
    ('evict', sc_evict, 'a client whose half-open connection was given up gets a reset, not silence'),
    ('ackahead', sc_ackahead, 'a bare ACK ahead of the guest counts and gets no answer'),
    ('orphans', sc_orphans, 'a closed connection still finishing holds no buffers'),
    ('pollscan', sc_pollscan, 'a SYN scan against a poll() server (when poll() is there)'),
    ('pollstall', sc_pollstall, 'a client that stops reading holds up no other (when poll() is there)'),
]


def run_scenarios(rig, names=None):
    """(name, ok) for each scenario, in order; a scenario that raises fails"""
    results = []
    for name, fn, what in SCENARIOS:
        if names and name not in names:
            continue
        try:
            ok = bool(fn(rig))
        except Exception as e:                      # a broken scenario fails, the others still run
            print('        (%s: %r)' % (name, e))
            ok = False
        results.append(('TCP %s: %s' % (name, what), ok))
        rig.q.wait(0.5)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', default='build/O0/surfos.bin')
    ap.add_argument('--initrd', default='build/images/initrd.tar')
    ap.add_argument('--qemu', default='qemu-system-i386')
    ap.add_argument('--mem', default='128')
    ap.add_argument('--scenario', action='append', help='only these (repeatable)')
    ap.add_argument('--verbose', action='store_true', help='print every segment')
    ap.add_argument('--test', action='store_true', help='run the scenarios (the default)')
    a = ap.parse_args()
    qr = load_harness()
    rig = Rig(qr, a.kernel, a.initrd, a.qemu, a.mem, a.verbose)
    try:
        if not rig.ok:
            print(qr.tail(rig.q.text()))
            print('TCP TEST FAILED: no shell prompt, or ifconfig failed')
            return 1
        results = run_scenarios(rig, a.scenario)
        rig.q.pump(0.5)
        text = rig.q.text()
        results += [('no "%s" on the console' % bad, bad not in text) for bad in qr.BAD]
        for name, ok in results:
            print(('PASS ' if ok else 'FAIL ') + name)
        failed = [n for n, ok in results if not ok]
        if failed:
            print('\n---- serial transcript (tail) ----\n' + qr.tail(text, 60))
            print('\nTCP TEST FAILED: %d of %d checks' % (len(failed), len(results)))
            return 1
        print('\nTCP TEST PASSED: %d checks' % len(results))
        return 0
    finally:
        rig.close()


if __name__ == '__main__':
    sys.exit(main())
