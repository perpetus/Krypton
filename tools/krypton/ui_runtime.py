# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Validate the complete module distribution before it reaches Chromium."""

import hashlib
import json
from pathlib import Path


RUNTIME_FILES = (
    Path('krypton_ui.dll'),
    Path('Microsoft.WindowsAppRuntime.Bootstrap.dll'),
    *(Path('assets/settings') / name for name in (
        'hr_classic.png', 'hr_compact.png', 'hr_dyn.png', 'vt_compact.png',
        'vt_sidebar.png', 'vt_sidebar_title.png')),
)


def verify_contract(vendored: Path, checkout: Path) -> None:
    """The ABI checked by the browser must be exactly the module's ABI."""
    source = checkout / 'modules/abi/include/krypton_ui/abi.h'
    if vendored.read_bytes() != source.read_bytes():
        raise ValueError(f'{vendored} differs from {source}; refresh the '
                         'vendored contract and update the adapter.')


def verify_runtime(directory: Path) -> dict[str, str]:
    """Reject an incomplete staging directory, including missing UI assets."""
    hashes = {}
    for relative in RUNTIME_FILES:
        path = directory / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f'KryptonUI runtime file is missing or empty: {path}')
        hashes[relative.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hashes


def write_manifest(directory: Path, contract: Path) -> None:
    manifest = {
        'abi_sha256': hashlib.sha256(contract.read_bytes()).hexdigest(),
        'files': verify_runtime(directory),
    }
    (directory / 'runtime-manifest.json').write_text(
        json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
