import sys
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as xml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from krypton import branding, layout


class BrandingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.tree = Path(self.temporary.name)
        (self.tree / 'tools/grit').mkdir(parents=True)
        _, self.util = branding._upstream()
        self.util.add_grit_to_path(layout.SOURCE_TREE)

    def write(self, name, text):
        path = self.tree / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
        return path

    def fingerprint(self, text):
        return str(self.util.compute_fp(xml.fromstring(f'<message>{text}</message>')))

    def test_translations_follow_new_fingerprints_and_second_pass_is_empty(self):
        grd = self.write('chrome/app/strings.grdp',
                         '<grit-part><message name="IDS_TITLE">About Helium</message>'
                         '<message name="IDS_URL">helium://settings/privacy</message></grit-part>')
        old = self.fingerprint('About Helium')
        xtb = self.write('chrome/app/strings_fr.xtb',
                         f'<translationbundle lang="fr"><translation id="{old}">À propos de Helium</translation></translationbundle>')
        self.assertEqual(len(branding.apply(self.tree)), 2)
        self.assertIn('About Krypton', grd.read_text(encoding='utf-8'))
        self.assertIn('krypton://settings/privacy', grd.read_text(encoding='utf-8'))
        translation = xml.parse(xtb).getroot()[0]
        self.assertEqual(translation.get('id'), self.fingerprint('About Krypton'))
        self.assertEqual(translation.text, 'À propos de Krypton')
        self.assertEqual(branding.apply(self.tree), [])

    def test_importer_and_product_can_share_the_old_translation(self):
        grd = self.write('chrome/app/strings.grdp',
                         '<grit-part><message name="IDS_PRODUCT">Helium</message>'
                         '<message name="IDS_IMPORT_FROM_HELIUM">Helium</message></grit-part>')
        old = self.fingerprint('Helium')
        xtb = self.write('chrome/app/strings_fr.xtb',
                         f'<translationbundle lang="fr"><translation id="{old}">Helium</translation></translationbundle>')
        branding.apply(self.tree)
        messages = {item.get('name'): item.text for item in xml.parse(grd).getroot()}
        self.assertEqual(messages['IDS_PRODUCT'], 'Krypton')
        self.assertEqual(messages['IDS_IMPORT_FROM_HELIUM'], 'Helium')
        translations = {item.get('id'): item.text for item in xml.parse(xtb).getroot()}
        self.assertEqual(translations[old], 'Helium')
        self.assertEqual(translations[self.fingerprint('Krypton')], 'Krypton')

    def test_translated_prefixes_and_inflections_are_renamed(self):
        self.write('chrome/app/strings.grdp',
                   '<grit-part><message name="IDS_PRODUCT">About Helium</message></grit-part>')
        old = self.fingerprint('About Helium')
        xtb = self.write('chrome/app/strings_am.xtb',
                         f'<translationbundle lang="am"><translation id="{old}">'
                         '\u12e8Helium Heliumin Heliums \u12e8helium://settings'
                         '</translation></translationbundle>')
        branding.apply(self.tree)
        translated = xml.parse(xtb).getroot()[0]
        self.assertEqual(translated.text,
                         '\u12e8Krypton Kryptonin Kryptons \u12e8krypton://settings')
        self.assertEqual(translated.get('id'), self.fingerprint('About Krypton'))

    def test_grdp_remains_a_fragment_for_the_onboarding_generator(self):
        grdp = self.write('components/helium_onboarding/strings.grdp',
                          '<grit-part><message name="IDS_TITLE">Welcome to Helium</message></grit-part>')
        branding.apply(self.tree)
        self.assertTrue(grdp.read_text(encoding='utf-8').startswith('<grit-part>'))
        self.assertEqual(branding.apply(self.tree), [])
        # Repair source trees prepared with the old serializer as well.
        grdp.write_text('<?xml version="1.0"?>\n' + grdp.read_text(encoding='utf-8'), encoding='utf-8')
        branding.apply(self.tree)
        self.assertTrue(grdp.read_text(encoding='utf-8').startswith('<grit-part>'))

    def test_product_about_and_import_permission_are_not_legal_or_importer_exclusions(self):
        grdp = self.write('chrome/app/about.grdp',
                          '<grit-part><message name="IDS_ABOUT_VERSION_COPYRIGHT">'
                          'Copyright The Helium Authors.</message>'
                          '<message name="IDS_VERSION_UI_LICENSE">Helium is made possible by '
                          'the Chromium project.</message>'
                          '<message name="IDS_HELIUM_ONBOARDING_DATA_IMPORT_PROMPT_PERMISSION">'
                          'Allow Helium to find browsers.</message>'
                          '<message name="IDS_IMPORT_FROM_HELIUM">Helium</message>'
                          '<message name="IDS_THIRD_PARTY_COPYRIGHT">The Helium Authors</message>'
                          '</grit-part>')
        branding.apply(self.tree)
        messages = {item.get('name'): item.text for item in xml.parse(grdp).getroot()}
        self.assertEqual(messages['IDS_ABOUT_VERSION_COPYRIGHT'], 'Copyright The Krypton Authors.')
        self.assertIn('Krypton is made possible by the Chromium project.', messages['IDS_VERSION_UI_LICENSE'])
        self.assertIn('Allow Krypton', messages['IDS_HELIUM_ONBOARDING_DATA_IMPORT_PROMPT_PERMISSION'])
        self.assertEqual(messages['IDS_IMPORT_FROM_HELIUM'], 'Helium')
        self.assertEqual(messages['IDS_THIRD_PARTY_COPYRIGHT'], 'The Helium Authors')
        self.assertEqual(branding.apply(self.tree), [])

    def test_identifiers_networking_code_and_attribution_are_preserved(self):
        source = self.write('chrome/common/identity.cc', 'const char kBrand[] = "Helium";')
        grd = self.write('chrome/app/strings.grdp',
                         '<grit-part><message name="IDS_COPYRIGHT">The Helium Authors</message>'
                         '<message name="IDS_HELIUM_PRODUCT">Helium</message></grit-part>')
        branding.apply(self.tree)
        self.assertIn('kBrand[] = "Helium"', source.read_text(encoding='utf-8'))
        self.assertIn('The Helium Authors', grd.read_text(encoding='utf-8'))
        self.assertIn('name="IDS_HELIUM_PRODUCT"', grd.read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
