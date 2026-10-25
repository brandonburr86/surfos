"""Tests for tools/web-fixtures.py (10/11/26), the web fixtures of Undertow's end-to-end tests, with Python's own
clients: raw sockets, ssl, gzip.

Run from the repository: python3 -m unittest discover -s tests -p test_web_fixtures.py   (make test-web-host)

What a fixture claims to send is checked byte for byte (framing, sizes, charsets, escape sequences), every route
and every server: the TLS versions, each certificate case (and that only its one flaw makes it fail), the
handshakes that never complete, the recording of SNI, ALPN, Host and the request line, the cleanup after an
exception, and ports that the system picks. Needs an OpenSSL 3 openssl command (the Mac's Homebrew one, the
toolchain image's); nothing connects anywhere but 127.0.0.1.
"""
import contextlib
import datetime
import gzip
import importlib.util
import io
import os
from pathlib import Path
import re
import socket
import ssl
import struct
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('web_fixtures', REPO / 'tools' / 'web-fixtures.py')
wf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wf)

FX = None               # one set of fixtures (one test CA) for the module; the cleanup tests make their own servers

OID_RSA = bytes.fromhex('06092a864886f70d010101')          # rsaEncryption
OID_EC = bytes.fromhex('06072a8648ce3d0201')               # id-ecPublicKey
OID_P256 = bytes.fromhex('06082a8648ce3d030107')           # prime256v1


def setUpModule():
    global FX
    FX = wf.Fixtures()


def tearDownModule():
    if FX:
        FX.close()


# ---- clients -----------------------------------------------------------------------------------------------------

def client_context(cafile=None, version=None, alpn=('http/1.1',)):
    ctx = ssl.create_default_context(cafile=str(cafile or FX.ca_file))
    if version:
        ctx.minimum_version = ctx.maximum_version = version
    if alpn:
        ctx.set_alpn_protocols(list(alpn))
    return ctx


def connect(server, hostname=wf.FIXTURE_HOST, ctx=None, timeout=10, fx=None):
    fx = fx or FX
    s = socket.create_connection(('127.0.0.1', fx.ports[server]), timeout=timeout)
    if fx.schemes[server] == 'https':
        try:
            s = (ctx or client_context()).wrap_socket(s, server_hostname=hostname)
        except BaseException:
            s.close()
            raise
    return s


def read_all(s, limit=4 << 20):
    data = b''
    while len(data) < limit:
        try:
            chunk = s.recv(65536)
        except (ssl.SSLEOFError, ConnectionResetError):
            break
        if not chunk:
            break
        data += chunk
    return data


def get(server, target, hostname=wf.FIXTURE_HOST, ctx=None, fx=None):
    """the raw response to GET target, read to the end of the connection"""
    fx = fx or FX
    s = connect(server, hostname, ctx, fx=fx)
    try:
        s.sendall(f'GET {target} HTTP/1.1\r\nHost: {hostname}:{fx.ports[server]}\r\nUser-Agent: test_web_fixtures\r\n'
                  f'Accept-Encoding: identity\r\nConnection: close\r\n\r\n'.encode())
        return read_all(s)
    finally:
        s.close()


def parse(raw):
    """the responses in raw, interim ones first: [(version, status, reason, [(name, value)], body, complete)]. A
    body is framed as RFC 9112 says; complete is False when the connection ended before its end"""
    out, rest = [], raw
    while rest:
        end = rest.find(b'\r\n\r\n')
        if end < 0:
            out.append((None, None, None, [], rest, False))
            return out
        lines = rest[:end].decode('latin-1').split('\r\n')
        rest = rest[end + 4:]
        version, status, reason = (lines[0].split(' ', 2) + ['', ''])[:3]
        headers = [tuple(x.strip() for x in l.split(':', 1)) for l in lines[1:]]
        status = int(status) if status.isdigit() else status
        names = {n.lower(): v for n, v in headers}
        if isinstance(status, int) and (100 <= status < 200 or status in (204, 304)):
            out.append((version, status, reason, headers, b'', True))
            if status == 101:
                return out
            continue
        if 'chunked' in names.get('transfer-encoding', ''):
            body, complete = b'', False
            while True:
                eol = rest.find(b'\r\n')
                if eol < 0:
                    body += rest
                    break
                size = int(rest[:eol].split(b';')[0], 16)
                if size == 0:
                    complete = rest.find(b'\r\n\r\n', eol) >= 0
                    break
                piece = rest[eol + 2:eol + 2 + size]
                body += piece
                if len(piece) < size:
                    break
                rest = rest[eol + 4 + size:]
            out.append((version, status, reason, headers, body, complete))
            return out
        if 'content-length' in names and names['content-length'].isdigit():
            n = int(names['content-length'])
            out.append((version, status, reason, headers, rest[:n], len(rest) >= n))
            return out
        out.append((version, status, reason, headers, rest, True))
        return out


def final(raw):
    return parse(raw)[-1]


