#!/usr/bin/env python3
"""Exercise the real files application on VGA and a resized Telnet terminal.

python3 tools/qemu-files.py --kernel build/O0/surfos.bin \
    --initrd build/images/initrd.tar --disk build/images/test.img
All fixtures and disk writes belong to temporary emulator copies.
"""
import argparse
import importlib.util
import io
from pathlib import Path
import re
import tarfile
import tempfile

spec = importlib.util.spec_from_file_location('qemu_run', Path(__file__).with_name('qemu-run.py'))
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)


def fixture(source, target):
    with tarfile.open(source) as old, tarfile.open(target, 'w', format=tarfile.USTAR_FORMAT) as new:
        for item in old:
            new.addfile(item, old.extractfile(item) if item.isfile() else None)
        for name in ('files-test', 'files-test/alpha', 'files-test/alpha/nested', 'files-test/empty'):
            item = tarfile.TarInfo(name)
            item.type, item.mode = tarfile.DIRTYPE, 0o755
            new.addfile(item)
        files = {'.hidden': b'hidden\n', 'a.txt': b'read me\n', 'binary': b'\x7fELF\x00data',
                 'a very long file name with spaces.txt': b'spaces\n',
                 'big.bin': b'x' * (1024 * 1024 + 1),
                 'unsafe\x1b[2J.txt': b'controls\n'}
        files.update({f'item{i:02}.txt': b'item\n' for i in range(24)})
        for name, data in files.items():
            item = tarfile.TarInfo('files-test/' + name)
            item.size, item.mode, item.mtime = len(data), 0o644, 1735689600
            new.addfile(item, io.BytesIO(data))


