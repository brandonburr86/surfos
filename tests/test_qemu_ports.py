"""Tests for the QEMU launches of the test harnesses (10/18/26, round fs-fix-11): a forwarded port that another
program takes between free_port() and QEMU's bind.

Run from the repository: python3 -m unittest discover -s tests -p test_qemu_ports.py   (part of make unittest)

tools/qemu-run.py's Qemu and tools/qemu-usb.py's Vm give QEMU ports picked by free_port(), which binds port 0, lets it
go and returns its number; QEMU binds it a moment later and stops at once if another program has taken it meanwhile
(test-all once stopped so: "Could not set up host forwarding rule 'tcp:127.0.0.1:39771-10.0.2.16:23'"). Here a port is
held for the whole test and free_port() returns it first. A stand-in for QEMU (FAKE below: it binds the forwarded and
datagram ports as QEMU does, and serves the monitor and serial sockets) stops with QEMU's message when it cannot bind
one, so the launch must start it again with fresh ports, say so, and give up after PORT_TRIES; any other failure must
stay fatal at the first try, and (10/19/26, audit B78) so must a port the caller wrote itself, also in a launch with
picked ports. Where qemu-system-i386 is installed, its own messages are checked as well.
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import warnings

REPO = Path(__file__).resolve().parents[1]

# the stand-in: every launch is one line in $FAKE_QEMU_LOG; $FAKE_QEMU_FAIL makes it stop with that message instead
FAKE = r'''
import os, re, socket, sys, threading, time
args = sys.argv[1:]
with open(os.environ['FAKE_QEMU_LOG'], 'a') as f:
    f.write(' '.join(args) + '\n')
if os.environ.get('FAKE_QEMU_FAIL'):
    print('qemu-system-i386: ' + os.environ['FAKE_QEMU_FAIL'], flush=True)
    sys.exit(1)
held = []
for i, a in enumerate(args):
    if i == 0 or args[i - 1] != '-netdev':
        continue
    for rule, port in re.findall(r'hostfwd=(tcp:127\.0\.0\.1:(\d+)-[^,]*)', a):
        s = socket.socket()
        try:
            s.bind(('127.0.0.1', int(port)))
            s.listen(4)
        except OSError:
            print("qemu-system-i386: -netdev %s: Could not set up host forwarding rule '%s'" % (a, rule), flush=True)
            sys.exit(1)
        held.append(s)
    for port in re.findall(r'local\.port=(\d+)', a):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(('127.0.0.1', int(port)))
        except OSError as e:
            print("qemu-system-i386: -netdev %s: can't bind ip=127.0.0.1 to socket: %s" % (a, os.strerror(e.errno)),
                  flush=True)
            sys.exit(1)
        held.append(s)

def serve(path, monitor):
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)
    def talk(c):
        if not monitor:
            return
        c.sendall(b"QEMU (stand-in) monitor - type 'help' for more information\r\n(qemu) ")
        buf = b''
        while True:
            d = c.recv(4096)
            if not d:
                return
            buf += d
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                if line.strip() == b'quit':
                    os._exit(0)
                c.sendall(b'\r\n(qemu) ')
    while True:
        c, _ = srv.accept()
        held.append(c)
        threading.Thread(target=talk, args=(c,), daemon=True).start()

for i, a in enumerate(args[:-1]):
    if a == '-monitor':
        path = args[i + 1].split(':', 1)[1].split(',')[0]
        threading.Thread(target=serve, args=(path, True), daemon=True).start()
    if a == '-chardev':
        path = re.search(r'path=([^,]+)', args[i + 1]).group(1)
        threading.Thread(target=serve, args=(path, False), daemon=True).start()
time.sleep(60)                                  # never left behind for long
'''


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, REPO / 'tools' / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def hold(kind=socket.SOCK_STREAM):
    """a port on 127.0.0.1 that this test keeps (listening, for TCP): the other program"""
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    if kind == socket.SOCK_STREAM:
        s.listen(4)
    return s


class Ports(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qr = load('qemu_run_ports', 'qemu-run.py')

    def setUp(self):
        warnings.simplefilter('ignore', ResourceWarning)    # the harnesses leave their sockets to the process's end
        self.tmp = tempfile.mkdtemp(prefix='qports-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        script = os.path.join(self.tmp, 'fake_qemu.py')
        with open(script, 'w') as f:
            f.write(FAKE)
        self.qemu = os.path.join(self.tmp, 'qemu-system-i386')
        with open(self.qemu, 'w') as f:            # a wrapper: an interpreter path with spaces cannot be a #! line
            f.write('#!/bin/sh\nexec "%s" "%s" "$@"\n' % (sys.executable, script))
        os.chmod(self.qemu, 0o755)
        self.kernel = os.path.join(self.tmp, 'kernel.bin')
        Path(self.kernel).write_bytes(b'\0' * 16)
        self.log = os.path.join(self.tmp, 'launches')
        env = mock.patch.dict(os.environ, {'FAKE_QEMU_LOG': self.log})
        env.start()
        self.addCleanup(env.stop)
        real_mkdtemp = tempfile.mkdtemp

        def mkdtemp(*a, **kw):                      # the harnesses' work directories, removed after each test
            d = real_mkdtemp(*a, **kw)
            self.addCleanup(shutil.rmtree, d, True)
            return d
        p = mock.patch('tempfile.mkdtemp', mkdtemp)
        p.start()
        self.addCleanup(p.stop)
        self.held = []

    def tearDown(self):
        for s in self.held:
            s.close()

    def launches(self):
        return Path(self.log).read_text().splitlines() if os.path.exists(self.log) else []

    def taken(self, qr, at, kind=socket.SOCK_STREAM, times=1):
        """free_port() returns a held port at its calls number at, ..., at + times - 1 (from 1): taken meanwhile"""
        s = hold(kind)
        self.held.append(s)
        port, real, calls = s.getsockname()[1], qr.free_port, []

        def free_port():
            calls.append(1)
            return port if at <= len(calls) < at + times else real()
        p = mock.patch.object(qr, 'free_port', free_port)
        p.start()
        self.addCleanup(p.stop)
        return port

    def start(self, *args, **kw):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            q = self.qr.Qemu(self.kernel, self.qemu, *args, **kw)
        self.addCleanup(q.close)
        return q, out.getvalue()

    # -- tools/qemu-run.py ------------------------------------------------------------------------------------------
    def test_taken_port_is_picked_again(self):
        """the flake of test-all's O0 smoke: telnet_wlan's port (free_port()'s fourth) taken before QEMU binds it"""
        port = self.taken(self.qr, 4)
        q, log = self.start('64')
        self.assertTrue(q.alive())
        self.assertEqual(len(self.launches()), 2)
        self.assertIn("Could not set up host forwarding rule 'tcp:127.0.0.1:%d-10.0.2.16:23'" % port, log)
        self.assertIn('starting it again with fresh ports (try 2 of %d)' % self.qr.PORT_TRIES, log)
        self.assertEqual(log.count('starting it again'), 1)
        names = ('echo', 'telnet', 'rx', 'telnet_wlan')
        self.assertNotEqual(q.ports['telnet_wlan'], port)
        self.assertEqual(len({q.ports[n] for n in names}), 4)
        for n in names:                             # the ports QEMU was given are the ones the tests will use
            self.assertIn('hostfwd=tcp:127.0.0.1:%d-' % q.ports[n], ' '.join(q.cmd))
        self.assertEqual(q.ports['http'], q.http.server_address[1])
        self.assertIn('httpport=%d' % q.ports['http'], ' '.join(q.cmd))

    def test_taken_every_time_gives_up(self):
        """bounded: PORT_TRIES launches, then QEMU's failure as before"""
        self.taken(self.qr, 1, times=1000)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(RuntimeError) as e:
            self.qr.Qemu(self.kernel, self.qemu, '64')
        self.assertEqual(len(self.launches()), self.qr.PORT_TRIES)
        self.assertEqual(out.getvalue().count('starting it again with fresh ports'), self.qr.PORT_TRIES - 1)
        self.assertIn('Could not set up host forwarding rule', str(e.exception))

    def test_other_failure_is_fatal(self):
        """a failure that is not a taken port: no second launch"""
        with mock.patch.dict(os.environ, {'FAKE_QEMU_FAIL': 'could not load kernel'}):
            out = io.StringIO()
            with contextlib.redirect_stdout(out), self.assertRaises(RuntimeError) as e:
                self.qr.Qemu(self.kernel, self.qemu, '64')
        self.assertEqual(len(self.launches()), 1)
        self.assertNotIn('starting it again', out.getvalue())
        self.assertIn('could not load kernel', str(e.exception))

    def test_fixed_port_is_not_retried(self):
        """a port the caller wrote itself is not this harness's to change: its failure stays fatal, alone and (10/19/26,
        audit B78) in a launch that has picked ports too (net=True: four of them), where it was tried again 4 times"""
        for net in (False, True):
            s = hold()
            self.held.append(s)
            extra = ['-netdev', 'user,id=n9,hostfwd=tcp:127.0.0.1:%d-:23' % s.getsockname()[1]]
            before = len(self.launches())
            out = io.StringIO()
            with contextlib.redirect_stdout(out), self.assertRaises(RuntimeError) as e:
                self.qr.Qemu(self.kernel, self.qemu, '64', extra, net=net)
            self.assertEqual(len(self.launches()) - before, 1, 'net=%s' % net)
            self.assertNotIn('starting it again', out.getvalue())
            self.assertIn("forwarding rule 'tcp:127.0.0.1:%d-" % s.getsockname()[1], str(e.exception))

    def test_taken_ports_read_from_qemu(self):
        """10/19/26 (audit B78): the port a failure names, QEMU's words for both kinds, and whether it was a picked one"""
        tcp = ("qemu-system-i386: -netdev user,id=net0,hostfwd=tcp:127.0.0.1:4001-:7,hostfwd=tcp:127.0.0.1:4002-:23: "
               "Could not set up host forwarding rule 'tcp:127.0.0.1:4002-:23'")
        udp = ("qemu-system-i386: -netdev dgram,id=d0,local.type=inet,local.host=127.0.0.1,local.port=4003,remote.type=inet,"
               "remote.host=127.0.0.1,remote.port=9: can't bind ip=127.0.0.1 to socket: Address already in use")
        self.assertEqual(self.qr.taken_ports(tcp), [4002])
        self.assertEqual(self.qr.taken_ports(udp), [4003])
        self.assertTrue(self.qr.picked_port_taken(tcp, {4001, 4002}))
        self.assertFalse(self.qr.picked_port_taken(tcp, {4001}))
        self.assertTrue(self.qr.picked_port_taken(udp, {4003}))
        self.assertFalse(self.qr.picked_port_taken(udp, set()))
        self.assertFalse(self.qr.picked_port_taken('qemu-system-i386: could not load kernel', {4001}))

    def test_datagram_port(self):
        """tools/rawnet.py's link (tools/qemu-tcp.py, the @tcp-raw steps): QEMU's own UDP port, taken meanwhile"""
        rawnet = load('rawnet_ports', 'rawnet.py')
        net = rawnet.Net()
        self.addCleanup(net.close)
        port = self.taken(self.qr, 1, socket.SOCK_DGRAM)
        q, log = self.start('64', net.qemu_args(), net=False)
        self.assertEqual(len(self.launches()), 2)
        self.assertIn('Address already in use', log)
        self.assertNotEqual(q.ports['dgram'], port)
        net.set_qport(q.ports['dgram'])
        self.assertEqual(net.dst, ('127.0.0.1', q.ports['dgram']))
        self.assertIn('local.port=%d,' % q.ports['dgram'], ' '.join(q.cmd))

    def test_dns_peer_port(self):
        """the DNS peer's link (@dns-bounded) the same way"""
        peer = self.qr.DnsPeer()
        self.addCleanup(peer.close)
        port = self.taken(self.qr, 1, socket.SOCK_DGRAM)
        q, log = self.start('64', peer.args(), net=False)
        self.assertEqual(len(self.launches()), 2)
        self.assertNotEqual(q.ports['dns'], port)
        self.assertIn('local.port=%d,' % q.ports['dns'], ' '.join(q.cmd))

    # -- tools/qemu-usb.py ------------------------------------------------------------------------------------------
    def test_usb_vm(self):
        """tools/qemu-usb.py's machine (test-usb's usbnet and devloop scenarios): the same retry"""
        usb = load('qemu_usb_ports', 'qemu-usb.py')
        image, code, vars_ = (os.path.join(self.tmp, n) for n in ('usb.img', 'code.fd', 'vars.fd'))
        for p in (image, code, vars_):
            Path(p).write_bytes(b'\0' * 512)
        port = self.taken(self.qr, 2)
        args = types.SimpleNamespace(qemu=self.qemu, extra='')
        out = io.StringIO()
        with mock.patch.object(usb, 'load_smoke_harness', return_value=self.qr), contextlib.redirect_stdout(out):
            vm = usb.Vm('ports', args, image, (code, vars_), serial=True,
                        extra=['-netdev', 'user,id=n0,hostfwd=tcp:127.0.0.1:{port:telnet}-:23,'
                                          'hostfwd=tcp:127.0.0.1:{port:rx}-:{port:rx}'])
        self.addCleanup(vm.close)
        self.assertEqual(len(self.launches()), 2)
        self.assertIn('starting it again with fresh ports (try 2 of', out.getvalue())
        self.assertNotEqual(vm.ports['rx'], port)
        self.assertIn('hostfwd=tcp:127.0.0.1:%d-:%d' % (vm.ports['rx'], vm.ports['rx']), ' '.join(vm.cmd))
        with mock.patch.dict(os.environ, {'FAKE_QEMU_FAIL': 'no firmware'}), self.assertRaises(RuntimeError):
            usb.Vm('fatal', args, image, (code, vars_), serial=True,
                   extra=['-netdev', 'user,id=n0,hostfwd=tcp:127.0.0.1:{port:telnet}-:23'])
        self.assertEqual(len(self.launches()), 3)
        fixed = hold()                              # (10/19/26, audit B78) a port the caller wrote, beside a picked one
        self.held.append(fixed)
        with mock.patch.object(usb, 'load_smoke_harness', return_value=self.qr), contextlib.redirect_stdout(out), \
                self.assertRaises(RuntimeError):
            usb.Vm('fixed', args, image, (code, vars_), serial=True,
                   extra=['-netdev', 'user,id=n0,hostfwd=tcp:127.0.0.1:{port:telnet}-:23,'
                                     'hostfwd=tcp:127.0.0.1:%d-:24' % fixed.getsockname()[1]])
        self.assertEqual(len(self.launches()), 4)
        self.assertEqual(out.getvalue().count('starting it again'), 1)    # (the first Vm's retry only)

    # -- QEMU itself --------------------------------------------------------------------------------------------------
    @unittest.skipUnless(shutil.which('qemu-system-i386'), 'qemu-system-i386 is not installed')
    def test_qemu_says_so(self):
        """QEMU's own words for a taken port are among the ones the harness looks for (PORT_TAKEN)"""
        tcp, udp = hold(), hold(socket.SOCK_DGRAM)
        self.held += [tcp, udp]
        for netdev, port in (('user,id=n0,hostfwd=tcp:127.0.0.1:%d-:23' % tcp.getsockname()[1], tcp.getsockname()[1]),
                             ('dgram,id=d0,local.type=inet,local.host=127.0.0.1,local.port=%d,remote.type=inet,'
                              'remote.host=127.0.0.1,remote.port=9' % udp.getsockname()[1], udp.getsockname()[1])):
            try:
                r = subprocess.run(['qemu-system-i386', '-machine', 'none', '-nodefaults', '-display', 'none', '-S',
                                    '-netdev', netdev], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=30)
            except subprocess.TimeoutExpired:
                self.fail('QEMU started although its port was taken: ' + netdev)
            text = r.stdout.decode(errors='replace')
            if 'not compiled into this binary' in text or 'expects a netdev backend type' in text:
                continue                            # a QEMU without this backend
            self.assertNotEqual(r.returncode, 0, text)
            self.assertTrue(self.qr.port_taken(text), text)
            self.assertEqual(self.qr.taken_ports(text), [port], text)     # (10/19/26, B78: the port read from it)


if __name__ == '__main__':
    unittest.main()
