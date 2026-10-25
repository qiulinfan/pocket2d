#!/usr/bin/env python3
"""Box2D on the shared task pool gives the same result as one thread.

Runs physics_benchmark on a pyramid large enough (2,485 bodies) to use the
pool, once with POCKET_PHYSICS_WORKERS=0 and once with every worker, and
compares the digest of final body positions.
"""
import os
from pathlib import Path
import subprocess
import sys


def run(benchmark, workers):
    result = subprocess.run([benchmark, "70", "0", "90"], capture_output=True, text=True, encoding="utf-8", timeout=300,
                            env=dict(os.environ, POCKET_PHYSICS_WORKERS=workers))
    if result.returncode != 0:
        raise AssertionError("physics benchmark failed: " + result.stderr)
    return result.stdout.strip().splitlines()[-1].split(",")


def main():
    benchmark = str(Path(sys.argv[1]).resolve())
    single, pooled = run(benchmark, "0"), run(benchmark, "auto")
    if single[1] != "2485" or single[-1] != pooled[-1]:
        raise AssertionError("pooled physics diverged: %r vs %r" % (single, pooled))
    print("physics on the shared pool matches one thread (%s bodies, digest %s)" % (single[1], single[-1]))


if __name__ == "__main__":
    main()
