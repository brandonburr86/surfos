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
              read() says ECONNRESET; a segment for it after the reset gets a reset
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
  closing     (11/1/26) both ends close at once (CLOSING), and the other end's FIN comes again
              with the ACK of the guest's, as a BSD stack sends it: TIME_WAIT at once
  shrink      a client that shrinks its window to 0 under bytes in flight: probes with their
              backoff, no full segment every second; when it opens, everything arrives
  pollscan    (poll() needed) the scan against a server driven by poll(), telnetd's shape
  pollstall   (poll() needed) one client stops reading: the poll() server serves another
  halfclose   (10/24/26) shutdown(SHUT_WR): the FIN behind a line, then 12000 bytes from the other
              end taken as the program reads them, read() 0 at its FIN, TIME_WAIT, gone; then a
              program that reads late, in TIME_WAIT: what came is kept for it (10/25/26)
  nowait      (10/24/26) connect() with CONNECT_NOWAIT: polled for while its SYN goes unanswered,
              made when one is answered late; refused by a reset (POLLERR, ECONNREFUSED)
  finwait     (10/25/26) a connection its program half-closed, its FIN acknowledged (FIN_WAIT_2),
              then an other end that vanishes: keepalive gives it up as an open one (5 probes)
  ncstuck     (10/25/26) nc -w 3 sending a file to an other end that takes nothing (its window shut)
              and half-closes at once: -w ends nc after 3 s (status 1)
  mdns        (10/25/26, UDP) the multicast DNS responder: mdns on probes for surfos.local and
              announces it (to the group, TTL 255); queries to the group answered to the group, a QU
              one to the querier, a one-shot one with its ID; others not; a conflict while probing
              makes it surfos-2; mdns off answers nothing (the e1000's multicast filter on the way)

  tools/qemu-tcp.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --test
  tools/qemu-tcp.py ... --scenario window --verbose     (the segments as they go)

tools/qemu-run.py runs the same scenarios as smoke steps ('@tcp-raw <name>').
"""

import argparse
import importlib.util
import os
import re
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rawnet                                       # noqa: E402
from rawnet import ACK, FIN, PSH, RST, SYN, Conn, Server   # noqa: E402


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
        self.net.set_qport(self.q.ports['dgram'])  # 10/18/26 (round fs-fix-11): the port QEMU was given
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
    rig.q.wait(0.2)
    rig.net.drain()
    c.ack(flags=ACK | PSH, data=b'late\n')         # the failed connection is none: a reset, not an ACK
    r = rig.net.wait(lambda s: c.mine(s), 2)
    ok = r is not None and bool(r.flags & RST) and ok
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


def sc_backlog(rig):
    """10/17/26 (httpd): a listener that asked for a backlog of 8 while its program does not accept: eight clients
    finish their handshakes, a ninth SYN gets no answer (every place holds a whole connection), and once the
    program accepts, the first is served"""
    ok = rig.start('nettest sleepsrv 23 3000 8', 'listening on port 23')
    conns = [Conn(rig.net, 3800 + i, 23) for i in range(8)]
    whole = sum(c.connect() is not None for c in conns)
    ninth = Conn(rig.net, 3808, 23)
    late = ninth.syn(1.0)
    first = conns[0]
    first.ack(flags=ACK | PSH, data=b'hello\n')
    first.ack(flags=ACK | FIN)
    ok = rig.expect('nettest: connection from 10.0.2.2 port 3800') and rig.expect('nettest: read 6 bytes, then EOF') and \
        whole == 8 and late is None and ok
    rig.finish(first)
    for c in conns[1:]:                             # the listener is closed: the rest were reset; nothing lingers
        c.rst()
    print('        (eight handshakes of eight finished; the ninth SYN: %s)' % ('no answer' if late is None else repr(late)))
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


def sc_closing(rig):
    """11/1/26 (audit B91): the other end's FIN crosses the guest's, which is not acknowledged yet (CLOSING), and comes
    again with the ACK of the guest's, as a BSD stack such as QEMU's user network sends it. That ACK counts, though
    nothing else in the segment is new: the socket is in TIME_WAIT at once, where it gives its place up when one is
    needed, not in CLOSING until the guest's FIN goes again, a second later. A FIN once more is answered too"""
    ok = rig.start('nettest writer 23 100', 'listening on port 23')
    c = Conn(rig.net, 3550, 23)
    ok = c.connect() is not None and ok
    ok = c.receive(100) == pattern(100) and ok
    fin = rig.net.wait(lambda s: c.mine(s) and s.flags & FIN, 5)
    ok = fin is not None and rig.expect('nettest: wrote 100 bytes in') and ok
    if not fin:
        return False
    ours, theirs = c.snd, (fin.seq + len(fin.data)) & 0xffffffff
    answered = lambda s: c.mine(s) and s.ack == (ours + 1) & 0xffffffff and not s.flags & FIN
    rig.net.send(c.pp, c.gp, ours, theirs, ACK | FIN)               # it crossed the guest's: that one not acknowledged
    ok = rig.net.wait(answered, 3) is not None and ok
    ok = rig.run('netstat', 'CLOSING') and ok
    rig.net.send(c.pp, c.gp, ours, theirs + 1, ACK | FIN)           # again, now with the ACK of the guest's
    ok = rig.net.wait(answered, 3) is not None and ok
    m = rig.mark()
    ok = rig.run('netstat', 'TIME_WAIT') and 'CLOSING' not in rig.since(m) and ok
    rig.net.send(c.pp, c.gp, ours, theirs + 1, ACK | FIN)
    ok = rig.net.wait(answered, 3) is not None and ok
    rig.q.wait(3)
    return rig.run('netstat', 'no TCP sockets') and ok


def sc_shrink(rig):
    n = 12000
    ok = rig.start('nettest writer 23 %d 4096' % n, 'listening on port 23')
    c = Conn(rig.net, 3700, 23, win=4096)
    ok = c.connect() is not None and ok
    t0, data, probes = time.time(), 0, 0
    while time.time() - t0 < 5:                     # nothing taken, and the window shrunk to 0 under what is in flight
        s = rig.net.wait(lambda s: c.mine(s), 0.5)
        if not s:
            continue
        if s.data:
            data += 1
        elif c.is_probe(s):
            probes += 1
        c.ack(win=0)
    c.ack(win=8192)                                 # it opens: what was in flight comes again, and the rest
    got = c.receive(n, win_for=lambda g: 8192, timeout=20)
    ok = rig.expect('nettest: wrote %d bytes in' % n, 15) and got == pattern(n) and data <= 3 and probes >= 1 and ok
    rig.finish(c)
    print('        (5 s with the window shrunk to 0: %d data segments, %d probes; then %d bytes)' % (data, probes, len(got)))
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


def sc_nowait(rig):
    """10/24/26: connect() with CONNECT_NOWAIT: the descriptor at once while its SYN goes unanswered, the program
    polling on (it is not held in the call); the SYN answered 2.5 s late makes the connection (POLLOUT), and the
    program's bytes and this end's answer go through; a SYN answered with a reset fails it (POLLERR, read() says
    ECONNREFUSED)"""
    ok = rig.start('nettest nowait 10.0.2.2 7400', 'nettest: connect started')
    ok = rig.net.wait(lambda s: s.dport == 7400 and s.flags & SYN, 5) is not None and ok
    m = rig.mark()
    rig.q.wait(2.5)                                 # unanswered: the program polls on
    Server(rig.net, 7400, close=True, reply=lambda d: b'got it\n' if d == b'hello\n' else b'what?\n')
    ok = rig.expect('nettest: connected after ', 15) and rig.expect('got it', 10) and \
        rig.expect('nettest: read 7 bytes, then EOF', 10) and ok
    polls = re.search(r'connected after (\d+) ms \((\d+) polls in between\)', rig.since(m))
    ok = polls is not None and int(polls.group(1)) >= 2500 and int(polls.group(2)) >= 4 and ok
    if polls:
        print('        (connected after %s ms, %s polls in between)' % polls.groups())
    ok = rig.start('nettest nowait 10.0.2.2 7401', 'nettest: connect started') and ok
    syn = rig.net.wait(lambda s: s.dport == 7401 and s.flags & SYN, 5)
    if syn:
        rig.net.send(7401, syn.sport, 0, syn.seq + 1, RST | ACK, 0)
    ok = syn is not None and rig.expect('nettest: the connection failed: Connection refused', 10) and ok
    return ok


def sc_halfclose(rig):
    """10/24/26: shutdown(SHUT_WR): the program's line, then its FIN behind it; acknowledged (FIN_WAIT_2), the
    program still reads: 12000 bytes sent after the FIN, more than its 8 KB window, are all taken and acknowledged
    (no reset) as it reads them; then this end's FIN: read() says 0, the FIN is acknowledged (TIME_WAIT), and the
    socket is gone 2 s later"""
    ok = rig.start('nettest halfclose 23', 'listening on port 23')
    c = Conn(rig.net, 3200, 23)
    ok = c.connect() is not None and ok
    got = c.receive(4096)                           # the line, then the FIN: receive() stops there
    ok = got == b'nettest: half-closed after this line\n' and c.fin is not None and ok
    if c.fin is not None:
        c.rcv = (c.fin + 1) & 0xffffffff
        c.ack()                                     # its FIN acknowledged: FIN_WAIT_2, and it reads on
    ok = rig.expect('nettest: shut; reading what comes after the FIN') and ok
    data = bytes(97 + i % 26 for i in range(12000))
    base, reset = c.snd, False
    end = (base + len(data)) & 0xffffffff
    for i in range(0, len(data), 1460):             # all at once: what does not fit comes again below
        rig.net.send(c.pp, c.gp, (base + i) & 0xffffffff, c.rcv, ACK | PSH, 8192, data[i:i + 1460])
    deadline = time.time() + 15
    while time.time() < deadline:
        s = rig.net.wait(lambda s: c.mine(s) and s.flags & (ACK | RST), 1.0)
        if not s:
            continue
        if s.flags & RST:
            reset = True
            break
        if s.ack == end:
            break
        if s.win and rawnet.seq_lt(s.ack, end):    # what its window takes now, from where it is
            off = (s.ack - base) & 0xffffffff
            rig.net.send(c.pp, c.gp, s.ack, c.rcv, ACK | PSH, 8192, data[off:off + min(1460, s.win)])
    c.snd = end
    c.ack(flags=ACK | FIN)
    ok = not reset and rig.expect('nettest: half-close: ok, %d bytes after the FIN' % len(data)) and ok
    ack = rig.net.wait(lambda s: c.mine(s) and s.flags & ACK and s.ack == c.snd, 3)
    ok = ack is not None and ok
    rig.q.wait(3)
    ok = rig.run('netstat', 'no TCP sockets') and ok
    # 10/25/26 (review: the above reaches TIME_WAIT with bytes unread only when the program happens to be slow): the
    # program reads 1 s after its shutdown, by when 6000 bytes and this end's FIN have come and been acknowledged
    # (TIME_WAIT, 2 s): the receive buffer is kept for it, and it reads them all, then the end
    ok = rig.start('nettest halfclose 23 1000', 'listening on port 23') and ok
    c = Conn(rig.net, 3201, 23)
    ok = c.connect() is not None and ok
    got = c.receive(4096)
    ok = got == b'nettest: half-closed after this line\n' and c.fin is not None and ok
    if c.fin is not None:
        c.rcv = (c.fin + 1) & 0xffffffff
        c.ack()
    ok = rig.expect('nettest: shut; reading what comes after the FIN') and ok
    data = bytes(65 + i % 26 for i in range(6000))
    base = c.snd
    for i in range(0, len(data), 1460):
        rig.net.send(c.pp, c.gp, (base + i) & 0xffffffff, c.rcv, ACK | PSH, 8192, data[i:i + 1460])
    c.snd = (base + len(data)) & 0xffffffff
    c.ack(flags=ACK | FIN)
    ack = rig.net.wait(lambda s: c.mine(s) and s.flags & ACK and s.ack == c.snd, 3)
    ok = ack is not None and rig.expect('nettest: half-close: ok, %d bytes after the FIN' % len(data), 10) and ok
    rig.q.wait(3)
    ok = rig.run('netstat', 'no TCP sockets') and ok
    return ok


def sc_finwait(rig):
    """10/25/26 (review): a connection whose program half-closed it waits in FIN_WAIT_2 for the other end's data
    and FIN with nothing in flight: keepalive probes it as an open one, so an other end that has vanished is given
    up (ETIMEDOUT after tcpka's 3 s and 5 probes, as in keepalive) and the read ends. Nothing ended it before"""
    ok = rig.start('nettest shutreader 23', 'listening on port 23')
    c = Conn(rig.net, 3300, 23)
    ok = c.connect() is not None and ok
    got = c.receive(4096, timeout=5)                # nothing, then its FIN: receive() stops there
    ok = got == b'' and c.fin is not None and rig.expect('nettest: shut at once; reading') and ok
    if c.fin is not None:
        c.rcv = (c.fin + 1) & 0xffffffff
        c.ack()                                     # its FIN acknowledged: FIN_WAIT_2, then silence
    t0 = time.time()
    ok = rig.expect('nettest: read 0 bytes, then Connection timed out', 20) and ok
    probes = len([s for s in rig.net.drain(lambda s: c.is_probe(s))])
    dt = time.time() - t0
    ok = ok and probes == 5 and 6 < dt < 15
    print('        (FIN_WAIT_2, the other end gone: given up after %.1f s and %d probes)' % (dt, probes))
    return ok


class ShutPeer:
    """10/25/26: the other end of a connection the guest makes to port: its SYN answered with the window shut, the
    FIN at once behind it, every probe after that answered with the window still shut; nothing is ever taken"""
    def __init__(self, net, port, iss=7000):
        self.net, self.port, self.iss, self.rcv, self.gp = net, port, iss, None, None
        net.servers[port] = self

    def handle(self, s):
        if s.flags & RST:
            return
        if s.flags & SYN:
            self.gp, self.rcv = s.sport, (s.seq + 1) & 0xffffffff
            self.net.send(self.port, s.sport, self.iss, self.rcv, SYN | ACK, 0)
            self.net.send(self.port, s.sport, self.iss + 1, self.rcv, FIN | ACK, 0)
        elif self.rcv is not None:
            self.net.send(self.port, s.sport, self.iss + 2, self.rcv, ACK, 0)

    def reset(self):
        """the connection given up from this end (the guest's orphan goes at once)"""
        if self.gp is not None:
            self.net.send(self.port, self.gp, self.iss + 2, self.rcv, RST | ACK, 0)
        del self.net.servers[self.port]


def sc_ncstuck(rig):
    """10/25/26 (review): nc -w 3 sending a file to an other end that takes none of it (the window shut from its SYN
    on) and half-closes at once: what waits never fits, nothing comes, and -w ends nc 3 s after its last write, with
    status 1. Its rests after the other end's FIN kept poll() from timing out, so it never ended, and spun once the
    time was up. (Through QEMU's user network this host's buffers take the whole file under Linux: no test there)"""
    peer = ShutPeer(rig.net, 7500)
    t0 = time.time()
    ok = rig.start('nc -w 3 10.0.2.2 7500 < /initrd/bin/nettest', '] started')
    ok = rig.expect('nc: nothing came or went for 3 s: the connection is closed', 20) and ok
    dt = time.time() - t0
    # the shell says a job's end before the prompt after it; nc's last moments may take longer than the shell's
    # first prompt (a smoke run beside the USB tests showed it), so a few: one that never ends is still caught
    m, prompts = rig.mark(), 0
    while prompts < 6 and '] exit status 1' not in rig.since(m):
        ok = rig.run('') and ok
        prompts += 1
        if '] exit status 1' not in rig.since(m):
            rig.q.wait(0.5)
    ok = '] exit status 1' in rig.since(m) and 3 <= dt < 12 and ok
    print('        (nc ended %.1f s after it started; its end said after %d prompt(s))' % (dt, prompts))
    peer.reset()
    rig.q.wait(1)
    return ok


MDNS_MAC = bytes.fromhex('01005e0000fb')
MDNS_IP = socket.inet_aton('224.0.0.251')


def mdns_name(labels):
    return b''.join(bytes([len(l)]) + l.encode() for l in labels.split('.')) + b'\0'


def mdns_query(name, qtype=1, qu=False, qid=0):
    """10/25/26: a query for name's qtype (1: A, 28: AAAA, 255: ANY), with QU when asked"""
    return struct.pack('!HHHHHH', qid, 0, 1, 0, 0, 0) + mdns_name(name) + struct.pack('!HH', qtype, 0x8001 if qu else 1)


def mdns_records(data):
    """the header and the records of a message as (id, flags, questions, [(section, name, type, class, ttl, rdata)]);
    None for one that does not parse"""
    def name_at(off, depth=0):
        labels = []
        while depth < 64:
            n = data[off]
            if n & 0xc0 == 0xc0:
                rest, _ = name_at(((n & 0x3f) << 8) | data[off + 1], depth + 1)
                return '.'.join(labels + ([rest] if rest else [])), off + 2
            off += 1
            if not n:
                return '.'.join(labels), off
            labels.append(data[off:off + n].decode('ascii', 'replace'))
            off += n
            depth += 1
        raise ValueError('a name loops')
    try:
        qid, flags, qd, an, ns, ar = struct.unpack('!HHHHHH', data[:12])
        off, questions, records = 12, [], []
        for _ in range(qd):
            name, off = name_at(off)
            questions.append((name.lower(),) + struct.unpack('!HH', data[off:off + 4]))
            off += 4
        for section, count in (('an', an), ('ns', ns), ('ar', ar)):
            for _ in range(count):
                name, off = name_at(off)
                rtype, rclass, ttl, size = struct.unpack('!HHIH', data[off:off + 10])
                records.append((section, name.lower(), rtype, rclass, ttl, data[off + 10:off + 10 + size]))
                off += 10 + size
        return qid, flags, questions, records
    except (IndexError, struct.error, ValueError):
        return None


def mdns_a(d, name, ip='10.0.2.15'):
    """d: a response whose answers hold name's A record for ip: (its TTL, cache-flush), or None"""
    m = mdns_records(d.data)
    if not m or not m[1] & 0x8000:
        return None
    for section, n, rtype, rclass, ttl, rdata in m[3]:
        if section == 'an' and n == name and rtype == 1 and rclass & 0x7fff == 1 and rdata == socket.inet_aton(ip):
            return ttl, bool(rclass & 0x8000)
    return None


def mdns_is_probe(d, name):
    m = mdns_records(d.data)
    return bool(m) and not m[1] & 0x8000 and any(q[0] == name and q[1] == 255 and q[2] & 0x8000 for q in m[2]) and \
        any(r[0] == 'ns' and r[1] == name and r[2] == 1 for r in m[3])


def mdns_nsec(d, name, section='an'):
    """d: a response whose section holds name's NSEC record (RFC 6762 section 6.1) saying A is its only type"""
    m = mdns_records(d.data) if d else None
    return bool(m) and any(s == section and n == name and t == 47 and c == 0x8001 and
                           r == mdns_name(name) + b'\x00\x01\x40' for s, n, t, c, ttl, r in m[3])


def mdns_probe_of(name, ip, qu=True):
    """another host's probe for name, its A record for ip in the authority section"""
    return struct.pack('!HHHHHH', 0, 0, 1, 0, 1, 0) + mdns_name(name) + struct.pack('!HH', 255, 0x8001 if qu else 1) + \
        mdns_name(name) + struct.pack('!HHIH', 1, 1, 120, 4) + socket.inet_aton(ip)


def sc_mdns(rig):
    """10/25/26: the multicast DNS responder (net/mdns.c, `mdns on`): three probes for surfos.local to the group with
    IP TTL 255, then two announcements; a query to the group (any case) is answered to the group with the
    cache-flush bit, a TTL of 120 s and an NSEC record (A alone); within the second after, a plain query gets
    nothing more to the group and another host's probe an answer; one with QU to the querier alone (IP TTL 255),
    one to our address too, one from off the link nothing; a one-shot query (port 40000) to it with its ID, the
    question as written and a TTL of 10 s; AAAA the NSEC record alone; another name nothing; mdns off a goodbye (TTL
    0). Then a conflict: an answer to a probe that gives surfos.local to another address makes it probe for
    surfos-2.local, and answer for it (the same answer from port 5354, or from off the link, changes nothing).
    mdns off: nothing more is answered. Then ties (RFC 6762 section 8.2): another host probing for surfos.local at
    the same moment with a later address wins, and the responder probes again a second later; with an earlier one
    it goes on. Last, a host that claims every name: after fifteen conflicts in a row the next probe waits 5 s
    (section 8.1). The queries go to the group's MAC, so the e1000's multicast filter has to take it
    (NETDEV_ADD_MULTICAST)"""
    ok = rig.run('mdns', 'mdns: not running')
    ok = rig.run('mdns on', 'mdns: answering for surfos.local') and ok
    probes = [rig.net.wait_udp(lambda d: d.dport == 5353 and d.mac == MDNS_MAC and mdns_is_probe(d, 'surfos.local'), 3)
              for _ in range(3)]
    ok = all(probes) and all(p.dst == MDNS_IP and p.ttl == 255 for p in probes) and ok
    ann = [rig.net.wait_udp(lambda d: d.dport == 5353 and d.dst == MDNS_IP and
                            mdns_a(d, 'surfos.local') == (120, True), 4) for _ in range(2)]
    ok = all(ann) and ok
    if all(probes) and all(ann):
        print('        (probes %.2f %.2f %.2f s, announcements %.2f %.2f s)' %
              tuple([p.t - probes[0].t for p in probes] + [a.t - probes[0].t for a in ann]))
        ok = 0.2 < probes[1].t - probes[0].t < 0.5 and 0.6 < ann[0].t - probes[0].t < 1.1 and \
            0.8 < ann[1].t - ann[0].t < 1.4 and ok
    rig.q.wait(1.1)                                 # its record goes to the group once a second at most
    rig.net.send_udp(5353, 5353, mdns_query('SurfOS.Local'), dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos.local'), 3)
    ok = d is not None and d.dst == MDNS_IP and d.mac == MDNS_MAC and mdns_a(d, 'surfos.local') == (120, True) and \
        mdns_nsec(d, 'surfos.local', 'ar') and ok
    # within that second: a plain query gets nothing to the group, another host's probe for the name an answer at
    # once (section 6: a probe is answered 250 ms after the last)
    rig.q.wait(0.3)
    rig.net.send_udp(5353, 5353, mdns_query('surfos.local'), dst=MDNS_IP, dst_mac=MDNS_MAC)
    ok = rig.net.wait_udp(lambda d: d.dport == 5353 and d.dst == MDNS_IP, 0.3) is None and ok
    rig.net.send_udp(5353, 5353, mdns_probe_of('surfos.local', '10.0.2.99', qu=False), src=socket.inet_aton('10.0.2.99'),
                     dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 5353 and d.dst == MDNS_IP and mdns_a(d, 'surfos.local'), 2)
    ok = d is not None and ok
    rig.net.send_udp(5353, 5353, mdns_query('surfos.local', qu=True), dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos.local'), 3)
    ok = d is not None and d.dst == rawnet.PEER_IP and d.ttl == 255 and ok
    rig.net.send_udp(5353, 5353, mdns_query('surfos.local'))                   # to our address (section 5.5)
    d = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos.local'), 3)
    ok = d is not None and d.dst == rawnet.PEER_IP and ok
    rig.net.send_udp(5353, 5353, mdns_query('surfos.local'), src=socket.inet_aton('192.168.77.5'))  # off the link
    ok = rig.net.wait_udp(lambda d: d.dport == 5353, 1.0) is None and ok
    rig.net.send_udp(40000, 5353, mdns_query('surfos.local', qid=0x4242), dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 40000, 3)
    m = mdns_records(d.data) if d else None
    ok = d is not None and d.dst == rawnet.PEER_IP and m and m[0] == 0x4242 and m[2] == [('surfos.local', 1, 1)] and \
        mdns_a(d, 'surfos.local') == (10, False) and d.ttl == 255 and not mdns_nsec(d, 'surfos.local', 'ar') and ok
    q = mdns_query('SurfOS.Local', qtype=255, qid=0x4343)                     # its question back as it was written
    rig.net.send_udp(40001, 5353, q, dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 40001, 3)
    ok = d is not None and d.data[12:len(q)] == q[12:] and mdns_a(d, 'surfos.local') == (10, False) and ok
    rig.net.send_udp(5353, 5353, mdns_query('surfos.local', qtype=28, qu=True), dst=MDNS_IP, dst_mac=MDNS_MAC)
    d = rig.net.wait_udp(lambda d: d.dport == 5353, 3)                         # AAAA: the NSEC record, no address
    ok = d is not None and mdns_nsec(d, 'surfos.local') and mdns_a(d, 'surfos.local') is None and ok
    rig.net.send_udp(5353, 5353, mdns_query('other.local', qu=True), dst=MDNS_IP, dst_mac=MDNS_MAC)
    ok = rig.net.wait_udp(lambda d: d.dport == 5353, 1.5) is None and ok
    ok = rig.run('mdns', 'mdns: answering for surfos.local', 'eth0 10.0.2.15') and ok
    ok = rig.run('mdns off', 'mdns: stopped') and ok
    d = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos.local') == (0, True), 2)     # goodbye
    ok = d is not None and d.dst == MDNS_IP and ok
    # a conflict while probing: another host answers a probe. The same claim from another port than 5353, or to our
    # address from off the link, is not looked at (sections 6 and 11)
    rig.net.dgrams.clear()
    ok = rig.run('mdns on', 'mdns: answering for surfos.local') and ok
    p = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_is_probe(d, 'surfos.local'), 3)
    claim = struct.pack('!HHHHHH', 0, 0x8400, 0, 1, 0, 0) + mdns_name('surfos.local') + \
        struct.pack('!HHIH', 1, 0x8001, 120, 4) + socket.inet_aton('10.0.2.99')
    rig.net.send_udp(5354, 5353, claim, dst=MDNS_IP, dst_mac=MDNS_MAC)
    rig.net.send_udp(5353, 5353, claim, src=socket.inet_aton('192.168.77.5'))
    p1 = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_is_probe(d, 'surfos.local'), 3)
    ok = p1 is not None and rig.net.wait_udp(lambda d: mdns_is_probe(d, 'surfos-2.local'), 0.05) is None and ok
    rig.net.send_udp(5353, 5353, claim, dst=MDNS_IP, dst_mac=MDNS_MAC)
    p2 = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_is_probe(d, 'surfos-2.local'), 3)
    a2 = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos-2.local') == (120, True), 5)
    ok = p is not None and p2 is not None and a2 is not None and ok
    ok = rig.run('mdns', 'mdns: answering for surfos-2.local') and ok
    ok = rig.run('dmesg', '+MDNS: another host answers for that name: probing for surfos-2.local instead') and ok
    ok = rig.run('mdns off', 'mdns: stopped') and ok
    rig.net.dgrams.clear()
    rig.net.send_udp(5353, 5353, mdns_query('surfos-2.local'), dst=MDNS_IP, dst_mac=MDNS_MAC)
    ok = rig.net.wait_udp(lambda d: d.dport == 5353, 1.5) is None and ok
    # ties: the other host's probe goes out as soon as the responder's first is seen (the command's output is read
    # after), so it lands while the responder probes. A later address wins: three probes again a second on, then the
    # announcements (that host never makes its own); an earlier one changes nothing
    for other, loses in (('10.0.2.99', True), ('10.0.2.3', False)):
        rig.net.dgrams.clear()
        rig.mark()
        rig.q.send('mdns on\n')
        p = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_is_probe(d, 'surfos.local'), 5)
        probe = struct.pack('!HHHHHH', 0, 0, 1, 0, 1, 0) + mdns_name('surfos.local') + struct.pack('!HH', 255, 0x8001) + \
            mdns_name('surfos.local') + struct.pack('!HHIH', 1, 1, 120, 4) + socket.inet_aton(other)
        rig.net.send_udp(5353, 5353, probe, src=socket.inet_aton(other), dst=MDNS_IP, dst_mac=MDNS_MAC)
        sent = rig.net.now()
        ok = rig.expect('mdns: answering for surfos.local') and rig.expect(rig.qr.PROMPT) and p is not None and ok
        later = [rig.net.wait_udp(lambda d: d.dport == 5353 and d.t > sent and mdns_is_probe(d, 'surfos.local'), 3)
                 for _ in range(3 if loses else 2)]
        a = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_a(d, 'surfos.local') == (120, True), 4)
        ok = all(later) and a is not None and a.t > later[-1].t and ok
        if all(later):
            print('        (a probe from %s: the next of ours %.2f s after it)' % (other, later[0].t - sent))
            ok = (0.9 < later[0].t - sent < 1.6 if loses else later[0].t - sent < 0.5) and ok
        if loses:
            ok = rig.run('dmesg', '+MDNS: eth0: another host probes for surfos.local too and wins the tie') and ok
        ok = rig.run('mdns off', 'mdns: stopped') and ok
    # a host that claims every name probed for: the probes for surfos-2 to surfos-15 go out at once, the one after the
    # fifteenth conflict in a row 5 s later (section 8.1)
    rig.net.dgrams.clear()
    ok = rig.run('mdns on', 'mdns: answering for surfos.local') and ok
    gaps, name, claimed = [], 'surfos.local', 0
    for k in range(16):
        p = rig.net.wait_udp(lambda d: d.dport == 5353 and mdns_is_probe(d, name), 7)
        if p is None:
            break
        if k:
            gaps.append(p.t - claimed)
        if k == 15:
            break
        claim = struct.pack('!HHHHHH', 0, 0x8400, 0, 1, 0, 0) + mdns_name(name) + \
            struct.pack('!HHIH', 1, 0x8001, 120, 4) + socket.inet_aton('10.0.2.99')
        rig.net.send_udp(5353, 5353, claim, dst=MDNS_IP, dst_mac=MDNS_MAC)
        claimed = rig.net.now()
        name = 'surfos-%d.local' % (k + 2)
    ok = len(gaps) == 15 and max(gaps[:14]) < 1 and 4.5 < gaps[14] < 6.5 and ok
    if len(gaps) == 15:
        print('        (a claim on every name: the next probe %.2f s after it at most, after the fifteenth %.2f s)' %
              (max(gaps[:14]), gaps[14]))
    ok = rig.run('dmesg', '+MDNS: 15 conflicts in a row: each probe from now on waits 5 s') and ok
    ok = rig.run('mdns off', 'mdns: stopped') and ok
    return ok


