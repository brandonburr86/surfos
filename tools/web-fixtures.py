#!/usr/bin/env python3
"""Web fixtures for Undertow, SurfOS's text web browser (10/11/26): HTTP and HTTPS servers with fixed pages.

    tools/web-fixtures.py serve [--dir DIR]   every server until Ctrl+C: their ports, the test CA, undertow.conf lines
    tools/web-fixtures.py certs DIR           only the test CA and the certificates, in DIR
    tools/web-fixtures.py routes              what each path sends
    tools/web-fixtures.py servers             what each server is for

The servers listen on 127.0.0.1 at ports the system picks; a SurfOS guest under QEMU's user network reaches
them at 10.0.2.2 (QEMU turns that address into this computer's 127.0.0.1), by the names fixture.test and
www.fixture.test when its /initrd/etc/undertow.conf maps them there (host=fixture.test 10.0.2.2), which is what
tools/qemu-web.py puts in its test initrd. Nothing here uses DNS, the Internet or a fixed port.

Each run makes its own test CA with the openssl command (OpenSSL 3.0 or later: the Mac's, or the toolchain
image's): two roots (RSA 2048 and ECDSA P-256, each with an intermediate) that the test image trusts, and a
third root that it does not. Every subject says "Undertow Fixture", so a bundle can be checked for them
(MARKER). The leaves: a good one for fixture.test, www.fixture.test and the IP address 10.0.2.2 (RSA and
ECDSA), one for another name, one that expired 30 days ago, one valid only from 30 days on, one from the
unknown root, and the good RSA leaf sent without its intermediate. Dates are relative to the start of the run,
so the tests never depend on the day; the CAs are valid until 20 years on (past 2038). The servers that use
them: TLS 1.2 and 1.3, TLS 1.2 only, TLS 1.3 only, one per bad certificate, and three that never complete a
handshake (garbage instead of a ServerHello, a hangup, silence). Every connection is recorded: the SNI, the
TLS version and ALPN protocol, a failed handshake's reason, the request line, the Host header and the other
headers, how many bytes went back, and when the client closed.

Every server (plain or TLS) serves the same paths (ROUTES): a few readable pages with links between them,
then one path for each thing the HTTP client and the renderer must handle: Content-Length, chunked with
extensions and trailers, close-delimited bodies, interim responses, 101, 204 and 304, early EOF, malformed
and ambiguous framing, 1 MiB and 1 MiB + 1 bytes, redirects (chains of any length, a loop, relative, every
code, https to http), a stall, a byte every 10 seconds, image/png, gzip, charsets (UTF-8, Latin-1,
windows-1252, Shift_JIS), 256 and 257 links, anchors, escape sequences in the title and the text, error
pages. The slow paths take their timings as query parameters (?interval=0.2) so that the host tests are quick.

Use from Python:

    fx = Fixtures()                 # or: with Fixtures() as fx:
    fx.ports['https']               # each server's port; fx.url('tls13', '/long') for an address
    fx.ca_pem                       # the roots a test image trusts (PEM bytes)
    fx.records(server='https')      # what came in; fx.wait_for(lambda r: ..., timeout) to wait for one
    fx.close()                      # every socket and thread ends, also after an exception (the with block)
"""
import argparse
import datetime
import gzip
import html
import json
import os
from pathlib import Path
import select
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import zlib

FIXTURE_HOST = 'fixture.test'
WWW_HOST = 'www.fixture.test'
WRONG_HOST = 'wrong-name.test'
GUEST_HOST = '10.0.2.2'                 # this computer, as a guest on QEMU's user network sees it
MARKER = 'Undertow Fixture'             # in every subject of the test CA: never in a production bundle
BODY_MAX = 1024 * 1024                  # Undertow's UT_BODY_MAX (user/undertow/undertow.h)
LINKS_MAX = 2048                        # UT_LINKS_MAX
LINK_BYTES_MAX = 1024 * 1024            # UT_LINK_BYTES_MAX: one page's link labels, addresses and anchor names
HEADERS_MAX = 32 * 1024                 # UT_HEADERS_MAX
INTERIM_MAX = 8                         # UT_INTERIM_MAX

# ---- the test CA -------------------------------------------------------------------------------------------------

OPENSSL_CNF = """\
# tools/web-fixtures.py: the configuration of one run's test CA (made again for every run)
[ req ]
distinguished_name = req_dn
prompt = no
[ req_dn ]
CN = unused

[ ca ]
default_ca = fixture_ca
[ fixture_ca ]
dir = .
database = $dir/index.txt
new_certs_dir = $dir/issued
rand_serial = yes
default_md = sha256
policy = policy_any
unique_subject = no
copy_extensions = none
email_in_dn = no
preserve = yes
[ policy_any ]
organizationName = optional
commonName = supplied

[ root ]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[ intermediate ]
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[ leaf_rsa ]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
subjectAltName = @fixture_names
[ leaf_ecdsa ]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
subjectAltName = @fixture_names
[ leaf_wrong ]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
subjectAltName = DNS:%(wrong)s
[ fixture_names ]
DNS.1 = %(host)s
DNS.2 = %(www)s
IP.1 = %(ip)s
""" % {'host': FIXTURE_HOST, 'www': WWW_HOST, 'wrong': WRONG_HOST, 'ip': GUEST_HOST}

DAY = datetime.timedelta(days=1)

# the certificates of one run: (name, key, subject CN, issuer (None: self-signed), extensions, from, until), the
# dates relative to the start of the run. The good leaves start an hour back: a guest whose clock is off by
# hours (an RTC offset that was not applied) sees them as not yet valid.
CERTS = [
    ('rsa-root', 'rsa-root', MARKER + ' RSA Root CA', None, 'root', -365 * DAY, 20 * 365 * DAY),
    ('rsa-ca', 'rsa-ca', MARKER + ' RSA Intermediate CA', 'rsa-root', 'intermediate', -365 * DAY, 5 * 365 * DAY),
    ('ecdsa-root', 'ecdsa-root', MARKER + ' ECDSA Root CA', None, 'root', -365 * DAY, 20 * 365 * DAY),
    ('ecdsa-ca', 'ecdsa-ca', MARKER + ' ECDSA Intermediate CA', 'ecdsa-root', 'intermediate', -365 * DAY, 5 * 365 * DAY),
    ('unknown-root', 'unknown-root', MARKER + ' Unknown Root CA', None, 'root', -365 * DAY, 20 * 365 * DAY),
    ('rsa', 'rsa-leaf', FIXTURE_HOST, 'rsa-ca', 'leaf_rsa', -datetime.timedelta(hours=1), 90 * DAY),
    ('ecdsa', 'ecdsa-leaf', FIXTURE_HOST, 'ecdsa-ca', 'leaf_ecdsa', -datetime.timedelta(hours=1), 90 * DAY),
    ('wrong-name', 'rsa-leaf', WRONG_HOST, 'rsa-ca', 'leaf_wrong', -datetime.timedelta(hours=1), 90 * DAY),
    ('expired', 'rsa-leaf', FIXTURE_HOST, 'rsa-ca', 'leaf_rsa', -90 * DAY, -30 * DAY),
    ('not-yet-valid', 'rsa-leaf', FIXTURE_HOST, 'rsa-ca', 'leaf_rsa', 30 * DAY, 120 * DAY),
    ('unknown-ca', 'rsa-leaf', FIXTURE_HOST, 'unknown-root', 'leaf_rsa', -datetime.timedelta(hours=1), 90 * DAY),
]
KEYS = {'rsa-root': 'rsa', 'rsa-ca': 'rsa', 'rsa-leaf': 'rsa', 'unknown-root': 'rsa',
        'ecdsa-root': 'ec', 'ecdsa-ca': 'ec', 'ecdsa-leaf': 'ec'}
TRUSTED_ROOTS = ('rsa-root', 'ecdsa-root')
# what each TLS server sends: (certificate, the CA certificates after it)
CHAINS = {'rsa': ('rsa', ['rsa-ca']), 'ecdsa': ('ecdsa', ['ecdsa-ca']), 'wrong-name': ('wrong-name', ['rsa-ca']),
          'expired': ('expired', ['rsa-ca']), 'not-yet-valid': ('not-yet-valid', ['rsa-ca']),
          'unknown-ca': ('unknown-ca', []), 'no-chain': ('rsa', [])}
KEY_OF = {name: key for name, key, *_ in CERTS}


