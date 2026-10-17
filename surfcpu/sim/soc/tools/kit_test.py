#!/usr/bin/env python3
"""
The physical test kit, proven before there is a board (milestone 1-9 acceptance procedures)
File: kit_test.py Date: 10/16/26 (SurfCPU, the os track)

make test-os-kit (on make check: about a minute) and make test-os-kit-rtl (a few minutes), in
mk/area-os.mk. tools/board-test.py and tools/uart.py run as the owner will run them, against:

  the RTL system   surfsim --uart-pty: the whole SoC with SurfBoot or the BRAM diagnostics in
                   its ROM, the DDR model and the io track's card and keyboard models, on a
                   pseudo-terminal as the board is on its USB serial port. m3 (bramdiag), m5
                   (SurfBoot's monitor: a missing and an invalid image, the DDR diagnostic from
                   the card, a missing card and the retry when it is back), m7 (SurfOS to the
                   shell, tasks, uptime, the keyboard model typing), m8 (the CRC procedure: the
                   "Mac" is mtools on the card image, a power cycle a new surfsim run on the
                   image the last one flushed) and m9 (two cold boots, a short soak).
  fake boards      a thread here on a pseudo-terminal: the baseline design's 1 Hz line for m1,
                   with its switches, a button, RESET and the receive pin; uart.py's capture,
                   script and console.

The edge cases must give a clear FAIL, in seconds, never a PASS: on fake boards a port that
goes away mid-run (USB unplugged), garbage on the line, a port where nothing arrives, a wrong
build manifest and a lost second; on the RTL system a surfsim run that ends in the middle of the
DDR diagnostic (the port gone) and a card without a kernel.

  --set check   the fake boards, m3, m7 and the RTL edge cases (make test-os-kit)
  --set rtl     m5, m8 and m9 (make test-os-kit-rtl); --set all: both; --set fake: no surfsim

The owner's steps are answered by `kit_test.py owner STATE STEP` (board-test.py --owner): it
starts, power-cycles and resets (SIGTERM: surfsim ends as a power cut, --sd-flush keeping what
the card got, and a new run starts on the image), takes the card out and puts it back (a run
with --sd-remove, SIGUSR2), writes and reads the card image with mtools while the "board" is
off, or sets a fake board's switches. Things only a person can see (LEDs, the VGA monitor) are
answered "cannot be observed here", and board-test.py lists them as not observed. The surfsim
runs are this script's own children, stopped by their pids.

  kit_test.py --surfsim build/sim/surfsim --fw build/fw --kernel ../build/riscv32/arty_a7/SURFOS.BIN
              --mkcard tools/mkcard.py [--set check|rtl|all|fake] [--only NAMES] [--work DIR]
              [--evidence FILE] [--evidence-full DIR] [--soak-minutes 1] [--jobs 4]

Exit status 0 when every scenario gave the verdict it must (PASS, or a clear FAIL for the
edge cases), in its time limit.
"""

import argparse
import json
import os
import pty
import random
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SURFCPU = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
BOARD_TEST = os.path.join(SURFCPU, 'tools', 'board-test.py')
UART = os.path.join(SURFCPU, 'tools', 'uart.py')
PART_OFFSET = 2048 * 512
SLOW = 10                                   # the RTL system: about 7 times slower than the board


