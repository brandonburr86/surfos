#!/usr/bin/env python3
"""
SurfCPU physical acceptance tests: one command per board milestone, its evidence saved
File: board-test.py Date: 10/16/26 (SurfCPU, the os track)

  board-test.py MILESTONE [--port PORT] [options]          (docs/PHYSICAL-TESTS.md)
  make -C surfcpu board-test M=MILESTONE [PORT=/dev/cu.usbserial-<serial>1] [ARGS="..."]

PORT defaults to auto: the only Arty UART plugged in (uart.py list).

MILESTONE (AGENT-INSTRUCTIONS.md section 10, the exit evidence the serial port can show), and the
design it asks the owner to load (--design names another):

  m1  arty_a7_baseline: N lines of its 1 Hz line, their form, count, timing and fields;
      a byte typed comes back as rx; the switches, a button and RESET as the owner works them
  m3  surfcpu_arty_a7_bramdiag, the BRAM diagnostics: every section to BRAMDIAG: PASS, the
      timer's progress, the deliberate faults recovered, typed bytes echoed with their codes
      (--design surfcpu_arty_a7_ddr_bramdiag: the same with the DDR3L's sections)
  m5  surfcpu_arty_a7_ddr, SurfBoot from the card: a missing and an invalid image diagnosed in
      the monitor, the DDR diagnostic booted from the card to DDRDIAG: PASS, a missing card
      diagnosed and the boot retried when it is put back
  m7  surfcpu_arty_a7_ddr, SurfOS from the card to the shell: the boot, the two-task test, the
      uptime advancing, the PS/2 keyboard typing into the shell
  m8  surfcpu_arty_a7_ddr, OWNER-GUIDE.md section 8's CRC procedure: a Mac file read on the
      board, files made and changed, the same CRCs after a power cycle, on the Mac and after
      a kernel update
  m9  surfcpu_arty_a7_ddr in the configuration flash: N cold boots, each one checked (--boots,
      default 10), and a soak (--minutes, default 60) of shell, task and storage commands
      with CRC checks

The bitstream is compared with the design's build manifest (build/fpga/BOARD/DESIGN/manifest.txt,
or --manifest): the script refuses to start when there is none, or when it is another design's,
since a run whose bitstream cannot be identified must not pass (make board-test passes DESIGN=
as --design, as make program-volatile takes it).

Each procedure opens the port first, so nothing the board prints afterwards is missed (a
thread reads it all the time, also while a question waits for the owner), then asks the
owner to do each step and waits for Enter (or y, n or s for something only a person can
see: yes, no, could not see it). It
checks what the port shows and writes docs/evidence/physical/<milestone>-<UTC date>.txt
(-2, -3 ... for further runs that day): the verdict, the build identity it read and what
it was compared with (the bitstream's build manifest, SURFOS.BIN, DDRDIAG.BIN), the steps
the owner was asked to do and the answers, every check, and the transcript with the time
of each line. PASS covers only what the script saw; what the owner reported is listed as
the owner's, and a step the owner could not do or answered no makes the run FAIL or
INCOMPLETE, never PASS. A run against the RTL system (the SoC says BUILD_FLAGS.SIM) is
labelled SIMULATION and, unless --evidence-dir says otherwise, goes to build/board-test/,
never to the physical evidence.

The port: uart.py's (115200 8N1 raw, typed bytes paced). A port that goes away is a FAIL at
once ("USB unplugged, or the board lost power?"), except during a step that may take it away
(programming, a power cycle, RESET with the card out) and after the owner's answer until the
board's next expected line arrives: then it is opened again when it is back (the Arty is
powered over the same USB cable), the
transcript marks both moments, and if SurfBoot's first lines came before the port was back,
the boot is read from the kernel load on and the bitstream's identity from bootinfo.

--owner CMD answers the questions instead of a person (the tests): CMD STEP is run with the
question on its standard input; exit 0 = done or yes, 1 = no, 3 = cannot be observed here.
--slow F multiplies every time limit (the RTL system runs about 7 times slower than the board).
"""

import argparse
import os
import re
import shlex
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uart  # noqa: E402

SURFCPU = Path(__file__).resolve().parents[1]
REPO = SURFCPU.parent
PROMPT = r'(?:~|≋)#> '                 # SurfOS's shell prompt, after the breadcrumb (ASCII or UTF-8)
MONITOR = r'surfboot> '
OS_FAILS = [r'Kernel [Pp]anic', r'Kernel Wipeout', r'SYSTEM HALTED', r'killed by', r'BOOT PROBLEM']
BOARD_IDS = {'arty_a7_100t': 0x100, 'arty_a7_35t': 0x135}
SIM_BOARD_ID = 0xf0
# what a simulation prints and a board never does: SurfBoot's and SurfOS's "(simulation)", the BRAM
# diagnostics' board line, the baseline's "sim" in place of clean or dirty, the diagnostics' build flags
# (hexadecimal) with BUILD_FLAGS.SIM (bit 1) set
SIM_MARKS = (r'(?m)\(simulation\)|^board simulation \(0xf0\)|^SurfCPU baseline \S+ 0x[0-9a-f]{8} build [0-9a-f]{8} sim '
             r'|bitstream build [0-9a-f]{8} flags [0-9a-f]*[2367abef],')
PACKAGE = struct.Struct('<IHHIIIIIIIIII12sI')   # BOOT-ABI.md section 2: the 64-byte header


class Stop(Exception):
    """the procedure cannot go on: a check that everything after it depends on failed"""


def package(path):
    """what SurfBoot will report for a package file: (build, file size, payload CRC), or None"""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return None
    if len(data) < 64:
        return None
    f = PACKAGE.unpack(data[:64])
    return {'build': '%08x' % f[12], 'size': len(data), 'crc': '0x%08x' % (zlib.crc32(data[64:]) & 0xFFFFFFFF),
            'path': str(path)}


MANIFEST_FORMATS = ('1', '2')         # tools/fpga-build.sh's manifest_format (none: 1); 2 since 10/27/26


def manifest(path):
    """a bitstream build manifest's top keys (tools/fpga-build.sh), or None; a recheck's (tools/fpga-build.sh
    --recheck: the build's checks run again) has its [recheck] table as the key 'recheck'"""
    try:
        text = Path(path).read_text()
    except OSError:
        return None
    keys = {}
    table = None
    for line in text.splitlines():
        if line.startswith('['):
            table = line.strip()
            if table == '[recheck]':
                keys['recheck'] = {}
            continue
        m = re.match(r'(\w+) = (.*)$', line)
        if m and table in (None, '[recheck]'):
            v = m.group(2).strip()
            v = v[1:-1] if v.startswith('"') else {'true': True, 'false': False}.get(v, v)
            (keys if table is None else keys['recheck'])[m.group(1)] = v
    return keys


def recheck_line(mf):
    """what a recheck's manifest says of the recheck, for the evidence (or None)"""
    rc = mf.get('recheck') if mf else None
    if not isinstance(rc, dict):
        return None
    return ('the manifest is a recheck\'s (tools/fpga-build.sh --recheck): %s, tools at %s, LVS by %s; the build\'s own '
            'result was %s' % (rc.get('rechecked_at'), str(rc.get('tools_revision', ''))[:12], rc.get('lvs_solver'),
                               rc.get('build_result')))


def tools_identity():
    """the commit the tools come from (where git can say) and their own SHA-256"""
    import hashlib
    rev = ''
    try:
        rev = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=str(REPO), capture_output=True,
                             text=True, timeout=10).stdout.strip()
        dirty = subprocess.run(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=str(REPO),
                               capture_output=True, text=True, timeout=10).stdout.strip()
        rev = rev + (' with uncommitted changes' if dirty and rev else '')
    except (OSError, subprocess.SubprocessError):
        pass
    sums = ', '.join('%s sha256 %s' % (f, hashlib.sha256((SURFCPU / 'tools' / f).read_bytes()).hexdigest()[:16])
                     for f in ('board-test.py', 'uart.py'))
    return '%s (commit %s)' % (sums, rev or 'unknown: git could not say')


