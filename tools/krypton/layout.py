# Copyright 2026 The Krypton Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
"""Where each layer lives, and the order the layers apply in.

Krypton is Helium plus a thin layer on top. Helium and helium-windows are
pinned, unmodified submodules under upstream/; everything Krypton owns sits
at the top level of this repository and is applied after them. Anything that
needs a path or the layer order should import it from here, so the order is
stated once.
"""

from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]

# Upstream source files are never edited in place. Windows releases normally
# pin their own core. A custom port records its override in helium-core-ref.txt
# and restores it through sync_helium.py; check_core_ref rejects stale checkouts.
HELIUM_WINDOWS = ROOT / 'upstream' / 'helium-windows'
HELIUM = HELIUM_WINDOWS / 'helium-chromium'
HELIUM_UTILS = HELIUM / 'utils'

# Krypton's layer.
KRYPTON_PATCHES = ROOT / 'patches'
KRYPTON_RESOURCES = ROOT / 'resources'
KRYPTON_CHROMIUM_SRC = ROOT / 'chromium_src'
KRYPTON_FLAGS = ROOT / 'flags.krypton.gn'
KRYPTON_DEPS = ROOT / 'deps.ini'
HELIUM_CORE_REF = ROOT / 'helium-core-ref.txt'
WINDOWS_PATCH_OVERRIDES = KRYPTON_PATCHES / 'helium-windows-overrides.json'

# KryptonUI's C contract as Chromium compiles it.
KRYPTON_UI_ABI = (KRYPTON_CHROMIUM_SRC / 'chrome' / 'browser' / 'ui' / 'krypton_ui' /
                  'include' / 'krypton_ui' / 'abi.h')

# Generated, ignored by Git.
BUILD_DIR = ROOT / 'build'
SOURCE_TREE = BUILD_DIR / 'src'
DOWNLOAD_CACHE = BUILD_DIR / 'download_cache'
STAGED_RESOURCES = BUILD_DIR / 'resources'
VALIDATION_DIR = BUILD_DIR / 'validation'

# Applied in this order. A later layer adapts to the earlier ones, never the
# other way round: when Helium changes, Krypton's patches are the ones fixed.
PATCH_LAYERS = (
    ('helium', HELIUM / 'patches'),
    ('helium-windows', HELIUM_WINDOWS / 'patches'),
    ('krypton', KRYPTON_PATCHES),
)

# Upstream patches Krypton leaves out, by layer: Helium's browser-chrome UI,
# which KryptonUI replaces. Upstream files stay untouched; the series is
# filtered as it is read.
SKIP_LISTS = {
    'helium': KRYPTON_PATCHES / 'skip-helium',
    'helium-windows': KRYPTON_PATCHES / 'skip-helium-windows',
}

# GN args, concatenated in this order. Krypton's file may add arguments but
# may not reassign one an upstream file sets; build.py enforces that.
GN_FLAG_FILES = (
    HELIUM / 'flags.gn',
    HELIUM_WINDOWS / 'flags.windows.gn',
)


def parse_series(patch_dir):
    """Patch paths named by a quilt series file, in order."""
    series = patch_dir / 'series'
    if not series.exists():
        return []
    paths = []
    for line in series.read_text(encoding='utf-8').splitlines():
        line = line.split(' #', 1)[0].strip()
        if line and not line.startswith('#'):
            paths.append(patch_dir / line)
    return paths


def skipped(layer):
    """Series entries of `layer` that Krypton does not apply."""
    skip_list = SKIP_LISTS.get(layer)
    if not skip_list or not skip_list.exists():
        return set()
    entries = set()
    for line in skip_list.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if line:
            entries.add(line)
    return entries


def layer_patches(layer):
    """Patch paths Krypton applies from `layer`, in series order."""
    patch_dir = dict(PATCH_LAYERS)[layer]
    skip = skipped(layer)
    paths = parse_series(patch_dir)
    unknown = skip - {p.relative_to(patch_dir).as_posix() for p in paths}
    if unknown:
        raise ValueError(f'{SKIP_LISTS[layer].name} names patches {layer} no longer has: ' +
                         ', '.join(sorted(unknown)))
    selected = [p for p in paths if p.relative_to(patch_dir).as_posix() not in skip]
    if layer != 'helium-windows' or not WINDOWS_PATCH_OVERRIDES.exists():
        return selected
    overrides = json.loads(WINDOWS_PATCH_OVERRIDES.read_text(encoding='utf-8'))
    known = {p.relative_to(patch_dir).as_posix() for p in selected}
    if overrides.keys() - known:
        raise ValueError('Windows patch override no longer names a selected upstream patch')
    result = []
    for original in selected:
        entry = overrides.get(original.relative_to(patch_dir).as_posix())
        if entry is None:
            result.append(original)
            continue
        if hashlib.sha256(original.read_bytes()).hexdigest() != entry['upstream_sha256']:
            raise ValueError(f'Windows patch changed upstream; refresh its override: {original}')
        replacement = (KRYPTON_PATCHES / entry['patch']).resolve()
        if not replacement.is_relative_to(KRYPTON_PATCHES.resolve()):
            raise ValueError('Windows patch override escapes Krypton patches')
        result.append(replacement)
    return result


def chromium_version():
    return (HELIUM / 'chromium_version.txt').read_text(encoding='utf-8').strip()


def check_core_ref():
    """A custom port must use the recorded core commit on every checkout."""
    if not HELIUM_CORE_REF.exists():
        return
    expected = HELIUM_CORE_REF.read_text(encoding='utf-8').strip()
    actual = subprocess.run(['git', '-C', str(HELIUM), 'rev-parse', 'HEAD'],
                            capture_output=True, text=True, check=True).stdout.strip()
    if actual != expected:
        raise ValueError('Helium core does not match helium-core-ref.txt; '
                         'run tools/sync_helium.py with the recorded Windows release first')
