# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.

"""Resume the existing Chromium graph with configurable native Ninja jobs."""

import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess

from krypton.build_jobs import job_count, ninja_arguments


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/src'


def ninja_target(value):
    # The shared VS-environment helper invokes cmd.exe. Keep targets literal
    # and prevent them from becoming extra Ninja options or shell commands.
    if value.startswith('-') or not re.fullmatch(r'[A-Za-z0-9_./:+-]+', value):
        raise argparse.ArgumentTypeError('expected a Ninja target name or output path')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-j', '--jobs', type=job_count,
                        help='local jobs; omit for Ninja automatic, 0 for unlimited')
    parser.add_argument('targets', nargs='*', type=ninja_target,
                        help='existing Ninja targets (default: chrome)')
    args = parser.parse_args()
    if not (SOURCE / 'out/Default/build.ninja').is_file():
        parser.error('build/src/out/Default/build.ninja is missing; configure the build first')

    spec = importlib.util.spec_from_file_location('krypton_build', ROOT / 'build.py')
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    targets = args.targets or ['chrome']
    parallelism = 'automatic' if args.jobs is None else ('unlimited' if args.jobs == 0 else str(args.jobs))
    print(f'Local Ninja: {parallelism} jobs; targets: {", ".join(targets)}', flush=True)
    os.chdir(SOURCE)
    try:
        build._run_build_process('ninja', '-C', 'out/Default',
                                 *ninja_arguments(args.jobs), *targets)
    except subprocess.CalledProcessError as error:
        return error.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
