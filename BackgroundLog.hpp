#pragma once
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aulay::logging {
// Worker owns output callbacks. Producers never hold this mutex during disk I/O.
struct Queue : std::enable_shared_from_this<Queue> {
	using Batch = std::vector<std::wstring>;
	std::mutex mutex;
	std::condition_variable wake, completed;
	std::deque<std::wstring> pending;
	size_t bytes = 0;
	uint64_t dropped = 0, totalDropped = 0, errors = 0;
	bool stopping = false, finished = false;
	static constexpr size_t maxEntries = 4096, maxBytes = 8 * 1024 * 1024;
	bool Push(std::wstring entry)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping)
			return false;
		auto size = entry.size() * sizeof(wchar_t);
		if (pending.size() >= maxEntries || size > maxBytes - bytes) {
			++dropped;
			++totalDropped;
			wake.notify_one();
			return false;
		}
		pending.push_back(std::move(entry));
		bytes += size;
		wake.notify_one();
		return true;
	}
	void Stop()
	{
		std::lock_guard<std::mutex> lock(mutex);
		stopping = true;
		wake.notify_all();
	}
	bool WaitFor(std::chrono::milliseconds limit)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return completed.wait_for(lock, limit, [&] { return finished; });
	}
	void Start(std::function<void(Batch const&)> write, std::function<void()> close)
	{
		auto self = shared_from_this();
		std::thread([self, write = std::move(write), close = std::move(close)] {
			try {
				for (;;) {
					Batch batch;
					bool last = false;
					{
						std::unique_lock<std::mutex> lock(self->mutex);
						self->wake.wait(lock, [&] { return self->stopping || !self->pending.empty() || self->dropped; });
						batch.reserve(self->pending.size() + 1);
						while (!self->pending.empty()) {
							batch.push_back(std::move(self->pending.front()));
							self->pending.pop_front();
						}
						self->bytes = 0;
						if (self->dropped) {
							batch.push_back(L"[diagnostics] app-log-queue-dropped=" + std::to_wstring(self->dropped) + L"\r\n");
							self->dropped = 0;
						}
						last = self->stopping;
					}
					if (!batch.empty())
						write(batch); // deliberately outside producer mutex
					if (last)
						break;
				}
			} catch (...) {
				std::lock_guard<std::mutex> lock(self->mutex);
				++self->errors;
			}
			try {
				close();
			} catch (...) {
				std::lock_guard<std::mutex> lock(self->mutex);
				++self->errors;
			}
			{
				std::lock_guard<std::mutex> lock(self->mutex);
				self->finished = true;
				self->stopping = true;
			}
			self->completed.notify_all();
		}).detach();
	}
};
}
