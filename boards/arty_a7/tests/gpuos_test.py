#!/usr/bin/env python3
"""
SurfOS's GPU driver on the RTL system: gfx in surfsim with SurfGPU
File: gpuos_test.py Date: 10/25/26 (SurfGPU G3)

make -C surfcpu test-gpuos (mk/area-gpuos.mk). build/sim/surfsim_gpu is the RTL of surfcpu_soc
with HAS_GPU = 1 and HAS_DDR = 1 (the real core, surf_vram, surf_gpu, surf_video with the
graphics modes) and the C++ models at its pins; SurfBoot (build/fw/surfboot.mem) loads the Arty
kernel package (SURFOS.BIN) from a tools/mkcard.py card, and the commands of STEPS below are
typed on the UART. Checked:

  * the driver found the GPU at boot (+GPU: line), drivers lists it, gfx describes it;
  * every `gfx crc` printed a FRAME_CRC that is the CRC of a frame captured at the VGA pins
    (surfsim --vga-crc), and, where the display shows graphics alone (mode 2), the CRC the C
    model computes for the same commands: boards/arty_a7/tests/gpu/gfxscene replays them in
    sim/models/gpu_model.c through the same gfx.c and gpu.c;
  * gfx get reads back what gfx put wrote; gfx demo 1 runs and ends, gfx demo 5 ends on the Ctrl+C
    typed after its first second, and after each the picture's CRC is the CRC before them (the
    display, the palette and VRAM back as they were); gfx bench reports its four rates; gfx reset;
  * gfx mode overlay: the last frame at the pins shows the console's text over the picture: the
    text decoded from the pixels (surfsim --vga-dir) holds the last command and its output, every
    pixel where no character is is the picture's (gfxscene's frame of it), and the rest are text;
  * nothing said a stopped engine, a GPU error, a missed blanking, a panic.

And on build/sim/surfsim (HAS_GPU = 0), side by side: the kernel boots, says +GPU: none at boot,
gfx says there is no GPU, drivers says the gpu is not present.

  gpuos_test.py --surfsim-gpu build/sim/surfsim_gpu --surfsim build/sim/surfsim --rom build/fw/surfboot.mem
                --mkcard tools/mkcard.py --package ../build/riscv32/arty_a7/SURFOS.BIN
                --gfxscene build/gpuos/gfxscene [--work DIR] [--evidence FILE] [--keep] [--seed N]

The card image is deleted at the end unless --keep. Exit status 0 when every check passed.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ANSI = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')
PROMPT = r'SURF ~(?:\x1b\[[0-9;]*m)*#(?:\x1b\[[0-9;]*m)*>(?:\x1b\[[0-9;]*m)* '
NL = r'\r?\n'
FAIL_ON = ['Kernel panic', 'Kernel Wipeout', 'BOOT PROBLEM', 'killed by', 'SurfBoot stopped', 'a stopped engine',
           'the GPU reports', 'no vertical blanking in', 'Invalid command']
CGA = {0x000, 0x00A, 0x0A0, 0x0AA, 0xA00, 0xA0A, 0xA50, 0xAAA, 0x555, 0x55F, 0x5F5, 0x5FF, 0xF55, 0xF5F, 0xFF5, 0xFFF}
CRC_LINE = re.compile(r'gfx crc: frame (\d+), crc ([0-9a-f]{8})')

# (what is typed, what its output must hold before the next prompt (a regex, or None), why[, a regex after
# which Ctrl+C is typed])
STEPS = [
    ('gfx', r'SurfGPU GPU1 \(gpu\.ID 0x31555047\), VRAM 300 KiB at 0x30000000' + NL + r'[\s\S]*'
            r'frames: FRAME_COUNT \d+', 'the state'),
    ('drivers', r'gpu\s+ok[\s\S]*gpu       SurfGPU GPU1, 300 KiB VRAM; \d+ fences, 0 errors',
     'the driver table'),
    ('gfx mode gfx 1', r'display: mode 2, graphics, 640x480' + NL + r'\s+the target is that picture: 640x480 at VRAM 0, '
     r'stride 640', 'graphics at 640x480'),
    ('gfx pal rgb332', r'palette: rgb332', 'the palette the configuration leaves (a warm reset keeps any other)'),
    ('gfx clear 0x25', None, ''),
    ('gfx fill 16 16 200 120 0xe0', None, ''),
    ('gfx rect 8 8 216 136 0xff', None, ''),
    ('gfx line 0 479 639 0 0x1c', None, ''),
    ('gfx line 0 0 639 479 0xfc', None, ''),
    ('gfx copy 16 16 400 300 200 120', None, ''),
    ('gfx copy 0 0 240 160 64 64 0x25', None, ''),
    ('gfx text 24 200 0xff - Hello from SurfOS on SurfGPU', None, ''),
    ('gfx text 24 220 0x03 0xfc opaque text in the console font', None, ''),
    ('gfx put 639 479 0x92', None, ''),
    ('gfx get 639 479', r'\(639, 479\) = 146 \(0x92\), VRAM byte 307199 \(0x3004afff\)', 'a pixel back'),
    ('gfx crc', CRC_LINE.pattern, 'crc 1'),
    ('gfx mode gfx 2', r'display: mode 2, graphics, 320x240 doubled' + NL + r'\s+the target is that picture: 320x240 '
     r'at VRAM 0, stride 320', 'graphics at 320x240'),
    ('gfx clear 0x49', None, ''),
    ('gfx fill 296 216 40 40 0xe3', None, ''),
    ('gfx line 0 239 319 0 0xff', None, ''),
    ('gfx text 8 8 0xff 0 320x240, doubled', None, ''),
    ('gfx pal ramps', r'palette: ramps \(8 hues of 32 shades\)', 'the ramps'),
    ('gfx crc', CRC_LINE.pattern, 'crc 2'),
    ('gfx pal rgb332', r'palette: rgb332', 'RGB332 again'),
    ('gfx crc', CRC_LINE.pattern, 'crc 3'),
    ('gfx demo 1', r'gfx demo: \d+ frames in \d+\.\d\d s, \d+\.\d a second; display back as it was', 'the demo'),
    ('gfx crc', CRC_LINE.pattern, 'crc 4'),
    ('gfx demo 5', r'gfx demo: \d+ frames in \d+\.\d\d s, \d+\.\d a second; display back as it was' + NL +
     r'\s+gfx demo: ended by Ctrl\+C', 'the demo ended by Ctrl+C', r'\n    1 s: \d+ frames'),
    ('gfx crc', CRC_LINE.pattern, 'crc 5'),
    ('gfx bench', r'text  64 x 32 cells: 262144 pixels in \d+\.\d+ ms, \d+\.\d million/s', 'the bench'),
    ('gfx reset', r'gfx reset: ABORT, errors cleared; target 640x480 at VRAM 0, stride 640', 'reset'),
    ('gfx mode text', r'display: mode 1, text', 'text'),
    ('gfx crc', CRC_LINE.pattern, 'crc 6'),
    ('gfx mode overlay 1', r'display: mode 3, graphics with the text over it, 640x480', 'the overlay'),
    ('gfx clear 0x49', None, ''),
    ('gfx fill 320 240 320 240 0x92', None, ''),
    ('gfx crc', CRC_LINE.pattern, 'crc 7'),
    ('echo the shell over the picture', r'the shell over the picture' + NL, 'a command over the picture'),
]
NOGPU_STEPS = [
    ('gfx', r'gfx: no GPU: sysinfo\.FEATURES has no GPU \(a bitstream without one\)', 'gfx'),
    ('drivers', r'gpu\s+not present[\s\S]*gpu       none: sysinfo\.FEATURES has no GPU', 'drivers'),
]


def rx(text):
    """text as an ECMAScript regular expression: only its special characters escaped"""
    return re.sub(r'([\\^$.|?*+()\[\]{}])', r'\\\1', text)


def read_ppm(path):
    """a P6 file's pixels as RGB444 values, row by row"""
    data = open(path, 'rb').read()
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', data)
    w, h = int(m.group(1)), int(m.group(2))
    p = data[m.end():]
    return w, h, [((p[3 * i] // 17) << 8) | ((p[3 * i + 1] // 17) << 4) | (p[3 * i + 2] // 17) for i in range(w * h)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--surfsim-gpu', required=True)
    ap.add_argument('--surfsim', required=True)
    ap.add_argument('--rom', required=True)
    ap.add_argument('--mkcard', required=True)
    ap.add_argument('--package', required=True)
    ap.add_argument('--gfxscene', required=True)
    ap.add_argument('--work', default='build/gpuos/system')
    ap.add_argument('--evidence')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--max-cycles', type=int, default=900000000)
    a = ap.parse_args()

    t0 = time.time()
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        print('%s %s' % ('ok  ' if ok else 'FAIL', what), flush=True)
        return bool(ok)

    w = a.work
    os.makedirs(w, exist_ok=True)
    for old in os.listdir(w):
        p = os.path.join(w, old)
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)

    # ---------------------------------------------------------------- the model's pictures
    script = os.path.join(w, 'gfx-script.txt')
    with open(script, 'w') as f:
        f.write('# the gfx commands typed in the RTL run, replayed in the C model (gfxscene)\n')
        for step in STEPS:
            if step[0].startswith('gfx'):
                f.write(step[0] + '\n')
    os.makedirs(os.path.join(w, 'model'))
    r = subprocess.run([a.gfxscene, '--ppm', os.path.join(w, 'model'), script], capture_output=True, text=True)
    with open(os.path.join(w, 'gfxscene.txt'), 'w') as f:
        f.write(r.stdout + r.stderr)
    expect = {int(n): (crc, int(mode)) for n, crc, mode in re.findall(r'^expect (\d+) ([0-9a-f]{8}) mode (\d+)$',
                                                                       r.stdout, re.M)}
    ncrc = sum(1 for step in STEPS if step[0] == 'gfx crc')
    if not check(r.returncode == 0 and len(expect) == ncrc,
                 'gfxscene: the %d gfx commands replayed in the C model (sim/models/gpu_model.c), %d FRAME_CRCs'
                 % (sum(1 for step in STEPS if step[0].startswith('gfx')), len(expect))):
        print(r.stdout[-3000:] + r.stderr[-2000:])
        sys.exit(1)

    # ---------------------------------------------------------------- the card
    with open(os.path.join(w, 'CMDLINE.TXT'), 'wb') as f:
        f.write(b'ascii\n')
    card = os.path.join(w, 'card.img')
    r = subprocess.run([sys.executable, a.mkcard, card, '--size', '64M', '--standard-dirs',
                        '--file', '/BOOT/SURFOS.BIN=' + a.package,
                        '--file', '/BOOT/CMDLINE.TXT=' + os.path.join(w, 'CMDLINE.TXT')], capture_output=True, text=True)
    if not check(r.returncode == 0, 'card: mkcard.py makes it (/BOOT/SURFOS.BIN, /BOOT/CMDLINE.TXT "ascii")'):
        print(r.stdout + r.stderr)
        sys.exit(1)

    # ---------------------------------------------------------------- the runs, side by side
    boot = [('the GPU at boot', r'\+GPU: SurfGPU GPU1 \(gpu\.ID 0x31555047\), VRAM 300 KiB at 0x30000000' + NL +
                                r'\+GPU: a 64-word command FIFO; fences wait on intc source 5 \(IE\.FENCE\)'),
            ('the prompt', PROMPT)]
    nogpu_boot = [('no GPU at boot', r'\+GPU: none: sysinfo\.FEATURES has no GPU \(a bitstream without one\)' + NL),
                  ('the prompt', PROMPT)]

    def plan(steps, boot_expects):
        e, sends = list(boot_expects), []
        for step in steps:
            cmd, want, why = step[:3]
            sends.append((PROMPT, cmd + '\\r'))
            if len(step) > 3:
                sends.append((step[3], '\\x03'))      # Ctrl+C, once that has been printed
            e.append((why or cmd, rx(cmd) + NL))
            if want:
                e.append((why or cmd, want))
            e.append(('prompt', PROMPT))
        return e, sends

    runs = {}
    e, s = plan(STEPS, boot)
    os.makedirs(os.path.join(w, 'vga'))
    runs['gpu'] = dict(sim=a.surfsim_gpu, expects=e, sends=s,
                       extra=['--vga-crc', os.path.join(w, 'frames.txt'), '--vga-dir', os.path.join(w, 'vga'),
                              '--settle', '4000000'])
    e, s = plan(NOGPU_STEPS, nogpu_boot)
    runs['nogpu'] = dict(sim=a.surfsim, expects=e, sends=s, extra=[])
    procs = {}
    for name, run in runs.items():
        argv = [run['sim'], '--rom', a.rom, '--sd', card, '--seed', str(a.seed), '--max-cycles', str(a.max_cycles),
                '--quiet', '--transcript', os.path.join(w, name + '.raw')]
        for f_ in FAIL_ON:
            argv += ['--fail-on', f_]
        for _, pattern in run['expects']:
            argv += ['--expect', pattern]
        for pattern, text in run['sends']:
            argv += ['--send', '%s=%s' % (pattern, text)]
        argv += run['extra']
        with open(os.path.join(w, name + '.cmd'), 'w') as f:
            f.write(' '.join("'%s'" % x if re.search(r'[^\w./=+:-]', x) else x for x in argv) + '\n')
        err = open(os.path.join(w, name + '.err'), 'wb')
        procs[name] = (subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=err), err)
    for name, (p, err) in procs.items():
        run = runs[name]
        run['status'] = p.wait()
        err.close()
        raw = os.path.join(w, name + '.raw')
        run['out'] = ANSI.sub('', open(raw, 'rb').read().decode('utf-8', 'replace')).replace('\r', '') \
            if os.path.exists(raw) else ''
        with open(os.path.join(w, name + '-transcript.txt'), 'w') as f:
            f.write(run['out'])
        run['err'] = open(os.path.join(w, name + '.err'), errors='replace').read()
        run['sum'] = re.search(r'surfsim: (.*) after (\d+) cycles \(([\d.]+) s simulated\) in ([\d.]+) s wall '
                               r'\(([\d.]+) M cycles/s\)', run['err'])
        run['events'] = {int(n): int(c) for c, n in re.findall(r'surfsim: cycle (\d+): --expect (\d+) of', run['err'])}

    def run_ok(name, what):
        run = runs[name]
        last = [l for l in run['err'].splitlines() if l.startswith('surfsim: ') and ' after ' in l]
        ok = check(run['status'] == 0 and run['sum'] and run['sum'].group(1) == 'output matched',
                   '%s: every expected line in order (%s)' % (what, last[-1][9:] if last else 'no summary'))
        if not ok:
            got = max(run['events']) if run['events'] else 0
            nxt = run['expects'][got][0] if got < len(run['expects']) else '?'
            print('  the run stopped before: %s (expect %d of %d)' % (nxt, got + 1, len(run['expects'])))
            print(run['err'][-2500:])
        return run['out']

    # ---------------------------------------------------------------- the GPU run
    out = run_ok('gpu', 'SurfOS on the SoC with SurfGPU (surfsim_gpu)')
    check(re.search(r'\+GPU: SurfGPU GPU1 \(gpu\.ID 0x31555047\)', out) and re.search(r'Drivers: serial[^\n]* gpu\n', out),
          'boot: the driver finds SurfGPU GPU1 (sysinfo.FEATURES.GPU, gpu.ID "GPU1") and starts')
    check('palette: rgb332 (as the configuration leaves it)' in out and 'fences; FENCE ' in out and
          re.search(r'interrupts: \d+ \(intc source 5, IE\.FENCE\)', out),
          'gfx: ID, VRAM, the display and its palette (RGB332 as configured), FIFO, fences on the interrupt, errors, frames')
    check(not any(f_ in out for f_ in FAIL_ON) and 'usage:' not in out,
          'no stopped engine, no GPU error, no blanking missed, no usage error, no panic')
    got = [crc for _, crc in CRC_LINE.findall(out)]
    frames = {}
    fp = os.path.join(w, 'frames.txt')
    if os.path.exists(fp):
        for n, crc in re.findall(r'^frame (\d+) crc ([0-9a-f]{8})$', open(fp).read(), re.M):
            frames.setdefault(crc, int(n))
    check(len(got) == ncrc, 'gfx crc printed %d FRAME_CRCs (%d typed)' % (len(got), ncrc))
    for i, crc in enumerate(got, 1):
        model, mode = expect.get(i, ('?', -1))
        pins = crc in frames
        if mode == 2:
            check(crc == model and pins, 'crc %d (graphics, mode 2): FRAME_CRC %s; the C model %s; a frame at the VGA pins %s'
                  % (i, crc, model, 'has it (frame %d)' % frames[crc] if pins else 'does not'))
        else:
            check(pins, 'crc %d (mode %d, the console\'s text in the picture): FRAME_CRC %s; a frame at the VGA pins %s'
                  % (i, mode, crc, 'has it (frame %d)' % frames[crc] if pins else 'does not'))
    if len(got) >= 5:
        check(got[3] == got[2] and got[4] == got[2], 'after gfx demo 1, and after gfx demo 5 ended by Ctrl+C, the picture '
              'is the one before them (crc 4 = crc 5 = crc 3: the display, the palette and VRAM back)')
    check(re.search(r'gfx demo: \d+ frames in [12]\.\d\d s[^\n]*\n\s+gfx demo: ended by Ctrl\+C', out),
          'gfx demo 5: Ctrl+C typed after its first second ends it within the next')
    m = re.search(r'gfx demo: (\d+) frames in (\d+\.\d\d) s, (\d+\.\d) a second', out)
    check(m and int(m.group(1)) >= 20, 'gfx demo 1: %s frames in %s s of simulated time (%s a second), two pages flipped'
          % (m.group(1), m.group(2), m.group(3)) if m else 'gfx demo 1: no summary line')
    rates = re.findall(r'(fill|copy|line|text) +[^\n]*: (\d+) pixels in (\d+\.\d+) ms, (\d+\.\d) million/s', out)
    check(len(rates) == 4, 'gfx bench: %s' % ', '.join('%s %s Mpx/s' % (r_[0], r_[3]) for r_ in rates))
    # the last frame at the pins: the console's text over the picture
    sp, pp = os.path.join(w, 'vga', 'screen.txt'), os.path.join(w, 'vga', 'screen.ppm')
    model_pic = os.path.join(w, 'model', 'crc-%d.ppm' % ncrc)
    rows, head = [], ''
    if os.path.exists(sp) and os.path.exists(pp) and os.path.exists(model_pic):
        head, _, body = open(sp, errors='replace').read().partition('\n')
        rows = [x.ljust(80)[:80] for x in body.split('\n')[:30]]
        _, _, shot = read_ppm(pp)
        _, _, pic = read_ppm(model_pic)
        mc = re.search(r'cursor at column (-?\d+) row (-?\d+)', head)
        cur = (int(mc.group(1)), int(mc.group(2))) if mc else (-1, -1)
        text_px = bad_space = bad_text = 0
        for y in range(480):
            for x in range(640):
                i = y * 640 + x
                ch = rows[y // 16][x // 8] if y // 16 < len(rows) else ' '
                blank = ch == ' ' and (x // 8, y // 16) != cur
                if shot[i] != pic[i]:
                    text_px += 1
                    if blank:
                        bad_space += 1
                    elif shot[i] not in CGA:
                        bad_text += 1
        check(any(re.match(r'SURF ~#> echo the shell over the picture\s*$', x) for x in rows) and
              any(re.match(r'the shell over the picture\s*$', x) for x in rows) and
              any(re.match(r'SURF ~#> gfx crc\s*$', x) for x in rows),
              'overlay: the console\'s text decoded from the pixels over the picture: the commands typed and their output')
        check(', 0 unmatched cells' in head, 'overlay: every 8x16 cell reads as a glyph over the picture (%s)'
              % head.split(', ')[-1] if head else 'overlay: no screen')
        check(text_px > 2000 and bad_space == 0 and bad_text == 0,
              'overlay: where no character is, the pixels are the picture\'s (the C model\'s frame of it); %d pixels '
              'differ, all text colours inside characters (%d in blank cells, %d not a text colour)'
              % (text_px, bad_space, bad_text))
    else:
        check(False, 'overlay: the last frame (vga/screen.ppm, screen.txt) and the model\'s picture are there')

    # ---------------------------------------------------------------- the run without a GPU
    out2 = run_ok('nogpu', 'SurfOS on the SoC without SurfGPU (surfsim, HAS_GPU = 0)')
    check('+GPU: none: sysinfo.FEATURES has no GPU' in out2 and 'gfx: no GPU: sysinfo.FEATURES has no GPU' in out2 and
          re.search(r'gpu\s+not present', out2) and not any(f_ in out2 for f_ in FAIL_ON),
          'without a GPU: said once at boot (+GPU: none), by gfx, and by drivers (not present); nothing else changes')

    # ---------------------------------------------------------------- the record
    passed = sum(1 for ok, _ in results if ok)
    parts = ['%s %s cycles = %s s simulated in %s s wall (%s M cycles/s)' %
             (n, r_['sum'].group(2), r_['sum'].group(3), r_['sum'].group(4), r_['sum'].group(5))
             for n, r_ in runs.items() if r_['sum']]
    line = ('test-gpuos: %d of %d checks passed in %.0f s (%s; the RTL of surfcpu_soc with SurfGPU and the C++ models, '
            'not the board)' % (passed, len(results), time.time() - t0, '; '.join(parts)))
    print(line)
    if a.evidence:
        os.makedirs(os.path.dirname(a.evidence) or '.', exist_ok=True)
        with open(a.evidence, 'w') as f:
            f.write('# %s\n# %s\n' % (line, time.strftime('%Y-%m-%d %H:%M:%S UTC', time.gmtime())))
            for ok, what in results:
                f.write('%s %s\n' % ('ok  ' if ok else 'FAIL', what))
            f.write('\nthe gfx commands typed, and what SurfOS printed for each gfx crc:\n')
            for step in STEPS:
                f.write('  %s%s\n' % (step[0], ' (Ctrl+C after its first second)' if len(step) > 3 else ''))
            for i, crc in enumerate(got, 1):
                model, mode = expect.get(i, ('?', -1))
                f.write('  crc %d: SurfOS %s, mode %d%s\n' % (i, crc, mode, ', the C model %s' % model if mode == 2 else ''))
            if rows:
                f.write('\nthe last frame at the pins, its text decoded from the pixels (over the picture):\n%s\n' % head)
                f.write(''.join('  |%s|\n' % x for x in rows))
    if not a.keep and os.path.exists(card):
        os.remove(card)
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
