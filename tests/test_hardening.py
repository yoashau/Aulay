#!/usr/bin/env python3
"""Portable integration guards for deadlines, adapter routing and settings fallback.
These are source constraints, not substitutes for tests/run_windows_tests.cmd.
"""
from pathlib import Path
import sys
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
def source(name):
    path = root / name
    return path.read_text(encoding='utf-8-sig') if path.exists() else ''
app = ''.join(source(name) for name in ('Aulay.cpp', 'AppModules.hpp', 'ConnectionState.hpp', 'DeviceListUi.hpp', 'BluetoothRecovery.hpp', 'ConnectionFlow.hpp', 'DeviceWatcher.hpp', 'TrayPanel.hpp'))
async_code = source('AsyncUtil.hpp')
routing = source('BluetoothRouting.hpp')
settings = source('SettingsUtil.hpp')
checks = {
    'completion event rather than fixed timeout sleep': 'operation.Completed(' in async_code and 'resume_on_signal(' in async_code and 'resume_after(' not in async_code,
    'timeout cancels without awaiting a hung Cancel': 'CancelAsyncInBackground(operation)' in async_code,
    'late completion retains only weak waiter': 'std::weak_ptr<AsyncWaitSignal>(waiter)' in async_code,
    'exit wakes recovery cancellation': 'g_app.bluetooth.cancellation->Request()' in app,
    'close cancels the current audio attempt': 'retired.cancellation->Request()' in app,
    'StartAsync and OpenAsync share deadline': all(call in app for call in ('AwaitBounded(StartConnectionInBackground(currentConnection), attemptDeadline, attemptCancellation)', 'AwaitBounded(OpenConnectionInBackground(currentConnection), attemptDeadline, attemptCancellation)')),
    'access and Off use primary deadline': all(call in app for call in ('AwaitBounded(Radio::RequestAccessAsync(), primaryDeadline, cancellation)', 'AwaitBounded(SetRadioStateInBackground(radio, RadioState::Off), offDeadline, cancellation)')),
    'On uses separate uncancelled cleanup deadline': 'AwaitBounded(SetRadioStateInBackground(radio, RadioState::On), restoreDeadline)' in app,
    'reset uses target device route': 'ResolveBluetoothRadio(deviceId, deadline, cancellation)' in app,
    'PnP ancestor walk rather than radio order': all(call in routing for call in ('CM_Get_Device_Interface_PropertyW(', 'CM_Get_Parent(', 'SelectBluetoothAdapterIndex(ancestors, instances)', 'adapter.GetRadioAsync()')),
    'ambiguous route fails explicitly': 'if (!index)' in routing and 'ERROR_NOT_FOUND' in routing,
    'discovery prewarms route without awaiting': 'PrewarmBluetoothRoute(id);' in app and 'co_await PrewarmBluetoothRoute' not in app,
    'device removal invalidates cached association': 'InvalidateBluetoothRoute(deviceId);' in app,
    'fallback is cached only after successful write': 'g_settingsStorage.activePath = path;' in settings and 'WriteSettingsAtomically(path, pending.utf8);' in settings,
    'startup chooses newer valid settings': 'PreferFallbackSettings(first, second)' in settings and 'ReadSettingsFile(path)' in settings,
    'both locations failing notify user': 'saveErrorShown' in settings and 'Settings could not be saved.' in settings,
    'atomic durable settings writes retained': all(call in settings for call in ('FlushFileBuffers(', 'ReplaceFileW(', 'MoveFileExW(')),
}
for label, result in checks.items():
    if not result:
        print('FAIL ' + label)
passed = sum(checks.values())
print(f'HARDENING_GUARDS {passed}/{len(checks)} passed')
print('HARDENING_RESULT=' + ('PASS' if passed == len(checks) else 'FAIL'))
sys.exit(0 if passed == len(checks) else 1)