def find_openssl():
    """an OpenSSL 3 command: $OPENSSL, else the first `openssl` on PATH that is OpenSSL 3 (macOS's /usr/bin/openssl
    is LibreSSL, which lacks options used here), else Homebrew's"""
    candidates = [os.environ.get('OPENSSL'), shutil.which('openssl'), '/opt/homebrew/bin/openssl',
                  '/usr/local/bin/openssl', '/usr/bin/openssl']
    for c in candidates:
        if not c or not os.path.exists(c):
            continue
        try:
            v = subprocess.run([c, 'version'], capture_output=True, text=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        if v.startswith('OpenSSL ') and int(v.split()[1].split('.')[0]) >= 3:
            return c
    raise RuntimeError('web-fixtures: no OpenSSL 3 openssl command found (set OPENSSL=/path/to/openssl)')


def asn1_time(t):
    """an openssl ca -startdate/-enddate value (YYYYMMDDHHMMSSZ, which OpenSSL 3.0 takes too)"""
    return t.strftime('%Y%m%d%H%M%SZ')


class Pki:
    """one run's test CA in directory: Pki(dir).paths['rsa'] is a certificate, .keys['rsa-leaf'] a key,
    .chain('rsa') a server's chain file, .ca_file the roots a test image trusts, .dates[name] (from, until)"""

    def __init__(self, directory, now=None, openssl=None):
        self.dir = Path(directory)
        self.openssl = openssl or find_openssl()
        self.now = (now or datetime.datetime.now(datetime.timezone.utc)).replace(microsecond=0)
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / 'issued').mkdir(exist_ok=True)
        (self.dir / 'index.txt').write_text('')
        (self.dir / 'openssl.cnf').write_text(OPENSSL_CNF)
        self.paths, self.keys, self.dates, self.chains = {}, {}, {}, {}
        for name, kind in KEYS.items():
            self.keys[name] = self._key(name, kind)
        for name, key, cn, issuer, ext, start, end in CERTS:
            self.dates[name] = (self.now + start, self.now + end)
            self.paths[name] = self._sign(name, self.keys[key], cn, issuer, ext, self.now + start, self.now + end)
        for name, (cert, cas) in CHAINS.items():
            path = self.dir / f'{name}-chain.pem'
            path.write_bytes(b''.join(self.paths[c].read_bytes() for c in [cert] + cas))
            self.chains[name] = (path, self.keys[KEY_OF[cert]])
        self.ca_file = self.dir / 'ca.pem'
        self.ca_file.write_bytes(b''.join(self.paths[r].read_bytes() for r in TRUSTED_ROOTS))
        self.unknown_ca_file = self.paths['unknown-root']

    def _run(self, *args):
        env = dict(os.environ, OPENSSL_CONF=str(self.dir / 'openssl.cnf'))
        r = subprocess.run([self.openssl, *args], cwd=self.dir, env=env, capture_output=True, text=True, timeout=120)
        if r.returncode:
            raise RuntimeError(f"web-fixtures: openssl {' '.join(args)} failed:\n{r.stdout}{r.stderr}")
        return r.stdout

    def _key(self, name, kind):
        path = self.dir / f'{name}.key'
        if kind == 'rsa':
            self._run('genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', path.name)
        else:
            self._run('genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256', '-out', path.name)
        return path

    def _sign(self, name, key, cn, issuer, ext, start, end):
        csr, out = self.dir / f'{name}.csr', self.dir / f'{name}.pem'
        self._run('req', '-new', '-config', 'openssl.cnf', '-key', key.name, '-subj',
                  f'/O=SurfOS test fixtures/CN={cn}', '-out', csr.name)
        args = ['ca', '-config', 'openssl.cnf', '-batch', '-notext', '-in', csr.name, '-out', out.name,
                '-extensions', ext, '-startdate', asn1_time(start), '-enddate', asn1_time(end)]
        if issuer:
            args += ['-cert', self.paths[issuer].name, '-keyfile', self.keys[KEY_OF[issuer]].name]
        else:
            args += ['-selfsign', '-keyfile', key.name]
        self._run(*args)
        return out

    def chain(self, name):
        return self.chains[name][0]


def pem_blocks(data):
    """the DER bytes of every certificate in PEM text (bytes); what lies between the markers and is not base64 (a
    program holds the markers as strings: Mbed TLS's PEM parser does) is no certificate and is skipped"""
    import base64
    import binascii
    out, rest = [], data
    while True:
        start = rest.find(b'-----BEGIN CERTIFICATE-----')
        if start < 0:
            return out
        end = rest.find(b'-----END CERTIFICATE-----', start)
        if end < 0:
            return out
        body = rest[start + len(b'-----BEGIN CERTIFICATE-----'):end]
        try:
            out.append(base64.b64decode(b''.join(body.split()), validate=True))
        except (binascii.Error, ValueError):
            pass
        rest = rest[end + 1:]


def has_fixture_ca(data):
    """does this file (bytes) hold a certificate of a fixture CA, in PEM or as DER? Their subjects say MARKER"""
    return MARKER.encode() in data or any(MARKER.encode() in der for der in pem_blocks(data))


# ---- the servers ---------------------------------------------------------------------------------------------------

# (name, kind, chain, TLS versions, what it is for): every 'tls' and 'http' server serves ROUTES
SERVERS = [
    ('http', 'http', None, None, 'plain HTTP'),
    ('https', 'tls', 'rsa', ('1.2', '1.3'), 'HTTPS: the RSA 2048 chain (leaf, intermediate), TLS 1.2 and 1.3'),
    ('https-ecdsa', 'tls', 'ecdsa', ('1.2', '1.3'), 'HTTPS: the ECDSA P-256 chain (leaf, intermediate)'),
    ('tls12', 'tls', 'rsa', ('1.2', '1.2'), 'HTTPS, TLS 1.2 only'),
    ('tls13', 'tls', 'rsa', ('1.3', '1.3'), 'HTTPS, TLS 1.3 only'),
    ('wrong-name', 'tls', 'wrong-name', ('1.2', '1.3'), f'a certificate for {WRONG_HOST} only'),
    ('expired', 'tls', 'expired', ('1.2', '1.3'), 'a certificate that expired 30 days ago'),
    ('not-yet-valid', 'tls', 'not-yet-valid', ('1.2', '1.3'), 'a certificate valid only from 30 days on'),
    ('unknown-ca', 'tls', 'unknown-ca', ('1.2', '1.3'), 'a certificate from a root the test image does not have'),
    ('no-chain', 'tls', 'no-chain', ('1.2', '1.3'), 'the good RSA leaf without its intermediate'),
    ('garbage', 'garbage', None, None, 'answers a ClientHello with bytes that are not TLS'),
    ('hangup', 'hangup', None, None, 'closes the connection once the ClientHello is in'),
    ('tls-stall', 'stall', None, None, 'accepts and never answers (the handshake bound)'),
    ('refused', 'refused', None, None, 'a port nothing listens on: connections are refused'),
]
TLS_VERSIONS = {'1.2': ssl.TLSVersion.TLSv1_2, '1.3': ssl.TLSVersion.TLSv1_3}
GARBAGE = b'SURFOS-FIXTURE: this is not TLS\r\n' + bytes((i * 37 + 11) & 255 for i in range(512))
HOLD_MAX = 300                          # seconds a stalling connection is held at most


