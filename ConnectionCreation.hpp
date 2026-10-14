#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <winrt/Windows.Media.Audio.h>

// A synchronous driver call cannot be interrupted. Keep it off the UI and
// retain its slot until its result has been handed off or closed in the worker.
namespace ConnectionCreation {
using winrt::Windows::Media::Audio::AudioPlaybackConnection;
struct State {
	std::mutex mutex;
	AudioPlaybackConnection connection { nullptr };
	HRESULT error = E_PENDING;
	bool abandoned = false;
	bool claimed = false;
	std::atomic_bool finished { false };
	wil::unique_event ready { wil::EventOptions::ManualReset };
	wil::unique_event released { wil::EventOptions::ManualReset };
};
inline void Abandon(State& state) noexcept
{
	std::lock_guard<std::mutex> lock(state.mutex);
	state.abandoned = true;
	state.released.SetEvent();
}
inline winrt::fire_and_forget Run(std::wstring id, std::shared_ptr<State> state)
{
	try {
		co_await winrt::resume_background();
		AudioPlaybackConnection connection { nullptr };
		HRESULT error = S_OK;
		try {
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				if (state->abandoned) {
					state->finished = true;
					state->ready.SetEvent();
					co_return;
				}
			}
			connection = AudioPlaybackConnection::TryCreateFromId(id);
		} catch (...) {
			error = static_cast<HRESULT>(winrt::to_hresult());
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->connection = std::move(connection);
			state->error = error;
		}
		state->ready.SetEvent();
		co_await winrt::resume_on_signal(state->released.get());
		// Signal waits resume on the pool (or inline on this existing worker).
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			if (!state->claimed)
				connection = std::move(state->connection);
		}
		if (connection)
			connection.Close();
	} catch (...) {
		// Late completion/close has no UI or application-state dependency.
		std::lock_guard<std::mutex> lock(state->mutex);
		state->error = static_cast<HRESULT>(winrt::to_hresult());
		state->ready.SetEvent();
	}
	state->finished = true;
}
inline std::shared_ptr<State> Start(std::wstring id)
{
	auto state = std::make_shared<State>();
	Run(std::move(id), state);
	return state;
}
inline winrt::Windows::Foundation::IAsyncAction WaitReady(std::shared_ptr<State> state)
{
	co_await winrt::resume_on_signal(state->ready.get());
}
inline AudioPlaybackConnection Take(State& state)
{
	std::lock_guard<std::mutex> lock(state.mutex);
	if (state.abandoned)
		winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
	winrt::check_hresult(state.error);
	state.claimed = true;
	state.released.SetEvent();
	return std::move(state.connection);
}
}