def run(a, initrd):
    q = harness.Qemu(a.kernel, qemu=a.qemu, initrd=initrd, disk=a.disk, net=True)
    count = 0

    def check(ok, label):
        nonlocal count
        if not ok:
            raise AssertionError(label + '\n' + q.screen() + '\n' + q.text()[-2500:])
        count += 1
        print('PASS ' + label, flush=True)

    def command(line):
        q.wait(.25)
        q.cursor = len(q.transcript)
        q.send(line + '\n')
        check(q.expect(harness.PROMPT, 15), 'shell: ' + line)

    def keys(data):
        q.ser.sendall(data)
        q.wait(.25)
        return q.screen()

    def click(row, col=42):
        return keys(f'\x1b[<0;{col};{row}M\x1b[<0;{col};{row}m'.encode())

    def file_row(name):
        screen = q.screen()
        # Only list rows, excluding the title/path/footer.
        return next(i + 1 for i, line in enumerate(screen.splitlines()) if 3 <= i < 21 and name in line)

    def launch(path):
        q.wait(.25)
        q.cursor = len(q.transcript)
        q.send('files ' + path + '\n')
        check(q.expect('SurfOS Files', 8), 'files launches: ' + path)
        q.wait(.25)
        return q.screen()

    try:
        check(q.expect(harness.PROMPT, 25), 'boot')
        screen = launch('/initrd/files-test')
        screen = click(25, 75)
        check('SurfOS Files' in screen, 'blank footer click does nothing')
        check('Folders' in screen and 'Modified' in screen and 'Bytes' in screen, 'hierarchy and metadata columns')
        check(file_row('alpha/') < file_row('.hidden'), 'directories precede files')
        check('2025-01-01' in screen, 'file modification dates')
        screen = keys(b'\r')
        check('/initrd/files-test/alpha' in screen and 'nested/' in screen, 'Enter descends into selected folder')
        screen = keys(b'\x1b[D')
        check('Selected: alpha/' in screen, 'parent navigation restores selection')
        screen = keys(b'\x1b[B\r')
        check('(empty folder)' in screen, 'empty folder is navigable')
        screen = keys(b'\x1b[A\x1b[B\r')
        check('(empty folder)' in screen, 'empty folder ignores selection and open')
        keys(b'\x7f')
        screen = keys(b'.')
        check('.hidden' not in screen, 'hide dotfiles')
        screen = keys(b'.')
        check('.hidden' in screen, 'show dotfiles')
        row = file_row('a.txt')
        screen = click(row)
        check('Selected: a.txt' in screen and '8 bytes' in screen, 'click selects file with exact size')
        screen = click(row)
        check('^S Save' in screen and 'read me' in screen, 'second click opens existing editor')
        screen = keys(b'\x11')
        check('SurfOS Files' in screen and 'Selected: a.txt' in screen, 'editor returns to selection')
        row = file_row('a very long')
        click(row)
        screen = keys(b'e')
        check('^S Save' in screen and 'spaces' in screen, 'clipped name with spaces opens the exact file')
        keys(b'\x11')
        row = file_row('big.bin')
        click(row)
        screen = keys(b'e')
        check('1 MiB limit' in screen, 'oversized files are refused')
        row = file_row('binary')
        click(row)
        screen = keys(b'e')
        check('Binary file' in screen and 'SurfOS Files' in screen, 'binary file is not sent to editor')
        screen = click(row)
        check('Binary file' in screen, 'mouse release preserves the action error')
        screen = keys(b'\x1b[F')
        check('unsafe?[2J.txt' in screen, 'untrusted filename cannot clear the terminal')
        screen = keys(b'\x1b[5~')
        check('item' in screen, 'page navigation scrolls long listings')
        screen = keys(b'\x1b[H')
        check('Selected: alpha/' in screen, 'Home selects first entry')
        keys(b'\x1b[<0;')
        screen = keys(b'\x1b[H')
        check('SurfOS Files' in screen, 'truncated mouse sequence times out safely')
        # Fragment a valid sequence, but within the input timeout.
        q.ser.sendall(b'\x1b[')
        q.wait(.03)
        screen = keys(b'B')
        check('Selected: empty/' in screen, 'fragmented arrow is parsed')
        screen = click(4, 2)
        check('Path: /' in screen and 'initrd/' in screen, 'ancestor root click navigates to root')
        screen = keys(b'\x1b[D\x7f')
        check('Path: /' in screen, 'root parent navigation stays at root')
        keys(b'q')
        command('mkdir /hda1/FILETEST')
        command('echo original > /hda1/FILETEST/note.txt')
        launch('/hda1/FILETEST')
        screen = keys(b'\r')
        check('^S Save' in screen, 'Enter opens editable file')
        keys(b'\x1b[F-added\x13')
        screen = keys(b'\x11')
        check('SurfOS Files' in screen and '15 bytes' in screen, 'save and return refreshes byte size')
        keys(b'q')
        command('cat /hda1/FILETEST/note.txt')
        check('original-added' in q.text(), 'editor changes persisted to disk')
        launch('/dev')
        keys(b'\x1b[F')                 # 10/25/26 (track A, A10): the last entry, a device (/dev/config, a folder, is first)
        screen = keys(b'e')
        check('device' in screen.lower() and 'SurfOS Files' in screen, 'device cannot be opened in editor')
        keys(b'q')
        command('files /missing-files-directory')
        command('echo FILES-SHELL-OK')
        check('FILES-SHELL-OK' in q.text(), 'terminal restored after exit/error')
        command('dhcp')
        command('telnetd on')
        t = harness.Telnet(q.ports['telnet'], cols=40, rows=12)
        try:
            check(t.expect(harness.TELNET_ROOT, 15), 'small terminal connects')
            t.send('files /initrd/files-test\n')
            check(t.expect('SurfOS Files', 10), 'small terminal launches')
            check(t.expect('Open/Edit', 10), 'small terminal has usable actions')
            t.send('\x1b[<0;8;12M\x1b[<0;8;12m')
            check(t.expect('/initrd/files-test/alpha', 10), 'small terminal Open/Edit click descends')
            t.send('q')
            check(t.expect(harness.TELNET_ROOT, 10), 'small terminal returns to shell')
            t.send('files /hda1/FILETEST\n')
            check(t.expect('SurfOS Files', 10) and t.expect('Open/Edit', 10), 'remote file browser launches')
            t.send('e')
            check(t.expect('^S Save', 10), 'remote editor starts')
            command('mv /hda1/FILETEST /hda1/MOVED')
            t.send('\x11')
            check(t.expect('Cannot open folder:', 10), 'failed refresh after edit is reported')
            t.send('q')
            check(t.expect(harness.TELNET_ROOT, 10), 'failed refresh still permits clean exit')
            t.send('exit\n')
        finally:
            t.close()
        check(not any(bad in q.text() for bad in harness.BAD), 'no kernel fault')
        print(f'FILES TEST PASSED: {count} checks', flush=True)
    finally:
        if a.screenshot:
            # The final screen and full transcript are debugging evidence.
            Path(a.screenshot).write_text(q.screen())
            Path(a.screenshot + '.log').write_text(q.text())
        q.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', required=True)
    p.add_argument('--initrd', required=True)
    p.add_argument('--disk', required=True)
    p.add_argument('--qemu', default='qemu-system-i386')
    p.add_argument('--screenshot')
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='files-test-') as tmp:
        initrd = str(Path(tmp) / 'initrd.tar')
        fixture(a.initrd, initrd)
        run(a, initrd)


if __name__ == '__main__':
    main()
