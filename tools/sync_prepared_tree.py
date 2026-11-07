#!/usr/bin/env python3
"""Install a separately prepared source upgrade without touching build outputs.

Only files in the old pinned archives, or the complete bundled blocker payload,
are candidates for removal. Unchanged files retain their timestamps; changed
inputs receive the time of installation so Ninja invalidates generated outputs.
The top-level out/ tree is never copied or removed; nested package out/
directories are inputs.
"""

import argparse
import filecmp
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tarfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from krypton import layout


def inventory(tree):
    """Do not follow links; only the source root's out/ is protected."""
    files = {}
    pending = [tree]
    while pending:
        directory = pending.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                relative = Path(entry.path).relative_to(tree)
                if relative.parts[0] == 'out':
                    continue
                if entry.is_symlink():
                    attributes = entry.stat(follow_symlinks=False).st_file_attributes
                    files[relative.as_posix()] = ('link', bool(attributes & stat.FILE_ATTRIBUTE_DIRECTORY))
                elif entry.is_dir(follow_symlinks=False):
                    if os.path.isjunction(entry.path):
                        raise ValueError(f'Unexpected source junction: {entry.path}')
                    pending.append(Path(entry.path))
                elif entry.is_file(follow_symlinks=False):
                    files[relative.as_posix()] = ('file', False)
    return files


def matching(source, destination, kind):
    if kind == 'link':
        return destination.is_symlink() and os.readlink(source) == os.readlink(destination)
    return (destination.is_file() and not destination.is_symlink() and
            filecmp.cmp(source, destination, shallow=False))


def digest(path):
    with path.open('rb') as content:
        return hashlib.file_digest(content, 'sha256').hexdigest()


def old_archive_files(archives, cache=None):
    identity = {str(path.resolve()): [path.stat().st_size, path.stat().st_mtime_ns]
                for path in archives}
    if cache is not None and cache.is_file():
        saved = json.loads(cache.read_text(encoding='utf-8'))
        if saved['archives'] != identity:
            raise ValueError('Old archive inputs changed since the cleanup manifest was saved')
        return set(saved['files'])
    wanted = set()
    for archive in archives:
        if not archive.is_file():
            raise ValueError(f'Missing old source manifest archive: {archive}')
        with tarfile.open(archive, 'r|zst', bufsize=1024 * 1024, stream=True) as bundle:
            for member in bundle:
                if not (member.isfile() or member.issym() or member.islnk()):
                    continue
                name = PurePosixPath(member.name)
                relative = PurePosixPath(*name.parts[1:])
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError(f'Unsafe old archive path: {member.name}')
                if relative.parts and relative.parts[0] != 'out':
                    wanted.add(relative.as_posix())
        print(f'Loaded source manifest: {archive.name} ({len(wanted)} paths).', flush=True)
    if cache is not None:
        cache.write_text(json.dumps({'archives': identity, 'files': sorted(wanted)}), encoding='utf-8')
    return wanted


