#!/usr/bin/env python3
"""Undertow, the SurfOS web browser, in QEMU against the web fixtures (10/11/26): the end-to-end tests.

    tools/qemu-web.py --kernel build/O0/surfos.bin --initrd build/images/initrd.tar --test      (make test-web)
    tools/qemu-web.py --kernel K --initrd I --test --scenario http-page --scenario tls-refused  some scenarios
    tools/qemu-web.py --list                       the scenarios: their boot, which need the real browser
    tools/qemu-web.py --kernel K --initrd I --interactive
                                                   the fixtures, a test initrd and QEMU on this terminal (Ctrl-A x)

The fixtures (tools/web-fixtures.py) run in this process: HTTP and HTTPS servers on 127.0.0.1 with a test CA
made for this run. The guest reaches them at 10.0.2.2 through QEMU's user network, by the names fixture.test
and www.fixture.test, which the test initrd maps there. That initrd is made in a temporary directory from a
copy of the normal one (--initrd, only ever read): the fixture CA as etc/ssl/cert.pem, an etc/undertow.conf
with the host= lines, written by tools/mkimage.py's initrd writer. The normal initrd must not hold a fixture
CA and must be the same file afterwards; both are checked. No disk is attached to any guest.

Every guest's DNS server is 10.0.2.99, an address on QEMU's network that nobody answers: a lookup can only
fail, inside the guest, and nothing reaches a public DNS server. The guests:
  main        -cpu max (RDRAND), the clock in UTC (-rtc base=utc), the test initrd: most scenarios
  no-rdrand   QEMU's default CPU, without RDRAND: HTTPS must refuse to run, HTTP must work
  old-clock   the RTC in 2004 (a flat battery): HTTPS stops until `clock` gives the time
  rtc-offset  the RTC 5 hours behind UTC and rtc_offset_minutes=-300 in undertow.conf
  bad-config  an undertow.conf the browser must refuse (a warning, then the defaults)
  production  the normal initrd itself: its roots must not trust the fixtures
  wlansim     the simulated Wi-Fi (kernel word wlansim): the only route is wlan0, eth0 has no address
  usbnet      no e1000: QEMU's usb-net on an xHCI is eth0

A scenario is a list of steps (SCENARIOS below): keys typed on the serial console (or a telnet session),
what must then appear, in order, in what the guest prints with its colour sequences taken out, what must
not appear (also as raw bytes: a page's escape sequences), and checks of what the fixtures saw (the request
line, Host, SNI, the TLS version, when the client gave up). A step ends at the first prompt (the browser's or
the shell's) after what it expects, once nothing more comes for a moment; a step that expects nothing ends when
the prompt is the last thing printed (an editor that redraws its line as keys are typed prints its prompt
then too). A scenario starts and ends at the shell's prompt; after a failure the harness gets back there
(Ctrl+C, quit, Ctrl+D) or boots the guest again.

What the browser prints (the screen, its messages) is in one place, "what the browser prints" and MSG below,
written from the design (docs/superpowers/specs/2026-10-10-undertow-design.md) and the brief's description of
the screen before the browser existed: adjust them there. While /initrd/bin/undertow is the stand-in that cannot browse,
the scenarios that need the browser are reported NOT RUN (--run-anyway runs them all the same).

Output: PASS/FAIL lines and a summary (WEB TEST PASSED: N checks), exit status 0 only when everything ran
and passed. --output-dir keeps each guest's serial transcript (.raw as it came, .txt without escape
sequences), each scenario's part of it (scenarios/), the fixtures' log (fixtures.jsonl) and the summary.
"""
import argparse
import contextlib
import datetime
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent


