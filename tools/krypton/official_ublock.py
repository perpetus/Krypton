"""Stage the pinned official release after upstream's component setup patches."""

import configparser
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import urllib.request
import zipfile

from krypton import layout

# Public key from the signed Chromium CRX distributed with the official release.
# This is the release identity, distinct from both the fork and the Web Store.
PUBLIC_KEY = 'MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsgdkJHEX8xHAytYy3Rihqn5FoU/cbPKhoorkCCsgF8HR2y2OSWGM1Ojrmnr0ebgM9WA1pl1hr1CmOH7DjgQVKRhzBjK7/Zb6RVJNPVGEQvV9CdCUwOTKsu1qQGRbjm9Z/DkYxgu6B2sLo0ZpQ/IBsmBvs+FGR4CqrWra8GZPwn7n3FibeoxcArWiAx85N2Oyiaef2Geytoog4hS+I5Fs3ymKkEeTYM3tzeC0U5nZ010LCnlQe0cQ3UDOro8VzLosuhaxAsrPFErIOfIUfvV3sNhQJrySqgii9Xv6RWT8TI3pHL1yjevKKTxNb2VbPlTOi5MyzPowWV8hHJEOkwq2dQIDAQAB'


def stage(source_tree, download_cache):
    """Replace the entire runtime payload; preserve only native build metadata."""
    config = configparser.ConfigParser()
    config.read(layout.KRYPTON_DEPS, encoding='utf-8')
    release = config['ublock_origin_official']
    archive = Path(download_cache) / release['download_filename']
    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        request = urllib.request.Request(release['url'], headers={'User-Agent': 'Krypton-build'})
        with urllib.request.urlopen(request, timeout=300) as response:
            payload = response.read()
    else:
        payload = archive.read_bytes()
    if hashlib.sha256(payload).hexdigest() != release['sha256']:
        raise ValueError(f'{archive.name} does not match the pinned official release SHA-256')
    if not archive.exists():
        archive.write_bytes(payload)

    third_party = Path(source_tree).resolve() / 'third_party'
    third_party.mkdir(parents=True, exist_ok=True)
    target = third_party / 'ublock'
    # Never follow a generated-tree symlink outside the source tree on replacement.
    if third_party.resolve() != third_party or target.resolve() != target:
        raise ValueError('uBlock staging paths must stay inside the source tree')

    with tempfile.TemporaryDirectory(prefix='.official-ublock-', dir=third_party) as directory:
        staged = Path(directory)
        prefix = release['strip_leading_dirs'] + '/'
        with zipfile.ZipFile(io.BytesIO(payload)) as bundle:
            for member in bundle.infolist():
                if member.is_dir():
                    continue
                if not member.filename.startswith(prefix):
                    raise ValueError(f'Unexpected official release path: {member.filename}')
                relative = PurePosixPath(member.filename[len(prefix):])
                if relative.is_absolute() or '..' in relative.parts or '\\' in str(relative) or ':' in str(relative):
                    raise ValueError(f'Unsafe official release path: {member.filename}')
                destination = staged / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(bundle.read(member))

        manifest_path = staged / 'manifest.json'
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        if manifest['version'] != release['version']:
            raise ValueError('Official uBlock manifest version does not match deps.ini')
        manifest['key'] = PUBLIC_KEY
        manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + '\n', encoding='utf-8')
        for name in ('BUILD.gn', 'generate_file_list.py', 'README.chromium'):
            shutil.copy2(layout.KRYPTON_CHROMIUM_SRC / 'third_party/ublock' / name, staged / name)

        if target.exists():
            shutil.rmtree(target)
        staged.rename(target)

    # The upstream unpacker stages this second archive too. It is only an input,
    # and must not leave a duplicate extension tree in the prepared sources.
    unpacked = third_party / 'ublock_official'
    if unpacked.exists():
        if unpacked.resolve() != unpacked:
            raise ValueError('Official uBlock unpack path must stay inside the source tree')
        shutil.rmtree(unpacked)
    print(f'Staged official uBlock Origin {release["version"]} (release signing identity).')
