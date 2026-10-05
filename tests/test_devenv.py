"""Script regression tests; Docker is replaced only at the process boundary.

Run from the repository: python3 -m unittest discover -s tests -p test_devenv.py
The kernel's real integration tests remain make test-all.
"""
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]


class DevenvTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='surfos scripts ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.repo = self.root / '06-restoration'
        self.repo.mkdir()
        shutil.copytree(REPO / 'devenv', self.repo / 'devenv')
        self.out = self.root / 'build' / 'surfos'
        self.out.mkdir(parents=True)
        (self.out / 'previous-build').write_text('keep on failure')
        # Include an old kernel too: its existence must never mask a make failure.
        (self.out / 'kernel').mkdir()
        (self.out / 'kernel' / 'surfos.bin').write_bytes(b'old kernel')
        self.archive = self.root / 'output.tar'
        with tarfile.open(self.archive, 'w') as tar:
            for name in ('build/O0/surfos.bin', 'build/O2/surfos.bin',
                         'build/images/test.img', 'build/images/initrd.tar',
                         'kernel/surfos.bin'):
                data = b'fixture artifact'
                info = tarfile.TarInfo(name)
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        self.fake = self.root / 'docker-double.py'
        self.fake.write_text('''#!/usr/bin/env python3
import json, os, sys
a = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f: f.write(json.dumps(a) + '\\n')
if a[0] == 'run':
    if 'make' in a:
        print('fixture make output')
        sys.exit(int(os.environ.get('MAKE_STATUS', '0')))
    if '-i' in a: sys.stdin.buffer.read()
    elif '-cf' in a:
        sys.stdout.buffer.write(open(os.environ['ARCHIVE'], 'rb').read())
''')
        self.fake.chmod(0o755)
        self.env = dict(os.environ, FAKE_DOCKER=str(self.fake),
                        SURFOS_OUT=str(self.out), SURFOS_LOG=str(self.root / 'build/build.log'),
                        CALLS=str(self.root / 'calls.jsonl'), ARCHIVE=str(self.archive))

    def build(self, *args, status=0):
        self.env['MAKE_STATUS'] = str(status)
        return subprocess.run(['bash', '-c',
            'docker() { "$FAKE_DOCKER" "$@"; }; export -f docker; exec bash "$@"',
            'test', str(self.repo / 'devenv' / 'build.sh'), *args],
            env=self.env, capture_output=True, text=True)

    def test_failed_make_exits_nonzero_and_preserves_last_export(self):
        result = self.build(status=2)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertTrue((self.out / 'previous-build').exists())
        self.assertIn('fixture make output', (self.root / 'build/build.log').read_text())

    def test_default_build_exports_modern_kernel_and_builds_boot_images(self):
        import json
        result = self.build()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('build/O0/surfos.bin', result.stdout)
        calls = [json.loads(line) for line in Path(self.env['CALLS']).read_text().splitlines()]
        make = next(call for call in calls if 'make' in call)
        self.assertIn('images', make)
        self.assertIn('linux/amd64', make)

    def test_optimized_build_reports_matching_kernel(self):
        result = self.build('O=2')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('build/O2/surfos.bin', result.stdout)

    def test_clean_succeeds_without_a_kernel(self):
        with tarfile.open(self.archive, 'w'):
            pass
        result = self.build('clean')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_run_loads_initrd_when_project_path_contains_spaces(self):
        for name in ('build/O0/surfos.bin', 'build/images/test.img', 'build/images/initrd.tar'):
            path = self.out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'boot artifact')
        fake_qemu = self.root / 'qemu-system-i386'
        # QEMU treats -initrd as a whitespace-separated list, even if shell-quoted.
        fake_qemu.write_text('''#!/usr/bin/env python3
import sys
path = sys.argv[sys.argv.index('-initrd') + 1].split()[0]
assert open(path, 'rb').read() == b'boot artifact'
''')
        fake_qemu.chmod(0o755)
        env = dict(self.env, PATH=str(self.root) + os.pathsep + os.environ['PATH'])
        result = subprocess.run(['bash', str(self.repo / 'devenv/run.sh'), '-display', 'none'],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
