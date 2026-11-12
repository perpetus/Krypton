#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Check owned Python syntax, version markers and the selected patch lists.

This is a source/configuration check, not a test runner or a Chromium build.
"""

import ast
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from krypton import layout, product_version


def main():
    files = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--', '*.py'],
                           check=True, capture_output=True).stdout.decode('utf-8').split('\0')
    owned = [ROOT / name for name in files if name and not name.startswith('upstream/')]
    for path in owned:
        ast.parse(path.read_text(encoding='utf-8-sig'), filename=str(path))
    product_version.parts()
    layout.check_core_ref()
    total = 0
    for name, directory in layout.PATCH_LAYERS:
        series = layout.parse_series(directory)
        if len(series) != len(set(series)):
            raise ValueError(f'Duplicate patch in {directory / "series"}')
        selected = layout.layer_patches(name)
        for path in selected:
            if not path.is_file() or path.stat().st_size == 0:
                raise ValueError(f'Missing or empty selected patch: {path}')
        total += len(selected)
    if not layout.KRYPTON_UI_ABI.is_file():
        raise ValueError('Missing browser/UI C contract')
    print(f'Configuration valid: {len(owned)} Python files; {total} selected patches; '
          f'Chromium {layout.chromium_version()}; Krypton '
          f'{layout.KRYPTON_VERSION.read_text(encoding="utf-8").strip()}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
