"""Stage exact local compiler/SDK inputs for hermetic Windows remote actions."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'build/src'
CONFIG = SOURCE / 'build/krypton_remote'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    CONFIG.mkdir(parents=True, exist_ok=True)
    stock_config = SOURCE / 'build/config/siso'
    shutil.copytree(stock_config, CONFIG, dirs_exist_ok=True)
    shutil.copyfile(stock_config / 'main.star', CONFIG / 'chromium.star')
    toolchain = (SOURCE / 'out/Default/toolchain.ninja').read_text()
    command = toolchain.split('rule cxx\n', 1)[1].split('\n', 1)[0]
    includes = re.findall(r'"-imsvc([^\"]+)"', command)
    if not includes or not any('10.0.28000.0' in path for path in includes):
        raise SystemExit('The configured compiler is missing pinned Windows SDK 10.0.28000.0.')
    mapping = []
    files = {}
    total = 0
    for index, name in enumerate(includes):
        original = Path(name).resolve(strict=True)
        staged = CONFIG / 'toolchain' / f'include{index}'
        staged.mkdir(parents=True, exist_ok=True)
        mapping.append((original.as_posix().lower(), '../../' + staged.relative_to(SOURCE).as_posix()))
        for path in sorted(original.rglob('*')):
            if not path.is_file():
                continue
            relative = path.relative_to(original)
            destination = staged / relative
            signature = digest(path)
            if not destination.exists() or digest(destination) != signature:
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)
            files[destination.relative_to(SOURCE).as_posix()] = signature
            total += path.stat().st_size
    compiler = SOURCE / 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe'
    identity = {
        'compiler': digest(compiler), 'sdk_version': '10.0.28000.0',
        'include_mapping': mapping, 'files': files,
    }
    signature = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    identity['sha256'] = signature
    (CONFIG / 'toolchain-manifest.json').write_text(json.dumps(identity, indent=2) + '\n')
    (CONFIG / 'mapping.star').write_text('MAPPINGS = ' + repr(mapping) + '\n', newline='\n')
    shutil.copyfile(Path(__file__).parent / 'main.star', CONFIG / 'main.star')
    print(f'Staged {len(files)} exact SDK/header inputs ({total / 1024**2:.1f} MiB).')
    print(f'Compiler/SDK identity: {signature}')


if __name__ == '__main__':
    main()