class Run:
    """one procedure: the port, the owner, the checks and the evidence"""

    def __init__(self, a, milestone, title, design):
        self.a, self.milestone, self.title, self.design = a, milestone, title, design
        self.slow = a.slow
        self.checks = []                # (status, text): ok, FAIL, owner, owner-no, skip, note
        self.steps = []                 # (time asked, step, question, answer, time answered)
        self.identity = []              # what was read and how it compared
        self.sim = False
        self.link = None
        self.pos = 0                    # where the next expect starts in the text
        self.power_pending = False      # a power step was answered and the board has not spoken since
        self.started = time.time()
        self.extra = []                 # more sections for the evidence (title, lines)
        self.full_log = None
        mpath = a.manifest or (SURFCPU / 'build' / 'fpga' / a.board / design / 'manifest.txt' if design else None)
        self.manifest_path = mpath
        self.manifest = manifest(mpath) if mpath else None

    # ------------------------------------------------------------------------- the port
    def open(self):
        try:
            self.link = uart.Link(self.a.port, flush=True, pace=self.a.pace)
        except uart.PortGone as e:
            self.fail('the port: %s (uart.py list shows the ports; the Arty\'s UART is the one ending in 1)' % e)
            raise Stop()
        self.note('port %s opened at 115200 8N1' % self.a.port)

    def t(self, seconds):
        return seconds * self.slow

    def expect(self, pattern, what, timeout, fails=None, start=None):
        """wait for pattern after the last match: the match, or Stop with a clear reason"""
        m, why = self.link.wait(pattern, self.pos if start is None else start, self.t(timeout),
                                fail_on=fails if fails is not None else [])
        if m:
            self.pos = m.end()
            if self.power_pending and self.link.port is not None:
                self.link.allow_reopen(0)   # the port is back and the board talks: from now on a lost port is a FAIL
                self.power_pending = False
            return m
        if why.startswith('fail-on'):
            fm = self.link.last_fail                # the whole line, and the one after it (the reason)
            whole, _ = self.link.wait(r'[^\n]*\n[^\n]*\n', fm.start(), self.t(1))
            if not whole:
                whole, _ = self.link.wait(r'[^\n]*\n', fm.start(), self.t(1))
            lines = [x.strip() for x in (whole.group(0) if whole else fm.group(0)).split('\n') if x.strip()]
            self.fail('%s: the board printed "%s" instead%s' % (what, lines[0], ' (then: "%s")' % lines[1]
                                                                 if len(lines) > 1 else ''))
        elif why.startswith('gone'):
            self.fail('%s: the port went away at +%.1f s (%s): USB unplugged, or the board lost power?'
                      % (what, self.link.gone_at or self.link.now(), why[6:]))
        else:
            hint = ' The bytes look garbled: the wrong port (use the one ending in 1) or the wrong baud rate?' \
                if self.link.text.garbled() else (' Nothing at all arrived on the port.' if not self.link.text.raw else '')
            if self.link.port is None:
                hint += ' The port went away at +%.1f s and did not come back (%s).' % (self.link.gone_at or 0,
                                                                                    self.link.gone)
            self.fail('%s: not seen within %.0f s (waiting for /%s/).%s' % (
                what, self.t(timeout), getattr(pattern, 'pattern', pattern), hint))
        raise Stop()

    def seen(self, pattern, start=0):
        """pattern somewhere in the text from start on (no waiting)"""
        return re.search(pattern, self.link.text.text[start:])

    def type(self, text):
        try:
            self.link.type(text)
        except uart.PortGone as e:
            self.fail('typing %r: the port went away (%s)' % (text.strip(), e))
            raise Stop()

    def cmd(self, line, prompt=PROMPT, timeout=20, fails=OS_FAILS, what=None):
        """a command typed at the prompt (the prompt must be the last thing seen): its output"""
        self.type(line + '\r')
        echo = self.expect(re.escape(line) + r'\n', '%s: its echo' % (what or line), 10, fails)
        out_start = echo.end()
        m = self.expect(prompt, '%s: the prompt after it' % (what or line), timeout, fails)
        out = self.link.text.text[out_start:m.start()]
        return out[:out.rfind('\n') + 1] if '\n' in out else ''

    # ------------------------------------------------------------------------- the owner
    def ask(self, step, question, yesno=False, power=False):
        """the owner does a step (Enter) or answers (y/n): 'yes', 'no' or 'skip'. power: the step may
        take the port away (a power cycle, the board programmed or switched over to its flash): while
        the owner works, and after the answer until the board's next expected line arrives (at most
        15 s if the port is not back by then), a port that goes away is opened again once it is
        back, and SurfBoot's first lines may then be missed. At any other time a port that goes away
        ends the procedure at once (USB unplugged)"""
        t = self.link.now() if self.link else 0.0
        if power and self.link:
            self.link.allow_reopen(float('inf'))
        if self.a.owner:
            try:
                r = subprocess.run(shlex.split(self.a.owner) + [step], input=question, text=True,
                                   timeout=self.t(600), capture_output=True)
                answer = {0: 'yes', 1: 'no', 3: 'skip'}.get(r.returncode, 'no')
            except subprocess.TimeoutExpired:
                answer = 'no'
        else:
            print('\n>>> %s' % question.replace('\n', '\n    '))
            answer = None
            while answer is None:
                try:
                    reply = input('    [%s] ' % ('y = yes, n = no, s = could not see it' if yesno else
                                                 'Enter when done, n if it could not be done')).strip().lower()
                except EOFError:
                    reply = 'n'
                if reply.startswith('n'):
                    answer = 'no'
                elif reply.startswith('s'):
                    answer = 'skip'
                elif reply.startswith('y') or (reply == '' and not yesno):
                    answer = 'yes'
        if power and self.link:
            self.link.allow_reopen(15)
            self.power_pending = True
        self.steps.append((t, step, question, answer, self.link.now() if self.link else 0.0))
        return answer

    def do(self, step, question, power=False):
        """a step the procedure cannot go on without"""
        answer = self.ask(step, question, power=power)
        if answer != 'yes':
            self.fail('the owner %s the step "%s"' % ('could not do' if answer == 'no' else 'skipped', step))
            raise Stop()

    def observe(self, step, question):
        """something only a person can see: recorded as the owner's answer"""
        answer = self.ask(step, question, yesno=True)
        text = ' '.join(question.split())
        if answer == 'yes':
            self.checks.append(('owner', text))
        elif answer == 'no':
            self.checks.append(('owner-no', text))
        else:
            self.checks.append(('skip', 'not observed: ' + text))

    # ------------------------------------------------------------------------- checks
    def check(self, ok, text):
        self.checks.append(('ok' if ok else 'FAIL', text))
        return ok

    def fail(self, text):
        self.checks.append(('FAIL', text))

    def note(self, text):
        self.checks.append(('note', text))

    def ident(self, text):
        self.identity.append(text)

    def compare_bitstream(self, build, flags=None, board_id=None):
        """the bitstream's build id (sysinfo) against the build manifest"""
        if flags is not None and flags & 2:
            self.sim = True
        if board_id == SIM_BOARD_ID:
            self.sim = True
        self.ident('bitstream build %s%s%s' % (build, '' if flags is None else ', flags %d%s%s' % (
            flags, ' (dirty)' if flags & 1 else '', ' (SIMULATION)' if flags & 2 else ''),
            '' if board_id is None else ', board id 0x%x' % board_id))
        if board_id is not None and board_id != SIM_BOARD_ID:
            self.check(board_id == BOARD_IDS.get(self.a.board), 'board id 0x%x is %s\'s (0x%x)'
                       % (board_id, self.a.board, BOARD_IDS.get(self.a.board, 0)))
        mf = self.manifest
        if not mf:
            self.ident('  not compared: no build manifest at %s' % self.manifest_path)
            self.fail('the bitstream\'s build id not compared: no build manifest at %s' % self.manifest_path)
            return
        same = mf.get('build_id') == build
        line = 'the bitstream\'s build id %s %s the manifest\'s (%s, %s)' % (
            build, 'is' if same else 'is NOT', mf.get('build_id'), self.manifest_path)
        self.ident('  ' + line)
        if recheck_line(mf):
            self.ident('  ' + recheck_line(mf))
        self.check(same, line)
        if flags is not None and isinstance(mf.get('source_dirty'), bool):
            self.check(bool(flags & 1) == mf['source_dirty'], 'its dirty flag (%d) matches the manifest\'s '
                       'source_dirty = %s' % (flags & 1, str(mf['source_dirty']).lower()))
        if mf.get('result') != 'pass':
            self.fail('the manifest says result = %s: that bitstream did not pass its build checks' % mf.get('result'))

    def compare_package(self, what, path, build, size=None, crc=None):
        """a package's build id, size and payload CRC as SurfBoot reported them, against the file"""
        p = package(path) if path else None
        self.ident('%s build %s%s%s' % (what, build, '' if size is None else ', %d bytes' % size,
                                        '' if crc is None else ', payload CRC %s' % crc))
        if not p:
            self.checks.append(('skip', '%s not compared: no package file at %s' % (what, path)))
            self.ident('  not compared: no file at %s' % path)
            return
        ok = p['build'] == build and (size is None or p['size'] == size) and (crc is None or p['crc'] == crc)
        line = '%s on the card %s %s (build %s, %d bytes, payload CRC %s)' % (
            what, 'is' if ok else 'is NOT', p['path'], p['build'], p['size'], p['crc'])
        self.ident('  ' + line)
        self.check(ok, line)

    # ------------------------------------------------------------------------- the evidence
    def verdict(self):
        stats = {s: sum(1 for x, _ in self.checks if x == s) for s in ('ok', 'FAIL', 'owner', 'owner-no', 'skip')}
        if stats['FAIL'] or stats['owner-no']:
            v = 'FAIL'
        elif not stats['ok']:
            v = 'INCOMPLETE'
        else:
            v = 'PASS'
        detail = '%d checks seen by this script passed%s%s%s%s' % (
            stats['ok'], ', %d failed' % stats['FAIL'] if stats['FAIL'] else '',
            '; the owner confirmed %d' % stats['owner'] if stats['owner'] else '',
            ', and answered no to %d' % stats['owner-no'] if stats['owner-no'] else '',
            '; %d not done or not compared' % stats['skip'] if stats['skip'] else '')
        return v, detail

    def write(self, interrupted=False):
        v, detail = self.verdict()
        if interrupted:
            v = 'INTERRUPTED'
        if self.link and re.search(SIM_MARKS, self.link.text.text):
            self.sim = True                 # a simulation, even if the run stopped before its identity was checked
        default = SURFCPU / ('build/board-test' if self.sim else 'docs/evidence/physical')
        d = Path(self.a.evidence_dir) if self.a.evidence_dir else default
        d.mkdir(parents=True, exist_ok=True)
        day = time.strftime('%Y-%m-%d', time.gmtime(self.started))
        path = d / ('%s-%s.txt' % (self.milestone, day))
        n = 2
        while path.exists():
            path = d / ('%s-%s-%d.txt' % (self.milestone, day, n))
            n += 1
        L = []
        L.append('SurfCPU acceptance test %s: %s' % (self.milestone, self.title))
        L.append('Verdict: %s (%s)' % (v, detail))
        if self.sim:
            L.append('Source: SIMULATION, not the board: the SoC reports BUILD_FLAGS.SIM or the simulation board id '
                     '(the RTL system in surfsim, through %s)' % self.a.port)
        else:
            L.append('Source: the board, through %s at 115200 8N1' % self.a.port)
        if self.a.owner:
            L.append('Owner: the steps were answered by a program, not by a person (--owner %s)' % self.a.owner)
        L.append('Date: %s UTC, %.0f s' % (time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(self.started)),
                                           time.time() - self.started))
        L.append('Command: %s' % ' '.join(shlex.quote(x) for x in sys.argv))
        L.append('Tools: %s; Python %s on %s' % (tools_identity(), sys.version.split()[0],
                                                os.uname().sysname + ' ' + os.uname().machine))
        L.append('')
        L.append('Build identity read from the port, and what it was compared with:')
        L += ['  ' + x for x in (self.identity or ['(none read)'])]
        L.append('')
        L.append('Steps the owner was asked to do, and the answers (+ seconds since the port was opened: when asked, '
                 'and when answered):')
        for t, step, q, ans, t_ans in self.steps:
            L.append('  +%8.1f  %-14s %s, answered at +%.1f' % (
                t, step, {'yes': 'done', 'no': 'NOT DONE / NO', 'skip': 'skipped'}[ans], t_ans))
            for ql in q.split('\n'):
                L.append('              | %s' % ql)
        if not self.steps:
            L.append('  (none)')
        L.append('')
        L.append('Checks (ok and FAIL: seen by this script; owner: the owner\'s answer; skip: not done or not compared):')
        for s, text in self.checks:
            L.append('  %-8s %s' % (s, text))
        for title, lines in self.extra:
            L.append('')
            L.append(title)
            L += ['  ' + x for x in lines]
        L.append('')
        if self.link:
            lines = self.link.text.lines()
            L.append('Transcript (seconds since the port was opened; escape sequences and CR removed; %d bytes received%s;'
                     % (self.link.text.raw, ', %d of them not printable' % self.link.text.bad if self.link.text.bad else ''))
            L.append('"*": the port going away and coming back):')
            keep = self.trim(lines)
            events = list(self.link.events)
            for item in keep:
                while events and not isinstance(item, str) and events[0][0] <= item[0]:
                    L.append('  +%9.3f * %s' % events.pop(0))
                L.append(item if isinstance(item, str) else '  +%9.3f | %s' % item)
            L += ['  +%9.3f * %s' % e for e in events]
        path.write_text('\n'.join(L) + '\n')
        if self.full_log:
            self.full_log.parent.mkdir(parents=True, exist_ok=True)
            with open(self.full_log, 'w') as f:
                for t, line in self.link.text.lines():
                    f.write('+%9.3f | %s\n' % (t, line))
        return path, v, detail

    def trim(self, lines):
        """the transcript for the evidence; a long soak keeps its start and end (the whole of it goes to
        build/board-test/)"""
        limit = 4000
        if len(lines) <= limit:
            return lines
        self.full_log = SURFCPU / 'build' / 'board-test' / ('%s-%s.full.txt' % (
            self.milestone, time.strftime('%Y%m%dT%H%M%SZ', time.gmtime(self.started))))
        return lines[:limit // 2] + ['  ... %d lines left out here; the whole transcript is in %s ...'
                                     % (len(lines) - limit, self.full_log.relative_to(SURFCPU))] + lines[-limit // 2:]


# ============================================================================== m1
BASELINE = re.compile(r'SurfCPU baseline (\S+) 0x([0-9a-f]{8}) build ([0-9a-f]{8}) (clean|dirty|sim) (\d+) \| '
                      r'up (\d{6}) s \| pix (\d+) Hz \| sw ([01]{4}) btn ([01]{4}) rx ([0-9a-f]{2}|--)\n')


def baseline_lines(r, n, timeout_each=3.0):
    """n consecutive baseline lines: [(host time the line arrived, match)]"""
    got = []
    for i in range(n):
        m = r.expect(BASELINE, 'baseline line %d of %d' % (i + 1, n), timeout_each if got else 5.0)
        got.append((r.link.arrival(m.start()), m))      # not now(): lines wait in the buffer while the owner reads
    return got


def m1(r):
    a = r.a
    r.open()
    r.do('program', 'Load the baseline design, in another Terminal window:\n'
                    '  make -C surfcpu program-volatile BOARD=%s DESIGN=arty_a7_baseline\n'
                    'and press Enter here once it says done.' % a.board)
    got = baseline_lines(r, a.lines)
    first = got[0][1]
    name, bid, build, state, btime = first.group(1), int(first.group(2), 16), first.group(3), first.group(4), first.group(5)
    r.sim = state == 'sim'
    r.check(all(m.group(1) == name and m.group(3) == build for _, m in got),
            '%d lines a second, each of the baseline\'s form ("SurfCPU baseline %s 0x%08x build %s %s %s | up ...")'
            % (len(got), name, bid, build, state, btime))
    ups = [int(m.group(6)) for _, m in got]
    r.check(all(b == x + 1 for x, b in zip(ups, ups[1:])), 'the seconds since reset count up by one a line (%s)'
            % ', '.join(str(u) for u in ups))
    gaps = [b[0] - x[0] for x, b in zip(got, got[1:])]
    if r.slow == 1:
        r.check(all(0.7 <= g <= 1.3 for g in gaps), 'a line every second by the Mac\'s clock (%s s)'
                % ', '.join('%.2f' % g for g in gaps))
    else:
        r.note('the lines\' spacing (%s s) not checked against the clock: --slow %g' % (
            ', '.join('%.2f' % g for g in gaps), r.slow))
    pix = [int(m.group(7)) for _, m in got[1:]]
    r.check(all(abs(p - 25173611) <= 1 for p in pix), 'the pixel clock measured 25,173,611 +- 1 Hz (%s)'
            % ', '.join(str(p) for p in sorted(set(pix))))
    r.check(BOARD_IDS.get(name) == bid or (r.sim and bid == SIM_BOARD_ID) or name not in BOARD_IDS,
            'the board %s and its id 0x%x agree' % (name, bid))
    if not r.sim:
        r.check(name == a.board, 'the design was built for %s, the board asked for (--board)' % a.board)
    r.ident('baseline: board %s 0x%08x, build %s, %s, built at %s' % (name, bid, build, state, btime))
    mf = r.manifest
    if mf:
        ok = mf.get('build_id') == build and str(mf.get('build_time')) == btime and \
            (state == 'sim' or mf.get('source_dirty') == (state == 'dirty')) and mf.get('board') == name
        line = 'the line\'s build %s %s, time %s and board %s %s the manifest\'s (%s, %s, source_dirty %s, %s; %s)' % (
            build, state, btime, name, 'match' if ok else 'do NOT match', mf.get('build_id'), mf.get('build_time'),
            str(mf.get('source_dirty')).lower(), mf.get('board'), r.manifest_path)
        r.ident('  ' + line)
        if recheck_line(mf):
            r.ident('  ' + recheck_line(mf))
        r.check(ok, line)
    else:
        r.ident('  not compared: no build manifest at %s' % r.manifest_path)
        r.fail('the build identity not compared: no build manifest at %s' % r.manifest_path)
    # the receive pin: a byte typed comes back in the line
    r.type('S')
    m = r.expect(r'\| sw [01]{4} btn [01]{4} rx 53\n', 'the byte typed (S, 0x53) in a later line', 3.5)
    r.check(bool(m), 'the FPGA\'s receive pin: the byte typed (S) shows as rx 53')
    # the switches and a button, by the owner; the lines show them
    start = r.pos
    if r.ask('switches', 'Set the switches to SW3 off, SW2 on, SW1 off, SW0 on (0101), then press Enter.') == 'yes':
        m = r.expect(r'\| sw 0101 btn', 'a line with the switches at 0101', 3.5, start=start)
        r.check(bool(m), 'the switches: a line shows sw 0101 as the owner set them')
    else:
        r.checks.append(('skip', 'the switches not set by the owner'))
    start = r.pos
    if r.ask('button', 'Press and hold BTN0 for two seconds, release it, then press Enter.') == 'yes':
        m = r.expect(r'btn [01]{3}1 rx', 'a line with BTN0 pressed', 1.0, start=start)
        r.check(bool(m), 'the buttons: a line shows BTN0 pressed (btn ...1)')
    else:
        r.checks.append(('skip', 'the button not pressed by the owner'))
    r.observe('leds', 'Do LD0 and LD1 blink green together, once a second; do LD4..LD7 follow the switches; and did\n'
                      'BTN0 light an RGB LED blue?')
    start = r.pos
    if r.ask('reset', 'Press the RESET button once, then press Enter.') == 'yes':
        m = r.expect(r'\| up 00000[12] s \|', 'a line counting from 1 again after RESET', 3.5, start=start)
        r.check(bool(m), 'RESET restarts the count (up %s s)' % m.group(0)[5:11])
    else:
        r.checks.append(('skip', 'RESET not pressed by the owner'))


# ============================================================================== m3
BRAMDIAG_SECTIONS = ['identity', 'ALU, immediates, x0', 'M extension', 'branches and jumps', 'loads and stores',
                     'CSRs and counters', 'scratch RAM', 'exceptions, each recovered', 'interrupts, timer and WFI',
                     'FENCE.I, code written to RAM']


def m3(r):
    a = r.a
    r.open()
    r.do('program', 'Load the CPU with its diagnostics, in another Terminal window:\n'
                    '  make -C surfcpu program-volatile BOARD=%s DESIGN=%s\n'
                    'and press Enter here once it says done.' % (a.board, r.design), power=True)
    banner = r'SurfCPU BRAM diagnostics (\S+) \(build ([0-9a-f]{8})(, dirty tree)?\)\n'
    m, _ = r.link.wait(banner, r.pos, r.t(10))
    if not m:                               # the port came back after the first run: Ctrl-R runs it again
        r.note('the diagnostics\' first run was not seen (the port was not open yet?): Ctrl-R typed to run them again')
        r.type('\x12')
    m = r.expect(banner, 'the diagnostics\' banner', 10)
    r.ident('BRAM diagnostics %s, ROM build %s%s' % (m.group(1), m.group(2), ' (a dirty tree)' if m.group(3) else ''))
    b = r.expect(r'board (.*) \((0x[0-9a-f]+)\), SoC (\S+), memory map (\d+), sys (\d+) Hz, timebase (\d+) Hz\n',
                 'the board line', 5)
    # bramdiag.c prints flags, features and DDR status in hexadecimal (DDR status 14: PRESENT and SWINIT)
    s = r.expect(r'bitstream build ([0-9a-f]{8}) flags ([0-9a-f]+), features ([0-9a-f]+), DDR status ([0-9a-f]+) '
                 r'size (\d+) MiB\n', 'the bitstream line', 5)
    r.compare_bitstream(s.group(1), int(s.group(2), 16), int(b.group(2), 16))
    r.check(int(b.group(5)) == 25000000, 'the system clock is 25,000,000 Hz (%s)' % b.group(5))
    start = r.pos
    end = r.expect(r'BRAMDIAG: (PASS|FAIL) \((\d+) checks?[^\n]*\n', 'BRAMDIAG\'s end', 60)
    body = r.link.text.text[start:end.start()]
    sections = {}
    for line in body.split('\n'):
        mm = re.match(r'(\S.*?)\s{2,}(?:\((.*?)\) )?(ok|FAIL|failed|skipped)\b(.*)$', line)
        if mm:
            sections[mm.group(1)] = (mm.group(3), mm.group(2) or '', mm.group(4))
    for name in BRAMDIAG_SECTIONS:
        hit = [k for k in sections if k.startswith(name)]
        st = sections[hit[0]] if hit else None
        r.check(st is not None and st[0] == 'ok', '%s: %s' % (name, ('%s%s' % (st[0], ' (%s)' % st[1] if st[1] else ''))
                                                             if st else 'not reported'))
    tick = sections.get('interrupts, timer and WFI', ('', '', ''))[1]
    mt = re.match(r'(\d+) timer ticks in (\d+) us', tick)
    r.check(bool(mt) and 0.8 <= int(mt.group(2)) / (1000.0 * int(mt.group(1))) <= 1.2,
            'the timer\'s progress: %s (1 ms a tick, +- 20 %%)' % (tick or 'not reported'))
    others = [k for k in sections if not any(k.startswith(n) for n in BRAMDIAG_SECTIONS)]
    for k in others:
        st = sections[k]
        r.check(st[0] in ('ok', 'skipped'), '%s: %s%s' % (k, st[0], st[2].rstrip()))
    r.check(end.group(1) == 'PASS', 'BRAMDIAG: %s (%s checks)' % (end.group(1), end.group(2)))
    # the receive pin: after the run each byte typed comes back with its code
    r.expect(r'UART echo: type[^\n]*\n', 'the echo after the diagnostics', 10)
    typed = 'Arty7'
    r.type(typed)
    for ch in typed:
        r.expect(r"echo\s+\d+: '%s' 0x%02x\n" % (re.escape(ch), ord(ch)), 'the typed %r echoed' % ch, 5)
    r.check(True, 'the UART both ways: each byte typed (%s) came back with its code' % typed)
    r.observe('leds', 'Are LD4..LD6 lit, LD7 blinking about once a second (its heartbeat while it echoes), and LD0\n'
                      'green (red would mean a failure)?')


# ============================================================================== SurfBoot and SurfOS
SURFBOOT = r'SurfBoot (\S+) \(build ([0-9a-f]{8})(, dirty tree)?\), the SurfCPU boot ROM\n'
SOC_LINE = (r'soc: (.*?) \((0x[0-9a-f]+)\), SoC (\S+), memory map (\d+), (\d+) Hz; bitstream ([0-9a-f]{8})'
            r'( dirty)?( \(simulation\))?\n')
POWER_UP = ('Power-cycle the board: unplug its USB cable, wait 5 seconds, plug it back in; then load the\n'
            'design again (the FPGA forgets it at power-off):\n'
            '  make -C surfcpu program-volatile BOARD=%s DESIGN=%s\n'
            '(or, if the board starts from its flash, just wait for it). %s Then press Enter.')


def surfboot_identity(r, start, end):
    """SurfBoot's banner and SoC line between start and end, if they were seen: (rom build, bitstream) or None"""
    text = r.link.text.text[start:end]
    m, soc = re.search(SURFBOOT, text), re.search(SOC_LINE, text)
    if not (m and soc):
        return None
    r.ident('SurfBoot %s, ROM build %s%s' % (m.group(1), m.group(2), ' (a dirty tree)' if m.group(3) else ''))
    flags = (1 if soc.group(7) else 0) | (2 if soc.group(8) else 0)
    r.compare_bitstream(soc.group(6), flags, int(soc.group(2), 16))
    return m.group(2), soc.group(6)


def boot_surfos(r, check_identity=True, timeout=60):
    """SurfBoot from the card to SurfOS's prompt, from r.pos on: {'rw', 'bitstream' (None if SurfBoot's first
    lines were missed, the port not being back yet after a power cycle), 'up'}. SurfBoot's lines are read
    once the kernel has been read, so a boot whose first lines came before the port was open still counts"""
    start = r.pos
    fails = [r'SurfBoot stopped[^\n]*'] + OS_FAILS
    k = r.expect(r'boot: SurfOS (\S+) build ([0-9a-f]{8}): (\d+) bytes in (\d+) fragments?[^\n]*\n', 'SurfBoot reading '
                 'the kernel from the card', 60, fails)
    seen = re.search(SURFBOOT, r.link.text.text[start:k.start()])
    ident = surfboot_identity(r, start, k.start()) if check_identity else None
    soc = re.search(SOC_LINE, r.link.text.text[start:k.start()])
    if not seen:
        r.note('SurfBoot\'s first lines were not seen: the port came back after them (a power cycle); the boot '
               'is read from the kernel load on')
    d = re.search(r'ddr: calibrated[^\n]*', r.link.text.text[start:k.start()])
    c = r.expect(r'payload CRC (0x[0-9a-f]{8}) matches \(card and DDR\)\n', 'its payload CRC checked', 10, fails)
    if check_identity:
        if d:
            r.ident('SurfBoot: %s' % d.group(0).strip())
        r.compare_package('SurfOS %s' % k.group(1), r.a.kernel, k.group(2), int(k.group(3)), c.group(1))
    r.expect(r'boot: starting the kernel at 0x80000000[^\n]*\n', 'the handoff', 10, fails)
    r.expect(r'SurfOS: serial console on the arty_a7 UART[^\n]*\n', 'the kernel\'s first line', 10, fails)
    mnt = r.expect(r'\+VFS: sd0p1 mounted on /sd0p1 \(fat(, read-only)?\)\n', 'the card mounted', timeout, fails)
    r.expect(r"Brandon Burr's SurfOS\n", 'the banner', 20, fails)
    r.expect(PROMPT, 'the shell prompt', 20, fails)
    return {'rw': mnt.group(1) is None, 'bitstream': soc.group(6) if soc else None, 'surfboot': bool(seen),
            'identity': ident}


def kernel_identity(r):
    """bootinfo: the bitstream's build id as the kernel reads it, or None"""
    out = r.cmd('bootinfo', what='bootinfo')
    m = re.search(r'sysinfo: board 0x([0-9a-f]+), SoC 0x[0-9a-f]+, map \d+, sys clock \d+ Hz, bitstream ([0-9a-f]{8})'
                  r'( \(dirty\))?( \(simulation\))?', out)
    if r.check(bool(m), 'bootinfo: the kernel reads the bitstream\'s identity (%s)' % (m.group(0) if m else 'not found')):
        if m.group(4):
            r.sim = True
        r.ident('SurfOS bootinfo: %s' % m.group(0))
        return m.group(2), (1 if m.group(3) else 0) | (2 if m.group(4) else 0), int(m.group(1), 16)
    return None


def boot_identity(r, boot):
    """the bitstream compared with its manifest: from SurfBoot's SoC line, or from bootinfo if that was missed"""
    if boot['identity'] is None:
        k = kernel_identity(r)
        if k:
            r.compare_bitstream(*k)
    else:
        kernel_identity(r)


def m5(r):
    a = r.a
    r.do('card', 'On the Mac, with the SurfOS card mounted (SurfOS installed with make deploy-sd, OWNER-GUIDE.md\n'
                 'section 7), copy the DDR diagnostic next to the kernel:\n'
                 '  cp surfcpu/build/fw/DDRDIAG.BIN /Volumes/<card>/BOOT/\n'
                 'then eject the card (diskutil eject /Volumes/<card>), put it in the Arty, and press Enter.')
    r.open()
    r.do('monitor', 'Load the design if it is not loaded (make -C surfcpu program-volatile BOARD=%s DESIGN=%s).\n'
                    'Then hold BTN1 down, press and release RESET, and keep holding BTN1 until surfboot> appears;\n'
                    'then press Enter.' % (a.board, r.design), power=True)
    r.expect(r'BTN1 held: staying in the monitor\n', 'BTN1 seen: SurfBoot staying in its monitor', 15)
    surfboot_identity(r, 0, r.pos)
    r.expect(MONITOR, 'the monitor\'s prompt', 10)
    out = r.cmd('info', MONITOR, 10)
    ddr = re.search(r'^ddr: status [0-9a-f]+ \(([^)]*)\), size (\d+) MiB', out, re.M)
    r.check(bool(ddr) and 'calibrated' in ddr.group(1).split(', ') and int(ddr.group(2)) > 0,
            'info: DDR calibrated (the DDR bitstream): %s' % (ddr.group(0) if ddr else 'no ddr line'))
    r.check('card: present' in out, 'info: the card is present (%s)' % next(
        (l.strip() for l in out.split('\n') if l.startswith('card:')), 'no card line'))
    if not r.identity:
        sysinfo = re.search(r'^sysinfo: magic \S+, SoC \S+, map \d+, board ([0-9a-f]+), bitstream build ([0-9a-f]{8}) '
                            r'flags ([0-9a-f]+)', out, re.M)
        if sysinfo:
            r.compare_bitstream(sysinfo.group(2), int(sysinfo.group(3), 16), int(sysinfo.group(1), 16))
    out = r.cmd('boot /BOOT/NOSUCH.BIN', MONITOR, 20)
    m = re.search(r'\((NOT_FOUND|NOT_FILE|BAD_PATH)\)', out)
    r.check(bool(m), 'a missing image: boot /BOOT/NOSUCH.BIN is diagnosed (%s) and the monitor is back'
            % (m.group(1) if m else 'no diagnosis'))
    out = r.cmd('ls /SYSTEM', MONITOR, 20)
    names = re.findall(r'^\s+\S{5}\s+(\S+)\s+\d+\s+cluster \d+$', out, re.M)    # files: a size, not <dir>
    if names:
        victim = '/SYSTEM/' + names[0]
        out = r.cmd('check %s' % victim, MONITOR, 20)
        m = re.search(r'^check: \S+: (?:FAILED: )?[^\n]*\((IMG_[A-Z_]+)\)$', out, re.M)
        r.check(bool(m), 'an invalid image: check %s (not a package) is diagnosed (%s), the monitor is back'
                % (victim, m.group(1) if m else 'no IMG_ diagnosis'))
    else:
        r.checks.append(('skip', 'an invalid image: no plain file in /SYSTEM to try as one'))
    out = r.cmd('check /BOOT/DDRDIAG.BIN', MONITOR, 60)
    r.check('every check passed' in out, 'check /BOOT/DDRDIAG.BIN: every package check passes')
    r.type('boot /BOOT/DDRDIAG.BIN\r')
    k = r.expect(r'boot: DDRdiag (\S+) build ([0-9a-f]{8}): (\d+) bytes[^\n]*\n', 'the DDR diagnostic read', 30,
                 [r'boot failed: [^\n]*'])
    c = r.expect(r'payload CRC (0x[0-9a-f]{8}) matches \(card and DDR\)\n', 'its payload CRC checked', 10)
    r.compare_package('DDRDIAG.BIN', a.ddrdiag, k.group(2), int(k.group(3)), c.group(1))
    r.expect(r'SurfCPU DDR diagnostic (\S+) \(build ([0-9a-f]{8})[^\n]*\n', 'the DDR diagnostic\'s banner', 20)
    area = r.expect(r'testing DDR \[(0x[0-9a-f]+), (0x[0-9a-f]+)\) \((\d+) MiB\)[^\n]*\n', 'its test area', 20)
    end = r.expect(r'DDRDIAG: (PASS|FAIL) \((\d+) checks?[^\n]*\n', 'DDRDIAG\'s end', 300)
    r.check(end.group(1) == 'PASS', 'the DDR diagnostic booted from the card: DDRDIAG: %s (%s checks; %s MiB from %s)'
            % (end.group(1), end.group(2), area.group(3), area.group(1)))
    # no card: diagnosed, and the boot retried when it is back
    if a.skip_no_card:
        r.checks.append(('skip', 'the missing card (--skip-no-card)'))
        return
    start = r.pos
    if r.ask('card-out', 'Take the card out of the Pmod MicroSD, then press and release RESET (no button held), then\n'
                         'press Enter.', power=True) != 'yes':
        r.checks.append(('skip', 'the missing card: the owner did not take it out'))
        return
    m = r.expect(r'SurfBoot stopped: no card \(NO_CARD\)\n', 'no card diagnosed', 20, start=start)
    r.expect(MONITOR, 'the monitor after it', 10)
    r.check(bool(m), 'no card: SurfBoot stopped: no card (NO_CARD), in the monitor')
    r.do('card-in', 'Put the card back in, then press Enter.')
    m = r.expect(r'card inserted: booting\n', 'the boot retried with the card back', 20)
    r.check(bool(m), 'the card back: "card inserted: booting", SurfBoot retries by itself')
    r.expect(r'boot: /BOOT/SURFOS\.BIN\n', 'the retry reading /BOOT/SURFOS.BIN', 20)


def m7(r):
    a = r.a
    r.do('card', 'Install SurfOS on the card (make -C surfcpu deploy-sd MOUNT=/Volumes/<card>), eject it, put it in\n'
                 'the Arty, and press Enter.')
    r.open()
    r.do('boot', 'Load the design (make -C surfcpu program-volatile BOARD=%s DESIGN=%s), or press and release\n'
                 'RESET if it is loaded already; hold no button. Then press Enter.' % (a.board, r.design), power=True)
    boot = boot_surfos(r)
    r.check(True, 'SurfOS booted from the card to the shell (SurfBoot, the kernel read and checked, the card mounted%s)'
            % (' read-write' if boot['rw'] else ' read-only'))
    boot_identity(r, boot)
    for n in (1, 2):
        out = r.cmd('tasks', timeout=60, what='tasks (%d)' % n)
        m = re.search(r'task A ran (\d+) rounds, task B (\d+), in (\d+) ticks; both ended \(status 0, 0\)', out)
        inter = re.findall(r'task (A|B): round \d+ of \d+, tick \d+, sleeping', out)
        r.check(bool(m) and 'A' in inter and 'B' in inter,
                'two kernel tasks make progress, sleep and wake (run %d: %s; their rounds interleaved: %s)'
                % (n, m.group(0) if m else 'not reported', ''.join(inter)))
    u1 = r.cmd('uptime')
    t1 = r.link.now()
    time.sleep(3)
    u2 = r.cmd('uptime')
    t2 = r.link.now()
    p = r'up (\d+)\.(\d+) s \((\d+) ticks at (\d+) Hz\)[\s\S]*?heartbeat (\d+)'
    m1_, m2_ = re.search(p, u1), re.search(p, u2)
    if r.check(bool(m1_ and m2_), 'uptime twice'):
        s1, s2 = float(m1_.group(1) + '.' + m1_.group(2)), float(m2_.group(1) + '.' + m2_.group(2))
        r.check(s2 > s1 and int(m2_.group(3)) > int(m1_.group(3)) and int(m2_.group(5)) >= int(m1_.group(5)),
                'the uptime advances: %.2f s then %.2f s, ticks %s then %s, heartbeat %s then %s'
                % (s1, s2, m1_.group(3), m2_.group(3), m1_.group(5), m2_.group(5)))
        if r.slow == 1:
            r.check(abs((s2 - s1) - (t2 - t1)) <= 0.25 * (t2 - t1),
                    'the board\'s clock keeps time: %.2f s on the board while %.2f s passed on the Mac' % (s2 - s1, t2 - t1))
        else:
            r.note('the uptime\'s rate not checked against the clock: --slow %g' % r.slow)
    r.cmd('echo kbd-ready')
    start = r.pos
    if r.ask('keyboard', 'On the PS/2 keyboard, type:  echo ps2-ok  and Enter, then press Enter here.') == 'yes':
        m = r.expect(r'\nps2-ok\n', 'what was typed on the keyboard', 10, start=start)
        r.check(bool(m), 'the PS/2 keyboard types into the shell: echo ps2-ok answered ps2-ok')
    else:
        r.checks.append(('skip', 'the PS/2 keyboard: the owner did not type'))
    r.observe('vga', 'Does the VGA monitor show the same shell: the banner and the commands just run?')


# ============================================================================== m8
MAC_TEXT = b'made on the Mac\n'


def crcs(r, names, where='/sd0p1/USERS/DOCUMENTS'):
    """{name: (crc, size)} from SurfOS's crc32"""
    out = r.cmd('crc32 ' + ' '.join('%s/%s' % (where, n) for n in names), timeout=60)
    got = {}
    for m in re.finditer(r'([0-9a-f]{8})\s+(\d+)\s+\S+/([^/\s]+)\n', out):
        got[m.group(3)] = ('%08x' % int(m.group(1), 16), int(m.group(2)))
    return got


def crc_of(data):
    return ('%08x' % (zlib.crc32(data) & 0xFFFFFFFF), len(data))


def mount_crcs(r, names):
    """{name: (crc, size)} read from the card's mount on the Mac (--mount), read only; a name in any case
    (FAT keeps 8.3 names in capitals, and systems show them in either case)"""
    got = {}
    d = Path(r.a.mount) / 'USERS' / 'DOCUMENTS'
    try:
        found = {p.name.upper(): p for p in d.iterdir()}
    except OSError:
        found = {}
    for n in names:
        try:
            got[n] = crc_of(found[n.upper()].read_bytes())
        except (KeyError, OSError):
            got[n] = None
    return got


def shown(got, names):
    return ', '.join('%s %s' % (n, (got.get(n) or ('missing',))[0]) for n in names)


def m8(r):
    a = r.a
    stamp = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(r.started))
    want = {'MAC.TXT': crc_of(MAC_TEXT)}
    r.do('mac-file', 'On the Mac, with the SurfOS card mounted (written to by SurfOS: make -C surfcpu deploy-sd\n'
                     'MOUNT=/Volumes/<card> DEPLOY_ARGS="--cmdline sd=rw"), make a file on it:\n'
                     '  printf \'made on the Mac\\n\' > /Volumes/<card>/USERS/DOCUMENTS/MAC.TXT\n'
                     'and press Enter (before ejecting).')
    if a.mount:
        got = mount_crcs(r, ['MAC.TXT'])
        r.check(got['MAC.TXT'] == want['MAC.TXT'], 'on the Mac (%s, read only): MAC.TXT has CRC %s, %s bytes'
                % (a.mount, *(got['MAC.TXT'] or ('missing', 0))))
    r.do('to-board', 'Eject the card (diskutil eject /Volumes/<card>), put it in the Arty, and press Enter.')
    r.open()
    r.do('boot', 'Load the design (make -C surfcpu program-volatile BOARD=%s DESIGN=%s), or press and release\n'
                 'RESET if it is loaded already; hold no button. Then press Enter.' % (a.board, r.design), power=True)
    boot = boot_surfos(r)
    if not r.check(boot['rw'], 'the card is mounted read-write (sd=rw in /BOOT/CMDLINE.TXT)'):
        raise Stop()
    boot_identity(r, boot)
    got = crcs(r, ['MAC.TXT'])
    r.check(got.get('MAC.TXT') == want['MAC.TXT'], 'a file made on the Mac, read on the board: MAC.TXT %s (%s on the Mac)'
            % ((got.get('MAC.TXT') or ('missing',))[0], want['MAC.TXT'][0]))
    board_text = 'made on the board %s' % stamp
    r.cmd('cd /sd0p1/USERS/DOCUMENTS')
    for line in ('write BOARD.TXT %s' % board_text, 'append BOARD.TXT a second line', 'cp MAC.TXT COPY.TXT',
                 'write MAC.TXT changed on the board'):
        out = r.cmd(line, timeout=60)
        r.check(not out.strip() or re.fullmatch(r'\s*\d+ bytes copied\s*', out),
                '%s: done without a word of error%s' % (line, ' (%s)' % out.strip() if out.strip() else ''))
    want.update({'BOARD.TXT': crc_of(('%s\na second line\n' % board_text).encode()), 'COPY.TXT': crc_of(MAC_TEXT),
                 'MAC.TXT': crc_of(b'changed on the board\n')})
    names = ['BOARD.TXT', 'COPY.TXT', 'MAC.TXT']
    got = crcs(r, names)
    r.check(all(got.get(n) == want[n] for n in names), 'create, edit, read: %s' % shown(got, names))
    r.cmd('cd /')
    out = r.cmd('eject sd0', timeout=60)
    r.check('sd0: safe to remove' in out, 'eject sd0: safe to remove')
    # a power cycle after the clean eject
    r.do('power-cycle', POWER_UP % (a.board, r.design, 'Hold no button.'), power=True)
    boot_surfos(r, check_identity=False)
    got = crcs(r, names)
    r.check(all(got.get(n) == want[n] for n in names), 'after a power cycle the same CRCs: %s' % shown(got, names))
    out = r.cmd('eject sd0', timeout=60)
    r.check('sd0: safe to remove' in out, 'eject sd0 again: safe to remove')
    # the Mac reads the same bytes
    listing = '\n'.join('  %s  %s  (%d bytes)' % (n, want[n][0], want[n][1]) for n in names)
    if a.mount:
        r.do('to-mac', 'Take the card out of the Arty, put it in the Mac (it mounts at %s), then press Enter.'
             % a.mount, power=True)
        got = mount_crcs(r, names)
        r.check(all(got[n] == want[n] for n in names), 'on the Mac (read only, %s) the same CRCs: %s' % (
            a.mount, shown(got, names)))
    else:
        r.observe('to-mac', 'Take the card out of the Arty and put it in the Mac. In Terminal:\n'
                            '  crc32 /Volumes/<card>/USERS/DOCUMENTS/*.TXT\n'
                            'Does the Mac print these CRCs for these files?\n' + listing)
    # a kernel update leaves the files alone
    if r.ask('kernel-update', 'Install the kernel again (make -C surfcpu deploy-sd MOUNT=/Volumes/<card>), eject the\n'
                              'card and put it back in the Arty. ' + POWER_UP % (a.board, r.design, 'Hold no button.'),
             power=True) != 'yes':
        r.checks.append(('skip', 'the kernel update not done by the owner'))
        return
    boot_surfos(r, check_identity=False)
    got = crcs(r, names)
    r.check(all(got.get(n) == want[n] for n in names), 'after a kernel update the files keep their CRCs: %s'
            % shown(got, names))
    out = r.cmd('eject sd0', timeout=60)
    r.check('sd0: safe to remove' in out, 'eject sd0 at the end: safe to remove')
    r.extra.append(('The files and the CRCs they must have:', [
        'MAC.TXT made on the Mac: %s (%d bytes); then changed on the board' % crc_of(MAC_TEXT)] + listing.split('\n')))


