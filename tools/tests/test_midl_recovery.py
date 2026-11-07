# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.

"""Exercise the patched Chromium MIDL action with pruned binary baselines."""

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(os.environ.get(
    'KRYPTON_MIDL_SCRIPT',
    Path(__file__).resolve().parents[2] / 'build/src/build/toolchain/win/midl.py'))
PLACEHOLDER = '01234567-89AB-CDEF-0123-456789ABCDEF'
REPLACEMENT = 'FEDCBA98-7654-3210-FEDC-BA9876543210'


@unittest.skipUnless(SCRIPT.is_file(), 'requires a prepared Chromium checkout')
class MidlRecoveryTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('chromium_midl', SCRIPT)
        self.midl = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.midl)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / 'baseline/x64'
        self.source.mkdir(parents=True)
        self.output = self.root / 'output'
        self.output.mkdir()
        self.generated = self.root / 'generated'
        self.generated.mkdir()
        self.environment = self.root / 'environment.x64'
        self.environment.write_text('PATH=test\0\0')
        self.template = self.root / 'test.template'
        self.template.write_text(f'uuid(PLACEHOLDER-GUID-{PLACEHOLDER})')
        for name in ('test.h', 'test_i.c', 'test_p.c'):
            (self.source / name).write_text(PLACEHOLDER)

    def run_action(self, replacement):
        # Keep the real baseline-copy, GUID substitution and output-copy paths.
        # Only the SDK compiler is replaced with deterministic generated files.
        def compile_idl(args, environment):
            self.assertEqual(environment['PATH'], 'test')
            self.assertEqual(Path(args[-1]).read_text(), f'uuid({replacement})')
            for name in ('test.h', 'test_i.c', 'test_p.c'):
                (self.generated / name).write_text(replacement + '\n// Generated output\n')
            (self.generated / 'test.tlb').write_bytes(
                b'MSFT\x02\x00\x01\x00' + replacement.encode('ascii'))
            return 0, str(self.generated)

        with mock.patch.object(self.midl.sys, 'platform', 'win32'), \
                mock.patch.object(self.midl, 'run_midl', side_effect=compile_idl) as compiler:
            result = self.midl.main(
                str(self.environment), str(self.source.parent), str(self.output),
                f'PLACEHOLDER-GUID-{PLACEHOLDER}={replacement}',
                'test.tlb', 'test.h', 'none', 'test_i.c', 'test_p.c',
                'clang-cl.exe', str(self.template), '/env', 'x64')
        self.assertEqual(result, 0)
        compiler.assert_called_once()
        for name in ('test.h', 'test_i.c', 'test_p.c', 'test.tlb'):
            self.assertEqual((self.output / name).read_bytes(),
                             (self.generated / name).read_bytes())
        self.assertIn('PLACEHOLDER-GUID-', self.template.read_text())

    def test_empty_baseline_regenerates_with_requested_guids(self):
        (self.source / 'test.tlb').touch()
        self.run_action(REPLACEMENT)

    def test_missing_baseline_and_retry_both_regenerate(self):
        self.run_action(REPLACEMENT)
        self.assertEqual((self.source / 'test.tlb').stat().st_size, 0)
        self.run_action(PLACEHOLDER)


if __name__ == '__main__':
    unittest.main()
