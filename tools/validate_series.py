#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
"""Check that every patch layer applies to pristine Chromium, in a minute.

A full source tree is tens of gigabytes and takes most of an hour to prepare.
The series only read a few thousand files, so this extracts exactly those
from the prepared Chromium archive and applies Helium, helium-windows and
Krypton in build order with the same `patch` flags Helium's build uses.

Unlike the build it does not stop at the first failure: one broken patch
usually breaks several after it, and seeing all of them at once saves passes.

    python tools/validate_series.py extract   # once per Chromium version
    python tools/validate_series.py apply     # after every patch change
"""

import argparse
import configparser
import hashlib
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from krypton import layout  # pylint: disable=wrong-import-position
from krypton import official_ublock  # pylint: disable=wrong-import-position

_PRISTINE = layout.VALIDATION_DIR / 'pristine'
_TREE = layout.VALIDATION_DIR / 'src'
_DEFAULT_PATCH = Path(r'C:\Program Files\Git\usr\bin\patch.exe')


def _all_patches():
    for name, _ in layout.PATCH_LAYERS:
        for path in layout.layer_patches(name):
            yield name, path


def _touched_files():
    """Files the series read, minus the ones a patch creates from nothing."""
    wanted, created = set(), set()
    for _, path in _all_patches():
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        for i, line in enumerate(lines[:-1]):
            if not (line.startswith('--- ') and lines[i + 1].startswith('+++ ')):
                continue
            old = line[4:].split('\t')[0].strip()
            new = lines[i + 1][4:].split('\t')[0].strip()
            if old == '/dev/null':
                created.add(new[2:] if new.startswith('b/') else new)
            else:
                wanted.add(old[2:] if old.startswith('a/') else old)
    return wanted - created


def _fetch_archive():
    """Helium's prepared Chromium archive, through Helium's own downloader."""
    sys.path.insert(0, str(layout.HELIUM_UTILS))
    import downloads  # pylint: disable=import-outside-toplevel,import-error
    sys.path.pop(0)
    info = downloads.DownloadInfo([layout.HELIUM / 'downloads.ini'])
    layout.DOWNLOAD_CACHE.mkdir(parents=True, exist_ok=True)
    downloads.retrieve_downloads(info, layout.DOWNLOAD_CACHE, None, True)
    downloads.check_downloads(info, layout.DOWNLOAD_CACHE, None)


def extract(archive):
    if not archive.exists():
        _fetch_archive()
    wanted = _touched_files()
    print(f'{len(wanted)} files to extract from {archive.name}', flush=True)
    if _PRISTINE.exists():
        shutil.rmtree(_PRISTINE)
    _PRISTINE.mkdir(parents=True)
    found = set()
    mode = 'r:zst' if archive.suffix == '.zst' else 'r:*'
    with tarfile.open(archive, mode) as tar:
        for member in tar:
            if not member.isfile():
                continue
            rel = member.name.split('/', 1)[1] if '/' in member.name else member.name
            if rel not in wanted:
                continue
            out = _PRISTINE / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as src, open(out, 'wb') as dst:
                shutil.copyfileobj(src, dst)
            found.add(rel)
            if len(found) == len(wanted):
                break
    missing = wanted - found
    print(f'extracted {len(found)}; {len(missing)} absent from the archive')
    missing -= _extract_from_dependencies(missing)
    for rel in sorted(missing):
        print(f'  absent: {rel}')


