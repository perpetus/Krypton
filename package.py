#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright 2026 The Krypton Authors
# You can use, redistribute, and/or modify this source code under
# the terms of the GPL-3.0 license that can be found in the LICENSE file.
"""
Krypton packaging script for Windows

Reuse helium-windows' packaging against Krypton's build tree and artifact
names. Krypton's additions stage a branded installer script under build/;
the pinned upstream installer remains untouched. See
docs/privacy-features-implementation.md for branding and verification limits.
"""

import argparse
import importlib.util
import platform
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent / 'tools'))
from krypton import layout  # pylint: disable=wrong-import-position
from krypton import product_version  # pylint: disable=wrong-import-position
sys.path.pop(0)


def _stage_installer_script():
    """Krypton: substitute a generated copy; pinned installer sources stay pristine."""
    source = (layout.HELIUM_WINDOWS / 'installer' / 'helium.nsi').read_text(encoding='utf-8')
    replacements = {
        '!define PRODUCT_NAME "Helium"': '!define PRODUCT_NAME "Krypton"',
        '!define PRODUCT_PUBLISHER "imput"': '!define PRODUCT_PUBLISHER "Krypton"',
        '!define PRODUCT_COMPANY_PATH "imput"': '!define PRODUCT_COMPANY_PATH "Krypton"',
        '!define PRODUCT_GUID "{FB68A146-637A-48C2-A0C4-1565DE45FEBD}"':
            '!define PRODUCT_GUID "{FCEF027B-845E-4D84-8EEA-46EF8547CA67}"',
    }
    for before, after in replacements.items():
        if source.count(before) != 1:
            raise ValueError(f'Pinned installer identity changed: {before}')
        source = source.replace(before, after)
    target = layout.BUILD_DIR / 'staged-installer' / 'helium.nsi'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(source, encoding='utf-8')
    return target


def _load_helium_packaging():
    spec = importlib.util.spec_from_file_location('helium_package',
                                                  layout.HELIUM_WINDOWS / 'package.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Krypton: packaging and Windows resources use the same updater marker.
    # Override this module's dependency, leaving the upstream module untouched.
    module.helium_version = SimpleNamespace(get_version_parts=lambda *_: product_version.parts())
    # Everything else it resolves (helium-chromium versions, the NSIS script,
    # the license) stays inside upstream/helium-windows, where it belongs.
    module._BUILD_SRC = layout.SOURCE_TREE  # pylint: disable=protected-access
    module._ICON_PATH = (layout.SOURCE_TREE / 'chrome' / 'app' / 'theme' / 'chromium' / 'win' /
                         'chromium.ico')  # pylint: disable=protected-access
    # Krypton: payload handling and legal text continue to use the pinned packager.
    def build_installer(version, arch, build_outputs, output_file):
        subprocess.run([
            str(layout.SOURCE_TREE / 'third_party' / 'nsis' / 'makensis.exe'),
            '-NOCD', f'-DVERSION={version}', f'-DARCH={arch}',
            f'-DSETUP_EXE={build_outputs / "setup.exe"}',
            f'-DHELIUM_7Z={build_outputs / "helium.packed.7z"}',
            f'-DICON_FILE={module._ICON_PATH}',
            f'-DOUTPUT_FILE={output_file}',
            f'-DLICENSE_FILE={layout.HELIUM_WINDOWS / "LICENSE"}',
            str(_stage_installer_script()),
        ], check=True)
    module._build_nsis_installer = build_installer  # pylint: disable=protected-access
    return module


def main():
    """Entrypoint for local unsigned packaging."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-outputs', type=Path,
                        default=layout.SOURCE_TREE / 'out/Default')
    parser.add_argument('--output-dir', type=Path, default=layout.BUILD_DIR)
    parser.add_argument(
        '--cpu-arch', metavar='ARCH', default=platform.architecture()[0],
        choices=('64bit', '32bit'),
        help='Target CPU filter in FILES.cfg. Default: %(default)s')
    args = parser.parse_args()

    helium_packaging = _load_helium_packaging()
    for artifact in helium_packaging.create_packages(args.build_outputs, args.output_dir,
                                                     args.cpu_arch):
        renamed = artifact.with_name('krypton_' + artifact.name.removeprefix('helium_'))
        artifact.replace(renamed)
        print(renamed)


if __name__ == '__main__':
    main()
