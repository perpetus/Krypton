"""Check Google's rich native omnibox results in an isolated profile.

The helper opts in only this profile and selects Google through native Settings.
Existing profiles and search-engine definitions are never modified.
"""
import json
from pathlib import Path
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    suffix = uuid.uuid4().hex
    profile = ROOT / 'build/profiles' / f'omnibox-images-{suffix}'
    (profile / 'Default').mkdir(parents=True)
    (profile / 'Default/Preferences').write_text(json.dumps({
        'browser': {'check_default_browser': False},
        'search': {'suggest_enabled': True},
    }), encoding='utf-8')
    result_path = ROOT / 'build' / f'omnibox-images-{suffix}.json'
    completed = subprocess.run([
        'powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT / 'tools/verify_omnibox_images.ps1'), '-Profile', str(profile),
        '-ResultPath', str(result_path),
    ], check=False, timeout=90)
    if result_path.exists():
        print(result_path.read_text(encoding='utf-8-sig'))
    return completed.returncode


if __name__ == '__main__':
    raise SystemExit(main())
