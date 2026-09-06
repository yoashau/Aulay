#pragma once
#include "AudioFlowPolicy.hpp"
#include "ConnectionTiming.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include "BluetoothRouting.hpp"
#include <wrl/client.h>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace AudioFlow {
using Microsoft::WRL::ComPtr;
using Log = std::function<void(std::wstring const&)>;
// Private Windows policy interface; fail locally if this version does not support it.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
 virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR,WAVEFORMATEX**)=0;
 virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR,bool,WAVEFORMATEX**)=0;
 virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR)=0;
 virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR,WAVEFORMATEX*,WAVEFORMATEX*)=0;
 virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR,bool,INT64*,INT64*)=0;
 virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR,INT64*)=0;
 virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR,void*)=0;
 virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR,void*)=0;
 virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR,BOOL,const PROPERTYKEY&,PROPVARIANT*)=0;
 virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR,BOOL,const PROPERTYKEY&,PROPVARIANT*)=0;
 virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR,ERole)=0;
 virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR,bool)=0;
};
static constexpr GUID clsid={0x870af99c,0x171d,0x4f9e,{0xaf,0x0d,0xe6,0x3d,0xf4,0x0c,0x2b,0xc9}};
struct State {
    std::atomic_bool stop{false}, closed{false}, cleanup{false}, done{false};
    std::atomic_bool cycling{false};
    wil::unique_event wake{wil::EventOptions::None};
    wil::unique_event closedSignal{wil::EventOptions::ManualReset};
    wil::unique_event finishedSignal{wil::EventOptions::ManualReset};
    std::atomic<HRESULT> cleanupResult{S_OK};
    std::wstring interfaceId;
    Log log;
};
inline void Write(State& s, std::wstring const& text) noexcept {
    try { if (s.log) s.log(text); } catch (...) {}
}
inline std::wstring Hr(HRESULT hr) {
    wchar_t b[16]; swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr)); return b;
}
inline bool Starts(std::wstring const& s, wchar_t const* prefix) {
    auto n=wcslen(prefix); return s.size()>=n && _wcsnicmp(s.c_str(),prefix,n)==0;
}
// The playback interface belongs to the remote 110A source profile itself.
// Select only its exact SWD capture child, including a temporarily non-present child.
// DEVPKEY_Device_Parent retains that relationship after disconnect (Windows 8+).
inline std::wstring ResolveEndpoint(std::wstring const& interfaceId) {
    wchar_t parentId[MAX_DEVICE_ID_LEN]{}; DEVPROPTYPE type=0; ULONG cb=sizeof(parentId);
    if (CM_Get_Device_Interface_PropertyW(interfaceId.c_str(), &DEVPKEY_Device_InstanceId,
        &type, reinterpret_cast<BYTE*>(parentId), &cb, 0)!=CR_SUCCESS || type!=DEVPROP_TYPE_STRING)
        return {};
    parentId[MAX_DEVICE_ID_LEN-1]=0;
    if (!Starts(parentId,L"BTHENUM\\{0000110A-0000-1000-8000-00805F9B34FB}")) return {};
    DEVINST parent=0;
    if (CM_Locate_DevNodeW(&parent,parentId,CM_LOCATE_DEVNODE_PHANTOM)!=CR_SUCCESS) return {};
    wchar_t service[64]{}; cb=sizeof(service);
    if (CM_Get_DevNode_PropertyW(parent,&DEVPKEY_Device_Service,&type,reinterpret_cast<BYTE*>(service),&cb,0)!=CR_SUCCESS ||
        type!=DEVPROP_TYPE_STRING || _wcsicmp(service,L"BthA2dp")!=0) return {};
    constexpr auto flags=CM_GETIDLIST_FILTER_ENUMERATOR;
    ULONG chars=0;
    if (CM_Get_Device_ID_List_SizeW(&chars,L"SWD\\MMDEVAPI",flags)!=CR_SUCCESS || chars<2 || chars>1024*1024) return {};
    std::vector<wchar_t> ids(chars+1,0);
    if (CM_Get_Device_ID_ListW(L"SWD\\MMDEVAPI",ids.data(),chars,flags)!=CR_SUCCESS) return {};
    std::wstring selected;
    for (auto id=ids.data();*id;id+=wcslen(id)+1) {
        if (!Starts(id,L"SWD\\MMDEVAPI\\{0.0.1.00000000}.")) continue;
        DEVINST child=0;
        if (CM_Locate_DevNodeW(&child,id,CM_LOCATE_DEVNODE_PHANTOM)!=CR_SUCCESS) continue;
        wchar_t actualParent[MAX_DEVICE_ID_LEN]{};cb=sizeof(actualParent);
        if (CM_Get_DevNode_PropertyW(child,&DEVPKEY_Device_Parent,&type,
            reinterpret_cast<BYTE*>(actualParent),&cb,0)!=CR_SUCCESS || type!=DEVPROP_TYPE_STRING) continue;
        actualParent[MAX_DEVICE_ID_LEN-1]=0;
        if (_wcsicmp(actualParent,parentId)!=0) continue;
        if (!selected.empty()) return {}; // ambiguous mapping is not permission to guess
        selected=std::wstring(id).substr(wcslen(L"SWD\\MMDEVAPI\\"));
    }
    return selected;
}
inline HRESULT EndpointState(IMMDeviceEnumerator* en, std::wstring const& id, DWORD& value) {
    ComPtr<IMMDevice> d; auto hr=en->GetDevice(id.c_str(),&d);
    return SUCCEEDED(hr) ? d->GetState(&value) : hr;
}
inline HRESULT Jack(IMMDevice* d, bool& connected) {
    ComPtr<IDeviceTopology> topology;
    auto hr=d->Activate(__uuidof(IDeviceTopology),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(topology.GetAddressOf()));
    if (FAILED(hr)) return hr;
    ComPtr<IConnector> from,to; ComPtr<IPart> part; ComPtr<IKsJackDescription> jack;
    if (FAILED(hr=topology->GetConnector(0,&from)) || FAILED(hr=from->GetConnectedTo(&to)) ||
        FAILED(hr=to.As(&part)) || FAILED(hr=part->Activate(CLSCTX_INPROC_SERVER,__uuidof(IKsJackDescription),reinterpret_cast<void**>(jack.GetAddressOf())))) return hr;
    UINT count=0; if (FAILED(hr=jack->GetJackCount(&count))) return hr;
    if (count!=1) return E_UNEXPECTED;
    KSJACK_DESCRIPTION desc{}; hr=jack->GetJackDescription(0,&desc);
    if (SUCCEEDED(hr)) connected=desc.IsConnected!=FALSE;
    return hr;
}
// Endpoint notifications acknowledge disable/enable; a timeout is only a bound.
struct EndpointEvents : winrt::implements<EndpointEvents, IMMNotificationClient> {
    std::wstring endpoint;
    wil::unique_event changed{wil::EventOptions::None};
    explicit EndpointEvents(std::wstring id):endpoint(std::move(id)){}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id,DWORD) noexcept override {Notify(id);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) noexcept override {Notify(id);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) noexcept override {Notify(id);return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) noexcept override {return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id,const PROPERTYKEY) noexcept override {Notify(id);return S_OK;}
    void Notify(LPCWSTR id) noexcept {if(id && _wcsicmp(id,endpoint.c_str())==0)SetEvent(changed.get());}
};
inline bool WaitEndpointState(IMMDeviceEnumerator* en,std::wstring const& id,HANDLE changed,DWORD wanted,DWORD timeoutMs=2000) {
    auto deadline=GetTickCount64()+timeoutMs;
    for (;;) {
        DWORD value=0;
        if(SUCCEEDED(EndpointState(en,id,value)) && (value & wanted)!=0)return true;
        auto now=GetTickCount64();if(now>=deadline)return false;
        // Registered before the mutation; auto-reset event retains an early notification.
        WaitForSingleObject(changed,static_cast<DWORD>(deadline-now));
    }
}
inline void RequestStop(State& s) noexcept {s.stop=true;SetEvent(s.wake.get());}
inline void SignalClosed(State& s) noexcept {s.closed=true;SetEvent(s.closedSignal.get());}
inline winrt::Windows::Foundation::IAsyncAction WaitFinished(std::shared_ptr<State> s) {
    co_await winrt::resume_on_signal(s->finishedSignal.get());
    if(s->stop && !s->closed)co_await winrt::resume_on_signal(s->closedSignal.get());
}
inline HRESULT Cycle(State& s, IMMDeviceEnumerator* en, std::wstring const& id, bool closing) {
    // A close can cancel monitoring, but cannot cancel the restore half of an active cycle.
    if ((!closing && s.stop) || ResolveEndpoint(s.interfaceId)!=id) return E_ABORT;
    DWORD original=0; auto hr=EndpointState(en,id,original);
    if (FAILED(hr)) return hr;
    if (original!=DEVICE_STATE_ACTIVE && original!=DEVICE_STATE_UNPLUGGED) return S_FALSE;
    ComPtr<IPolicyConfig> pc;
    hr=CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&pc));
    if (FAILED(hr)) return hr;
    if (!closing && s.stop) return E_ABORT;
    auto events=winrt::make_self<EndpointEvents>(id);
    hr=en->RegisterEndpointNotificationCallback(events.get());
    if(FAILED(hr))return hr;
    auto unregister=wil::scope_exit([&]{en->UnregisterEndpointNotificationCallback(events.get());});
    s.cycling=true;
    struct Restore {
        IPolicyConfig* pc; const std::wstring& id; State& state; bool needed=true;
        ~Restore() { if (needed) pc->SetEndpointVisibility(id.c_str(),true); state.cycling=false; }
    } restore{pc.Get(),id,s};
    Write(s,closing?L"audio-flow cleanup begin":L"audio-flow silent-recovery begin");
    auto disabled=pc->SetEndpointVisibility(id.c_str(),false);
    bool observed=false;
    if (SUCCEEDED(disabled)) {
        observed=WaitEndpointState(en,id,events->changed.get(),DEVICE_STATE_DISABLED);
    }
    HRESULT enabled=E_FAIL; bool restored=false;
    auto restoreDeadline=GetTickCount64()+6000;
    unsigned retry=0;
    do {
        enabled=pc->SetEndpointVisibility(id.c_str(),true);
        auto remaining=restoreDeadline>GetTickCount64()?restoreDeadline-GetTickCount64():0;
        restored=SUCCEEDED(enabled) && WaitEndpointState(en,id,events->changed.get(),DEVICE_STATE_ACTIVE|DEVICE_STATE_UNPLUGGED,
            static_cast<DWORD>((std::min)(remaining,ULONGLONG(2000))));
        if(restored)break;
        auto now=GetTickCount64();if(now>=restoreDeadline)break;
        // A rejected call also backs off instead of busy-spinning; notifications wake early.
        auto delay=(std::min)(static_cast<ULONGLONG>(aulay::timing::RetryDelayMs(retry++)),restoreDeadline-now);
        WaitForSingleObject(events->changed.get(),static_cast<DWORD>(delay));
    } while(GetTickCount64()<restoreDeadline);
    restore.needed=!restored;
    Write(s,L"audio-flow cycle disable="+Hr(disabled)+L" enable="+Hr(enabled)+
        L" disabled-observed="+std::to_wstring(observed)+L" restored="+std::to_wstring(restored));
    if (!restored) return FAILED(enabled)?enabled:E_FAIL;
    return FAILED(disabled)?disabled:(observed?S_OK:E_FAIL);
}
struct Capture {
    ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> capture;
    ~Capture(){Close();}
    void Close() noexcept {if(client){client->Stop();client->Reset();}capture.Reset();client.Reset();}
    HRESULT Open(IMMDevice* d) {
        Close(); auto hr=d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf()));
        if(FAILED(hr))return hr;
        WAVEFORMATEX* format=nullptr;hr=client->GetMixFormat(&format);
        if(SUCCEEDED(hr)){hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,10000000,0,format,nullptr);}
        CoTaskMemFree(format);
        if(SUCCEEDED(hr))hr=client->GetService(IID_PPV_ARGS(&capture));
        if(SUCCEEDED(hr))hr=client->Start();
        if(FAILED(hr))Close();return hr;
    }
    HRESULT Drain(std::uint64_t& frames) {
        if(!capture)return E_UNEXPECTED;
        // Bounded drain prevents cancellation starvation. No audio samples are read or saved.
        for(unsigned i=0;i<128;++i) {
            UINT32 packet=0;auto hr=capture->GetNextPacketSize(&packet);if(FAILED(hr)||!packet)return hr;
            BYTE* data=nullptr;UINT32 n=0;DWORD flags=0;
            hr=capture->GetBuffer(&data,&n,&flags,nullptr,nullptr);if(FAILED(hr))return hr;
            frames+=n;hr=capture->ReleaseBuffer(n);if(FAILED(hr))return hr;
        }
        return S_OK;
    }
};
inline void Run(std::shared_ptr<State> s) noexcept {
    auto finish=wil::scope_exit([&]{s->done=true;SetEvent(s->finishedSignal.get());});
    auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(initialized)){s->cleanupResult=initialized;Write(*s,L"audio-flow COM failed "+Hr(initialized));s->done=true;return;}
    try {
        ComPtr<IMMDeviceEnumerator> en;
        auto hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en));
        if(FAILED(hr))winrt::throw_hresult(hr);
        Capture capture; aulay::flow::RecoveryPolicy policy; std::wstring endpoint;
        ULONGLONG nextResolve=0,nextCapture=0,verifyUntil=0;bool mappedLogged=false,errorLogged=false;
        while(!s->stop) {
            auto tick=GetTickCount64();
            if(endpoint.empty() && tick>=nextResolve){endpoint=ResolveEndpoint(s->interfaceId);nextResolve=tick+1000;}
            if(!endpoint.empty()&&!mappedLogged){Write(*s,L"audio-flow capture endpoint mapped from exact 110A child");mappedLogged=true;}
            ComPtr<IMMDevice> d;DWORD deviceState=0;bool jack=false,known=false;std::uint64_t frames=0;
            bool active=!endpoint.empty() && SUCCEEDED(en->GetDevice(endpoint.c_str(),&d)) &&
                SUCCEEDED(d->GetState(&deviceState)) && deviceState==DEVICE_STATE_ACTIVE;
            if(active)known=SUCCEEDED(Jack(d.Get(),jack));
            bool started=false,readOk=false;
            if(active && known && jack) {
                if(!capture.client && tick>=nextCapture){hr=capture.Open(d.Get());nextCapture=tick+1000;
                    if(FAILED(hr)&&!errorLogged){Write(*s,L"audio-flow capture unavailable "+Hr(hr));errorLogged=true;}}
                started=bool(capture.client);if(started){hr=capture.Drain(frames);readOk=SUCCEEDED(hr);if(!readOk)capture.Close();}
            } else capture.Close();
            if(!active && tick>=nextResolve){endpoint.clear();nextResolve=tick+1000;}
            auto decision=policy.Observe({tick,true,true,s->stop.load(),active,known,jack,started,readOk,frames});
            if(decision==aulay::flow::Decision::Recover) {
                capture.Close(); // own client must not keep the old audio stream alive
                auto result=Cycle(*s,en.Get(),endpoint,false);
                Write(*s,L"audio-flow silent-recovery result="+Hr(result));
                if(result==S_OK)verifyUntil=GetTickCount64()+10000;
            }
            if(verifyUntil && frames){Write(*s,L"audio-flow recovery verified frames="+std::to_wstring(frames));verifyUntil=0;}
            if(verifyUntil && tick>=verifyUntil){Write(*s,L"audio-flow recovery not verified; no radio escalation");verifyUntil=0;}
            WaitForSingleObject(s->wake.get(),100); // sampling cadence, stop wakes immediately
        }
        capture.Close();
        WaitForSingleObject(s->closedSignal.get(),INFINITE); // Close completion, not a guessed delay
        if(s->cleanup) {
            endpoint=ResolveEndpoint(s->interfaceId);
            auto result=endpoint.empty()?HRESULT_FROM_WIN32(ERROR_NOT_FOUND):Cycle(*s,en.Get(),endpoint,true);
            s->cleanupResult=result;
            Write(*s,L"audio-flow disconnect-cleanup result="+Hr(result));
        }
    } catch(...) {auto error=static_cast<HRESULT>(winrt::to_hresult());s->cleanupResult=error;Write(*s,L"audio-flow worker failed "+Hr(error));}
    CoUninitialize();s->done=true;
}
inline std::shared_ptr<State> Start(std::wstring id, Log log, bool cleanupOnly=false, bool closedAlready=true) {
    auto s=std::make_shared<State>();s->interfaceId=std::move(id);s->log=std::move(log);
    if(cleanupOnly){s->cleanup=true;s->stop=true;if(closedAlready)SignalClosed(*s);}
    std::thread([s]{Run(s);}).detach();return s;
}
} // namespace AudioFlow