def mtools(args, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', LC_ALL='C.UTF-8', LANG='C.UTF-8')
    return subprocess.run(args, env=env, capture_output=True, **kw)


def relink(link, target):
    """point link at target, atomically"""
    tmp = link + '.tmp'
    if os.path.lexists(tmp):
        os.remove(tmp)
    os.symlink(target, tmp)
    os.replace(tmp, link)


# ====================================================================================== fake boards
# The DDR diagnostics (surfcpu_arty_a7_ddr_bramdiag) as the owner's A7-100T printed them, built at 0bcadbc with
# the fixed open flow (docs/evidence/physical/ddr-fixed-flow-2026-10-17.txt), but for the build id and the DDR
# status, which a fake board takes as arguments. bramdiag.c prints the bitstream line's flags and DDR status in
# hexadecimal (%x): the board printed 14 (PRESENT and SWINIT); 1c (PRESENT, TIMEOUT and SWINIT) has a letter.
BRAMDIAG_DDR = '''
SurfCPU BRAM diagnostics 1.0.0 (build {build})
board Arty A7-100T (0x100), SoC 0.2.0, memory map 2, sys 25000000 Hz, timebase 1000000 Hz
bitstream build {build} flags 0, features ff, DDR status {status} size 256 MiB
identity                           ok (7 checks)
ALU, immediates, x0                ok (33 checks)
M extension                        ok (23 checks)
branches and jumps                 ok (9 checks)
loads and stores                   ok (8 checks)
CSRs and counters                  ok (30 checks)
scratch RAM (March C-)             (56 KiB) ok (49 checks)
text RAM (cells 2400-4095)         ok (1 checks)
exceptions, each recovered         ok (34 checks)
interrupts, timer and WFI          (10 timer ticks in 10186 us) ok (7 checks)
FENCE.I, code written to RAM       ok (2 checks)
DDR initialization (DDR_STATUS.SWINIT), read leveling:
  lane 0: bitslip 2, delay 16 (window +-8)
  lane 1: bitslip 2, delay 16 (window +-8)
  229 ms
DDR initialization                 ok (3 checks)
DDR calibration                    ok (1 checks)
DDR data lines                     ok (192 checks)
DDR byte lanes                     ok (9 checks)
DDR address lines and aliases      ok (54 checks)
DDR boundaries                     ok (98 checks)
DDR patterns                       (1024 KiB in full, 4096 blocks sampled) ok (2 checks)
DDR sustained copies               (4 x 256 KiB copied and checked, 720 KiB/s) ok (4 checks)
code running from DDR              ok (6 checks)

BRAMDIAG: PASS (572 checks)

UART echo: type; each character comes back with its code. Ctrl-R runs the diagnostics again.
'''


class FakeBoard(threading.Thread):
    """a pseudo-terminal the procedures open as a board's port; state comes from a control file
    the stand-in owner writes ({"sw": "0101", "btn_until": t, "reset": n, "program": n}). Modes: baseline
    (the 1 Hz line), bramdiag (the DDR diagnostics, run when the owner loads the design and on Ctrl-R, then
    each byte typed echoed with its code), echo, garbage, silent, skip, vanish"""

    def __init__(self, link, control, mode='baseline', **kw):
        super().__init__(daemon=True)
        self.master, self.slave = pty.openpty()
        import fcntl
        import tty
        tty.setraw(self.slave)
        self.slave_name = os.ttyname(self.slave)
        # The slave stays open here and the master never blocks, as a UART sends whether anyone
        # listens or not: on macOS a write to a master whose slave nobody has open can wait for ever
        fcntl.fcntl(self.master, fcntl.F_SETFL, fcntl.fcntl(self.master, fcntl.F_GETFL) | os.O_NONBLOCK)
        self.link, self.control, self.mode, self.kw = link, control, mode, kw
        relink(link, self.slave_name)
        self.stop = False
        self.received = b''
        self.closed = False

    def state(self):
        try:
            return json.load(open(self.control))
        except (OSError, ValueError):
            return {}

    def emit(self, data):
        try:
            os.write(self.master, data)
        except OSError:
            pass

    def emit_all(self, data, limit=5.0):
        """all of data, a part at a time as the reader makes room (the master does not block)"""
        deadline = time.time() + limit
        while data and time.time() < deadline and not self.stop:
            try:
                data = data[os.write(self.master, data):]
            except BlockingIOError:
                time.sleep(0.01)
            except OSError:
                return

    def diag_run(self):
        """the DDR diagnostics from the start, then the echo loop"""
        text = BRAMDIAG_DDR.format(build=self.kw.get('build', '0bcadbcf'), status=self.kw.get('ddr_status', '14'))
        self.emit_all(text.replace('\n', '\r\n').encode())
        self.echoed = 0

    def diag_input(self, data):
        for b in data:
            if b == 0x12:                                   # Ctrl-R: the diagnostics again
                self.diag_run()
            elif self.echoed is not None:
                self.echoed += 1
                self.emit_all(("echo %3d: '%c' 0x%02x\r\n" % (self.echoed, b, b)).encode())

    def run(self):
        up, rx, resets, lines, loads = 0, '--', 0, 0, 0
        self.echoed = None                                  # the diagnostics' echo loop: bytes echoed so far
        next_t = time.time() + 1.0
        rnd = random.Random(7)
        while not self.stop:
            try:
                import select
                r, _, _ = select.select([self.master], [], [], 0.05)
                if r:
                    data = os.read(self.master, 1024)
                    self.received += data
                    if data:
                        rx = '%02x' % data[-1]
                        if self.mode == 'echo':
                            self.emit(data.replace(b'\r', b'\r\n'))
                        elif self.mode == 'bramdiag':
                            self.diag_input(data)
            except OSError:
                pass
            if self.mode == 'garbage':
                self.emit(bytes(rnd.getrandbits(8) for _ in range(64)))
                time.sleep(0.1)
                continue
            if self.mode in ('silent', 'echo'):
                continue
            if self.mode == 'bramdiag':
                st = self.state()
                if st.get('program', 0) != loads:           # the owner loaded the design: a fresh run
                    loads = st['program']
                    self.diag_run()
                continue
            st = self.state()
            if st.get('reset', 0) != resets:
                resets = st['reset']
                up = 0
                next_t = time.time() + 1.0
            if time.time() >= next_t:
                next_t += 1.0
                up += 1
                lines += 1
                if self.mode == 'skip' and lines == 3:
                    up += 1                                     # a second lost
                btn = '0001' if time.time() < st.get('btn_until', 0) else '0000'
                line = 'SurfCPU baseline %s 0x%08x build %s %s %d | up %06d s | pix %d Hz | sw %s btn %s rx %s\r\n' % (
                    self.kw.get('board', 'arty_a7_100t'), self.kw.get('board_id', 0x100), self.kw.get('build', '1a2b3c4d'),
                    self.kw.get('state', 'clean'), self.kw.get('time', 1760560000), up,
                    25173611 if lines > 1 else 25173604, st.get('sw', '0000'), btn, rx)
                self.emit(line.encode())
                if self.mode == 'vanish' and lines == self.kw.get('vanish_after', 2):
                    time.sleep(0.2)                         # the line on its way, then the cable is pulled
                    os.close(self.master)
                    self.closed = True
                    return

    def close(self):
        self.stop = True
        self.join(2)
        for fd in ([] if self.closed else [self.master]) + [self.slave]:
            try:
                os.close(fd)
            except OSError:
                pass


# ====================================================================================== the stand-in owner
BOOT_STEPS = ('program', 'monitor', 'boot', 'power-cycle')


def load(state_path):
    return json.load(open(state_path))


def save(state_path, st):
    tmp = state_path + '.tmp'
    json.dump(st, open(tmp, 'w'), indent=1)
    os.replace(tmp, state_path)


def alive(pid):
    try:
        with open('/proc/%d/status' % pid) as f:
            return not any(l.startswith('State:') and 'Z' in l.split()[1] for l in f)
    except OSError:
        return False


def start_sim(st, extra=None):
    """a new surfsim run on the card image (a power-up): extra, or the scenario's own options for its first run"""
    n = st['runs'] + 1
    err_path = os.path.join(st['dir'], 'sim%d.err' % n)
    argv = [st['surfsim'], '--rom', st['rom'], '--uart-pty', '--quiet', '--sd-flush', '--seed', '1',
            '--max-cycles', str(st['max_cycles'])]
    if st.get('card'):
        argv += ['--sd', st['card']]
    argv += list(st.get('extra', []) if extra is None else extra)
    with open(os.path.join(st['dir'], 'sim%d.cmd' % n), 'w') as f:
        f.write(' '.join(argv) + '\n')
    p = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=open(err_path, 'w'), start_new_session=True)
    deadline = time.time() + 60
    path = None
    while time.time() < deadline and path is None:
        time.sleep(0.05)
        m = re.search(r'surfsim: --uart-pty (/dev/\S+)\n', open(err_path).read())
        path = m.group(1) if m else None
        if p.poll() is not None and path is None:
            break
    if path is None:
        raise SystemExit('kit_test: surfsim gave no pseudo-terminal (%s)' % err_path)
    relink(st['link'], path)
    st.update(pid=p.pid, fresh=True, runs=n)
    st.setdefault('pids', []).append(p.pid)


