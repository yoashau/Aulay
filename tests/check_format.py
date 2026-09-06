#!/usr/bin/env python3
"""Verify clang-format compliance over the app sources.

CI runs this with a pinned clang-format (see .github/workflows/build.yaml);
locally, any clang-format 19+ on PATH works (env override: AULAY_CLANG_FORMAT).
Version.h, targetver.h and resource.h are excluded on purpose: their macro
literals (AULAY_VERSION_NUMBER 1,0,0,0) are matched verbatim by guard tests.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
EXCLUDED = {"Version.h", "targetver.h", "resource.h"}
files = sorted(
    p
    for group in (root.glob("*.cpp"), root.glob("*.h"), root.glob("*.hpp"), (root / "tests").glob("*.cpp"))
    for p in group
    if p.name not in EXCLUDED
)

clang_format = os.environ.get("AULAY_CLANG_FORMAT") or shutil.which("clang-format") or shutil.which("clang-format.exe")
if not clang_format:
    print("FORMAT_CHECK: clang-format not found (pip install clang-format==19.1.5, or set AULAY_CLANG_FORMAT)")
    sys.exit(1)

not_clean = []
for f in files:
    result = subprocess.run(
        [clang_format, "-style=file", "--dry-run", "-Werror", str(f)], capture_output=True, text=True
    )
    if result.returncode != 0:
        not_clean.append(str(f.relative_to(root)))

if not_clean:
    print("FORMAT_CHECK: not clang-format clean (run: clang-format -i -style=file <file>):")
    for f in not_clean:
        print("  " + f)
    sys.exit(1)
print(f"FORMAT_CHECK: {len(files)} files clang-format clean")
