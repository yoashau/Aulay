#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <windows.h>
#include <wil/resource.h>

// The low-level hook must never wait for the XAML thread: Windows delivers
// every mouse event through it, including movement that we do not handle.
class FlyoutMouseHook {
	struct State {
		HWND window;
		UINT message;
		std::atomic_bool active { true };
		std::atomic_bool installed { false };
		wil::unique_event stop { wil::EventOptions::ManualReset };
		wil::unique_event ready { wil::EventOptions::ManualReset };

		State(HWND owner, UINT clickMessage)
			: window(owner)
			, message(clickMessage)
		{
		}
	};

	std::shared_ptr<State> state;
	inline static thread_local State* current = nullptr;

	static LRESULT CALLBACK HookProc(int code, WPARAM message, LPARAM data) noexcept
	{
		auto context = current;
		if (code >= 0 && context && context->active && data
			&& (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
				|| message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN
				|| message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN)) {
			auto point = reinterpret_cast<MSLLHOOKSTRUCT const*>(data)->pt;
			PostMessageW(context->window, context->message,
				static_cast<WPARAM>(static_cast<INT_PTR>(point.x)), static_cast<LPARAM>(point.y));
		}
		return CallNextHookEx(nullptr, code, message, data);
	}

	static void Run(std::shared_ptr<State> context) noexcept
	{
		current = context.get();
		MSG message {};
		PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // Create the thread's message queue.
		auto hook = context->active ? SetWindowsHookExW(WH_MOUSE_LL, HookProc, nullptr, 0) : nullptr;
		context->installed = hook != nullptr;
		context->ready.SetEvent();
		if (!hook && context->active)
			OutputDebugStringW(L"Aulay: unable to install the device flyout mouse hook.\n");

		if (hook) {
			auto stop = context->stop.get();
			while (context->active) {
				auto result = MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT);
				if (result != WAIT_OBJECT_0 + 1)
					break;
				while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
					if (message.message == WM_QUIT)
						context->active = false;
					else {
						TranslateMessage(&message);
						DispatchMessageW(&message);
					}
				}
			}
			UnhookWindowsHookEx(hook);
		}
		current = nullptr;
	}

public:
	~FlyoutMouseHook() { Stop(); }

	bool Start(HWND window, UINT message) noexcept
	{
		Stop();
		try {
			auto context = std::make_shared<State>(window, message);
			std::thread([context] { Run(context); }).detach();
			state = std::move(context);
			return true;
		} catch (...) {
			return false;
		}
	}

	void Stop() noexcept
	{
		if (auto context = std::exchange(state, {})) {
			context->active = false;
			context->stop.SetEvent();
		}
	}

	bool Active() const noexcept { return bool(state); }

	// Used by the native integration test; the UI never waits for this thread.
	bool WaitUntilReady(DWORD timeout) const noexcept
	{
		auto context = state;
		return context && WaitForSingleObject(context->ready.get(), timeout) == WAIT_OBJECT_0 && context->installed;
	}
};