def stop_sim(st, sig=signal.SIGTERM, wait=180):
    pid = st.get('pid')
    if not pid:
        return
    if alive(pid):
        os.kill(pid, sig)
        deadline = time.time() + wait
        while alive(pid) and time.time() < deadline:
            time.sleep(0.1)
        if alive(pid):
            os.kill(pid, signal.SIGKILL)                       # our own surfsim, by its pid
    st['pid'] = None


def extract(st):
    """the card on "the Mac": /USERS/DOCUMENTS copied out of the image into the mount"""
    m = st.get('mount')
    if not m:
        return
    shutil.rmtree(m, ignore_errors=True)
    os.makedirs(os.path.join(m, 'USERS'))
    mtools(['mcopy', '-s', '-n', '-i', '%s@@%d' % (st['card'], PART_OFFSET), '::/USERS/DOCUMENTS',
            os.path.join(m, 'USERS')])


def owner(state_path, step):
    """board-test.py's --owner: do the step on the RTL system or a fake board. Exit 0 done or yes, 1 no,
    3 cannot be observed here"""
    question = sys.stdin.read()
    st = load(state_path)
    with open(os.path.join(st['dir'], 'owner.log'), 'a') as f:
        f.write('%s %s\n' % (time.strftime('%H:%M:%S'), step))
    code = 0
    if step in ('leds', 'vga'):
        code = 3
    elif st['mode'] == 'fake':
        c = st['control']
        cs = json.load(open(c)) if os.path.exists(c) else {}
        hold = st.get('slow', {}).get(step, 0)                  # an owner who takes a while (seconds)
        if step == 'switches':
            cs['sw'] = '0101'
        elif step == 'button':
            cs['btn_until'] = time.time() + 2.5                 # held for 2.5 s, then released
            hold = max(hold, 2.6)
        elif step == 'reset':
            cs['reset'] = cs.get('reset', 0) + 1
            hold = max(hold, 0.5)
        elif step == 'program':
            cs['program'] = cs.get('program', 0) + 1            # the design loaded (the bramdiag board runs)
        tmp = c + '.tmp'
        json.dump(cs, open(tmp, 'w'))
        os.replace(tmp, c)
        time.sleep(hold)
    elif step in BOOT_STEPS:
        if st.get('pid') and st.get('fresh'):
            st['fresh'] = False                                 # it starts as the port is opened
        else:
            stop_sim(st)
            start_sim(st)
            st['fresh'] = False
    elif step == 'card-out':                                    # out, then RESET: a new run whose card comes out
        stop_sim(st)                                            # at SurfBoot's banner, before it looks
        start_sim(st, ['--sd-remove', 'SurfBoot'])                # no button held
        st['fresh'] = False
    elif step == 'card-in':
        os.kill(st['pid'], signal.SIGUSR2)
    elif step == 'mac-file':                                    # the card in the Mac: the board is off
        stop_sim(st)
        tmp = os.path.join(st['dir'], 'MAC.TXT')
        open(tmp, 'wb').write(b'made on the Mac\n')
        r = mtools(['mcopy', '-o', '-i', '%s@@%d' % (st['card'], PART_OFFSET), tmp, '::/USERS/DOCUMENTS/MAC.TXT'])
        code = 0 if r.returncode == 0 else 1
        extract(st)
    elif step == 'to-board':
        if not st.get('pid'):
            start_sim(st)
    elif step == 'to-mac':
        stop_sim(st)
        extract(st)
    elif step == 'kernel-update':                               # deploy-sd's part: the package on the card
        stop_sim(st)
        r = mtools(['mcopy', '-o', '-i', '%s@@%d' % (st['card'], PART_OFFSET), st['kernel'], '::/BOOT/SURFOS.BIN'])
        code = 0 if r.returncode == 0 else 1
        start_sim(st)
    elif step in ('card', 'flash', 'keyboard'):
        pass                                                    # the card was made so; the keyboard model types
    else:
        code = 1
    save(state_path, st)
    return code


