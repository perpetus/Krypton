#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright 2026 The Krypton Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
#
# Derived from upstream/helium-windows/build.py:
# Copyright 2025 The Helium Authors
# Copyright (c) 2019 The ungoogled-chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
Krypton build script for Windows

This is helium-windows' build.py, step for step, with Krypton's layer added at
fixed points. Every addition is marked "Krypton:"; everything else should read
the same as upstream/helium-windows/build.py, so an upstream change can be
carried over by comparing the two files (tools/sync_helium.py shows the diff).
"""

import sys
import time
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import ctypes
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
from krypton import layout  # pylint: disable=wrong-import-position
from krypton import ui_runtime  # pylint: disable=wrong-import-position
from krypton import branding  # Krypton: final user-visible name pass.
from krypton import official_ublock  # Krypton: replace the fork runtime payload.
from krypton import build_jobs  # Krypton: configurable native build parallelism.
sys.path.pop(0)

sys.path.insert(0, str(layout.HELIUM_UTILS))
import downloads
import domain_substitution
import i18n_apply
import name_substitution
import helium_version
import generate_resources
import replace_resources
import prune_binaries
import patches
from _common import ENCODING, USE_REGISTRY, ExtractorEnum, get_logger
sys.path.pop(0)

_PATCH_BIN_RELPATH = Path('third_party/git/usr/bin/patch.exe')
_STATE_PATH = layout.BUILD_DIR / '.krypton_state.json'


def _get_vcvars_path(name='64'):
    """
    Returns the path to the corresponding vcvars*.bat path

    As of VS 2017, name can be one of: 32, 64, all, amd64_x86, x86_amd64
    """
    vswhere_exe = '%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer\\vswhere.exe'
    result = subprocess.run(
        '"{}" -products * -prerelease -latest -property installationPath'.format(vswhere_exe),
        shell=True,
        check=True,
        stdout=subprocess.PIPE,
        universal_newlines=True)
    vcvars_path = Path(result.stdout.strip(), 'VC/Auxiliary/Build/vcvars{}.bat'.format(name))
    if not vcvars_path.exists():
        raise RuntimeError(
            'Could not find vcvars batch script in expected location: {}'.format(vcvars_path))
    return vcvars_path


# Krypton: VS 2026's developer scripts invoke vswhere by name. Keep their
# installer directory available without changing the user's global PATH.
def _build_environment():
    environment = os.environ.copy()
    installer = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer'
    environment['PATH'] = str(installer) + os.pathsep + environment.get('PATH', '')
    return environment


def _run_build_process(*args, **kwargs):
    """
    Runs the subprocess with the correct environment variables for building
    """
    # Add call to set VC variables
    cmd_input = ['call "%s" >nul || exit /b 1' % _get_vcvars_path()]
    cmd_input.append('set DEPOT_TOOLS_WIN_TOOLCHAIN=0')
    cmd_input.append(' '.join(map('"{}"'.format, args)))
    cmd_input.append('exit\n')
    subprocess.run(('cmd.exe', '/k'),
                   input='\n'.join(cmd_input),
                   check=True,
                   encoding=ENCODING,
                   env=_build_environment(),
                   **kwargs)


def _run_build_process_timeout(*args, timeout):
    """
    Runs the subprocess with the correct environment variables for building
    """
    # Add call to set VC variables
    cmd_input = ['call "%s" >nul || exit /b 1' % _get_vcvars_path()]
    cmd_input.append('set DEPOT_TOOLS_WIN_TOOLCHAIN=0')
    cmd_input.append(' '.join(map('"{}"'.format, args)))
    cmd_input.append('exit\n')
    with subprocess.Popen(('cmd.exe', '/k'), encoding=ENCODING, stdin=subprocess.PIPE, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP, env=_build_environment()) as proc:
        proc.stdin.write('\n'.join(cmd_input))
        proc.stdin.close()
        try:
            proc.wait(timeout)
            if proc.returncode != 0:
                raise RuntimeError('Build failed!')
        except subprocess.TimeoutExpired:
            print('Sending keyboard interrupt')
            for _ in range(3):
                ctypes.windll.kernel32.GenerateConsoleCtrlEvent(1, proc.pid)
                time.sleep(1)
            try:
                proc.wait(10)
            except:
                proc.kill()
            sys.exit(42)


def _make_tmp_paths():
    """Creates TMP and TEMP variable dirs so ninja won't fail"""
    tmp_path = Path(os.environ['TMP'])
    if not tmp_path.exists():
        tmp_path.mkdir()
    tmp_path = Path(os.environ['TEMP'])
    if not tmp_path.exists():
        tmp_path.mkdir()


