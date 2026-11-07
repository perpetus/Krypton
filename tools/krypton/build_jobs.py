"""Native build parallelism: automatic by default, zero explicitly unlimited."""

import argparse


def job_count(value):
    try:
        jobs = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('jobs must be a nonnegative integer') from error
    if jobs < 0:
        raise argparse.ArgumentTypeError('jobs must be nonnegative (0 means unlimited)')
    return jobs


def ninja_arguments(jobs):
    return [] if jobs is None else ['-j', str(jobs)]


def cmake_arguments(jobs):
    if jobs is None:
        return []
    # CMake's --parallel expects a positive number. Our presets use Ninja,
    # which accepts -j0 to request unlimited jobs explicitly.
    return ['--', '-j0'] if jobs == 0 else ['--parallel', str(jobs)]
