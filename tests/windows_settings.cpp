// Generated with the actual SettingsSave.hpp by generate_settings_test.py.
// Only disk writes and the dispatcher are doubles; WinRT coroutines and waits are real.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.h>
using namespace std::chrono;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
namespace fs = std::filesystem;
struct TestDispatcher {
} g_uiDispatcher;
std::mutex uiMutex, evidenceMutex;
std::deque<std::function<void()>> uiQueue;
DWORD uiThread;
namespace winrt {
inline auto resume_foreground(TestDispatcher)
{
	struct Awaitable {
		bool await_ready() const noexcept { return false; }
		void await_resume() const noexcept { }
		void await_suspend(std::coroutine_handle<> continuation) const
		{
			std::lock_guard lock(uiMutex);
			uiQueue.push_back([continuation] { continuation.resume(); });
		}
	};
	return Awaitable {};
}
}
bool PumpOne()
{
	std::function<void()> callback;
	{
		std::lock_guard lock(uiMutex);
		if (uiQueue.empty())
			return false;
		callback = std::move(uiQueue.front());
		uiQueue.pop_front();
	}
	callback();
	return true;
}
bool PumpUntil(std::function<bool()> ready, milliseconds timeout = seconds(10))
{
	auto deadline = steady_clock::now() + timeout;
	while (!ready()) {
		if (steady_clock::now() >= deadline)
			return false;
		if (!PumpOne())
			Sleep(1);
	}
	return true;
}
struct {
	std::vector<std::wstring> desiredDevices;
} g_app;
struct {
	fs::path activePath, portablePath = L"fixture.json", fallbackPath;
	bool saveErrorShown = false;
} g_settingsStorage;
HWND g_hWnd = nullptr;
bool stopping = true, failSnapshot = false;
std::atomic_bool failWrite { false };
std::string snapshot;
std::vector<std::string> writes;
std::vector<std::wstring> logs;
wil::unique_event writeEntered { wil::EventOptions::ManualReset };
wil::unique_event releaseWrite { wil::EventOptions::ManualReset | wil::EventOptions::Signaled };
int dialogs = 0;
bool IsStopping()
{
	return stopping;
}
std::string BuildSettingsJson()
{
	winrt::check_bool(GetCurrentThreadId() == uiThread);
	if (failSnapshot)
		winrt::throw_hresult(E_FAIL);
	return snapshot;
}
void WriteSettingsAtomically(fs::path const&, std::string const& value)
{
	winrt::check_bool(GetCurrentThreadId() != uiThread);
	writeEntered.SetEvent();
	releaseWrite.wait();
	if (failWrite)
		winrt::throw_hresult(E_ACCESSDENIED);
	std::lock_guard lock(evidenceMutex);
	writes.push_back(value);
}
void RecordDiagnostic(std::wstring_view, std::wstring const& message)
{
	std::lock_guard lock(evidenceMutex);
	logs.push_back(message);
}
std::wstring FormatDiagnosticHresult(HRESULT hr)
{
	return std::to_wstring(hr);
}
HRESULT TestTaskDialog(HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR, TASKDIALOG_COMMON_BUTTON_FLAGS, PCWSTR, int*)
{
	++dialogs;
	return S_OK;
}
#define TaskDialog TestTaskDialog
#define _(text) text
// SETTINGS_IMPLEMENTATION
#undef TaskDialog
IAsyncAction Flush()
{
	// FLUSH_CALL
	co_return;
}
int checks = 0, failures = 0;
void Check(bool ok, char const* label)
{
	++checks;
	if (!ok) {
		++failures;
		std::cout << "FAIL " << label << '\n';
	}
}
bool HasLog(wchar_t const* text)
{
	std::lock_guard lock(evidenceMutex);
	for (auto const& line : logs)
		if (line.find(text) != std::wstring::npos)
			return true;
	return false;
}
void Reset()
{
	releaseWrite.SetEvent();
	Check(PumpUntil([] { return !g_settingsSaveWorkerRunning; }), "worker settles before next case");
	while (PumpOne()) { }
	std::lock_guard lock(evidenceMutex);
	writes.clear();
	logs.clear();
	writeEntered.ResetEvent();
	g_settingsPending = {};
	g_settingsStorage.saveErrorShown = false;
	failWrite = false;
	failSnapshot = false;
	dialogs = 0;
}
void Complete(IAsyncAction const& action)
{
	Check(PumpUntil([&] { return action.Status() != AsyncStatus::Started; }), "flush finishes within bound");
	action.GetResults();
}
int main()
{
	std::cout << std::unitbuf;
	uiThread = GetCurrentThreadId();
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	try {
		snapshot = "fast";
		auto start = steady_clock::now();
		Complete(Flush());
		auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
		Check(elapsed < 2000, "fast save does not wait five seconds");
		Check(HasLog(L"shutdown settings drain complete"), "fast save reports completion");
		std::cout << "FAST_SAVE elapsed-ms=" << elapsed << '\n';
		Reset();

		releaseWrite.ResetEvent();
		snapshot = "old";
		QueueSaveSettings();
		Check(writeEntered.wait(2000), "old snapshot write starts");
		snapshot = "latest";
		std::thread release([] { Sleep(80); releaseWrite.SetEvent(); });
		auto drain = Flush();
		Complete(drain);
		release.join();
		{
			std::lock_guard lock(evidenceMutex);
			Check(writes == std::vector<std::string> { "old", "latest" }, "drain commits latest coalesced snapshot before return");
		}
		Reset();

		releaseWrite.ResetEvent();
		snapshot = "slow";
		start = steady_clock::now();
		auto slow = Flush();
		Complete(slow);
		elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
		Check(elapsed >= 4500 && elapsed < 8000, "stuck writer is bounded at five seconds");
		Check(HasLog(L"shutdown settings drain timeout-ms=5000"), "timeout explicitly reports unsaved snapshot");
		Check(!HasLog(L"shutdown settings drain complete"), "timeout is not reported as success");
		std::cout << "SLOW_SAVE elapsed-ms=" << elapsed << '\n';
		Reset();

		failWrite = true;
		snapshot = "failure";
		Complete(Flush());
		Check(HasLog(L"shutdown settings drain failed"), "write failure is not reported as success");
		Check(dialogs == 0, "shutdown failure does not block on a modal dialog");
		Reset();

		failSnapshot = true;
		Complete(Flush());
		Check(HasLog(L"shutdown settings drain failed"), "snapshot failure is not reported as success");
		Reset();
	} catch (...) {
		++failures;
		releaseWrite.SetEvent();
		std::cout << "EXCEPTION " << static_cast<HRESULT>(winrt::to_hresult()) << '\n';
	}
	std::cout << "SETTINGS_SHUTDOWN checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