class Fixtures:
    """every server of SERVERS (or of the names in servers) on 127.0.0.1, with one run's test CA (or pki, a Pki
    made before); a context manager"""

    def __init__(self, directory=None, now=None, openssl=None, servers=None, pki=None):
        self.stopping = threading.Event()
        self.lock = threading.Lock()
        self.log = []
        self.counters = {}
        self.once_seen = set()
        self.ports, self.schemes = {}, {}
        self._listeners, self._threads, self._conns = [], [], set()
        self._own_dir = directory is None
        self.dir = Path(tempfile.mkdtemp(prefix='ut-fixtures-') if directory is None else directory)
        try:
            self.pki = pki or Pki(self.dir / 'pki', now, openssl)
            self.ca_file = self.pki.ca_file
            self.ca_pem = self.ca_file.read_bytes()
            for name, kind, chain, versions, _ in SERVERS:
                if servers is None or name in servers:
                    self._start(name, kind, chain, versions)
        except BaseException:
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    # -- starting and stopping --
    def _start(self, name, kind, chain, versions):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(('127.0.0.1', 0))
        self._listeners.append(sock)
        self.ports[name] = sock.getsockname()[1]
        self.schemes[name] = 'http' if kind in ('http', 'refused') else 'https'
        if kind == 'refused':
            # a port nothing listens on: macOS drops a SYN to a socket that is bound but not listening (no RST,
            # the connect times out), so the port is let go again, as tools/qemu-run.py's free_port() does
            sock.close()
            self._listeners.remove(sock)
            return
        sock.listen(64)
        ctx = self._context(chain, versions) if kind == 'tls' else None
        t = threading.Thread(target=self._accept, args=(sock, name, kind, ctx), name=f'fixture-{name}', daemon=True)
        self._threads.append(t)
        t.start()

    def _context(self, chain, versions):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version, ctx.maximum_version = TLS_VERSIONS[versions[0]], TLS_VERSIONS[versions[1]]
        path, key = self.pki.chains[chain]
        ctx.load_cert_chain(str(path), str(key))
        ctx.set_alpn_protocols(['http/1.1'])

        def sni(sslsock, server_name, _ctx):
            sslsock.fixture_sni = server_name     # None when the client sent no server name
        ctx.sni_callback = sni
        return ctx

    def close(self):
        """stop every server: the listening sockets, every open connection, every thread (idempotent). The
        stop is set before the lock is taken and _accept() checks it under the lock, so no connection or
        thread can start after the copies below are made."""
        self.stopping.set()
        for s in self._listeners:
            try:
                s.close()
            except OSError:
                pass
        with self.lock:
            conns, threads = list(self._conns), list(self._threads)
        for c in conns:
            try:
                # the TCP connection under TLS too (SSLSocket.shutdown() would drop the TLS object under the
                # thread reading it): its reads and writes end at once
                socket.socket.shutdown(c, socket.SHUT_RDWR)
            except OSError:
                pass
        for t in threads:
            if t is not threading.current_thread():
                t.join(timeout=5)
        if self._own_dir:
            shutil.rmtree(self.dir, ignore_errors=True)

    def threads_alive(self):
        return [t.name for t in self._threads if t.is_alive()]

    # -- what came in --
    def _record(self, server, peer):
        rec = {'server': server, 'scheme': self.schemes[server], 'peer': list(peer), 'sni': None, 'tls': None,
               'alpn': None, 'cipher': None, 'handshake_error': None, 'request_line': None, 'method': None,
               'target': None, 'version': None, 'host': None, 'headers': [], 'route': None, 'bytes_sent': 0,
               'client_closed': False, 'error': None, 'connected': time.monotonic(), 'requested': None,
               'ended': None, 'received': b''}
        with self.lock:
            self.log.append(rec)
        return rec

    def _set(self, rec, **fields):
        with self.lock:
            rec.update(fields)

    def records(self, server=None, path=None, since=0):
        """copies of the records (each connection's), oldest first: of one server, of one path (without the
        query), after a mark()"""
        with self.lock:
            recs = [dict(r) for r in self.log[since:]]
        if server is not None:
            recs = [r for r in recs if r['server'] == server]
        if path is not None:
            recs = [r for r in recs if r['target'] is not None and r['target'].split('?')[0] == path]
        return recs

    def mark(self):
        """a place in the log: records(since=mark) are the connections after it"""
        with self.lock:
            return len(self.log)

    def wait_for(self, test, timeout=10, since=0):
        """the first record (a copy) after since for which test(record) is true, waiting up to timeout; None"""
        deadline = time.monotonic() + timeout
        while True:
            for r in self.records(since=since):
                if test(r):
                    return r
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def url(self, server, path='/', host=FIXTURE_HOST):
        """the address of path on a server as the guest writes it: http://fixture.test:PORT/path"""
        return f'{self.schemes[server]}://{host}:{self.ports[server]}{path}'

    def undertow_conf(self, offset=0):
        """the test image's /initrd/etc/undertow.conf"""
        return (f'# Undertow test image (tools/qemu-web.py): the fixture servers of tools/web-fixtures.py on the\n'
                f'# host that runs QEMU, which the guest reaches at {GUEST_HOST}\n'
                f'rtc_offset_minutes={offset}\nhost={FIXTURE_HOST} {GUEST_HOST}\nhost={WWW_HOST} {GUEST_HOST}\n')

    def dump_log(self, path):
        """every record as one JSON line (the bytes a server received are given as text, with escapes)"""
        with open(path, 'w') as f:
            for r in self.records():
                r = dict(r)
                r['received'] = r['received'].decode('latin-1')
                f.write(json.dumps(r, sort_keys=True) + '\n')

    # -- connections --
    def _accept(self, lsock, name, kind, ctx):
        while not self.stopping.is_set():
            try:
                r, _, _ = select.select([lsock], [], [], 0.2)
                if not r:
                    continue
                conn, peer = lsock.accept()
            except (OSError, ValueError):
                break                       # the listening socket was closed: the fixtures stop
            t = threading.Thread(target=self._serve, args=(conn, peer, name, kind, ctx),
                                 name=f'fixture-{name}-{peer[1]}', daemon=True)
            with self.lock:
                if self.stopping.is_set():
                    conn.close()
                    break
                self._conns.add(conn)
                self._threads = [x for x in self._threads if x.is_alive()] + [t]
            t.start()

    def _serve(self, conn, peer, name, kind, ctx):
        rec = self._record(name, peer)
        sock = conn
        try:
            conn.settimeout(60)
            if kind == 'garbage':
                self._set(rec, received=_recv_some(conn))
                conn.sendall(GARBAGE)
                self._set(rec, bytes_sent=len(GARBAGE))
                return
            if kind == 'hangup':
                self._set(rec, received=_recv_some(conn))
                return
            if kind == 'stall':
                self._hold(conn, rec)
                return
            if ctx is not None:
                with self.lock:                 # close() shuts what _conns holds: the TLS socket from now on
                    if self.stopping.is_set():
                        return
                    sock = ctx.wrap_socket(conn, server_side=True, do_handshake_on_connect=False)
                    self._conns.discard(conn)
                    self._conns.add(sock)
                try:
                    sock.do_handshake()
                except (ssl.SSLError, OSError) as e:
                    self._set(rec, handshake_error=_reason(e), sni=getattr(sock, 'fixture_sni', None))
                    return
                cipher = sock.cipher()
                self._set(rec, sni=getattr(sock, 'fixture_sni', None), tls=sock.version(),
                          alpn=sock.selected_alpn_protocol(), cipher=cipher[0] if cipher else None)
            Exchange(self, sock, rec, name).run()
        except Exception as e:              # a client that went away, or a route that failed: recorded, never fatal
            if rec['error'] is None:
                self._set(rec, error=f'{type(e).__name__}: {e}')
        finally:
            self._set(rec, ended=time.monotonic())
            try:
                sock.close()
            except OSError:
                pass
            with self.lock:
                self._conns.discard(sock)
                self._conns.discard(conn)

    def _hold(self, conn, rec):
        """read whatever comes until the client closes (or HOLD_MAX, or the fixtures stop), never answering"""
        got, deadline = b'', time.monotonic() + HOLD_MAX
        while not self.stopping.is_set() and time.monotonic() < deadline:
            r, _, _ = select.select([conn], [], [], 0.2)
            if not r:
                continue
            try:
                data = conn.recv(4096)
            except OSError:
                data = b''
            if not data:
                self._set(rec, client_closed=True)
                break
            got += data
        self._set(rec, received=got[:4096])


def _recv_some(conn, limit=4096, wait=10):
    """what the client sends first (a ClientHello), up to limit bytes, waiting up to wait seconds for it"""
    conn.settimeout(wait)
    try:
        return conn.recv(limit)
    except OSError:
        return b''


def _reason(e):
    """a handshake's failure in a few words: the OpenSSL reason (TLSV1_ALERT_UNKNOWN_CA, ...) or the exception"""
    if isinstance(e, ssl.SSLError) and e.reason:
        return e.reason
    if isinstance(e, (socket.timeout, TimeoutError)):
        return 'timeout'
    return type(e).__name__


