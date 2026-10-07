"""
SurfOS test peer: a raw Ethernet link to the guest
--------------------
File: rawnet.py   Date: 10/6/26
--------------------
For tests that need TCP segments made by hand (tools/qemu-tcp.py). QEMU's guest NIC
(e1000) is joined to this program by a "dgram" netdev: every Ethernet frame the guest
sends arrives here as a UDP datagram, and every datagram sent back is a frame for the
guest. This side plays the rest of the network, 10.0.2.2 and any other address on
10.0.2.0/24: it answers ARP (except for the addresses in NO_ARP), records the guest's
TCP segments, and sends whatever segments a test makes, right or wrong. A test is then
the guest's other end, one segment at a time (Conn), or leaves a port to a small
automatic server (Server) when the guest is the one that connects.

Grown from the experiments of the review of track N into a test tool.
"""

import random
import socket
import struct
import threading
import time

GUEST_MAC = bytes.fromhex('525400123456')
PEER_MAC = bytes.fromhex('52550a000202')
GUEST_IP = socket.inet_aton('10.0.2.15')
PEER_IP = socket.inet_aton('10.0.2.2')
NO_ARP = {socket.inet_aton('10.0.2.99')}        # an address nobody answers ARP for
FIN, SYN, RST, PSH, ACK = 1, 2, 4, 8, 16


