#!/usr/bin/env python3
"""Run portable policy execution tests plus source-level WinRT integration guards.

python tests/test_regressions.py [project-root]
The C++ harness compiles the actual policy functions from Aulay.cpp, replacing
only Windows/UI dependencies. Use CXX=g++/clang++ or run under a VS dev prompt.
The source guards are not Bluetooth hardware integration tests.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
CPP = ''.join((ROOT / name).read_text(encoding='utf-8-sig') for name in ('Aulay.cpp', 'AppModules.hpp', 'ConnectionState.hpp', 'SettingsSave.hpp', 'DeviceListUi.hpp', 'BluetoothRecovery.hpp', 'ConnectionFlow.hpp', 'DeviceWatcher.hpp', 'TrayPanel.hpp'))
HEADER = (ROOT / 'Aulay.h').read_text(encoding='utf-8-sig')
DIAG = (ROOT / 'Diagnostics.hpp').read_text(encoding='utf-8-sig')


def block(text, declaration):
    match = re.search(declaration, text, re.M)
    start = match.start()
    opening = match.end() - 1 if text[match.end() - 1] == '{' else text.index('{', start)
    # Ignore braces in comments and string/character literals.
    tokens = re.finditer(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', text[opening:], re.S)
    depth = 0
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:opening + token.end()]
    raise ValueError(declaration)


def function(name, text=CPP):
    return block(text, r'^\w[^\n;]*\b' + name + r'\([^;]*?\)\n\{')


def harness():
    enums = '\n'.join(block(HEADER, '^enum class ' + name + r'\b') + ';' for name in (
        'ConnectionRequestMode', 'ConnectionRecoveryStrategy', 'ConnectionAttemptResult', 'BluetoothRecoveryResult'))
    request = block(HEADER, r'^struct ConnectionRequest\b') + ';'
    code = r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
struct DeviceInformation {
    std::wstring id;
    DeviceInformation(std::nullptr_t = nullptr) {}
    explicit DeviceInformation(std::wstring value) : id(std::move(value)) {}
    explicit operator bool() const { return !id.empty(); }
    std::wstring Id() const { return id; }
};
struct Event { int signals = 0; void SetEvent() { ++signals; } };
'''
    code += enums + '\n' + request
    # The initializer is read from the actual runtime struct, not duplicated here.
    needed = re.search(r'bool needed = (true|false)', HEADER)[1]
    code += r'''
struct Runtime {
    struct Bluetooth { bool needed = NEEDED; bool inProgress = false;
        BluetoothRecoveryResult lastResult = BluetoothRecoveryResult::NotRequired; } bluetooth;
    bool connectionWorkerRunning = true, unexpectedDisconnectRecoveryPending = false;
    bool reconnectEnabled = true, stopping = false;
    uint64_t nextConnectionAttemptId = 0;
    std::unordered_map<std::wstring, int> sessions;
    std::unordered_map<std::wstring, uint64_t> connectGenerations;
    std::unordered_set<std::wstring> pendingAutoReconnects;
    std::vector<std::wstring> desiredDevices;
    std::deque<ConnectionRequest> connectionQueue;
    Event connectionQueueChanged;
} g_app;
std::unordered_map<std::wstring, DeviceInformation> g_discoveredDevices;
bool IsStopping() { return g_app.stopping; }
void ProcessConnectionQueue() {}
bool PrepareForcedConnection(std::wstring const& id) { g_app.sessions.erase(id); return true; }
void ClearDeviceError(std::wstring const&) {}
void ApplySessionStatusToRow(std::wstring const&) {}
void RecordConnectionDiagnostic(uint64_t, std::wstring const&, std::wstring const&) {}
void MaybeQueuePendingReconnect(DeviceInformation const&);
void QueueConnection(DeviceInformation const&, ConnectionRequestMode);
void QueueConnection(std::wstring const&, ConnectionRequestMode);
'''.replace('NEEDED', needed)
    for name in ('ConnectionRequestModeName', 'ConnectionRecoveryStrategyName',
                 'SelectInitialRecoveryStrategy', 'SelectRetryRecoveryStrategy',
                 'IsConnectionQueued', 'IsRetryableConnectionResult', 'QueueConnectionInternal',
                 'QueueRememberedDevices', 'MaybeQueuePendingReconnect'):
        code += '\n' + function(name)
    for signature in (r'^void QueueConnection\(DeviceInformation const&', r'^void QueueConnection\(std::wstring const&'):
        # Select the definition rather than the top-of-file forward declaration.
        code += '\n' + block(CPP, signature + r'[^;]*?\)\n\{')
    if 'auto FindNextConnectionRequest()' in CPP:
        code += '\n' + function('FindNextConnectionRequest')
    else:
        # Baseline worker explicitly popped the front before considering deadlines.
        assert 'g_app.connectionQueue.front()' in function('ProcessConnectionQueue')
        code += '\nauto FindNextConnectionRequest() { return g_app.connectionQueue.begin(); }'
    code += r'''
int main() {
    int failed = 0, checked = 0;
    auto check = [&](bool ok, const char* name) {
        ++checked; if (!ok) { ++failed; std::cout << "FAIL " << name << '\n'; }
    };
    auto reset = [&] { g_app = Runtime{}; g_discoveredDevices.clear(); };
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct,
          "startup automatic connection never resets Bluetooth");
    g_app.bluetooth.needed = false;
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct,
          "successful recovery clears the startup reset requirement");
    g_app.bluetooth.needed = true;
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Quick) == ConnectionRecoveryStrategy::Direct, "quick stays direct");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Forced) == ConnectionRecoveryStrategy::RestartBluetooth, "forced starts with reset");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct, "automatic does not escalate known failure");
    for (auto mode : {ConnectionRequestMode::Quick, ConnectionRequestMode::Forced}) {
        ConnectionRequest req; req.mode = mode; req.recoveryStrategy = ConnectionRecoveryStrategy::RestartBluetooth;
        check(SelectRetryRecoveryStrategy(req, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
              "manual retries never add resets");
    }
    ConnectionRequest req;
    check(SelectRetryRecoveryStrategy(req, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
          "automatic direct failure stays direct");
    for (auto result : {BluetoothRecoveryResult::AccessDenied, BluetoothRecoveryResult::RadioUnavailable,
                        BluetoothRecoveryResult::TurnOffDenied, BluetoothRecoveryResult::TurnOnDenied}) {
        g_app.bluetooth.lastResult = result;
        check(IsRetryableConnectionResult(ConnectionAttemptResult::BluetoothRecoveryFailed), "failure remains retryable until user cancels");
    }
    g_app.bluetooth.lastResult = BluetoothRecoveryResult::TurnOffTimedOut;
    check(IsRetryableConnectionResult(ConnectionAttemptResult::BluetoothRecoveryFailed), "radio timeout remains retryable");
    check(!IsRetryableConnectionResult(ConnectionAttemptResult::Success), "success is terminal");
    check(!IsRetryableConnectionResult(ConnectionAttemptResult::Cancelled), "cancellation is terminal");
    reset(); g_app.pendingAutoReconnects.insert(L"phone");
    MaybeQueuePendingReconnect(DeviceInformation(L"phone"));
    check(g_app.connectionQueue.size() == 1, "discovery queues remembered phone");
    check(g_app.pendingAutoReconnects.count(L"phone") == 0, "accepted discovery consumes auto reconnect budget");
    g_app.connectionQueue.clear(); // simulate exhausting the bounded retry batch
    MaybeQueuePendingReconnect(DeviceInformation(L"phone"));
    check(g_app.connectionQueue.empty(), "unchanged reconciliation cannot restart exhausted retry batch");
    reset(); g_app.reconnectEnabled = false; g_app.desiredDevices = {L"phone"};
    g_discoveredDevices.emplace(L"phone", DeviceInformation(L"phone")); QueueRememberedDevices();
    check(g_app.connectionQueue.empty(), "disabled auto reconnect does not queue a startup reset");
    reset(); g_app.desiredDevices = {L"absent"}; QueueRememberedDevices();
    check(g_app.connectionQueue.empty(), "absent remembered device waits for discovery without radio reset");
    check(g_app.pendingAutoReconnects.count(L"absent") == 1, "absent remembered device is retained");
    MaybeQueuePendingReconnect(DeviceInformation(L"absent"));
    check(g_app.connectionQueue.size() == 1 &&
          g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::Direct,
          "late-discovered startup device stays direct");
    reset(); g_app.bluetooth.inProgress = true; g_app.pendingAutoReconnects.insert(L"phone");
    MaybeQueuePendingReconnect(DeviceInformation(L"phone"));
    check(g_app.connectionQueue.empty() && g_app.pendingAutoReconnects.count(L"phone") == 1,
          "busy adapter defers rather than consumes discovery intent");
    reset();
    QueueConnectionInternal(L"slow", nullptr, ConnectionRequestMode::Quick, 2, {}, {}, {},
        std::chrono::steady_clock::now() + std::chrono::seconds(60));
    QueueConnectionInternal(L"fast", nullptr, ConnectionRequestMode::Quick, 0);
    check(FindNextConnectionRequest()->deviceId == L"fast", "fresh click overtakes delayed retry");
    auto before = g_app.connectionQueueChanged.signals;
    QueueConnectionInternal(L"slow", nullptr, ConnectionRequestMode::Quick, 0);
    auto& promoted = g_app.connectionQueue.front();
    check(promoted.retryCount == 0 && promoted.notBefore == std::chrono::steady_clock::time_point{},
          "new click resets stale retry budget and deadline");
    check(g_app.connectionQueueChanged.signals > before, "new click wakes deferred worker");
    check(g_app.connectionQueue.size() == 2, "promotion never duplicates device");
    reset(); g_app.stopping = true;
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Forced, 0);
    check(g_app.connectionQueue.empty(), "shutdown rejects new requests");
    std::cout << "POLICY " << checked - failed << '/' << checked << " passed\n";
    return failed ? 1 : 0;
}
'''
    return code


def run():
    failures = 0
    with tempfile.TemporaryDirectory(prefix='aulay-tests-') as directory:
        source = Path(directory) / 'policy.cpp'
        source.write_text(harness(), encoding='utf-8')
        compiler = os.environ.get('CXX') or next((c for c in ('g++', 'clang++', 'cl') if shutil.which(c)), None)
        if not compiler:
            raise RuntimeError('Set CXX or run in a C++ compiler developer environment')
        binary = Path(directory) / ('policy.exe' if os.name == 'nt' else 'policy')
        if Path(compiler).stem.lower() == 'cl':
            command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', str(source), '/Fe:' + str(binary)]
        else:
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)]
        subprocess.run(command, cwd=directory, check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        print(result.stdout, end='')
        failures += result.returncode != 0

    worker = function('ProcessConnectionQueue')
    connect = function('ConnectDeviceAsync')
    recovery = function('EnsureBluetoothReady')
    checks = {
        'late Off always receives a compensating On request': '!offRequested && radio.State() == RadioState::On' in function('RunBluetoothRecoveryTransaction'),
        'attempt exceptions release placeholder sessions': 'if (FAILED(attemptError))' in worker and 'CloseConnectionSession(request.deviceId, request.generation, false)' in worker,
        'radio wait is asynchronous': 'resume_on_signal' in function('WaitForRadioState') and '->wait(' not in function('WaitForRadioState'),
        'recovery join uses completion signal': 'resume_on_signal(completion->signal.get())' in recovery,
        'forced reset waits for endpoint restore': 'AwaitBounded(AudioFlow::WaitFinished(task), deadline, cancellation)' in recovery,
        'unexpected close resets radio': 'EnsureBluetoothReady(true, true, deviceId)' in function('BeginUnexpectedDisconnectRecovery'),
        'unexpected reset is coalesced before closing sessions': function('HandleConnectionClosed').index('g_app.unexpectedDisconnectRecoveryPending = true') < function('HandleConnectionClosed').index('for (const auto& sessionId : sessionIds)'),
        'radio recovery skips redundant endpoint cleanup': '!g_app.unexpectedDisconnectRecoveryPending' in function('CloseConnectionSession'),
        'unexpected reconnect retains preference and generation guard': all(x in function('BeginUnexpectedDisconnectRecovery') for x in ('g_app.reconnectEnabled && IsDesiredDevice(deviceId)', 'g_app.connectGenerations[deviceId] == generation')),
        'new connection waits for old endpoint cleanup': 'AwaitBounded(AudioFlow::WaitFinished(previousTask), attemptDeadline, attemptCancellation)' in connect,
        'shutdown waits for endpoint restore': 'AwaitBounded(AudioFlow::WaitFinished(task), flowDrainDeadline)' in function('FinishShutdownWhenReady'),
        'connection request radio dispatch requires forced initial request': 'request.mode != ConnectionRequestMode::Forced || request.retryCount != 0' in connect,
        'queue wait is interruptible': 'resume_on_signal(g_app.connectionQueueChanged.get(), delay)' in worker,
        'shutdown wakes deferred queue': 'connectionQueueChanged.SetEvent()' in function('BeginShutdown'),
        'success has no unconditional stability timer': 'CONNECTION_POST_OPEN_STABILITY_MS' not in connect,
        'opened state uses event instead of polling': 'resume_on_signal(openedSignal->get()' in connect and 'CONNECTION_OPENED_POLL_MS' not in connect,
        'cancel wakes opened waiter': 'retired.openedSignal->SetEvent()' in function('CloseConnectionSession'),
        'forced recovery revokes established sessions before transaction': 'CloseConnectionSession(id, std::nullopt, false)' in recovery and recovery.index('CloseConnectionSession') < recovery.index('co_await RunBluetoothRecoveryTransaction'),
        'stale close callback checks live state': 'sender.State() != AudioPlaybackConnectionState::Closed' in function('HandleConnectionClosed'),
        'stale watcher callbacks are rejected': function('StartDeviceWatcher').count('g_deviceWatcher != sender') >= 5,
        'log writes do not force a disk flush': 'FlushFileBuffers(' not in function('WriteDiagnosticEntryLocked', DIAG),
        'shutdown still flushes logs': 'FlushFileBuffers(file->logFile.get())' in function('InitializeDiagnostics', DIAG) and 'g_diagnosticWriter->Stop()' in function('CloseDiagnostics', DIAG),
        'settings shutdown awaits worker before destroying window': 'co_await FlushPendingSettings();' in function('FinishShutdownWhenReady') and 'FlushPendingSettings' not in function('ShutdownApplication'),
        'settings shutdown wait pumps dispatcher and has a deadline': 'co_await winrt::resume_on_signal(g_settingsSaveIdle.get(), std::chrono::milliseconds(5000))' in function('FlushPendingSettings') and '.wait(' not in function('FlushPendingSettings'),
        'settings failure during shutdown has no modal dialog': 'saveFailed && !IsStopping()' in function('SaveSettingsWorker'),
        'settings retain durable atomic writes': all(x in (ROOT/'SettingsUtil.hpp').read_text() for x in ('FlushFileBuffers(', 'ReplaceFileW(', 'MoveFileExW(')),
        'tray and app share one icon source': re.findall(r'^IDI_\w+\s+ICON\s+"([^"]+)"', (ROOT/'Aulay.rc').read_text(encoding='utf-16'), re.M) == ['Aulay.ico', 'Aulay.ico'],
    }
    for name, passed in checks.items():
        if not passed:
            print('FAIL ' + name)
    failed = sum(not value for value in checks.values())
    failures += failed
    print(f'SOURCE_GUARDS {len(checks) - failed}/{len(checks)} passed')
    print('REGRESSION_RESULT=' + ('FAIL' if failures else 'PASS'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(run())