def header(resp, name):
    values = [v for n, v in resp[3] if n.lower() == name.lower()]
    return values[0] if values else None


def last_record(mark, server=None):
    return FX.wait_for(lambda r: r['ended'] is not None and (server is None or r['server'] == server), 10, mark)


# ---- the test CA -------------------------------------------------------------------------------------------------

class PkiTests(unittest.TestCase):
    def test_roots_in_the_bundle(self):
        ders = wf.pem_blocks(FX.ca_pem)
        self.assertEqual(len(ders), 2)
        self.assertTrue(all(wf.MARKER.encode() in d for d in ders))
        self.assertTrue(wf.has_fixture_ca(FX.ca_pem))
        unknown = wf.pem_blocks(FX.pki.unknown_ca_file.read_bytes())[0]
        self.assertNotIn(unknown, ders)
        self.assertIn(OID_RSA, ders[0])
        self.assertIn(OID_EC, ders[1])

    def test_marker_absent_elsewhere(self):
        self.assertFalse(wf.has_fixture_ca(b''))
        self.assertFalse(wf.has_fixture_ca(b'no certificates here'))
        # a program holds the PEM markers as strings, with anything between them (Mbed TLS's parser does)
        program = b'\x7fELF\0-----BEGIN CERTIFICATE-----\0\x01pem\xff-----END CERTIFICATE-----\0' * 3
        self.assertFalse(wf.has_fixture_ca(program))
        self.assertEqual(wf.pem_blocks(program), [])
        self.assertTrue(wf.has_fixture_ca(program + FX.ca_pem))
        self.assertTrue(wf.has_fixture_ca(b'raw DER: ' + wf.pem_blocks(FX.ca_pem)[0]))
        system = ssl.get_default_verify_paths().cafile
        if system and os.path.exists(system):
            self.assertFalse(wf.has_fixture_ca(Path(system).read_bytes()), system)

    def test_dates_relative_to_the_run(self):
        now = datetime.datetime.now(datetime.timezone.utc)
        d = FX.pki.dates
        for good in ('rsa', 'ecdsa', 'wrong-name', 'unknown-ca'):
            self.assertLess(d[good][0], now - datetime.timedelta(minutes=50), good)
            self.assertGreater(d[good][1], now + datetime.timedelta(days=60), good)
        self.assertLess(d['expired'][1], now - datetime.timedelta(days=29))
        self.assertGreater(d['not-yet-valid'][0], now + datetime.timedelta(days=29))
        self.assertGreater(d['rsa-root'][1].year, 2038)              # past 32-bit time

    def test_key_types(self):
        for name, oid in (('rsa', OID_RSA), ('rsa-ca', OID_RSA), ('ecdsa', OID_EC), ('ecdsa-ca', OID_EC)):
            der = wf.pem_blocks(FX.pki.paths[name].read_bytes())[0]
            self.assertIn(oid, der, name)
        self.assertIn(OID_P256, wf.pem_blocks(FX.pki.paths['ecdsa'].read_bytes())[0])

    def test_chain_files(self):
        self.assertEqual(len(wf.pem_blocks(FX.pki.chain('rsa').read_bytes())), 2)
        self.assertEqual(len(wf.pem_blocks(FX.pki.chain('ecdsa').read_bytes())), 2)
        self.assertEqual(len(wf.pem_blocks(FX.pki.chain('no-chain').read_bytes())), 1)


# ---- TLS -----------------------------------------------------------------------------------------------------------

