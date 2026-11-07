"""An upgrade changes sources while preserving active incremental metadata."""

from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sync_prepared_tree
from krypton import layout


class PreparedSyncTests(unittest.TestCase):
    def setUp(self):
        parent = layout.BUILD_DIR / 'tmp'
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.prepared = self.root / 'prepared'
        self.active = self.root / 'active'
        for tree in (self.prepared, self.active):
            tree.mkdir()
        for name in ('.ninja_log', '.ninja_deps', 'keep.obj'):
            self.write(self.active, 'out/Default/' + name, b'original ' + name.encode())

    def write(self, tree, name, data):
        path = tree / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def test_sync_preserves_outputs_tracking_and_identical_timestamps(self):
        same = self.write(self.active, 'same.cc', b'unchanged')
        os.utime(same, ns=(1600000000000000000, 1600000000000000000))
        previous_time = same.stat().st_mtime_ns
        self.write(self.prepared, 'same.cc', b'unchanged')
        self.write(self.active, 'changed.cc', b'old')
        changed = self.write(self.prepared, 'changed.cc', b'new')
        # A newly installed source may have an archive timestamp older than
        # its previous generated output. That output must become dirty.
        os.utime(changed, ns=(1500000000000000000, 1500000000000000000))
        generated = self.write(self.active, 'out/Default/generated.h', b'old generated data')
        os.utime(generated, ns=(1600000000000000000, 1600000000000000000))
        self.write(self.active, 'old.cc', b'obsolete source')
        self.write(self.active, 'local.txt', b'keep local data')
        self.write(self.prepared, 'out/Default/.ninja_log', b'stage graph must not replace this')
        self.write(self.prepared, 'third_party/package/out/runtime.js', b'package input')
        outputs = {p.name: p.read_bytes() for p in (self.active / 'out/Default').iterdir()}
        sync_prepared_tree.sync(self.prepared, self.active, {'old.cc'}, self.root / 'report')
        self.assertEqual(same.stat().st_mtime_ns, previous_time)
        self.assertEqual((self.active / 'changed.cc').read_bytes(), b'new')
        self.assertGreater((self.active / 'changed.cc').stat().st_mtime_ns,
                           generated.stat().st_mtime_ns)
        self.assertFalse((self.active / 'old.cc').exists())
        self.assertEqual((self.active / 'local.txt').read_bytes(), b'keep local data')
        self.assertEqual((self.active / 'third_party/package/out/runtime.js').read_bytes(), b'package input')
        self.assertEqual(outputs, {p.name: p.read_bytes() for p in (self.active / 'out/Default').iterdir()})

    def test_overlap_is_rejected_before_any_output_changes(self):
        with self.assertRaisesRegex(ValueError, 'overlap'):
            sync_prepared_tree.sync(self.active, self.active, set(), self.root / 'report')

    def test_sync_without_existing_tracking_is_rejected(self):
        (self.active / 'out/Default/.ninja_log').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing existing incremental tracking'):
            sync_prepared_tree.sync(self.prepared, self.active, set(), self.root / 'report')

    def test_inventory_does_not_follow_a_dangling_directory_symlink(self):
        link = self.prepared / 'directory-link'
        try:
            os.symlink('unavailable-target', link, target_is_directory=True)
        except OSError as error:
            if getattr(error, 'winerror', None) == 1314:
                self.skipTest('Windows symlink privilege is unavailable')
            raise
        self.assertEqual(sync_prepared_tree.inventory(self.prepared)['directory-link'],
                         ('link', True))

    def test_resuming_requires_the_original_tracking_to_be_unchanged(self):
        report = self.root / 'report'
        sync_prepared_tree.sync(self.prepared, self.active, set(), report)
        sync_prepared_tree.sync(self.prepared, self.active, set(), report)
        (self.active / 'out/Default/.ninja_log').write_bytes(b'unexpected new build')
        with self.assertRaisesRegex(ValueError, 'refusing to resume'):
            sync_prepared_tree.sync(self.prepared, self.active, set(), report)


if __name__ == '__main__':
    unittest.main()
