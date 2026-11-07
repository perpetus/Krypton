"""Patch validation must include sources shipped in pinned tar dependencies."""

import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import validate_series


class ValidationDependenciesTests(unittest.TestCase):
    def test_tar_dependency_extracts_only_requested_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cache').mkdir()
            archive = root / 'cache/source.tar.gz'
            with tarfile.open(archive, 'w:gz') as bundle:
                for name, content in [('bundle/strings.grdp', b'<grit-part/>'),
                                      ('bundle/unrequested.txt', b'not needed')]:
                    info = tarfile.TarInfo(name)
                    info.size = len(content)
                    bundle.addfile(info, io.BytesIO(content))
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            (root / 'deps.ini').write_text(
                '[onboarding]\nversion=1\nurl=https://example.invalid/source.tar.gz\n'
                f'download_filename=source.tar.gz\nsha256={digest}\n'
                'output_path=./components/onboarding\nstrip_leading_dirs=bundle\n',
                encoding='utf-8')
            pristine = root / 'pristine'
            with patch.object(validate_series.layout, 'HELIUM', root), \
                 patch.object(validate_series.layout, 'HELIUM_WINDOWS', root), \
                 patch.object(validate_series.layout, 'KRYPTON_DEPS', root / 'krypton-deps.ini'), \
                 patch.object(validate_series.layout, 'DOWNLOAD_CACHE', root / 'cache'), \
                 patch.object(validate_series, '_PRISTINE', pristine):
                wanted = {'components/onboarding/strings.grdp'}
                self.assertEqual(validate_series._extract_from_dependencies(wanted), wanted)
            self.assertEqual((pristine / 'components/onboarding/strings.grdp').read_bytes(),
                             b'<grit-part/>')
            self.assertFalse((pristine / 'components/onboarding/unrequested.txt').exists())


if __name__ == '__main__':
    unittest.main()
