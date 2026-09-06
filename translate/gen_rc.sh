#!/bin/sh

set -eu
cd "$(dirname "$0")"

mkdir -p ./generated

python ./po2ymo.py ./source/zh_CN.po ./generated/zh_CN.ymo
python ./po2ymo.py ./source/zh_TW.po ./generated/zh_TW.ymo

rc_utf8=./generated/translate.rc.utf8
{
	echo '#include "../../targetver.h"'
	echo '#include "windows.h"'
	echo 'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED'
	echo '1 YMO "zh_CN.ymo"'
	echo 'LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL'
	echo '1 YMO "zh_TW.ymo"'
} > "$rc_utf8"

# Resource scripts are checked out as UTF-16LE with a BOM. Generate that format
# explicitly so running this script never leaves the worktree in an invalid state.
python - "$rc_utf8" ./generated/translate.rc <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1])
target = Path(sys.argv[2])
text = source.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\n", "\r\n")
target.write_bytes(b"\xff\xfe" + text.encode("utf-16le"))
source.unlink()
PY
