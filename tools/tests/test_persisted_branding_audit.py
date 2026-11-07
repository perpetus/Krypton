import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import audit_persisted_branding


class PersistedBrandingAuditTests(unittest.TestCase):
    def test_generated_cache_preferences_and_shortcut_are_audited_without_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Default').mkdir()
            state = root / 'Local State'
            state.write_text(json.dumps({'profile': {'info_cache': {'Default': {
                'name': 'Your Helium', 'is_using_default_name': True,
                'shortcut_name': 'Your Helium'}}}}), encoding='utf-8')
            prefs = root / 'Default/Preferences'
            prefs.write_text(json.dumps({'profile': {
                'name': 'Your Helium'}}), encoding='utf-8')
            before = (state.read_bytes(), prefs.read_bytes())
            report = audit_persisted_branding.audit(root)
            self.assertEqual({item['source'] for item in report['findings']},
                             {'profile_cache', 'profile_preferences', 'generated_shortcut'})
            self.assertEqual((state.read_bytes(), prefs.read_bytes()), before)

    def test_custom_labels_are_counted_without_disclosing_or_modifying_them(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
                'Default': {'name': 'Helium research', 'is_using_default_name': False,
                            'shortcut_name': 'Helium research'}}}}), encoding='utf-8')
            report = audit_persisted_branding.audit(root)
            self.assertEqual(report['findings'], [])
            self.assertEqual(report['custom_labels_preserved'], 1)
            self.assertNotIn('Helium research', json.dumps(report))

    def test_profile_paths_cannot_escape_the_supplied_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
                '../outside': {'name': 'Your Helium', 'is_using_default_name': True}}}}),
                encoding='utf-8')
            report = audit_persisted_branding.audit(root)
            self.assertEqual(report['findings'][0]['status'], 'invalid_profile_path')

    def test_already_branded_defaults_do_not_need_migration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'Local State').write_text(json.dumps({'profile': {'info_cache': {
                'Default': {'name': 'Your Krypton', 'is_using_default_name': True}}}}),
                encoding='utf-8')
            self.assertEqual(audit_persisted_branding.audit(root)['findings'], [])


if __name__ == '__main__':
    unittest.main()