def _configure_remoteexec(source_tree):
    address = os.environ.get('SISO_REAPI_ADDRESS', '')
    instance = os.environ.get('SISO_REAPI_INSTANCE') or 'main'
    backend = ''
    if address:
        os.environ['SISO_REAPI_INSTANCE'] = instance
        backend = 'nativelink.star'

    subprocess.run([
        sys.executable, str(source_tree / 'build/config/siso/configure_siso.py'),
        '--rbe_instance=projects/rbe-chrome-untrusted/instances/default_instance',
        f'--reapi_address={address}',
        f'--reapi_instance={instance if address else ""}',
        f'--reapi_backend_config_path={backend}',
    ], check=True)


# Krypton: the layer applied on top of Helium -------------------------------


def _prepare_fingerprint():
    """Identity of everything the prepare stage consumes.

    Covers all three layers, so moving the upstream submodules invalidates a
    prepared tree just as editing a Krypton patch does.
    """
    digest = hashlib.sha256()
    inputs = [Path(__file__), Path(build_jobs.__file__), layout.KRYPTON_DEPS,
              Path(official_ublock.__file__),
              layout.HELIUM_CORE_REF, layout.WINDOWS_PATCH_OVERRIDES]
    for name, patch_dir in layout.PATCH_LAYERS:
        inputs.append(patch_dir / 'series')
        inputs.extend(layout.layer_patches(name))
    inputs.extend(layout.SKIP_LISTS.values())
    for directory in (layout.KRYPTON_CHROMIUM_SRC, layout.KRYPTON_RESOURCES,
                      layout.HELIUM / 'resources', layout.HELIUM_WINDOWS / 'resources'):
        inputs.extend(p for p in directory.rglob('*') if p.is_file())
    inputs.extend((layout.HELIUM / 'chromium_version.txt', layout.HELIUM / 'revision.txt',
                   layout.HELIUM_WINDOWS / 'revision.txt'))
    for path in sorted(inputs, key=lambda p: p.as_posix().lower()):
        digest.update(path.relative_to(layout.ROOT).as_posix().encode() + b'\0')
        digest.update(path.read_bytes() if path.is_file() else b'<missing>')
        digest.update(b'\0')
    return digest.hexdigest()


def _read_state(path=_STATE_PATH):
    try:
        return json.loads(path.read_text(encoding=ENCODING))
    except (OSError, ValueError):
        return {}


def _write_state(path=_STATE_PATH, **values):
    state = _read_state(path)
    state.update(values, updated=datetime.datetime.now().isoformat(timespec='seconds'))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2), encoding=ENCODING)


def _refuse_to_discard(source_tree):
    """Stop before a prepare unpacks over a tree that already has a build.

    Upstream's prepare unpacks and patches in place, so on an existing tree
    every patch fails as already applied, after the hour spent on a build.
    """
    if not (source_tree / 'BUILD.gn').exists():
        return
    print('\n'.join([
        '',
        'Refusing to prepare: ' + str(source_tree) + ' already holds a prepared tree.',
        '',
        '  To reuse it if nothing it was prepared from has changed:',
        '    python build.py --resume',
        '  To discard it, including any compiled output, and start over:',
        '    python build.py --overwrite',
        '',
    ]), file=sys.stderr)
    sys.exit(1)


def _apply_krypton_patches(source_tree):
    patches.apply_patches(
        layout.layer_patches('krypton'),
        source_tree,
        patch_bin_path=(source_tree / _PATCH_BIN_RELPATH)
    )