class TlsTests(unittest.TestCase):
    def handshake(self, server, hostname=wf.FIXTURE_HOST, ctx=None):
        s = connect(server, hostname, ctx)
        try:
            return s.version(), s.selected_alpn_protocol(), s.getpeercert(binary_form=True)
        finally:
            s.close()

    def refused_with(self, server, code, hostname=wf.FIXTURE_HOST, ctx=None):
        mark = FX.mark()
        with self.assertRaises(ssl.SSLCertVerificationError) as cm:
            connect(server, hostname, ctx).close()
        self.assertEqual(cm.exception.verify_code, code, cm.exception)
        rec = last_record(mark, server)
        self.assertIsNotNone(rec['handshake_error'], rec)          # the client's alert: no request ever came
        self.assertIsNone(rec['request_line'])

    def test_versions(self):
        self.assertEqual(self.handshake('https')[0], 'TLSv1.3')
        self.assertEqual(self.handshake('https', ctx=client_context(version=ssl.TLSVersion.TLSv1_2))[0], 'TLSv1.2')
        self.assertEqual(self.handshake('tls12')[0], 'TLSv1.2')
        self.assertEqual(self.handshake('tls13')[0], 'TLSv1.3')
        with self.assertRaises(ssl.SSLError):
            connect('tls12', ctx=client_context(version=ssl.TLSVersion.TLSv1_3)).close()
        with self.assertRaises(ssl.SSLError):
            connect('tls13', ctx=client_context(version=ssl.TLSVersion.TLSv1_2)).close()

    def test_names_and_address(self):
        for name in (wf.FIXTURE_HOST, wf.WWW_HOST, wf.GUEST_HOST):
            self.assertEqual(self.handshake('https', name)[1], 'http/1.1', name)

    def test_ecdsa_chain(self):
        version, alpn, der = self.handshake('https-ecdsa')
        self.assertIn(OID_EC, der)
        self.assertIn(OID_P256, der)
        self.assertIn(OID_RSA, self.handshake('https')[2])

    def test_wrong_name(self):
        self.refused_with('wrong-name', 62)                         # X509_V_ERR_HOSTNAME_MISMATCH
        self.handshake('wrong-name', wf.WRONG_HOST)                 # its own name: nothing else is wrong

    def test_expired_and_not_yet_valid(self):
        self.refused_with('expired', 10)                            # X509_V_ERR_CERT_HAS_EXPIRED
        self.refused_with('not-yet-valid', 9)                       # X509_V_ERR_CERT_NOT_YET_VALID

    def test_unknown_ca(self):
        self.refused_with('unknown-ca', 20)                         # unable to get local issuer certificate
        self.handshake('unknown-ca', ctx=client_context(cafile=FX.pki.unknown_ca_file))

    def test_no_chain(self):
        self.refused_with('no-chain', 20)
        both = FX.dir / 'root-and-intermediate.pem'
        both.write_bytes(FX.ca_pem + FX.pki.paths['rsa-ca'].read_bytes())
        self.handshake('no-chain', ctx=client_context(cafile=both))   # only the intermediate was missing

    def test_garbage(self):
        mark = FX.mark()
        with self.assertRaises(ssl.SSLError):
            connect('garbage').close()
        rec = last_record(mark, 'garbage')
        self.assertEqual(rec['received'][:1], b'\x16')                # a TLS handshake record came in
        s = socket.create_connection(('127.0.0.1', FX.ports['garbage']), timeout=10)
        s.sendall(b'\x16\x03\x01\x00\x00')
        self.assertEqual(read_all(s), wf.GARBAGE)
        s.close()

    def test_hangup(self):
        with self.assertRaises((ssl.SSLError, ConnectionResetError, OSError)):
            connect('hangup').close()

    def test_stall(self):
        mark = FX.mark()
        with self.assertRaises((socket.timeout, TimeoutError)):
            connect('tls-stall', timeout=0.5).close()
        rec = last_record(mark, 'tls-stall')
        self.assertTrue(rec['client_closed'])
        self.assertEqual(rec['received'][:1], b'\x16')

    def test_refused(self):
        with self.assertRaises(ConnectionRefusedError):
            socket.create_connection(('127.0.0.1', FX.ports['refused']), timeout=5)

    def test_recording(self):
        mark = FX.mark()
        resp = final(get('https', '/about?x=1'))
        self.assertEqual(resp[1], 200)
        rec = last_record(mark, 'https')
        self.assertEqual(rec['sni'], wf.FIXTURE_HOST)
        self.assertEqual(rec['tls'], 'TLSv1.3')
        self.assertEqual(rec['alpn'], 'http/1.1')
        self.assertEqual(rec['request_line'], 'GET /about?x=1 HTTP/1.1')
        self.assertEqual(rec['target'], '/about?x=1')
        self.assertEqual(rec['host'], f"{wf.FIXTURE_HOST}:{FX.ports['https']}")
        self.assertIn(('User-Agent', 'test_web_fixtures'), rec['headers'])
        self.assertEqual(rec['route'], '/about')
        self.assertGreater(rec['bytes_sent'], 0)
        self.assertEqual(FX.records(server='https', path='/about', since=mark)[0]['request_line'], rec['request_line'])

    def test_recording_without_sni_or_alpn(self):
        mark = FX.mark()
        s = connect('https', wf.GUEST_HOST, client_context(alpn=()))   # Python sends no SNI for an address
        s.close()
        rec = last_record(mark, 'https')
        self.assertIsNone(rec['sni'])
        self.assertIsNone(rec['alpn'])

    def test_plain_http_recording(self):
        mark = FX.mark()
        get('http', '/')
        rec = last_record(mark, 'http')
        self.assertEqual((rec['scheme'], rec['tls'], rec['sni']), ('http', None, None))
        self.assertEqual(rec['request_line'], 'GET / HTTP/1.1')


# ---- routes ----------------------------------------------------------------------------------------------------------