# ============================================================================== m9
def m9(r):
    a = r.a
    builds = []
    if not a.soak_only:
        r.do('flash', 'Make sure the board starts from its configuration flash by itself: programmed with make -C\n'
                      'surfcpu program-flash, the Arty\'s JP1 (MODE) jumper on, the SurfOS card (with sd=rw in\n'
                      '/BOOT/CMDLINE.TXT) in the Pmod MicroSD. Then press Enter.')
    r.open()
    for i in range(1, (0 if a.soak_only else a.boots) + 1):
        start = r.pos
        if r.ask('power-cycle', 'Cold boot %d of %d: unplug the Arty\'s USB cable, wait 5 seconds, plug it back in (no\n'
                                'programming, no button held), then press Enter.' % (i, a.boots), power=True) != 'yes':
            r.fail('cold boot %d: the owner did not power-cycle' % i)
            raise Stop()
        r.pos = start
        boot = boot_surfos(r, check_identity=i == 1)
        build = boot['bitstream']
        if build is None:                   # SurfBoot's lines missed: the kernel says which bitstream it runs on
            k = kernel_identity(r)
            build = k[0] if k else None
            if i == 1 and k:
                r.compare_bitstream(*k)
        elif i == 1:
            kernel_identity(r)
        builds.append(build)
        out = r.cmd('uptime')
        up = re.search(r'up (\d+)\.(\d+) s', out)
        fresh = bool(up) and float(up.group(1) + '.' + up.group(2)) < 60 * r.slow
        r.check(fresh and build is not None and build == builds[0],
                'cold boot %d: SurfBoot, the kernel and the shell; up %s s, a fresh boot; bitstream %s'
                % (i, up.group(1) + '.' + up.group(2) if up else '?', build))
    if builds:
        r.check(len(set(builds)) == 1 and None not in builds and len(builds) == a.boots,
                '%d cold boots in a row, each one checked, all from the bitstream %s' % (len(builds), builds[0]))
    if a.minutes <= 0:
        return
    if a.soak_only:
        r.do('boot', POWER_UP % (a.board, r.design, 'Hold no button.'), power=True)
        boot_identity(r, boot_surfos(r))
    soak(r, a.minutes)


