#!/usr/bin/env python3
# Copyright 2026 The Krypton Authors. GPL-3.0.
"""Materialize pinned streaming sources and the selected official CPU runtime.

This explicit development command does not install/download voice models, run
inference, configure Chromium or write to a build graph. Native builds use these
verified inputs offline. Runtime packs have a separate, user-consented installer.
"""
import argparse
import configparser
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    with path.open('rb') as data:
        return hashlib.file_digest(data, 'sha256').hexdigest()


def relative(name):
    path = PurePosixPath(name)
    if not path.parts or path.is_absolute() or '..' in path.parts or ':' in name:
        raise ValueError('Invalid pinned relative path')
    return Path(*path.parts)


def download(url, destination, size, sha256):
    if destination.exists():
        if destination.stat().st_size != size or digest(destination) != sha256:
            raise ValueError('Existing dependency hash mismatch: ' + destination.name)
        return
    temporary = destination.with_name(destination.name + '.partial')
    with urllib.request.urlopen(url, timeout=60) as response, temporary.open('xb') as output:
        received = 0
        while data := response.read(1024 * 1024):
            received += len(data)
            if received > size:
                raise ValueError('Dependency exceeded pinned size')
            output.write(data)
    if temporary.stat().st_size != size or digest(temporary) != sha256:
        raise ValueError('Dependency hash mismatch: ' + destination.name)
    temporary.replace(destination)


def materialize(destination, patch):
    if destination.exists():
        raise ValueError('Use a new source-input directory, never an existing graph')
    source_manifest = ROOT / 'voice_engine/source-manifest.json'
    pins = json.loads(source_manifest.read_text(encoding='utf8'))
    declared = (ROOT / 'deps.ini').read_text(encoding='utf8')
    if ('# commit = ' + pins['commit'] not in declared or
            '# source_manifest_sha256 = ' + digest(source_manifest) not in declared):
        raise ValueError('Moonshine source pins disagree with deps.ini')
    destination.mkdir(parents=True)
    source = destination / 'moonshine'
    for pin in pins['files']:
        target = source / relative(pin['path'])
        if pin['size'] > 4 * 1024 * 1024:
            raise ValueError('Pinned source file exceeds source-only limit')
        target.parent.mkdir(parents=True, exist_ok=True)
        download('https://raw.githubusercontent.com/moonshine-ai/moonshine/' +
                 pins['commit'] + '/' + pin['path'], target, pin['size'], pin['sha256'])
    subprocess.run([str(patch), '-p1', '--batch', '--fuzz=0', '-i',
                    str(ROOT / 'voice_engine/patches/native-sandbox.patch')],
                   cwd=source, check=True)
    ort = json.loads((ROOT / 'voice_engine/official-ort-manifest.json').read_text())
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'deps.ini', encoding='utf8')
    section = config['onnxruntime_cpu_official']
    if section['url'] != ort['url'] or section['sha256'] != ort['sha256']:
        raise ValueError('Official ONNX Runtime pins disagree with deps.ini')
    cache = ROOT / 'build/download_cache'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / section['download_filename']
    download(ort['url'], archive, ort['size'], ort['sha256'])
    expected = {pin['path']: pin for pin in ort['files']}
    output = destination / 'official-ort'
    with zipfile.ZipFile(archive) as bundle:
        for entry in bundle.infolist():
            if entry.filename not in expected:
                continue
            pin = expected[entry.filename]
            data = bundle.read(entry)
            if len(data) != pin['size'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
                raise ValueError('Official package member hash mismatch')
            target = output / relative(entry.filename)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    print('Pinned native inputs prepared. No model installation or build ran.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--patch', type=Path, default=shutil.which('patch'))
    args = parser.parse_args()
    if not args.patch:
        parser.error('Specify the existing patch executable with --patch')
    materialize(args.destination.resolve(), args.patch)
