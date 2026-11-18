# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Upload matching browser/UI PE and PDB files without source bundles or secrets."""

import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[2]
REQUIRED = ('chrome.dll', 'chrome.exe', 'chrome_elf.dll')
OPTIONAL = ('libEGL.dll', 'libGLESv2.dll', 'dxcompiler.dll', 'chrome_wer.dll',
            'vulkan-1.dll', 'notification_helper.exe', 'chrome_proxy.exe')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def call(cli, arguments, context):
    environment = os.environ.copy()
    environment.update(SENTRY_ORG=context['org'], SENTRY_PROJECT=context['project'],
                       SENTRY_URL=context['url'])
    result = subprocess.run([cli, *arguments], cwd=ROOT, env=environment,
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'Sentry command failed ({result.returncode}): '
                           f'{result.stderr.strip()}')
    return json.loads(result.stdout)


def upload_native(native_cli, cli, paths, context, log_path):
    # Obtain CLI-managed credentials only in memory. Never put them in command
    # arguments, reports, logs or a persistent legacy CLI configuration.
    credentials = subprocess.run([cli, 'auth', 'token'], cwd=ROOT,
                                 capture_output=True, text=True)
    if credentials.returncode or not credentials.stdout.strip():
        raise RuntimeError('Unable to obtain CLI-managed Sentry credentials.')
    environment = os.environ.copy()
    environment.update(SENTRY_AUTH_TOKEN=credentials.stdout.strip(),
                       SENTRY_ORG=context['org'], SENTRY_PROJECT=context['project'],
                       SENTRY_URL=context['url'], SENTRY_LOG_LEVEL='info',
                       SENTRY_DISABLE_UPDATE_CHECK='1')
    credentials = None
    try:
        with log_path.open('w', encoding='utf-8') as log:
            result = subprocess.run([str(native_cli), 'debug-files', 'upload',
                                     '--type', 'pdb', '--type', 'pe', '--no-sources',
                                     '--no-zips', '--wait', *paths], cwd=ROOT,
                                    env=environment, stdout=log, stderr=subprocess.STDOUT)
    finally:
        environment.pop('SENTRY_AUTH_TOKEN', None)
    if result.returncode:
        raise RuntimeError(f'Native Sentry uploader failed ({result.returncode}); '
                           f'see {log_path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser-dir', type=Path, default=ROOT / 'build/src/out/Default')
    parser.add_argument('--ui-dir', type=Path,
                        default=ROOT.parent / 'KryptonUI/out/build/release-max/bin/Release')
    parser.add_argument('--dry-run', action='store_true', help='Validate only; upload nothing')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--native-cli', type=Path,
                        help='Use a verified native sentry-cli for large-file upload')
    parser.add_argument('--org', help='Override the public CLI organization default')
    parser.add_argument('--project', help='Override the public CLI project default')
    parser.add_argument('--url', help='Override the Sentry instance URL for self-hosting')
    args = parser.parse_args()
    config = configparser.ConfigParser()
    config.read(ROOT / '.sentryclirc', encoding='utf-8')
    context = {name: (getattr(args, name) or config.get('defaults', name)).rstrip('/')
               for name in ('org', 'project', 'url')}
    cli = shutil.which('sentry')
    if not cli:
        raise RuntimeError('Install and authenticate the Sentry CLI before uploading symbols.')
    pairs = []
    omitted = []
    for name in REQUIRED + OPTIONAL:
        binary = args.browser_dir / name
        pdb = args.browser_dir / (name + '.pdb')
        if not binary.is_file() or not pdb.is_file():
            if name in REQUIRED:
                raise RuntimeError(f'Missing required browser binary/PDB pair: {name}')
            omitted.append(name)
            continue
        pairs.append((binary, pdb))
    ui = args.ui_dir / 'krypton_ui.dll'
    ui_pdb = args.ui_dir / 'krypton_ui.pdb'
    if not ui.is_file() or not ui_pdb.is_file():
        raise RuntimeError('Missing optimized UI symbols; build kr_shell with '
                           'KR_RELEASE_MAX_SYMBOLS=ON before uploading.')
    if digest(ui) != digest(args.browser_dir / ui.name):
        raise RuntimeError('The staged UI differs from the UI paired with this PDB.')
    pairs.append((ui, ui_pdb))
    # Explicit PE/PDB paths only. Never scan the checkout, tests, installer
    # artifacts, dependency caches or internal documents, and never add sources.
    # The CLI's scan can omit a large matching PDB after inspecting its PE.
    # Present symbols first so the complete pair reaches verification/upload.
    paths = [str(path.resolve()) for binary, pdb in pairs for path in (pdb, binary)]
    scan = call(cli, ['debug-files', 'upload', '--no-upload', '--json', *paths], context)
    files = {entry['name'].lower(): entry for entry in scan['files']}
    checked = []
    for binary, pdb in pairs:
        binary_info = files.get(binary.name.lower())
        pdb_info = files.get(pdb.name.lower())
        if not binary_info or not pdb_info:
            raise RuntimeError(f'Sentry scan omitted a binary/PDB pair: {binary.name}')
        if binary_info['debugId'] != pdb_info['debugId']:
            raise RuntimeError(f'Binary/PDB debug identifiers do not match: {binary.name}')
        checked.append({'binary': str(binary), 'pdb': str(pdb),
                        'debug_id': binary_info['debugId'],
                        'binary_sha256': digest(binary), 'pdb_sha256': digest(pdb)})
    report = {'uploaded': False, 'pairs': checked, 'omitted_optional_modules': omitted,
              'source_bundles_included': False, 'sentry': context}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    if not args.dry_run:
        auth = call(cli, ['auth', 'status', '--fresh', '--json'], context)
        organizations = auth.get('verification', {}).get('organizations', [])
        if (not auth.get('authenticated') or auth.get('host', '').rstrip('/') != context['url']
                or not any(org['slug'] == context['org'] for org in organizations)):
            raise RuntimeError('Authenticated Sentry context does not match the requested target.')
        # .sentryclirc contains only public defaults. Use the native uploader
        # when the JavaScript CLI cannot handle the large Chromium PDB.
        if args.native_cli:
            if not args.native_cli.is_file() or not args.report:
                raise RuntimeError('Native upload requires an existing CLI and --report.')
            log_path = args.report.with_suffix('.native.log')
            upload_native(args.native_cli, cli, paths, context, log_path)
            report.update(uploaded=True, uploader=str(args.native_cli),
                          upload_log=str(log_path))
        else:
            response = call(cli, ['debug-files', 'upload', '--wait', '--json', *paths], context)
            report.update(uploaded=response.get('uploaded', False),
                          files_uploaded=response.get('filesUploaded'),
                          uploaded_files=response.get('files', []))
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