def sync(prepared, active, old_files, report_dir):
    prepared, active = prepared.resolve(), active.resolve()
    allowed = layout.BUILD_DIR.resolve()
    if not prepared.is_relative_to(allowed) or not active.is_relative_to(allowed):
        raise ValueError('Both source trees must be inside this repository build directory')
    if prepared.is_relative_to(active) or active.is_relative_to(prepared):
        raise ValueError('Prepared and active source trees must not overlap')
    report_dir = report_dir.resolve()
    if not report_dir.is_relative_to(allowed) or report_dir.is_relative_to(active):
        raise ValueError('Sync reports must be outside the active source tree, under build/')
    report_dir.mkdir(parents=True, exist_ok=True)
    protected = {}
    for name in ('.ninja_log', '.ninja_deps'):
        path = active / 'out/Default' / name
        if not path.is_file():
            raise ValueError(f'Missing existing incremental tracking: {path}')
        protected[name] = digest(path)
    backup = report_dir / 'previous-build-state'
    if backup.exists():
        saved = json.loads((report_dir / 'tracking-before.json').read_text(encoding='utf-8'))
        if saved != protected or any(digest(backup / name) != value for name, value in saved.items()):
            raise ValueError('Existing backup differs from active tracking; refusing to resume')
    else:
        backup.mkdir()
        for name in ('.ninja_log', '.ninja_deps', 'args.gn', 'build.ninja',
                     'build.ninja.d', 'build.ninja.stamp', 'toolchain.ninja'):
            path = active / 'out/Default' / name
            if path.is_file():
                shutil.copy2(path, backup / name)
        state = layout.BUILD_DIR / '.krypton_state.json'
        if state.is_file():
            shutil.copy2(state, backup / 'preparation-state.json')
        (report_dir / 'tracking-before.json').write_text(json.dumps(protected, indent=2), encoding='utf-8')
    files = inventory(prepared)
    (report_dir / 'prepared-files.json').write_text(json.dumps(files), encoding='utf-8')
    counts = {'unchanged': 0, 'copied': 0, 'removed': 0, 'links': 0}
    checked_parents = {active}
    with (report_dir / 'source-changes.jsonl').open('w', encoding='utf-8') as changes:
        for index, (name, (kind, is_directory)) in enumerate(files.items(), 1):
            source, destination = prepared / name, active / name
            # Do not traverse an existing output-tree link when replacing a file.
            parent = destination.parent
            visited = []
            while parent not in checked_parents:
                if parent.is_symlink() or os.path.isjunction(parent):
                    raise ValueError(f'Active source parent is a link: {parent}')
                visited.append(parent)
                parent = parent.parent
            checked_parents.update(visited)
            if matching(source, destination, kind):
                counts['unchanged'] += 1
            else:
                destination.parent.mkdir(parents=True, exist_ok=True)
                if destination.is_symlink():
                    destination.unlink()
                elif destination.exists() and not destination.is_file():
                    raise ValueError(f'Source file collides with directory: {destination}')
                if kind == 'link':
                    if destination.exists():
                        destination.unlink()
                    os.symlink(os.readlink(source), destination, target_is_directory=is_directory)
                    counts['links'] += 1
                else:
                    # Archive timestamps can predate existing generated files.
                    # Changed inputs must carry their actual installation time;
                    # preserving the stage's mtime can hide required rebuilds.
                    shutil.copyfile(source, destination)
                    shutil.copymode(source, destination)
                    counts['copied'] += 1
                changes.write(json.dumps({'copy': name}) + '\n')
            if index % 50000 == 0:
                print(f'Compared {index}/{len(files)} source files; copied {counts["copied"]}.', flush=True)

        previous_payload = active / 'third_party/ublock'
        if previous_payload.exists():
            old_files.update('third_party/ublock/' + name for name in inventory(previous_payload))
        removed_parents = set()
        for name in sorted(old_files - files.keys()):
            relative = PurePosixPath(name)
            if relative.is_absolute() or '..' in relative.parts or not relative.parts or relative.parts[0] == 'out':
                raise ValueError(f'Refusing unsafe obsolete path: {name}')
            destination = active / name
            parent = destination.parent
            while parent != active:
                if parent.is_symlink() or os.path.isjunction(parent):
                    raise ValueError(f'Obsolete source parent is a link: {parent}')
                parent = parent.parent
            if destination.is_symlink() or destination.is_file():
                destination.unlink()
                removed_parents.add(destination.parent)
                changes.write(json.dumps({'remove': name}) + '\n')
                counts['removed'] += 1
        for directory in sorted(removed_parents, key=lambda p: len(p.parts), reverse=True):
            while directory != active:
                try:
                    directory.rmdir()
                except OSError:
                    break
                directory = directory.parent

    # Verify every staged source, including package-nested out/ directories.
    filecmp.clear_cache()
    for name, (kind, is_directory) in files.items():
        if not matching(prepared / name, active / name, kind):
            raise ValueError(f'Source verification mismatch after sync: {name}')
    after = {name: digest(active / 'out/Default' / name) for name in protected}
    if after != protected:
        raise ValueError('Incremental tracking changed during source synchronization')
    summary = {'prepared': str(prepared), 'active': str(active), 'files': len(files),
               'counts': counts, 'tracking_sha256': after, 'verified': True}
    (report_dir / 'sync-summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--prepared-state', required=True, type=Path)
    parser.add_argument('--old-archive', required=True, type=Path, action='append')
    parser.add_argument('--report-dir', required=True, type=Path)
    args = parser.parse_args()
    layout.check_core_ref()
    spec = importlib.util.spec_from_file_location('krypton_build', layout.ROOT / 'build.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    state = json.loads(args.prepared_state.read_text(encoding='utf-8'))
    if state['prepare'] != build._prepare_fingerprint():
        parser.error('prepared inputs changed; verify the stage and refresh its checkpoint first')
    print('Loading old archive manifests for bounded obsolete-file cleanup.', flush=True)
    report = args.report_dir.resolve()
    if not report.is_relative_to(layout.BUILD_DIR.resolve()) or report.is_relative_to(layout.SOURCE_TREE.resolve()):
        parser.error('sync reports must be outside the active source tree, under build/')
    report.mkdir(parents=True, exist_ok=True)
    obsolete = old_archive_files(args.old_archive, report / 'old-source-manifest.json')
    sync(args.prepared, layout.SOURCE_TREE, obsolete, args.report_dir)
    # A valid checkpoint is written only after source and tracking verification.
    build._write_state(prepare=state['prepare'])


if __name__ == '__main__':
    main()
