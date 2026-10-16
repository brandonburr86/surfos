"""SurfOS: build-time vendor patch tests
----------------------
File: test_webpatch.py  Date: 10/12/26
----------------------
The patch is exact, checked and never changes its input.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('webpatch', ROOT / 'tools/patch-webdeps.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.source = b'old\n'
        self.spec = {'source_sha256': hashlib.sha256(self.source).hexdigest(),
                     'patched_sha256': hashlib.sha256(b'new\n').hexdigest(),
                     'replacements': [{'old': 'old\n', 'new': 'new\n'}]}

    def test_exact_patch(self):
        self.assertEqual(patch.patched_source(self.source, self.spec), b'new\n')

    def test_wrong_input_and_result_rejected(self):
        with self.assertRaisesRegex(ValueError, 'input SHA-256'):
            patch.patched_source(b'changed\n', self.spec)
        self.spec['patched_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'output SHA-256'):
            patch.patched_source(self.source, self.spec)

    def test_missing_context_rejected(self):
        self.spec['replacements'][0]['old'] = 'missing'
        with self.assertRaisesRegex(ValueError, 'exactly once'):
            patch.patched_source(self.source, self.spec)

    def test_atomic_output_input_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'vendor' / 'source.c'
            source.parent.mkdir()
            source.write_bytes(self.source)
            description = root / 'patch.json'
            description.write_text(json.dumps(self.spec))
            output = root / 'build' / 'source.c'
            patch.generate(source, description, output)
            self.assertEqual(output.read_bytes(), b'new\n')
            self.assertEqual(source.read_bytes(), self.source)
            with self.assertRaisesRegex(ValueError, 'outside'):
                patch.generate(source, description, source)
            source.write_bytes(b'wrong')
            with self.assertRaises(ValueError):
                patch.generate(source, description, output)
            self.assertEqual(output.read_bytes(), b'new\n')

    def test_real_patch(self):
        source = ROOT / 'third_party/mbedtls/library/ssl_tls13_keys.c'
        if not source.exists():
            self.skipTest('fetch-webdeps.py has not extracted Mbed TLS')
        description = json.loads((ROOT / 'third_party/patches/mbedtls-3.6.7-tls13-cleanup.json').read_text())
        before = source.read_bytes()
        after = patch.patched_source(before, description)
        for kind in ('earlydata', 'handshake', 'application'):
            added = f'        mbedtls_ssl_transform_free(transform_{kind});\n'.encode()
            self.assertEqual(after.count(added) - before.count(added), 1)
        self.assertEqual(len(after.splitlines()), len(before.splitlines()) + 4)


if __name__ == '__main__':
    unittest.main()
