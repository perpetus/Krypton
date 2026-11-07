"""Prepare a pinned, native Windows Buildbarn deployment for SSH transport."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'build/remote-execution'
EXECUTION_REVISION = '582d07ff2acc2faa91b3e875e62bf610b13d37b5'
STORAGE_REVISION = 'ae61334ea7982155ceb317e286e2a462d875477d'
BYTESTREAM_VERSION = 'v0.0.0-20260526163538-3dc84a4a5aaa'


def build_binaries() -> None:
    environment = os.environ | {'GOMAXPROCS': '2'}
    binaries = WORK / 'bin'
    binaries.mkdir(parents=True, exist_ok=True)
    for name, revision in [('bb-remote-execution', EXECUTION_REVISION),
                           ('bb-storage', STORAGE_REVISION)]:
        checkout = WORK / 'src' / name
        if not checkout.exists():
            subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout',
                            f'https://github.com/buildbarn/{name}.git', str(checkout)], check=True)
            subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', revision], check=True)
        actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise SystemExit(f'Refusing to build an unexpected {name} revision: {actual}')
    execution = WORK / 'src/bb-remote-execution'
    for command in ['bb_runner', 'bb_scheduler', 'bb_worker']:
        subprocess.run(['go', 'build', '-p', '2', '-o', str(binaries / f'{command}.exe'),
                        f'./cmd/{command}'], cwd=execution, env=environment, check=True)

    storage = WORK / 'src/bb-storage'
    # Buildbarn's Bazel build generates this registration function against the
    # current gRPC API. The published Go module still has a *grpc.Server-only
    # signature. Adapt that single signature in an isolated module copy; the
    # descriptor, handlers and wire protocol remain the original generated code.
    module_cache = Path(subprocess.check_output(['go', 'env', 'GOMODCACHE'], text=True).strip())
    upstream = module_cache / 'google.golang.org/genproto/googleapis' / f'bytestream@{BYTESTREAM_VERSION}'
    if not upstream.exists():
        subprocess.run(['go', 'mod', 'download', f'google.golang.org/genproto/googleapis/bytestream@{BYTESTREAM_VERSION}'],
                       cwd=storage, env=environment, check=True)
    adapted = WORK / 'src/bytestream-grpc-registrar'
    adapted.mkdir(exist_ok=True)
    for path in upstream.iterdir():
        if path.is_file():
            shutil.copyfile(path, adapted / path.name)
    generated = adapted / 'bytestream.pb.go'
    before = 'func RegisterByteStreamServer(s *grpc.Server, srv ByteStreamServer) {'
    after = 'func RegisterByteStreamServer(s grpc.ServiceRegistrar, srv ByteStreamServer) {'
    contents = generated.read_text()
    if contents.count(before) != 1:
        raise SystemExit('The pinned ByteStream registration source has changed.')
    generated.write_text(contents.replace(before, after), newline='\n')
    modfile = WORK / 'storage.go.mod'
    modfile.write_text((storage / 'go.mod').read_text() +
                       f'\nreplace google.golang.org/genproto/googleapis/bytestream => {adapted.as_posix()}\n',
                       newline='\n')
    shutil.copyfile(storage / 'go.sum', modfile.with_suffix('.sum'))
    subprocess.run(['go', 'build', '-p', '2', '-mod=mod', f'-modfile={modfile}', '-o',
                    str(binaries / 'bb_storage.exe'), './cmd/bb_storage'],
                   cwd=storage, env=environment, check=True)
    manifest = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in binaries.glob('*.exe')}
    (binaries / 'manifest.json').write_text(json.dumps({
        'execution_revision': EXECUTION_REVISION, 'storage_revision': STORAGE_REVISION,
        'bytestream_module': BYTESTREAM_VERSION,
        'bytestream_adaptation': {'before': before, 'after': after}, 'files': manifest,
    }, indent=2) + '\n')


def prepare_config(remote_jobs: int) -> None:
    if not 1 <= remote_jobs <= 8:
        raise SystemExit('A Windows worker must have between one and eight slots.')
    destination = WORK / 'deploy'
    destination.mkdir(parents=True, exist_ok=True)
    allow = {'allow': {}}
    def server(port: int) -> list[dict]:
        return [{'listenAddresses': [f'127.0.0.1:{port}'], 'authenticationPolicy': allow}]
    def diagnostics(port: int) -> dict:
        return {'diagnosticsHttpServer': {'httpServers': server(port), 'enablePrometheus': True}}
    def local_store(name: str, size: int, entries: int, new_blocks: int = 3) -> dict:
        return {'local': {
            'keyLocationMapInMemory': {'entries': entries},
            'keyLocationMapMaximumGetAttempts': 16,
            'keyLocationMapMaximumPutAttempts': 64,
            'oldBlocks': 1 if name == 'cas' else 8,
            'currentBlocks': 4 if name == 'cas' else 24,
            'newBlocks': 2 if name == 'cas' else new_blocks,
            'blocksOnBlockDevice': {
                'source': {'file': {'path': f'C:/KryptonBuild/storage/{name}/blocks', 'sizeBytes': size}},
                'spareBlocks': 1 if name == 'cas' else 3,
            },
        }}
    cas = local_store('cas', 5 * 1024**3, 262144)
    ac = {'completenessChecking': {'backend': local_store('ac', 64 * 1024**2, 8192, new_blocks=1),
                                   'maximumTotalTreeSizeBytes': 64 * 1024**2}}
    storage = {
        'grpcServers': server(8980), 'maximumMessageSizeBytes': 8 * 1024**2,
        'global': diagnostics(9000),
        'contentAddressableStorage': {'backend': cas, 'getAuthorizer': allow,
                                     'putAuthorizer': allow, 'findMissingAuthorizer': allow},
        'actionCache': {'backend': ac, 'getAuthorizer': allow, 'putAuthorizer': allow},
        'executeAuthorizer': allow,
        'schedulers': {'': {'endpoint': {'address': '127.0.0.1:8982'}}},
    }
    grpc_cas = {'grpc': {'client': {'address': '127.0.0.1:8980'}}}
    scheduler = {
        'clientGrpcServers': server(8982), 'workerGrpcServers': server(8983),
        'buildQueueStateGrpcServers': server(8984),
        'contentAddressableStorage': grpc_cas, 'maximumMessageSizeBytes': 8 * 1024**2,
        'global': diagnostics(9001), 'executeAuthorizer': allow,
        'modifyDrainsAuthorizer': allow, 'killOperationsAuthorizer': allow,
        'synchronizeAuthorizer': allow,
        'actionRouter': {'simple': {'platformKeyExtractor': {'action': {}},
            'invocationKeyExtractors': [{'toolInvocationId': {}}],
            'initialSizeClassAnalyzer': {'defaultExecutionTimeout': '900s', 'maximumExecutionTimeout': '1800s'}}},
        'platformQueueWithNoWorkersTimeout': '120s',
    }
    runner = {'buildDirectoryPath': 'C:/KryptonBuild/worker/build',
              'grpcServers': server(8985), 'global': diagnostics(9003),
              'setTmpdirEnvironmentVariable': True}
    worker = {
        'blobstore': {'contentAddressableStorage': grpc_cas, 'actionCache': grpc_cas},
        'maximumMessageSizeBytes': 8 * 1024**2, 'scheduler': {'address': '127.0.0.1:8983'},
        'global': diagnostics(9002), 'inputDownloadConcurrency': 8, 'outputUploadConcurrency': 4,
        'buildDirectories': [{'native': {
            'buildDirectoryPath': 'C:/KryptonBuild/worker/build',
            'cacheDirectoryPath': 'C:/KryptonBuild/worker/cache',
            'maximumCacheFileCount': 100000, 'maximumCacheSizeBytes': 5 * 1024**3,
            'cacheReplacementPolicy': 'LEAST_RECENTLY_USED'},
            'runners': [{'endpoint': {'address': '127.0.0.1:8985'}, 'concurrency': remote_jobs,
                'instanceNamePrefix': 'krypton',
                'platform': {'properties': [{'name': 'OSFamily', 'value': 'Windows'}]},
                'workerId': {'hostname': 'thinkpad', 'purpose': 'krypton-chromium'},
                'environmentVariables': {'SystemRoot': 'C:/Windows', 'WINDIR': 'C:/Windows',
                                         'PATH': 'C:/Windows/System32;C:/Windows'},
            }]}],
        'directoryCache': {'maximumCount': 4096, 'maximumSizeBytes': 16 * 1024**2,
                           'cacheReplacementPolicy': 'LEAST_RECENTLY_USED'},
    }
    for name, configuration in [('storage', storage), ('scheduler', scheduler),
                                ('runner', runner), ('worker', worker)]:
        (destination / f'{name}.json').write_text(json.dumps(configuration, indent=2) + '\n')
    for script in ['Start-Services.ps1', 'Stop-Services.ps1', 'Install-Services.ps1']:
        shutil.copyfile(Path(__file__).parent / script, destination / script)
    print(f'Prepared loopback-only native Windows deployment with {remote_jobs} worker slots.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--remote-jobs', type=int, default=6)
    arguments = parser.parse_args()
    if arguments.build:
        build_binaries()
    prepare_config(arguments.remote_jobs)
