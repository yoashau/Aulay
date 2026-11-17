#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <windows.h>
#include <wil/resource.h>

// Low-level input hooks must never wait for the XAML thread. In particular,
// mouse movement can arrive hundreds of times a second while a game is active.
class FlyoutInputHook {
	struct State {
		HWND window;
		UINT clickMessage;
		UINT activityMessage;
		DWORD ownerProcessId;
		std::atomic_bool active { true };
		std::atomic_bool mouseInstalled { false };
		std::atomic_bool keyboardInstalled { false };
		std::atomic_bool movementPending { false };
		wil::unique_event stop { wil::EventOptions::ManualReset };
		wil::unique_event ready { wil::EventOptions::ManualReset };

		State(HWND owner, UINT click, UINT activity)
			: window(owner)
			, clickMessage(click)
			, activityMessage(activity)
			, ownerProcessId(GetCurrentProcessId())
		{
		}
	};

	std::shared_ptr<State> state;
	inline static thread_local State* current = nullptr;

	static HWND ForeignForeground(State const& context) noexcept
	{
		auto foreground = GetForegroundWindow();
		DWORD processId = 0;
		if (foreground)
			GetWindowThreadProcessId(foreground, &processId);
		return processId && processId != context.ownerProcessId ? foreground : nullptr;
	}

	static LRESULT CALLBACK MouseHookProc(int code, WPARAM message, LPARAM data) noexcept
	{
		auto context = current;
		if (code >= 0 && context && context->active && data
			&& (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
				|| message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN
				|| message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN)) {
			auto point = reinterpret_cast<MSLLHOOKSTRUCT const*>(data)->pt;
			PostMessageW(context->window, context->clickMessage,
				static_cast<WPARAM>(static_cast<INT_PTR>(point.x)), static_cast<LPARAM>(point.y));
		} else if (code >= 0 && context && context->active && message == WM_MOUSEMOVE
			&& !context->movementPending && ForeignForeground(*context)) {
			// Keep at most one notification outstanding. The UI checks the latest
			// cursor position again before releasing this gate, so a final movement
			// is not lost while a notification is queued.
			context->movementPending = true;
			if (!PostMessageW(context->window, context->activityMessage, 0, 0))
				context->movementPending = false;
		}
		return CallNextHookEx(nullptr, code, message, data);
	}

	static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM message, LPARAM data) noexcept
	{
		auto context = current;
		if (code >= 0 && context && context->active && data
			&& (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) {
			if (auto foreground = ForeignForeground(*context)) {
				// Only the destination window is forwarded, never the key or its text.
				PostMessageW(context->window, context->activityMessage, reinterpret_cast<WPARAM>(foreground), 1);
			}
		}
		return CallNextHookEx(nullptr, code, message, data);
	}

	static void Run(std::shared_ptr<State> context) noexcept
	{
		current = context.get();
		MSG message {};
		PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // Create the thread's message queue.
		auto mouseHook = context->active ? SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, nullptr, 0) : nullptr;
		auto keyboardHook = context->active ? SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, nullptr, 0) : nullptr;
		context->mouseInstalled = mouseHook != nullptr;
		context->keyboardInstalled = keyboardHook != nullptr;
		context->ready.SetEvent();
		if (!mouseHook && context->active)
			OutputDebugStringW(L"Aulay: unable to install the device flyout mouse hook.\n");
		if (!keyboardHook && context->active)
			OutputDebugStringW(L"Aulay: unable to install the device flyout keyboard hook.\n");

		if (mouseHook || keyboardHook) {
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
			if (mouseHook)
				UnhookWindowsHookEx(mouseHook);
			if (keyboardHook)
				UnhookWindowsHookEx(keyboardHook);
		}
		current = nullptr;
	}

public:
	~FlyoutInputHook() { Stop(); }

	bool Start(HWND window, UINT clickMessage, UINT activityMessage) noexcept
	{
		Stop();
		try {
			auto context = std::make_shared<State>(window, clickMessage, activityMessage);
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

	void AcknowledgeMovement() noexcept
	{
		if (auto context = state)
			context->movementPending = false;
	}

	// Used by the native integration test; the UI never waits for this thread.
	bool WaitUntilReady(DWORD timeout) const noexcept
	{
		auto context = state;
		return context && WaitForSingleObject(context->ready.get(), timeout) == WAIT_OBJECT_0
			&& context->mouseInstalled && context->keyboardInstalled;
	}
};
