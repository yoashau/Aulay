#pragma once
#include <cstdint>
namespace aulay::diagnostics {
struct Sampling {
    static constexpr std::uint64_t burstMs=30000;
    std::uint64_t detailedUntil=0;
    void Boost(std::uint64_t now) noexcept {if(now+burstMs>detailedUntil)detailedUntil=now+burstMs;}
    bool Detailed(std::uint64_t now) const noexcept {return now<detailedUntil;}
    std::uint64_t SampleMs(std::uint64_t now) const noexcept {return Detailed(now)?50:500;}
    std::uint64_t ReportMs(std::uint64_t now) const noexcept {return Detailed(now)?1000:5000;}
    std::uint64_t InventoryMs(std::uint64_t now) const noexcept {return Detailed(now)?2000:30000;}
};
}
