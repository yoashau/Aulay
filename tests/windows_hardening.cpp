// Native Windows tests for the actual deadline, routing and settings helpers.
// No radio power state is changed. Run via tests/run_windows_tests.cmd.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Audio.h>
#include "../AsyncUtil.hpp"
#include "../BluetoothRouting.hpp"

namespace fs = std::filesystem;
using winrt::Windows::Foundation::AsyncActionCompletedHandler;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;
using namespace winrt::Windows::Data::Json;
using namespace std::chrono;
HINSTANCE g_hInst = GetModuleHandleW(nullptr);
HWND g_hWnd = nullptr;
struct {
	bool reconnectEnabled = false;
	std::vector<std::wstring> desiredDevices;
} g_app;
void RecordDiagnostic(std::wstring_view, std::wstring_view) { }
std::wstring FormatDiagnosticHresult(HRESULT error)
{
	return std::to_wstring(error);
}
#include "../Util.hpp"
int notices = 0;
int portableWriteAttempts = 0;
std::wstring portablePrefix;
HRESULT WINAPI TestTaskDialog(HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR, TASKDIALOG_COMMON_BUTTON_FLAGS, PCWSTR, int*)
{
	++notices;
	return S_OK;
}
HANDLE WINAPI TestCreateFileW(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES sa,
	DWORD creation, DWORD flags, HANDLE templateFile)
{
	if ((access & GENERIC_WRITE) && !portablePrefix.empty() && std::wstring(path).find(portablePrefix) == 0)
		++portableWriteAttempts;
	return ::CreateFileW(path, access, sharing, sa, creation, flags, templateFile);
}
#define TaskDialog TestTaskDialog
#define CreateFileW TestCreateFileW
#define _(value) value
#include "../SettingsUtil.hpp"
#undef CreateFileW
#undef TaskDialog

int checks = 0, failures = 0;
void Check(bool value, char const* name)
{
	++checks;
	if (!value) {
		++failures;
		std::cout << "FAIL " << name << '\n';
	}
}

struct ControlledAction : winrt::implements<ControlledAction, IAsyncAction, winrt::Windows::Foundation::IAsyncInfo> {
	std::mutex mutex;
	AsyncStatus status = AsyncStatus::Started;
	winrt::hresult error = S_OK;
	AsyncActionCompletedHandler handler { nullptr };
	wil::unique_event cancelCalled { wil::EventOptions::ManualReset };
	uint32_t Id() const { return 1; }
	AsyncStatus Status()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return status;
	}
	winrt::hresult ErrorCode()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return error;
	}
	void Cancel() { cancelCalled.SetEvent(); } // deliberately ignores cancellation
	void Close() { }
	void GetResults()
	{
		std::lock_guard<std::mutex> lock(mutex);
		winrt::check_hresult(error);
	}
	AsyncActionCompletedHandler Completed()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return handler;
	}
	void Completed(AsyncActionCompletedHandler const& value)
	{
		AsyncStatus observed;
		{
			std::lock_guard<std::mutex> lock(mutex);
			handler = value;
			observed = status;
		}
		if (observed != AsyncStatus::Started)
			value(*this, observed);
	}
	void Finish(HRESULT result = S_OK)
	{
		AsyncActionCompletedHandler callback { nullptr };
		AsyncStatus observed;
		{
			std::lock_guard<std::mutex> lock(mutex);
			error = result;
			status = FAILED(result) ? AsyncStatus::Error : AsyncStatus::Completed;
			callback = handler;
			observed = status;
		}
		if (callback)
			callback(*this, observed);
	}
};

IAsyncOperation<int> ImmediateValue()
{
	co_return 7;
}