class PageTests(unittest.TestCase):
    def page(self, target, server='http'):
        resp = final(get(server, target))
        self.assertEqual(resp[1], 200, target)
        self.assertTrue(resp[5], target)
        return resp

    def test_home_and_links(self):
        for server, other in (('http', 'https'), ('https', 'http')):
            body = self.page('/', server)[4].decode()
            self.assertIn('<title>Undertow Fixture Home</title>', body)
            for href in ('/about', 'docs/', '/long', '/anchors#section-2', FX.url(other, '/'), 'mailto:'):
                self.assertIn(f'href="{href}', body)

    def test_documents(self):
        resp = final(get('http', '/docs'))
        self.assertEqual((resp[1], header(resp, 'Location')), (301, '/docs/'))
        body = self.page('/docs/')[4].decode()
        for href in ('"guide"', '"./guide?part=2"', '"../about"', '"?sort=name"', f'"//fixture.test:{FX.ports["http"]}/about"',
                     '"#bottom"'):
            self.assertIn('href=' + href, body)
        self.assertIn('id="bottom"', body)
        self.assertIn('This is part 2 of the guide.', self.page('/docs/guide?part=2')[4].decode())
        self.assertIn('This is part 1 of the guide.', self.page('/docs/guide?part=<x>')[4].decode())

    def test_long(self):
        body = self.page('/long')[4].decode()
        lines = re.findall(r'^(?:<pre>)?Line (\d{3}): (.*)$', body, re.M)
        self.assertEqual(len(lines), wf.LONG_LINES)
        needles = [int(n) for n, text in lines if 'needle' in text.lower()]
        self.assertEqual(needles, [120, 240, 360])

    def test_anchors(self):
        body = self.page('/anchors')[4].decode()
        for target in ('id="section-1"', 'id="section-2"', 'name="section-3"', 'id="section-4"', 'href="#nowhere"',
                       'href="/anchors?again=1#section-4"'):
            self.assertIn(target, body)
        self.assertEqual(body.count('id="section-1"'), 2)
        self.assertEqual(body.count('Text of section 3'), 12)

    def test_byte_cap_url_length_ignores_port_width(self):
        for port in (1, 8080, 65535):
            base = f'http://fixture.test:{port}'
            path = wf.bytecap_path(base)
            self.assertEqual(len(base + path), 2000)
            self.assertLessEqual(len(base + path + '2048'), 2048)

    def test_byte_cap(self):
        """/bytecap/<2000 bytes>/ has 2048 relative links, each an address of about 2000 bytes once resolved: past
        UT_LINK_BYTES_MAX long before the 2048th; its links lead to a small page each"""
        path = wf.bytecap_path(FX.url('http', ''))
        self.assertEqual(path[-1], '/')
        self.assertGreater(len(FX.url('http', path)), 1900)
        self.assertLessEqual(len(FX.url('http', path)) + 4, 2048)
        body = self.page(path)[4].decode()
        hrefs = re.findall(r'href="(\d+)"', body)
        self.assertEqual(hrefs, [str(i) for i in range(1, 2049)])
        resolved = sum(len(FX.url('http', path + h)) for h in hrefs)
        self.assertGreater(resolved, 3 * wf.LINK_BYTES_MAX)
        self.assertLess(len(body), 100000)
        self.assertIn('This is target 600 of the byte cap.', self.page(path + '600')[4].decode())

    def test_links(self):
        for n in (1, 256, 257, 2048, 2049):
            body = self.page(f'/links/{n}')[4].decode()
            self.assertEqual(body.count('<a href='), n)
            self.assertIn(f'href="/links/target?n={n}">link {n}</a>', body)
        self.assertIn('You followed link 2049.', self.page('/links/target?n=2049')[4].decode())
        for bad in ('/links/0', '/links/3001', '/links/x'):
            self.assertEqual(final(get('http', bad))[1], 404)

    def test_charsets(self):
        r = self.page('/windows-1252')
        self.assertEqual(header(r, 'Content-Type'), 'text/html; charset=windows-1252')
        self.assertIn(b'\x93Smart quotes\x94, caf\xe9, \x805', r[4])
        r = self.page('/latin1')
        self.assertEqual(header(r, 'Content-Type'), 'text/html')
        self.assertIn(b'charset=iso-8859-1', r[4][:1024])
        self.assertIn(b'caf\xe9, na\xefve', r[4])
        r = self.page('/shift_jis')
        self.assertEqual(header(r, 'Content-Type'), 'text/html; charset=Shift_JIS')
        self.assertIn('\u65e5\u672c\u8a9e', r[4].decode('shift_jis'))
        self.assertIn(b'ASCII survives', r[4])
        r = self.page('/utf8')
        self.assertIn('Gr\u00fc\u00dfe aus Z\u00fcrich', r[4].decode())

    def test_media(self):
        r = self.page('/image.png')
        self.assertEqual(header(r, 'Content-Type'), 'image/png')
        self.assertEqual(r[4][:8], b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>II', r[4][16:24]), (1, 1))
        r = self.page('/gzip')
        self.assertEqual(header(r, 'Content-Encoding'), 'gzip')
        self.assertIn(b'This was compressed.', gzip.decompress(r[4]))
        for target in ('/binary', '/no-type'):
            self.assertIsNone(header(self.page(target), 'Content-Type'), target)
        self.assertIn(b'\x00', self.page('/binary')[4])
        self.assertNotIn(b'\x00', self.page('/no-type')[4])
        self.assertEqual(header(self.page('/octet'), 'Content-Type'), 'application/octet-stream')
        self.assertEqual(header(self.page('/xhtml'), 'Content-Type'), 'application/xhtml+xml')
        self.assertEqual(header(self.page('/plain.txt'), 'Content-Type'), 'text/plain; charset=utf-8')

    def test_forms_and_scripts(self):
        self.assertIn(b'<textarea', self.page('/forms')[4])
        body = self.page('/scripts')[4]
        for text in (b'UT-SCRIPT-RAN', b'UT-STYLE-TEXT', b'UT-TEMPLATE-TEXT', b'Shown without scripts.'):
            self.assertIn(text, body)

    def test_escape(self):
        body = self.page('/escape')[4]
        for b in wf.ESCAPE_BYTES:                       # the harness's raw checks look for these: they must be sent
            self.assertIn(b, body)
        self.assertIn(wf.ESCAPE_TITLE.encode(), body)
        self.assertIn(b'\x00', body)
        raw = get('http', '/escape/reason')
        self.assertTrue(raw.startswith(b'HTTP/1.1 404 Not\x1b[2JFound\x07\r\n'))
        self.assertIn(b'X-Note: \x1b[6n', raw)

    def test_status(self):
        for code in (404, 500, 418):
            resp = final(get('http', f'/status/{code}'))
            self.assertEqual(resp[1], code)
            self.assertIn(f'status {code}'.encode(), resp[4])
        for code in (204, 304):
            raw = get('http', f'/status/{code}')
            self.assertTrue(raw.startswith(f'HTTP/1.1 {code} '.encode()), raw)
            self.assertTrue(raw.endswith(b'\r\n\r\n'))
        resp = final(get('http', '/missing<b>\x1b'.replace('\x1b', '%1b')))
        self.assertEqual(resp[1], 404)
        self.assertIn(b'/missing&lt;b&gt;%1b', resp[4])

    def test_counter_and_once(self):
        first = int(re.search(rb'fetched (\d+) times', self.page('/counter')[4])[1])
        self.assertEqual(int(re.search(rb'fetched (\d+) times', self.page('/counter')[4])[1]), first + 1)
        key = f'/once?id=t{time.monotonic_ns()}'
        self.assertIn(b'first and only good fetch', self.page(key)[4])
        again = final(get('http', key))
        self.assertFalse(again[5])                       # the second fetch ends early

    def test_plumbing(self):
        self.assertEqual(self.page('/plumbing')[4], b'SurfOS web fixture: plumbing ok\n')

    def test_other_methods(self):
        s = connect('http')
        s.sendall(b'POST / HTTP/1.1\r\nHost: fixture.test\r\nContent-Length: 0\r\n\r\n')
        self.assertEqual(final(read_all(s))[1], 405)
        s.close()


class EveryRouteTests(unittest.TestCase):
    SAMPLES = {'/links/': '/links/3', '/status/': '/status/410', '/redirect/code/': '/redirect/code/307',
               '/redirect/': '/redirect/2', '/bytecap/': '/bytecap/abc/', }

    def test_every_route_answers(self):
        """every path of ROUTES (the slow ones have their own tests) sends a response, or the bytes it says it sends
        instead; the parameterised ones with a sample"""
        targets = [p for p in wf.ROUTES if not p.startswith('/slow/')] + [self.SAMPLES[p] for p, _, _ in wf.PREFIXES]
        self.assertEqual(len(wf.PREFIXES), len(self.SAMPLES))
        for target in targets:
            for server in ('http', 'https'):
                raw = get(server, target)
                ok = raw.startswith((b'HTTP/1.1 ', b'HTTP/1.0 ')) or target == '/frame/no-status-line'
                self.assertTrue(ok and raw, f'{server} {target}: {raw[:60]!r}')


class FramingTests(unittest.TestCase):
    def test_length(self):
        for server in ('http', 'https'):
            r = final(get(server, '/frame/length'))
            self.assertEqual((r[1], r[4], r[5]), (200, wf.text_body('Content-Length framing'), True))

    def test_lengths_refused(self):
        r = final(get('http', '/frame/length-twice'))
        lengths = [v for n, v in r[3] if n == 'Content-Length']
        self.assertEqual(len(lengths), 2)
        self.assertEqual(len(set(lengths)), 1)
        r = final(get('http', '/frame/length-conflict'))
        self.assertEqual(len({v for n, v in r[3] if n == 'Content-Length'}), 2)
        for target, value in (('/frame/length-bad', '12abc'), ('/frame/length-negative', '-1'),
                              ('/frame/length-20-digits', '18446744073709551617')):
            self.assertEqual([v for n, v in final(get('http', target))[3] if n == 'Content-Length'], [value])

    def test_chunked(self):
        for server in ('http', 'https'):
            raw = get(server, '/frame/chunked')
            r = final(raw)
            self.assertEqual((r[1], r[4], r[5]), (200, wf.CHUNKED_BODY, True))
            for part in (b'19;name=value\r\n', b';quoted="a;b"\r\n', b';flag\r\n', b'0;last=yes\r\n',
                         b'X-Fixture-Trailer: present\r\n\r\n'):
                self.assertIn(part, raw)

    def test_chunked_refused(self):
        self.assertIn(b'\r\n\r\nffffffffffffffff\r\n', get('http', '/frame/chunked-huge'))
        r = final(get('http', '/frame/chunked-and-length'))
        self.assertEqual((header(r, 'Transfer-Encoding'), header(r, 'Content-Length')), ('chunked', '10'))
        r = final(get('http', '/frame/te-gzip'))
        self.assertEqual(header(r, 'Transfer-Encoding'), 'gzip, chunked')
        self.assertEqual(gzip.decompress(r[4]), wf.CHUNKED_BODY)

    def test_close_delimited(self):
        for target, version in (('/frame/close', 'HTTP/1.1'), ('/frame/http10', 'HTTP/1.0')):
            for server in ('http', 'https'):
                r = final(get(server, target))
                self.assertEqual(r[0], version)
                self.assertIsNone(header(r, 'Content-Length'))
                self.assertIsNone(header(r, 'Transfer-Encoding'))
                self.assertTrue(r[4].endswith(b'the body arrived whole.\n'))

    def test_close_notify(self):
        """10/25/26: over TLS a whole response ends with close_notify; /frame/close-cut's (a close-delimited body)
        with the TCP connection's end alone"""
        for target, clean in (('/frame/close', True), ('/frame/close-cut', False)):
            ctx = client_context()
            ctx.options &= ~getattr(ssl, 'OP_IGNORE_UNEXPECTED_EOF', 0)     # Python 3.11 sets it (3.14 does not):
            s = socket.create_connection(('127.0.0.1', FX.ports['https']), timeout=10)     # an end is an end then
            s = ctx.wrap_socket(s, server_hostname=wf.FIXTURE_HOST, suppress_ragged_eofs=False)
            try:
                s.sendall(f'GET {target} HTTP/1.1\r\nHost: {wf.FIXTURE_HOST}\r\nConnection: close\r\n\r\n'.encode())
                data, ended = b'', None
                while ended is None:
                    try:
                        chunk = s.recv(65536)
                    except ssl.SSLError as e:       # 3.14: SSLEOFError; 3.11: an SSLError of that reason
                        if not isinstance(e, ssl.SSLEOFError) and e.reason != 'UNEXPECTED_EOF_WHILE_READING':
                            raise
                        ended = False
                        continue
                    data += chunk
                    if not chunk:
                        ended = True
                self.assertEqual(ended, clean, target)
                self.assertIsNone(header(final(data), 'Content-Length'), target)
            finally:
                s.close()

    def test_interim(self):
        for target, n in (('/frame/interim', 2), ('/frame/interim-8', 8), ('/frame/interim-9', 9)):
            rs = parse(get('http', target))
            self.assertEqual([r[1] for r in rs], [100] + [103] * (n - 1) + [200], target)
            self.assertTrue(rs[-1][5])
        self.assertEqual([r[1] for r in parse(get('http', '/frame/101'))], [101])

    def test_early_eof(self):
        for server in ('http', 'https'):
            r = final(get(server, '/frame/early-eof'))
            self.assertEqual((header(r, 'Content-Length'), len(r[4]), r[5]), ('1000', 100, False))
            r = final(get(server, '/frame/early-eof-chunked'))
            self.assertEqual((len(r[4]), r[5]), (50, False))
            self.assertEqual(get(server, '/frame/early-eof-head'), b'HTTP/1.1 200 OK\r\nContent-Type: text/pl')

    def test_malformed(self):
        self.assertIn(b'X-Folded: first part\r\n  second part\r\n', get('http', '/frame/folded'))
        raw = get('http', '/frame/huge-headers')
        self.assertGreater(raw.find(b'\r\n\r\n'), wf.HEADERS_MAX)
        self.assertTrue(get('http', '/frame/bad-status').startswith(b'HTTP/1.1 2OO OK\r\n'))
        self.assertTrue(get('http', '/frame/no-status-line').startswith(b'<html>'))


class SplitAndLongTests(unittest.TestCase):
    def test_split(self):
        """/split arrives one byte per write: over plain TCP the reads come in pieces of a few bytes at most
        (TCP_NODELAY), and the bytes, put together, are a chunked response with extensions"""
        for server in ('http', 'https'):
            s = connect(server)
            s.sendall(b'GET /split HTTP/1.1\r\nHost: fixture.test\r\nConnection: close\r\n\r\n')
            pieces = []
            while True:
                try:
                    chunk = s.recv(65536)
                except (ssl.SSLEOFError, ConnectionResetError):
                    break
                if not chunk:
                    break
                pieces.append(chunk)
            s.close()
            raw = b''.join(pieces)
            r = final(raw)
            self.assertEqual((r[1], header(r, 'Transfer-Encoding'), r[5]), (200, 'chunked', True), server)
            self.assertIn(b';x=1\r\n', raw)
            self.assertIn('caf&eacute; &#x2014; &amp; &#8364;5 \u65e5\u672c', r[4].decode())
            self.assertGreater(len(pieces), len(raw) // 8, server)      # not one big read: it came apart

    def test_long_fields(self):
        r = final(get('http', '/long-fields'))
        body = r[4].decode()
        title = re.search(r'<title>(.*)</title>', body, re.S).group(1)
        self.assertGreater(len(title), 3000)
        self.assertTrue(title.startswith('UT-LONG-TITLE-START') and title.endswith('UT-LONG-TITLE-END'))
        for b in wf.LONG_ESCAPES:
            self.assertIn(b.decode('latin-1'), title)
        hrefs = re.findall(r'href="([^"]*)"', body)
        self.assertEqual(len(FX.url('http', hrefs[0])), 2000)
        self.assertGreater(len(FX.url('http', hrefs[1])), 2048)
        self.assertIn('UT-WORD-START' + 'w' * 5000 + 'UT-WORD-END', body)


class SizeTests(unittest.TestCase):
    def test_sizes(self):
        for target, n, framing in (('/size/1m', wf.BODY_MAX, 'Content-Length'),
                                   ('/size/1m+1', wf.BODY_MAX + 1, 'Content-Length'),
                                   ('/size/1m-chunked', wf.BODY_MAX, 'Transfer-Encoding'),
                                   ('/size/1m+1-chunked', wf.BODY_MAX + 1, 'Transfer-Encoding'),
                                   ('/size/1m+1-close', wf.BODY_MAX + 1, None)):
            r = final(get('http', target))
            self.assertEqual((len(r[4]), r[5]), (n, True), target)
            if framing:
                self.assertIsNotNone(header(r, framing), target)
            else:
                self.assertIsNone(header(r, 'Content-Length'))
                self.assertIsNone(header(r, 'Transfer-Encoding'))
        r = final(get('https', '/size/1m'))
        self.assertEqual(len(r[4]), wf.BODY_MAX)


class RedirectTests(unittest.TestCase):
    def follow(self, target, server='http', limit=20):
        """the chain from target: [(status, Location)] until a response that is not a redirect"""
        chain = []
        while len(chain) < limit:
            r = final(get(server, target))
            location = header(r, 'Location')
            chain.append((r[1], location))
            if r[1] not in (301, 302, 303, 307, 308) or not location or not location.startswith('/'):
                return chain
            target = location
        return chain

    def test_chains(self):
        for n in (0, 5, 6):
            chain = self.follow(f'/redirect/{n}')
            self.assertEqual(len(chain), n + 1)
            self.assertEqual(chain[-1], (200, None))
        self.assertIn(b'Every redirect was followed.', final(get('http', '/redirect/0'))[4])

    def test_loop(self):
        chain = self.follow('/redirect/loop-a', limit=4)
        self.assertEqual([c[1] for c in chain], ['/redirect/loop-b', '/redirect/loop-a'] * 2)

    def test_codes_and_kinds(self):
        for code in (301, 302, 303, 307, 308):
            r = final(get('http', f'/redirect/code/{code}'))
            self.assertEqual((r[1], header(r, 'Location')), (code, '/about'))
        self.assertEqual(final(get('http', '/redirect/code/300'))[1], 404)
        cases = {'/redirect/relative': '../about', '/redirect/to-http': FX.url('http', '/about'),
                 '/redirect/to-https': FX.url('https', '/about'),
                 '/redirect/to-www': FX.url('https', '/about', host=wf.WWW_HOST),
                 '/redirect/fragment': '/anchors', '/redirect/with-fragment': '/anchors#section-4'}
        for target, location in cases.items():
            self.assertEqual(header(final(get('https', target)), 'Location'), location, target)
        self.assertEqual(header(final(get('http', '/redirect/space')), 'Location'), '/space here')
        self.assertIn(b'arrived as %20', final(get('http', '/space%20here'))[4])
        r = final(get('http', '/status/300'))
        self.assertEqual((r[1], r[2], header(r, 'Location')), (300, 'Multiple Choices', None))
        r = final(get('http', '/redirect/no-location'))
        self.assertEqual((r[1], header(r, 'Location')), (302, None))


class SlowTests(unittest.TestCase):
    def test_stall(self):
        mark = FX.mark()
        s = connect('http')
        s.sendall(b'GET /slow/stall HTTP/1.1\r\nHost: fixture.test\r\n\r\n')
        s.settimeout(0.5)
        got = b''
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                got += chunk
        except socket.timeout:
            pass
        self.assertTrue(got.endswith(b'\r\n\r\n0123456789'), got)
        s.close()
        rec = last_record(mark, 'http')
        self.assertTrue(rec['client_closed'])

    def test_stall_head(self):
        s = connect('https')
        s.sendall(b'GET /slow/stall-head HTTP/1.1\r\nHost: fixture.test\r\n\r\n')
        s.settimeout(0.5)
        with self.assertRaises((socket.timeout, TimeoutError)):
            s.recv(1)
        s.close()

    def test_trickle(self):
        start = time.monotonic()
        r = final(get('http', '/slow/trickle?interval=0.05&length=6'))
        self.assertEqual((r[4], r[5]), (b'.....\n', True))
        self.assertGreater(time.monotonic() - start, 0.25)

    def test_trickle_client_leaves(self):
        mark = FX.mark()
        s = connect('https')
        s.sendall(b'GET /slow/trickle?interval=0.05&length=1000 HTTP/1.1\r\nHost: fixture.test\r\n\r\n')
        time.sleep(0.3)
        s.close()
        rec = last_record(mark, 'https')
        self.assertTrue(rec['client_closed'])
        self.assertLess(rec['bytes_sent'], 400)

    def test_delay(self):
        start = time.monotonic()
        r = final(get('http', '/slow/delay?ms=200'))
        self.assertIn(b'waited 200 ms', r[4])
        self.assertGreater(time.monotonic() - start, 0.15)


# ---- the servers' life -----------------------------------------------------------------------------------------------

class LifecycleTests(unittest.TestCase):
    def test_ports_are_the_systems(self):
        with wf.Fixtures(pki=FX.pki) as other:
            ports = list(FX.ports.values()) + list(other.ports.values())
            self.assertEqual(len(set(ports)), len(ports))
            self.assertTrue(all(p > 0 for p in ports))
            self.assertEqual(final(get('http', '/plumbing', fx=other))[1], 200)
            for s in other._listeners:
                self.assertEqual(s.getsockname()[0], '127.0.0.1')

    def test_cleanup_after_an_exception(self):
        holder = {}
        with self.assertRaises(RuntimeError):
            with wf.Fixtures(pki=FX.pki) as fx:
                holder['fx'] = fx
                s = connect('http', fx=fx)
                s.sendall(b'GET /slow/stall HTTP/1.1\r\nHost: fixture.test\r\n\r\n')
                fx.wait_for(lambda r: r['route'] == '/slow/stall')
                holder['sock'] = s
                start = time.monotonic()
                raise RuntimeError('a test failed in the middle')
        fx, s = holder['fx'], holder['sock']
        self.assertLess(time.monotonic() - start, 5)
        self.assertEqual(fx.threads_alive(), [])
        s.settimeout(5)
        self.assertTrue(read_all(s).endswith(b'0123456789'))        # the held connection ended
        s.close()
        for name in ('http', 'https', 'tls-stall'):
            with self.assertRaises(OSError):
                socket.create_connection(('127.0.0.1', fx.ports[name]), timeout=2).close()

    def test_cleanup_when_starting_fails(self):
        started = []
        real = wf.Fixtures._start

        def start(self, name, *args):
            if len(started) == 3:
                raise OSError('no more ports')
            started.append(name)
            real(self, name, *args)
            started[-1] = (name, self.ports[name])
        wf.Fixtures._start = start
        try:
            with self.assertRaises(OSError):
                wf.Fixtures(pki=FX.pki)
        finally:
            wf.Fixtures._start = real
        for name, port in started:
            with self.assertRaises(OSError):
                socket.create_connection(('127.0.0.1', port), timeout=2).close()

    def test_a_failing_route(self):
        real = wf.ROUTES['/about']
        wf.ROUTES['/about'] = (lambda ex: 1 / 0, real[1])
        try:
            mark = FX.mark()
            self.assertEqual(get('http', '/about'), b'')
            self.assertIn('ZeroDivisionError', last_record(mark, 'http')['error'])
        finally:
            wf.ROUTES['/about'] = real
        self.assertEqual(final(get('http', '/about'))[1], 200)

    def test_log_and_helpers(self):
        self.assertEqual(FX.url('tls13', '/long'), f"https://fixture.test:{FX.ports['tls13']}/long")
        conf = FX.undertow_conf()
        self.assertIn('\nrtc_offset_minutes=0\n', conf)
        self.assertIn('\nhost=fixture.test 10.0.2.2\nhost=www.fixture.test 10.0.2.2\n', conf)
        self.assertIn('rtc_offset_minutes=-300', FX.undertow_conf(-300))
        path = FX.dir / 'log.jsonl'
        get('http', '/plumbing')
        FX.dump_log(path)
        self.assertIn('"/plumbing"', path.read_text())

    def test_describe(self):
        self.assertIn('/frame/chunked', wf.describe_routes())
        self.assertIn('/redirect/*', wf.describe_routes())
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(wf.main(['servers']), 0)
        self.assertIn('tls-stall', out.getvalue())


if __name__ == '__main__':
    unittest.main()
