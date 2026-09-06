#!/usr/bin/env python3
"""Compile the actual settings worker with a pumped dispatcher and controlled I/O."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]
source = (root / 'SettingsSave.hpp').read_text(encoding='utf-8-sig')
source = re.sub(r'^#(?:include|pragma)[^\n]*\n', '', source, flags=re.M)
template = (Path(__file__).resolve().parent / 'windows_settings.cpp').read_text()
# Also run this harness against the pre-fix synchronous implementation.
call = 'FlushPendingSettings();' if re.search(r'^void FlushPendingSettings\(', source, re.M) else 'co_await FlushPendingSettings();'
Path(sys.argv[1]).write_text(template.replace('// SETTINGS_IMPLEMENTATION', source).replace('// FLUSH_CALL', call), encoding='utf-8')