def _stage_resources():
    """Helium's resource folders with Krypton's art laid over them.

    The staged copies keep upstream's folder names, so upstream's own lists
    (including helium-windows' ../helium-chromium/ references) resolve
    unchanged. Helium decides where every resource goes; Krypton only
    supplies different files under the same names.
    """
    staged_core = layout.STAGED_RESOURCES / 'helium-chromium' / 'resources'
    staged_windows = layout.STAGED_RESOURCES / 'resources'
    if layout.STAGED_RESOURCES.exists():
        shutil.rmtree(layout.STAGED_RESOURCES)
    shutil.copytree(layout.HELIUM / 'resources', staged_core)
    shutil.copytree(layout.HELIUM_WINDOWS / 'resources', staged_windows)
    shutil.copytree(layout.KRYPTON_RESOURCES / 'core', staged_core, dirs_exist_ok=True)
    # Krypton: every upstream product mark and internal-page favicon must have
    # an owned replacement, including newly added assets after an upstream bump.
    for group in ('branding', 'favicons'):
        for original in (layout.HELIUM / 'resources' / group).rglob('*'):
            if original.is_file():
                relative = original.relative_to(layout.HELIUM / 'resources')
                if not (layout.KRYPTON_RESOURCES / 'core' / relative).is_file():
                    raise ValueError(f'Krypton artwork missing: {relative}')
    return staged_core, staged_windows


def _copy_chromium_src(source_tree):
    """Krypton-owned files that live in the Chromium tree."""
    for path in layout.KRYPTON_CHROMIUM_SRC.rglob('*'):
        if path.is_file() and path.name != 'README.md':
            destination = source_tree / path.relative_to(layout.KRYPTON_CHROMIUM_SRC)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)


# Krypton: optional Cast compilation exceptions, subject to explicit approval.
_KRYPTON_OPTIONAL_FEATURE_GN_EXCEPTIONS = {
    'enable_mdns': 'Compile local Cast discovery; runtime discovery remains opt-in.',
    'enable_service_discovery': 'Compile local device discovery for optional Cast.',
}

_GN_ASSIGNMENT = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=', re.M)


def _krypton_gn_flags(upstream_flags):
    """Krypton's GN args, with only the named approved upstream exceptions."""
    text = layout.KRYPTON_FLAGS.read_text(encoding=ENCODING)
    overridden = sorted((set(_GN_ASSIGNMENT.findall(text)) &
                         set(_GN_ASSIGNMENT.findall(upstream_flags))) -
                        set(_KRYPTON_OPTIONAL_FEATURE_GN_EXCEPTIONS))
    if overridden:
        raise SystemExit('flags.krypton.gn reassigns args Helium already sets: ' +
                         ', '.join(overridden) + '. Helium takes precedence; remove them.')
    return text


def _build_krypton_ui(ui_dir, out_dir, jobs=None):
    """Build and install the complete module before GN consumes its files."""
    vendored = layout.KRYPTON_UI_ABI
    ui_runtime.verify_contract(vendored, ui_dir)
    previous_cwd = os.getcwd()
    os.chdir(ui_dir)
    try:
        _run_build_process('cmake', '--preset', 'windows')
        _run_build_process('cmake', '--build', '--preset', 'windows-release',
                           *build_jobs.cmake_arguments(jobs))
        _run_build_process('cmake', '--install', 'out/build/windows',
                           '--config', 'Release', '--component', 'Runtime',
                           '--prefix', str(out_dir.resolve()))
    finally:
        os.chdir(previous_cwd)
    ui_runtime.write_manifest(out_dir, vendored)
    get_logger().info('Staged the complete KryptonUI runtime in %s', out_dir)


