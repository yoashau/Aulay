#!/usr/bin/env python3
"""Execute actual diagnostic marker/disconnect functions with fake UI/connection.
No radio or audio device is changed. Also checks debug/normal build separation.
"""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
APP = ''.join((ROOT/name).read_text(encoding='utf-8-sig') for name in ('Aulay.cpp', 'AppModules.hpp', 'ConnectionState.hpp', 'DeviceListUi.hpp', 'BluetoothRecovery.hpp', 'ConnectionFlow.hpp', 'DeviceWatcher.hpp', 'TrayPanel.hpp'))
if 'void MarkNoSound(' not in APP:
    print('DEBUG_MARKERS_RESULT=FAIL missing diagnostic marker implementation')
    sys.exit(1)
shared = runpy.run_path(str(Path(__file__).with_name('test_regressions.py')))
block = shared['block']
function = shared['function']
HEADER = (ROOT/'Aulay.h').read_text(encoding='utf-8-sig')
code = r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#define AULAY_DIAGNOSTIC_BUILD 1
#define _(s) s
using HRESULT = int32_t;
namespace winrt { HRESULT to_hresult() { return -1; } }
std::wstring FormatDiagnosticHresult(HRESULT) { return L"test-error"; }
uint64_t clockTick = 1000;
uint64_t GetTickCount64() { return clockTick; }
enum class AudioPlaybackConnectionState { Closed, Opened };
struct FakeConnection {
    AudioPlaybackConnectionState state = AudioPlaybackConnectionState::Opened;
    explicit operator bool() const { return true; }
    auto State() const { return state; }
};
'''
for name in ('ConnectionPhase','ConnectionRequestMode','ConnectionRecoveryStrategy','BluetoothRecoveryStage','BluetoothRecoveryResult'):
    code += block(HEADER, '^enum class '+name+r'\b')+';\n'
code += r'''
struct Session {
    uint64_t attemptId=7, generation=12;
    uint32_t noSoundReports=0;
    FakeConnection connection;
    ConnectionPhase phase=ConnectionPhase::Connected;
    ConnectionRequestMode requestMode=ConnectionRequestMode::Quick;
    std::chrono::steady_clock::time_point connectedAt=std::chrono::steady_clock::now();
};
struct ConnectionRequest { std::wstring deviceId; };
struct App {
    struct { void SetEvent() {} } connectionQueueChanged;
    std::unordered_map<std::wstring, Session> sessions;
    std::unordered_map<std::wstring,uint64_t> connectGenerations;
    std::deque<ConnectionRequest> connectionQueue;
    std::unordered_map<std::wstring,int> pendingAutoReconnects;
    struct { BluetoothRecoveryStage stage=BluetoothRecoveryStage::Idle;
        BluetoothRecoveryResult lastResult=BluetoothRecoveryResult::NotRequired; } bluetooth;
} g_app;
bool stopping=false;
int uiUpdates=0, closeCalls=0, forcedRequests=0;
void QueueConnection(std::wstring const&,ConnectionRequestMode mode) { if(mode==ConnectionRequestMode::Forced)++forcedRequests; }
std::vector<std::wstring> logs, snapshots;
bool IsStopping() { return stopping; }
void RecordConnectionDiagnostic(uint64_t attempt, std::wstring_view id, std::wstring_view msg) {
    logs.push_back(L"attempt="+std::to_wstring(attempt)+L" device="+std::wstring(id)+L" "+std::wstring(msg));
}
void RecordDiagnostic(std::wstring_view, std::wstring_view) {}
std::wstring DiagnosticDeviceToken(std::wstring const& id) { return id; }
void ApplySessionStatusToRow(std::wstring const&) { ++uiUpdates; }
std::wstring DebugAudioMonitorStatus() { return L"test-monitor"; }
void RequestDebugAudioSnapshot(std::wstring trace) { snapshots.push_back(std::move(trace)); }
void ForgetDevice(std::wstring const&) {}
void ClearDeviceError(std::wstring const&) {}
void ScheduleDeviceReconciliation() {}
void UpdateDeviceRowStatus(std::wstring const&, std::wstring_view, std::wstring_view, bool) {}
void CloseConnectionSession(std::wstring const& id, std::optional<uint64_t>, bool) {
    ++closeCalls; g_app.sessions.erase(id);
}
'''
code += block(APP,r'^struct DebugDisconnectRecord\b')+';\n'
code += r'''
std::unordered_map<std::wstring,DebugDisconnectRecord> g_debugLastDisconnect;
uint64_t g_debugNextMarker=0;
'''
for name in ('ConnectionPhaseName','ConnectionRequestModeName','AudioConnectionStateName',
             'BluetoothStageName','BluetoothRecoveryResultName','ElapsedMilliseconds',
             'RecordDebugConnectionSnapshot','MarkNoSound','DisconnectDevice'):
    code += function(name)+'\n'
code += r'''
int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* label){ ++checks;if(!ok){++failed;std::cout<<"FAIL "<<label<<'\n';} };
    auto has=[&](std::wstring_view value){return !logs.empty() && logs.back().find(value)!=std::wstring::npos;};
    MarkNoSound(L"missing");check(logs.empty() && g_app.sessions.empty(),"missing session ignored");
    g_app.sessions[L"phone"]={};
    g_app.sessions[L"phone"].phase=ConnectionPhase::Opening;
    MarkNoSound(L"phone");check(logs.empty(),"opening session ignored");
    g_app.sessions[L"phone"].phase=ConnectionPhase::Connected;
    MarkNoSound(L"phone");
    check(has(L"USER_NO_SOUND marker=1"),"explicit user marker recorded");
    check(has(L"attempt=7 device=phone") && has(L"generation=12 mode=quick"),"attempt and generation correlated");
    check(has(L"previous-disconnect=none-in-this-run"),"no invented prior disconnect");
    check(forcedRequests==1 && closeCalls==0,"marker queues shared Forced entry without manual disconnect");
    check(snapshots.size()==1 && snapshots.back().find(L"marker=1")!=std::wstring::npos,"snapshot trace keeps marker identity");
    MarkNoSound(L"phone");check(has(L"marker=2") && has(L"reports=2"),"repeated marker uniquely numbered");
    DisconnectDevice(L"phone");check(g_app.sessions.empty() && closeCalls==1,"real manual disconnect path closes once");
    check(g_debugLastDisconnect.at(L"phone").attemptId==7,"manual disconnect remembers old attempt");
    clockTick+=500;
    g_app.sessions[L"phone"]={};g_app.sessions[L"phone"].attemptId=8;g_app.sessions[L"phone"].generation=13;
    MarkNoSound(L"phone");
    check(has(L"attempt=8") && has(L"previous-disconnect-attempt=7") && has(L"since-disconnect-ms=500"),"reconnect silence linked to old disconnect");
    check(has(L"reports=1"),"new session starts a fresh report count");
    g_app.sessions[L"second"]={};MarkNoSound(L"second");
    check(has(L"previous-disconnect=none-in-this-run"),"different device never inherits disconnect");
    stopping=true;auto before=logs.size();MarkNoSound(L"phone");check(logs.size()==before,"shutdown ignores marker");
    stopping=false;g_app.sessions[L"phone"].connection.state=AudioPlaybackConnectionState::Closed;MarkNoSound(L"phone");
    check(has(L"state=closed"),"captures actual state even when UI has not caught up");
    std::cout<<"DEBUG_MARKERS checks="<<checks<<" failures="<<failed<<'\n';
    return failed?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='aulay-debug-test-') as tmp:
    tmp=Path(tmp);source=tmp/'markers.cpp';source.write_text(code)
    compiler=os.environ.get('CXX') or next((c for c in ('g++','clang++','cl') if shutil.which(c)),None)
    if not compiler: raise RuntimeError('C++ compiler required')
    exe=tmp/('markers.exe' if os.name=='nt' else 'markers')
    cmd=[compiler,'-std=c++17','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)]
    if Path(compiler).stem.lower()=='cl':cmd=[compiler,'/nologo','/std:c++17','/EHsc','/utf-8',str(source),'/Fe:'+str(exe)]
    subprocess.run(cmd,cwd=tmp,check=True)
    subprocess.run([str(exe)],check=True)

variant=(ROOT/'BuildVariant.hpp').read_text()
collector=(ROOT/'DebugAudioDiagnostics.hpp').read_text()
project=(ROOT/'Aulay.vcxproj').read_text()
checks={
    'collector always compiled':'#ifdef AULAY_DIAGNOSTIC_BUILD' not in collector,
    'collector is read only':not any(x in collector for x in ('SetMute(', 'SetMasterVolume(', 'SetStateAsync(', 'OpenAsync(', 'StartAsync(')),
    'collector has no app globals':'g_app.' not in collector and 'g_uiDispatcher' not in collector,
    'collector says zero peak not an audibility verdict':'not-proof-of-audibility' in collector,
    'on-click no connection wait':'resume_after' not in function('MarkNoSound') and 'co_await' not in function('MarkNoSound'),
    'capture sent to independent background worker':'EnqueueDebugAudioEvent(trace, true' in function('RequestDebugAudioSnapshot') and 'std::thread([s]' in (ROOT/'DebugAudioMonitor.hpp').read_text(),
    'connected layout exposes adjacent marker':'true, false, true);' in function('ApplySessionStatusToRow'),
    'button text reset on disconnect':'showNoSound ? _(L"No sound") : _(L"Force Connect")' in APP,
    'one executable variant':'AulayDebug$(PlatformArchitecture)' not in project and 'AulayDiagnostic' not in project,
    'unified settings and log namespace':'L"Aulay.json"' in variant and 'L"Aulay"' in variant,
}
for label,ok in checks.items():
    if not ok: print('FAIL '+label)
print(f'DEBUG_SOURCE_GUARDS {sum(checks.values())}/{len(checks)} passed')
print('DEBUG_MARKERS_RESULT='+('PASS' if all(checks.values()) else 'FAIL'))
sys.exit(0 if all(checks.values()) else 1)