class Exchange:
    """one request and its response on one connection (Connection: close: one request per connection)"""

    def __init__(self, fx, sock, rec, server):
        self.fx, self.sock, self.rec, self.server = fx, sock, rec, server
        self.scheme = fx.schemes[server]
        self.port = fx.ports[server]
        self.headers, self.query = [], {}
        self.path = self.target = None
        self.closed_by_client = False

    # -- the request --
    def run(self):
        head = self._read_head()
        if head is None:
            return
        lines = head.decode('latin-1').split('\r\n') if b'\r\n' in head else head.decode('latin-1').split('\n')
        request_line, header_lines = lines[0], [l for l in lines[1:] if l]
        parts = request_line.split(' ')
        self.fx._set(self.rec, request_line=request_line, requested=time.monotonic())
        for l in header_lines:
            name, sep, value = l.partition(':')
            if sep:
                self.headers.append((name.strip(), value.strip()))
        self.fx._set(self.rec, headers=list(self.headers), host=self.header('host'))
        if len(parts) != 3 or not parts[2].startswith('HTTP/'):
            self.fx._set(self.rec, route='bad request')
            self.respond(400, 'Bad Request', 'text/plain', b'web-fixtures: a request line is METHOD TARGET VERSION\n')
            return
        method, target, version = parts
        self.target = target
        path, _, query = target.partition('?')
        self.path = path
        self.query = {k: v[0] for k, v in urllib.parse.parse_qs(query, keep_blank_values=True).items()}
        self.fx._set(self.rec, method=method, target=target, version=version)
        handler, route = find_route(path)
        self.fx._set(self.rec, route=route)
        if method != 'GET':
            self.respond(405, 'Method Not Allowed', 'text/plain', b'web-fixtures: GET only\n', extra=[('Allow', 'GET')])
        else:
            handler(self)
        self.finish()

    def _read_head(self):
        data = b''
        while b'\r\n\r\n' not in data and b'\n\n' not in data:
            if len(data) > 65536:
                self.respond(431, 'Request Header Fields Too Large', 'text/plain', b'web-fixtures: request too long\n')
                return None
            try:
                chunk = self.sock.recv(4096)
            except (socket.timeout, TimeoutError):
                self.fx._set(self.rec, error='no request within 60 s', received=data)
                return None
            except (OSError, ValueError):
                chunk = b''
            if not chunk:
                self.fx._set(self.rec, client_closed=True, received=data)
                return None
            data += chunk
        self.fx._set(self.rec, received=data[:8192])
        end = data.find(b'\r\n\r\n')
        return data[:end] if end >= 0 else data[:data.find(b'\n\n')]

    def header(self, name):
        for n, v in self.headers:
            if n.lower() == name.lower():
                return v
        return None

    # -- the response --
    def send(self, data):
        """bytes as they are"""
        self.sock.sendall(data)
        with self.fx.lock:
            self.rec['bytes_sent'] += len(data)

    def respond(self, status, reason, ctype, body, extra=(), length=True, version='HTTP/1.1'):
        """a whole response: status line, Content-Type (none if ctype is None), Content-Length (unless length is
        False: then the body ends with the connection), extra headers, the body"""
        head = [f'{version} {status} {reason}', 'Server: undertow-web-fixtures', 'Connection: close']
        if ctype is not None:
            head.append(f'Content-Type: {ctype}')
        if length:
            head.append(f'Content-Length: {len(body)}')
        head += [f'{n}: {v}' for n, v in extra]
        self.send(('\r\n'.join(head) + '\r\n\r\n').encode('latin-1') + body)

    def page(self, title, body, status=200, reason='OK', charset='utf-8', extra=()):
        """an HTML page in UTF-8 (body: the markup inside <body>)"""
        doc = (f'<!DOCTYPE html>\n<html lang="en"><head><meta charset="{charset}">\n<title>{title}</title></head>\n'
               f'<body>\n{body.strip()}\n</body></html>\n')
        self.respond(status, reason, f'text/html; charset={charset}', doc.encode(charset), extra)

    def finish(self):
        """the end of a response that went out whole: TLS's close_notify (best effort), then the close"""
        if isinstance(self.sock, ssl.SSLSocket):
            try:
                self.sock.settimeout(1)
                self.sock.unwrap()
            except (OSError, ValueError):
                pass

    def abort(self):
        """the connection ends here, without close_notify (an early end: what a crash or a cut cable does)"""
        self.finish = lambda: None

    def wait(self, seconds):
        """wait up to seconds while watching the connection: False as soon as the client has closed it (or the
        fixtures stop), True when the time is up"""
        deadline = time.monotonic() + seconds
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return True
            if self.fx.stopping.is_set():
                return False
            try:
                r, _, _ = select.select([self.sock], [], [], min(left, 0.1))
            except (OSError, ValueError):
                r = [self.sock]
            if r and self._client_gone():
                return False

    def _client_gone(self):
        try:
            self.sock.settimeout(0.5)
            data = self.sock.recv(1)
        except (ssl.SSLWantReadError, socket.timeout, TimeoutError):
            return False
        except (OSError, ValueError):
            data = b''
        finally:
            try:
                self.sock.settimeout(60)
            except (OSError, ValueError):
                pass
        if not data:
            self.closed_by_client = True
            self.fx._set(self.rec, client_closed=True)
            return True
        return False

    def url(self, path, server=None, host=FIXTURE_HOST):
        return self.fx.url(server or self.server, path, host)

    def number(self, name, default, low, high):
        """a query parameter as a number in [low, high], else default"""
        try:
            v = float(self.query.get(name, default))
        except ValueError:
            return default
        return min(max(v, low), high)


# ---- the routes ------------------------------------------------------------------------------------------------------

ROUTES = {}         # path: (handler, what it sends)
PREFIXES = []       # (prefix, handler, what it sends) for paths with a parameter


def route(path, doc):
    def add(f):
        if path.endswith('*'):
            PREFIXES.append((path[:-1], f, doc))
        else:
            ROUTES[path] = (f, doc)
        return f
    return add


def find_route(path):
    """(handler, the route's name) for a path; not_found for anything else"""
    if path in ROUTES:
        return ROUTES[path][0], path
    for prefix, f, _ in PREFIXES:
        if path.startswith(prefix):
            return f, prefix + '*'
    return not_found, '404'


def safe(text):
    """a request's text inside a page: printable ASCII, HTML-escaped"""
    return html.escape(''.join(c if ' ' <= c <= '~' else '?' for c in text))


# -- the readable pages --

@route('/', 'the home page: links to the other pages, the other scheme and a mailto: link')
def home(ex):
    other = 'https' if ex.scheme == 'http' else 'http'
    ex.page('Undertow Fixture Home', f'''
<h1>Undertow fixtures</h1>
<p>This site is served by tools/web-fixtures.py for the tests of Undertow, the SurfOS web browser.
Every page is fixed text.</p>
<ul>
<li><a href="/about">About these fixtures</a></li>
<li><a href="docs/">The documents</a></li>
<li><a href="/long">A long page</a></li>
<li><a href="/anchors#section-2">Anchors, at section 2</a></li>
<li><a href="{ex.url('/', other)}">The same site over {other.upper()}</a></li>
<li><a href="mailto:owner@{FIXTURE_HOST}">Write to the owner</a></li>
</ul>
<p>The last line of the home page.</p>''')


@route('/about', 'a short page with two links')
def about(ex):
    ex.page('About the fixtures', f'''
<h1>About the fixtures</h1>
<p>The fixture servers listen on 127.0.0.1 of the computer that runs QEMU; the guest reaches them at
{GUEST_HOST}, by the names its undertow.conf gives that address.</p>
<p><a href="/">Home</a> | <a href="docs/guide?part=1">The guide, part 1</a></p>''')


@route('/docs', 'a redirect to /docs/ (301)')
def docs_slash(ex):
    ex.respond(301, 'Moved Permanently', 'text/html', b'<a href="/docs/">/docs/</a>\n', extra=[('Location', '/docs/')])


@route('/docs/', 'relative links: a sibling, ./ with a query, ../, a query alone, scheme-relative, a fragment alone')
def docs_index(ex):
    filler = '\n'.join(f'<p>Filler paragraph {i} of the documents.</p>' for i in range(1, 13))
    ex.page('The documents', f'''
<h1>The documents</h1>
<p><a href="guide">The guide</a> is the only document.</p>
<p><a href="./guide?part=2">The guide, part 2</a></p>
<p><a href="../about">About, one level up</a></p>
<p><a href="?sort=name">These documents sorted by name</a></p>
<p><a href="//{FIXTURE_HOST}:{ex.port}/about">About, scheme-relative</a></p>
<p><a href="#bottom">To the bottom of this page</a></p>
{filler}
<p id="bottom">The bottom of the documents.</p>''')


@route('/docs/guide', 'the guide; ?part=N (digits) is its part')
def guide(ex):
    part = ex.query.get('part', '1')
    part = part if part.isdigit() and len(part) <= 3 else '1'
    ex.page(f'The guide, part {part}', f'''
<h1>The guide, part {part}</h1>
<p>This is part {part} of the guide.</p>
<p><a href="../">Back to the documents</a> | <a href="/">Home</a></p>''')


