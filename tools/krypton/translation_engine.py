#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors. GPL-3.0.
"""Materialize the hash-pinned inference stack, without running its build.

The native engine is built separately from Chromium. No binary downloads,
recursive Git commands, package managers or network access occur during builds.
"""
import argparse
import configparser
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]

def materialize(destination):
    pins = json.loads((ROOT / 'translation_engine/source-pins.json').read_text())
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'deps.ini')
    cache = ROOT / 'build/download_cache'
    cache.mkdir(parents=True, exist_ok=True)
    destination.mkdir(parents=True, exist_ok=False)
    for pin in pins:
        declared = config['bergamot_' + pin['name']]
        if any(declared[key] != pin[key] for key in ('url', 'sha256')):
            raise ValueError('Source pins and deps.ini disagree')
        archive = cache / declared['download_filename']
        if not archive.exists():
            temporary = archive.with_suffix('.partial')
            with urllib.request.urlopen(pin['url'], timeout=60) as response, temporary.open('wb') as out:
                size = 0
                while data := response.read(1024 * 1024):
                    size += len(data)
                    if size > 32 * 1024 * 1024:
                        raise ValueError('Engine archive exceeds size cap')
                    out.write(data)
            temporary.replace(archive)
        with archive.open('rb') as data:
            if hashlib.file_digest(data, 'sha256').hexdigest() != pin['sha256']:
                raise ValueError('Engine source hash mismatch: ' + pin['name'])
        target = destination / pin['path']
        with zipfile.ZipFile(archive) as bundle:
            for entry in bundle.infolist():
                parts = PurePosixPath(entry.filename).parts
                if len(parts) < 2:
                    continue
                relative = PurePosixPath(*parts[1:])
                if relative.is_absolute() or '..' in relative.parts or any(':' in p for p in relative.parts):
                    raise ValueError('Unsafe engine archive path')
                # Training data, orchestration and unrelated submodules are not
                # shipped with the inference component.
                if pin['name'] == 'translations' and relative.parts[0] not in ('inference', 'LICENSE'):
                    continue
                path = target.joinpath(*relative.parts)
                if entry.is_dir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(bundle.read(entry))
    for name in ('native-sandbox.patch', 'vendored-iterator-warnings.patch'):
        subprocess.run(['patch', '-p1', '--batch', '--fuzz=0', '-i',
                        str(ROOT / 'translation_engine/patches' / name)],
                       cwd=destination, check=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    materialize(args.destination.resolve())
