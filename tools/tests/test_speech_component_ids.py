import sys
from pathlib import Path
import re
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from krypton import layout
from krypton.speech_component_ids import language_ids


class SpeechComponentIdsTests(unittest.TestCase):
    def test_allowlist_exactly_matches_the_pinned_engine_and_language_keys(self):
        config = (layout.SOURCE_TREE / 'components/soda/constants.h').read_text(encoding='utf-8')
        expected = {'icnkogojpkfjeajonkmlplionaamopkf'}
        expected.update(component for _, component in language_ids(config))
        source = (layout.KRYPTON_CHROMIUM_SRC / 'components/krypton_privacy_features/speech_component_ids.h').read_text(encoding='utf-8')
        actual = set(re.findall(r'"([a-p]{32})"', source))
        self.assertEqual(actual, expected)
        self.assertGreater(len(actual), 1)


if __name__ == '__main__':
    unittest.main()
