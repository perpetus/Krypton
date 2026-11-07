# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from krypton import ui_runtime  # pylint: disable=wrong-import-position
sys.path.pop(0)


class UiRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def populate(self):
        for relative in ui_runtime.RUNTIME_FILES:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(relative.as_posix().encode('ascii'))

    def test_rejects_contract_drift(self):
        vendored = self.root / 'vendored.h'
        vendored.write_bytes(b'version-two')
        source = self.root / 'modules/abi/include/krypton_ui/abi.h'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'version-one')
        with self.assertRaisesRegex(ValueError, 'differs'):
            ui_runtime.verify_contract(vendored, self.root)
        source.write_bytes(vendored.read_bytes())
        ui_runtime.verify_contract(vendored, self.root)

    def test_rejects_missing_asset(self):
        self.populate()
        (self.root / ui_runtime.RUNTIME_FILES[-1]).unlink()
        with self.assertRaisesRegex(ValueError, 'missing or empty'):
            ui_runtime.verify_runtime(self.root)

    def test_rejects_empty_module(self):
        self.populate()
        (self.root / 'krypton_ui.dll').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'krypton_ui.dll'):
            ui_runtime.verify_runtime(self.root)

    def test_records_all_runtime_files_and_contract(self):
        self.populate()
        contract = self.root / 'abi.h'
        contract.write_bytes(b'contract')
        ui_runtime.write_manifest(self.root, contract)
        manifest = json.loads((self.root / 'runtime-manifest.json').read_text())
        self.assertEqual(len(manifest['files']), 8)
        self.assertEqual(len(manifest['abi_sha256']), 64)
        self.assertTrue(all(len(value) == 64
                            for value in manifest['files'].values()))


if __name__ == '__main__':
    unittest.main()
