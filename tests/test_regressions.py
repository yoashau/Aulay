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
    bool connectionWorkerRunning = true;
    bool reconnectEnabled = true, stopping = false;
    uint64_t nextConnectionAttemptId = 0;
    std::unordered_map<std::wstring, int> sessions;
    std::unordered_map<std::wstring, uint64_t> connectGenerations;
    std::unordered_set<std::wstring> pendingAutoReconnects, suppressedAutoReconnects, deferredBluetoothRecovery, preparedDevices;
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
                 'SelectInitialRecoveryStrategy', 'SelectRetryRecoveryStrategy', 'CanRetryConnection',
                 'IsConnectionQueued', 'QueueConnectionInternal',
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
    lookup_condition = re.search(r'^\tif \((!device \|\| recoveryResult[^\n]+)\) \{',
                                 function('ConnectDeviceAsync'), re.M)[1]
    code += '\nbool NeedsDeviceLookup(DeviceInformation device, BluetoothRecoveryResult recoveryResult, [[maybe_unused]] bool resetRequested) { return ' + lookup_condition + '; }\n'
    code += r'''
int main() {
    int failed = 0, checked = 0;
    auto check = [&](bool ok, const char* name) {
        ++checked; if (!ok) { ++failed; std::cout << "FAIL " << name << '\n'; }
    };
    auto reset = [&] { g_app = Runtime{}; g_discoveredDevices.clear(); };
    check(!NeedsDeviceLookup(DeviceInformation{L"phone"}, BluetoothRecoveryResult::NotRequired, true),
          "protecting another stream does not re-query a valid snapshot");
    check(NeedsDeviceLookup(DeviceInformation{nullptr}, BluetoothRecoveryResult::NotRequired, true),
          "a missing snapshot is queried even when recovery is skipped");
    check(NeedsDeviceLookup(DeviceInformation{L"phone"}, BluetoothRecoveryResult::Success, true),
          "actual radio preparation always refreshes the snapshot");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct,
          "prepared automatic connection stays direct");
    g_app.bluetooth.needed = false;
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct,
          "already prepared automatic connection never resets again");
    g_app.bluetooth.needed = true;
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Quick) == ConnectionRecoveryStrategy::Direct, "quick stays direct");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Forced) == ConnectionRecoveryStrategy::RestartBluetooth, "forced starts with reset");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic) == ConnectionRecoveryStrategy::Direct, "automatic does not escalate known failure");
    for (auto mode : {ConnectionRequestMode::Quick, ConnectionRequestMode::Forced}) {
        ConnectionRequest req; req.mode = mode; req.recoveryStrategy = ConnectionRecoveryStrategy::Direct; req.preparationResult = BluetoothRecoveryResult::Success;
        check(SelectRetryRecoveryStrategy(req, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
              "manual retries never add resets");
    }
    ConnectionRequest req;
    check(SelectRetryRecoveryStrategy(req, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
          "automatic direct failure stays direct");
    for (auto result : {BluetoothRecoveryResult::AccessDenied, BluetoothRecoveryResult::RadioUnavailable,
                        BluetoothRecoveryResult::TurnOffDenied, BluetoothRecoveryResult::TurnOnDenied}) {
        g_app.bluetooth.lastResult = result;
        req.recoveryStrategy = ConnectionRecoveryStrategy::RestartBluetooth; req.preparationAttempts = 1; req.preparationResult = result;
        check(CanRetryConnection(req, ConnectionAttemptResult::BluetoothRecoveryFailed) == (result == BluetoothRecoveryResult::RadioUnavailable), "denials stop while unavailable hardware permits one retry");
    }
    g_app.bluetooth.lastResult = BluetoothRecoveryResult::TurnOffTimedOut;
    req.preparationResult = BluetoothRecoveryResult::TurnOffTimedOut;
    check(CanRetryConnection(req, ConnectionAttemptResult::BluetoothRecoveryFailed), "radio timeout permits one preparation retry");
    check(!CanRetryConnection(req, ConnectionAttemptResult::Success), "success is terminal");
    check(!CanRetryConnection(req, ConnectionAttemptResult::Cancelled), "cancellation is terminal");
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
          g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth &&
          g_app.connectionQueue.front().prepareBeforeConnect,
          "late-discovered startup request includes one-time preparation");
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

    reset(); g_app.suppressedAutoReconnects.insert(L"phone"); g_app.pendingAutoReconnects.insert(L"phone");
    MaybeQueuePendingReconnect(DeviceInformation(L"phone"));
    check(g_app.connectionQueue.empty(), "remote-closed device is not reopened by rediscovery");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Automatic, 3);
    check(g_app.connectionQueue.empty(), "automatic retry cannot bypass remote-close suppression");
    QueueConnectionInternal(L"other", nullptr, ConnectionRequestMode::Automatic, 0);
    check(g_app.connectionQueue.size() == 1, "other remembered devices still reconnect");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Quick, 0);
    check(g_app.connectionQueue.size() == 2 && g_app.suppressedAutoReconnects.empty(), "explicit quick connect resumes playback");
    reset(); g_app.suppressedAutoReconnects.insert(L"phone");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Forced, 0);
    check(g_app.connectionQueue.size() == 1 && g_app.suppressedAutoReconnects.empty(), "explicit force connect resumes playback");

    reset(); g_app.deferredBluetoothRecovery.insert(L"phone");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Quick, 0);
    auto repair = g_app.connectionQueue.front();
    check(repair.recoverAfterRemoteClose && repair.recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth, "known remote close is cleaned before explicit reconnect");
    repair.recoveryStrategy = ConnectionRecoveryStrategy::Direct; repair.preparationResult = BluetoothRecoveryResult::Success;
    for (auto result : {ConnectionAttemptResult::StartFailed, ConnectionAttemptResult::OpenTimedOut, ConnectionAttemptResult::OpenFailed, ConnectionAttemptResult::ClosedUnexpectedly, ConnectionAttemptResult::DeadlineExceeded})
        check(SelectRetryRecoveryStrategy(repair, result) == ConnectionRecoveryStrategy::Direct, "failed reconnect never repeats initial cleanup");
    check(SelectRetryRecoveryStrategy(repair, ConnectionAttemptResult::DeniedBySystem) == ConnectionRecoveryStrategy::Direct, "permission denial never repeats initial cleanup");
    repair.retryCount = 1;
    check(SelectRetryRecoveryStrategy(repair, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct, "deferred reset is limited to one retry");
    repair.retryCount = 0; repair.mode = ConnectionRequestMode::Automatic;
    check(SelectRetryRecoveryStrategy(repair, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct, "automatic request cannot consume deferred reset");
    g_app.connectionQueue.clear(); repair.mode = ConnectionRequestMode::Quick;
    QueueConnectionInternal(L"phone", nullptr, repair.mode, 1, repair.generation, repair.attemptId,
        ConnectionRecoveryStrategy::RestartBluetooth, {}, repair.recoverAfterRemoteClose);
    check(g_app.connectionQueue.front().recoverAfterRemoteClose, "retry preserves the explicit request's repair state");
    g_app.connectionQueue.clear(); g_app.deferredBluetoothRecovery.clear(); g_app.preparedDevices.insert(L"phone");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Quick, 0);
    check(!g_app.connectionQueue.front().recoverAfterRemoteClose && !g_app.connectionQueue.front().prepareBeforeConnect
          && g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::Direct, "prepared quick connection does not add a reset");

    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic, true) == ConnectionRecoveryStrategy::Direct,
          "startup cannot consume deferred cleanup even when marked");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Quick, true) == ConnectionRecoveryStrategy::RestartBluetooth,
          "known stale state does not depend on OpenAsync reporting failure");
    reset();
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Automatic, 0);
    g_app.deferredBluetoothRecovery.insert(L"phone");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Quick, 0);
    check(g_app.connectionQueue.size() == 1 && g_app.connectionQueue.front().recoverAfterRemoteClose
          && g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth,
          "promoted user click picks up pending cleanup");
    check(g_app.deferredBluetoothRecovery.count(L"phone") == 1,
          "queuing and promotion do not prematurely discard cleanup");

    reset(); g_app.desiredDevices={L"phone"};
    g_discoveredDevices.emplace(L"phone", DeviceInformation(L"phone"));
    QueueRememberedDevices();
    auto startup = g_app.connectionQueue.front();
    check(startup.mode == ConnectionRequestMode::Automatic && startup.prepareBeforeConnect
          && startup.recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth,
          "cold startup remembered connection prepares before first open");
    check(g_app.preparedDevices.empty(), "queuing does not mark startup preparation successful");
    g_app.connectionQueue.clear();
    QueueConnectionInternal(L"phone", nullptr, startup.mode, 1, startup.generation, startup.attemptId,
        ConnectionRecoveryStrategy::Direct, {}, startup.recoverAfterRemoteClose, startup.prepareBeforeConnect);
    check(g_app.connectionQueue.front().prepareBeforeConnect
          && g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::Direct,
          "startup retry retains verification context without repeating the reset");
    reset(); g_app.preparedDevices.insert(L"phone");
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Automatic, 0);
    check(!g_app.connectionQueue.front().prepareBeforeConnect
          && g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::Direct,
          "prepared background request has no startup recovery authorization");
    reset();
    QueueConnectionInternal(L"phone", nullptr, ConnectionRequestMode::Quick, 0);
    check(g_app.connectionQueue.front().prepareBeforeConnect
          && g_app.connectionQueue.front().recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth,
          "first manual connection also prepares stale state from previous process");
    auto firstManual = g_app.connectionQueue.front(); firstManual.retryCount=1;
    firstManual.recoveryStrategy=ConnectionRecoveryStrategy::Direct; firstManual.preparationResult=BluetoothRecoveryResult::Success;
    check(SelectRetryRecoveryStrategy(firstManual, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
          "first manual request spends preparation only once");
    check(SelectInitialRecoveryStrategy(ConnectionRequestMode::Automatic, false, true) == ConnectionRecoveryStrategy::RestartBluetooth,
          "initial startup preparation is independent of an API failure");

    for (auto mode : {ConnectionRequestMode::Automatic, ConnectionRequestMode::Quick, ConnectionRequestMode::Forced}) {
        reset();
        QueueConnectionInternal(L"phone", nullptr, mode, 0);
        auto request = g_app.connectionQueue.front();
        request.preparationAttempts = 1;
        request.preparationResult = BluetoothRecoveryResult::TurnOffTimedOut;
        check(CanRetryConnection(request, ConnectionAttemptResult::BluetoothRecoveryFailed), "transient preparation failure gets one retry");
        check(SelectRetryRecoveryStrategy(request, ConnectionAttemptResult::BluetoothRecoveryFailed) == ConnectionRecoveryStrategy::RestartBluetooth,
              "failed preparation is never replaced by direct connection");
        g_app.connectionQueue.clear();
        QueueConnectionInternal(L"phone", nullptr, mode, 1, request.generation, request.attemptId,
            SelectRetryRecoveryStrategy(request, ConnectionAttemptResult::BluetoothRecoveryFailed), {},
            request.recoverAfterRemoteClose, request.prepareBeforeConnect, request.preparationAttempts, request.preparationResult);
        auto retry = g_app.connectionQueue.front();
        check(retry.preparationAttempts == 1 && retry.preparationResult == BluetoothRecoveryResult::TurnOffTimedOut,
              "preparation retry retains failure history");
        retry.preparationAttempts = 2;
        check(!CanRetryConnection(retry, ConnectionAttemptResult::BluetoothRecoveryFailed), "two failed preparations stop this request");
        retry.preparationResult = BluetoothRecoveryResult::Success; retry.recoveryStrategy = ConnectionRecoveryStrategy::Direct;
        check(CanRetryConnection(retry, ConnectionAttemptResult::OpenFailed) && SelectRetryRecoveryStrategy(retry, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
              "successful preparation leaves offline-phone connection retries intact");
        for (auto failure : {BluetoothRecoveryResult::AccessDenied, BluetoothRecoveryResult::TurnOffDenied, BluetoothRecoveryResult::TurnOnDenied, BluetoothRecoveryResult::Cancelled}) {
            request.preparationResult = failure;
            check(!CanRetryConnection(request, ConnectionAttemptResult::BluetoothRecoveryFailed), "denial and cancellation do not repeat preparation");
        }
        request.preparationAttempts = 0; request.preparationResult = BluetoothRecoveryResult::NotRequired;
        check(CanRetryConnection(request, ConnectionAttemptResult::DeadlineExceeded) && SelectRetryRecoveryStrategy(request, ConnectionAttemptResult::DeadlineExceeded) == ConnectionRecoveryStrategy::RestartBluetooth,
              "failure before preparation preserves preparation obligation");
        request.preparationAttempts=1; request.recoveryStrategy=ConnectionRecoveryStrategy::Direct;
        check(SelectRetryRecoveryStrategy(request, ConnectionAttemptResult::OpenFailed) == ConnectionRecoveryStrategy::Direct,
              "protected active playback is not reset on later retries");
        check(!CanRetryConnection(request, ConnectionAttemptResult::DeniedBySystem), "Open permission denial stops retrying");
    }
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
        'remote close never resets radio or queues playback': all(x not in function('HandleConnectionClosed') for x in ('EnsureBluetoothReady', 'BeginUnexpectedDisconnectRecovery', 'QueueConnection(')),
        'remote close skips endpoint cleanup': 'CloseConnectionSession(deviceId, generation, false, true)' in function('HandleConnectionClosed'),
        'startup automatic connection receives initial verification': 'if (request.mode != ConnectionRequestMode::Automatic || request.prepareBeforeConnect) {' in connect,
        'startup verification checks live connection': '[connection = currentConnection] { return connection.State() == AudioPlaybackConnectionState::Opened; }' in connect,
        'new connection waits for old endpoint cleanup': 'AwaitBounded(AudioFlow::WaitFinished(previousTask), attemptDeadline, attemptCancellation)' in connect,
        'shutdown waits for endpoint restore': 'AwaitBounded(AudioFlow::WaitFinished(task), flowDrainDeadline)' in function('FinishShutdownWhenReady'),
        'radio dispatch consumes actual preparation result': all(x in connect for x in ('bool resetRequested = request.recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth;', 'request.preparationAttempts == 0', '++request.preparationAttempts;', 'request.preparationResult = recoveryResult;', 'recoveryResult == BluetoothRecoveryResult::Success || recoveryResult == BluetoothRecoveryResult::NotRequired', 'auto allowActiveSessionReset = forcedReset;')),
        'cleanup precedes opening the audio link': connect.index('co_await EnsureBluetoothReady(') < connect.index('OpenAsync requested'),
        'Opened alone cannot clear pending cleanup': 'g_app.deferredBluetoothRecovery.erase' not in connect and 'if (g_app.bluetooth.lastSucceeded) {\n\t\tg_app.deferredBluetoothRecovery.erase(deviceId);' in function('EnsureBluetoothReady'),
        'only completed cleanup marks a device prepared': 'g_app.preparedDevices.insert' not in connect and 'g_app.preparedDevices.insert(deviceId);' in recovery and 'if (g_app.bluetooth.lastSucceeded)' in recovery,
        'failed explicit preparation invalidates old success': 'g_app.preparedDevices.erase(deviceId);' in connect and connect.index('g_app.preparedDevices.erase(deviceId);') < connect.index('co_await EnsureBluetoothReady('),
        'startup verification context survives retry': 'request.recoverAfterRemoteClose,\n\t\t\t\t\trequest.prepareBeforeConnect,\n\t\t\t\t\trequest.preparationAttempts,\n\t\t\t\t\trequest.preparationResult);' in worker,
        'queue owns preparation progress until awaited connect returns': 'ConnectDeviceAsync(ConnectionRequest& request)' in connect and 'co_await ConnectDeviceAsync(request)' in worker,
        'creation has cancellation and deadline protection': 'AwaitBounded(ConnectionCreation::WaitReady(creation), attemptDeadline, attemptCancellation)' in connect and 'abandonCreation.release();' in connect,
        'a busy old creation blocks only its own device before recovery': connect.index('!existingCreation->second->finished') < connect.index('co_await EnsureBluetoothReady(') and 'ConnectionAttemptResult::DeviceUnavailable' in connect.split('auto device = request.device;',1)[0],
        'late creation cannot synchronously close on the UI': 'currentConnection.Close();' not in connect and 'ConnectionCreation::Take(*creation)' in connect,
        'failure hints and force purpose are reachable by tooltip': 'ToolTipService::SetToolTip(row->second.statusText' in function('UpdateDeviceRowStatus') and 'This cannot turn on Bluetooth on your phone.' in CPP,
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
        'settings shutdown awaits worker before destroying window': 'auto settingsDrain = FlushPendingSettings();' in function('FinishShutdownWhenReady') and 'co_await settingsDrain;' in function('FinishShutdownWhenReady') and 'FlushPendingSettings' not in function('ShutdownApplication'),
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
