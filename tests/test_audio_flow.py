#!/usr/bin/env python3
"""Compile and exercise the production frame-delivery recovery policy (no live writes)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="aulay-flow-") as tmp:
    exe = str(Path(tmp) / "flow-policy")
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(root), str(root / "tests/audio_flow_policy.cpp"), "-o", exe], check=True)
    subprocess.run([exe], check=True)
helper = (root / 'AudioFlowRecovery.hpp').read_text()
guards = [
    'CM_GETIDLIST_FILTER_ENUMERATOR' in helper and 'CM_LOCATE_DEVNODE_PHANTOM' in helper,
    'DEVPKEY_Device_Parent' in helper and '_wcsicmp(actualParent,parentId)!=0' in helper,
    'if (!selected.empty()) return {}' in helper,
    'SetDefaultEndpoint(' not in helper.split('inline HRESULT Cycle', 1)[1],
    's->cleanupResult=error' in helper,
]
assert all(guards), guards
print('FLOW_INTEGRATION_GUARDS 5/5 passed')

app=''.join((root/name).read_text(encoding='utf-8-sig') for name in ('Aulay.cpp', 'AppModules.hpp', 'ConnectionState.hpp', 'SettingsSave.hpp', 'DeviceListUi.hpp', 'BluetoothRecovery.hpp', 'ConnectionFlow.hpp', 'DeviceWatcher.hpp', 'TrayPanel.hpp'))
assert 'CONNECTION_MAX_TRIES' not in app and 'retryBudgetMs' not in app
assert 'co_await winrt::resume_background();' in app.split('CloseSessionInBackground(',1)[1].split('bool CloseConnectionSession',1)[0]
assert 'Sleep(1000)' not in helper and 'RegisterEndpointNotificationCallback' in helper
assert 'WaitFinished(previousTask)' in app and 'closedSignal' in helper
assert 'QueueConnection(deviceId, ConnectionRequestMode::Forced);' in app.split('void MarkNoSound(std::wstring const& deviceId)\n{',1)[1].split('}',1)[0]
print('RESPONSIVE_GUARDS 5/5 passed')

assert 'if(predecessor && predecessor!=flow)co_await AudioFlow::WaitFinished(predecessor)' in app
assert app.count('cancellation.callback([operation]{CancelAsyncInBackground(operation);});') == 3
print('CANCELLATION_AND_CLEANUP_CHAIN=PASS')
