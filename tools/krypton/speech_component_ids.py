"""Krypton: derive the exact caption component allowlist from pinned SODA keys."""
from pathlib import Path
import re


def language_ids(source):
    records = re.split(r'\{LanguageCode::', source)[1:]
    result = []
    for record in records:
        language = re.search(r'"([^"]+)"', record)
        values = re.findall(r'0x([0-9a-fA-F]{2})\b', record.split('}}', 1)[0])
        if not language or len(values) != 32:
            raise ValueError('Pinned SODA language configuration changed')
        key_hash = bytes(int(value, 16) for value in values)
        component_id = ''.join(chr(ord('a') + int(nibble, 16)) for nibble in key_hash[:16].hex())
        result.append((language[1], component_id))
    if not result:
        raise ValueError('No pinned SODA language components found')
    return result


def generate(source_tree, destination):
    source = (Path(source_tree) / 'components/soda/constants.h').read_text(encoding='utf-8')
    ids = [('engine', 'icnkogojpkfjeajonkmlplionaamopkf')] + language_ids(source)
    lines = ['// Copyright 2026 The Krypton Authors',
             '// Generated from pinned SODA installer keys. See tools/krypton/speech_component_ids.py.',
             '#ifndef COMPONENTS_KRYPTON_PRIVACY_FEATURES_SPEECH_COMPONENT_IDS_H_',
             '#define COMPONENTS_KRYPTON_PRIVACY_FEATURES_SPEECH_COMPONENT_IDS_H_',
             '#include <array>', '#include <string_view>',
             'namespace krypton::privacy {',
             f'inline constexpr std::array<std::string_view, {len(ids)}> kLiveCaptionComponents{{']
    lines += [f'    "{component}",  // {language}' for language, component in ids]
    lines += ['};', '}  // namespace krypton::privacy', '#endif', '']
    Path(destination).write_text('\n'.join(lines), encoding='utf-8')
    return ids