LONG_LINES = 400
LONG_SPECIAL = {1: 'the first line', 120: 'here is the first needle', 240: 'here is the second NEEDLE',
                360: 'here is the third Needle', LONG_LINES: 'the last line'}


@route('/long', f'{LONG_LINES} numbered lines in <pre> (paging; "needle" in lines 120, 240 and 360, in three cases)')
def long_page(ex):
    lines = '\n'.join(f'Line {i:03d}: {LONG_SPECIAL.get(i, "nothing to see here")}' for i in range(1, LONG_LINES + 1))
    ex.page('A long page', f'<h1>A long page</h1>\n<pre>{lines}</pre>\n<p><a href="/">Home</a></p>')


@route('/anchors', 'four sections with id= and <a name=> targets, 24 rows each; links to #section-3, #section-2, '
                   'a missing #nowhere, and this page by another address at #section-4; a duplicate id')
def anchors(ex):
    def section(n, open_tag):
        paras = '\n'.join(f'<p>Text of section {n}, paragraph {i}.</p>' for i in range(1, 13))
        return f'{open_tag}\n{paras}'
    ex.page('Anchors', '\n'.join([
        '<p><a href="#section-3">To section 3</a> | <a href="#section-2">To section 2</a> | '
        '<a href="#nowhere">To a missing anchor</a> | <a href="/anchors?again=1#section-4">Section 4, by address</a></p>',
        section(1, '<h2 id="section-1">Section 1</h2>'),
        section(2, '<h2 id="section-2">Section 2</h2>'),
        section(3, '<a name="section-3"></a><h2>Section 3</h2>'),
        section(4, '<h2 id="section-4">Section 4</h2>'),
        '<p id="section-1">A second element with the id section-1: the first one counts.</p>']))


@route('/links/target', 'where the links of /links/N go: ?n=K')
def link_target(ex):
    n = ex.query.get('n', '0')
    n = n if n.isdigit() and len(n) <= 4 else '0'
    ex.page(f'Link target {n}', f'<h1>Link target {n}</h1>\n<p>You followed link {n}.</p>')


@route('/links/*', '/links/N: N links (1..3000), "link K" to /links/target?n=K')
def links_n(ex):
    n = ex.path[len('/links/'):]
    if not n.isdigit() or not 1 <= int(n) <= 3000:
        return not_found(ex)
    n = int(n)
    items = '\n'.join(f'<li><a href="/links/target?n={i}">link {i}</a></li>' for i in range(1, n + 1))
    ex.page(f'{n} links', f'<h1>{n} links</h1>\n<ul>\n{items}\n</ul>\n<p>The end of the {n} links.</p>')


def bytecap_path(base):
    """the path of the byte-cap page for a server whose address is base ("http://fixture.test:PORT"): long enough
    that each of its relative links resolves to about 2000 bytes, short enough that a link's address stays under
    Undertow's 2048"""
    return '/bytecap/' + 'p' * (2000 - len(base) - len('/bytecap/') - 1) + '/'


@route('/bytecap/*', '/bytecap/LONG/: 2048 relative links of one digit to four, each about 2000 bytes once '
                     'resolved, so a page\'s link bytes pass 1 MiB before its 2048th link; /bytecap/LONG/N: target N')
def bytecap(ex):
    last = ex.path.rsplit('/', 1)[1]
    if last.isdigit() and len(last) <= 4:
        ex.page(f'Byte cap target {last}', f'<h1>Byte cap target {last}</h1>\n<p>This is target {last} of the byte cap.</p>')
        return
    if not ex.path.endswith('/'):
        return not_found(ex)
    items = '\n'.join(f'<li><a href="{i}">l{i}</a></li>' for i in range(1, LINKS_MAX + 1))
    ex.page('The byte cap', f'<h1>The byte cap</h1>\n<ul>\n{items}\n</ul>\n<p>The end of the byte cap.</p>')


@route('/utf8', 'UTF-8 text: accents, quotes, a dash, an ellipsis, symbols')
def utf8(ex):
    ex.page('UTF-8 text', '<h1>UTF-8 text</h1>\n<p>Gr\u00fc\u00dfe aus Z\u00fcrich \u2014 \u201cquoted\u201d '
                          'and \u2018single\u2019\u2026 \u00bd \u00b0 \u00b1 caf\u00e9.</p>')


@route('/latin1', 'ISO-8859-1 bytes, the charset in a <meta> only')
def latin1(ex):
    body = (b'<!DOCTYPE html>\n<html><head><meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">\n'
            b'<title>Latin-1 text</title></head>\n<body><p>Latin-1: caf\xe9, na\xefve, \xbd, \xa9.</p></body></html>\n')
    ex.respond(200, 'OK', 'text/html', body)


@route('/windows-1252', 'windows-1252 bytes (smart quotes, euro, e acute), the charset in Content-Type')
def windows_1252(ex):
    body = (b'<!DOCTYPE html>\n<html><head><title>Windows-1252 text</title></head>\n'
            b'<body><p>\x93Smart quotes\x94, caf\xe9, \x805 and an en dash \x96 here.</p></body></html>\n')
    ex.respond(200, 'OK', 'text/html; charset=windows-1252', body)


@route('/shift_jis', 'Shift_JIS bytes, a charset Undertow does not read: its ASCII and a notice')
def shift_jis(ex):
    body = ('<!DOCTYPE html>\n<html><head><title>Shift_JIS page</title></head>\n'
            '<body><p>ASCII survives: \u65e5\u672c\u8a9e\u306e\u30da\u30fc\u30b8.</p></body></html>\n').encode('shift_jis')
    ex.respond(200, 'OK', 'text/html; charset=Shift_JIS', body)


@route('/plain.txt', 'text/plain: shown as it is, markup and all')
def plain(ex):
    ex.respond(200, 'OK', 'text/plain; charset=utf-8',
               b'Plain text, shown as it is.\n    An indented line.\n<b>Not markup</b> &amp; no entities.\n')


@route('/xhtml', 'application/xhtml+xml')
def xhtml(ex):
    body = (b'<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head>'
            b'<title>An XHTML page</title></head><body><p>XHTML is read as HTML.</p></body></html>\n')
    ex.respond(200, 'OK', 'application/xhtml+xml', body)


@route('/no-type', 'text without a Content-Type header')
def no_type(ex):
    ex.respond(200, 'OK', None, b'This page came without a Content-Type header.\nIt is plain text all the same.\n')


@route('/binary', 'bytes with NULs and no Content-Type')
def binary(ex):
    ex.respond(200, 'OK', None, bytes(range(256)) * 4)


@route('/octet', 'application/octet-stream')
def octet(ex):
    ex.respond(200, 'OK', 'application/octet-stream', b'\x7fELF' + bytes(60))


def png_1x1():
    """the smallest PNG: one white pixel"""
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\x00\xff\xff\xff')) + chunk(b'IEND', b''))


@route('/image.png', 'image/png (a 1x1 picture): not text')
def image(ex):
    ex.respond(200, 'OK', 'image/png', png_1x1())


@route('/gzip', 'Content-Encoding: gzip (HTML, compressed): an encoding Undertow does not undo')
def gzipped(ex):
    body = gzip.compress(b'<html><title>Compressed</title><p>This was compressed.</p></html>\n', mtime=0)
    ex.respond(200, 'OK', 'text/html; charset=utf-8', body, extra=[('Content-Encoding', 'gzip')])


@route('/forms', 'a form with every kind of control')
def forms(ex):
    ex.page('A form', '''
<h1>A form</h1>
<form action="/search" method="get">
<label>Search: <input type="text" name="q"></label>
<input type="submit" value="Search">
<select name="s"><option>First option</option><option>Second option</option></select>
<input type="checkbox" name="c">
<textarea name="t">Typed text</textarea>
</form>
<p>After the form.</p>''')


@route('/scripts', 'script, style, template (never shown) and noscript (shown)')
def scripts(ex):
    ex.page('Scripts and styles', '''
<script>document.write("UT-SCRIPT-RAN"); if (1 < 2 && "</p>") {}</script>
<style>body { color: red } /* UT-STYLE-TEXT */</style>
<template><p>UT-TEMPLATE-TEXT</p></template>
<noscript><p>Shown without scripts.</p></noscript>
<p>Visible text after the scripts.</p>''')


