#!/usr/bin/env python3
"""Read-only audit of browser-generated profile labels outside resource packs.

Reads only the profile cache/name metadata. Never modifies a live profile or
reads cookies, credentials, history, bookmarks, or saved tab contents.
"""

import argparse
import json
from pathlib import Path


def audit(user_data_dir):
    root = Path(user_data_dir).resolve()
    state = json.loads((root / 'Local State').read_text(encoding='utf-8'))
    cache = state.get('profile', {}).get('info_cache', {})
    findings = []
    custom_labels_preserved = 0
    for directory, info in cache.items():
        if not isinstance(info, dict):
            continue
        profile_dir = (root / directory).resolve()
        if not profile_dir.is_relative_to(root) or profile_dir == root:
            findings.append({'profile': directory, 'source': 'cache',
                             'status': 'invalid_profile_path'})
            continue
        is_default = info.get('is_using_default_name') is True
        name = info.get('name', '')
        if isinstance(name, str) and 'Helium' in name:
            if is_default:
                findings.append({'profile': directory, 'source': 'profile_cache',
                                 'status': 'legacy_generated_label'})
            else:
                custom_labels_preserved += 1
        shortcut = info.get('shortcut_name', '')
        if is_default and isinstance(shortcut, str) and 'Helium' in shortcut:
            findings.append({'profile': directory, 'source': 'generated_shortcut',
                             'status': 'legacy_generated_label'})
        preferences = profile_dir / 'Preferences'
        if not preferences.is_file():
            continue
        prefs = json.loads(preferences.read_text(encoding='utf-8')).get('profile', {})
        pref_name = prefs.get('name', '')
        # Registered default-valued prefs can be absent from Preferences. In
        # that case the explicit cache metadata is the available evidence.
        if (prefs.get('using_default_name', is_default) is True and isinstance(pref_name, str)
                and 'Helium' in pref_name):
            findings.append({'profile': directory, 'source': 'profile_preferences',
                             'status': 'legacy_generated_label'})
    return {'findings': findings, 'custom_labels_preserved': custom_labels_preserved}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--user-data-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = json.dumps(audit(args.user_data_dir), indent=2)
    if args.output:
        args.output.write_text(report + '\n', encoding='utf-8')
    else:
        print(report)


if __name__ == '__main__':
    main()
