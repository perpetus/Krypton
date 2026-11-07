"""Staging and validation must not borrow active build checkpoints."""

from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import build
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import validate_series
from krypton import layout


class UpgradeStagingTests(unittest.TestCase):
    def setUp(self):
        parent = layout.BUILD_DIR / 'tmp'
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def windows_override(self):
        upstream = self.root / 'upstream'
        owned = self.root / 'owned'
        upstream.mkdir()
        owned.mkdir()
        (upstream / 'series').write_text('original.patch\n', encoding='utf-8')
        original = upstream / 'original.patch'
        original.write_text('# pinned upstream patch\n', encoding='utf-8')
        replacement = owned / 'replacement.patch'
        replacement.write_text('# refreshed context\n', encoding='utf-8')
        manifest = owned / 'overrides.json'
        manifest.write_text(json.dumps({'original.patch': {
            'patch': 'replacement.patch',
            'upstream_sha256': hashlib.sha256(original.read_bytes()).hexdigest()}}), encoding='utf-8')
        for attribute, value in [('PATCH_LAYERS', (('helium-windows', upstream),)),
                                 ('SKIP_LISTS', {}), ('KRYPTON_PATCHES', owned),
                                 ('WINDOWS_PATCH_OVERRIDES', manifest)]:
            mock = patch.object(layout, attribute, value)
            mock.start()
            self.addCleanup(mock.stop)
        return original, replacement, manifest

    def test_windows_context_override_preserves_series_position(self):
        original, replacement, manifest = self.windows_override()
        self.assertEqual(layout.layer_patches('helium-windows'), [replacement])

    def test_windows_override_detects_a_changed_upstream_patch(self):
        original, replacement, manifest = self.windows_override()
        original.write_text('# changed upstream\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'changed upstream'):
            layout.layer_patches('helium-windows')

    def test_windows_override_cannot_escape_owned_patch_directory(self):
        original, replacement, manifest = self.windows_override()
        data = json.loads(manifest.read_text(encoding='utf-8'))
        data['original.patch']['patch'] = '../outside.patch'
        manifest.write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'escapes'):
            layout.layer_patches('helium-windows')

    def test_resource_prose_is_not_a_symbol_but_resource_attributes_are(self):
        pristine = self.root / 'pristine'
        tree = self.root / 'src'
        patch_dir = self.root / 'patches'
        for directory in (pristine, tree, patch_dir):
            directory.mkdir()
        (patch_dir / 'series').write_text('omitted.patch\n', encoding='utf-8')
        (patch_dir / 'omitted.patch').write_text(
            '--- /dev/null\n+++ b/legacy.h\n@@ -0,0 +1 @@\n+int centers = 1;\n', encoding='utf-8')
        skipped = self.root / 'skip'
        skipped.write_text('omitted.patch\n', encoding='utf-8')
        resource = tree / 'messages.grd'
        resource.write_text('<message name="IDS_PRESENT" desc="centers the item">centers</message>',
                            encoding='utf-8')
        with patch.object(layout, 'PATCH_LAYERS', (('helium', patch_dir),)), \
             patch.object(layout, 'SKIP_LISTS', {'helium': skipped}), \
             patch.object(validate_series, '_PRISTINE', pristine), \
             patch.object(validate_series, '_TREE', tree), \
             patch.object(validate_series, '_all_patches', return_value=iter([])):
            self.assertEqual(validate_series.check_references(), 0)
            resource.write_text('<message name="centers">Text</message>', encoding='utf-8')
            self.assertEqual(validate_series.check_references(), 1)

    def test_separate_checkpoint_is_read_and_written_independently(self):
        state = self.root / 'state.json'
        state.write_text('{"unrelated":"keep"}', encoding='utf-8')
        active_before = build._STATE_PATH.read_bytes()
        build._write_state(state, prepare='staged')
        self.assertEqual(build._read_state(state)['prepare'], 'staged')
        self.assertEqual(build._read_state(state)['unrelated'], 'keep')
        self.assertEqual(build._STATE_PATH.read_bytes(), active_before)

    def test_custom_core_mismatch_requires_sync(self):
        pin = self.root / 'core-ref.txt'
        pin.write_text('expected\n', encoding='utf-8')
        with patch.object(layout, 'HELIUM_CORE_REF', pin), \
             patch.object(layout.subprocess, 'run', return_value=SimpleNamespace(stdout='other\n')):
            with self.assertRaisesRegex(ValueError, 'does not match'):
                layout.check_core_ref()

    def test_validator_refuses_to_use_active_tree(self):
        result = subprocess.run([sys.executable, str(layout.ROOT / 'tools/validate_series.py'),
                                 '--work-dir', str(layout.BUILD_DIR), 'apply'],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn('must not overlap', result.stderr)

    def test_fuzzy_krypton_patch_is_a_validation_failure(self):
        pristine = self.root / 'pristine'
        pristine.mkdir()
        source = self.root / 'change.patch'
        source.write_text('# test\n', encoding='utf-8')
        with patch.object(validate_series, '_PRISTINE', pristine), \
             patch.object(validate_series, '_TREE', self.root / 'src'), \
             patch.object(validate_series, '_all_patches', return_value=iter([('krypton', source)])), \
             patch.object(layout, 'ROOT', self.root), \
             patch.object(validate_series.subprocess, 'run', return_value=SimpleNamespace(
                 stdout='Hunk #1 succeeded with fuzz 2.\n', stderr='', returncode=0)), \
             patch.object(validate_series.official_ublock, 'stage') as stage:
            self.assertEqual(validate_series.apply(Path('patch.exe'), ['krypton']), 1)
            stage.assert_not_called()

    def test_skipped_patch_reference_is_a_validation_failure(self):
        pristine = self.root / 'pristine'
        pristine.mkdir()
        with patch.object(validate_series, '_PRISTINE', pristine), \
             patch.object(validate_series, '_TREE', self.root / 'src'), \
             patch.object(validate_series, '_all_patches', return_value=iter([])), \
             patch.object(validate_series, 'check_references', return_value=1), \
             patch.object(validate_series.official_ublock, 'stage') as stage:
            self.assertEqual(validate_series.apply(Path('patch.exe'), ['krypton']), 1)
            stage.assert_not_called()


if __name__ == '__main__':
    unittest.main()