def soak(r, minutes):
    out = r.cmd('mount')
    if not r.check(re.search(r'sd0p1\s+on /sd0p1\s+type fat(?! \(ro\))', out) is not None,
                   'the soak: /sd0p1 is mounted read-write (sd=rw)'):
        raise Stop()
    deadline = time.time() + minutes * 60
    n = 0
    up0 = None
    doc = '/sd0p1/USERS/DOCUMENTS'
    t0 = time.time()
    counts, heap = [], []
    while time.time() < deadline:
        n += 1
        out = r.cmd('uptime', what='soak %d: uptime' % n)
        up = re.search(r'up (\d+)\.(\d+) s \(\d+ ticks at \d+ Hz\), (\d+) tasks', out)
        upv = float(up.group(1) + '.' + up.group(2)) if up else -1
        if up0 is not None and upv <= up0:
            r.fail('soak %d: the uptime did not advance (%.2f s after %.2f s)' % (n, upv, up0))
            raise Stop()
        up0 = upv
        if up:
            counts.append(int(up.group(3)))
            if counts[-1] > counts[0] + 2:
                r.fail('soak %d: %d tasks, %d at the start: tasks are piling up' % (n, counts[-1], counts[0]))
                raise Stop()
        r.cmd('ps', what='soak %d: ps' % n)
        out = r.cmd('tasks', timeout=60, what='soak %d: tasks' % n)
        if 'both ended (status 0, 0)' not in out:
            r.fail('soak %d: the two tasks did not both end well' % n)
            raise Stop()
        text = 'soak iteration %d' % n
        for line in ('write %s/SOAK.TXT %s' % (doc, text), 'cp %s/SOAK.TXT %s/SOAK2.TXT' % (doc, doc)):
            out = r.cmd(line, timeout=60, what='soak %d' % n)
            if out.strip() and not re.fullmatch(r'\s*\d+ bytes copied\s*', out):
                r.fail('soak %d: %s said: %s' % (n, line, out.strip()))
                raise Stop()
        got = crcs(r, ['SOAK.TXT', 'SOAK2.TXT'])
        want = crc_of((text + '\n').encode())
        if got.get('SOAK.TXT') != want or got.get('SOAK2.TXT') != want:
            r.fail('soak %d: CRCs %s, not %s' % (n, got, want))
            raise Stop()
        out = r.cmd('rm %s/SOAK2.TXT' % doc, what='soak %d: rm' % n)
        if out.strip():
            r.fail('soak %d: rm said: %s' % (n, out.strip()))
            raise Stop()
        out = r.cmd('memstat', what='soak %d: memstat' % n)
        if 'heap check: ok' not in out:
            r.fail('soak %d: memstat does not say "heap check: ok"' % n)
            raise Stop()
        used = re.search(r'^kernel heap: [^\n]*\n\s+used: (\d+) bytes', out, re.M)
        if used:
            heap.append(int(used.group(1)))
    r.check(n > 0, 'the soak: %d rounds of uptime, ps, tasks, write, cp, crc32, rm and memstat in %.1f minutes, every '
            'CRC right, the heap checked every round, no panic and no killed task' % (n, (time.time() - t0) / 60))
    if counts:
        r.note('the soak: %s tasks at each uptime' % ('%d' % counts[0] if len(set(counts)) == 1 else
                                                      '%d to %d' % (min(counts), max(counts))))
    if heap:
        r.note('the soak: the kernel heap\'s use went from %d to %d bytes (between %d and %d)'
               % (heap[0], heap[-1], min(heap), max(heap)))


