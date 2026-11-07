#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
"""Move Krypton to a new Helium release.

Checks out a helium-windows release tag in upstream/helium-windows, which also
moves the Helium core it pins, then reports what Krypton has to react to:

  - the Chromium version, if it moved (re-extract the validation subset);
  - helium-windows' build.py and package.py, which Krypton mirrors;
  - GN flag files, which flags.krypton.gn must not override;
  - patches upstream added or removed, flagging new Helium UI patches that
    patches/skip-helium may need and skip entries upstream has dropped.

Nothing in upstream/ is edited. Krypton's own patches are fixed afterwards
with tools/validate_series.py.

    python tools/sync_helium.py              # newest release tag
    python tools/sync_helium.py 0.19.5.1     # a specific one
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from krypton import layout  # pylint: disable=wrong-import-position

# Files Krypton mirrors or depends on, and what to do when they change.
_WATCHED = (
    ('build.py', 'build.py mirrors this; carry the change over'),
    ('package.py', 'package.py wraps this; check its paths and names still apply'),
    ('flags.windows.gn', 'flags.krypton.gn may not reassign anything set here'),
    ('downloads.ini', 'Windows dependencies the build fetches'),
    ('downloads.x64.ini', 'the prepared Chromium archive for x64'),
    ('resources/platform_resources.txt', 'resources/core may need new art'),
    ('helium-chromium/flags.gn', 'flags.krypton.gn may not reassign anything set here'),
    ('helium-chromium/resources/helium_resources.txt', 'resources/core may need new art'),
    ('helium-chromium/deps.ini', 'pinned dependencies (uBlock, onboarding, ...)'),
)


def _git(repo, *args, check=True):
    result = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True,
                            encoding='utf-8', check=False)
    if check and result.returncode != 0:
        raise SystemExit(f'git {" ".join(args)} failed in {repo}:\n{result.stderr}')
    return result.stdout.strip()


def _series(repo, commit, path):
    text = _git(repo, 'show', f'{commit}:{path}', check=False)
    return {line.split(' #', 1)[0].strip() for line in text.splitlines()
            if line.strip() and not line.startswith('#')}


def _snapshot():
    return {
        'windows': _git(layout.HELIUM_WINDOWS, 'rev-parse', 'HEAD'),
        'helium': _git(layout.HELIUM, 'rev-parse', 'HEAD'),
        'chromium': layout.chromium_version(),
        'tag': _git(layout.HELIUM_WINDOWS, 'describe', '--tags', '--always'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('tag', nargs='?', help='helium-windows release tag (default: newest)')
    core = parser.add_mutually_exclusive_group()
    core.add_argument('--core-ref', help='explicit Helium core commit for a custom Windows port')
    core.add_argument('--release-core', action='store_true',
                      help='return to the core commit pinned by the Windows release')
    args = parser.parse_args()

    if _git(layout.HELIUM_WINDOWS, 'status', '--porcelain', '--ignore-submodules=all') or \
            _git(layout.HELIUM, 'status', '--porcelain'):
        raise SystemExit('upstream/helium-windows has local changes. Upstream is never '
                         'edited in place; move the change into patches/krypton/.')

    saved_core = (layout.HELIUM_CORE_REF.read_text(encoding='utf-8').strip()
                  if layout.HELIUM_CORE_REF.exists() else None)
    core_ref = args.core_ref or (None if args.release_core else saved_core)
    before = _snapshot()
    _git(layout.HELIUM_WINDOWS, 'fetch', '--tags', '--force', 'origin')
    tag = args.tag or _git(layout.HELIUM_WINDOWS, 'tag', '--sort=-creatordate').splitlines()[0]
    _git(layout.HELIUM_WINDOWS, 'checkout', '--quiet', tag)
    _git(layout.HELIUM_WINDOWS, 'submodule', 'update', '--init')
    if core_ref:
        _git(layout.HELIUM, 'fetch', '--no-tags', 'origin', core_ref)
        resolved = _git(layout.HELIUM, 'rev-parse', 'FETCH_HEAD^{commit}')
        _git(layout.HELIUM, 'checkout', '--quiet', '--detach', resolved)
        layout.HELIUM_CORE_REF.write_text(resolved + '\n', encoding='utf-8')
        print(f'Custom Windows port: Helium core explicitly pinned to {resolved}')
    elif args.release_core and layout.HELIUM_CORE_REF.exists():
        layout.HELIUM_CORE_REF.unlink()
    after = _snapshot()

    print(f'helium-windows  {before["tag"]} -> {after["tag"]}')
    print(f'helium          {before["helium"][:10]} -> {after["helium"][:10]}')
    print(f'chromium        {before["chromium"]} -> {after["chromium"]}')
    if before['windows'] == after['windows'] and before['helium'] == after['helium']:
        print('\nAlready at this release; nothing to do.')
        return 0

    print('\nChanged upstream files Krypton depends on:')
    any_changed = False
    for path, why in _WATCHED:
        repo, rel, old, new = layout.HELIUM_WINDOWS, path, before['windows'], after['windows']
        if path.startswith('helium-chromium/'):
            repo, rel = layout.HELIUM, path.removeprefix('helium-chromium/')
            old, new = before['helium'], after['helium']
        stat = _git(repo, 'diff', '--stat', old, new, '--', rel)
        if stat:
            any_changed = True
            print(f'  {path}: {why}')
            print(f'      git -C {repo.relative_to(layout.ROOT).as_posix()} diff '
                  f'{old[:10]} {new[:10]} -- {rel}')
    if not any_changed:
        print('  none')

    for name, repo, old, new in (('helium', layout.HELIUM, before['helium'], after['helium']),
                                 ('helium-windows', layout.HELIUM_WINDOWS, before['windows'],
                                  after['windows'])):
        old_series = _series(repo, old, 'patches/series')
        new_series = _series(repo, new, 'patches/series')
        added, removed = sorted(new_series - old_series), sorted(old_series - new_series)
        skipped = layout.skipped(name)
        if added or removed:
            print(f'\n{name} patches: {len(added)} added, {len(removed)} removed')
            for patch in added:
                note = ''
                if name == 'helium' and patch.startswith('helium/ui/'):
                    note = '   <- browser chrome? if so, add it to patches/skip-helium'
                print(f'  + {patch}{note}')
            for patch in removed:
                note = '   <- delete it from patches/skip-helium' if patch in skipped else ''
                print(f'  - {patch}{note}')

    print('\nNext:')
    if before['chromium'] != after['chromium']:
        print('  python tools/validate_series.py extract   # new Chromium version')
    print('  python tools/validate_series.py apply     # then fix Krypton\'s patches')
    if core_ref:
        print('  helium-core-ref.txt records the custom core pin; upstream source files stay pristine')
    else:
        print('  git add upstream/helium-windows           # record the new pin')
    return 0


if __name__ == '__main__':
    sys.exit(main())