void TestDeadlines()
{
	auto start = steady_clock::now();
	for (int i = 0; i < 100; ++i)
		Check(AwaitBounded(ImmediateValue(), steady_clock::now() + seconds(5)).get() == 7, "immediate result");
	std::cout << "FAST_COMPLETIONS count=100 elapsed-us=" << duration_cast<microseconds>(steady_clock::now() - start).count() << '\n';
	Check(steady_clock::now() - start < seconds(2), "completed operations do not wait for timeout");
	{
		auto action = winrt::make_self<ControlledAction>();
		std::thread completion([action] { std::this_thread::sleep_for(milliseconds(20)); action->Finish(); });
		AwaitBounded(action.as<IAsyncAction>(), steady_clock::now() + seconds(2)).get();
		completion.join();
		Check(!action->cancelCalled.is_signaled(), "normal completion is not cancelled");
	}
	{
		auto action = winrt::make_self<ControlledAction>();
		bool timedOut = false;
		start = steady_clock::now();
		try {
			AwaitBounded(action.as<IAsyncAction>(), start + milliseconds(30)).get();
		} catch (winrt::hresult_error const& error) {
			timedOut = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		}
		Check(timedOut && steady_clock::now() - start < seconds(1), "hung driver is bounded even when Cancel is ignored");
		Check(action->cancelCalled.wait(1000), "timeout requests cancellation");
		action->Finish(); // must not dereference the destroyed waiter or any UI state
		Check(true, "late completion is harmless");
	}
	{
		auto action = winrt::make_self<ControlledAction>();
		auto cancellation = std::make_shared<AsyncCancellation>();
		std::thread shutdown([cancellation] { std::this_thread::sleep_for(milliseconds(20)); cancellation->Request(); });
		bool cancelled = false;
		start = steady_clock::now();
		try {
			AwaitBounded(action.as<IAsyncAction>(), start + seconds(5), cancellation).get();
		} catch (winrt::hresult_error const& error) {
			cancelled = error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED);
		}
		shutdown.join();
		Check(cancelled && steady_clock::now() - start < seconds(1), "shutdown wakes waiter before deadline");
		Check(action->cancelCalled.wait(1000), "shutdown requests underlying cancellation");
		action->Finish();
	}
	{
		auto cancellation = std::make_shared<AsyncCancellation>();
		cancellation->Request();
		bool cancelled = false;
		try {
			AwaitBounded(ImmediateValue(), steady_clock::now() + seconds(1), cancellation).get();
		} catch (winrt::hresult_error const& error) {
			cancelled = error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED);
		}
		Check(cancelled, "pre-cancelled operation is rejected");
	}
	{
		auto action = winrt::make_self<ControlledAction>();
		action->Finish(E_ACCESSDENIED);
		bool preserved = false;
		try {
			AwaitBounded(action.as<IAsyncAction>(), steady_clock::now() + seconds(1)).get();
		} catch (winrt::hresult_error const& error) {
			preserved = error.code() == E_ACCESSDENIED;
		}
		Check(preserved, "original HRESULT is preserved");
	}
	{
		auto deadline = steady_clock::now() + milliseconds(200);
		auto first = winrt::make_self<ControlledAction>();
		std::thread done1([first] { std::this_thread::sleep_for(milliseconds(130)); first->Finish(); });
		AwaitBounded(first.as<IAsyncAction>(), deadline).get();
		done1.join();
		auto second = winrt::make_self<ControlledAction>();
		std::thread done2([second] { std::this_thread::sleep_for(milliseconds(130)); second->Finish(); });
		bool timedOut = false;
		try {
			AwaitBounded(second.as<IAsyncAction>(), deadline).get();
		} catch (winrt::hresult_error const& error) {
			timedOut = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT);
		}
		done2.join();
		Check(timedOut, "stages share an absolute deadline rather than resetting their budget");
	}
}

void TestRouting()
{
	Check(SelectBluetoothAdapterIndex({ L"child", L"USB\\B", L"root" }, { L"USB\\A", L"USB\\B" }) == 1,
		"select associated second adapter, not first enabled radio");
	Check(!SelectBluetoothAdapterIndex({ L"unknown" }, { L"USB\\A", L"USB\\B" }), "ambiguous multi-adapter route is rejected");
	Check(SelectBluetoothAdapterIndex({}, { L"USB\\A" }) == 0, "single-adapter fallback");
	Check(!SelectBluetoothAdapterIndex({}, {}), "no adapter");
	Check(SelectBluetoothAdapterIndex({ L"usb\\a" }, { L"USB\\A", L"USB\\B" }) == 0, "case-insensitive instance IDs");
	Check(!SelectBluetoothAdapterIndex({ L"USB\\A" }, { L"USB\\A", L"usb\\a" }), "duplicate matching identities are ambiguous");
	Check(SelectBluetoothAdapterIndex({ L"USB\\B", L"USB\\A" }, { L"USB\\A", L"USB\\B" }) == 1, "nearest ancestor wins");
}

void SetDirectoryAccess(fs::path const& path, bool writable)
{
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	winrt::check_bool(ConvertStringSecurityDescriptorToSecurityDescriptorW(
		writable ? L"D:P(A;OICI;GA;;;WD)" : L"D:P(A;OICI;GRGX;;;WD)", SDDL_REVISION_1, &descriptor, nullptr));
	auto release = wil::scope_exit([&] { LocalFree(descriptor); });
	BOOL present = FALSE, defaulted = FALSE;
	PACL acl = nullptr;
	winrt::check_bool(GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted));
	winrt::check_win32(SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr));
}