SCENARIOS = [
    ('scan', sc_scan, 'a SYN scan does not make a listener deaf'),
    ('flood', sc_flood, 'SYNs never acknowledged give their place to a real client'),
    ('reset', sc_reset, 'a connection reset before accept(): accepted, read() says ECONNRESET; then it is none'),
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
    ('backlog', sc_backlog, 'a listener that asked for a backlog of 8 keeps 8 whole connections waiting'),
    ('ackahead', sc_ackahead, 'a bare ACK ahead of the guest counts and gets no answer'),
    ('orphans', sc_orphans, 'a closed connection still finishing holds no buffers'),
    ('closing', sc_closing, "the ACK on a FIN sent again ends CLOSING at once"),
    ('shrink', sc_shrink, 'a window shrunk under bytes in flight is probed, not flooded, and then served'),
    ('pollscan', sc_pollscan, 'a SYN scan against a poll() server (when poll() is there)'),
    ('pollstall', sc_pollstall, 'a client that stops reading holds up no other (when poll() is there)'),
    ('halfclose', sc_halfclose, 'a half-closed connection reads on after its FIN, until the other end closes'),
    ('nowait', sc_nowait, 'a connection not waited for is polled for: made late, or refused'),
    ('finwait', sc_finwait, 'a half-closed connection whose other end vanished is given up by keepalive'),
    ('ncstuck', sc_ncstuck, 'nc -w ends a connection whose other end half-closed and takes nothing'),
    ('mdns', sc_mdns, 'the multicast DNS responder: probes, announcements, answers, conflicts, off, ties'),
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
