"""Krypton: user-visible names, using the pinned upstream GRIT utilities.

Only the same GRD/GRDP/XTB candidates as the upstream name pass are visited.
Identifiers, comments, pref keys, networking code and legal text are not renamed.
"""

from contextlib import contextmanager
from copy import deepcopy
from fnmatch import fnmatchcase
import importlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as xml

from krypton import layout

EXCLUSIONS = Path(__file__).with_name('branding_exclusions.json')


def _upstream():
    utils = str(layout.HELIUM / 'utils')
    sys.path.insert(0, utils)
    try:
        return (importlib.import_module('name_substitution'),
                importlib.import_module('name_substitution_utils'))
    finally:
        sys.path.remove(utils)


def _excluded(value, entries):
    return any(fnmatchcase(value, entry['pattern']) for entry in entries)


def _excluded_message(name, exclusions):
    return (_excluded(name, exclusions['messages']) and
            not _excluded(name, exclusions.get('product_messages', [])))


@contextmanager
def _krypton_rules(util, exclusions, excluded_fingerprints):
    old_regexes = util.REPLACEMENT_REGEXES
    old_message = util.replace_grit_message
    old_translation = util.replace_xtb_translation

    def message(element):
        if element.tag == 'message' and _excluded_message(element.get('name', ''), exclusions):
            return False
        return old_message(element)

    def translation(element, fingerprint_map):
        if element.get('id') in excluded_fingerprints:
            return False
        return old_translation(element, fingerprint_map)

    util.REPLACEMENT_REGEXES = [
        # Product names can adjoin non-Latin letters or grammatical suffixes
        # in translations (e.g. Amharic prefixes and Finnish "Heliumin").
        (re.compile(r'Helium'), 'Krypton'),
        (re.compile(r'helium://'), 'krypton://'),
    ]
    util.replace_grit_message = message
    util.replace_xtb_translation = translation
    try:
        yield old_translation
    finally:
        util.REPLACEMENT_REGEXES = old_regexes
        util.replace_grit_message = old_message
        util.replace_xtb_translation = old_translation


def apply(source_tree, *, exclusions_path=EXCLUSIONS):
    """Run after upstream names and translations, recomputing fingerprints."""
    tree = Path(source_tree)
    exclusions = json.loads(Path(exclusions_path).read_text(encoding='utf-8'))
    for entry in exclusions['files'] + exclusions['messages'] + exclusions.get('product_messages', []):
        if not entry.get('reason'):
            raise ValueError('Every branding exclusion needs a reason')
    upstream, util = _upstream()
    util.add_grit_to_path(tree)
    files = [path for path in upstream.get_substitutable_files(tree, ['grd', 'grdp'])
             if not _excluded(path.relative_to(tree).as_posix(), exclusions['files'])]
    excluded_fingerprints = set()
    for path in files:
        source = path.read_text(encoding='utf-8')
        if 'Helium' not in source and 'helium://' not in source:
            continue
        root = xml.fromstring(source, util.get_parser())
        for message in root.findall('.//message'):
            if _excluded_message(message.get('name', ''), exclusions):
                excluded_fingerprints.add(str(util.compute_fp(message)))
    fingerprint_map = {}
    modified = []
    dollar = '!!dollar-sign-literal!!'
    with _krypton_rules(util, exclusions, excluded_fingerprints) as replace_translation:
        for path in files:
            source = path.read_text(encoding='utf-8')
            original = source
            # GRDP files are fragments. The onboarding generator expects their
            # first node to be grit-part, whereas upstream serialization adds
            # a document declaration. Keep fragments usable by both consumers,
            # including files branded by an earlier preparation pass.
            if path.suffix == '.grdp':
                source = re.sub(r'^\s*<\?xml[^?]*\?>\s*', '', source)
            if 'Helium' not in source and 'helium://' not in source:
                if source != original:
                    path.write_text(source, encoding='utf-8')
                    modified.append(path.relative_to(tree).as_posix())
                continue
            replaced, mapping = util.replace_grit_tree(source.replace('&#36;', dollar))
            if mapping:
                fingerprint_map.update({str(key): str(value) for key, value in mapping.items()})
                if path.suffix == '.grdp':
                    replaced = re.sub(r'^\s*<\?xml[^?]*\?>\s*', '', replaced)
                path.write_text(replaced.replace(dollar, '&#36;'), encoding='utf-8')
                modified.append(path.relative_to(tree).as_posix())
            elif source != original:
                path.write_text(source, encoding='utf-8')
                modified.append(path.relative_to(tree).as_posix())
        for path in upstream.get_substitutable_files(tree, ['xtb']):
            if _excluded(path.relative_to(tree).as_posix(), exclusions['files']):
                continue
            source = path.read_text(encoding='utf-8')
            protected = source.replace('&#36;', dollar)
            replaced = util.replace_xtb_tree(protected, fingerprint_map)
            # An importer and a product label can have identical source text.
            # Retain the importer's old translation and add the renamed product
            # translation under its newly computed fingerprint.
            parsed = xml.fromstring(protected, util.get_parser())
            copies = []
            for item in parsed.findall('.//translation'):
                old_id = item.get('id')
                if old_id in excluded_fingerprints and old_id in fingerprint_map:
                    renamed = deepcopy(item)
                    replace_translation(renamed, fingerprint_map)
                    renamed.set('id', fingerprint_map[old_id])
                    copies.append(renamed)
            if copies:
                result = xml.fromstring(replaced or protected, util.get_parser())
                ids = {item.get('id') for item in result.findall('.//translation')}
                for item in copies:
                    if item.get('id') not in ids:
                        result.append(item)
                        ids.add(item.get('id'))
                replaced = xml.tostring(result, encoding='unicode', xml_declaration=True)
            if replaced:
                path.write_text(replaced.replace(dollar, '&#36;'), encoding='utf-8')
                modified.append(path.relative_to(tree).as_posix())
    print(f'Krypton: renamed user-visible strings in {len(modified)} files')
    return modified
