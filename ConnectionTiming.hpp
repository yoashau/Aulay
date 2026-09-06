#pragma once
#include <cstdint>
namespace aulay::timing {
constexpr std::uint32_t retryBaseMs = 200;
constexpr std::uint32_t retryCapMs = 300000;
constexpr std::uint32_t RetryDelayMs(std::uint32_t retry) noexcept
{
	return retry >= 11 ? retryCapMs : retryBaseMs << retry;
}
}