def csum(data):
    if len(data) % 2:
        data += b'\0'
    s = sum(struct.unpack('!%dH' % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def flagstr(f):
    return ''.join(n for b, n in ((SYN, 'S'), (ACK, 'A'), (FIN, 'F'), (RST, 'R'), (PSH, 'P')) if f & b) or '.'


def seq_lt(a, b):
    return ((a - b) & 0xffffffff) >= 0x80000000


class Seg:
    """a TCP segment the guest sent"""
    def __init__(self, t, src, dst, sport, dport, seq, ack, flags, win, data, opts):
        self.t, self.src, self.dst, self.sport, self.dport = t, src, dst, sport, dport
        self.seq, self.ack, self.flags, self.win, self.data, self.opts = seq, ack, flags, win, data, opts

    def __repr__(self):
        return '%.2f %d->%d %s seq=%u ack=%u win=%u len=%d' % (self.t, self.sport, self.dport, flagstr(self.flags),
                                                                self.seq, self.ack, self.win, len(self.data))


class Net:
    """the link: ARP answered, the guest's segments queued for the test (or handed to a Server)"""
    def __init__(self, verbose=False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('127.0.0.1', 0))
        self.port = self.sock.getsockname()[1]
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2.bind(('127.0.0.1', 0))
        self.qport = s2.getsockname()[1]           # QEMU's end: free when QEMU binds it (a small race, as free_port())
        s2.close()
        self.dst = ('127.0.0.1', self.qport)
        self.segs = []
        self.servers = {}                           # peer port -> Server
        self.cv = threading.Condition()
        self.lock = threading.Lock()                # one sender at a time (the test and the servers)
        self.verbose = verbose
        self.t0 = time.time()
        self.ipid = 1
        self.running = True
        self.th = threading.Thread(target=self._rx, daemon=True)
        self.th.start()

    def qemu_args(self):
        return ['-netdev', f'dgram,id=d0,local.type=inet,local.host=127.0.0.1,local.port={self.qport},'
                           f'remote.type=inet,remote.host=127.0.0.1,remote.port={self.port}',
                '-device', 'e1000,netdev=d0,mac=52:54:00:12:34:56']

    def now(self):
        return time.time() - self.t0

    def _rx(self):
        self.sock.settimeout(0.2)
        while self.running:
            try:
                f, _ = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if len(f) < 14:
                continue
            et = struct.unpack('!H', f[12:14])[0]
            if et == 0x0806 and len(f) >= 42:
                op = struct.unpack('!H', f[20:22])[0]
                spa, tpa = f[28:32], f[38:42]
                if op == 1 and tpa != GUEST_IP and tpa not in NO_ARP:
                    rep = (f[6:12] + PEER_MAC + b'\x08\x06' + struct.pack('!HHBBH', 1, 0x0800, 6, 4, 2) +
                           PEER_MAC + tpa + f[22:28] + spa)
                    with self.lock:
                        self.sock.sendto(rep, self.dst)
                continue
            if et != 0x0800:
                continue
            ip = f[14:]
            ihl = (ip[0] & 15) * 4
            tot = struct.unpack('!H', ip[2:4])[0]
            if ip[9] != 6:
                continue
            tcp = ip[ihl:tot]
            sport, dport, seq, ack, off, flags, win = struct.unpack('!HHIIBBH', tcp[:16])
            hl = (off >> 4) * 4
            seg = Seg(self.now(), ip[12:16], ip[16:20], sport, dport, seq, ack, flags, win, tcp[hl:], tcp[20:hl])
            if self.verbose:
                print('    <- guest', seg, flush=True)
            server = self.servers.get(dport)
            if server:
                server.handle(seg)
                continue
            with self.cv:
                self.segs.append(seg)
                self.cv.notify_all()

    def send(self, sport, dport, seq, ack, flags, win=8192, data=b'', src=PEER_IP, mss=None):
        opts = struct.pack('!BBH', 2, 4, mss) if mss else b''
        off = (20 + len(opts)) // 4
        tcp = struct.pack('!HHIIBBHHH', sport, dport, seq & 0xffffffff, ack & 0xffffffff, off << 4, flags, win, 0, 0) + opts + data
        pseudo = src + GUEST_IP + struct.pack('!BBH', 0, 6, len(tcp))
        tcp = tcp[:16] + struct.pack('!H', csum(pseudo + tcp)) + tcp[18:]
        with self.lock:
            ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(tcp), self.ipid, 0x4000, 64, 6, 0, src, GUEST_IP)
            self.ipid += 1
            ip = ip[:10] + struct.pack('!H', csum(ip)) + ip[12:]
            if self.verbose:
                print('    -> guest %.2f %d->%d %s seq=%u ack=%u win=%u len=%d' % (self.now(), sport, dport, flagstr(flags),
                      seq & 0xffffffff, ack & 0xffffffff, win, len(data)), flush=True)
            self.sock.sendto(GUEST_MAC + PEER_MAC + b'\x08\x00' + ip + tcp, self.dst)

    def wait(self, pred, timeout=5.0):
        """the first queued segment matching pred (taken from the queue), or None"""
        deadline = time.time() + timeout
        with self.cv:
            while True:
                for i, s in enumerate(self.segs):
                    if pred(s):
                        del self.segs[i]
                        return s
                left = deadline - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def drain(self, pred=None):
        with self.cv:
            out = [s for s in self.segs if not pred or pred(s)]
            self.segs = [s for s in self.segs if pred and not pred(s)]
        return out

    def close(self):
        self.running = False
        self.sock.close()


class Conn:
    """this side of one connection to a guest port (peer port pp <-> guest port gp), segment by segment"""
    def __init__(self, net, pp, gp, iss=1000, win=8192):
        self.net, self.pp, self.gp, self.snd, self.rcv, self.win = net, pp, gp, iss, 0, win
        self.fin = None                             # where the guest's FIN is, once it came (receive())

    def mine(self, s):
        return s.sport == self.gp and s.dport == self.pp

    def syn(self, timeout=3.0, mss=1460):
        """a SYN; the guest's SYN-ACK, or None"""
        self.net.send(self.pp, self.gp, self.snd, 0, SYN, self.win, mss=mss)
        sa = self.net.wait(lambda s: self.mine(s) and s.flags & SYN, timeout)
        if sa:
            self.rcv = (sa.seq + 1) & 0xffffffff
            self.snd += 1
        return sa

    def ack(self, win=None, flags=ACK, data=b''):
        self.net.send(self.pp, self.gp, self.snd, self.rcv, flags, self.win if win is None else win, data)
        self.snd += len(data) + (1 if flags & (SYN | FIN) else 0)

    def connect(self, timeout=3.0):
        sa = self.syn(timeout)
        if sa:
            self.ack()
        return sa

    def rst(self):
        self.net.send(self.pp, self.gp, self.snd, self.rcv, RST | ACK, 0)

    def is_probe(self, s):
        """a probe of the guest's: an ACK with no data and an old sequence number (keepalive, or of a shut window)"""
        return self.mine(s) and not s.data and not s.flags & (SYN | FIN | RST) and seq_lt(s.seq, self.rcv)

    def receive(self, want, win_for=None, timeout=10.0, answer_probes=True):
        """the guest's data, acknowledged as it comes, until want bytes (or the time is up); win_for(got) is the
        window to advertise after got bytes. Probes are answered with that window. The bytes, in order."""
        got = b''
        deadline = time.time() + timeout
        while len(got) < want and time.time() < deadline:
            s = self.net.wait(lambda s: self.mine(s) and (s.data or (s.flags & FIN) or self.is_probe(s)), deadline - time.time())
            if not s:
                break
            if s.data and s.seq == self.rcv:
                got += s.data
                self.rcv = (self.rcv + len(s.data)) & 0xffffffff
            elif not s.data and not answer_probes:
                continue
            if s.flags & FIN and (s.seq + len(s.data)) & 0xffffffff == self.rcv:
                self.fin = self.rcv                 # the guest is done: Rig.finish() answers it
                break
            self.ack(win=win_for(len(got)) if win_for else None)
        return got


class Server:
    """the guest's other end on a port of this side, by itself (the guest connects): it answers the SYN,
    acknowledges what the guest sends and answers its FIN with its own. greeting is sent at once after the
    SYN-ACK, followed by a FIN when close is set, or a reset when reset is set (the server that greets and
    closes before the guest has even acknowledged the SYN-ACK). reply(data) gives what to send back for data the
    guest sent (then a FIN when close is set): a tiny HTTP server, say."""
    def __init__(self, net, port, greeting=b'', close=False, reset=False, reply=None):
        self.net, self.port, self.greeting, self.close, self.reset, self.reply = net, port, greeting, close, reset, reply
        self.conns = {}
        self.done = 0                               # connections that ended both ways
        net.servers[port] = self

    def _send(self, c, flags, data=b''):
        self.net.send(self.port, c['gp'], c['snd'], c['rcv'], flags, 8192, data)
        c['snd'] = (c['snd'] + len(data) + (1 if flags & (SYN | FIN) else 0)) & 0xffffffff

    def handle(self, s):
        if s.flags & RST:
            self.conns.pop(s.sport, None)
            return
        if s.flags & SYN:
            c = {'gp': s.sport, 'snd': random.randrange(1 << 32), 'rcv': (s.seq + 1) & 0xffffffff, 'fin': False}
            self.conns[s.sport] = c
            self._send(c, SYN | ACK)
            if self.greeting:
                self._send(c, ACK | PSH, self.greeting)
            if self.reset:
                self.net.send(self.port, c['gp'], c['snd'], c['rcv'], RST | ACK, 0)
                del self.conns[s.sport]
            elif self.close and not self.reply:
                self._send(c, ACK | FIN)
                c['fin'] = True
            return
        c = self.conns.get(s.sport)
        if not c:
            return
        if s.data and s.seq == c['rcv']:
            c['rcv'] = (c['rcv'] + len(s.data)) & 0xffffffff
            answer = self.reply(s.data) if self.reply else b''
            self._send(c, ACK | (PSH if answer else 0), answer or b'')
            if answer and self.close and not c['fin']:
                self._send(c, ACK | FIN)
                c['fin'] = True
        if s.flags & FIN:
            c['rcv'] = (c['rcv'] + 1) & 0xffffffff
            if c['fin']:
                self._send(c, ACK)
            else:
                self._send(c, ACK | FIN)
                c['fin'] = True
            self.done += 1
            del self.conns[s.sport]
        elif s.data or not s.flags & ACK:
            pass
        elif seq_lt(s.seq, c['rcv']):               # a probe: say where we are
            self._send(c, ACK)
