#include "BackgroundLog.hpp"
#include "DiagnosticSampling.hpp"
#include <cassert>
#include <future>
#include <iostream>
using namespace std::chrono_literals;
int main() {
    aulay::diagnostics::Sampling p;
    assert(p.SampleMs(100)==500 && p.ReportMs(100)==5000 && p.InventoryMs(100)==30000);
    p.Boost(100);assert(p.Detailed(100) && p.SampleMs(30099)==50);
    assert(!p.Detailed(30100) && p.SampleMs(30100)==500);
    p.Boost(200);p.Boost(150);assert(p.detailedUntil==30200);
    assert(!p.Detailed(40000)); // stale queued events cannot resurrect detail
    p.Boost(40000);assert(p.Detailed(69999) && !p.Detailed(70000));
    auto q=std::make_shared<aulay::logging::Queue>();
    std::promise<void> entered, release;
    auto gate=release.get_future().share();
    std::vector<std::wstring> lines;
    bool closed=false;auto caller=std::this_thread::get_id();
    q->Start([&](auto const& batch) {
        assert(std::this_thread::get_id()!=caller);
        if(lines.empty()){entered.set_value();gate.wait();}
        for(auto const& s:batch)lines.push_back(s);
    },[&]{closed=true;});
    assert(q->Push(L"first"));assert(entered.get_future().wait_for(2s)==std::future_status::ready);
    // Writer is deliberately blocked; enqueue must still complete before releasing it.
    auto producer=std::async(std::launch::async,[&]{return q->Push(L"second");});
    assert(producer.wait_for(1s)==std::future_status::ready && producer.get());
    for(size_t i=1;i<q->maxEntries;++i)assert(q->Push(L"queued"));
    assert(!q->Push(L"overflow"));
    q->Stop();assert(!q->Push(L"after-stop"));
    release.set_value();assert(q->WaitFor(2s));assert(closed && lines.front()==L"first" && lines[1]==L"second");
    assert(lines.back().find(L"app-log-queue-dropped=1")!=std::wstring::npos);
    auto failed=std::make_shared<aulay::logging::Queue>();bool finally=false;
    failed->Start([](auto const&){throw 1;},[&]{finally=true;});failed->Push(L"failure");
    assert(failed->WaitFor(2s));assert(finally && failed->errors==1);
    std::cout << "DIAGNOSTIC_OVERHEAD=PASS sampling-expiry=30s producer-independent-of-sink=1 drain=1 bounded-queue=1 fault-completion=1\n";
}
