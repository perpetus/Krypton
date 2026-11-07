"""Build through the SSH Windows worker: four local and six remote jobs."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import shutil

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'build/src'


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--local-jobs', type=int, default=4)
    parser.add_argument('--remote-jobs', type=int, default=6)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--strict-remote', action='store_true')
    parser.add_argument('targets', nargs='*', default=['chrome'])
    args = parser.parse_args()
    if not 1 <= args.local_jobs <= 8 or not 1 <= args.remote_jobs <= 8:
        parser.error('Each Windows machine must have between one and eight jobs.')
    manifest_path = SOURCE / 'build/krypton_remote/toolchain-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    compiler = SOURCE / 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe'
    if hashlib.sha256(compiler.read_bytes()).hexdigest() != manifest['compiler']:
        raise SystemExit('The compiler changed; run prepare_toolchain.py again.')
    with socket.create_connection(('127.0.0.1', 18980), timeout=3):
        pass
    output = 'out/Default'
    targets = args.targets or ['chrome']
    if args.smoke:
        output = 'out/KryptonRemoteSmoke'
        smoke = SOURCE / output
        smoke.mkdir(parents=True, exist_ok=True)
        for name in ['args.gn', 'gn_logs.txt']:
            shutil.copyfile(SOURCE / 'out/Default' / name, smoke / name)
        toolchain = (SOURCE / 'out/Default/toolchain.ninja').read_text().splitlines()
        start = toolchain.index('rule cxx')
        end = next(i for i in range(start + 1, len(toolchain)) if toolchain[i].startswith('rule '))
        ui = (SOURCE / 'out/Default/obj/chrome/browser/ui/ui.ninja').read_text().splitlines()
        first = next(i for i, line in enumerate(ui) if line.startswith('build '))
        contents = '\n'.join(toolchain[start:end]) + '\n' + '\n'.join(ui[:first]) + '\n'
        contents += 'target_out_dir = obj\nlabel_name = smoke\n'
        targets = []
        for index in range(args.remote_jobs):
            name = f'probe{index}'
            (smoke / f'{name}.cc').write_text(
                '#include <windows.h>\n#include <vector>\n'
                'static_assert(sizeof(void*) == 8);\n'
                f'extern "C" __declspec(dllexport) int KryptonRemoteProbe{index}() {{\n'
                '  std::vector<DWORD> values{1, 2, 3};\n'
                f'  return static_cast<int>(values.size()) + {index};\n}}\n')
            target = f'obj/smoke/{name}.obj'
            targets.append(target)
            contents += (f'build {target}: cxx {name}.cc\n'
                         f'  source_file_part = {name}.cc\n  source_name_part = {name}\n')
        (smoke / 'build.ninja').write_text(contents, newline='\n')
    spec = importlib.util.spec_from_file_location('krypton_build', ROOT / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    command = [str(SOURCE / 'third_party/siso/cipd/siso.exe'), 'ninja',
               f'-local_jobs={args.local_jobs}', f'-remote_jobs={args.remote_jobs}',
               '-config_repo_dir=build/krypton_remote', '-reapi_address=127.0.0.1:18980',
               '-reapi_instance=krypton', '-reapi_insecure', '-project=',
               '-config=builder', '-C', output]
    if args.strict_remote or args.smoke:
        command.append('-strict_remote')
    # Job limits do not bound the Go coordinator's heap. Keep its retained
    # buffers under pressure on the 16 GiB host; live data and child compilers
    # may still exceed this soft limit. Allow a deliberate environment override.
    coordinator_memory = os.environ.setdefault('GOMEMLIMIT', '4GiB')
    print(f'Coordinator Go soft memory limit: {coordinator_memory}.', flush=True)
    print(f'Coordinator limits: {args.local_jobs} local + {args.remote_jobs} remote jobs.', flush=True)
    os.chdir(SOURCE)
    module._run_build_process(*command, *targets)


if __name__ == '__main__':
    main()
