"""The shipped extension must come entirely from the pinned official package."""

import base64
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from krypton import official_ublock


class OfficialUBlockTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tree = self.root / 'src'
        self.cache = self.root / 'cache'
        self.cache.mkdir()
        self.target = self.tree / 'third_party/ublock'
        self.target.mkdir(parents=True)
        (self.target / 'fork-only.js').write_bytes(b'fork code')
        self.metadata = self.root / 'chromium_src/third_party/ublock'
        self.metadata.mkdir(parents=True)
        for name in ('BUILD.gn', 'generate_file_list.py', 'README.chromium'):
            (self.metadata / name).write_bytes(b'build metadata')
        self.manifest = {'version': '1.2.3', 'manifest_version': 2}
        self.files = {
            'manifest.json': json.dumps(self.manifest).encode(),
            'js/assets.js': b'official code',
            'assets/assets.json': b'{"official":true}',
            'assets/filter.txt': b'official filters',
        }
        self.config = self.root / 'deps.ini'
        self.write_release()
        for attribute, value in [('KRYPTON_DEPS', self.config),
                                 ('KRYPTON_CHROMIUM_SRC', self.metadata.parents[1])]:
            mock = patch.object(official_ublock.layout, attribute, value)
            mock.start()
            self.addCleanup(mock.stop)

    def write_release(self):
        archive = self.cache / 'official.zip'
        with zipfile.ZipFile(archive, 'w') as bundle:
            for name, content in self.files.items():
                bundle.writestr('uBlock0.chromium/' + name, content)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.config.write_text(
            '[ublock_origin_official]\nversion=1.2.3\n'
            'url=https://example.invalid/official.zip\ndownload_filename=official.zip\n'
            f'sha256={digest}\nstrip_leading_dirs=uBlock0.chromium\n', encoding='utf-8')

    def test_replaces_fork_and_preserves_every_official_runtime_file(self):
        official_ublock.stage(self.tree, self.cache)
        self.assertFalse((self.target / 'fork-only.js').exists())
        for name, content in self.files.items():
            if name != 'manifest.json':
                self.assertEqual((self.target / name).read_bytes(), content)
        manifest = json.loads((self.target / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest.pop('key'), official_ublock.PUBLIC_KEY)
        self.assertEqual(manifest, self.manifest)
        for name in ('BUILD.gn', 'generate_file_list.py', 'README.chromium'):
            self.assertEqual((self.target / name).read_bytes(), b'build metadata')

    def test_checksum_failure_does_not_touch_the_existing_extension(self):
        (self.cache / 'official.zip').write_bytes(b'wrong download')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            official_ublock.stage(self.tree, self.cache)
        self.assertEqual((self.target / 'fork-only.js').read_bytes(), b'fork code')

    def test_version_mismatch_does_not_touch_the_existing_extension(self):
        self.files['manifest.json'] = b'{"version":"9.9.9"}'
        self.write_release()
        with self.assertRaisesRegex(ValueError, 'version'):
            official_ublock.stage(self.tree, self.cache)
        self.assertTrue((self.target / 'fork-only.js').exists())

    def test_archive_path_cannot_escape_the_extension(self):
        self.files['../escaped.js'] = b'bad path'
        self.write_release()
        with self.assertRaisesRegex(ValueError, 'Unsafe'):
            official_ublock.stage(self.tree, self.cache)
        self.assertTrue((self.target / 'fork-only.js').exists())
        self.assertFalse((self.tree / 'third_party/escaped.js').exists())

    def test_public_key_matches_the_native_component_identity(self):
        digest = hashlib.sha256(base64.b64decode(official_ublock.PUBLIC_KEY)).digest()
        extension_id = ''.join(chr(97 + int(c, 16)) for c in digest[:16].hex())
        self.assertEqual(extension_id, 'fkgkibajhfbepljeaefdnfnegdcjomkh')
        self.assertEqual(hashlib.sha1(extension_id.encode()).hexdigest().upper(),
                         '26F8288C7B137C0A9887BD3F52089F81B566FFF5')


if __name__ == '__main__':
    unittest.main()
