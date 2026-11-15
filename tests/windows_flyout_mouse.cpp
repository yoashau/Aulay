#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <wil/resource.h>
#include "FlyoutMouseHook.hpp"

namespace {
constexpr ULONG_PTR inputTag = 0xA17A0123;
wil::unique_event observerReady { wil::EventOptions::ManualReset };
wil::unique_event observerStop { wil::EventOptions::ManualReset };
wil::unique_event inputObserved { wil::EventOptions::ManualReset };
std::atomic_bool observerInstalled { false };
std::atomic<ULONGLONG> inputSeenAt { 0 };

LRESULT CALLBACK ObserveMouse(int code, WPARAM message, LPARAM data)
{
	if (code >= 0 && message == WM_MOUSEMOVE && data) {
		auto event = reinterpret_cast<MSLLHOOKSTRUCT const*>(data);
		if ((event->flags & LLMHF_INJECTED) && event->dwExtraInfo == inputTag) {
			inputSeenAt = GetTickCount64();
			inputObserved.SetEvent();
		}
	}
	return CallNextHookEx(nullptr, code, message, data);
}

void RunObserver()
{
	MSG message {};
	PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
	auto hook = SetWindowsHookExW(WH_MOUSE_LL, ObserveMouse, nullptr, 0);
	observerInstalled = hook != nullptr;
	observerReady.SetEvent();
	if (!hook)
		return;
	auto stop = observerStop.get();
	while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	UnhookWindowsHookEx(hook);
}
}

int main(int argc, char** argv)
{
	int checks = 0, failures = 0;
	auto check = [&](bool ok, char const* name) {
		++checks;
		if (!ok) {
			++failures;
			std::cout << "FAIL " << name << '\n';
		}
	};
	// A message-only target prevents a test click from reaching other windows.
	auto target = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
	check(target != nullptr, "create message-only target");
	if (!target)
		return 1;

	FlyoutMouseHook hook;
	for (int i = 0; i < 20; ++i) {
		check(hook.Start(target, WM_APP + 10), "start hook worker");
		check(hook.WaitUntilReady(3000), "hook worker installs");
		hook.Stop();
		check(!hook.Active(), "stop detaches the active hook");
	}

	// Optional on an interactive desktop: tagged one-pixel movements return the
	// cursor to its starting position. The observer is installed before the hook,
	// so it is reached only after that hook forwards the event. The target/UI
	// thread deliberately does not pump messages while the input is sent.
	if (argc > 1 && std::string(argv[1]) == "--input") {
		std::thread observer(RunObserver);
		check(WaitForSingleObject(observerReady.get(), 3000) == WAIT_OBJECT_0 && observerInstalled, "observer hook installs");
		check(hook.Start(target, WM_APP + 11) && hook.WaitUntilReady(3000), "production hook installs after observer");
		std::atomic<ULONGLONG> sentAt { 0 };
		std::atomic<UINT> sent { 0 };
		std::thread inject([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			INPUT input[2] {};
			for (auto& event : input) {
				event.type = INPUT_MOUSE;
				event.mi.dwFlags = MOUSEEVENTF_MOVE;
				event.mi.dwExtraInfo = inputTag;
			}
			input[0].mi.dx = 1;
			input[1].mi.dx = -1;
			sentAt = GetTickCount64();
			sent = SendInput(2, input, sizeof(INPUT));
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		inject.join();
		check(sent == 2, "out-and-back test input sent");
		check(WaitForSingleObject(inputObserved.get(), 0) == WAIT_OBJECT_0, "mouse hook forwarded while target thread was blocked");
		if (inputSeenAt && sentAt) {
			auto delay = inputSeenAt.load() - sentAt.load();
			std::cout << "FLYOUT_HOOK_INPUT_DELAY_MS=" << delay << '\n';
			check(delay < 200, "hook forwarding did not wait for target thread");
		}
		hook.Stop();
		observerStop.SetEvent();
		observer.join();
	}
	DestroyWindow(target);
	std::cout << "FLYOUT_MOUSE_HOOK checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