def _load(name, file):
    spec = importlib.util.spec_from_file_location(name, TOOLS / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


harness = _load('qemu_run', 'qemu-run.py')
wf = _load('web_fixtures', 'web-fixtures.py')
mkimage = _load('mkimage', 'mkimage.py')

DNS_NOWHERE = '10.0.2.99'       # on QEMU's network, answered by nobody (QEMU answers ARP for .2 and .3 only)
STATIC_NET = f'ifconfig eth0 10.0.2.15 255.255.255.0 10.0.2.2 {DNS_NOWHERE}'

# ---- the test initrd ------------------------------------------------------------------------------------------------


class InitrdError(Exception):
    pass


def member_name(name):
    """a member's name as a relative path inside the initrd, or InitrdError: no absolute name, no .., nothing empty"""
    if name.startswith('/') or '\\' in name or '\0' in name:
        raise InitrdError(f'{name!r}: not a relative name')
    parts = [p for p in name.split('/') if p not in ('', '.')]
    if not parts or '..' in parts:
        raise InitrdError(f'{name!r}: not a name inside the initrd')
    return '/'.join(parts)


def initrd_files(path):
    """{name: bytes} of an initrd's regular files (directories as None), every member checked by member_name();
    links, devices and anything else are refused"""
    out = {}
    with tarfile.open(path) as t:
        for m in t.getmembers():
            name = member_name(m.name)
            if m.isdir():
                out.setdefault(name, None)
            elif m.isfile():
                out[name] = (t.extractfile(m).read(), m.mode & 0o777, m.mtime)
            else:
                raise InitrdError(f'{m.name}: a link, a device or a special file: not copied into a test initrd')
    return out


def fixture_ca_in(path):
    """the names of the files in an initrd that hold a certificate of a fixture CA (web-fixtures.MARKER)"""
    return sorted(n for n, v in initrd_files(path).items() if v is not None and wf.has_fixture_ca(v[0]))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def build_test_initrd(normal, out, replace):
    """out: a copy of the initrd normal (never written: read once) with the files of replace ({name: bytes}) put
    in or replaced, made in a temporary directory and written by tools/mkimage.py's initrd writer. out must not
    be normal itself, nor in normal's directory (build/images/ is the normal build's)."""
    normal, out = Path(normal).resolve(), Path(out).resolve()
    if out == normal or out.parent == normal.parent:
        raise InitrdError(f'{out}: a test initrd never goes where the normal one is ({normal.parent})')
    files = initrd_files(normal)
    with tempfile.TemporaryDirectory(prefix='ut-initrd-') as tmp:
        root = Path(tmp) / 'initrd'
        root.mkdir()
        for name, v in sorted(files.items()):
            p = root / name
            if v is None:
                p.mkdir(parents=True, exist_ok=True)
                continue
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(v[0])
            os.chmod(p, v[1] or 0o644)
            os.utime(p, (v[2], v[2]))
        for name, data in replace.items():
            p = root / member_name(name)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        out.parent.mkdir(parents=True, exist_ok=True)
        with contextlib.redirect_stdout(io.StringIO()):     # mkimage says what it wrote; the log has it below
            mkimage.make_initrd(str(out), str(root))
    return out


BAD_CONF = (b'# a broken undertow.conf (tools/qemu-web.py, bad-config): the browser must warn and use the defaults\n'
            b'rtc_offset_minutes=0\ncolour=blue\nhost=fixture.test 10.0.2.2\n')


def test_initrds(normal, fx, directory):
    """the initrds the guests boot: {'test': ..., 'offset': ..., 'bad-config': ..., 'normal': a copy}"""
    d = Path(directory)
    ca = {'etc/ssl/cert.pem': fx.ca_pem}
    out = {'test': build_test_initrd(normal, d / 'test' / 'initrd.tar',
                                     dict(ca, **{'etc/undertow.conf': fx.undertow_conf().encode()})),
           'offset': build_test_initrd(normal, d / 'offset' / 'initrd.tar',
                                       dict(ca, **{'etc/undertow.conf': fx.undertow_conf(-300).encode()})),
           'bad-config': build_test_initrd(normal, d / 'bad-config' / 'initrd.tar',
                                           dict(ca, **{'etc/undertow.conf': BAD_CONF}))}
    out['normal'] = d / 'normal' / 'initrd.tar'
    out['normal'].parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(normal, out['normal'])
    return out


# ---- what the browser prints: adjust here ---------------------------------------------------------------------------
# The screen (the brief, track S): a title line ("Undertow - TITLE" and, at the right, "[HTTPS verified TLSv1.3]",
# "[HTTP unencrypted]" or "[HTTP 404 Not Found]" when the status is not 200), the address line, a rule, the page's
# rows, a rule, a position line ("lines 1-27 of 240 (11%)" and the useful keys), the prompt "undertow> ". Before a
# load: "Loading ADDRESS ...".

def loading(url):
    return f'Loading {url} ...'


def loading_from(url):
    """the start of a Loading line: for an address that goes on (a long one), or that may show its fragment"""
    return f'Loading {url}'


def title(text):
    return f'Undertow - {text}'


HTTPS_13 = '[HTTPS verified TLSv1.3]'
HTTPS_12 = '[HTTPS verified TLSv1.2]'
HTTPS_ANY = re.compile(r'\[HTTPS verified TLSv1\.[23]\]')
HTTP_PLAIN = '[HTTP unencrypted]'
VERIFIED = 'HTTPS verified'                     # never on the screen of a load that failed


def status(code, reason):
    return f'{code} {reason}'


POSITION = re.compile(r'lines (\d+)-(\d+) of (\d+)')
UT_PROMPT = re.compile(r'undertow> ')
SH_PROMPT = re.compile(r'SurfOS\*>|(?:\u224b|~)[!#@?]?> ')     # surfsh (the console's and a telnet session's), sh

# The messages (transport.h, http.h, config.c and the brief name what each must say; config.c's are exact)
MSG = {
    'refused': re.compile(r'(?i)refused'),
    'dns': re.compile(r'(?i)nowhere\.test'),
    'no-host': re.compile(r'(?i)fixture\.test'),
    'timeout': re.compile(r'(?i)time[ds]? ?out|no (?:progress|data|answer)|\d+ seconds'),
    'handshake': re.compile(r'(?i)handshake|TLS|secure connection'),
    'untrusted': re.compile(r'(?i)not trusted|untrusted|unknown (?:issuer|authority|root|CA)|issuer'),
    'wrong-name': re.compile(r'(?i)wrong-name\.test|name'),
    'expired': re.compile(r'(?i)expired'),
    'not-yet-valid': re.compile(r'(?i)not yet valid'),
    'clock': re.compile(r'(?i)clock'),
    'clock-howto': 'clock YYYY-MM-DD HH:MM:SS',
    'entropy': re.compile(r'(?i)random|entropy|RDRAND'),
    'not-text': re.compile(r'is not text'),
    # track H's messages (http.c), as substrings: exact
    'gzip': 'sent the page compressed (Content-Encoding: gzip)',
    'too-big': 'over 1 MB',
    'early-eof': 'closed the connection before the end of the page',
    # the load's 60 s: the transport's words when it runs out during a read, http.c's between the stages
    'deadline': re.compile(r'the 60-second limit for loading\s+a page ran out|the page did not load within 60 seconds'),
    'headers': 'sent more than 32 KB of headers',
    'no-status': 'did not answer with an HTTP status line',
    'bad-status': 'sent a malformed status line',
    'plain-to-tls': re.compile(r'reset the\s+connection|did not answer with an HTTP status line|closed the\s+connection'),
    'not-text-sniffed': 'does not say what it is and does not look like text',
    'sjis': 'the page says it is in shift_jis; only its ASCII is shown',              # html.c's notice
    'interim': 'sent more than 8 interim (1xx) responses',
    'downgrade': 'redirected from HTTPS to unencrypted HTTP',
    'bad-response': re.compile(r'(?i)malformed|invalid|bad|refus|ambiguous|unsupported|too (?:many|long|large|big)|'
                               r'over|early|ended|closed|incomplete|truncat|length|chunk|header|status|101|interim|'
                               r'switching|location|response'),
    'redirects': 'more than 5 redirects (the next was to ',
    'loop': 'the page redirects in a loop (back to ',
    'no-forward': re.compile(r'(?i)no (?:page|history|later page|next page)|nothing (?:to go|ahead|after)|'
                             r'end of (?:the )?history|cannot go forward|at the (?:newest|last)'),
    'no-link': re.compile(r'(?i)no link 257|there is no link|only 256|no such link'),
    'unusable': re.compile(r'(?i)mailto|cannot|not (?:an? )?(?:http|web|supported)|unsupported'),
    'not-found': re.compile(r'"no such words" (?:is not on|was not found on) this page'),     # session.c
    'too-long': re.compile(r'(?i)too long|over 4096|4096 bytes|longer than'),
    'unknown-command': re.compile(r'(?i)unknown|not a command|try help|type help|\bhelp\b'),
    'forms': 'forms are not supported',
    'truncated': 'the rest of this page is not shown',
    'use-dump': '--dump',
    'not-a-date': '2026-02-30 is not a date',                               # config.c
    'bad-month': 'the month must be 01 to 12',                              # config.c
    'clock-form': 'give the time in UTC as clock YYYY-MM-DD HH:MM:SS',      # config.c
    'conf-warning': 'undertow.conf line 3: unknown setting "colour"',       # config.c
    'url-space': 'URL contains a space',                                    # url.c
    'url-scheme': 'unsupported URL scheme',                                 # url.c
    'url-ipv6': 'IPv6 is unsupported',                                      # url.c
    'url-credentials': 'URL credentials are unsupported',                   # url.c
    'url-long': 'URL exceeds 2048 bytes',                                   # url.c
}

# ---- scenarios -------------------------------------------------------------------------------------------------------

UT, SH, TN = 'undertow', 'shell', 'telnet shell'        # what ends a step: the browser's prompt, the shell's, telnet's


class Step:
    """keys typed (a str is a line, Enter added; bytes go as they are; a function of the context returns either),
    then expects in order, then the prompt of wait. absent: never in the step's text; raw_absent: never in its
    raw bytes; check(ctx) -> (ok, why) afterwards; on: 'serial' or 'telnet'. A Step with act instead of keys runs
    act(ctx) -> (ok, why)."""

    def __init__(self, keys, *expects, wait=UT, absent=(), raw_absent=(), check=None, timeout=20, on='serial',
                 act=None, name=None):
        self.keys, self.expects, self.wait, self.absent, self.raw_absent = keys, expects, wait, absent, raw_absent
        self.check, self.timeout, self.on, self.act = check, timeout, on, act
        self.name = name or (keys if isinstance(keys, str) else repr(keys) if isinstance(keys, bytes)
                             else '(a line made by a function)' if callable(keys) else 'act')


class Scenario:
    def __init__(self, name, doc, steps, boot='main', browser=True):
        self.name, self.doc, self.steps, self.boot, self.browser = name, doc, steps, boot, browser


def url(server, path='/', host='fixture.test'):
    """the address of path on a fixture server, as typed in the guest; {server} becomes the port"""
    scheme = 'http' if server in ('http', 'refused') else 'https'
    return f'{scheme}://{host}:{{{server}}}{path}'


H = lambda path='/': url('http', path)              # noqa: E731  (the plain server: most pages)
S = lambda path='/': url('https', path)              # noqa: E731


def act(fn, name):
    return Step(None, act=fn, name=name)


def sleep(seconds):
    return act(lambda ctx: (time.sleep(seconds), (True, ''))[1], f'(wait {seconds} s)')


def later(text, timeout):
    """text on the serial console at any time since the scenario began (a job in the background prints it)"""
    def wait(ctx):
        end = ctx.run.wait_text(ctx.guest.serial, ctx.serial_start, 0, text, timeout)
        return end is not None and end >= 0, f'{text!r} never came'
    return act(wait, f'(wait for {text!r})')


# -- checks of what the fixtures saw during a step (or since the scenario began: whole=True) --

def _recs(ctx, server=None, path=None, whole=False):
    return ctx.fx.records(server=server, path=path, since=ctx.scenario_mark if whole else ctx.mark)


def fetched(server, path, n=1, **want):
    """n requests for path on server during the step, each with the headers of a browser that behaves (10/25/26: or
    of fetch, the downloader: agent='fetch/1.0', the start of its User-Agent)"""
    def check(ctx):
        recs = [r for r in _recs(ctx, server, path) if r['request_line']]
        if len(recs) != n:
            return False, f'{len(recs)} requests for {path} on {server}, not {n}'
        for r in recs:
            heads = {k.lower(): v for k, v in r['headers']}
            problems = []
            if '#' in (r['target'] or ''):
                problems.append(f'the fragment was sent: {r["target"]}')
            if not r['request_line'].endswith(' HTTP/1.1') or r['method'] != 'GET':
                problems.append(f'request line {r["request_line"]!r}')
            host = fill(want.get('host', f'fixture.test:{{{server}}}'), ctx)
            if r['host'] != host:
                problems.append(f'Host {r["host"]!r}, not {host!r}')
            if not heads.get('user-agent', '').startswith(want.get('agent', 'Undertow/1.0')):
                problems.append(f'User-Agent {heads.get("user-agent")!r}')
            if heads.get('accept-encoding') != 'identity' or heads.get('connection', '').lower() != 'close':
                problems.append('Accept-Encoding identity and Connection close expected')
            if 'tls' in want and r['tls'] != want['tls']:
                problems.append(f'TLS {r["tls"]}, not {want["tls"]}')
            if r['scheme'] == 'https' and (r['sni'] != want.get('sni', host.split(':')[0]) or r['alpn'] != 'http/1.1'):
                problems.append(f'SNI {r["sni"]!r}, ALPN {r["alpn"]!r}')
            if problems:
                return False, f'{path} on {server}: ' + '; '.join(problems)
        return True, ''
    return check


def nothing_fetched(ctx):
    recs = [r for r in _recs(ctx) if r['request_line']]
    return (not recs, 'fetched: ' + ', '.join(f'{r["server"]} {r["target"]}' for r in recs))


def no_http_on(server):
    """the server's connections during the step never carried an HTTP request (TLS failed first), and the plain
    HTTP server got none either (no fallback)"""
    def check(ctx):
        bad = [r for r in _recs(ctx) if r['request_line'] and r['server'] in (server, 'http')]
        return (not bad, 'requests: ' + ', '.join(f'{r["server"]} {r["request_line"]}' for r in bad))
    return check


def no_picture_over_http(ctx):
    """an https page's pictures: nothing asked of the http server (11/1/26, stage 2.4)"""
    bad = [r for r in _recs(ctx) if r['request_line'] and r['server'] == 'http']
    return (not bad, 'requests: ' + ', '.join(r['request_line'] for r in bad))


def gave_up(server, path, low, high, since='requested'):
    """the client closed its latest connection for path (None: any) on server low..high seconds after the request
    (or the connection: since='connected'); that connection may have begun in an earlier step of the scenario"""
    def check(ctx):
        def mine(r):
            return r['server'] == server and (path is None or (r['target'] or '').split('?')[0] == path)
        deadline = time.monotonic() + 15
        while True:
            recs = [r for r in ctx.fx.records(since=ctx.scenario_mark) if mine(r)]
            rec = recs[-1] if recs else None
            if (rec and rec['ended'] is not None) or time.monotonic() > deadline:
                break
            time.sleep(0.1)
        if rec is None or rec['ended'] is None:
            return False, f'no finished connection for {path} on {server}'
        t0 = rec[since] if rec[since] is not None else rec['connected']
        took = rec['ended'] - t0
        ok = rec['client_closed'] and low <= took <= high
        return ok, f'{server} {path}: the client {"closed" if rec["client_closed"] else "did not close"} after {took:.1f} s'
    return check


def all_of(*checks):
    def check(ctx):
        for c in checks:
            ok, why = c(ctx)
            if not ok:
                return ok, why
        return True, ''
    return check


def remember(key, pattern, group=1):
    """keep a match of pattern in the step's text (its last) for later steps"""
    def check(ctx):
        ms = list(re.finditer(pattern, ctx.out))
        if not ms:
            return False, f'nothing matched {pattern}'
        ctx.state[key] = ms[-1].group(group)
        return True, ''
    return check


def position(expect_start=None, after=None, same_as=None, key='pos'):
    """the step's last position line: its first line is expect_start, or the line after the end of the view kept
    as after, or the same first line as same_as; it is kept as key"""
    def check(ctx):
        ms = list(POSITION.finditer(ctx.out))
        if not ms:
            return False, 'no position line'
        first, last, total = (int(x) for x in ms[-1].groups())
        ctx.state[key] = (first, last, total)
        if expect_start is not None and first != expect_start:
            return False, f'the view starts at line {first}, not {expect_start}'
        if after is not None and first != ctx.state[after][1] + 1:
            return False, f'the view starts at line {first}, not after {ctx.state[after]}'
        if same_as is not None and first != ctx.state[same_as][0]:
            return False, f'the view starts at line {first}, not at {ctx.state[same_as][0]}'
        return True, ''
    return check


def counter_went_up(key):
    def check(ctx):
        m = re.findall(r'fetched (\d+) times', ctx.out)
        if not m:
            return False, 'no counter on the page'
        before, ctx.state[key] = ctx.state[key], m[-1]
        return int(m[-1]) == int(before) + 1, f'the counter says {m[-1]} after {before}'
    return check


def netstat_clean(ctx):
    """netstat (the step's output) shows no connection to the fixtures that something still holds: none open
    (ESTABLISHED, CLOSE_WAIT, SYN_SENT) and none that is not an orphan, but for TIME_WAIT. An orphan the kernel is
    still closing (FIN_WAIT, CLOSING, LAST_ACK: its program has ended) is no leak."""
    ports = set(ctx.fx.ports.values())
    left = []
    for l in ctx.out.splitlines():
        m = re.search(r'tcp\s+(\w+)\s+local \d+ remote 10\.0\.2\.2:(\d+)', l)
        if not m or int(m.group(2)) not in ports:
            continue
        held = m.group(1) in ('ESTABLISHED', 'CLOSE_WAIT', 'SYN_SENT') or \
            ('(orphan)' not in l and m.group(1) not in ('TIME_WAIT', 'LISTEN'))
        if held:
            left.append(l.strip())
    return (not left, 'still open: ' + ' | '.join(left))


# -- the scenarios: browser=False ones are the plumbing, which runs before the browser exists --

NETSTAT_TCP = re.compile(r'(?i)\btcp\b')        # a "tcp ..." line, or "(no TCP sockets)"
# /split's text, entities decoded: as the console draws it (no euro sign, no CJK in its set), and as it is
SPLIT_TEXT = 'Gr\u00fc\u00dfe \u2014 caf\u00e9 \u2014 & EUR5 ??'
SPLIT_TEXT_UTF8 = 'Gr\u00fc\u00dfe \u2014 caf\u00e9 \u2014 & \u20ac5 \u65e5\u672c'

def _type_long(prefix, n, tail=''):
    return lambda ctx: prefix + 'a' * n + tail


SCENARIOS = [
    Scenario('plumbing', 'the test initrd, the fixtures from the guest (nettest), DNS that cannot leave the guest, '
                         'RDRAND, the program', [
        Step('cat /initrd/etc/undertow.conf', 'host=fixture.test 10.0.2.2', 'host=www.fixture.test 10.0.2.2', wait=SH),
        Step('ls /initrd/etc/ssl', 'cert.pem', wait=SH),
        Step('nettest get 10.0.2.2 {http} /plumbing', 'SurfOS web fixture: plumbing ok', wait=SH,
             check=lambda ctx: (any(r['request_line'] == 'GET /plumbing HTTP/1.0' for r in _recs(ctx, 'http')),
                                'the fixture did not see nettest\'s request')),
        Step('nettest connect 10.0.2.2 {https}', 'connected to 10.0.2.2', wait=SH,
             check=lambda ctx: (ctx.fx.wait_for(lambda r: r['server'] == 'https' and r['ended'], 10, ctx.mark)
                                is not None, 'the TLS fixture saw no connection')),
        Step('nettest refuse 10.0.2.2 {refused}', 'Connection refused', wait=SH),
        Step('nettest resolve nowhere.test', 'resolve nowhere.test:', 'errno=', wait=SH, timeout=20),
        Step('nettest entropy supported', 'ENTROPY PASS', wait=SH),
        Step('undertow --help', 'usage: undertow [ADDRESS]', wait=SH),
    ], browser=False),
    Scenario('plumbing-no-rdrand', 'QEMU\'s default CPU has no RDRAND', [
        Step('nettest entropy unsupported', 'ENTROPY PASS', wait=SH),
    ], boot='no-rdrand', browser=False),
    Scenario('plumbing-old-clock', 'the RTC says 2004', [
        Step('date', '2004-06-01', wait=SH),
    ], boot='old-clock', browser=False),
    Scenario('plumbing-rtc-offset', 'the offset initrd', [
        Step('cat /initrd/etc/undertow.conf', 'rtc_offset_minutes=-300', wait=SH),
    ], boot='rtc-offset', browser=False),

    # -- the browser --
    Scenario('welcome', 'undertow alone: the welcome screen (clock, HTTPS ready), help, ?, an unknown command, quit', [
        Step('undertow', 'Undertow', '{today}', MSG['entropy']),
        Step('help', 'open', 'back', 'forward', 'reload', 'links', 'findnext', 'history', 'clock', 'quit'),
        Step('?', 'findnext'),
        Step('frobnicate', MSG['unknown-command']),
        Step('quit', wait=SH),
    ]),
    Scenario('http-page', 'an HTTP page: Loading, title, [HTTP unencrypted], address, links, position; the request',
             [
        Step('undertow ' + H('/'), loading(H('/')), title('Undertow Fixture Home'), HTTP_PLAIN, H('/'),
             'Undertow fixtures', 'About these fixtures[1]', 'The documents[2]', POSITION,
             check=fetched('http', '/')),
        Step('q', wait=SH),
    ]),
    Scenario('https-versions', 'HTTPS with TLS 1.3 and with TLS 1.2, the ECDSA chain, both names and the IP address', [
        Step('undertow ' + url('tls13', '/about'), loading(url('tls13', '/about')), title('About the fixtures'),
             HTTPS_13, check=fetched('tls13', '/about', tls='TLSv1.3')),
        Step('open ' + url('tls12', '/about'), loading(url('tls12', '/about')), title('About the fixtures'), HTTPS_12,
             check=fetched('tls12', '/about', tls='TLSv1.2')),
        Step('open ' + url('https-ecdsa', '/about'), loading(url('https-ecdsa', '/about')), HTTPS_ANY,
             check=fetched('https-ecdsa', '/about')),
        Step('open ' + url('https', '/about', 'www.fixture.test'), loading(url('https', '/about', 'www.fixture.test')),
             HTTPS_ANY, check=fetched('https', '/about', host='www.fixture.test:{https}', sni='www.fixture.test')),
        Step('open ' + url('https', '/about', '10.0.2.2'), loading(url('https', '/about', '10.0.2.2')), HTTPS_ANY),
        Step('q', wait=SH),
    ]),
    Scenario('bare-host', 'an address without a scheme is HTTPS; o is open; an address typed alone is opened, '
                          'an IPv4 address too', [
        Step('undertow fixture.test:{https}/about', loading(S('/about')), title('About the fixtures'), HTTPS_ANY),
        Step('o ' + H('/docs/'), loading(H('/docs/')), title('The documents'), HTTP_PLAIN),
        Step('fixture.test:{https}/docs/guide', loading(S('/docs/guide')), title('The guide, part 1'), HTTPS_ANY),
        # 10/15/26 (review M3): an IPv4 address alone, with its port, is an address too
        Step('10.0.2.2:{https}', loading(url('https', '/', '10.0.2.2')), HTTPS_ANY),
        Step('q', wait=SH),
    ]),
    Scenario('editing', 'the line editor: Backspace, Up for the line before, Ctrl+U, PgDn and PgUp on an empty line', [
        Step('undertow', 'Undertow'),
        Step(lambda ctx: b'opex\x7fn ' + fill(H('/long'), ctx).encode() + b'\n', loading(H('/long')),
             title('A long page'), POSITION, check=position(expect_start=1, key='e1')),
        Step(b'\x1b[6~', POSITION, check=position(after='e1', key='e2')),
        Step(b'\x1b[5~', POSITION, check=position(expect_start=1)),
        Step(b'\x1b[A\n', loading(H('/long')), title('A long page'), check=fetched('http', '/long')),
        Step(b'top secret\x15url\n', H('/long'), 'status 200 OK'),
        Step('q', wait=SH),
    ]),
    Scenario('links', 'numbered links: relative, ./ with a query, ../, a query alone, scheme-relative, a fragment '
                      'alone (no fetch); go N; links; url', [
        Step('undertow ' + H('/docs'), loading(H('/docs')), title('The documents'), H('/docs/'),
             check=all_of(fetched('http', '/docs'), fetched('http', '/docs/'))),
        Step('1', loading(H('/docs/guide')), 'This is part 1 of the guide.', check=fetched('http', '/docs/guide')),
        Step('back', title('The documents'), check=nothing_fetched),
        Step('go 2', loading(H('/docs/guide?part=2')), 'This is part 2 of the guide.'),
        Step('back', title('The documents')),
        Step('3', loading(H('/about')), title('About the fixtures')),
        Step('b', title('The documents')),
        Step('4', loading(H('/docs/?sort=name')), title('The documents')),
        Step('back', title('The documents')),
        Step('5', loading(H('/about')), title('About the fixtures')),
        Step('back', title('The documents')),
        Step('6', 'The bottom of the documents.', absent=['Loading'], check=nothing_fetched),
        Step('links', 'The guide', H('/docs/guide'), 'About, scheme-relative', H('/about'), H('/docs/#bottom')),
        Step('url', H('/docs/')),
        Step('q', wait=SH),
    ]),
    Scenario('unusable-link', 'a mailto: link keeps its number and says why it is not followed', [
        Step('undertow ' + H('/'), title('Undertow Fixture Home')),
        Step('6', MSG['unusable'], absent=['Loading'], check=nothing_fetched),
        Step('q', wait=SH),
    ]),
    Scenario('back-forward', 'back and forward without fetching; a failed open keeps the forward entry; a page that '
                             'loads drops it', [
        Step('undertow ' + H('/'), loading(H('/')), title('Undertow Fixture Home')),
        Step('open ' + H('/about'), loading(H('/about')), title('About the fixtures')),
        Step('back', title('Undertow Fixture Home'), check=nothing_fetched),
        Step('forward', title('About the fixtures'), check=nothing_fetched),
        Step('b', title('Undertow Fixture Home'), check=nothing_fetched),
        Step('f', title('About the fixtures'), check=nothing_fetched),
        Step('back', title('Undertow Fixture Home')),
        Step('open ' + url('refused'), loading(url('refused')), MSG['refused'], absent=[VERIFIED]),
        Step('open http://nowhere.test/', loading('http://nowhere.test/'), MSG['dns']),
        Step('open http://bad host/', MSG['url-space'], check=nothing_fetched),
        Step('forward', title('About the fixtures'), check=nothing_fetched),
        Step('back', title('Undertow Fixture Home')),
        Step('open ' + H('/long'), loading(H('/long')), title('A long page')),
        Step('forward', MSG['no-forward'], absent=[title('About the fixtures')], check=nothing_fetched),
        Step('back', title('Undertow Fixture Home')),
        Step('q', wait=SH),
    ]),
    Scenario('reload', 'reload and r fetch again; a reload that fails keeps the page', [
        Step('undertow ' + H('/counter'), title('Counter'), check=remember('n', r'fetched (\d+) times')),
        Step('reload', loading(H('/counter')), title('Counter'), check=counter_went_up('n')),
        Step('r', loading(H('/counter')), title('Counter'), check=counter_went_up('n')),
        Step('open ' + H('/once?id=reload'), title('Only once'), 'first and only good fetch of reload'),
        Step('reload', loading(H('/once?id=reload')), MSG['early-eof']),
        Step('url', H('/once?id=reload')),
        Step('top', 'first and only good fetch of reload'),
        Step('q', wait=SH),
    ]),
    Scenario('paging', 'Enter and next, prev, top through 400 lines; the position line', [
        Step('undertow ' + H('/long'), title('A long page'), 'Line 001: the first line', POSITION,
             check=position(expect_start=1, key='p1')),
        Step('', POSITION, absent=['Line 001:'], check=position(after='p1', key='p2')),
        Step('next', POSITION, check=position(after='p2', key='p3')),
        Step('prev', POSITION, check=position(same_as='p2')),
        Step('top', 'Line 001: the first line', POSITION, check=position(expect_start=1)),
        Step('find the last line', 'Line 400: the last line'),
        Step('q', wait=SH),
    ]),
    Scenario('find', 'find (ASCII case-insensitive) from the position, findnext, wrapping once, nothing found', [
        Step('undertow ' + H('/long'), title('A long page')),
        Step('find needle', 'Line 120: here is the first needle', check=nothing_fetched),
        Step('findnext', 'Line 240: here is the second NEEDLE'),
        Step('findnext', 'Line 360: here is the third Needle'),
        Step('findnext', 'Line 120: here is the first needle'),
        Step('find no such words', MSG['not-found']),
        Step('q', wait=SH),
    ]),
    Scenario('anchors', 'an address with a fragment (never sent), fragment links on the page (no fetch), a missing '
                        'anchor, the same page by another address', [
        Step('undertow ' + H('/anchors#section-3'), loading_from(H('/anchors')), title('Anchors'), 'Section 3',
             'Text of section 3, paragraph 1.', absent=['Section 1', 'Section 2'], check=fetched('http', '/anchors')),
        Step('2', 'Section 2', 'Text of section 2, paragraph 1.', absent=['Loading'], check=nothing_fetched),
        Step('3', absent=['Loading'], check=nothing_fetched),
        Step('4', loading_from(H('/anchors?again=1')), 'Section 4', 'Text of section 4, paragraph 1.',
             absent=['Section 3'], check=fetched('http', '/anchors')),
        Step('q', wait=SH),
    ]),
    Scenario('history', 'history lists the pages, the current one marked', [
        Step('undertow ' + H('/'), title('Undertow Fixture Home')),
        Step('open ' + H('/about'), title('About the fixtures')),
        Step('open ' + H('/docs/'), title('The documents')),
        Step('history', 'Undertow Fixture Home', 'About the fixtures', 'The documents'),
        Step('back', title('About the fixtures')),
        Step('history', 'Undertow Fixture Home', 'About the fixtures', 'The documents'),
        Step('q', wait=SH),
    ]),
    Scenario('redirects', 'five redirects and six, a loop, relative, every code, https to http (stops, then opened '
                          'on purpose), http to https, another name, fragments kept or replaced', [
        Step('undertow ' + H('/redirect/5'), loading(H('/redirect/5')), title('The end of the redirects'),
             H('/redirect/0'), check=fetched('http', '/redirect/0')),
        Step('open ' + H('/redirect/6'), loading(H('/redirect/6')), MSG['redirects'],
             absent=[title('The end of the redirects')],
             check=lambda ctx: (not _recs(ctx, 'http', '/redirect/0') and len([r for r in _recs(ctx, 'http')
                                if r['request_line']]) == 6, 'six requests, and none for /redirect/0')),
        Step('open ' + H('/redirect/loop-a'), loading(H('/redirect/loop-a')), MSG['loop']),
        Step('open ' + H('/redirect/relative'), title('About the fixtures'), H('/about')),
        *[Step('open ' + H(f'/redirect/code/{c}'), title('About the fixtures'), H('/about'))
          for c in (301, 302, 303, 307, 308)],
        Step('open ' + S('/redirect/to-http'), loading(S('/redirect/to-http')), MSG['downgrade'], 'open ' + H('/about'),
             absent=[title('About the fixtures')], check=lambda ctx: (not _recs(ctx, 'http'), 'it went to http')),
        Step('open ' + H('/about'), loading(H('/about')), title('About the fixtures'), HTTP_PLAIN),
        Step('open ' + H('/redirect/to-https'), title('About the fixtures'), HTTPS_ANY, S('/about')),
        Step('open ' + S('/redirect/to-www'), HTTPS_ANY, url('https', '/about', 'www.fixture.test'),
             check=fetched('https', '/about', host='www.fixture.test:{https}', sni='www.fixture.test')),
        Step('open ' + H('/redirect/fragment#section-3'), title('Anchors'), H('/anchors#section-3'), 'Section 3',
             absent=['Section 1']),
        Step('open ' + H('/redirect/with-fragment#section-3'), title('Anchors'), H('/anchors#section-4'), 'Section 4'),
        Step('open ' + H('/redirect/space'), loading(H('/redirect/space')), title('A space in the address'),
             H('/space%20here'), check=fetched('http', '/space%20here')),
        Step('open ' + H('/redirect/no-location'), loading(H('/redirect/no-location')), MSG['bad-response'],
             absent=[title('About the fixtures')]),
        Step('q', wait=SH),
    ]),
    Scenario('status-pages', 'a 404 and a 500 are pages with their status; 204; escapes in a reason phrase', [
        Step('undertow ' + H('/missing'), title('Not Found'), status(404, 'Not Found'), 'There is no page at /missing.'),
        Step('open ' + H('/status/500'), status(500, 'Internal Server Error'), 'This page came with status 500.'),
        Step('open ' + H('/status/300'), status(300, 'Multiple Choices'), 'This page came with status 300.'),
        Step('open ' + S('/status/404'), status(404, 'Not Found'), 'This page came with status 404.'),
        Step('open ' + H('/status/204'), status(204, 'No Content')),
        Step('open ' + H('/escape/reason'), '404', 'The reason phrase had escapes in it.',
             raw_absent=[b'\x1b[2JFound', b'\x07', b'\x1b[6n']),
        Step('q', wait=SH),
    ]),
    Scenario('framing', 'Content-Length (once and twice), chunked with extensions and a trailer, close-delimited, '
                        'HTTP/1.0, interim responses (2 and 8), over HTTP and HTTPS; then every malformed response '
                        'refused with the last page kept', [
        Step('undertow ' + H('/frame/length'), 'Content-Length framing: the body arrived whole.'),
        Step('open ' + H('/frame/length-twice'), 'Content-Length given twice with one value: the body arrived whole.'),
        Step('open ' + H('/frame/chunked'), 'Chunked transfer coding: three chunks, with extensions and a trailer.'),
        Step('open ' + S('/frame/chunked'), HTTPS_ANY, 'Chunked transfer coding: three chunks'),
        Step('open ' + H('/frame/close'), 'Close-delimited framing: the body arrived whole.'),
        Step('open ' + S('/frame/close'), 'Close-delimited framing: the body arrived whole.'),
        Step('open ' + H('/frame/http10'), 'An HTTP/1.0 response: the body arrived whole.'),
        Step('open ' + H('/frame/interim'), 'After two interim responses: the body arrived whole.'),
        Step('open ' + H('/frame/interim-8'), 'After 8 interim responses: the body arrived whole.'),
        *[Step('open ' + H(p), loading(H(p)), MSG[m], absent=['the body arrived whole', 'early end.'])
          for p, m in (('/frame/length-conflict', 'bad-response'), ('/frame/length-bad', 'bad-response'),
                       ('/frame/length-negative', 'bad-response'), ('/frame/length-20-digits', 'bad-response'),
                       ('/frame/chunked-huge', 'bad-response'), ('/frame/chunked-and-length', 'bad-response'),
                       ('/frame/te-gzip', 'bad-response'), ('/frame/interim-9', 'interim'),
                       ('/frame/101', 'bad-response'), ('/frame/early-eof', 'early-eof'),
                       ('/frame/early-eof-chunked', 'early-eof'), ('/frame/early-eof-head', 'bad-response'),
                       ('/frame/folded', 'bad-response'), ('/frame/huge-headers', 'headers'),
                       ('/frame/bad-status', 'bad-status'), ('/frame/no-status-line', 'no-status'))],
        Step('url', H('/frame/interim-8')),
        Step('q', wait=SH),
    ]),
    Scenario('sizes', '1 MiB loads (its text cut at 512 KB with a notice), 1 MiB + 1 is refused in every framing', [
        Step('undertow ' + H('/size/1m'), '0000000 one mebibyte of text', POSITION, timeout=60),
        Step('find the rest of this page', MSG['truncated'], timeout=30),
        *[Step('open ' + H(p), loading(H(p)), MSG['too-big'], timeout=60)
          for p in ('/size/1m+1', '/size/1m+1-chunked', '/size/1m+1-close')],
        Step('url', H('/size/1m')),
        Step('open ' + S('/size/1m-chunked'), '0000000 one mebibyte of text', timeout=60),
        Step('q', wait=SH),
    ]),
    Scenario('content-types', 'image/png, gzip, binary and octet-stream refused with the reason; no type, plain text '
                              'and XHTML shown', [
        Step('undertow ' + H('/image.png'), loading(H('/image.png')), 'image/png ' + 'is not text'),
        Step('open ' + H('/gzip'), loading(H('/gzip')), MSG['gzip']),
        Step('open ' + H('/binary'), loading(H('/binary')), MSG['not-text-sniffed']),
        Step('open ' + H('/octet'), loading(H('/octet')), 'application/octet-stream ' + 'is not text'),
        Step('open ' + H('/no-type'), 'This page came without a Content-Type header.'),
        Step('open ' + H('/plain.txt'), 'Plain text, shown as it is.', '<b>Not markup</b> &amp; no entities.'),
        Step('open ' + H('/xhtml'), 'XHTML is read as HTML.'),
        Step('q', wait=SH),
    ]),
    Scenario('charsets', 'UTF-8, Latin-1 (meta), windows-1252 (header), Shift_JIS (its ASCII and a notice): on the '
                         'console what its character set has (code page 437 and its aliases: dashes and quotes) and '
                         'ASCII stand-ins for the rest (\u00a9, \u20ac); in a telnet session every character', [
        Step('undertow ' + H('/utf8'), 'Gr\u00fc\u00dfe aus Z\u00fcrich \u2014 \u201cquoted\u201d and \u2018single\u2019\u2026 '
                                       '\u00bd \u00b0 \u00b1 caf\u00e9.'),
        Step('open ' + H('/latin1'), 'Latin-1: caf\u00e9, na\u00efve, \u00bd, (c).'),
        Step('open ' + H('/windows-1252'), '\u201cSmart quotes\u201d, caf\u00e9, EUR5 and an en dash \u2013 here.'),
        Step('open ' + H('/shift_jis'), 'ASCII survives', MSG['sjis']),
        Step('q', wait=SH),
        Step('telnetd on', 'telnetd: listening', wait=SH),
        act(lambda ctx: ctx.open_telnet(80, 24), 'a telnet session, 80x24'),
        Step('undertow ' + H('/windows-1252'), '\u201cSmart quotes\u201d, caf\u00e9, \u20ac5 and an en dash \u2013 here.',
             on='telnet'),
        Step('open ' + H('/latin1'), 'Latin-1: caf\u00e9, na\u00efve, \u00bd, \u00a9.', on='telnet'),
        Step('open ' + H('/split'), SPLIT_TEXT_UTF8, on='telnet', timeout=60),
        Step('q', on='telnet', wait=TN),
        act(lambda ctx: ctx.close_telnet(), 'the telnet session ends'),
        Step('telnetd off', wait=SH),
    ]),
    Scenario('splits', 'a page that arrives one byte per TCP segment, and over HTTPS one byte per TLS record: the '
                       'status line, headers, chunk lines, UTF-8 and entities split at every byte keep their meaning', [
        Step('undertow ' + H('/split'), loading(H('/split')), title('Split everywhere'), SPLIT_TEXT,
             'The end of the split page.', timeout=60, check=fetched('http', '/split')),
        Step('open ' + S('/split'), loading(S('/split')), HTTPS_ANY, SPLIT_TEXT, 'The end of the split page.',
             timeout=90),
        Step('q', wait=SH),
    ]),
    Scenario('escapes', 'escape sequences, C1 and bidi controls in a title, the text, labels and an href never reach '
                        'the terminal', [
        Step('undertow ' + H('/escape'), title('Escapes'), 'Before.', 'UT-SGR', 'The end of the escapes.',
             raw_absent=wf.ESCAPE_BYTES),
        Step('links', H('/about'), raw_absent=wf.ESCAPE_BYTES),
        Step('url', H('/escape'), raw_absent=wf.ESCAPE_BYTES),
        Step('q', wait=SH),
        Step('undertow --dump ' + H('/escape'), 'Before.', 'The end of the escapes.', wait=SH, raw_absent=wf.ESCAPE_BYTES),
    ]),
    Scenario('long-fields', 'a 3000-byte title full of escape sequences, links of 2000 and 2100 bytes, a 5000-byte '
                            'word, then a line over 4096 bytes whose tail is quit: bounded, safe, the tail never runs', [
        Step('undertow ' + H('/long-fields'), title('UT-LONG-TITLE-START'), POSITION, raw_absent=wf.LONG_ESCAPES,
             absent=['UT-LONG-TITLE-END']),
        Step('links', 'An address of 2000 bytes', H('/about?x=xxxx'), raw_absent=wf.LONG_ESCAPES),
        Step('1', loading_from(H('/about?x=xxxx')), title('About the fixtures'), check=fetched('http', '/about')),
        Step('back', title('UT-LONG-TITLE-START'), raw_absent=wf.LONG_ESCAPES),
        Step('2', absent=['Loading'], check=nothing_fetched),
        Step('find UT-WORD-END', re.compile(r'w{10}UT-WORD-END')),
        Step(lambda ctx: 'url' + ' ' * 4093 + 'quit', MSG['too-long'], timeout=60),
        Step('history', 'UT-LONG-TITLE-START', raw_absent=wf.LONG_ESCAPES, absent=['UT-LONG-TITLE-END']),
        Step('url', H('/long-fields')),
        Step('q', wait=SH),
    ]),
    Scenario('links-limit', '2048 links are numbered, the 2049th keeps its text without a number', [
        Step('undertow ' + H('/links/2049'), title('2049 links'), 'link 1[1]', timeout=40),
        Step('2048', loading(H('/links/target?n=2048')), 'You followed link 2048.'),
        Step('back', title('2049 links')),
        Step('2049', MSG['no-link'], absent=['Loading'], check=nothing_fetched),
        Step('find link 2049', '* link 2049', absent=['[2049]']),
        # 10/15/26 (review M2): links says the real cutoff, and numbers from 1000 on keep the labels in one column
        Step('links', 'Links on this page: 2048 (the ones after the first 2048 have no number)',
             re.compile(r'(?<! )   1\. link 1\n {6}http'), re.compile(r'(?<! ) 999\. link 999\n'),
             re.compile(r'(?<! )1000\. link 1000\n'), re.compile(r'(?<! )2048\. link 2048\n {6}http'),
             absent=['2049.'], timeout=180),
        Step('q', wait=SH),
    ]),
    Scenario('link-byte-limit', 'long resolved links exhaust the separate 1 MiB string budget before 2048 links', [
        Step('undertow', 'To read a page'),
        Step(lambda ctx: 'open ' + fill(H(''), ctx) + wf.bytecap_path(fill(H(''), ctx)),
             title('The byte cap'), 'l1[1]', re.compile(r'only its first [1-5][0-9]{2} links have numbers'), timeout=60),
        Step('1', 'This is target 1 of the byte cap.'),
        Step('back', title('The byte cap')),
        Step('600', MSG['no-link'], absent=['Loading'], check=nothing_fetched),
        Step('find l600', '* l600', absent=['[600]']),
        Step('q', wait=SH),
    ]),
    Scenario('forms-scripts', 'form controls as placeholders with the notice; scripts, styles and templates never '
                              'shown, noscript shown', [
        Step('undertow ' + H('/forms'), title('A form'), '[text field]', '[button: Search]', '[select: First option]',
             '[checkbox]', '[text area]', MSG['forms']),
        Step('open ' + H('/scripts'), 'Shown without scripts.', 'Visible text after the scripts.',
             absent=['UT-SCRIPT-RAN', 'UT-STYLE-TEXT', 'UT-TEMPLATE-TEXT']),
        Step('q', wait=SH),
    ]),
    Scenario('tls-refused', 'a wrong name, expired and future certificates, an unknown root, a missing intermediate, '
                            'garbage and a hangup instead of a handshake, HTTP to a TLS port: never "HTTPS verified", '
                            'no HTTP request, no fallback', [
        Step('undertow ' + url('wrong-name'), loading(url('wrong-name')), MSG['wrong-name'], absent=[VERIFIED],
             check=no_http_on('wrong-name')),
        Step('open ' + url('expired'), loading(url('expired')), MSG['expired'], absent=[VERIFIED],
             check=no_http_on('expired')),
        Step('open ' + url('not-yet-valid'), loading(url('not-yet-valid')), MSG['not-yet-valid'], absent=[VERIFIED],
             check=no_http_on('not-yet-valid')),
        Step('open ' + url('unknown-ca'), loading(url('unknown-ca')), MSG['untrusted'], absent=[VERIFIED],
             check=no_http_on('unknown-ca')),
        Step('open ' + url('no-chain'), loading(url('no-chain')), MSG['untrusted'], absent=[VERIFIED],
             check=no_http_on('no-chain')),
        Step('open ' + url('garbage'), loading(url('garbage')), MSG['handshake'], absent=[VERIFIED],
             check=no_http_on('garbage')),
        Step('open ' + url('hangup'), loading(url('hangup')), MSG['handshake'], absent=[VERIFIED]),
        Step('open http://fixture.test:{https}/', loading('http://fixture.test:{https}/'), MSG['plain-to-tls'],
             absent=[VERIFIED]),
        Step('q', wait=SH),
    ]),
    Scenario('failures', 'a name that does not resolve, a refused connection, a stall (15 s without progress), '
                         'silence after the request, a byte every 10 s (the 60 s deadline), a TLS server that never '
                         'answers (the 20 s handshake bound)', [
        Step('undertow http://nowhere.test/', loading('http://nowhere.test/'), MSG['dns']),
        Step('open ' + url('refused'), loading(url('refused')), MSG['refused']),
        Step('open ' + H('/slow/stall'), loading(H('/slow/stall')), MSG['timeout'], timeout=40,
             check=gave_up('http', '/slow/stall', 13, 25)),
        Step('open ' + H('/slow/stall-head'), loading(H('/slow/stall-head')), MSG['timeout'], timeout=40,
             check=gave_up('http', '/slow/stall-head', 13, 25)),
        Step('open ' + H('/slow/trickle'), loading(H('/slow/trickle')), MSG['deadline'], timeout=100,
             check=gave_up('http', '/slow/trickle', 50, 75, since='connected')),
        Step('open ' + url('tls-stall'), loading(url('tls-stall')), MSG['timeout'], absent=[VERIFIED], timeout=45,
             check=gave_up('tls-stall', None, 17, 30, since='connected')),
        Step('q', wait=SH),
    ]),
    Scenario('repeated-failures', '40 failed loads leave no socket behind: a page still loads, netstat is clean', [
        Step('undertow', 'Undertow'),
        *[Step('open ' + url('refused'), loading(url('refused')), MSG['refused']) for _ in range(20)],
        *[Step('open ' + H('/frame/early-eof'), loading(H('/frame/early-eof')), MSG['early-eof'])
          for _ in range(20)],
        Step('open ' + H('/about'), title('About the fixtures')),
        Step('q', wait=SH),
        Step('netstat', NETSTAT_TCP, wait=SH, check=netstat_clean),
    ]),
    Scenario('interrupts', 'Ctrl+C while loading returns to the shell and closes the connection; Ctrl+C at the '
                           'prompt and Ctrl+D (EOF) end the browser', [
        Step('undertow ' + H('/slow/stall-head'), loading(H('/slow/stall-head')), wait=None),
        sleep(2),
        Step(b'\x03', wait=SH, check=gave_up('http', '/slow/stall-head', 0, 8, since='connected')),
        Step('netstat', NETSTAT_TCP, wait=SH, check=netstat_clean),
        Step('undertow ' + H('/about'), title('About the fixtures')),
        Step(b'\x03', wait=SH),
        Step('undertow', 'Undertow'),
        Step(b'\x04', wait=SH),
        Step('echo still here', re.compile(r'(?m)^still here$'), wait=SH),
    ]),
    Scenario('long-input', 'a line over 4096 bytes is refused whole: neither its start nor its tail runs', [
        Step('undertow ' + H('/about'), title('About the fixtures')),
        Step(_type_long('open ' + H('/long?') + '', 4200), MSG['too-long'], check=nothing_fetched, timeout=60),
        Step(lambda ctx: 'url' + ' ' * 4093 + 'quit', MSG['too-long'], timeout=60),
        Step('url', H('/about')),
        Step('q', wait=SH),
    ]),
    Scenario('bad-addresses', 'addresses url.c refuses (a space, another scheme, IPv6, credentials, 2049 bytes) are '
                              'refused with its message and change nothing', [
        Step('undertow ' + H('/about'), title('About the fixtures')),
        Step('open http://bad host/', MSG['url-space'], check=nothing_fetched),
        Step('open ftp://fixture.test/', MSG['url-scheme'], check=nothing_fetched),
        Step('open http://[::1]/', MSG['url-ipv6'], check=nothing_fetched),
        Step('open http://user@fixture.test:{http}/', MSG['url-credentials'], check=nothing_fetched),
        Step(lambda ctx: 'open ' + fill(H('/'), ctx) + 'a' * (2049 - len(fill(H('/'), ctx))), MSG['url-long'],
             check=nothing_fetched),
        Step('url', H('/about')),
        Step('q', wait=SH),
    ]),
    Scenario('clock', 'clock shows UTC; bad times are refused (config.c\'s messages); the session time is what '
                      'certificates are checked against', [
        Step('undertow', 'Undertow', '{today}'),
        Step('clock', '{today}', 'UTC'),
        Step('clock 2026-02-30 00:00:00', MSG['not-a-date']),
        Step('clock 2025-13-01 00:00:00', MSG['bad-month']),
        Step('clock yesterday', MSG['clock-form']),
        Step('clock {now-3h}'),
        Step('clock', '{now-3h:date}'),
        Step('open ' + S('/about'), loading(S('/about')), MSG['not-yet-valid'], absent=[VERIFIED],
             check=no_http_on('https')),
        Step('clock {now}'),
        Step('clock', '{today}'),
        Step('open ' + S('/about'), loading(S('/about')), title('About the fixtures'), HTTPS_ANY),
        Step('q', wait=SH),
    ]),
    Scenario('dump', 'undertow --dump: the text and the numbered addresses, exit 0 for 2xx, 1 otherwise, 2 for bad '
                     'usage; interactive mode needs a terminal', [
        Step('undertow --dump ' + H('/'), 'Undertow fixtures', 'About these fixtures[1]', H('/about'), wait=SH),
        Step('echo status $?', 'status 0', wait=SH),
        Step('undertow --dump ' + H('/status/404'), 'This page came with status 404.', wait=SH),
        Step('echo status $?', 'status 1', wait=SH),
        Step('undertow --dump ' + url('wrong-name'), MSG['wrong-name'], wait=SH, absent=[VERIFIED]),
        Step('echo status $?', 'status 1', wait=SH),
        Step('undertow --dump', 'usage', wait=SH),
        Step('echo status $?', 'status 2', wait=SH),
        Step('undertow --dump ' + H('/long'), 'Line 001: the first line', 'Line 400: the last line', wait=SH,
             timeout=60),
        Step('undertow ' + H('/') + ' > /dev/null', MSG['use-dump'], wait=SH),
        Step('echo status $?', 'status 2', wait=SH),
    ]),
    Scenario('two-consoles', 'the browser in a telnet session (80x24) and on the console at once: separate pages, '
                             'views sized to each terminal', [
        Step('telnetd on', 'telnetd: listening', wait=SH),
        act(lambda ctx: ctx.open_telnet(80, 24), 'a telnet session, 80x24'),
        Step('undertow ' + H('/long'), title('A long page'), POSITION, on='telnet', check=position(key='tn')),
        Step('undertow ' + S('/long'), title('A long page'), HTTPS_ANY, POSITION, check=position(key='con')),
        Step('', POSITION, on='telnet', check=position(after='tn')),
        Step('open ' + H('/about'), title('About the fixtures')),
        Step('find needle', 'Line 120: here is the first needle', on='telnet'),
        Step('q', on='telnet', wait=TN),
        Step('q', wait=SH,
             check=lambda ctx: (ctx.state['tn'][1] < ctx.state['con'][1],
                                f'telnet view {ctx.state["tn"]} vs console {ctx.state["con"]}: 24 rows show fewer lines')),
        act(lambda ctx: ctx.close_telnet(), 'the telnet session ends'),
        Step('telnetd off', wait=SH),
    ]),
    Scenario('concurrent', 'an HTTPS load in the background while the console looks up a name and asks for strong '
                           'randomness: the page arrives whole, nothing is left open', [
        Step('undertow --dump ' + S('/slow/delay?ms=6000') + ' &', '] started', wait=SH),
        Step('nettest resolve nowhere.test', 'resolve nowhere.test:', 'errno=', wait=SH),
        Step('nettest entropy supported', 'ENTROPY PASS', wait=SH),
        later('This page waited 6000 ms.', 40),
        Step('netstat', NETSTAT_TCP, wait=SH, check=netstat_clean),
    ]),
    Scenario('no-rdrand', 'without RDRAND: the welcome screen says HTTPS cannot work, HTTPS refuses, HTTP works', [
        Step('undertow', 'Undertow', MSG['entropy']),
        Step('open ' + S('/about'), loading(S('/about')), MSG['entropy'], absent=[VERIFIED], check=no_http_on('https')),
        Step('open ' + H('/about'), title('About the fixtures'), HTTP_PLAIN),
        Step('q', wait=SH),
    ], boot='no-rdrand'),
    Scenario('old-clock', 'a clock in 2004: HTTPS says how to set the time, clock fixes it for the session, the '
                          'hardware clock is left alone', [
        Step('undertow ' + S('/about'), loading(S('/about')), MSG['clock'], absent=[VERIFIED], check=no_http_on('https')),
        Step('clock', '2004-06-01'),
        Step('clock {now}'),
        Step('clock', '{today}'),
        Step('open ' + S('/about'), loading(S('/about')), title('About the fixtures'), HTTPS_ANY,
             check=fetched('https', '/about')),
        Step('q', wait=SH),
        Step('date', '2004-06-01', wait=SH),
    ], boot='old-clock'),
    Scenario('rtc-offset', 'an RTC on UTC-5 with rtc_offset_minutes=-300: UTC is right and HTTPS works (a leaf valid '
                           'from an hour ago would not be without the offset)', [
        Step('undertow', 'Undertow', '{today}'),
        Step('open ' + S('/about'), loading(S('/about')), title('About the fixtures'), HTTPS_ANY),
        Step('q', wait=SH),
    ], boot='rtc-offset'),
    Scenario('bad-config', 'a broken undertow.conf: a warning naming the line, then the defaults (no host table)', [
        Step('undertow http://10.0.2.2:{http}/about', MSG['conf-warning'], title('About the fixtures')),
        Step('open ' + H('/about'), loading(H('/about')), MSG['no-host'], absent=[title('The documents')]),
        Step('q', wait=SH),
    ], boot='bad-config'),
    Scenario('production', 'the normal initrd: its roots do not trust the fixture CA', [
        Step('ls /initrd/etc/ssl', 'cert.pem', wait=SH),
        Step('undertow --dump https://10.0.2.2:{https}/about', MSG['untrusted'], wait=SH,
             absent=[VERIFIED, 'The fixture servers listen']),
        Step('echo status $?', 'status 1', wait=SH),
        Step('undertow --dump http://10.0.2.2:{http}/about', 'The fixture servers listen on 127.0.0.1', wait=SH),
        Step('echo status $?', 'status 0', wait=SH),
    ], boot='production'),
    Scenario('wlansim', 'over the simulated Wi-Fi with no route through Ethernet: HTTP and HTTPS', [
        Step('undertow --dump ' + S('/about'), 'The fixture servers listen on 127.0.0.1', wait=SH),
        Step('echo status $?', 'status 0', wait=SH),
        Step('undertow --dump ' + H('/long'), 'Line 400: the last line', wait=SH, timeout=60),
        Step('wifi status', 'associated with "SurfSim"', wait=SH),
    ], boot='wlansim'),
    Scenario('wlansim-join', 'an HTTPS load over Ethernet while the Wi-Fi joins a WPA2 network on the same console: '
                             'the page arrives, the passphrase never shows', [
        Step(f'ifconfig eth0 10.0.2.16 255.255.255.0 10.0.2.2 {DNS_NOWHERE}', 'inet 10.0.2.16', wait=SH),
        Step('undertow --dump ' + S('/slow/delay?ms=8000') + ' &', '] started', wait=SH),
        Step('wifi join SurfSimWPA', 'Password for SurfSimWPA: ', wait=None),
        Step(lambda ctx: harness.WLANSIM_PASSPHRASE, 'joined SurfSimWPA', wait=SH, timeout=40,
             absent=[harness.WLANSIM_PASSPHRASE]),
        later('This page waited 8000 ms.', 40),
        Step('netstat', NETSTAT_TCP, wait=SH, check=netstat_clean),
    ], boot='wlansim'),
    Scenario('usbnet', 'over a USB Ethernet adapter (QEMU usb-net): HTTP and HTTPS', [
        Step('undertow --dump ' + S('/about'), 'The fixture servers listen on 127.0.0.1', wait=SH),
        Step('echo status $?', 'status 0', wait=SH),
        Step('undertow --dump ' + H('/long'), 'Line 400: the last line', wait=SH, timeout=60),
    ], boot='usbnet'),

    # -- 10/25/26: fetch, the downloader (user/fetch/), on the browser's own transport: HTTPS checked as Undertow
    # checks it (the same roots, versions and refusals), the fixtures' framing read as strictly, to standard output
    # (-o -) or /dev/null (the guests have no disk) --
    Scenario('fetch', 'fetch -o -: HTTP, HTTPS with TLS 1.3, 1.2 and the ECDSA chain, chunked, close-delimited and '
                      'HTTP/1.0 bodies, interim responses, a redirect, 1 MiB to /dev/null both framings; its requests', [
        Step('fetch -o - ' + H('/plain.txt'), 'Plain text, shown as it is.', '<b>Not markup</b> &amp; no entities.',
             wait=SH, check=fetched('http', '/plain.txt', agent='fetch/1.0')),
        Step('fetch -o - ' + url('tls13', '/frame/chunked'), 'Chunked transfer coding: three chunks, with extensions '
             'and a trailer.', wait=SH, check=fetched('tls13', '/frame/chunked', tls='TLSv1.3', agent='fetch/1.0')),
        Step('fetch -o /dev/null ' + url('tls12', '/size/1m'), 'fetch: 1.0 MB in ', ', TLSv1.2 -> /dev/null', wait=SH,
             timeout=60, check=fetched('tls12', '/size/1m', tls='TLSv1.2', agent='fetch/1.0')),
        Step('fetch -o /dev/null ' + url('https-ecdsa', '/size/1m-chunked'), 'fetch: 1.0 MB in ', ', TLSv1.',
             '-> /dev/null', wait=SH, timeout=60),
        Step('fetch -o - ' + S('/frame/close'), 'Close-delimited framing: the body arrived whole.', wait=SH),
        Step('fetch -o - ' + H('/frame/close-cut'), 'A body that only the end of the connection delimits, cut.',
             wait=SH),                  # over HTTP the end of the connection is the only end there is
        Step('fetch -o - ' + S('/frame/http10'), 'An HTTP/1.0 response: the body arrived whole.', wait=SH),
        Step('fetch -o - ' + S('/frame/interim'), 'After two interim responses: the body arrived whole.', wait=SH),
        Step('fetch -o - ' + S('/redirect/relative'), 'fetch: 302 ', ': to ' + S('/about'), 'About the fixtures',
             wait=SH),
        Step('echo status $?', 'status 0', wait=SH),
    ], browser=False),
    Scenario('fetch-refused', 'what fetch refuses, as Undertow does: certificates from an unknown root, for another '
                              'name, expired (no request sent); https to http; two lengths, a transfer coding, '
                              'folding, nine interim responses, an early end; a redirect loop; a 404; and (fetch '
                              'alone) a close-delimited HTTPS body ended without close_notify', [
        Step('fetch -o - ' + url('unknown-ca', '/'), MSG['untrusted'], wait=SH, check=no_http_on('unknown-ca')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('fetch -o - ' + url('wrong-name', '/'), 'HTTPS refused', wait=SH, check=no_http_on('wrong-name')),
        Step('fetch -o - ' + url('expired', '/'), MSG['expired'], wait=SH, check=no_http_on('expired')),
        Step('fetch -o - ' + S('/redirect/to-http'), 'redirects from HTTPS to unencrypted HTTP', wait=SH,
             check=fetched('https', '/redirect/to-http', agent='fetch/1.0')),
        Step('fetch -o - ' + S('/frame/length-conflict'), 'two different Content-Lengths', wait=SH),
        Step('fetch -o - ' + S('/frame/te-gzip'), 'a transfer coding other than chunked', wait=SH),
        Step('fetch -o - ' + S('/frame/folded'), 'obsolete line folding', wait=SH),
        Step('fetch -o - ' + S('/frame/interim-9'), 'more than 8 interim (1xx) responses', wait=SH),
        Step('fetch -o - ' + S('/frame/early-eof'), 'after 100 of 1000 bytes', wait=SH),
        Step('fetch -o - ' + S('/frame/close-cut'), 'ended its TLS connection without close_notify after 58 bytes: it '
             'may be cut short', wait=SH),
        Step('fetch -o - ' + S('/redirect/loop-a'), 'the address redirects in a loop (back to ', wait=SH),
        Step('fetch -o - ' + S('/status/404'), ': 404 Not Found', wait=SH),
        Step('echo status $?', 'status 1', wait=SH),
    ], browser=False),
    Scenario('fetch-production', 'the normal initrd: fetch does not trust the fixture CA either; HTTP works', [
        Step('fetch -o - https://10.0.2.2:{https}/about', MSG['untrusted'], wait=SH, check=no_http_on('https')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('fetch -o - http://10.0.2.2:{http}/plain.txt', 'Plain text, shown as it is.', wait=SH),
    ], boot='production', browser=False),
    Scenario('fetch-old-clock', 'a clock in 2004: fetch refuses HTTPS as Undertow does; -t gives the time for the run; '
                                'a time that is none is a usage error', [
        Step('fetch -o - ' + S('/about'), MSG['clock'], wait=SH, check=no_http_on('https')),
        Step('echo status $?', 'status 1', wait=SH),
        Step("fetch -t '{now}' -o - " + S('/about'), 'About the fixtures', wait=SH,
             check=fetched('https', '/about', agent='fetch/1.0')),
        Step("fetch -t '2026-02-30 10:00:00' -o - " + S('/about'), 'fetch: -t: ' + MSG['not-a-date'], wait=SH),
        Step('echo status $?', 'status 2', wait=SH),
        Step('date', '2004-06-01', wait=SH),
    ], boot='old-clock', browser=False),
    Scenario('fetch-no-rdrand', 'without RDRAND: fetch refuses HTTPS and says why; HTTP works', [
        Step('fetch -o - ' + S('/about'), MSG['entropy'], wait=SH, check=no_http_on('https')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('fetch -o - ' + H('/plain.txt'), 'Plain text, shown as it is.', wait=SH),
    ], boot='no-rdrand', browser=False),

    # -- 10/25/26: wundertow, the browser in a window (user/wundertow/), on Undertow's transport and settings: its
    # --layout mode shows a page as the window lays it out, with how it came, and refuses what Undertow refuses --
    Scenario('wundertow', 'wundertow --layout: HTTPS verified with TLS 1.3 and 1.2, the page as the window lays it '
                          'out (its heading twice the size, its links); a redirect followed; HTTP, not verified; '
                          'refused as Undertow refuses: an unknown root, another name, expired (no request sent), '
                          'HTTPS to HTTP', [
        Step('wundertow --layout ' + url('tls13', '/about'), 'address: ' + url('tls13', '/about'),
             'status: 200 verified TLSv1.3', 'title: About the fixtures', ' s32 b "About the fixtures"', 'link=0 "Home"',
             wait=SH, check=fetched('tls13', '/about', tls='TLSv1.3')),
        Step('echo status $?', 'status 0', wait=SH),
        Step('wundertow --layout ' + url('tls12', '/about'), 'status: 200 verified TLSv1.2', wait=SH,
             check=fetched('tls12', '/about', tls='TLSv1.2')),
        Step('wundertow --layout ' + S('/redirect/relative'), 'address: ' + S('/about'), 'status: 200 verified TLSv1.',
             'title: About the fixtures', wait=SH),
        Step('wundertow --layout ' + H('/about'), 'address: ' + H('/about'), 'status: 200 ', 'title: About the fixtures',
             wait=SH, absent=['verified']),
        Step('wundertow --layout ' + url('unknown-ca', '/'), MSG['untrusted'], wait=SH, absent=['status: '],
             check=no_http_on('unknown-ca')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('wundertow --layout ' + url('wrong-name', '/'), MSG['wrong-name'], wait=SH, absent=['status: '],
             check=no_http_on('wrong-name')),
        Step('wundertow --layout ' + url('expired', '/'), MSG['expired'], wait=SH, absent=['status: '],
             check=no_http_on('expired')),
        Step('wundertow --layout ' + S('/redirect/to-http'), 'redirected from HTTPS to unencrypted HTTP', wait=SH,
             absent=['status: ']),
        # 11/1/26 (stage 2.3): the same through a helper process, as the window loads: HTTPS verified, and refused with
        # the same sentence
        Step('wundertow --load ' + url('tls13', '/about'), 'address: ' + url('tls13', '/about'),
             'status: 200 verified TLSv1.3', 'title: About the fixtures', wait=SH,
             check=fetched('tls13', '/about', tls='TLSv1.3')),
        Step('wundertow --load ' + url('unknown-ca', '/'), MSG['untrusted'], wait=SH, absent=['status: '],
             check=no_http_on('unknown-ca')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('wundertow --load ' + S('/redirect/to-http'), 'redirected from HTTPS to unencrypted HTTP', wait=SH,
             absent=['status: ']),
        # 11/1/26 (stage 2.4): a page's pictures, each by a helper of its own, over verified HTTPS: one of each kind, by
        # a redirect too; a broken file, a page, a 404 and a compressed one fail; the one over http is refused and asks
        # the http server for nothing
        Step('wundertow --load ' + S('/pictures'), 'status: 200 verified TLSv1.', 'title: Pictures',
             'picture 0 loaded 4x3 kept 4x3 middle 00ff0000', 'picture 1 loaded 4x3 kept 4x3 middle 0000ff00',
             'picture 2 loaded 4x3 kept 4x3 middle 000000ff', 'picture 3 loaded 40x30',
             'picture 4 loaded 4x3 kept 4x3 middle 00ff0000', 'picture 5 failed', 'picture 6 failed', 'picture 7 failed',
             'picture 8 refused', 'picture 9 failed', wait=SH, check=no_picture_over_http),
        Step('wundertow --load ' + H('/pictures'), 'title: Pictures', 'picture 0 loaded 4x3', 'picture 8 loaded 4x3',
             wait=SH),
        # 11/01/26 (stage 3.3): a page's sheet from the network and what it imports, over HTTPS; one over http on an
        # https page is never loaded
        Step('wundertow --load ' + url('tls13', '/styled'), 'color=123456', 'color=654321', wait=SH,
             check=fetched('tls13', '/styled2.css', tls='TLSv1.3')),
        Step('wundertow --load ' + url('tls13', '/styled-http'), 'Not styled by a sheet over http.', wait=SH,
             absent=['color=123456']),
        Step('wundertow --load ' + url('tls13', '/styled-404'), 'Not styled by a 404.', wait=SH, absent=['color=123456']),
    ], browser=False),
    Scenario('wundertow-production', 'the normal initrd: wundertow does not trust the fixture CA either; HTTP works', [
        Step('wundertow --layout https://10.0.2.2:{https}/about', MSG['untrusted'], wait=SH, absent=['status: '],
             check=no_http_on('https')),
        Step('echo status $?', 'status 1', wait=SH),
        Step('wundertow --layout http://10.0.2.2:{http}/about', 'status: 200 ', 'title: About the fixtures', wait=SH),
    ], boot='production', browser=False),
]

# the guests: (initrd, CPU with RDRAND, the RTC, kernel words, a USB Ethernet adapter instead of the e1000, setup)
WLAN_SETUP = [('wifi up', 'wlan0: up'), ('wifi join SurfSim', 'joined SurfSim'), ('dhcp wlan0', 'lease 10.0.2.15'),
              (f'ifconfig wlan0 10.0.2.15 255.255.255.0 10.0.2.2 {DNS_NOWHERE}', f'dns {DNS_NOWHERE}')]
BOOTS = {
    'main': ('test', True, 'utc', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'no-rdrand': ('test', False, 'utc', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'old-clock': ('test', True, '2004-06-01T12:00:00', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'rtc-offset': ('offset', True, 'utc-5h', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'bad-config': ('bad-config', True, 'utc', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'production': ('normal', True, 'utc', '', False, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
    'wlansim': ('test', True, 'utc', 'wlansim', False, WLAN_SETUP),
    'usbnet': ('test', True, 'utc', '', True, [(STATIC_NET, f'dns {DNS_NOWHERE}')]),
}


def rtc_base(spec, now=None):
    """QEMU's -rtc base= for a boot: utc, utc-5h (the clock of a machine on US Central Daylight Time), a date"""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    if spec == 'utc':
        return 'utc'
    if spec == 'utc-5h':
        return (now - datetime.timedelta(hours=5)).strftime('%Y-%m-%dT%H:%M:%S')
    return spec


def fill(text, ctx):
    """{server} becomes its port; {today}, {now}, {now-3h} and {now-3h:date} the UTC time now (or 3 hours ago)"""
    now = datetime.datetime.now(datetime.timezone.utc)
    times = {'today': now.strftime('%Y-%m-%d'), 'now': now.strftime('%Y-%m-%d %H:%M:%S'),
             'now-3h': (now - datetime.timedelta(hours=3)).strftime('%Y-%m-%d %H:%M:%S'),
             'now-3h:date': (now - datetime.timedelta(hours=3)).strftime('%Y-%m-%d')}

    def sub(m):
        key = m.group(1)
        if key in ctx.ports:
            return str(ctx.ports[key])
        return times.get(key, m.group(0))
    return re.sub(r'\{([\w:-]+)\}', sub, text)


# ---- running --------------------------------------------------------------------------------------------------------

_CSI = re.compile(r'\x1b\[[0-9;?]*[ -/]*[@-~]')
_OSC = re.compile(r'\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)?')
_ESC = re.compile(r'\x1b[@-Z\\-_]')


def clean(raw):
    """what a terminal shows of raw bytes, as text: UTF-8 (errors replaced), escape sequences and CRs taken out"""
    t = raw.decode('utf-8', errors='replace')
    return _ESC.sub('', _CSI.sub('', _OSC.sub('', t))).replace('\r', '')


def describe(e):
    return e.pattern if isinstance(e, re.Pattern) else repr(e)


_LITERALS = {}


def literal(text):
    """a literal expectation as a pattern that still matches once the browser has wrapped it: the view breaks
    a long line at a space (the row ends, the next row may be indented), so each run of spaces in text
    matches any run of white space"""
    if text not in _LITERALS:
        _LITERALS[text] = re.compile(''.join(r'\s+' if part.startswith(' ') else re.escape(part)
                                             for part in re.split(r'( +)', text) if part))
    return _LITERALS[text]


def search(pattern, text, pos=0):
    """the first match of an expectation (a literal or a compiled pattern) in text at or after pos"""
    return (pattern if isinstance(pattern, re.Pattern) else literal(pattern)).search(text, pos)


def type_paced(term, data, send):
    """data typed as a person would rather than pasted: the tty's input queue holds 256 bytes and drops what does
    not fit (kernel/tty.c), so a long line goes in 32-byte pieces, each once the guest has been quiet for 0.12 s
    (the editor redraws its row after the first key it reads, long before it has read the rest: output alone
    says nothing; silence says it waits for more), or after 5 s"""
    if len(data) <= 64:
        send(data)
        return
    for i in range(0, len(data), 32):
        send(data[i:i + 32])
        last, changed, deadline = len(term.raw()), time.monotonic(), time.monotonic() + 5
        while time.monotonic() < deadline:
            term.pump(0.02)
            n = len(term.raw())
            if n != last:
                last, changed = n, time.monotonic()
            elif time.monotonic() - changed >= 0.12:
                break


class SerialTerm:
    def __init__(self, q):
        self.q = q

    def raw(self):
        return self.q.transcript

    def type(self, keys):
        type_paced(self, (keys + '\n').encode() if isinstance(keys, str) else keys, self.q.ser.sendall)

    def pump(self, t):
        self.q.pump(t)

    def alive(self):
        return self.q.alive()


class TelnetTerm:
    def __init__(self, t):
        self.t = t

    def raw(self):
        return self.t.data

    def type(self, keys):
        type_paced(self, keys + '\n' if isinstance(keys, str) else keys, self.t.send)

    def pump(self, t):
        self.t.pump(t)

    def alive(self):
        return not self.t.closed


class Context:
    def __init__(self, run, guest, scenario):
        self.run, self.guest, self.scenario, self.fx = run, guest, scenario, run.fx
        self.ports = run.fx.ports
        self.state = {}
        self.scenario_mark = self.mark = run.fx.mark()
        self.serial_start = len(guest.serial.raw())
        self.out, self.raw = '', b''

    def open_telnet(self, cols, rows):
        try:
            t = harness.Telnet(self.guest.q.ports['telnet'], cols=cols, rows=rows, timeout=20)
        except OSError as e:
            return False, f'telnet: {e}'
        self.guest.telnet = TelnetTerm(t)
        return t.expect(harness.TELNET_ROOT, 20), 'no telnet prompt'

    def close_telnet(self):
        if self.guest.telnet:
            self.guest.telnet.t.send('exit\n')
            self.guest.telnet.t.wait_closed(10)
            self.guest.telnet.t.close()
            self.guest.telnet = None
        return True, ''


class Guest:
    def __init__(self, name, q):
        self.name, self.q, self.telnet = name, q, None
        self.serial = SerialTerm(q)


WORDS = {True: 'PASS', False: 'FAIL', None: 'NOT RUN'}


class Run:
    def __init__(self, a, fx, initrds, outdir):
        self.a, self.fx, self.initrds, self.outdir = a, fx, initrds, outdir
        self.results = []           # (name, ok, why)
        self.boots = {}

    def say(self, ok, name, why=''):
        self.results.append((name, ok, why))
        print(f'{WORDS[ok]} {name}' + (f': {why}' if why and ok is not True else ''), flush=True)

    # -- guests --
    def boot(self, name):
        initrd, rdrand, rtc, words, usbnet, setup = BOOTS[name]
        extra = (['-cpu', 'max,+rdrand'] if rdrand else []) + ['-rtc', 'base=' + rtc_base(rtc)]
        path = str(self.initrds[initrd])
        if usbnet:
            q = harness.usbnet_later_boot(self.a.kernel, self.a.qemu, self.a.mem, extra, path, words.split())
        else:
            q = harness.Qemu(self.a.kernel, self.a.qemu, self.a.mem, extra, initrd=path, net=True, append=words)
        guest = Guest(name, q)
        self.boots[name] = self.boots.get(name, 0) + 1
        ok = q.expect(harness.PROMPT, self.a.boot_timeout)
        why = '' if ok else 'no shell prompt'
        if ok and usbnet:
            ok = harness.usbnet_plug_check(q)
            why = '' if ok else 'the USB adapter did not become eth0'
        for cmd, expect in setup if ok else []:
            q.wait(0.3)
            start = len(q.transcript)
            q.send(cmd + '\n')
            ok = q.expect(expect, 20) and q.expect(harness.PROMPT, 20)
            if not ok:
                why = f'{cmd}: no {expect!r}\n' + harness.tail(clean(q.transcript[start:]), 15)
                break
        return guest, ok, why

    def save(self, guest):
        n = self.boots[guest.name]
        stem = guest.name + ('' if n == 1 else f'-{n}')
        if self.outdir:
            (self.outdir / f'{stem}.raw').write_bytes(guest.q.transcript)
            (self.outdir / f'{stem}.txt').write_text(clean(guest.q.transcript))

    def stop(self, guest):
        if guest.telnet:
            guest.telnet.t.close()
        self.save(guest)
        guest.q.close()

    # -- steps --
    def wait_text(self, term, start, pos, pattern, timeout, stop=None):
        """the end of pattern's first match at or after pos in the text since start; None after timeout, and -1 as
        soon as stop (a prompt: the shell's, when the browser should be running) has ended the text for a second"""
        deadline = time.monotonic() + timeout
        last_len, last_change = -1, time.monotonic()
        while True:
            raw = term.raw()
            text = clean(raw[start:])
            m = search(pattern, text, pos)
            if m:
                return m.end()
            if len(raw) != last_len:
                last_len, last_change = len(raw), time.monotonic()
            elif stop is not None and time.monotonic() - last_change >= 1:
                ends = [m.end() for m in stop.finditer(text, pos)]
                if ends and not text[ends[-1]:].strip(' '):
                    return -1
            if time.monotonic() > deadline or not term.alive():
                return None
            term.pump(0.1)

    def wait_prompt(self, term, start, pos, pattern, timeout, last=False, quiet=0.3):
        """True once the prompt (pattern) shows after pos in the text since start, and then nothing more comes for
        quiet seconds. With last, the prompt must also be the very end of the text: for a step that expects nothing
        else, where an editor redrawing its line (prompt and all) as keys are typed could look like the end. The
        first prompt after the step's last expectation is the end otherwise: the kernel or a server (telnetd) may
        print a line on the console after it."""
        deadline = time.monotonic() + timeout
        last_len, last_change = -1, time.monotonic()
        while True:
            raw = term.raw()
            if len(raw) != last_len:
                last_len, last_change = len(raw), time.monotonic()
            elif time.monotonic() - last_change >= quiet:
                text = clean(raw[start:])
                ends = [m.end() for m in pattern.finditer(text, pos)]
                if ends and (not last or not text[ends[-1]:].strip(' ')):
                    return True
            if time.monotonic() > deadline or not term.alive():
                return False
            term.pump(0.1)

    def run_step(self, ctx, step):
        guest = ctx.guest
        if step.act:
            return step.act(ctx)
        term = guest.telnet if step.on == 'telnet' else guest.serial
        if term is None:
            return False, 'no telnet session'
        keys = step.keys(ctx) if callable(step.keys) else step.keys
        if isinstance(keys, str):
            keys = fill(keys, ctx)
        ctx.mark = self.fx.mark()
        start = len(term.raw())
        term.type(keys)
        pos = 0
        for e in step.expects:
            e = fill(e, ctx) if isinstance(e, str) else e
            # a browser step stops waiting once the shell's prompt is back: the browser has ended
            end = self.wait_text(term, start, pos, e, step.timeout, SH_PROMPT if step.wait == UT else None)
            if end is None or end < 0:
                ctx.out, ctx.raw = clean(term.raw()[start:]), term.raw()[start:]
                return False, f'did not see {describe(e)}' + (' (the browser ended: the shell\'s prompt is back)'
                                                              if end == -1 else '')
            pos = end
        if step.wait in (UT, SH, TN):
            pattern = UT_PROMPT if step.wait == UT else SH_PROMPT     # TN: a telnet session's shell (\u224b@>)
            if not self.wait_prompt(term, start, pos, pattern, step.timeout, last=not step.expects):
                ctx.out, ctx.raw = clean(term.raw()[start:]), term.raw()[start:]
                return False, f'no {step.wait} prompt at the end'
        ctx.raw = term.raw()[start:]
        ctx.out = clean(ctx.raw)
        for a in step.absent:
            a = fill(a, ctx) if isinstance(a, str) else a
            if search(a, ctx.out):
                return False, f'{describe(a)} appeared'
        for b in step.raw_absent:
            if b in ctx.raw:
                return False, f'the raw bytes {b!r} reached the terminal'
        if step.check:
            ok, why = step.check(ctx)
            if not ok:
                return False, why
        return True, ''

    def recover(self, guest):
        """back to the shell's prompt after a failure: Ctrl+C, quit, Ctrl+D, each followed by an echo that only the
        shell answers; True once it does"""
        if guest.telnet:
            guest.telnet.t.close()
            guest.telnet = None
        term = guest.serial
        for keys in (b'\x03', b'quit\n', b'\x04', b'\x03\x03'):
            term.type(keys)
            guest.q.wait(1)
            start = len(term.raw())
            term.type('echo ut-recovered')
            pos = self.wait_text(term, start, 0, re.compile(r'(?m)^ut-recovered$'), 6)
            if pos is not None and self.wait_prompt(term, start, pos, SH_PROMPT, 6):
                return True
        return False

    def run_scenario(self, guest, sc):
        ctx = Context(self, guest, sc)
        start = len(guest.q.transcript)
        ok, why, where = True, '', ''
        for i, step in enumerate(sc.steps):
            ok, why = self.run_step(ctx, step)
            if not ok:
                where = f'step {i + 1} ({fill(step.name, ctx)[:70]!r})'
                break
        if self.outdir:
            text = clean(guest.q.transcript[start:])
            if guest.telnet:
                text += '\n---- telnet session ----\n' + clean(guest.telnet.raw())
            (self.outdir / 'scenarios' / f'{sc.name}.txt').write_text(
                text + ('' if ok else f'\n---- FAILED at {where}: {why}\n---- the step\'s output ----\n{ctx.out}'))
        if not ok:
            print(f'\n---- {sc.name}: {where}: {why}\n---- its output (tail) ----\n' + harness.tail(ctx.out, 25) +
                  '\n----', flush=True)
        return ok, (f'{where}: {why}' if not ok else '')

    def stand_in(self, guest):
        """is /initrd/bin/undertow the foundation's stand-in, which cannot browse yet?"""
        term = guest.serial
        start = len(term.raw())
        term.type(fill('undertow --dump ' + H('/plumbing'), Context(self, guest, None)))
        self.wait_prompt(term, start, 0, SH_PROMPT, 60, last=True)
        return 'cannot browse yet' in clean(term.raw()[start:])

    def run(self, scenarios, run_anyway):
        by_boot = {}
        for sc in scenarios:
            by_boot.setdefault(sc.boot, []).append(sc)
        stand_in = None
        for boot_name in BOOTS:
            todo = by_boot.get(boot_name, [])
            if not todo:
                continue
            if stand_in and not run_anyway and all(sc.browser for sc in todo):
                for sc in todo:                     # no guest booted for nothing
                    self.say(None, sc.name, 'needs the browser (tracks R, H, T and S merged)')
                continue
            guest, ok, why = self.boot(boot_name)
            self.say(ok, f'boot {boot_name}', why)
            if not ok:
                self.stop(guest)
                for sc in todo:
                    self.say(False, sc.name, f'the {boot_name} guest did not come up')
                continue
            try:
                for n, sc in enumerate(todo):
                    if sc.browser and not run_anyway:
                        if stand_in is None:
                            stand_in = self.stand_in(guest)
                            if stand_in:
                                print(f'     /initrd/bin/undertow is the stand-in that cannot browse yet: the '
                                      f'scenarios that need the browser are not run', flush=True)
                        if stand_in:
                            self.say(None, sc.name, 'needs the browser (tracks R, H, T and S merged)')
                            continue
                    t0 = time.monotonic()
                    ok, why = self.run_scenario(guest, sc)
                    self.say(ok, f'{sc.name}: {sc.doc}', why + f' ({time.monotonic() - t0:.0f} s)' if not ok else '')
                    if not ok and not self.recover(guest):
                        print(f'     the {boot_name} guest does not answer: booting it again', flush=True)
                        self.stop(guest)
                        guest, ok, why = self.boot(boot_name)
                        if not ok:
                            self.say(False, f'boot {boot_name} again', why)
                            for rest in todo[n + 1:]:
                                self.say(False, rest.name, f'the {boot_name} guest did not come up again')
                            break
            finally:
                self.stop(guest)


# ---- the public interoperability check (--public): never part of --test or make test-web ------------------------
# Public pages through QEMU's user network: DHCP's name server forwards to this computer's resolver, so this does
# what the deterministic suite never does, use public DNS and the Internet. Run it on purpose, by hand.
PUBLIC = [
    ('http://example.com/', 'plain HTTP'),
    ('http://neverssl.com/', 'plain HTTP, a site that never redirects to HTTPS'),
    ('https://example.com/', 'HTTPS'),
    ('https://en.wikipedia.org/wiki/Surfing', 'a large HTTPS page'),
    ('https://tls-v1-2.badssl.com:1012/', 'a server that speaks TLS 1.2 only'),
    ('https://text.npr.org/', 'a text-friendly news site'),
    ('https://lite.cnn.com/', 'another text-friendly news site'),
]


def public_check(a, outdir):
    """--public: the production initrd (--initrd), -cpu max, DHCP, then undertow --dump of each page of PUBLIC with
    its text sent to /dev/null: the status line (final address, status, HTTPS verified and the TLS version) or the
    error, the exit status and the time from the command to the shell's prompt. A table; 0 when every page loaded."""
    q = harness.Qemu(a.kernel, a.qemu, a.mem, ['-cpu', 'max,+rdrand', '-rtc', 'base=utc'], initrd=a.initrd, net=True)
    rows, ok_all = [], True
    try:
        if not q.expect(harness.PROMPT, a.boot_timeout):
            print('the guest did not come up')
            return 1
        q.send('dhcp\n')
        if not (q.expect('lease', 20) and q.expect(harness.PROMPT, 20)):
            print('dhcp: no lease')
            return 1
        for address, what in PUBLIC:
            q.wait(0.3)
            start, t0 = len(q.transcript), time.monotonic()
            q.send(f'undertow --dump {address} > /dev/null\n')
            done = q.expect(harness.PROMPT, 90)
            took = time.monotonic() - t0
            q.send('echo status $?\n')
            q.expect(harness.PROMPT, 10)
            out = clean(q.transcript[start:])
            status = re.search(r'status (\d+)', out)
            line = re.search(r'undertow: (\S+): (HTTP \d+.*)', out)
            error = re.search(r'undertow: (.*)', out)
            result = line.group(2).strip() if line else (error.group(1).strip() if error else '(nothing)')
            final = line.group(1) if line else ''
            code = status.group(1) if status else '?'
            ok_all = ok_all and done and code == '0'
            rows.append((address, what, final, result, code, f'{took:.1f} s' if done else 'no prompt in 90 s'))
            print(f'{"PASS" if done and code == "0" else "FAIL"} {address} ({what}): {result} -> {final or "-"}, '
                  f'exit {code}, {rows[-1][5]}', flush=True)
    finally:
        if outdir:
            (outdir / 'public.txt').write_text(clean(q.transcript))
        q.close()
    when = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
    print(f'\nPublic check, {when} (QEMU user network, -cpu max, the production roots):\n')
    print('| Address | What | Final address | Status line | Exit | Time |\n| --- | --- | --- | --- | --- | --- |')
    for r in rows:
        print('| ' + ' | '.join(x.replace('|', '/') for x in r) + ' |')
    return 0 if ok_all else 1


def check_normal_initrd(path):
    """the normal initrd must hold no fixture CA: (ok, why)"""
    try:
        holders = fixture_ca_in(path)
    except (InitrdError, tarfile.TarError, OSError) as e:
        return False, f'{path}: {e}'
    return (not holders, f'{path} holds a fixture CA in ' + ', '.join(holders))


def list_scenarios():
    for sc in SCENARIOS:
        print(f'{sc.name:20} {sc.boot:11} {"browser" if sc.browser else "plumbing"}  {sc.doc}')


def interactive(a):
    with tempfile.TemporaryDirectory(prefix='ut-web-') as tmp, wf.Fixtures() as fx:
        initrds = test_initrds(a.initrd, fx, tmp)
        print(f'the fixtures (this computer\'s 127.0.0.1, the guest\'s 10.0.2.2):')
        for name, _, _, _, doc in wf.SERVERS:
            print(f'  {name:14} {fx.url(name)}  {doc}')
        print(f'\nin the guest: {STATIC_NET}\n          then: undertow {fx.url("http")}\nCtrl-A x ends QEMU.\n')
        cmd = [a.qemu, '-m', str(a.mem), '-kernel', os.path.abspath(a.kernel), '-initrd', 'initrd.tar',
               '-cpu', 'max,+rdrand', '-rtc', 'base=utc', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
               '-nographic', '-no-reboot']
        # QEMU splits -initrd at commas and spaces: run it where the initrd has a plain name
        return subprocess.call(cmd, cwd=initrds['test'].parent)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kernel', default='build/O0/surfos.bin')
    ap.add_argument('--initrd', default='build/images/initrd.tar', help='the normal initrd (only read)')
    ap.add_argument('--qemu', default='qemu-system-i386')
    ap.add_argument('--mem', default='256')
    ap.add_argument('--boot-timeout', type=float, default=60)
    ap.add_argument('--output-dir', help='transcripts, the fixtures\' log and the summary (build/web-test/O<n>)')
    ap.add_argument('--scenario', action='append', help='only this scenario (again for more)')
    ap.add_argument('--run-anyway', action='store_true', help='run the browser scenarios with the stand-in too')
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--test', action='store_true', help='run the scenarios')
    mode.add_argument('--list', action='store_true', help='list the scenarios')
    mode.add_argument('--interactive', action='store_true', help='the fixtures and a guest on this terminal')
    mode.add_argument('--public', action='store_true', help='the public interoperability check: public pages over '
                      'the Internet with the production initrd (never part of --test)')
    a = ap.parse_args(argv)
    if a.list:
        list_scenarios()
        return 0
    if a.interactive:
        return interactive(a)
    if a.public:
        out = Path(a.output_dir).resolve() if a.output_dir else None
        if out:
            out.mkdir(parents=True, exist_ok=True)
        return public_check(a, out)
    scenarios = SCENARIOS
    if a.scenario:
        names = {sc.name for sc in SCENARIOS}
        unknown = [n for n in a.scenario if n not in names]
        if unknown:
            print('no such scenario: ' + ', '.join(unknown) + ' (--list)')
            return 2
        scenarios = [sc for sc in SCENARIOS if sc.name in a.scenario]
    outdir = None
    if a.output_dir:
        outdir = Path(a.output_dir).resolve()
        if outdir.is_dir() and (REPO / 'build') in outdir.parents:
            shutil.rmtree(outdir)                  # an earlier run's, under build/: never anything else
        (outdir / 'scenarios').mkdir(parents=True, exist_ok=True)
    before = sha256(a.initrd)
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='ut-web-') as tmp, wf.Fixtures() as fx:
        run = Run(a, fx, None, outdir)
        ok, why = check_normal_initrd(a.initrd)
        run.say(ok, 'the normal initrd holds no fixture CA', why)
        try:
            run.initrds = test_initrds(a.initrd, fx, tmp)
            run.say(True, 'test initrds made from a copy of the normal one')
        except (InitrdError, tarfile.TarError, OSError) as e:
            run.say(False, 'test initrds made from a copy of the normal one', str(e))
        if run.initrds:
            ok, why = check_normal_initrd(run.initrds['normal'])
            if not fixture_ca_in(run.initrds['test']):
                ok, why = False, 'the test initrd has no fixture CA'
            run.say(ok, 'only the test initrds hold the fixture CA', why)
            run.run(scenarios, a.run_anyway)
        if outdir:
            fx.dump_log(outdir / 'fixtures.jsonl')
    after = sha256(a.initrd)
    run.say(before == after, 'the normal initrd is the same file afterwards', f'SHA-256 {before} became {after}')
    passed = sum(1 for _, ok, _ in run.results if ok is True)
    failed = [n for n, ok, _ in run.results if ok is False]
    not_run = [n for n, ok, _ in run.results if ok is None]
    took = f'{time.monotonic() - t0:.0f} s'
    if failed:
        summary = f'WEB TEST FAILED: {len(failed)} of {len(run.results)} checks ({took})'
    elif not_run:
        summary = f'WEB TEST INCOMPLETE: {passed} checks passed, {len(not_run)} scenarios not run ({took})'
    else:
        summary = f'WEB TEST PASSED: {passed} checks ({took})'
    print('\n' + summary)
    if outdir:
        lines = [f'{WORDS[ok]} {n}' + (f': {w}' if w and ok is not True else '') for n, ok, w in run.results]
        (outdir / 'summary.txt').write_text('\n'.join(lines) + '\n\n' + summary + '\n')
    return 0 if not failed and not not_run else 1


if __name__ == '__main__':
    sys.exit(main())
