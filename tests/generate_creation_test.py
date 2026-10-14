#!/usr/bin/env python3
"""Test the actual creation worker with controlled synchronous create/close calls."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
helper = (root / 'ConnectionCreation.hpp').read_text()
alias = 'using winrt::Windows::Media::Audio::AudioPlaybackConnection;'
call = 'AudioPlaybackConnection::TryCreateFromId(id)'
assert helper.count(alias) == helper.count(call) == 1
helper = helper.replace(alias, 'using AudioPlaybackConnection = TestConnection;')
helper = helper.replace(call, 'TestCreate(id)').replace('#pragma once', '')
code = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.h>
#include "AsyncUtil.hpp"
using namespace std::chrono;
struct Scenario {
    wil::unique_event entered{wil::EventOptions::ManualReset}, returnAllowed{wil::EventOptions::ManualReset};
    wil::unique_event closeEntered{wil::EventOptions::ManualReset}, closeAllowed{wil::EventOptions::ManualReset};
    std::atomic_int creates{0}, closes{0};
    std::atomic<DWORD> closeThread{0};
    HRESULT error=S_OK;
};
struct TestConnection {
    std::shared_ptr<Scenario> scenario;
    TestConnection(std::nullptr_t=nullptr){}
    explicit TestConnection(std::shared_ptr<Scenario> s):scenario(std::move(s)){}
    explicit operator bool()const{return !!scenario;}
    void Close(){++scenario->closes;scenario->closeThread=GetCurrentThreadId();scenario->closeEntered.SetEvent();scenario->closeAllowed.wait();}
};
std::mutex scenariosMutex;
std::unordered_map<std::wstring,std::shared_ptr<Scenario>> scenarios;
TestConnection TestCreate(std::wstring const& id){
    std::shared_ptr<Scenario> s;{std::lock_guard<std::mutex> lock(scenariosMutex);s=scenarios.at(id);}
    ++s->creates;s->entered.SetEvent();s->returnAllowed.wait();winrt::check_hresult(s->error);return TestConnection(s);
}
'''
code += helper
code += r'''
int checks=0,failures=0;
void Check(bool ok,char const* name){++checks;if(!ok){++failures;std::cout<<"FAIL "<<name<<'\n';}}
auto ScenarioFor(std::wstring id,bool ready=true,bool close=true){
    auto s=std::make_shared<Scenario>();if(ready)s->returnAllowed.SetEvent();if(close)s->closeAllowed.SetEvent();
    std::lock_guard<std::mutex> lock(scenariosMutex);scenarios[id]=s;return s;
}
bool Finished(std::shared_ptr<ConnectionCreation::State> s){
    auto until=steady_clock::now()+seconds(2);
    while(!s->finished&&steady_clock::now()<until)std::this_thread::sleep_for(milliseconds(1));return s->finished;
}
HRESULT Wait(std::shared_ptr<ConnectionCreation::State> s,milliseconds budget,std::shared_ptr<AsyncCancellation> cancel={}){
    try{AwaitBounded(ConnectionCreation::WaitReady(s),steady_clock::now()+budget,cancel).get();return S_OK;}
    catch(winrt::hresult_error const&e){return e.code();}
}
int main(){
    std::cout<<std::unitbuf;winrt::init_apartment(winrt::apartment_type::multi_threaded);
    {
        auto scenario=ScenarioFor(L"normal");auto start=steady_clock::now();auto s=ConnectionCreation::Start(L"normal");
        Check(Wait(s,seconds(2))==S_OK,"normal creation completes");auto connection=ConnectionCreation::Take(*s);
        Check(bool(connection)&&connection.scenario==scenario,"result ownership transferred");
        Check(Finished(s)&&scenario->closes==0,"claimed connection is not closed by worker");
        Check(steady_clock::now()-start<seconds(1),"normal result does not wait for deadline");connection.Close();
    }
    {
        auto scenario=ScenarioFor(L"failure");scenario->error=E_ACCESSDENIED;auto s=ConnectionCreation::Start(L"failure");
        Check(Wait(s,seconds(1))==S_OK,"exception wakes waiter");bool denied=false;
        try{ConnectionCreation::Take(*s);}catch(winrt::hresult_error const&e){denied=e.code()==E_ACCESSDENIED;}
        ConnectionCreation::Abandon(*s);Check(denied&&Finished(s),"error preserves HRESULT and releases slot");
    }
    {
        auto scenario=ScenarioFor(L"timeout",false,false);auto s=ConnectionCreation::Start(L"timeout");
        Check(scenario->entered.wait(1000),"synchronous create entered");auto start=steady_clock::now();
        Check(Wait(s,milliseconds(30))==HRESULT_FROM_WIN32(ERROR_TIMEOUT),"hung create has bounded caller wait");
        ConnectionCreation::Abandon(*s);
        Check(steady_clock::now()-start<seconds(1)&&!s->finished,"timeout returns promptly and retains occupied slot");
        auto otherScenario=ScenarioFor(L"other");auto other=ConnectionCreation::Start(L"other");
        Check(Wait(other,seconds(1))==S_OK,"another device is not blocked");auto connection=ConnectionCreation::Take(*other);
        Check(Finished(other)&&otherScenario->creates==1,"other device completes independently");connection.Close();
        scenario->returnAllowed.SetEvent();Check(scenario->closeEntered.wait(1000),"late result is closed in worker");
        Check(scenario->closeThread!=GetCurrentThreadId(),"late close never runs on caller thread");
        Check(!s->finished,"slot remains occupied during late close");bool cancelled=false;
        try{ConnectionCreation::Take(*s);}catch(winrt::hresult_error const&e){cancelled=e.code()==HRESULT_FROM_WIN32(ERROR_CANCELLED);}
        Check(cancelled,"late result cannot be adopted after cancellation");scenario->closeAllowed.SetEvent();
        Check(Finished(s)&&scenario->creates==1&&scenario->closes==1,"late result closed once and slot released");
    }
    {
        auto scenario=ScenarioFor(L"cancel",false);auto s=ConnectionCreation::Start(L"cancel");
        Check(scenario->entered.wait(1000),"cancellation create entered");auto cancel=std::make_shared<AsyncCancellation>();
        std::thread trigger([cancel]{std::this_thread::sleep_for(milliseconds(15));cancel->Request();});
        auto start=steady_clock::now();auto result=Wait(s,seconds(45),cancel);trigger.join();ConnectionCreation::Abandon(*s);
        Check(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)&&steady_clock::now()-start<seconds(1),"cancel does not wait for 45 second deadline");
        Check(!s->finished,"cancel does not claim to interrupt driver call");scenario->returnAllowed.SetEvent();
        Check(Finished(s)&&scenario->closes==1,"cancelled late result closes without UI");
    }
    {
        auto scenario=ScenarioFor(L"published");auto s=ConnectionCreation::Start(L"published");
        Check(Wait(s,seconds(1))==S_OK,"result published before cancel");ConnectionCreation::Abandon(*s);
        Check(Finished(s)&&scenario->closes==1,"cancel after publication does not leak connection");
    }
    {
        auto start=steady_clock::now();
        std::vector<std::shared_ptr<ConnectionCreation::State>> tasks;
        for(int i=0;i<100;++i){auto id=L"fast"+std::to_wstring(i);ScenarioFor(id);auto s=ConnectionCreation::Start(id);
            Wait(s,seconds(45));auto connection=ConnectionCreation::Take(*s);tasks.push_back(s);connection.Close();}
        auto elapsed=duration_cast<milliseconds>(steady_clock::now()-start).count();
        std::cout<<"FAST_CREATIONS count=100 elapsed-ms="<<elapsed<<'\n';Check(elapsed<2000,"fast results have no fixed delay");
        for(auto const& task:tasks)Finished(task);
    }
    std::cout<<"CONNECTION_CREATION checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
'''
Path(sys.argv[1]).write_text(code, encoding='utf-8')
