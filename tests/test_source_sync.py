"""A source edited during a snapshot build must invalidate the old object."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which('rsync'), 'rsync is needed')
class SourceSyncTests(unittest.TestCase):
    def test_same_size_edit_older_than_previous_build_is_recompiled(self):
        with tempfile.TemporaryDirectory() as temp:
            src, dst = Path(temp)/'source', Path(temp)/'volume'
            src.mkdir(); dst.mkdir()
            (src/'demo.c').write_text('new source')
            (dst/'demo.c').write_text('old source')
            stamp = time.time()-100
            os.utime(src/'demo.c',(stamp,stamp))
            os.utime(dst/'demo.c',(stamp,stamp))
            (dst/'build').mkdir()
            obj=dst/'build'/'demo.o'; obj.write_text('old object')
            os.utime(obj,(stamp+50,stamp+50))
            (src/'steady.c').write_text('unchanged')
            (dst/'steady.c').write_text('unchanged')
            os.utime(dst/'steady.c',(stamp,stamp))
            (dst/'deleted.c').write_text('removed')
            cmd=['sh',str(ROOT/'devenv'/'sync-source.sh'),str(src),str(dst)]
            subprocess.run(cmd,check=True)
            self.assertEqual((dst/'demo.c').read_text(),'new source')
            self.assertGreater((dst/'demo.c').stat().st_mtime,obj.stat().st_mtime)
            self.assertEqual((dst/'steady.c').stat().st_mtime,stamp)
            self.assertFalse((dst/'deleted.c').exists())
            self.assertTrue(obj.exists())
            copied=(dst/'demo.c').stat().st_mtime
            subprocess.run(cmd,check=True)
            self.assertEqual((dst/'demo.c').stat().st_mtime,copied)

if __name__ == '__main__': unittest.main()