# Escape sequences a page might send to take over a terminal: none of them may reach it (tools/qemu-web.py
# checks the raw serial bytes for ESCAPE_BYTES). The browser's own colours are other sequences.
ESCAPE_TITLE = 'Escapes \x1b[2J\x1b]0;UT-PWNED-TITLE\x07 in the title'
ESCAPE_BYTES = [b'\x1b[31;1;4;5;7m', b'\x1b]0;', b'\x07', b'\x1b[6n', b'\xc2\x9b', b'\xe2\x80\xae', b'\x1bP',
                b'\x08X', b'\x1b[7mUT-INVERSE']


@route('/escape', 'ESC, OSC, DSR, C1, bidi overrides, BEL, BS and NUL in the title, the text, a label and an href')
def escape(ex):
    ex.page(ESCAPE_TITLE, '''
<p>Before. \x1b[31;1;4;5;7mUT-SGR\x1b[0m after SGR. OSC: \x1b]0;UT-PWNED-OSC\x07. DSR: \x1b[6n.
DCS: \x1bPq#0\x1b\\. C1 CSI: \u009b31mUT-C1. Bidi: \u202eUT-RLO\u202c. Bell:\x07. Backspace:\x08X. NUL:\x00.</p>
<p><a href="/about\x1b[2J">A link whose address has an escape</a></p>
<p><a href="/about">A label with \x1b[7mUT-INVERSE\x1b[0m inside</a></p>
<p>The end of the escapes.</p>''')


@route('/escape/reason', 'a 404 whose reason phrase and a header hold escape sequences')
def escape_reason(ex):
    body = b'<html><title>Bad reason</title><p>The reason phrase had escapes in it.</p></html>\n'
    ex.send(b'HTTP/1.1 404 Not\x1b[2JFound\x07\r\nContent-Type: text/html\r\nX-Note: \x1b[6n\r\nConnection: close\r\n'
            b'Content-Length: ' + str(len(body)).encode() + b'\r\n\r\n' + body)


SPLIT_BODY = ('<!DOCTYPE html>\n<html><head><meta charset="utf-8"><title>Split everywhere</title></head>\n'
              '<body><p>Gr\u00fc\u00dfe \u2014 caf&eacute; &#x2014; &amp; &#8364;5 \u65e5\u672c</p>\n'
              '<p>The end of the split page.</p></body></html>\n').encode()


@route('/split', 'a chunked page written one byte at a time (TCP_NODELAY: each byte a segment of its own, over TLS a '
                 'record of its own): the status line, headers, chunk lines, UTF-8 and entities split at every byte')
def split(ex):
    data = (b'HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nTransfer-Encoding: chunked\r\n'
            b'Connection: close\r\n\r\n' + chunked([(SPLIT_BODY[i:i + 7], ';x=1') for i in range(0, len(SPLIT_BODY), 7)]))
    ex.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    for i in range(len(data)):
        ex.send(data[i:i + 1])
        if not ex.wait(0.002):
            ex.abort()
            return


# a title far over UT_FIELD_MAX with escape sequences all through it; tools/qemu-web.py checks the raw bytes
LONG_ESCAPES = [b'\x1b]0;', b'\x07', b'\x1b[6n']
LONG_TITLE = ('UT-LONG-TITLE-START ' + 'a long title with \x1b]0;UT-T\x07 and \x1b[6n inside it, ' * 70 +
              'UT-LONG-TITLE-END')


@route('/long-fields', 'a 3000-byte title full of escape sequences, a link whose address is 2000 bytes and one '
                       'over 2048, a 5000-byte word')
def long_fields(ex):
    href0 = '/about?x='
    href0 += 'x' * (2000 - len(ex.url(href0)))
    href1 = '/about?y=' + 'y' * 2100
    ex.page(LONG_TITLE, f'''
<h1>Long fields</h1>
<p><a href="{href0}">An address of 2000 bytes</a></p>
<p><a href="{href1}">An address over 2048 bytes</a></p>
<p>UT-WORD-START{"w" * 5000}UT-WORD-END</p>
<p>The end of the long fields.</p>''')


@route('/counter', 'how many times this server sent /counter (reload)')
def counter(ex):
    with ex.fx.lock:
        n = ex.fx.counters.get((ex.server, 'counter'), 0) + 1
        ex.fx.counters[(ex.server, 'counter')] = n
    ex.page('Counter', f'<h1>Counter</h1>\n<p>This page has been fetched {n} times.</p>')


@route('/once', '?id=X: a page the first time; an early end every time after (a reload that fails)')
def once(ex):
    key = (ex.server, ex.query.get('id', ''))
    with ex.fx.lock:
        first = key not in ex.fx.once_seen
        ex.fx.once_seen.add(key)
    if first:
        ex.page('Only once', f'<h1>Only once</h1>\n<p>The first and only good fetch of {safe(key[1])}.</p>')
    else:
        early_eof(ex)


@route('/plumbing', 'text/plain: what the harness fetches with nettest get, before any browser runs')
def plumbing(ex):
    ex.respond(200, 'OK', 'text/plain', b'SurfOS web fixture: plumbing ok\n')


@route('/status/*', '/status/NNN: that status with an HTML page (no body for 1xx, 204 and 304)')
def status(ex):
    code = ex.path[len('/status/'):]
    if not code.isdigit() or not 200 <= int(code) <= 599:
        return not_found(ex)
    code = int(code)
    reason = REASONS.get(code, 'Status')
    if code in (204, 304):
        ex.send(f'HTTP/1.1 {code} {reason}\r\nServer: undertow-web-fixtures\r\nConnection: close\r\n\r\n'.encode())
        return
    ex.page(f'Status {code}', f'<h1>{code} {reason}</h1>\n<p>This page came with status {code}.</p>\n'
                              f'<p><a href="/">Home</a></p>', status=code, reason=reason)


REASONS = {200: 'OK', 201: 'Created', 204: 'No Content', 300: 'Multiple Choices', 301: 'Moved Permanently',
           302: 'Found', 303: 'See Other', 304: 'Not Modified', 307: 'Temporary Redirect', 308: 'Permanent Redirect',
           400: 'Bad Request', 403: 'Forbidden', 404: 'Not Found', 410: 'Gone', 418: "I'm a teapot",
           500: 'Internal Server Error', 502: 'Bad Gateway', 503: 'Service Unavailable'}


def not_found(ex):
    ex.page('Not Found', f'<h1>Not Found</h1>\n<p>There is no page at {safe(ex.path or "")}.</p>\n'
                         f'<p><a href="/">Home</a></p>', status=404, reason='Not Found')


# -- framing --

def text_body(what):
    return f'{what}: the body arrived whole.\n'.encode()


@route('/frame/length', 'Content-Length')
def frame_length(ex):
    ex.respond(200, 'OK', 'text/plain', text_body('Content-Length framing'))


@route('/frame/length-twice', 'the same Content-Length twice (fine)')
def frame_length_twice(ex):
    body = text_body('Content-Length given twice with one value')
    ex.respond(200, 'OK', 'text/plain', body, extra=[('Content-Length', str(len(body)))])


@route('/frame/length-conflict', 'two different Content-Length values (refused)')
def frame_length_conflict(ex):
    body = text_body('two Content-Length values')
    ex.respond(200, 'OK', 'text/plain', body, extra=[('Content-Length', str(len(body) + 5))])


@route('/frame/length-bad', 'Content-Length: 12abc (refused)')
def frame_length_bad(ex):
    ex.respond(200, 'OK', 'text/plain', b'x' * 12, length=False, extra=[('Content-Length', '12abc')])


@route('/frame/length-negative', 'Content-Length: -1 (refused)')
def frame_length_negative(ex):
    ex.respond(200, 'OK', 'text/plain', b'x' * 12, length=False, extra=[('Content-Length', '-1')])


@route('/frame/length-20-digits', 'a 20-digit Content-Length (refused)')
def frame_length_20(ex):
    ex.respond(200, 'OK', 'text/plain', b'x' * 12, length=False, extra=[('Content-Length', '18446744073709551617')])


CHUNKS = [(b'Chunked transfer coding: ', ';name=value'), (b'three chunks, ', ';quoted="a;b"'),
          (b'with extensions and a trailer.\n', ';flag')]
CHUNKED_BODY = b''.join(c for c, _ in CHUNKS)