# ============================================================================== main
# each procedure's design: the one the owner is asked to load, and whose build manifest the bitstream is compared
# with (--design names another: m3 with surfcpu_arty_a7_ddr_bramdiag runs the DDR diagnostics too). m5-m9 need
# SurfBoot with DDR: surfcpu_arty_a7_top, SurfBoot without DDR, stops at DDR_ABSENT
PROCEDURES = {
    'm1': (m1, 'the baseline design (milestone 1): LEDs, clocks, reset and the UART', 'arty_a7_baseline'),
    'm3': (m3, 'the CPU on the board (milestone 3): the BRAM diagnostics', 'surfcpu_arty_a7_bramdiag'),
    'm5': (m5, 'SurfBoot and the card (milestone 5): the DDR diagnostic from the card, missing and invalid images',
           'surfcpu_arty_a7_ddr'),
    'm7': (m7, 'SurfOS from the card (milestone 7): the shell, two kernel tasks, the uptime, the keyboard',
           'surfcpu_arty_a7_ddr'),
    'm8': (m8, 'persistent files (milestone 8): OWNER-GUIDE.md section 8\'s CRC procedure', 'surfcpu_arty_a7_ddr'),
    'm9': (m9, 'the standalone release (milestone 9): cold boots and a soak', 'surfcpu_arty_a7_ddr'),
}


