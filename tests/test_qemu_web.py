"""Tests for tools/qemu-web.py (10/11/26), the harness that runs Undertow in QEMU against the web fixtures, on the
host: no QEMU, no network beyond 127.0.0.1.

Run from the repository: python3 -m unittest discover -s tests -p test_qemu_web.py   (make test-web-host)

The test initrd is a copy of the normal one with the fixture CA and the host table, written in a temporary
directory, and the normal initrd is never written; the production initrd and rootfs/ never hold a fixture CA (a
real build's build/images/initrd.tar too, when there is one, or $SURFOS_INITRD); members that could write outside
the copy are refused; nothing opens a disk device and no guest gets a disk. Then the harness's own mechanics
against a scripted terminal: steps, prompts (an editor that redraws its prompt, a console line after the prompt),
what must not appear, raw escape bytes; and the scenario table: names, boots, placeholders, and the rule that a
step's first expectation is never part of the line typed (the echo would satisfy it).
"""
import argparse
import builtins
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import tarfile
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, REPO / 'tools' / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


web = load('qemu_web', 'qemu-web.py')
wf = web.wf
mkimage = web.mkimage
PKI = None
TMP = None


def setUpModule():
    global PKI, TMP
    TMP = tempfile.TemporaryDirectory(prefix='test-qemu-web-')
    PKI = wf.Pki(Path(TMP.name) / 'pki')


def tearDownModule():
    TMP.cleanup()


class FakeFixtures:
    """what the initrd builder needs of the fixtures: the CA and the settings file"""
    ports = {name: 40000 + i for i, (name, *_) in enumerate(wf.SERVERS)}

    def __init__(self):
        self.ca_pem = PKI.ca_file.read_bytes()

    undertow_conf = wf.Fixtures.undertow_conf

    def mark(self):
        return 0

    def records(self, **kw):
        return []

    def wait_for(self, *a, **kw):
        return None


PRODUCTION_PEM = (b'# a production bundle stand-in\n-----BEGIN CERTIFICATE-----\n' +
                  __import__('base64').encodebytes(b'0\x82\x01\x00 not a fixture CA, just DER-ish bytes') +
                  b'-----END CERTIFICATE-----\n')


def normal_initrd(directory, extra=None):
    """an initrd as `make images` writes one (tools/mkimage.py, from a directory), with a production bundle"""
    root = Path(directory) / 'tree'
    files = {'bin/undertow': b'\x7fELF undertow', 'bin/surfsh': b'\x7fELF surfsh', 'motd': b'Welcome to SurfOS\n',
             'etc/undertow.conf': b'rtc_offset_minutes=0\n', 'etc/ssl/cert.pem': PRODUCTION_PEM,
             'etc/build-id': b'abc123 2026-10-11T00:00:00Z\n'}
    files.update(extra or {})
    for name, data in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)
    out = Path(directory) / 'images' / 'initrd.tar'
    out.parent.mkdir(parents=True, exist_ok=True)
    with contextlib.redirect_stdout(io.StringIO()):
        mkimage.make_initrd(str(out), str(root))
    return out