def chunked(pieces, last_ext='', trailers=()):
    """pieces ((bytes, extension)) as chunks, the last chunk and the trailers"""
    out = b''.join(b'%x%s\r\n%s\r\n' % (len(c), e.encode(), c) for c, e in pieces)
    return out + f'0{last_ext}\r\n'.encode() + b''.join(f'{n}: {v}\r\n'.encode() for n, v in trailers) + b'\r\n'


@route('/frame/chunked', 'chunked: three chunks with extensions, a last chunk with one, a trailer')
def frame_chunked(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n'
            b'Trailer: X-Fixture-Trailer\r\nConnection: close\r\n\r\n' +
            chunked(CHUNKS, ';last=yes', [('X-Fixture-Trailer', 'present')]))


@route('/frame/chunked-huge', 'a chunk of ffffffffffffffff bytes (refused)')
def frame_chunked_huge(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
            b'ffffffffffffffff\r\n' + b'x' * 64)
    ex.abort()


@route('/frame/chunked-and-length', 'Transfer-Encoding: chunked and Content-Length both (refused)')
def frame_chunked_and_length(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nContent-Length: 10\r\n'
            b'Connection: close\r\n\r\n' + chunked(CHUNKS))


@route('/frame/te-gzip', 'Transfer-Encoding: gzip, chunked (a coding Undertow does not undo: refused)')
def frame_te_gzip(ex):
    data = gzip.compress(CHUNKED_BODY, mtime=0)
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: gzip, chunked\r\nConnection: close\r\n\r\n'
            + chunked([(data, '')]))


@route('/frame/close', 'HTTP/1.1 without a length: the body ends with the connection')
def frame_close(ex):
    ex.respond(200, 'OK', 'text/plain', text_body('Close-delimited framing'), length=False)


@route('/frame/http10', 'an HTTP/1.0 response without a length')
def frame_http10(ex):
    ex.respond(200, 'OK', 'text/plain', text_body('An HTTP/1.0 response'), length=False, version='HTTP/1.0')


def interim(n):
    return b'HTTP/1.1 100 Continue\r\n\r\n' + b'HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n' * (n - 1)


@route('/frame/interim', '100 Continue and 103 Early Hints before the response')
def frame_interim(ex):
    ex.send(interim(2))
    ex.respond(200, 'OK', 'text/plain', text_body('After two interim responses'))


@route('/frame/interim-8', f'{INTERIM_MAX} interim responses (the most Undertow takes)')
def frame_interim_8(ex):
    ex.send(interim(INTERIM_MAX))
    ex.respond(200, 'OK', 'text/plain', text_body(f'After {INTERIM_MAX} interim responses'))


@route('/frame/interim-9', f'{INTERIM_MAX + 1} interim responses (refused)')
def frame_interim_9(ex):
    ex.send(interim(INTERIM_MAX + 1))
    ex.respond(200, 'OK', 'text/plain', text_body(f'After {INTERIM_MAX + 1} interim responses'))


