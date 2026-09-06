#!/usr/bin/env python3
"""Extract actual recovery transaction/waiter; replace only Radio with a test double."""
from pathlib import Path
import runpy
import sys
root=Path(__file__).resolve().parents[1]
output=Path(sys.argv[1])
sys.argv=[__file__, str(root)]
ns=runpy.run_path(str(root/'tests/test_regressions.py'))
header=(root/'Aulay.h').read_text(encoding='utf-8-sig')
code=r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Radios.h>
#include "AsyncUtil.hpp"
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Devices::Radios::RadioState;
using winrt::Windows::Devices::Radios::RadioAccessStatus;
using namespace std::chrono;
constexpr uint32_t BLUETOOTH_OFF_TIMEOUT_MS=40;
constexpr uint32_t BLUETOOTH_ON_TIMEOUT_MS=60;
constexpr uint32_t BLUETOOTH_RESTORE_TIMEOUT_MS=60;
'''
for enum in ('BluetoothRecoveryStage','BluetoothRecoveryResult'):
    code+=ns['block'](header,'^enum class '+enum+r'\b')+';\n'
code+=r'''
struct { std::optional<RadioAccessStatus> radioAccess; } g_app;
std::vector<std::wstring> logEntries;
void RecordDiagnostic(std::wstring_view, std::wstring const& message) { logEntries.push_back(message); }
std::wstring FormatDiagnosticHresult(HRESULT error) { return std::to_wstring(error); }
uint64_t ElapsedMilliseconds(steady_clock::time_point start) { return duration_cast<milliseconds>(steady_clock::now()-start).count(); }
IAsyncAction SetBluetoothRecoveryStage(BluetoothRecoveryStage) { co_return; }
'''
code+=ns['function']('BluetoothRecoveryResultName')+'\n'+ns['function']('RadioAccessStatusName')+'\n'
code+=r'''
struct Scenario {
 std::atomic<RadioState> state{RadioState::On};
 bool denyOff=false, denyOn=false;
 int offDelay=0, onDelay=0;
 std::atomic_int offCalls{0}, onCalls{0};
 std::shared_ptr<AsyncCancellation> cancelAfterOff;
 std::mutex mutex;
 std::function<void()> changed;
};
struct MockRadio {
 std::shared_ptr<Scenario> scenario;
 MockRadio(std::nullptr_t=nullptr) {}
 MockRadio(std::shared_ptr<Scenario> s):scenario(std::move(s)) {}
 explicit operator bool() const { return !!scenario; }
 RadioState State() const { return scenario->state; }
 static IAsyncOperation<RadioAccessStatus> RequestAccessAsync() { co_return RadioAccessStatus::Allowed; }
 struct Revoker {
  std::shared_ptr<Scenario> scenario;
  ~Revoker(){ std::lock_guard<std::mutex> lock(scenario->mutex); scenario->changed={}; }
 };
 template<class Callback> Revoker StateChanged(winrt::auto_revoke_t, Callback callback) const {
  auto s=scenario;
  std::lock_guard<std::mutex> lock(s->mutex);
  s->changed=[weak=std::weak_ptr<Scenario>(s),callback]{
   if(auto state=weak.lock())callback(MockRadio(state),nullptr);
  };
  return {s};
 }
 IAsyncOperation<RadioAccessStatus> SetStateAsync(RadioState target) const {
  auto s=scenario; // own state before yielding, no dangling member-coroutine this
  if(target==RadioState::Off)++s->offCalls; else ++s->onCalls;
  auto delay=target==RadioState::Off?s->offDelay:s->onDelay;
  if(delay)co_await winrt::resume_after(milliseconds(delay));
  if((target==RadioState::Off&&s->denyOff)||(target==RadioState::On&&s->denyOn))
   co_return RadioAccessStatus::DeniedBySystem;
  s->state=target;
  std::function<void()> changed;
  { std::lock_guard<std::mutex> lock(s->mutex);changed=s->changed; }
  if(changed)changed();
  if(target==RadioState::Off&&s->cancelAfterOff)s->cancelAfterOff->Request();
  co_return RadioAccessStatus::Allowed;
 }
};
'''
code+=ns['function']('SetRadioStateInBackground').replace('winrt::Windows::Devices::Radios::Radio radio','MockRadio radio')+'\n'
code+=ns['function']('WaitForRadioState').replace('winrt::Windows::Devices::Radios::Radio radio','MockRadio radio').replace('Radio const& sender','MockRadio const& sender')+'\n'
code+=ns['function']('RunBluetoothRecoveryTransaction').replace('winrt::Windows::Devices::Radios::Radio radio','MockRadio radio').replace('Radio::RequestAccessAsync()', 'MockRadio::RequestAccessAsync()')+'\n'
code+=r'''
int main(){
 std::cout<<std::unitbuf;winrt::init_apartment(winrt::apartment_type::multi_threaded);
 int checked=0,failed=0;
 auto check=[&](bool result,const char* name){++checked;if(!result){++failed;std::cout<<"FAIL "<<name<<'\n';}};
 auto run=[&](std::shared_ptr<Scenario> scenario,std::shared_ptr<AsyncCancellation> cancel={}){
  logEntries.clear();g_app.radioAccess.reset();
  return static_cast<BluetoothRecoveryResult>(RunBluetoothRecoveryTransaction(MockRadio(scenario),steady_clock::now()+milliseconds(200),cancel).get());
 };
 { auto s=std::make_shared<Scenario>();auto start=steady_clock::now();
  check(run(s)==BluetoothRecoveryResult::Success,"normal transaction succeeds");
  check(s->offCalls==1&&s->onCalls==1&&s->state==RadioState::On,"normal Off then On");
  check(steady_clock::now()-start<milliseconds(200),"normal transaction does not consume fault deadlines");
 }
 { auto s=std::make_shared<Scenario>();auto cancel=std::make_shared<AsyncCancellation>();cancel->Request();
  check(run(s,cancel)==BluetoothRecoveryResult::Cancelled,"cancelled before Off");
  check(s->offCalls==0&&s->onCalls==0,"pre-cancel touches no power state");
 }
 { auto s=std::make_shared<Scenario>();s->cancelAfterOff=std::make_shared<AsyncCancellation>();
  check(run(s,s->cancelAfterOff)==BluetoothRecoveryResult::Cancelled,"shutdown after Off preserves cancellation result");
  check(s->offCalls==1&&s->onCalls==1&&s->state==RadioState::On,"shutdown cancellation does not cancel On cleanup");
 }
 { auto s=std::make_shared<Scenario>();s->offDelay=150;
  check(run(s)==BluetoothRecoveryResult::OperationTimedOut,"Off timeout reported");
  check(s->onCalls==1&&s->state==RadioState::On,"Off timeout still requests On");
 }
 { auto s=std::make_shared<Scenario>();s->denyOff=true;
  check(run(s)==BluetoothRecoveryResult::TurnOffDenied,"Off denied is not success");
  check(s->onCalls==1,"Off denial still compensates request");
 }
 { auto s=std::make_shared<Scenario>();s->onDelay=180;auto start=steady_clock::now();
  check(run(s)==BluetoothRecoveryResult::TurnOnTimedOut,"On timeout is not false success");
  check(steady_clock::now()-start<milliseconds(500),"hung On cleanup has bounded wait");
 }
 { auto s=std::make_shared<Scenario>();s->state=RadioState::Off;
  check(run(s)==BluetoothRecoveryResult::Success&&s->offCalls==0&&s->onCalls==1,"initially Off goes directly to On");
 }
 { auto s=std::make_shared<Scenario>();s->denyOn=true;
  check(run(s)==BluetoothRecoveryResult::TurnOnDenied,"On denied reported");
 }
 // Give cancelled synthetic delay coroutines time to release their private state.
 std::this_thread::sleep_for(milliseconds(200));
 std::cout<<"RECOVERY_FAULTS checks="<<checked<<" failures="<<failed<<'\n';
 return failed?1:0;
}
'''
output.write_text(code,encoding='utf-8')
