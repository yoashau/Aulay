#!/usr/bin/env python3
"""Execute the production remote-close callback with a queued fake dispatcher."""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
shared = runpy.run_path(str(Path(__file__).with_name('test_regressions.py')))
function, block = shared['function'], shared['block']
header = (ROOT / 'Aulay.h').read_text()
code = r'''
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define LOG_CAUGHT_EXCEPTION() ((void)0)
namespace winrt::Windows::UI::Core { enum class CoreDispatcherPriority { High }; }
enum class AudioPlaybackConnectionState { Closed, Opened };
struct Link { AudioPlaybackConnectionState state=AudioPlaybackConnectionState::Closed; bool throws=false; };
struct AudioPlaybackConnection {
    std::shared_ptr<Link> link=std::make_shared<Link>();
    auto State() const { if(link->throws)throw std::runtime_error("disposed");return link->state; }
    bool operator!=(const AudioPlaybackConnection& other)const{return link!=other.link;}
};
'''
code += block(header, r'^enum class ConnectionPhase\b') + ';\n'
code += r'''
struct Session {
    uint64_t generation=12, attemptId=7;
    ConnectionPhase phase=ConnectionPhase::Connected;
    AudioPlaybackConnection connection;
    std::chrono::steady_clock::time_point connectedAt=std::chrono::steady_clock::now();
};
struct Runtime {
    struct { bool inProgress=false; } bluetooth;
    std::unordered_map<std::wstring,Session> sessions;
    std::unordered_set<std::wstring> pendingAutoReconnects,suppressedAutoReconnects,deferredBluetoothRecovery;
} g_app;
struct Dispatcher {
    std::vector<std::function<void()>> pending;
    int RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority,std::function<void()> f){pending.push_back(std::move(f));return 0;}
    void Drain(){auto work=std::move(pending);pending.clear();for(auto&f:work)f();}
} g_uiDispatcher;
bool stopping=false;
int closes=0,updates=0,reconciliations=0;
bool skippedCleanup=false;
bool IsStopping(){return stopping;}
int64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point){return 1000;}
std::wstring ConnectionPhaseName(ConnectionPhase){return L"opening";}
void RecordConnectionDiagnostic(uint64_t,std::wstring const&,std::wstring const&){}
void ClearDeviceError(std::wstring const&){}
void ApplySessionStatusToRow(std::wstring const&){++updates;}
void ScheduleDeviceReconciliation(){++reconciliations;}
void CloseConnectionSession(std::wstring const&id,std::optional<uint64_t> generation,bool mark,bool skip){
    assert(!mark&&generation==g_app.sessions.at(id).generation);
    ++closes;skippedCleanup=skip;g_app.sessions.erase(id);
}
'''
code += function('HandleConnectionClosed')
code += r'''
namespace Cleanup {
struct Event { int signals=0; void SetEvent(){++signals;} };
struct Cancellation { bool requested=false; void Request(){requested=true;} };
namespace AudioFlow {
struct State { bool done=false,cleanup=false,stop=false,closed=false; Event finishedSignal; };
int starts=0; bool failStart=false;
template<class Log> auto Start(std::wstring const&,Log,bool cleanupOnly,bool closedAlready){
    ++starts;assert(cleanupOnly&&!closedAlready);if(failStart)throw std::runtime_error("launch");
    return std::make_shared<State>();
}
void RequestStop(State&s){s.stop=true;}
}
struct ConnectionSession {
    uint64_t generation=12,attemptId=7;
    ConnectionPhase phase=ConnectionPhase::Connected;
    std::shared_ptr<AudioFlow::State> audioFlow;
    std::shared_ptr<Cancellation> cancellation=std::make_shared<Cancellation>();
    std::shared_ptr<Event> openedSignal=std::make_shared<Event>();
};
struct Runtime {
    struct {bool inProgress=false,needed=false;} bluetooth;
    std::unordered_map<std::wstring,ConnectionSession> sessions;
} g_app;
std::unordered_map<std::wstring,std::shared_ptr<AudioFlow::State>> g_audioFlowTasks;
bool stopping=false,worker=false;int backgroundCloses=0;
std::shared_ptr<AudioFlow::State> previousFlow;
bool IsStopping(){return stopping;}
bool HasConnectedSessions(){return !g_app.sessions.empty();}
void RecordDebugConnectionSnapshot(std::wstring const&,std::wstring_view,int,bool){}
void RecordConnectionDiagnostic(uint64_t,std::wstring const&,std::wstring const&){}
void CloseSessionInBackground(ConnectionSession retired,std::wstring id,std::shared_ptr<AudioFlow::State> flow,
    bool workerStarted,std::shared_ptr<AudioFlow::State> predecessor){
    assert(!g_app.sessions.count(id)&&retired.cancellation->requested&&retired.openedSignal->signals==1);
    ++backgroundCloses;worker=workerStarted;previousFlow=predecessor;
    flow->closed=true;if(!workerStarted){flow->done=true;flow->finishedSignal.SetEvent();}
}
'''
code += function('CloseConnectionSession')
code += r'''
void CheckPaths(){
    auto reset=[] {g_app={};g_audioFlowTasks.clear();stopping=false;AudioFlow::starts=0;
        AudioFlow::failStart=false;backgroundCloses=0;worker=false;previousFlow.reset();g_app.sessions[L"phone"]={};};
    reset();g_app.sessions[L"phone"].phase=ConnectionPhase::Starting;
    assert(!CloseConnectionSession(L"phone",12,false,false));auto flow=g_audioFlowTasks.at(L"phone");
    assert(AudioFlow::starts==0&&backgroundCloses==1&&!worker&&flow->done&&flow->closed&&!flow->cleanup&&flow->finishedSignal.signals==1);
    reset();assert(CloseConnectionSession(L"phone",12,false,true));
    assert(AudioFlow::starts==0&&!worker&&!g_audioFlowTasks.at(L"phone")->cleanup);
    reset();assert(CloseConnectionSession(L"phone",12,false,false));
    assert(AudioFlow::starts==1&&worker&&g_audioFlowTasks.at(L"phone")->cleanup);
    reset();auto live=std::make_shared<AudioFlow::State>();g_app.sessions[L"phone"].audioFlow=live;g_audioFlowTasks[L"phone"]=live;
    assert(CloseConnectionSession(L"phone",12,false,true));
    assert(AudioFlow::starts==0&&worker&&g_audioFlowTasks.at(L"phone")==live&&live->stop&&live->closed&&!live->done&&previousFlow==live);
    reset();auto finished=std::make_shared<AudioFlow::State>();finished->done=true;g_app.sessions[L"phone"].audioFlow=finished;g_audioFlowTasks[L"phone"]=finished;
    assert(CloseConnectionSession(L"phone",12,false,true));
    assert(AudioFlow::starts==0&&!worker&&g_audioFlowTasks.at(L"phone")!=finished&&previousFlow==finished);
    reset();auto pending=std::make_shared<AudioFlow::State>();g_audioFlowTasks[L"phone"]=pending;
    g_app.sessions[L"phone"].phase=ConnectionPhase::Opening;
    assert(!CloseConnectionSession(L"phone",12,false,false));
    assert(AudioFlow::starts==0&&previousFlow==pending&&g_audioFlowTasks.at(L"phone")!=pending);
    reset();stopping=true;assert(CloseConnectionSession(L"phone",12,false,false));assert(AudioFlow::starts==0&&!worker);
    reset();g_app.bluetooth.inProgress=true;assert(CloseConnectionSession(L"phone",12,false,false));assert(AudioFlow::starts==0&&!worker);
    reset();AudioFlow::failStart=true;assert(CloseConnectionSession(L"phone",12,false,false));
    assert(AudioFlow::starts==1&&!worker&&g_audioFlowTasks.at(L"phone")->done&&g_audioFlowTasks.at(L"phone")->cleanup);
    reset();assert(!CloseConnectionSession(L"phone",13,false,false)&&g_app.sessions.count(L"phone")&&backgroundCloses==0);
    assert(!CloseConnectionSession(L"missing",12,false,false)&&backgroundCloses==0);
    std::cout<<"CLOSE_CLEANUP PASS cases=11 startup/skipped/required/live/finished/predecessor/shutdown/radio/launch-failure/stale/missing\n";
}
}
'''
code += r'''
int main(){
    Cleanup::CheckPaths();
    auto reset=[] {g_app={};g_uiDispatcher.pending.clear();stopping=false;closes=updates=reconciliations=0;skippedCleanup=false;g_app.sessions[L"phone"]={};};
    auto notify=[] {HandleConnectionClosed(L"phone",12,g_app.sessions.at(L"phone").connection);};
    reset();g_app.sessions[L"other"]={};g_app.pendingAutoReconnects={L"phone",L"other"};
    notify();notify();assert(closes==0);g_uiDispatcher.Drain();
    assert(closes==1&&skippedCleanup&&updates==1&&reconciliations==1);
    assert(!g_app.sessions.count(L"phone")&&g_app.sessions.count(L"other"));
    assert(g_app.suppressedAutoReconnects==std::unordered_set<std::wstring>{L"phone"});
    assert(g_app.pendingAutoReconnects==std::unordered_set<std::wstring>{L"other"});
    assert(g_app.deferredBluetoothRecovery==std::unordered_set<std::wstring>{L"phone"});
    reset();notify();g_app.sessions[L"phone"].generation=13;g_uiDispatcher.Drain();assert(closes==0&&g_app.suppressedAutoReconnects.empty());
    reset();HandleConnectionClosed(L"phone",12,AudioPlaybackConnection{});g_uiDispatcher.Drain();assert(closes==0);
    reset();notify();g_app.sessions[L"phone"].connection.link->state=AudioPlaybackConnectionState::Opened;g_uiDispatcher.Drain();assert(closes==0&&g_app.suppressedAutoReconnects.empty());
    reset();g_app.sessions[L"phone"].phase=ConnectionPhase::Opening;notify();g_uiDispatcher.Drain();assert(closes==0&&g_app.suppressedAutoReconnects.empty());
    reset();notify();stopping=true;g_uiDispatcher.Drain();assert(closes==0);
    reset();stopping=true;notify();assert(g_uiDispatcher.pending.empty());
    reset();notify();g_app.sessions.erase(L"phone");g_uiDispatcher.Drain();assert(closes==0);
    reset();g_app.sessions[L"phone"].connection.link->throws=true;notify();g_uiDispatcher.Drain();assert(closes==1&&skippedCleanup);
    reset();g_app.bluetooth.inProgress=true;notify();g_uiDispatcher.Drain();assert(closes==1&&skippedCleanup&&g_app.suppressedAutoReconnects.empty());
    std::cout<<"REMOTE_CLOSE PASS cases=10 duplicate/stale/reopened/opening/shutdown/disposed/other-device/manual-reset\n";
}
'''
with tempfile.TemporaryDirectory(prefix='aulay-remote-close-') as tmp:
    source = Path(tmp) / 'remote-close.cpp'
    source.write_text(code)
    compiler = os.environ.get('CXX') or next((c for c in ('g++', 'clang++', 'cl') if shutil.which(c)), None)
    if not compiler:
        raise RuntimeError('C++ compiler required')
    exe = Path(tmp) / ('remote-close.exe' if os.name == 'nt' else 'remote-close')
    cmd = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)]
    if Path(compiler).stem.lower() == 'cl':
        cmd = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', str(source), '/Fe:' + str(exe)]
    subprocess.run(cmd, cwd=tmp, check=True)
    subprocess.run([str(exe)], check=True)