def main(argv=None):
    ap = argparse.ArgumentParser(description='SurfCPU physical acceptance tests')
    ap.add_argument('milestone', choices=sorted(PROCEDURES))
    ap.add_argument('--port', default='auto', help='the Arty\'s UART: /dev/cu.usbserial-<serial>1 (uart.py list); '
                                                  'auto (the default): the only such port plugged in')
    ap.add_argument('--board', default=os.environ.get('BOARD', 'arty_a7_100t'), choices=sorted(BOARD_IDS))
    ap.add_argument('--design', help='the design to load, whose build manifest the bitstream is compared with '
                                     '(m1: arty_a7_baseline; m3: surfcpu_arty_a7_bramdiag, or surfcpu_arty_a7_ddr_bramdiag '
                                     'for the DDR diagnostics; m5-m9: surfcpu_arty_a7_ddr; make board-test passes '
                                     'DESIGN= as this)')
    ap.add_argument('--manifest', help='the build manifest (default build/fpga/BOARD/DESIGN/manifest.txt)')
    ap.add_argument('--kernel', default=str(REPO / 'build/riscv32/arty_a7/SURFOS.BIN'), help='the SURFOS.BIN installed')
    ap.add_argument('--ddrdiag', default=str(SURFCPU / 'build/fw/DDRDIAG.BIN'), help='the DDRDIAG.BIN on the card')
    ap.add_argument('--evidence-dir', help='where the evidence goes (default docs/evidence/physical/; '
                                           'build/board-test/ for a simulation)')
    ap.add_argument('--owner', help='a command that answers for the owner (tests): CMD STEP, the question on stdin')
    ap.add_argument('--slow', type=float, default=1.0, help='multiply every time limit (the RTL system: about 10)')
    ap.add_argument('--pace', type=float, default=uart.PACE, help='seconds between typed bytes')
    ap.add_argument('--lines', type=int, default=10, help='m1: baseline lines to check')
    ap.add_argument('--mount', help='m8: the card\'s mount point on the Mac, read only (the CRCs on the Mac)')
    ap.add_argument('--boots', type=int, default=10, help='m9: cold boots')
    ap.add_argument('--minutes', type=float, default=60, help='m9: the soak\'s length (0: none)')
    ap.add_argument('--soak-only', action='store_true', help='m9: the soak without the cold boots')
    ap.add_argument('--skip-no-card', action='store_true', help='m5: leave out the missing-card part')
    a = ap.parse_args(argv)
    fn, title, design = PROCEDURES[a.milestone]
    design = a.design or design
    r = Run(a, a.milestone, title, design)
    # the bitstream is identified by its build manifest: without one, or with another design's, the
    # identity cannot be checked, so the run could not pass; refused before the owner is asked anything
    if not r.manifest:
        ap.error('no build manifest at %s, so the bitstream loaded could not be identified and this run could not '
                 'pass. Name the design you load (make board-test DESIGN=..., or --design) or its build manifest '
                 '(--manifest FILE)' % r.manifest_path)
    if str(r.manifest.get('manifest_format', '1')) not in MANIFEST_FORMATS:
        ap.error('the build manifest %s is format %s, which this script does not know (it reads %s): use the tools of '
                 'a commit that knows it' % (r.manifest_path, r.manifest.get('manifest_format'),
                                            ' and '.join(MANIFEST_FORMATS)))
    if r.manifest.get('design') and r.manifest['design'] != design:
        ap.error('the build manifest %s is %s\'s, not that of the design this run asks you to load (%s): name the '
                 'design you load (make board-test DESIGN=..., or --design)' % (r.manifest_path,
                                                                             r.manifest['design'], design))
    if a.port == 'auto':
        a.port, why = uart.auto_port()
        if not a.port:
            ap.error('--port auto: %s' % why)
    print('board-test %s: %s' % (a.milestone, title))
    interrupted = False
    try:
        fn(r)
    except Stop:
        pass
    except KeyboardInterrupt:
        interrupted = True
    finally:
        if r.link:
            time.sleep(0.3)
            r.link.close()
    path, verdict, detail = r.write(interrupted)
    print('board-test %s: %s (%s)%s; evidence in %s' % (a.milestone, verdict, detail,
                                                       ' [SIMULATION]' if r.sim else '', path))
    for s, text in r.checks:
        if s in ('FAIL', 'owner-no'):
            print('  %s %s' % (s, text))
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
