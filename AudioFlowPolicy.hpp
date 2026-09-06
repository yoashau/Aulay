#pragma once
#include <cstdint>
namespace aulay::flow {
struct Observation {
	std::uint64_t tickMs;
	bool currentSession;
	bool opened;
	bool cancelled;
	bool endpointActive;
	bool jackKnown;
	bool jackConnected;
	bool captureStarted;
	bool captureReadOk;
	std::uint64_t frames;
};
enum class Decision { Wait,
	Healthy,
	Paused,
	Unknown,
	Cancelled,
	Recover,
	AlreadyAttempted };
// Per-session, single-worker policy. Zero amplitude is deliberately not an input:
// packets containing digital silence still prove that the pipeline supplies frames.
class RecoveryPolicy {
public:
	static constexpr std::uint64_t noFramesMs = 4000;
	static constexpr std::uint64_t maxObservationGapMs = 1000;
	Decision Observe(const Observation& o) noexcept
	{
		if (o.cancelled || !o.currentSession || !o.opened) {
			pending_ = false;
			return Decision::Cancelled;
		}
		if (!o.endpointActive || !o.jackKnown) {
			pending_ = false;
			return Decision::Unknown;
		}
		if (!o.jackConnected) {
			pending_ = false;
			return Decision::Paused;
		}
		if (!o.captureStarted || !o.captureReadOk) {
			pending_ = false;
			return Decision::Unknown;
		}
		if (o.frames != 0) {
			pending_ = false;
			return Decision::Healthy;
		}
		if (attempted_)
			return Decision::AlreadyAttempted;
		if (!pending_ || o.tickMs < last_ || o.tickMs - last_ > maxObservationGapMs) {
			pending_ = true;
			began_ = o.tickMs;
		}
		last_ = o.tickMs;
		if (o.tickMs - began_ < noFramesMs)
			return Decision::Wait;
		pending_ = false;
		attempted_ = true; // Consume the attempt before side effects, even on failure.
		return Decision::Recover;
	}

private:
	bool pending_ = false;
	bool attempted_ = false;
	std::uint64_t began_ = 0, last_ = 0;
};
}
