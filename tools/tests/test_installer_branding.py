import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('krypton_package', ROOT / 'package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class InstallerBrandingTests(unittest.TestCase):
    def test_staging_preserves_upstream_and_legal_text_and_uses_krypton_paths(self):
        original = package.layout.HELIUM_WINDOWS / 'installer/helium.nsi'
        before = original.read_bytes()
        staged = package._stage_installer_script()
        self.assertEqual(original.read_bytes(), before)
        text = staged.read_text(encoding='utf-8')
        self.assertIn('!define PRODUCT_NAME "Krypton"', text)
        self.assertIn('!define PRODUCT_COMPANY_PATH "Krypton"', text)
        self.assertIn('Copyright The Helium Authors', text)
        module = package._load_helium_packaging()
        with patch.object(package.subprocess, 'run') as run:
            module._build_nsis_installer('1.2.3.4', 'x64', ROOT / 'build/test-inputs', ROOT / 'build/test.exe')
        command = run.call_args.args[0]
        self.assertEqual(Path(command[-1]), staged)
        self.assertIn('-DVERSION=1.2.3.4', command)
        self.assertTrue(any(value.startswith('-DLICENSE_FILE=') for value in command))


if __name__ == '__main__':
    unittest.main()
