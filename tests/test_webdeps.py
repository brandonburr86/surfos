"""Tests for tools/fetch-webdeps.py (10/11/26): Undertow's pinned Mbed TLS, checked and extracted safely.

Run from the repository: python3 -m unittest discover -s tests -p test_webdeps.py   (make test-webdeps)

Nothing here reaches the Internet: the archives are made by the tests, the one download is from a server on
127.0.0.1 that a test starts, and an address where nothing listens stands for having no network.
"""
import contextlib
import hashlib
import http.server
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import tarfile
import tempfile
import threading
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fetch_webdeps', REPO / 'tools' / 'fetch-webdeps.py')
fw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fw)

ROOT = 'mbedtls-9.9.9'
FILES = {
    'LICENSE': b'Apache-2.0 OR GPL-2.0-or-later\n',
    'include/mbedtls/build_info.h': b'/* build_info */\n',
    'include/psa/crypto.h': b'/* psa */\n',
    'library/ssl_tls.c': b'int ssl_tls;\n',
    'library/aes.c': b'int aes;\n',
    'programs/hello.c': b'int main(void) { return 0; }\n',
    'README.md': b'not wanted\n',
}


def unused_port():
    """a port on 127.0.0.1 where nothing listens (the network that is not there)"""
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def make_archive(path, files=FILES, root=ROOT, extra=None):
    """a .tar.bz2 shaped like the release: a top directory, the files under it, then extra(tar) if given"""
    with tarfile.open(path, 'w:bz2') as tar:
        d = tarfile.TarInfo(root)
        d.type = tarfile.DIRTYPE
        d.mode = 0o755
        tar.addfile(d)
        for name, data in files.items():
            info = tarfile.TarInfo(root + '/' + name)
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 1783000000
            tar.addfile(info, io.BytesIO(data))
        if extra:
            extra(tar)
    return sha256(path)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class WebdepsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos webdeps ')
        self.addCleanup(self.tmp.cleanup)
        self.top = Path(self.tmp.name)
        self.tp = self.top / 'third_party'
        self.tp.mkdir()
        self.lock = self.tp / 'undertow-lock.json'
        self.dest = self.tp / 'mbedtls'
        self.cache = self.tp / 'cache'
        self.archive = self.top / 'mbedtls-9.9.9.tar.bz2'
        self.sha = make_archive(self.archive)
        self.port = unused_port()
        self.write_lock(self.sha, self.archive.stat().st_size)

    def write_lock(self, sha, size, url=None, ca=None):
        entry = {'version': '9.9.9', 'url': url or f'http://127.0.0.1:{self.port}/mbedtls-9.9.9.tar.bz2',
                 'sha256': sha, 'size': size, 'root': ROOT, 'extract': ['LICENSE', 'include/', 'library/']}
        lock = {'mbedtls': entry}
        if ca:
            lock['ca_bundle'] = ca
        self.lock.write_text(json.dumps(lock))

    def run_tool(self, *args):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            status = fw.main(['--lock', str(self.lock), '--dest', str(self.dest), '--cache', str(self.cache), *args])
        return status, err.getvalue()

    def tree(self):
        """every file under dest (relative path: bytes), the stamp left out"""
        out = {}
        for path in self.dest.rglob('*'):
            if path.is_file() and path.name != fw.STAMP:
                out[path.relative_to(self.dest).as_posix()] = path.read_bytes()
        return out

    def leftovers(self):
        return sorted(p.name for p in self.tp.iterdir() if p.name.startswith('.mbedtls.'))

    def extract_good(self):
        status, err = self.run_tool('--archive', str(self.archive))
        self.assertEqual(status, 0, err)
        return self.tree()

    # ---- extraction ----

    def test_extracts_only_what_the_build_needs(self):
        tree = self.extract_good()
        self.assertEqual(sorted(tree), ['LICENSE', 'include/mbedtls/build_info.h', 'include/psa/crypto.h',
                                        'library/aes.c', 'library/ssl_tls.c'])
        self.assertEqual(tree['library/aes.c'], FILES['library/aes.c'])
        stamp = json.loads((self.dest / fw.STAMP).read_text())
        self.assertEqual(stamp['lock']['sha256'], self.sha)
        self.assertEqual(stamp['files']['LICENSE'], hashlib.sha256(FILES['LICENSE']).hexdigest())
        self.assertEqual((self.dest / 'library/aes.c').stat().st_mode & 0o777, 0o644)
        self.assertEqual(self.leftovers(), [])

    def test_a_current_tree_is_left_alone(self):
        self.extract_good()
        stamp = self.dest / fw.STAMP
        os.utime(self.lock, (time.time() - 100, time.time() - 100))
        before = stamp.stat().st_mtime_ns
        status, err = self.run_tool('--offline')
        self.assertEqual(status, 0, err)
        self.assertIn('ok', err)
        self.assertEqual(stamp.stat().st_mtime_ns, before, 'a rebuild for nothing: the stamp must not move')

    def test_a_newer_lock_only_touches_the_stamp(self):
        self.extract_good()
        stamp = self.dest / fw.STAMP
        os.utime(stamp, (time.time() - 100, time.time() - 100))
        inode = (self.dest / 'library/aes.c').stat().st_ino
        status, err = self.run_tool('--offline')
        self.assertEqual(status, 0, err)
        self.assertGreaterEqual(stamp.stat().st_mtime, self.lock.stat().st_mtime)
        self.assertEqual((self.dest / 'library/aes.c').stat().st_ino, inode)

    def test_a_missing_file_extracts_again(self):
        self.extract_good()
        (self.dest / 'library/aes.c').unlink()
        status, err = self.run_tool('--archive', str(self.archive))
        self.assertEqual(status, 0, err)
        self.assertEqual(self.tree()['library/aes.c'], FILES['library/aes.c'])

    # ---- checksums ----

    def test_checksum_mismatch_keeps_the_good_tree(self):
        good = self.extract_good()
        other = self.top / 'other.tar.bz2'
        files = dict(FILES)
        files['library/aes.c'] = b'int evil;\n'
        make_archive(other, files)
        self.write_lock('0' * 64, other.stat().st_size)     # a new pin that no archive here matches
        status, err = self.run_tool('--archive', str(other))
        self.assertEqual(status, 1)
        self.assertIn('is not the pinned', err)
        self.assertEqual(self.tree(), good)
        self.assertEqual(json.loads((self.dest / fw.STAMP).read_text())['lock']['sha256'], self.sha)
        self.assertTrue(other.exists(), 'the user\'s own file is never deleted')

    def test_a_cached_archive_that_does_not_match_is_deleted(self):
        self.cache.mkdir()
        bad = self.cache / 'mbedtls-9.9.9.tar.bz2'
        bad.write_bytes(b'not the archive')
        status, err = self.run_tool()
        self.assertEqual(status, 1)
        self.assertIn('does not match the lock', err)
        self.assertFalse(bad.exists())
        self.assertFalse(self.dest.exists())

    def test_a_download_that_does_not_match_is_deleted(self):
        served = self.top / 'served'
        served.mkdir()
        (served / 'mbedtls-9.9.9.tar.bz2').write_bytes(b'something else entirely')
        with self.server(served) as port:
            self.write_lock(self.sha, self.archive.stat().st_size, url=f'http://127.0.0.1:{port}/mbedtls-9.9.9.tar.bz2')
            status, err = self.run_tool()
        self.assertEqual(status, 1)
        self.assertIn('does not match the lock', err)
        self.assertEqual(list(self.cache.iterdir()), [])
        self.assertFalse(self.dest.exists())

    def test_a_download_that_matches_is_cached_and_extracted(self):
        served = self.top / 'served'
        served.mkdir()
        (served / 'mbedtls-9.9.9.tar.bz2').write_bytes(self.archive.read_bytes())
        with self.server(served) as port:
            self.write_lock(self.sha, self.archive.stat().st_size, url=f'http://127.0.0.1:{port}/mbedtls-9.9.9.tar.bz2')
            status, err = self.run_tool()
        self.assertEqual(status, 0, err)
        self.assertEqual(sha256(self.cache / 'mbedtls-9.9.9.tar.bz2'), self.sha)
        self.assertIn('library/ssl_tls.c', self.tree())

    # ---- no network ----

    def test_offline_with_a_verified_cache(self):
        self.cache.mkdir()
        (self.cache / 'mbedtls-9.9.9.tar.bz2').write_bytes(self.archive.read_bytes())
        status, err = self.run_tool()                       # the lock's address has nothing listening
        self.assertEqual(status, 0, err)
        self.assertNotIn('downloading', err)
        self.assertIn('library/aes.c', self.tree())

    def test_no_cache_and_no_network_says_what_to_do(self):
        status, err = self.run_tool()
        self.assertEqual(status, 1)
        self.assertIn('cannot download', err)
        self.assertIn(str(self.cache), err)
        self.assertIn('--archive', err)
        self.assertIn(self.sha, err)
        self.assertFalse(self.dest.exists())
        status, err = self.run_tool('--offline')
        self.assertEqual(status, 1)
        self.assertIn('--offline', err)

    # ---- unsafe archives: refused whole, nothing written ----

    def refused(self, extra, want):
        bad = self.top / 'bad.tar.bz2'
        sha = make_archive(bad, extra=extra)
        self.write_lock(sha, bad.stat().st_size)
        good = self.tree() if self.dest.exists() else None
        status, err = self.run_tool('--archive', str(bad))
        self.assertEqual(status, 1, err)
        self.assertIn(want, err)
        self.assertEqual(self.tree() if self.dest.exists() else None, good)
        self.assertEqual(self.leftovers(), [])
        self.assertFalse((self.top / 'evil').exists())
        self.assertFalse((self.tp / 'evil').exists())

    def add(self, tar, name, kind=tarfile.REGTYPE, linkname='', data=b'evil\n'):
        info = tarfile.TarInfo(name)
        info.type = kind
        info.linkname = linkname
        info.size = len(data) if kind == tarfile.REGTYPE else 0
        tar.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)

    def test_traversal_is_refused(self):
        self.extract_good()
        self.refused(lambda t: self.add(t, ROOT + '/library/../../evil'), 'unsafe member')
        self.refused(lambda t: self.add(t, '../evil'), 'unsafe member')
        self.refused(lambda t: self.add(t, '/tmp/evil-' + str(os.getpid())), 'unsafe member')

    def test_links_are_refused(self):
        self.extract_good()
        self.refused(lambda t: self.add(t, ROOT + '/library/link.c', tarfile.SYMTYPE, '/etc/passwd'), 'a link')
        self.refused(lambda t: self.add(t, ROOT + '/library/hard.c', tarfile.LNKTYPE, ROOT + '/LICENSE'), 'a link')

    def test_devices_and_fifos_are_refused(self):
        self.refused(lambda t: self.add(t, ROOT + '/library/dev', tarfile.CHRTYPE), 'a device or FIFO')
        self.refused(lambda t: self.add(t, ROOT + '/library/fifo', tarfile.FIFOTYPE), 'a device or FIFO')

    def test_members_outside_the_top_directory_are_refused(self):
        self.refused(lambda t: self.add(t, 'elsewhere/library/x.c'), 'outside')

    def test_a_name_twice_is_refused(self):
        self.refused(lambda t: self.add(t, ROOT + '/library/aes.c', data=b'int second;\n'), 'cannot be read')

    def test_a_broken_archive_keeps_the_good_tree(self):
        good = self.extract_good()
        broken = self.top / 'broken.tar.bz2'
        broken.write_bytes(self.archive.read_bytes()[:-200])
        self.write_lock(sha256(broken), broken.stat().st_size)
        status, err = self.run_tool('--archive', str(broken))
        self.assertEqual(status, 1)
        self.assertIn('cannot be read', err)
        self.assertEqual(self.tree(), good)
        self.assertEqual(self.leftovers(), [])

    def test_an_archive_without_the_library_is_refused(self):
        files = {k: v for k, v in FILES.items() if k != 'library/ssl_tls.c'}
        lacking = self.top / 'lacking.tar.bz2'
        self.write_lock(make_archive(lacking, files), lacking.stat().st_size)
        status, err = self.run_tool('--archive', str(lacking))
        self.assertEqual(status, 1)
        self.assertIn('library/ssl_tls.c', err)
        self.assertFalse(self.dest.exists())

    # ---- --check ----

    def test_check(self):
        status, err = self.run_tool('--check')
        self.assertEqual(status, 1)
        self.assertIn('missing', err)
        self.extract_good()
        ca = self.top / 'rootfs/etc/ssl/cert.pem'
        ca.parent.mkdir(parents=True)
        ca.write_bytes(b'roots\n')
        self.write_lock(self.sha, self.archive.stat().st_size,
                        ca={'file': 'rootfs/etc/ssl/cert.pem', 'sha256': hashlib.sha256(b'roots\n').hexdigest()})
        status, err = self.run_tool('--check')
        self.assertEqual(status, 0, err)
        (self.dest / 'library/aes.c').write_bytes(b'int changed;\n')
        (self.dest / 'library/extra.c').write_bytes(b'int extra;\n')
        ca.write_bytes(b'other roots\n')
        status, err = self.run_tool('--check')
        self.assertEqual(status, 1)
        self.assertIn('library/aes.c: changed', err)
        self.assertIn('library/extra.c: not from the archive', err)
        self.assertIn('cert.pem: its SHA-256', err)

    # ---- the repository's own pins ----

    def test_the_repository_lock(self):
        lock = fw.load_lock(str(REPO / 'third_party' / 'undertow-lock.json'))
        m = lock['mbedtls']
        self.assertEqual(m['version'], '3.6.7')
        self.assertEqual(m['url'], 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/'
                                   'mbedtls-3.6.7.tar.bz2')
        self.assertEqual(m['sha256'], 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6')
        self.assertEqual(m['extract'], ['LICENSE', 'include/', 'library/'])
        ca = lock['ca_bundle']
        bundle = REPO / ca['file']
        self.assertEqual(sha256(bundle), ca['sha256'])
        self.assertEqual(ca['sha256'], 'a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505')
        self.assertEqual(bundle.read_bytes().count(b'-----BEGIN CERTIFICATE-----'), ca['certificates'])

    @contextlib.contextmanager
    def server(self, directory):
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=str(directory), **k)

            def log_message(self, *a):
                pass

        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Quiet)
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            yield httpd.server_address[1]
        finally:
            httpd.shutdown()
            httpd.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
