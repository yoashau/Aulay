#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,sys,tempfile
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='aulay-log-') as tmp:
    exe=str(Path(tmp)/'check')
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I',str(root),str(root/'tests/diagnostic_overhead.cpp'),'-o',exe],check=True)
    subprocess.run([exe],check=True)
diag=(root/'Diagnostics.hpp').read_text();monitor=(root/'DebugAudioMonitor.hpp').read_text()
record=diag.split('void RecordDiagnostic(',1)[1].split('std::wstring DiagnosticDeviceToken',1)[0]
assert all(x not in record for x in ('WriteFile(', 'WriteDiagnosticEntryLocked(', 'OutputDebugStringW(', 'Utf16ToUtf8('))
assert 'sampling.Boost(e.tick)' in monitor and 'sampling.detailedUntil' in monitor
assert 'sampling.SampleMs(now)' in monitor and 'sampling.InventoryMs(now)' in monitor
assert 'WaitDiagnosticsFinished()' in (root/'Aulay.cpp').read_text()
print('DIAGNOSTIC_IO_AND_SAMPLING_GUARDS=PASS')