# ====================================================================================== the scenarios
class Kit:
    def __init__(self, a):
        self.a = a
        self.results = []
        self.work = os.path.abspath(a.work)
        self.lock = threading.Lock()
        self.sim_manifest = None
        self.runs = []                      # (name, milestone, against, verdict, seconds, evidence text)
        self.rtl_slots = threading.Semaphore(max(1, a.jobs))

    def check(self, ok, what):
        with self.lock:
            self.results.append((bool(ok), what))
            print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    def board_test(self, name, milestone, link, state, args=(), timeout=600):
        """board-test.py as the owner runs it, the stand-in owner answering: (exit, evidence text, seconds)"""
        d = os.path.join(self.work, name)
        ev = os.path.join(d, 'evidence')
        os.makedirs(ev, exist_ok=True)
        argv = [sys.executable, BOARD_TEST, milestone, '--port', link, '--evidence-dir', ev,
                '--owner', ' '.join(shlex.quote(x) for x in (sys.executable, os.path.abspath(__file__), 'owner', state))
                ] + list(args)                  # board-test.py splits --owner as a shell would: paths with spaces quoted
        t0 = time.time()
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
            code, out = r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired as e:
            code, out = 'timeout', 'board-test.py did not end within %d s: %r' % (timeout, e.stdout)
        dt = time.time() - t0
        with open(os.path.join(d, 'board-test.out'), 'w') as f:
            f.write(' '.join(argv) + '\n' + str(out))
        files = sorted(os.listdir(ev))
        text = open(os.path.join(ev, files[-1])).read() if files else ''
        return code, text, dt

    @staticmethod
    def verdict(text):
        m = re.search(r'^Verdict: (\S+) \(([^\n]*)\)', text, re.M)
        return (m.group(1), m.group(2)) if m else ('none', 'no evidence written')

    def judge(self, name, milestone, against, code, text, dt, expect, must, within, must_not=()):
        """the verdict board-test.py had to give, its exit status, its time and the lines its evidence needs"""
        v, detail = self.verdict(text)
        missing = [p for p in must if not re.search(p, text)] + ['not ' + p for p in must_not if re.search(p, text)]
        ok = v == expect and code == (0 if expect == 'PASS' else 1) and dt <= within and not missing
        if against == 'the RTL system':
            ok = ok and 'Source: SIMULATION' in text
        self.check(ok, '%s: board-test %s against %s: %s in %.0f s (%s)%s' % (
            name, milestone, against, v, dt, detail, '' if ok else ' -- expected %s within %d s; missing: %s' % (
                expect, within, missing)))
        with self.lock:
            self.runs.append((name, milestone, against, v, dt, text))

    # ---------------------------------------------------------------- fake boards
    def fake(self, name, milestone, mode, args=(), manifest=None, expect='PASS', must=(), within=60, slow=None, **kw):
        """slow: {step: seconds} the stand-in owner takes over a step"""
        d = os.path.join(self.work, name)
        os.makedirs(d, exist_ok=True)
        link = os.path.join(d, 'port')
        control = os.path.join(d, 'control.json')
        json.dump({}, open(control, 'w'))
        state = os.path.join(d, 'state.json')
        save(state, {'mode': 'fake', 'control': control, 'dir': d, 'slow': slow or {}})
        fb = FakeBoard(link, control, mode, **kw)
        fb.start()
        extra = list(args)
        if manifest:
            mpath = os.path.join(d, 'manifest.txt')
            open(mpath, 'w').write(manifest)
            extra += ['--manifest', mpath]
        try:
            code, text, dt = self.board_test(name, milestone, link, state, extra, timeout=within + 30)
        finally:
            fb.close()
        self.judge(name, milestone, 'a fake board (%s)' % mode, code, text, dt, expect, must, within)

    def fake_jobs(self):
        good = ('# a fake board\'s build manifest\nbuild_id = "1a2b3c4d"\nbuild_time = 1760560000\n'
                'source_dirty = false\nboard = "arty_a7_100t"\nresult = "pass"\n')
        diag = ('# the DDR diagnostics build of the fixed flow, as a manifest for the fake board\n'
                'design = "surfcpu_arty_a7_ddr_bramdiag"\nbuild_id = "0bcadbcf"\nsource_dirty = false\n'
                'board = "arty_a7_100t"\nresult = "pass"\n')
        return [
            # m3 asked for the DDR diagnostics: the bitstream line's DDR status is hexadecimal, and 1c (TIMEOUT
            # set) once failed board-test.py's decimal parse ("the bitstream line: not seen"); the step names
            # the design asked for
            ('m3-ddr-status-hex', lambda: self.fake(
                'm3-ddr-status-hex', 'm3', 'bramdiag', ['--board', 'arty_a7_100t', '--design',
                                                        'surfcpu_arty_a7_ddr_bramdiag'], manifest=diag,
                within=30, ddr_status='1c',
                must=[r'program-volatile BOARD=arty_a7_100t DESIGN=surfcpu_arty_a7_ddr_bramdiag\n',
                      r'ok\s+board id 0x100 is arty_a7_100t\'s', r'ok\s+the bitstream\'s build id 0bcadbcf is the '
                      r'manifest\'s', r'ok\s+its dirty flag \(0\) matches', r'ok\s+the system clock is 25,000,000 Hz',
                      r'ok\s+DDR initialization: ok', r'ok\s+code running from DDR: ok',
                      r'ok\s+BRAMDIAG: PASS \(572 checks\)', r'ok\s+the UART both ways',
                      r'skip\s+not observed: Are LD4'])),
            ('m1-baseline', lambda: self.fake(
                'm1-baseline', 'm1', 'baseline', ['--lines', '4'], manifest=good, within=40,
                must=[r'ok\s+4 lines a second', r'ok\s+the seconds since reset count up by one',
                      r'ok\s+a line every second by the Mac\'s clock', r'ok\s+the pixel clock measured',
                      r'ok\s+the FPGA\'s receive pin: the byte typed \(S\) shows as rx 53',
                      r'ok\s+the switches: a line shows sw 0101', r'ok\s+the buttons: a line shows BTN0 pressed',
                      r'ok\s+RESET restarts the count', r'ok\s+the line\'s build .* match the manifest',
                      r'skip\s+not observed: Do LD0 and LD1 blink', r'(?m)^Owner: the steps were answered by a program'])),
            # the owner presses Enter 3.5 s after being asked to load the design: the lines that came
            # meanwhile wait in the buffer and must keep the times they arrived (the first board run,
            # 2026-10-16, saw 0.00 s gaps)
            ('m1-slow-owner', lambda: self.fake(
                'm1-slow-owner', 'm1', 'baseline', ['--lines', '6'], manifest=good, within=45,
                slow={'program': 3.5},
                must=[r'ok\s+a line every second by the Mac\'s clock', r'ok\s+6 lines a second',
                      r'(?m)^  \+\s+0\.\d  program\s+done, answered at \+\s*[3-5]\.\d'])),
            ('m1-wrong-bitstream', lambda: self.fake(
                'm1-wrong-bitstream', 'm1', 'baseline', ['--lines', '3'], within=40, expect='FAIL',
                manifest=good.replace('1a2b3c4d', '9999aaaa'), must=[r'FAIL\s+the line\'s build .* do NOT match'])),
            ('edge-unplugged', lambda: self.fake(
                'edge-unplugged', 'm1', 'vanish', ['--lines', '5'], expect='FAIL', within=15,
                must=[r'FAIL\s+baseline line \d of 5: the port went away at \+[\d.]+ s',
                      r'USB unplugged, or the board lost power\?', r'\* the port went away'])),
            ('edge-garbage', lambda: self.fake(
                'edge-garbage', 'm1', 'garbage', ['--lines', '3'], expect='FAIL', within=20,
                must=[r'FAIL\s+baseline line 1 of 3: not seen within 5 s', r'The bytes look garbled: the wrong port'])),
            ('edge-silent', lambda: self.fake(
                'edge-silent', 'm1', 'silent', ['--lines', '3'], expect='FAIL', within=20,
                must=[r'not seen within 5 s', r'Nothing at all arrived on the port'])),
            ('edge-lost-second', lambda: self.fake(
                'edge-lost-second', 'm1', 'skip', ['--lines', '4'], expect='FAIL', within=30,
                must=[r'FAIL\s+the seconds since reset count up by one a line \(1, 2, 4, 5\)'])),
            ('uart', self.uart_tests),
        ]

    def uart_tests(self):
        d = os.path.join(self.work, 'uart')
        os.makedirs(d, exist_ok=True)
        link = os.path.join(d, 'port')
        control = os.path.join(d, 'control.json')
        json.dump({}, open(control, 'w'))
        out = os.path.join(d, 'capture.txt')

        def run(argv, timeout=30):
            t0 = time.time()
            r = subprocess.run([sys.executable, UART] + argv, capture_output=True, text=True, timeout=timeout)
            return r.returncode, r.stdout + r.stderr, time.time() - t0

        def console(keys, pause=1.0):
            """uart.py console with keys typed, then Ctrl-]: (exit, what it printed, its log)"""
            log = os.path.join(d, 'console.log')
            if os.path.exists(log):
                os.remove(log)
            p = subprocess.Popen([sys.executable, UART, 'console', link, '--log', log], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            time.sleep(pause)
            try:
                p.stdin.write(keys)
                p.stdin.flush()
                time.sleep(pause)
                p.stdin.write(b'\x1d')
                p.stdin.flush()
            except OSError:
                pass                        # it ended first (the port went away)
            try:
                text = p.communicate(timeout=10)[0].decode('utf-8', 'replace')
            except subprocess.TimeoutExpired:
                p.kill()
                text = p.communicate()[0].decode('utf-8', 'replace') + '\n(did not end: killed)'
            return p.returncode, text, open(log, 'rb').read().decode('utf-8', 'replace') if os.path.exists(log) else ''

        fb = FakeBoard(link, control, 'baseline')
        fb.start()
        try:
            code, text, dt = run(['capture', link, '--until', r'up 000002 s[^\n]*\n', '--timeout', '10', '--out', out])
            cap = open(out).read() if os.path.exists(out) else ''
            self.check(code == 0 and re.search(r'^\+\s+[\d.]+ \| SurfCPU baseline .* up 000002 s', cap, re.M),
                       'uart.py capture: baseline lines recorded with their times, ended by --until (exit %s, %.1f s)'
                       % (code, dt))
            code, text, dt = run(['capture', link, '--until', 'never printed', '--timeout', '2', '--out', out])
            self.check(code == 2 and 'timeout' in text, 'uart.py capture: --timeout ends it: exit %s in %.1f s' % (code, dt))
        finally:
            fb.close()
        fb = FakeBoard(link, control, 'echo')
        fb.start()
        try:
            sc = os.path.join(d, 'echo.script')
            open(sc, 'w').write('# an echo: what is typed comes back\n--send ^=hello\\r\n--expect hello\\n\n'
                                '--send hello\\n=world\\r\n--expect world\\n\n--timeout 10\n')
            code, text, dt = run(['script', link, sc, '--log', os.path.join(d, 'script.log')])
            self.check(code == 0 and 'every expect matched' in text, 'uart.py script: --send and --expect in order, '
                       'typed with pacing: exit %s in %.1f s' % (code, dt))
            open(sc, 'w').write('--send ^=boom\\r\n--fail-on boom\n--expect never\n--timeout 10\n')
            code, text, dt = run(['script', link, sc])
            self.check(code == 1 and 'fail-on' in text, 'uart.py script: --fail-on ends it: exit %s in %.1f s' % (code, dt))
            open(sc, 'w').write('--expect never printed\n--timeout 2\n')
            code, text, dt = run(['script', link, sc])
            self.check(code == 2 and 'time ran out' in text, 'uart.py script: its --timeout ends it: exit %s in %.1f s'
                       % (code, dt))
            code, text, log = console(b'console-test\r')
            self.check(code == 0 and 'console-test' in text and 'console-test' in log,
                       'uart.py console: keys typed go out, what comes back is shown and logged, Ctrl-] quits (exit %s)'
                       % code)
        finally:
            fb.close()
        fb = FakeBoard(link, control, 'vanish', vanish_after=1)
        fb.start()
        try:
            code, text, dt = run(['capture', link, '--until', 'never printed', '--timeout', '20', '--out', out])
            self.check(code == 1 and 'gone' in text and dt < 10, 'uart.py capture: a port that goes away ends it at '
                       'once: exit %s in %.1f s, not at the 20 s limit' % (code, dt))
        finally:
            fb.close()
        fb = FakeBoard(link, control, 'vanish', vanish_after=1)
        fb.start()
        try:
            code, text, log = console(b'', pause=2.5)
            self.check(code == 1 and 'the port went away' in text, 'uart.py console: a port that goes away ends it: '
                       'exit %s, "the port went away"' % code)
        finally:
            fb.close()
        code, text, dt = run(['capture', os.path.join(d, 'no-such-port'), '--until', 'x', '--timeout', '1', '--out', out])
        self.check(code == 1 and 'No such file' in text, 'uart.py: a port that is not there: exit %s, and why' % code)
        code, text, dt = run(['list'])
        self.check(code in (0, 1) and text.strip(), 'uart.py list runs here (exit %s: %s)' % (
            code, text.strip().split('\n')[0]))

    # ---------------------------------------------------------------- the RTL system
    def card(self, path, files, size='36M'):
        argv = [sys.executable, self.a.mkcard, path, '--size', size, '--standard-dirs']
        for dest, src in files:
            argv += ['--file', '%s=%s' % (dest, src)]
        r = subprocess.run(argv, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('kit_test: mkcard: %s' % r.stderr)

    def rtl(self, name, milestone, rom, card_files=None, extra=(), args=(), must=(), within=900,
            prestart=True, mount=False, expect='PASS', max_cycles=20000000000):
        with self.rtl_slots:
            self._rtl(name, milestone, rom, card_files, extra, args, must, within, prestart, mount, expect, max_cycles)

    # a PASS on the RTL must have read every line: the port is open before each run starts
    RTL_MUST_NOT = [r'SurfBoot\'s first lines were not seen', r'first run was not seen']

    def _rtl(self, name, milestone, rom, card_files, extra, args, must, within, prestart, mount, expect, max_cycles):
        d = os.path.join(self.work, name)
        os.makedirs(d, exist_ok=True)
        link = os.path.join(d, 'port')
        state = os.path.join(d, 'state.json')
        card = None
        if card_files is not None:
            card = os.path.join(d, 'card.img')
            self.card(card, card_files)
        st = {'mode': 'rtl', 'dir': d, 'surfsim': self.a.surfsim, 'rom': rom, 'card': card, 'extra': list(extra),
              'link': link, 'pid': None, 'fresh': False, 'runs': 0, 'max_cycles': max_cycles,
              'kernel': self.a.kernel, 'mount': os.path.join(d, 'mount') if mount else None}
        if prestart:
            start_sim(st)
        save(state, st)
        margs = list(args) + ['--slow', str(SLOW)]
        if self.sim_manifest:
            margs += ['--manifest', self.sim_manifest]
        if mount:
            margs += ['--mount', st['mount']]
        try:
            code, text, dt = self.board_test(name, milestone, link, state, margs, timeout=within + 60)
        finally:
            st = load(state)
            stop_sim(st)
            for pid in st.get('pids', []):
                if alive(pid):
                    os.kill(pid, signal.SIGKILL)            # only the surfsim runs this scenario started
            save(state, st)
            if card and not self.a.keep:
                os.remove(card)
        self.judge(name, milestone, 'the RTL system', code, text, dt, expect, must, within,
                   self.RTL_MUST_NOT if expect == 'PASS' else ())

    def manifest_for_sim(self, d):
        """a build manifest with the identity surfsim's SoC reports (build/soc's stamp), so the comparison runs"""
        soc = os.path.join(SURFCPU, 'build', 'soc')
        stamps = sorted((os.path.getmtime(os.path.join(soc, f)), f) for f in os.listdir(soc)
                        if f.startswith('build-id.')) if os.path.isdir(soc) else []
        m = re.match(r'build-id\.([0-9a-f]{8})\.(\d+)\.stamp', stamps[-1][1]) if stamps else None
        if not m:
            return None
        path = os.path.join(d, 'manifest-sim.txt')
        open(path, 'w').write('# surfsim\'s build identity as a manifest, for the comparison (kit_test.py)\n'
                              'build_id = "%s"\nsource_dirty = %s\nresult = "pass"\n'
                              % (m.group(1), 'true' if int(m.group(2)) & 1 else 'false'))
        return path

    def rtl_jobs(self):
        fw = self.a.fw
        d = os.path.join(self.work, 'cardfiles')
        os.makedirs(d, exist_ok=True)
        files = {'CMDLINE.TXT': b'ascii sd=rw\n', 'DDRCMD.TXT': b'size=2M loops=1\n', 'SURFCARD.TXT':
                 b'SurfCPU card\n', 'HELLO.TXT': b'Hello from the card.\n'}
        for n, b in files.items():
            open(os.path.join(d, n), 'wb').write(b)
        system = [('/SYSTEM/SURFCARD.TXT', os.path.join(d, 'SURFCARD.TXT')),
                  ('/USERS/DOCUMENTS/HELLO.TXT', os.path.join(d, 'HELLO.TXT'))]
        os_card = [('/BOOT/SURFOS.BIN', self.a.kernel), ('/BOOT/CMDLINE.TXT', os.path.join(d, 'CMDLINE.TXT'))] + system
        m5_card = [('/BOOT/SURFOS.BIN', self.a.kernel), ('/BOOT/DDRDIAG.BIN', os.path.join(fw, 'DDRDIAG.BIN')),
                   ('/BOOT/CMDLINE.TXT', os.path.join(d, 'DDRCMD.TXT'))] + system
        boot = os.path.join(fw, 'surfboot.mem')
        m5_must = [r'ok\s+info: DDR calibrated', r'ok\s+a missing image: boot /BOOT/NOSUCH.BIN is diagnosed \(NOT_FOUND\)',
                   r'ok\s+an invalid image: check /SYSTEM/SURFCARD.TXT \(not a package\) is diagnosed \(IMG_SHORT\)',
                   r'ok\s+DDRDIAG.BIN on the card is', r'ok\s+the DDR diagnostic booted from the card: DDRDIAG: PASS',
                   r'ok\s+no card: SurfBoot stopped: no card \(NO_CARD\)', r'ok\s+the card back: "card inserted: booting"',
                   r'ok\s+the bitstream\'s build id [0-9a-f]{8} is the manifest\'s']
        return [
            ('m3', 'check', lambda: self.rtl(
                'm3', 'm3', os.path.join(fw, 'bramdiag.mem'), within=300,
                must=[r'ok\s+exceptions, each recovered: ok', r'ok\s+the timer\'s progress: \d+ timer ticks in \d+ us',
                      r'ok\s+BRAMDIAG: PASS', r'ok\s+the UART both ways', r'ok\s+the bitstream\'s build id',
                      r'bitstream build [0-9a-f]{8}, flags \d+.*\(SIMULATION\)', r'skip\s+not observed: Are LD4'])),
            ('m7', 'check', lambda: self.rtl(
                'm7', 'm7', boot, os_card, within=600, extra=['--ps2-send', r'\nkbd-ready\r?\n=echo ps2-ok\n'],
                must=[r'program-volatile BOARD=\S+ DESIGN=surfcpu_arty_a7_ddr\)',   # the design: SurfBoot with DDR
                      r'ok\s+SurfOS booted from the card to the shell', r'ok\s+SurfOS 1\.0\.0 on the card is',
                      r'ok\s+bootinfo: the kernel reads the bitstream\'s identity',
                      r'ok\s+two kernel tasks make progress, sleep and wake \(run 2', r'ok\s+the uptime advances',
                      r'ok\s+the PS/2 keyboard types into the shell', r'skip\s+not observed: Does the VGA monitor'])),
            ('edge-rtl-no-kernel', 'check', lambda: self.rtl(
                'edge-rtl-no-kernel', 'm7', boot, system, within=120, expect='FAIL',
                must=[r'FAIL\s+SurfBoot reading the kernel from the card: the board printed "SurfBoot stopped: file not '
                      r'found \(NOT_FOUND\)" instead \(then: "/BOOT/SURFOS.BIN does not exist"\)'])),
            ('edge-rtl-unplugged', 'check', lambda: self.rtl(
                'edge-rtl-unplugged', 'm5', boot, m5_card, within=240, expect='FAIL', extra=['--btn', '2'],
                max_cycles=110000000, args=['--ddrdiag', os.path.join(fw, 'DDRDIAG.BIN')],
                must=[r'FAIL\s+DDRDIAG\'s end: the port went away at \+[\d.]+ s .*USB unplugged, or the board lost power\?',
                      r'\* the port went away'])),
            ('m5', 'rtl', lambda: self.rtl(
                'm5', 'm5', boot, m5_card, within=900, extra=['--btn', '2'],
                args=['--ddrdiag', os.path.join(fw, 'DDRDIAG.BIN')], must=m5_must)),
            ('m8', 'rtl', lambda: self.rtl(
                'm8', 'm8', boot, os_card, within=1500, prestart=False, mount=True,
                must=[r'ok\s+on the Mac \(.*read only\): MAC.TXT has CRC', r'ok\s+a file made on the Mac, read on the board',
                      r'ok\s+create, edit, read', r'ok\s+eject sd0: safe to remove',
                      r'ok\s+after a power cycle the same CRCs', r'ok\s+on the Mac \(read only, .*\) the same CRCs',
                      r'ok\s+after a kernel update the files keep their CRCs', r'ok\s+eject sd0 at the end'])),
            ('m9', 'rtl', lambda: self.rtl(
                'm9', 'm9', boot, os_card, within=1500,
                args=['--boots', '2', '--minutes', str(self.a.soak_minutes)],
                must=[r'ok\s+cold boot 1: SurfBoot, the kernel and the shell', r'ok\s+cold boot 2: SurfBoot',
                      r'ok\s+2 cold boots in a row', r'ok\s+the soak: \d+ rounds'])),
        ]


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == 'owner':
        sys.exit(owner(sys.argv[2], sys.argv[3]))
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfsim')
    ap.add_argument('--fw', help='build/fw: bramdiag.mem, surfboot.mem, DDRDIAG.BIN')
    ap.add_argument('--kernel', help='the Arty SURFOS.BIN')
    ap.add_argument('--mkcard')
    ap.add_argument('--work', default='build/os/kit')
    ap.add_argument('--set', default='check', choices=('check', 'rtl', 'all', 'fake'),
                    help='check: fake boards, m3, m7 and the RTL edge cases; rtl: m5, m8, m9; all; fake: no surfsim')
    ap.add_argument('--only', help='these scenarios only (comma-separated names)')
    ap.add_argument('--jobs', type=int, default=4, help='surfsim runs at once')
    ap.add_argument('--evidence', help='the summary, with each RTL run\'s evidence up to its transcript')
    ap.add_argument('--evidence-full', help='a directory for each RTL run\'s whole evidence file')
    ap.add_argument('--soak-minutes', type=float, default=1.0)
    ap.add_argument('--keep', action='store_true', help='keep the card images')
    a = ap.parse_args()
    for k in ('surfsim', 'fw', 'kernel', 'mkcard'):
        if getattr(a, k):
            setattr(a, k, os.path.abspath(getattr(a, k)))
    t0 = time.time()
    k = Kit(a)
    if os.path.isdir(k.work):
        shutil.rmtree(k.work)
    os.makedirs(k.work)
    jobs = [(n, 'check', f) for n, f in k.fake_jobs()] if a.set in ('check', 'all', 'fake') else []
    if a.set != 'fake':
        missing = [x for x in ('surfsim', 'fw', 'kernel', 'mkcard') if not getattr(a, x)]
        if missing:
            ap.error('the RTL scenarios need --%s' % ', --'.join(missing))
        k.sim_manifest = k.manifest_for_sim(k.work)
        jobs += [(n, s, f) for n, s, f in k.rtl_jobs() if a.set == 'all' or s == a.set]
    if a.only:
        names = a.only.split(',')
        jobs = [j for j in jobs if j[0] in names]
    threads = [threading.Thread(target=f, name=n) for n, _, f in jobs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    passed = sum(1 for ok, _ in k.results if ok)
    head = 'test-os-kit (%s): %d of %d checks passed in %.0f s' % (
        a.only or a.set, passed, len(k.results), time.time() - t0)
    print(head)
    if a.evidence:
        with open(a.evidence, 'w') as f:
            f.write('# %s\n# %s UTC, kit_test.py --set %s%s, Python %s on %s %s (%s)\n' % (
                head, time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime()), a.set, ' --only ' + a.only if a.only else '',
                sys.version.split()[0], os.uname().sysname, os.uname().machine,
                'the RTL system: surfsim --uart-pty; fake boards: pseudo-terminals in kit_test.py; the owner\'s '
                'steps answered by kit_test.py owner'))
            for ok, what in sorted(k.results, key=lambda x: x[1]):
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
            for name, ms, against, v, dt, text in sorted(k.runs):
                if against != 'the RTL system':
                    continue
                f.write('\n==== %s: board-test.py %s against %s, %s in %.0f s: its evidence, up to the transcript ====\n'
                        % (name, ms, against, v, dt))
                f.write(text.split('\nTranscript (')[0].rstrip() + '\n')
    if a.evidence_full:
        os.makedirs(a.evidence_full, exist_ok=True)
        for name, ms, against, v, dt, text in k.runs:
            if against == 'the RTL system':
                open(os.path.join(a.evidence_full, '%s.txt' % name), 'w').write(text)
    sys.exit(0 if passed == len(k.results) and k.results else 1)


if __name__ == '__main__':
    main()