def _extract_from_dependencies(missing):
    """Fill in files that come from Helium's pinned archive dependencies.

    Onboarding, uBlock Origin and WinSparkle are unpacked by the build, not
    shipped in the Chromium archive, but Helium's patches edit them.
    """
    parser = configparser.ConfigParser()
    parser.read([layout.HELIUM / 'deps.ini', layout.HELIUM_WINDOWS / 'downloads.ini',
                 layout.KRYPTON_DEPS],
                encoding='utf-8')
    found = set()
    for section in parser.sections():
        entry = parser[section]
        output = entry.get('output_path', '').strip('./') + '/'
        wanted = {rel for rel in missing if rel.startswith(output)}
        is_zip = entry['url'].endswith('.zip')
        is_tar = entry['url'].endswith(('.tar.gz', '.tar.xz', '.tar.bz2'))
        if not wanted or not (is_zip or is_tar):
            continue
        archive = layout.DOWNLOAD_CACHE / entry['download_filename']
        if not archive.exists():
            print(f'downloading {section} {entry["version"]}', flush=True)
            with urllib.request.urlopen(entry['url'], timeout=300) as response:
                archive.write_bytes(response.read())
        if hashlib.sha256(archive.read_bytes()).hexdigest() != entry['sha256']:
            archive.unlink()
            raise SystemExit(f'{archive.name} does not match the sha256 pinned in its ini')
        strip = entry.get('strip_leading_dirs', '').strip('/')
        if is_zip:
            with zipfile.ZipFile(archive) as bundle:
                for name in bundle.namelist():
                    inner = name[len(strip) + 1:] if strip and name.startswith(strip + '/') else name
                    rel = output + inner
                    if rel in wanted:
                        out = _PRISTINE / rel
                        out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_bytes(bundle.read(name))
                        found.add(rel)
        else:
            with tarfile.open(archive, 'r:*') as bundle:
                for member in bundle:
                    if not member.isfile():
                        continue
                    name = member.name.removeprefix('./')
                    inner = name[len(strip) + 1:] if strip and name.startswith(strip + '/') else name
                    rel = output + inner
                    if rel in wanted:
                        out = _PRISTINE / rel
                        out.parent.mkdir(parents=True, exist_ok=True)
                        with bundle.extractfile(member) as source:
                            out.write_bytes(source.read())
                        found.add(rel)
    print(f'extracted {len(found)} more from pinned dependencies')
    return found


def apply(patch_bin, layers):
    if not _PRISTINE.exists():
        raise SystemExit('No pristine subset yet; run "extract" first.')
    if _TREE.exists():
        shutil.rmtree(_TREE)
    shutil.copytree(_PRISTINE, _TREE)

    selected = [(name, path) for name, path in _all_patches() if name in layers]
    print(f'{len(selected)} patches from {", ".join(layers)}', flush=True)
    failures = []
    fuzzy = []
    for index, (name, path) in enumerate(selected, 1):
        result = subprocess.run([
            str(patch_bin), '-p1', '--ignore-whitespace', '-i', str(path), '-d',
            str(_TREE), '--no-backup-if-mismatch', '--forward'
        ], capture_output=True, text=True)
        rel = path.relative_to(layout.ROOT).as_posix()
        # A fuzzy match applies, but possibly in the wrong place; Krypton's
        # own patches should apply exactly.
        if name == 'krypton' and 'with fuzz' in result.stdout:
            fuzzy.append(rel)
            print(f'[FUZZ {index}/{len(selected)}] {rel}', flush=True)
            for line in result.stdout.splitlines():
                if 'with fuzz' in line:
                    print(f'    {line}', flush=True)
        if result.returncode == 0:
            continue
        failures.append(rel)
        print(f'[FAIL {index}/{len(selected)}] {rel}', flush=True)
        for line in (result.stdout + result.stderr).splitlines():
            if any(key in line for key in ('FAILED', "can't find file", 'No file to patch',
                                           'Reversed', 'malformed', 'garbage')):
                print(f'    {line}', flush=True)
    print(f'\n{len(failures)} failure(s) of {len(selected)}')
    if failures or fuzzy:
        return 1
    if check_references():
        return 1
    if 'krypton' in layers:
        # Reference checking covers the host patch subset. The complete official
        # payload also contains filter syntax and vendored CSS parsers, where
        # words from skipped UI patches are data rather than missing host names.
        official_ublock.stage(_TREE, layout.DOWNLOAD_CACHE)
    return 0


# Identifiers and file stems, dotted pref keys, hyphenated WebUI names.
_TOKEN = re.compile(r'(?<![\w.-])(?:[A-Za-z_][A-Za-z0-9_]{5,}|[a-z]+(?:\.[a-z_]+){2,}'
                    r'|[a-z]+(?:-[a-z0-9]+){2,})(?![\w-])')


def _added_lines(path):
    """(file, line) for every line a patch adds, and the files it creates."""
    added, created, current, previous = [], set(), None, ''
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if line.startswith('+++ '):
            current = line[4:].split('\t')[0].removeprefix('b/')
            if previous.startswith('--- /dev/null'):
                created.add(current)
        elif line.startswith('+') and current and not current.endswith('.icon'):
            added.append((current, line[1:]))
        previous = line
    return added, created


