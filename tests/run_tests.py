#!/usr/bin/env python3
"""Run every portable check from any working directory; requires Git and C++17."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TESTS = (
    'test_project_layout.py',
    'test_regressions.py',
    'test_audio_flow.py',
    'test_diagnostic_overhead.py',
    'test_hardening.py',
    'test_debug_markers.py',
    'test_debug_timeline.py',
)


def main():
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT
    failures = 0
    for name in TESTS:
        print(f'RUN {name}', flush=True)
        result = subprocess.run(
            [sys.executable, str(ROOT / 'tests' / name), str(root)],
            cwd=root,
            env={**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'},
        )
        failures += result.returncode != 0
    print(f'PORTABLE_SUITES {len(TESTS) - failures}/{len(TESTS)} passed', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