void TestSettings()
{
	auto root = fs::temp_directory_path() / (L"Aulay-settings-test-" + std::to_wstring(GetCurrentProcessId()));
	auto portable = root / L"portable";
	auto fallback = root / L"fallback";
	fs::create_directories(portable);
	auto cleanup = wil::scope_exit([&] {
		SetDirectoryAccess(portable, true);
		if (fs::is_directory(fallback))
			SetDirectoryAccess(fallback, true);
		std::error_code error;
		fs::remove_all(root, error);
	});
	g_settingsStorage = { portable / CONFIG_NAME, fallback / CONFIG_NAME, {}, false };
	portablePrefix = portable.wstring();
	g_app.reconnectEnabled = true;
	g_app.desiredDevices = { L"old-phone" };
	SaveSettings();
	Check(fs::is_regular_file(g_settingsStorage.portablePath), "normal save uses portable config");
	Check(!fs::exists(fallback), "normal save does not create fallback directory");
	Check(!fs::exists(g_settingsStorage.portablePath.wstring() + L".tmp"), "atomic save consumes temporary file");
	fs::last_write_time(g_settingsStorage.portablePath, fs::file_time_type::clock::now() - seconds(10));
	SetDirectoryAccess(portable, false);
	g_app.desiredDevices = { L"new-phone" };
	SaveSettings();
	Check(fs::is_regular_file(g_settingsStorage.fallbackPath), "read-only directory falls back to writable storage");
	Check(g_settingsStorage.activePath == g_settingsStorage.fallbackPath, "fallback path is cached");
	auto writes = portableWriteAttempts;
	SaveSettings();
	Check(portableWriteAttempts == writes, "cached fallback skips repeated failing primary writes");
	Check(notices == 0, "successful fallback has no blocking dialog");
	g_settingsStorage.activePath.clear();
	LoadSettings();
	Check(g_app.reconnectEnabled && g_app.desiredDevices == std::vector<std::wstring> { L"new-phone" },
		"restart loads newer fallback, not stale portable config");
	{
		std::ofstream stream(g_settingsStorage.fallbackPath);
		stream << "{ invalid json";
	}
	LoadSettings();
	Check(g_app.desiredDevices == std::vector<std::wstring> { L"old-phone" }, "corrupt fallback still loads valid portable config");
	WriteSettingsAtomically(g_settingsStorage.fallbackPath, "{\"reconnect\":true,\"lastDevices\":[\"phone\",\"phone\",\"\",3]}");
	LoadSettings();
	Check(g_app.desiredDevices == std::vector<std::wstring> { L"phone" }, "fallback retains deduplication and type checks");
	SetDirectoryAccess(fallback, false);
	g_settingsStorage.activePath = g_settingsStorage.portablePath;
	SaveSettings();
	SaveSettings();
	Check(notices == 1, "both paths failing shows a single error notification");
}

void ProbeLocalRouting()
{
	using namespace winrt::Windows::Devices::Enumeration;
	using namespace winrt::Windows::Media::Audio;
	using namespace winrt::Windows::Devices::Bluetooth;
	auto deadline = steady_clock::now() + seconds(10);
	auto adapters = AwaitBounded(DeviceInformation::FindAllAsync(BluetoothAdapter::GetDeviceSelector()), deadline).get();
	auto devices = AwaitBounded(DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector()), deadline).get();
	std::vector<std::wstring> instances;
	for (auto const& adapter : adapters)
		instances.push_back(DeviceInterfaceInstanceId(std::wstring(adapter.Id())));
	int mapped = 0, ancestorMatches = 0;
	for (auto const& device : devices) {
		auto ancestors = DeviceAncestorIds(std::wstring(device.Id()));
		auto index = SelectBluetoothAdapterIndex(ancestors, instances);
		if (index) {
			if (std::any_of(ancestors.begin(), ancestors.end(), [&](auto const& id) { return SameDeviceInstance(id, instances[*index]); }))
				++ancestorMatches;
			auto start = steady_clock::now();
			auto radio = ResolveBluetoothRadio(std::wstring(device.Id()), deadline, {}).get();
			auto coldUs = duration_cast<microseconds>(steady_clock::now() - start).count();
			start = steady_clock::now();
			auto cached = ResolveBluetoothRadio(std::wstring(device.Id()), deadline, {}).get();
			auto warmUs = duration_cast<microseconds>(steady_clock::now() - start).count();
			if (radio && radio == cached)
				++mapped;
			std::cout << "ROUTE_LATENCY cold-us=" << coldUs << " cached-us=" << warmUs << '\n';
		}
	}
	std::cout << "READ_ONLY_ROUTE_PROBE adapters=" << adapters.Size() << " audio-devices=" << devices.Size() << " mapped=" << mapped << " ancestor-matches=" << ancestorMatches << '\n';
}

int main(int argc, char** argv)
{
	std::cout << std::unitbuf;
	std::cout << "NATIVE_START\n";
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	try {
		std::cout << "DEADLINES_START\n";
		TestDeadlines();
		std::cout << "ROUTING_START\n";
		TestRouting();
		std::cout << "SETTINGS_START\n";
		TestSettings();
		if (argc > 1 && std::string(argv[1]) == "--probe")
			ProbeLocalRouting();
	} catch (winrt::hresult_error const& error) {
		++failures;
		std::cout << "EXCEPTION HRESULT=" << std::hex << static_cast<uint32_t>(error.code()) << std::dec << '\n';
	} catch (std::exception const& error) {
		++failures;
		std::cout << "EXCEPTION " << error.what() << '\n';
	}
	std::cout << "WINDOWS_HARDENING checks=" << checks << " failures=" << failures << '\n';
	return failures ? 1 : 0;
}
