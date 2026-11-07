"""Approved discovery exceptions must not permit other upstream overrides."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import build


class OptionalFeatureGnTests(unittest.TestCase):
    def flags(self, text, upstream):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'flags.gn'
            path.write_text(text, encoding='utf-8')
            with patch.object(build.layout, 'KRYPTON_FLAGS', path):
                return build._krypton_gn_flags(upstream)

    def test_named_discovery_overrides_and_new_args_are_allowed(self):
        text = 'enable_mdns = true\nenable_service_discovery = true\nnew_arg = true\n'
        self.assertEqual(self.flags(text, 'enable_mdns=false\nenable_service_discovery=false\n'), text)

    def test_approved_override_does_not_allow_an_unrelated_override(self):
        with self.assertRaisesRegex(SystemExit, 'safe_browsing_mode'):
            self.flags('enable_mdns=true\nsafe_browsing_mode=1\n',
                       'enable_mdns=false\nsafe_browsing_mode=0\n')


if __name__ == '__main__':
    unittest.main()
