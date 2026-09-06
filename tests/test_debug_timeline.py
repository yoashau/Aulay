#!/usr/bin/env python3
"""Structural coverage guards; executable Windows tests validate runtime behavior."""
from pathlib import Path
import sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path(__file__).resolve().parents[1]
app=''.join((root/name).read_text(encoding='utf-8-sig') for name in ('Aulay.cpp', 'AppModules.hpp', 'ConnectionState.hpp', 'SettingsSave.hpp', 'DeviceListUi.hpp', 'BluetoothRecovery.hpp', 'ConnectionFlow.hpp', 'DeviceWatcher.hpp', 'TrayPanel.hpp'))
monitor=(root/'DebugAudioMonitor.hpp').read_text() if (root/'DebugAudioMonitor.hpp').exists() else ''
diag=(root/'Diagnostics.hpp').read_text()
checks={
 'no extra debug heading':'debugTitle' not in app,
 'normal tray tooltip':'wcscpy_s(g_nid.szTip, _(L"Aulay"));' in app,
 'normal connected status':'session->second.noSoundReports ? _(L"No sound recorded")' not in app,
 'no busy snapshot discard':'snapshot-skipped=busy' not in app,
 'worker owns COM apartment':'CoInitializeEx(nullptr,COINIT_MULTITHREADED)' in monitor and 'std::thread([s]' in monitor,
 'adaptive sampling plus measured gaps':'sample-period-ms=500' in monitor and 'detailed-sample-period-ms=50' in monitor and 'max-sample-gap-ms=' in monitor,
 'app logs included in marker history':'EnqueueDebugAudioEvent(L"app "' in diag,
 'before and after close requests explicitly named':'CLOSE_REQUEST_ASYNC' in app and 'CLOSE_AFTER_RETURN' in app,
 'endpoint and session event registrations':all(v in monitor for v in ('RegisterEndpointNotificationCallback','RegisterSessionNotification','RegisterAudioSessionNotification','RegisterControlChangeNotify','GetCount(&count)')),
 'periodic inventory includes inactive endpoints':'DEVICE_STATEMASK_ALL' in monitor,
 'system service PID mapping':all(v in monitor for v in ('Audiosrv','AudioEndpointBuilder','bthserv','QueryServiceStatusEx')),
 'markers include bounded pre/post windows':'pre-window-ms=120000' in monitor and 'post-window-ms=30000' in monitor,
 'queue loss and I/O errors explicit':'queue-dropped=' in monitor and 'io-errors=' in monitor,
 'shutdown queue drain plus bounded UI wait':'events.swap(state->queue)' in monitor and 'empty=state->queue.empty()' in monitor and 'co_await WaitDebugAudioMonitorFinished()' in app and 'std::chrono::seconds(5)' in monitor,
 'worker never accesses app/UI globals':'g_app.' not in monitor and 'g_uiDispatcher' not in monitor,
 'production collector never changes playback':not any(v in monitor for v in ('SetMute(', 'SetMasterVolume(', 'SetStateAsync(', 'OpenAsync(', 'StartAsync(', 'client->Start(')),
 'small peak values retained':'L"%.9g"' in monitor,
}
for label,ok in checks.items():
 if not ok: print('FAIL '+label)
print(f'DEBUG_TIMELINE_GUARDS {sum(checks.values())}/{len(checks)} passed')
print('DEBUG_TIMELINE_RESULT='+('PASS' if all(checks.values()) else 'FAIL'))
sys.exit(0 if all(checks.values()) else 1)