@route('/frame/101', '101 Switching Protocols (refused: Undertow asked for no upgrade)')
def frame_101(ex):
    ex.send(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n')
    ex.wait(1)                  # a client that asked for no upgrade gives up at once


@route('/frame/early-eof', 'Content-Length: 1000 and 100 bytes, then the end of the connection')
def early_eof(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n' +
            b'early end. ' * 9 + b'.')
    ex.abort()


@route('/frame/early-eof-chunked', 'chunked: a chunk of 100 bytes cut after 50')
def frame_early_eof_chunked(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
            b'64\r\n' + b'y' * 50)
    ex.abort()


@route('/frame/close-cut', 'HTTP/1.1 without a length, the connection ended without TLS close_notify (10/25/26: '
                           'fetch refuses that over HTTPS)')
def frame_close_cut(ex):
    ex.respond(200, 'OK', 'text/plain', b'A body that only the end of the connection delimits, cut.\n', length=False)
    ex.abort()


@route('/frame/early-eof-head', 'the end of the connection in the middle of the headers')
def frame_early_eof_head(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/pl')
    ex.abort()


@route('/frame/folded', 'obsolete line folding in a header (refused)')
def frame_folded(ex):
    body = text_body('A folded header')
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Folded: first part\r\n  second part\r\n'
            b'Content-Length: ' + str(len(body)).encode() + b'\r\nConnection: close\r\n\r\n' + body)


@route('/frame/huge-headers', f'over {HEADERS_MAX // 1024} KiB of header lines (refused)')
def frame_huge_headers(ex):
    filler = [(f'X-Filler-{i:02d}', 'h' * 1000) for i in range(HEADERS_MAX // 1000 + 2)]
    ex.respond(200, 'OK', 'text/plain', text_body('Too many headers'), extra=filler)


@route('/frame/bad-status', 'a status line whose code is not three digits (refused)')
def frame_bad_status(ex):
    ex.send(b'HTTP/1.1 2OO OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbad')


@route('/frame/no-status-line', 'a body without any status line (HTTP/0.9 style: refused)')
def frame_no_status(ex):
    ex.send(b'<html><title>No status line</title><p>Just a body.</p></html>\n')


# -- sizes --

def mebibyte_text(extra=0):
    """exactly BODY_MAX + extra bytes of numbered text lines (64 bytes each)"""
    lines = b''.join(b'%07d one mebibyte of text, line by line, for the body limit.\n' % i
                     for i in range(BODY_MAX // 64))
    assert len(lines) == BODY_MAX
    return lines + b'!' * extra


@route('/size/1m', f'exactly {BODY_MAX} bytes of text (the most Undertow takes; it renders the first 512 KB)')
def size_1m(ex):
    ex.respond(200, 'OK', 'text/plain', mebibyte_text())


@route('/size/1m+1', f'{BODY_MAX + 1} bytes of text (refused)')
def size_1m_plus(ex):
    ex.respond(200, 'OK', 'text/plain', mebibyte_text(1))


@route('/size/1m-chunked', f'{BODY_MAX} bytes in 64 KiB chunks')
def size_1m_chunked(ex):
    body = mebibyte_text()
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n' +
            chunked([(body[i:i + 65536], '') for i in range(0, len(body), 65536)]))


@route('/size/1m+1-chunked', f'{BODY_MAX + 1} bytes in 64 KiB chunks (refused)')
def size_1m_plus_chunked(ex):
    body = mebibyte_text(1)
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n' +
            chunked([(body[i:i + 65536], '') for i in range(0, len(body), 65536)]))


@route('/size/1m+1-close', f'{BODY_MAX + 1} bytes without a length (refused)')
def size_1m_plus_close(ex):
    ex.respond(200, 'OK', 'text/plain', mebibyte_text(1), length=False)


# -- redirects --

@route('/redirect/code/*', '/redirect/code/NNN: a redirect with that code (301, 302, 303, 307, 308) to /about')
def redirect_code(ex):
    code = ex.path[len('/redirect/code/'):]
    if code not in ('301', '302', '303', '307', '308'):
        return not_found(ex)
    redirect(ex, int(code), '/about')


def redirect(ex, code, location):
    ex.respond(code, REASONS[code], 'text/html', f'<a href="{html.escape(location)}">moved</a>\n'.encode(),
               extra=[('Location', location)])


@route('/redirect/loop-a', 'a redirect to /redirect/loop-b, which sends you back here (a loop)')
def redirect_loop_a(ex):
    redirect(ex, 302, '/redirect/loop-b')


@route('/redirect/loop-b', 'a redirect to /redirect/loop-a')
def redirect_loop_b(ex):
    redirect(ex, 302, '/redirect/loop-a')


@route('/redirect/relative', 'a redirect to ../about (relative: /about)')
def redirect_relative(ex):
    redirect(ex, 302, '../about')


# 11/1/26: pictures for the browser in a window (stage 2.4): a page with one of each kind, made here: a tiny PNG, a GIF
# and a BMP of one colour each, a JPEG of the same small baseline file the host tests use. /pictures on the https server
# also asks for one over http, which must not reach the http server.
def little_png(rgb):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    raw = b''.join(b'\0' + bytes(rgb) * 4 for _ in range(3))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 4, 3, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def little_bmp(rgb):
    row = bytes((rgb[2], rgb[1], rgb[0])) * 4
    body = row * 3
    return (b'BM' + struct.pack('<IHHI', 54 + len(body), 0, 0, 54) +
            struct.pack('<IiiHHIIiiII', 40, 4, 3, 1, 24, 0, 0, 0, 0, 0, 0) + body)


def little_gif_bytes(rgb):
    # LZW by the book, as tools/wundertow-images.py has it: a colour table of two, twelve pixels of the first
    out, acc, n, size = bytearray(), 0, 0, 3
    next_code = 6

    def put(code):
        nonlocal acc, n
        acc |= code << n
        n += size
        while n >= 8:
            out.append(acc & 255)
            acc >>= 8
            n -= 8
    table, prefix = {}, None
    put(4)
    for v in [0] * 12:
        if prefix is None:
            prefix = (v,)
            continue
        s = prefix + (v,)
        if s in table:
            prefix = s
            continue
        put(prefix[0] if len(prefix) == 1 else table[prefix])
        table[s] = next_code
        next_code += 1
        if next_code == (1 << size) + 1 and size < 12:
            size += 1
        prefix = (v,)
    put(prefix[0] if len(prefix) == 1 else table[prefix])
    put(5)
    if n:
        out.append(acc & 255)
    return (b'GIF89a' + struct.pack('<HHBBB', 4, 3, 0xf1, 0, 0) + bytes(rgb) + bytes(9) +
            b'\x2c' + struct.pack('<HHHHB', 0, 0, 4, 3, 0) + b'\x02' + bytes([len(out)]) + bytes(out) + b'\0\x3b')


@route('/pic/red.png', 'a PNG of 4 x 3 pixels, all red')
def pic_png(ex):
    ex.respond(200, 'OK', 'image/png', little_png((255, 0, 0)))


@route('/pic/green.gif', 'a GIF of 4 x 3 pixels, all green')
def pic_gif(ex):
    ex.respond(200, 'OK', 'image/gif', little_gif_bytes((0, 255, 0)))


@route('/pic/blue.bmp', 'a BMP of 4 x 3 pixels, all blue, with no Content-Type')
def pic_bmp(ex):
    ex.respond(200, 'OK', None, little_bmp((0, 0, 255)))


@route('/pic/gray.jpg', 'a JPEG of 16 x 16 pixels, by the host tests\' fixtures')
def pic_jpg(ex):
    path = Path(__file__).resolve().parent.parent / 'tests/fixtures/wundertow/images/own444.jpg'
    ex.respond(200, 'OK', 'image/jpeg', path.read_bytes())


@route('/pic/moved.png', 'a redirect to the PNG')
def pic_moved(ex):
    redirect(ex, 302, '/pic/red.png')


@route('/pic/broken.png', 'a PNG cut in half')
def pic_broken(ex):
    body = little_png((255, 0, 0))
    ex.respond(200, 'OK', 'image/png', body[:len(body) // 2])


@route('/pic/page.html', 'text/html where a picture was asked for')
def pic_page(ex):
    ex.respond(200, 'OK', 'text/html', b'<p>Not a picture.')


@route('/pictures', 'a page of pictures of each kind, a broken one, a missing one, one over http')
def pictures(ex):
    body = ('<!DOCTYPE html><title>Pictures</title><img src="/pic/red.png"><img src="/pic/green.gif">'
            '<img src="/pic/blue.bmp"><img src="/pic/gray.jpg"><img src="/pic/moved.png">'
            '<img src="/pic/broken.png" alt="broken"><img src="/pic/page.html" alt="a page">'
            '<img src="/no/such/picture.png" alt="missing">'
            f'<img src="{ex.url("/pic/red.png", "http")}" alt="over http">'
            '<img src="/pic/gzip.png" alt="gzip">').encode()
    ex.respond(200, 'OK', 'text/html', body)


@route('/pic/gzip.png', 'a PNG sent compressed, which is refused')
def pic_gzip(ex):
    ex.respond(200, 'OK', 'image/png', gzip.compress(little_png((255, 0, 0))), extra=(('Content-Encoding', 'gzip'),))


@route('/redirect/to-http', 'a redirect to the http server\'s /about (from https: a downgrade, refused)')
def redirect_to_http(ex):
    redirect(ex, 302, ex.url('/about', 'http'))


@route('/redirect/to-https', 'a redirect to the https server\'s /about')
def redirect_to_https(ex):
    redirect(ex, 302, ex.url('/about', 'https'))


@route('/redirect/to-www', f'a redirect to {WWW_HOST} on this server')
def redirect_to_www(ex):
    redirect(ex, 302, ex.url('/about', host=WWW_HOST))


@route('/redirect/space', 'a redirect to "/space here": a Location with a space, which a client sends as %20')
def redirect_space(ex):
    redirect(ex, 302, '/space here')


@route('/space%20here', 'where /redirect/space goes')
def space_here(ex):
    ex.page('A space in the address', '<h1>A space in the address</h1>\n<p>The redirect\'s space arrived as %20.</p>')


@route('/redirect/no-location', 'a 302 without a Location header')
def redirect_no_location(ex):
    ex.respond(302, 'Found', 'text/html', b'<p>Moved, but nobody says where.</p>\n')


@route('/redirect/fragment', 'a redirect to /anchors without a fragment (the request\'s fragment stays)')
def redirect_fragment(ex):
    redirect(ex, 302, '/anchors')


@route('/redirect/with-fragment', 'a redirect to /anchors#section-4 (its own fragment wins)')
def redirect_with_fragment(ex):
    redirect(ex, 302, '/anchors#section-4')


@route('/redirect/*', '/redirect/N: a chain of N redirects (302) ending at a page, /redirect/0')
def redirect_chain(ex):
    n = ex.path[len('/redirect/'):]
    if not n.isdigit() or int(n) > 50:
        return not_found(ex)
    n = int(n)
    if n == 0:
        ex.page('The end of the redirects', '<h1>The end of the redirects</h1>\n<p>Every redirect was followed.</p>')
    else:
        redirect(ex, 302, f'/redirect/{n - 1}')


# -- slow --

@route('/slow/stall', 'headers (Content-Length: 1000) and 10 bytes, then silence until the client goes')
def slow_stall(ex):
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n' +
            b'0123456789')
    ex.wait(HOLD_MAX)
    ex.abort()


@route('/slow/stall-head', 'no response at all until the client goes')
def slow_stall_head(ex):
    ex.wait(HOLD_MAX)
    ex.abort()


@route('/slow/trickle', 'headers (Content-Length: ?length=, 100), then one byte every ?interval= seconds (10)')
def slow_trickle(ex):
    interval = ex.number('interval', 10, 0.05, 30)
    length = int(ex.number('length', 100, 1, 10000))
    ex.send(b'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % length)
    for i in range(length):
        if not ex.wait(interval):
            ex.abort()
            return
        ex.send(b'.' if i < length - 1 else b'\n')


@route('/slow/delay', 'a page after ?ms= milliseconds (3000)')
def slow_delay(ex):
    ms = ex.number('ms', 3000, 0, 120000)
    if not ex.wait(ms / 1000):
        ex.abort()
        return
    ex.page('A delayed page', f'<h1>A delayed page</h1>\n<p>This page waited {int(ms)} ms.</p>')


# ---- the command line ------------------------------------------------------------------------------------------------

def describe_routes():
    rows = [(p, d) for p, (_, d) in ROUTES.items()] + [(p + '*', d) for p, _, d in PREFIXES]
    return '\n'.join(f'{p:24} {d}' for p, d in sorted(rows))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('serve', help='run every server until Ctrl+C')
    s.add_argument('--dir', help='keep the test CA and certificates here (default: a temporary directory)')
    c = sub.add_parser('certs', help='only make the test CA and the certificates')
    c.add_argument('dir')
    sub.add_parser('routes', help='what each path sends')
    sub.add_parser('servers', help='what each server is for')
    a = ap.parse_args(argv)
    if a.cmd == 'routes':
        print(describe_routes())
        return 0
    if a.cmd == 'servers':
        print('\n'.join(f'{n:14} {d}' for n, _, _, _, d in SERVERS))
        return 0
    if a.cmd == 'certs':
        pki = Pki(a.dir)
        print(f'{pki.ca_file}: the roots to trust; chains: ' + ', '.join(str(p) for p, _ in pki.chains.values()))
        return 0
    with Fixtures(a.dir) as fx:
        print(f'test CA (trust these roots): {fx.ca_file}')
        for name, _, _, _, doc in SERVERS:
            print(f'  {name:14} 127.0.0.1:{fx.ports[name]:<6} {fx.url(name)}  {doc}')
        print('\nin the guest\'s /initrd/etc/undertow.conf (and the CA as /initrd/etc/ssl/cert.pem):\n')
        print(fx.undertow_conf())
        print('Ctrl+C stops the servers.')
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print('\nstopping')
    return 0


if __name__ == '__main__':
    sys.exit(main())
