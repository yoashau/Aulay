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
#include "../AudioFlowRecovery.hpp"

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

struct CaptureScenario {
	DWORD flags = 0;
	REFERENCE_TIME duration = -1;
	HANDLE event = nullptr;
	bool rejectEvents = false;
	unsigned activations = 0, starts = 0, stops = 0;
	UINT32 frames = 441;
};
struct TestCaptureClient : winrt::implements<TestCaptureClient, IAudioCaptureClient> {
	std::shared_ptr<CaptureScenario> state;
	explicit TestCaptureClient(std::shared_ptr<CaptureScenario> s)
		: state(std::move(s))
	{
	}
	HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64*, UINT64*) noexcept override
	{
		*data = nullptr;
		*frames = state->frames;
		*flags = AUDCLNT_BUFFERFLAGS_SILENT;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32) noexcept override
	{
		state->frames = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) noexcept override
	{
		*frames = state->frames;
		return S_OK;
	}
};
struct TestAudioClient : winrt::implements<TestAudioClient, IAudioClient> {
	std::shared_ptr<CaptureScenario> state;
	explicit TestAudioClient(std::shared_ptr<CaptureScenario> s)
		: state(std::move(s))
	{
	}
	HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration, REFERENCE_TIME periodicity, WAVEFORMATEX const*, GUID const*) noexcept override
	{
		if (mode != AUDCLNT_SHAREMODE_SHARED || periodicity != 0)
			return E_INVALIDARG;
		state->flags = flags;
		state->duration = duration;
		return state->rejectEvents && (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) ? E_INVALIDARG : S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32*) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, WAVEFORMATEX const*, WAVEFORMATEX**) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) noexcept override
	{
		*format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
		if (!*format)
			return E_OUTOFMEMORY;
		**format = {};
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE Start() noexcept override
	{
		if ((state->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !state->event)
			return E_FAIL;
		++state->starts;
		if (state->event)
			SetEvent(state->event);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Stop() noexcept override
	{
		++state->stops;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Reset() noexcept override { return S_OK; }
	HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) noexcept override
	{
		state->event = event;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetService(GUID const& iid, void** out) noexcept override
	{
		try {
			auto capture = winrt::make_self<TestCaptureClient>(state);
			return capture->QueryInterface(iid, out);
		} catch (...) {
			return winrt::to_hresult();
		}
	}
};
struct TestCaptureDevice : winrt::implements<TestCaptureDevice, IMMDevice> {
	std::shared_ptr<CaptureScenario> state;
	explicit TestCaptureDevice(std::shared_ptr<CaptureScenario> s)
		: state(std::move(s))
	{
	}
	HRESULT STDMETHODCALLTYPE Activate(GUID const& iid, DWORD, PROPVARIANT*, void** out) noexcept override
	{
		try {
			++state->activations;
			auto client = winrt::make_self<TestAudioClient>(state);
			return client->QueryInterface(iid, out);
		} catch (...) {
			return winrt::to_hresult();
		}
	}
	HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) noexcept override { return E_NOTIMPL; }
	HRESULT STDMETHODCALLTYPE GetState(DWORD* state) noexcept override
	{
		*state = DEVICE_STATE_ACTIVE;
		return S_OK;
	}
};
void TestCaptureNotifications()
{
	for (bool fallback : { false, true }) {
		auto scenario = std::make_shared<CaptureScenario>();
		scenario->rejectEvents = fallback;
		auto device = winrt::make_self<TestCaptureDevice>(scenario);
		AudioFlow::Capture capture;
		Check(capture.Open(device.get()) == S_OK, "capture opens with event or compatible timer buffering");
		Check(capture.eventDriven == !fallback && scenario->activations == (fallback ? 2u : 1u), "unsupported event buffering falls back once");
		Check(scenario->duration == (fallback ? 10000000 : 0), "event capture requests minimum engine buffer");
		Check(fallback || WaitForSingleObject(capture.ready.get(), 0) == WAIT_OBJECT_0, "audio-ready event is registered before Start");
		std::uint64_t frames = 0;
		Check(capture.Drain(frames) == S_OK && frames == 441, "silent packets verify frame delivery without reading samples");
		capture.Close();
		Check(!capture.client && !capture.capture && !capture.eventDriven && WaitForSingleObject(capture.ready.get(), 0) == WAIT_TIMEOUT, "close releases client and clears stale readiness");
	}
	auto discovery = winrt::make_self<AudioFlow::EndpointEvents>(L"");
	discovery->Notify(L"new-capture");
	Check(WaitForSingleObject(discovery->changed.get(), 0) == WAIT_OBJECT_0, "endpoint discovery notification wakes immediately");
	auto exact = winrt::make_self<AudioFlow::EndpointEvents>(L"my-capture");
	exact->Notify(L"other-capture");
	Check(WaitForSingleObject(exact->changed.get(), 0) == WAIT_TIMEOUT, "endpoint cycle ignores unrelated notifications");
	exact->Notify(L"my-capture");
	Check(WaitForSingleObject(exact->changed.get(), 0) == WAIT_OBJECT_0, "endpoint cycle receives exact notification");
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

void TestRecoveryGates()
{
	AudioFlow::State state;
	state.recoveryDeadline = GetTickCount64() + 10000;
	// A null enumerator is deliberate: rejected requests must not reach the
	// audio backend. No endpoint is disabled by these tests.
	auto rejected = [&] { return AudioFlow::Cycle(state, nullptr, L"", false) == E_ABORT; };
	Check(rejected(), "no explicit authorization cannot cycle endpoint");
	state.isOpened = [] { return false; };
	Check(rejected(), "closed connection cannot cycle endpoint");
	state.isOpened = []() -> bool { winrt::throw_hresult(E_FAIL); };
	Check(rejected(), "unreadable connection cannot cycle endpoint");
	unsigned calls = 0;
	state.isOpened = [&] { ++calls; return true; };
	state.stop = true;
	Check(rejected() && calls == 0, "cancelled request does not query or cycle endpoint");
	state.stop = false;
	state.recoveryDeadline = GetTickCount64();
	Check(rejected(), "expired authorization cannot cycle endpoint");
	state.recoveryDeadline = GetTickCount64() + 10000;
	state.isOpened = [&] { state.recoveryDeadline = 0; return true; };
	Check(rejected(), "deadline is rechecked after live-state query");
	state.recoveryDeadline = GetTickCount64() + 10000;
	state.isOpened = [&] { state.stop = true; return true; };
	Check(rejected(), "cancellation during live-state query vetoes recovery");
	state.stop = false;
	state.isOpened = [] { return true; };
	Check(AudioFlow::ConnectionOpened(state), "authorized live connection remains eligible");
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
		TestCaptureNotifications();
		std::cout << "RECOVERY_GATES_START\n";
		TestRecoveryGates();
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
