#include "AudioFlowPolicy.hpp"
#include "ConnectionTiming.hpp"
#include <cassert>
#include <iostream>
using namespace aulay::flow;
Observation sample(std::uint64_t t)
{
	return { t, true, true, false, true, true, true, true, true, 0 };
}
int main()
{
	constexpr auto threshold = RecoveryPolicy::noFramesMs;
	assert(threshold == 500);
	const unsigned expected[] = { 200, 400, 800, 1600, 3200, 6400, 12800, 25600, 51200, 102400, 204800, 300000, 300000 };
	for (unsigned i = 0; i < 13; ++i)
		assert(aulay::timing::RetryDelayMs(i) == expected[i]);
	assert(aulay::timing::RetryDelayMs(UINT32_MAX) == 300000);
	std::cout << "BACKOFF PASS checks=14 cap-ms=300000 count-limit=none total-deadline=none\n";

	{
		RecoveryPolicy p(true);
		for (unsigned t = 0; t <= 10000; t += 250) {
			auto o = sample(t);
			o.jackConnected = false;
			assert(p.Observe(o) == Decision::Paused);
		}
	}
	{
		RecoveryPolicy p(true);
		for (unsigned t = 0; t <= 10000; t += 250) {
			auto o = sample(t);
			o.frames = 441;
			assert(p.Observe(o) == Decision::Healthy);
		}
	}
	{
		RecoveryPolicy p(true);
		for (unsigned t = 0; t < threshold; t += 250)
			assert(p.Observe(sample(t)) == Decision::Wait);
		assert(p.Observe(sample(threshold)) == Decision::Recover);
		assert(p.Observe(sample(8000)) == Decision::AlreadyAttempted);
	}
	{
		RecoveryPolicy p(true);
		p.Observe(sample(0));
		assert(p.Observe(sample(10000)) == Decision::Wait);
	} // no recovery from an observation gap
	{
		RecoveryPolicy p(true);
		p.Observe(sample(5000));
		assert(p.Observe(sample(1)) == Decision::Wait);
	} // monotonic-clock reset
	for (unsigned reason = 0; reason < 7; reason++) {
		RecoveryPolicy p(true);
		for (unsigned t = 0; t < threshold; t += 250)
			p.Observe(sample(t));
		auto o = sample(threshold);
		switch (reason) {
		case 0:
			o.currentSession = false;
			break;
		case 1:
			o.opened = false;
			break;
		case 2:
			o.cancelled = true;
			break;
		case 3:
			o.endpointActive = false;
			break;
		case 4:
			o.jackKnown = false;
			break;
		case 5:
			o.captureStarted = false;
			break;
		case 6:
			o.captureReadOk = false;
			break;
		}
		auto result = p.Observe(o);
		assert(result == Decision::Cancelled || result == Decision::Unknown);
		assert(p.Observe(sample(threshold + 250)) == (reason < 3 ? Decision::Deferred : Decision::Wait));
	}
	{
		RecoveryPolicy p(true);
		for (unsigned t = 0; t < threshold; t += 250)
			p.Observe(sample(t));
		auto o = sample(threshold);
		o.frames = 1;
		assert(p.Observe(o) == Decision::Healthy);
		assert(p.Observe(sample(threshold + 250)) == Decision::Deferred);
	}
	{
		RecoveryPolicy p(true);
		for (unsigned t = 0; t < threshold; t += 250)
			p.Observe(sample(t));
		auto o = sample(threshold);
		o.jackConnected = false;
		assert(p.Observe(o) == Decision::Paused);
		assert(p.Observe(sample(threshold + 250)) == Decision::Deferred);
	}
	{
		RecoveryPolicy p;
		for (unsigned t = 0; t <= 20000; t += 250)
			assert(p.Observe(sample(t)) == Decision::Deferred);
	} // No explicit connect authorization: silence never starts recovery.
	{
		RecoveryPolicy p(true);
		auto o = sample(0);
		o.frames = 1; // Digital silence is enough to end initial verification.
		assert(p.Observe(o) == Decision::Healthy);
		for (unsigned t = 250; t <= 20000; t += 250)
			assert(p.Observe(sample(t)) == Decision::Deferred);
	} // Switching output after successful playback never rearms recovery.
	{
		RecoveryPolicy p(true);
		auto o = sample(0);
		o.jackConnected = false;
		assert(p.Observe(o) == Decision::Paused);
		for (unsigned t = 250; t <= 20000; t += 250)
			assert(p.Observe(sample(t)) == Decision::Deferred);
	} // Pause/resume is not a new explicit connect request.
	for (auto delivery : { 100u, 300u, 499u }) {
		RecoveryPolicy p(true);
		for (unsigned tick = 0; tick < delivery; tick += 100)
			assert(p.Observe(sample(tick)) == Decision::Wait);
		auto o = sample(delivery);
		o.frames = 1;
		assert(p.Observe(o) == Decision::Healthy);
		assert(p.Observe(sample(delivery + threshold)) == Decision::Deferred);
	}
	{
		RecoveryPolicy p(true);
		p.Observe(sample(0));
		assert(p.Observe(sample(300)) == Decision::Wait);
		assert(p.Observe(sample(500)) == Decision::Wait);
		assert(p.Observe(sample(700)) == Decision::Wait);
		assert(p.Observe(sample(800)) == Decision::Recover);
	} // A scheduler stall does not consume the shorter confirmation window.
	std::cout << "FLOW_POLICY PASS cases=21 threshold-ms=500 idle=NO_RECOVERY frames-with-zero-amplitude=HEALTHY authorized-startup-zero-frames=RECOVER after-first-frames=DEFERRED max-attempts=1 live-writes=0\n";
}
