#pragma once

// Completion-driven deadlines: a successful operation never waits for its timer.
// Callbacks capture only weak waiter state, not the UI or application globals.
using AsyncDeadline = std::chrono::steady_clock::time_point;

struct AsyncWaitSignal
{
	wil::unique_event signal{ wil::EventOptions::ManualReset };
	std::atomic_bool completed{ false };
	std::atomic_bool cancelled{ false };
};

class AsyncCancellation
{
	std::mutex mutex;
	std::atomic_bool requested{ false };
	std::vector<std::weak_ptr<AsyncWaitSignal>> waiters;
public:
	bool Requested() const noexcept { return requested.load(); }

	void Register(std::shared_ptr<AsyncWaitSignal> const& waiter)
	{
		std::lock_guard<std::mutex> lock(mutex);
		waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
			[](auto const& item) { return item.expired(); }), waiters.end());
		if (requested)
		{
			waiter->cancelled = true;
			waiter->signal.SetEvent();
		}
		else
			waiters.emplace_back(waiter);
	}

	void Request() noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		requested = true;
		for (auto const& weak : waiters)
		{
			if (auto waiter = weak.lock())
			{
				waiter->cancelled = true;
				waiter->signal.SetEvent();
			}
		}
		waiters.clear();
	}
};

inline void CheckAsyncDeadline(AsyncDeadline deadline, std::shared_ptr<AsyncCancellation> const& cancellation = {})
{
	if (cancellation && cancellation->Requested())
		winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
	if (std::chrono::steady_clock::now() >= deadline)
		winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
}

template<typename Async>
winrt::fire_and_forget CancelAsyncInBackground(Async operation)
{
	try
	{
		// A broken implementation of Cancel must not stall the UI or its waiter.
		co_await winrt::resume_background();
		operation.Cancel();
	}
	catch (...) {}
}

template<typename Async>
winrt::Windows::Foundation::IAsyncAction AwaitCompletionUntil(
	Async operation, AsyncDeadline deadline, std::shared_ptr<AsyncCancellation> cancellation = {})
{
	using winrt::Windows::Foundation::AsyncStatus;
	try
	{
		CheckAsyncDeadline(deadline, cancellation);
		if (operation.Status() != AsyncStatus::Started)
			co_return;

		auto waiter = std::make_shared<AsyncWaitSignal>();
		if (cancellation)
			cancellation->Register(waiter);
		operation.Completed([weak = std::weak_ptr<AsyncWaitSignal>(waiter)](auto const&, auto const&) noexcept {
			if (auto state = weak.lock())
			{
				state->completed = true;
				state->signal.SetEvent();
			}
		});

		auto remaining = deadline - std::chrono::steady_clock::now();
		if (remaining > std::chrono::steady_clock::duration::zero())
			co_await winrt::resume_on_signal(waiter->signal.get(),
				std::chrono::ceil<std::chrono::milliseconds>(remaining));

		if (waiter->cancelled)
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
		if (!waiter->completed)
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
	}
	catch (...)
	{
		CancelAsyncInBackground(operation);
		throw;
	}
}

template<typename T>
winrt::Windows::Foundation::IAsyncOperation<T> AwaitBounded(
	winrt::Windows::Foundation::IAsyncOperation<T> operation,
	AsyncDeadline deadline, std::shared_ptr<AsyncCancellation> cancellation = {})
{
	co_await AwaitCompletionUntil(operation, deadline, std::move(cancellation));
	co_return operation.GetResults();
}

inline winrt::Windows::Foundation::IAsyncAction AwaitBounded(
	winrt::Windows::Foundation::IAsyncAction operation,
	AsyncDeadline deadline, std::shared_ptr<AsyncCancellation> cancellation = {})
{
	co_await AwaitCompletionUntil(operation, deadline, std::move(cancellation));
	operation.GetResults();
}