def members(path):
    with tarfile.open(path) as t:
        return {m.name: (m.type, m.mode & 0o777, t.extractfile(m).read() if m.isfile() else None) for m in t}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class TestInitrdTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.normal = normal_initrd(self.dir)

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_copy_with_the_fixture_ca_and_hosts(self):
        before, mtime = digest(self.normal), os.stat(self.normal).st_mtime_ns
        out = self.dir / 'test' / 'initrd.tar'
        conf = FakeFixtures().undertow_conf().encode()
        web.build_test_initrd(self.normal, out, {'etc/ssl/cert.pem': PKI.ca_file.read_bytes(), 'etc/undertow.conf': conf})
        self.assertEqual((digest(self.normal), os.stat(self.normal).st_mtime_ns), (before, mtime))
        old, new = members(self.normal), members(out)
        self.assertEqual(set(old), set(new))
        self.assertEqual(new['etc/ssl/cert.pem'][2], PKI.ca_file.read_bytes())
        self.assertEqual(new['etc/undertow.conf'][2], conf)
        self.assertIn(b'host=fixture.test 10.0.2.2\nhost=www.fixture.test 10.0.2.2\n', conf)
        for name in set(old) - {'etc/ssl/cert.pem', 'etc/undertow.conf'}:
            self.assertEqual(old[name], new[name], name)
        self.assertEqual(web.fixture_ca_in(out), ['etc/ssl/cert.pem'])
        self.assertEqual(web.fixture_ca_in(self.normal), [])

    def test_a_missing_directory_is_made(self):
        normal = normal_initrd(self.dir / 'bare', {})
        with tarfile.open(normal) as t:
            names = t.getnames()
        out = web.build_test_initrd(normal, self.dir / 'out' / 'initrd.tar', {'etc/new/thing.txt': b'x'})
        self.assertEqual(members(out)['etc/new/thing.txt'][2], b'x')
        self.assertTrue(set(names) <= set(members(out)))

    def test_the_normal_initrd_is_only_read(self):
        os.chmod(self.normal, 0o444)
        os.chmod(self.normal.parent, 0o555)
        try:
            web.build_test_initrd(self.normal, self.dir / 'test' / 'initrd.tar', {'etc/ssl/cert.pem': b'x'})
        finally:
            os.chmod(self.normal.parent, 0o755)
            os.chmod(self.normal, 0o644)

    def test_never_over_or_beside_the_normal_one(self):
        with self.assertRaises(web.InitrdError):
            web.build_test_initrd(self.normal, self.normal, {})
        with self.assertRaises(web.InitrdError):
            web.build_test_initrd(self.normal, self.normal.parent / 'initrd-test.tar', {})
        self.assertEqual(web.fixture_ca_in(self.normal), [])

    def test_members_that_could_escape_are_refused(self):
        def tar_with(add):
            path = self.dir / 'evil' / f'{len(os.listdir(self.dir))}.tar'
            path.parent.mkdir(exist_ok=True)
            with tarfile.open(path, 'w', format=tarfile.USTAR_FORMAT) as t:
                add(t)
            return path

        def member(name, kind=tarfile.REGTYPE, data=b'x', link=''):
            def add(t):
                info = tarfile.TarInfo(name)
                info.type, info.linkname = kind, link
                info.size = len(data) if kind == tarfile.REGTYPE else 0
                t.addfile(info, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
            return add
        cases = [member('/etc/passwd'), member('../outside'), member('bin/../../outside'),
                 member('etc/link', tarfile.SYMTYPE, link='/etc/passwd'), member('etc/hard', tarfile.LNKTYPE, link='motd'),
                 member('dev/sda', tarfile.BLKTYPE), member('dev/tty', tarfile.CHRTYPE), member('fifo', tarfile.FIFOTYPE)]
        for add in cases:
            path = tar_with(add)
            with self.assertRaises(web.InitrdError, msg=str(path)):
                web.build_test_initrd(path, self.dir / 'out' / 'x.tar', {})
        self.assertFalse((self.dir.parent / 'outside').exists())

    def test_all_initrds(self):
        initrds = web.test_initrds(self.normal, FakeFixtures(), self.dir / 'work')
        self.assertEqual(set(initrds), {'test', 'offset', 'bad-config', 'normal'})
        self.assertEqual(digest(initrds['normal']), digest(self.normal))
        self.assertIn(b'rtc_offset_minutes=-300', members(initrds['offset'])['etc/undertow.conf'][2])
        self.assertIn(b'colour=blue', members(initrds['bad-config'])['etc/undertow.conf'][2])
        for name in ('test', 'offset', 'bad-config'):
            self.assertEqual(web.fixture_ca_in(initrds[name]), ['etc/ssl/cert.pem'], name)
        self.assertEqual(web.fixture_ca_in(initrds['normal']), [])
        ok, why = web.check_normal_initrd(initrds['test'])
        self.assertFalse(ok)
        self.assertIn('etc/ssl/cert.pem', why)


class ProductionTests(unittest.TestCase):
    """the production images never trust the fixtures: rootfs/ (what make images and make usb-image pack) and a
    real build's initrd"""

    def test_rootfs_has_no_fixture_ca(self):
        for path in (REPO / 'rootfs').rglob('*'):
            if path.is_file():
                self.assertFalse(wf.has_fixture_ca(path.read_bytes()), path)

    def test_a_built_initrd(self):
        path = os.environ.get('SURFOS_INITRD') or REPO / 'build' / 'images' / 'initrd.tar'
        if not os.path.exists(path):
            self.skipTest(f'no {path}: build images first (or set SURFOS_INITRD)')
        self.assertEqual(web.fixture_ca_in(path), [])

    def test_only_the_test_targets_use_the_fixtures(self):
        lines = [l for l in (REPO / 'Makefile').read_text().splitlines() if 'web-fixtures' in l or 'qemu-web' in l]
        self.assertTrue(lines)
        for l in lines:
            self.assertFalse('initrd' in l and 'qemu-web.py' not in l and not l.lstrip().startswith('#'), l)
            self.assertNotIn('mkimage', l)


class NoDiskTests(unittest.TestCase):
    """no guest gets a disk and nothing the harness opens is a device"""

    def test_guests_have_no_disk(self):
        calls = []

        class FakeQemu:
            def __init__(self, *args, **kw):
                calls.append((args, kw))
                self.transcript, self.ports = b'', {}

            def expect(self, *a, **kw):
                return False

            def close(self):
                pass

        def fake_usb(*args):
            calls.append((args, {}))
            return FakeQemu.__new__(FakeQemu)
        a = argparse.Namespace(kernel='k.bin', qemu='qemu', mem='256', boot_timeout=0.1)
        run = web.Run(a, FakeFixtures(), {k: f'/tmp/x/{k}.tar' for k in ('test', 'offset', 'bad-config', 'normal')}, None)
        with mock.patch.object(web.harness, 'Qemu', FakeQemu), mock.patch.object(web.harness, 'usbnet_later_boot', fake_usb):
            for name in web.BOOTS:
                guest, ok, why = run.boot(name)
                self.assertFalse(ok)
        self.assertEqual(len(calls), len(web.BOOTS))
        for args, kw in calls:
            self.assertNotIn('disk', kw)
            flat = [str(x) for x in args] + [str(v) for v in kw.values()]
            flat += [str(y) for x in args if isinstance(x, (list, tuple)) for y in x]
            for word in flat:
                self.assertFalse(word.startswith('/dev/'), word)
                self.assertNotIn(word, ('-drive', '-hda', '-hdb', '-cdrom', '-blockdev', '-sd', '-mtdblock'))
                self.assertNotIn('usb-storage', word)

    def test_building_opens_no_device(self):
        opened = []
        real_open, real_os_open = builtins.open, os.open

        def track_open(file, *a, **kw):
            opened.append(str(file))
            return real_open(file, *a, **kw)

        def track_os_open(path, *a, **kw):
            opened.append(str(path))
            return real_os_open(path, *a, **kw)
        with tempfile.TemporaryDirectory() as tmp:
            normal = normal_initrd(tmp)
            with mock.patch('builtins.open', track_open), mock.patch('os.open', track_os_open):
                web.test_initrds(normal, FakeFixtures(), Path(tmp) / 'work')
        self.assertTrue(opened)
        self.assertEqual([p for p in opened if p.startswith('/dev/')], [])


# ---- the mechanics, against a scripted terminal -----------------------------------------------------------------------

class ScriptedTerm:
    """a terminal whose guest answers each line typed with chunks of output, each after a delay"""

    def __init__(self, script):
        self.script, self.data, self.pending = script, b'', []

    def raw(self):
        return self.data

    def type(self, keys):
        line = keys if isinstance(keys, str) else keys.decode('latin-1')
        now = time.monotonic()
        for delay, chunk in self.script(line):
            self.pending.append((now + delay, chunk))

    def pump(self, t):
        time.sleep(t)
        now = time.monotonic()
        due = [p for p in self.pending if p[0] <= now]
        self.pending = [p for p in self.pending if p[0] > now]
        for _, chunk in sorted(due, key=lambda p: p[0]):
            self.data += chunk

    def alive(self):
        return True


class FakeGuest:
    def __init__(self, term):
        self.serial, self.telnet, self.name = term, None, 'fake'


def run_steps(script, *steps):
    a = argparse.Namespace(kernel='k', qemu='q', mem='64', boot_timeout=1)
    run = web.Run(a, FakeFixtures(), None, None)
    guest = FakeGuest(ScriptedTerm(script))
    ctx = web.Context(run, guest, None)
    results = [run.run_step(ctx, s) for s in steps]
    return results, guest.serial.data


def page(line):
    """a browser that echoes the line (redrawing its prompt), then loads a page"""
    echo = b'\r\x1b[Kundertow> ' + line.encode() + b'\r\n'
    if line.startswith('open '):
        u = line[5:].encode()
        return [(0, echo), (0.2, b'Loading ' + u + b' ...\r\n'),
                (0.5, b'\x1b[1mUndertow - About\x1b[0m   [HTTP unencrypted]\r\n' + u + b'\r\nAbout[1]\r\n'
                      b'lines 1-3 of 3 (100%)\r\n\x1b[1mundertow\x1b[0m> ')]
    if line == 'evil':
        return [(0, echo), (0.1, b'text \x1b]0;pwned\x07 more\r\nundertow> ')]
    if line == 'quiet':
        return [(0, echo)]
    if line == 'late':
        return [(0, echo), (0.1, b'late output\r\nundertow> '), (0.2, b'\r\n+NET: a line from the kernel\r\n')]
    return [(0, echo), (0.1, b'?\r\nundertow> ')]


class MechanicsTests(unittest.TestCase):
    def test_a_page(self):
        (r,), data = run_steps(page, web.Step('open ' + web.H('/about'), web.loading(web.H('/about')),
                                              web.title('About'), web.HTTP_PLAIN, web.H('/about'), web.POSITION))
        self.assertEqual(r, (True, ''))

    def test_the_echo_does_not_end_a_step(self):
        start = time.monotonic()
        (r,), _ = run_steps(page, web.Step('open ' + web.H('/x')))           # no expectations: the prompt must be last
        self.assertTrue(r[0])
        self.assertGreater(time.monotonic() - start, 0.5)                   # it waited for the page, not the echo

    def test_a_console_line_after_the_prompt(self):
        (r,), data = run_steps(page, web.Step('late', 'late output'))
        self.assertEqual(r, (True, ''))
        self.assertTrue(data.endswith(b'the kernel\r\n'))

    def test_missing_expectation(self):
        (r,), _ = run_steps(page, web.Step('open ' + web.H('/about'), 'Not on the page', timeout=1))
        self.assertFalse(r[0])
        self.assertIn('did not see', r[1])

    def test_the_browser_ended(self):
        start = time.monotonic()
        (r,), _ = run_steps(lambda line: [(0, b'usage: undertow\r\n[exit status 2]\r\nSURF \xe2\x89\x8b!> ')],
                            web.Step('undertow', 'Undertow fixtures', timeout=20))
        self.assertFalse(r[0])
        self.assertIn('the browser ended', r[1])
        self.assertLess(time.monotonic() - start, 5)                      # not the 20 s of the timeout

    def test_no_prompt(self):
        (r,), _ = run_steps(page, web.Step('quiet', timeout=1))
        self.assertEqual(r, (False, 'no undertow prompt at the end'))

    def test_absent_and_raw_bytes(self):
        (r,), _ = run_steps(page, web.Step('open ' + web.H('/a'), web.title('About'), absent=[web.VERIFIED]))
        self.assertTrue(r[0])
        (r,), _ = run_steps(page, web.Step('open ' + web.H('/a'), web.title('About'), absent=['About[1]']))
        self.assertEqual(r, (False, "'About[1]' appeared"))
        (r,), _ = run_steps(page, web.Step('evil', 'more', raw_absent=wf.ESCAPE_BYTES))
        self.assertFalse(r[0])
        self.assertIn('raw bytes', r[1])

    def test_checks_run(self):
        seen = []
        (r,), _ = run_steps(page, web.Step('open ' + web.H('/a'), web.title('About'),
                                           check=lambda ctx: (seen.append(ctx.out) or False, 'checked')))
        self.assertEqual(r, (False, 'checked'))
        self.assertIn('Undertow - About', seen[0])
        self.assertNotIn('\x1b', seen[0])

    def test_clean_and_prompts(self):
        self.assertEqual(web.clean(b'\x1b[1;33mSURF\x1b[0m \x1b]0;t\x07x\r\n\x1b[?25l'), 'SURF x\n')
        for prompt in ('SURF \u224b> ', 'initrd \u25b8 bin \u224b> ', 'SURF \u224b@> ', 'SURF ~> ', 'SURF \u224b!> '):
            self.assertTrue(web.SH_PROMPT.search(prompt), prompt)
        self.assertTrue(web.UT_PROMPT.search('undertow> '))
        self.assertFalse(web.SH_PROMPT.search('undertow> '))

    def test_fill(self):
        ctx = argparse.Namespace(ports={'http': 1234, 'wrong-name': 99})
        self.assertEqual(web.fill('http://fixture.test:{http}/ {wrong-name} {other} {x', ctx),
                         'http://fixture.test:1234/ 99 {other} {x')
        self.assertRegex(web.fill('{now} {today} {now-3h:date}', ctx),
                         r'^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d \d{4}-\d\d-\d\d \d{4}-\d\d-\d\d$')

    def test_rtc(self):
        import datetime
        now = datetime.datetime(2026, 10, 11, 3, 0, 0, tzinfo=datetime.timezone.utc)
        self.assertEqual(web.rtc_base('utc-5h', now), '2026-10-10T22:00:00')
        self.assertEqual(web.rtc_base('utc', now), 'utc')


# ---- the scenario table -------------------------------------------------------------------------------------------------

class ScenarioTableTests(unittest.TestCase):
    TIMES = ('today', 'now', 'now-3h', 'now-3h:date')

    def test_names_and_boots(self):
        names = [sc.name for sc in web.SCENARIOS]
        self.assertEqual(len(names), len(set(names)))
        for sc in web.SCENARIOS:
            self.assertIn(sc.boot, web.BOOTS, sc.name)
            self.assertTrue(sc.steps, sc.name)
        self.assertEqual({b for b in web.BOOTS}, {sc.boot for sc in web.SCENARIOS})

    def test_placeholders(self):
        servers = {name for name, *_ in wf.SERVERS}
        for sc in web.SCENARIOS:
            for step in sc.steps:
                texts = [x for x in (step.keys, *step.expects, *step.absent) if isinstance(x, str)]
                for t in texts:
                    for key in re.findall(r'\{([\w:-]+)\}', t):
                        self.assertTrue(key in servers or key in self.TIMES, f'{sc.name}: {{{key}}} in {t!r}')

    def test_first_expectation_is_never_the_echo(self):
        ctx = argparse.Namespace(ports=FakeFixtures.ports, state={})
        for sc in web.SCENARIOS:
            for step in sc.steps:
                keys = step.keys(ctx) if callable(step.keys) else step.keys
                if not isinstance(keys, str) or not step.expects:
                    continue
                typed, first = web.fill(keys, ctx), step.expects[0]
                if isinstance(first, str):
                    self.assertNotIn(web.fill(first, ctx), typed, f'{sc.name}: {keys!r}')
                else:
                    self.assertIsNone(first.search(typed), f'{sc.name}: {keys!r} matches {first.pattern}')

    def test_telnet_steps_follow_a_session(self):
        for sc in web.SCENARIOS:
            opened = False
            for step in sc.steps:
                if step.act and 'telnet session' in step.name:
                    opened = 'ends' not in step.name
                if step.on == 'telnet':
                    self.assertTrue(opened, f'{sc.name}: {step.name}')

    def test_every_scenario_ends_at_the_shell(self):
        for sc in web.SCENARIOS:
            last = [s for s in sc.steps if not s.act][-1]
            self.assertEqual(last.wait, web.SH, sc.name)

    def test_list_and_unknown_scenario(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(web.main(['--list']), 0)
            self.assertEqual(web.main(['--test', '--scenario', 'no-such-scenario']), 2)
        self.assertIn('tls-refused', out.getvalue())
        self.assertIn('no such scenario: no-such-scenario', out.getvalue())


if __name__ == '__main__':
    unittest.main()