def main():
    """CLI Entrypoint"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--7z-path',
        dest='sevenz_path',
        default=USE_REGISTRY,
        help=('Command or path to 7-Zip\'s "7z" binary. If "_use_registry" is '
              'specified, determine the path from the registry. Default: %(default)s'))
    parser.add_argument(
        '--winrar-path',
        dest='winrar_path',
        default=USE_REGISTRY,
        help=('Command or path to WinRAR\'s "winrar.exe" binary. If "_use_registry" is '
              'specified, determine the path from the registry. Default: %(default)s'))
    parser.add_argument(
        '-j',
        type=int,
        dest='thread_count',
        help=('Compile jobs; omit for automatic parallelism, 0 for unlimited'))
    parser.add_argument(
        '--ci',
        type=int,
    )
    parser.add_argument(
        '--arm',
        action='store_true'
    )
    sources = parser.add_mutually_exclusive_group()
    sources.add_argument(
        '--tarball', dest='tarball', action='store_true',
        help='Download prepared Chromium archives (default for x64)')
    sources.add_argument(
        '--clone', dest='tarball', action='store_false',
        help='Clone Chromium and prepare dependencies with gclient (default for ARM64)')
    parser.set_defaults(tarball=None)
    parser.add_argument(
        '--dev',
        action='store_true'
    )
    # Krypton: checkpointing and the UI module.
    parser.add_argument(
        '--resume', action='store_true',
        help='Skip prepare when the tree was prepared from the current inputs')
    parser.add_argument(
        '--prepare-only', action='store_true',
        help='Prepare the pinned source tree without configuring or compiling')
    parser.add_argument(
        '--configure-only', action='store_true',
        help='Prepare and generate build files without starting compilation')
    parser.add_argument(
        '--overwrite', action='store_true',
        help='Delete an existing source tree, and any build in it, before preparing')
    parser.add_argument(
        '--krypton-ui', type=Path, metavar='DIR',
        default=os.environ.get('KRYPTON_UI_DIR') or layout.ROOT.parent / 'KryptonUI',
        help='Required KryptonUI checkout (default: $KRYPTON_UI_DIR or ../KryptonUI)')
    parser.add_argument('--source-tree', type=Path,
                        help='prepare a separate source tree without touching the active build')
    parser.add_argument('--state-file', type=Path,
                        help='preparation checkpoint for a separately staged source tree')
    args = parser.parse_args()
    try:
        layout.check_core_ref()
    except ValueError as error:
        parser.error(str(error))
    # Krypton: keep Ninja's automatic default; -j0 explicitly means unlimited.
    if args.thread_count is not None and args.thread_count < 0:
        parser.error('Build jobs must be nonnegative (-j0 means unlimited).')
    if args.tarball is None:
        args.tarball = not args.arm
    if args.tarball and args.arm:
        parser.error('the prepared Windows archive targets x64; use --clone for ARM64')

    # Set common variables
    source_tree = args.source_tree.resolve() if args.source_tree else layout.SOURCE_TREE
    if args.source_tree and not args.state_file:
        parser.error('--source-tree requires a separate --state-file')
    state_file = args.state_file.resolve() if args.state_file else _STATE_PATH
    downloads_cache = layout.DOWNLOAD_CACHE
    if os.environ.get('SISO_REAPI_ADDRESS'):
        os.environ['RBE_service_no_security'] = 'true'

    # Krypton: decide whether prepare runs, and never let it run over a tree
    # by accident.
    fingerprint = _prepare_fingerprint()
    prepare = not args.ci or not (source_tree / 'BUILD.gn').exists()
    if prepare and args.resume and (source_tree / 'BUILD.gn').exists() \
            and _read_state(state_file).get('prepare') == fingerprint:
        get_logger().info('Source tree is current; skipping prepare')
        prepare = False
    if prepare and not args.ci:
        if args.overwrite and source_tree.exists():
            get_logger().info('Deleting %s', source_tree)
            shutil.rmtree(source_tree)
        _refuse_to_discard(source_tree)

    if prepare:
        # Setup environment
        source_tree.mkdir(parents=True, exist_ok=True)
        downloads_cache.mkdir(parents=True, exist_ok=True)
        _make_tmp_paths()

        # Extractors
        extractors = {
            ExtractorEnum.SEVENZIP: args.sevenz_path,
            ExtractorEnum.WINRAR: args.winrar_path,
        }

        download_configs = [
            layout.HELIUM_WINDOWS / 'downloads.ini',
            layout.HELIUM / 'deps.ini',
            layout.KRYPTON_DEPS,  # Krypton: official extension release input.
        ]
        if args.tarball:
            download_configs = [
                layout.HELIUM / 'downloads.ini',
                layout.HELIUM_WINDOWS / 'downloads.x64.ini',
            ] + download_configs
        else:
            subprocess.run([
                sys.executable, str(layout.HELIUM_UTILS / 'clone.py'),
                '-o', str(source_tree), '-p', 'win-arm64' if args.arm else 'win64',
            ], check=True)

        get_logger().info('Downloading required files...')
        download_info = downloads.DownloadInfo(download_configs)
        downloads.retrieve_downloads(download_info, downloads_cache, None, True)
        try:
            downloads.check_downloads(download_info, downloads_cache, None)
        except downloads.HashMismatchError as exc:
            get_logger().error('File checksum does not match: %s', exc)
            parser.exit(1)
        get_logger().info('Unpacking downloads...')
        downloads.unpack_downloads(download_info, downloads_cache, None, source_tree, extractors)

        prune_binaries.prune_dirs(source_tree, False, None, keep_build_dependencies=True)
        pruning_list = layout.HELIUM / 'pruning.list'
        prune_list = tuple(filter(len, pruning_list.read_text(encoding=ENCODING).splitlines()))
        missing_files = prune_binaries.prune_files(
            source_tree, prune_list, keep_build_dependencies=True)
        if missing_files:
            get_logger().info('Skipped %d pruning entries absent from this source tree',
                              len(missing_files))

        if not args.dev:
            # Apply patches
            # First, ungoogled-chromium-patches
            # Krypton: less the Helium UI patches in patches/skip-helium.
            patches.apply_patches(
                layout.layer_patches('helium'),
                source_tree,
                patch_bin_path=(source_tree / _PATCH_BIN_RELPATH)
            )
            # Then Windows-specific patches
            patches.apply_patches(
            layout.layer_patches('helium-windows'),  # Krypton: 157 compatibility refreshes.
                source_tree,
                patch_bin_path=(source_tree / _PATCH_BIN_RELPATH)
            )
            # Krypton: then Krypton's patches, last.
            _apply_krypton_patches(source_tree)

        else:
            print("Apply patches using quilt, then press Enter")
            input()

        # Remote Windows compiles use a separate Linux compiler on workers.
        # Apply its extraction patch before downloading, and substitute domains after.
        if os.environ.get('SISO_REAPI_ADDRESS'):
            subprocess.run([
                sys.executable, str(source_tree / 'tools/clang/scripts/update.py'),
                '--host-os=linux',
                '--output-dir=' + str(source_tree / 'third_party/llvm-build/Release+Asserts_linux'),
            ], check=True)

        # Krypton: Krypton-owned files join the tree before substitution, so
        # they are treated exactly like patched files.
        _copy_chromium_src(source_tree)

        if not args.dev:
            # Substitute domains
            domain_substitution_list = layout.HELIUM / 'domain_substitution.list'
            domain_substitution.apply_substitution(
                layout.HELIUM / 'domain_regex.list',
                domain_substitution_list,
                source_tree,
                None
            )

            # Substitute names
            name_substitution.do_substitution(
                source_tree,
                tarpath=None,
                workers=min(32, os.cpu_count()),
                dry_run=False
            )

            # Append translations
            i18n_apply.apply_translations(source_tree)

        # Krypton: final visible names follow upstream substitution/translations.
        # Identifiers, legal text and website-visible identity are preserved.
        branding.apply(source_tree)

        # Set version
        version_parts = helium_version.get_version_parts(layout.HELIUM, layout.HELIUM_WINDOWS)
        chrome_version_path = source_tree / "chrome" / "VERSION"
        helium_version.check_existing_version(chrome_version_path)
        with open(chrome_version_path, "a") as f:
            for name, version in version_parts.items():
                helium_version.append_version(f, name, version)

        # Krypton: resources come from staged copies carrying Krypton's art.
        staged_core, staged_windows = _stage_resources()

        # Copy resources
        # First, generate and copy Windows-specific resources
        generate_resources.generate_resources(
            staged_windows / 'generate_resources.txt',
            staged_windows
        )

        replace_resources.copy_resources(
            staged_windows / 'platform_resources.txt',
            staged_windows,
            source_tree
        )

        # Then common helium-chromium resources
        generate_resources.generate_resources(
            staged_core / 'generate_resources.txt',
            staged_core
        )

        replace_resources.copy_resources(
            staged_core / 'helium_resources.txt',
            staged_core,
            source_tree
        )

        _configure_remoteexec(source_tree)
        # Krypton: upstream patches supply host plumbing; ship the official
        # extension after every upstream transformation has finished.
        official_ublock.stage(source_tree, downloads_cache)

        # Krypton: record what this tree was prepared from.
        _write_state(state_file, prepare=fingerprint)

    # Krypton: allow a reproducible source baseline before adapter development.
    if args.prepare_only:
        return

    # Krypton: required runtime inputs must exist before GN and compilation.
    if not args.arm:
        _build_krypton_ui(Path(args.krypton_ui).resolve(),
                          source_tree / 'krypton_ui_runtime', args.thread_count)

    if not args.ci or not (source_tree / 'out/Default').exists():
        # Output args.gn
        (source_tree / 'out/Default').mkdir(parents=True, exist_ok=True)
        gn_flags = (layout.HELIUM / 'flags.gn').read_text(encoding=ENCODING)
        gn_flags += '\n'
        windows_flags = (layout.HELIUM_WINDOWS / 'flags.windows.gn').read_text(encoding=ENCODING)
        if args.arm:
            windows_flags = windows_flags.replace('x64', 'arm64')
        if args.dev:
            windows_flags += '\nchrome_pgo_phase=0\n'

        if os.environ.get('SISO_REAPI_ADDRESS'):
            windows_flags += 'use_remoteexec = true\n'
            # Precompiled modules are not portable across Windows/Linux hosts.
            windows_flags += 'use_clang_modules = false\n'
        elif shutil.which('sccache'):
            windows_flags += 'cc_wrapper = "sccache"\n'

        gn_flags += windows_flags
        if args.dev:
            gn_flags += 'is_component_build=true\n'
        else:
            gn_flags += 'is_official_build=true\n'

        winsparkle_ed_key = os.environ.get('WINSPARKLE_ED_KEY', '')
        authenticode_org = os.environ.get('WINSPARKLE_AUTHENTICODE_ORG', '')
        if winsparkle_ed_key and authenticode_org:
            gn_flags += 'enable_winsparkle=true\n'
            gn_flags += f'winsparkle_ed_key="{winsparkle_ed_key}"\n'
            gn_flags += f'winsparkle_authenticode_org="{authenticode_org}"\n'

        # Krypton: add-only args plus the two approved discovery exceptions.
        krypton_flags = _krypton_gn_flags(gn_flags)
        if args.arm:
            krypton_flags = re.sub(r'^\s*use_(sse\w*|avx\w*|fma)\s*=.*$', '', krypton_flags,
                                   flags=re.M)
        gn_flags += '\n' + krypton_flags

        (source_tree / 'out/Default/args.gn').write_text(gn_flags, encoding=ENCODING)

    # Enter source tree to run build commands
    os.chdir(source_tree)

    if not args.ci or not os.path.exists('out\\Default\\build.ninja'):
        # Run gn gen
        _run_build_process(
            'buildtools\\win\\gn.exe', 'gen', 'out\\Default', '--fail-on-unused-args')

    # Krypton: GN validation need not launch a full browser compilation.
    if args.configure_only:
        return

    # Ninja commandline
    # Krypton: native Ninja preserves the selected backend and existing graph.
    ninja_commandline = ['ninja']
    ninja_commandline.extend(build_jobs.ninja_arguments(args.thread_count))
    ninja_commandline.append('-C')
    ninja_commandline.append('out\\Default')

    # Finish all release targets before signing. Siso can replace signed outputs
    # if it is invoked again to build their dependents (the mini installer).
    ninja_commandline.extend(['chrome', 'chromedriver', 'setup', 'mini_installer'])

    # Run ninja
    if args.ci:
        max_time = 5.5 * 60 * 60
        secs_spent = int(time.time()) - args.ci
        timeout = int(max_time - secs_spent)
        print(f"{timeout} seconds left for build")

        _run_build_process_timeout(*ninja_commandline, timeout=timeout)
    else:
        _run_build_process(*ninja_commandline)


if __name__ == '__main__':
    main()
