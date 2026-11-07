"""User parallelism choices reach native tools without an eight-job ceiling."""

import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from krypton import build_jobs


class BuildJobsTests(unittest.TestCase):
    def test_counts_above_eight_are_accepted(self):
        self.assertEqual(build_jobs.job_count('32'), 32)

    def test_explicit_unlimited_is_forwarded_to_ninja(self):
        self.assertEqual(build_jobs.ninja_arguments(build_jobs.job_count('0')), ['-j', '0'])

    def test_automatic_omits_the_ninja_job_option(self):
        self.assertEqual(build_jobs.ninja_arguments(None), [])

    def test_cmake_unlimited_uses_the_native_ninja_option(self):
        self.assertEqual(build_jobs.cmake_arguments(0), ['--', '-j0'])

    def test_cmake_accepts_large_explicit_parallelism(self):
        self.assertEqual(build_jobs.cmake_arguments(24), ['--parallel', '24'])

    def test_cmake_automatic_omits_the_parallel_option(self):
        self.assertEqual(build_jobs.cmake_arguments(None), [])

    def test_negative_and_non_integer_counts_are_rejected(self):
        for value in ('-1', 'many', '1.5'):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                build_jobs.job_count(value)


if __name__ == '__main__':
    unittest.main()