def _defines(line, token):
    """Whether an added line looks like the definition of `token`."""
    t = re.escape(token)
    return bool(re.search(
        rf'\b(?:class|struct|enum(?:\s+class)?|namespace)\s+{t}\b'
        rf'|#define\s+{t}\b|\b{t}\s*(?:\[\])?\s*=[^=]|^\s*{t},?\s*(?://.*)?$'
        rf'|^\s*[\w:<>,*&]+[\s*&]+{t}\s*\(|"{t}"', line))


def check_references():
    """Name anything only a skipped upstream patch adds that the tree still uses.

    Skipping a patch removes what it defines. A kept patch that still names
    one of those (a pref, a command ID, a header) applies cleanly and then
    fails to compile an hour into the build, so look for them here. Hits are
    for review: a name can also be Chromium's own from outside the subset.
    """
    skipped = []
    for name, patch_dir in layout.PATCH_LAYERS:
        omitted = layout.skipped(name)
        skipped.extend(p for p in layout.parse_series(patch_dir)
                       if p.relative_to(patch_dir).as_posix() in omitted)
    if not skipped:
        return

    def tokens_of(directory):
        found = {}
        for path in directory.rglob('*'):
            if path.is_file() and path.suffix not in ('.rej', '.orig', '.png', '.ico', '.icon'):
                text = path.read_text(encoding='utf-8', errors='replace')
                if path.suffix in ('.grd', '.grdp', '.xtb'):
                    # Resource prose is not a host symbol reference. Keep XML
                    # attributes (message IDs, conditions and include paths).
                    text = re.sub(r'<!--[\s\S]*?-->', '', text)
                    text = re.sub(r'\bdesc="[^"]*"', '', text)
                    text = re.sub(r'>[^<]+<', '><', text)
                for token in _TOKEN.findall(text):
                    found.setdefault(token, set()).add(path.relative_to(directory).as_posix())
        return found

    pristine = tokens_of(_PRISTINE)
    introduced = {}
    for path in skipped:
        for _, line in _added_lines(path)[0]:
            for token in _TOKEN.findall(line):
                if token not in pristine:
                    introduced.setdefault(token, set()).add(path.name)

    kept_defined = set()
    for _, path in _all_patches():
        added, created = _added_lines(path)
        for new_file in map(Path, created):
            kept_defined.add(new_file.stem)
            kept_defined.update(new_file.parent.parts)
        for _, line in added:
            kept_defined.update(t for t in _TOKEN.findall(line)
                                if t in introduced and _defines(line, t))

    remaining = tokens_of(_TREE)
    hits = sorted(t for t in introduced if t in remaining and t not in kept_defined)
    for token in hits:
        print(f'[REF] {token}: added by {", ".join(sorted(introduced[token]))}, '
              f'still named in {", ".join(sorted(remaining[token]))}', flush=True)
    print(f'{len(hits)} name(s) from {len(skipped)} skipped patch(es) still in the tree')
    return len(hits)


def main():
    global _PRISTINE, _TREE
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work-dir', type=Path, default=layout.VALIDATION_DIR,
                        help='separate validation workspace (never the active source tree)')
    sub = parser.add_subparsers(dest='command', required=True)
    ext = sub.add_parser('extract', help='extract the touched files from the archive')
    ext.add_argument('--archive', type=Path,
                     default=layout.DOWNLOAD_CACHE /
                     f'chromium-{layout.chromium_version()}-base.tar.zst')
    app = sub.add_parser('apply', help='apply the layers to a fresh copy of the subset')
    app.add_argument('--patch', type=Path, default=_DEFAULT_PATCH)
    app.add_argument('--layers', nargs='+', default=[n for n, _ in layout.PATCH_LAYERS],
                     choices=[n for n, _ in layout.PATCH_LAYERS])
    args = parser.parse_args()
    try:
        layout.check_core_ref()
    except ValueError as error:
        parser.error(str(error))
    work_dir = args.work_dir.resolve()
    tree = work_dir / 'src'
    active = layout.SOURCE_TREE.resolve()
    if tree.is_relative_to(active) or active.is_relative_to(tree):
        parser.error('validation workspace must not overlap the active source tree')
    _PRISTINE, _TREE = work_dir / 'pristine', tree
    if args.command == 'extract':
        extract(args.archive)
        return 0
    return apply(args.patch, args.layers)


if __name__ == '__main__':
    sys.exit(main())
