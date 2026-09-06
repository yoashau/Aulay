#pragma once
#include "DebugAudioDiagnostics.hpp"
#include "DiagnosticSampling.hpp"
#include <audioclient.h>
#include <winsvc.h>
#include <thread>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <algorithm>
#include <chrono>

// A separate, read-only MTA worker owns all COM subscriptions and disk I/O.
// No callback calls app code, waits for the UI, or changes playback/volume/radio.
namespace DebugAudio {
struct Event {
	uint64_t tick = GetTickCount64();
	std::wstring text;
	bool snapshot = false;
	bool marker = false;
	winrt::com_ptr<IAudioSessionControl> session;
	std::wstring endpoint;
};
struct State {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<Event> queue;
	std::atomic<uint64_t> dropped { 0 }, ioErrors { 0 };
	std::atomic<bool> stopping { false }, finished { false }, fatal { false };
	wil::unique_event finishedSignal { wil::EventOptions::ManualReset };
	std::atomic<uint32_t> pendingSnapshots { 0 };
	std::filesystem::path directory;
	uint64_t salt = 0;
	std::wstring run;
	void Push(Event event, bool callback = false) noexcept
	{
		try {
			std::unique_lock<std::mutex> lock(mutex, std::defer_lock);
			if (callback) {
				if (!lock.try_lock()) {
					++dropped;
					return;
				}
			} else
				lock.lock();
			if (stopping || finished)
				return;
			if (queue.size() >= 4096 && !event.marker && !event.snapshot) {
				++dropped;
				return;
			}
			// Explicit user markers and requested snapshots are never displaced by polling.
			bool snapshot = event.snapshot;
			queue.push_back(std::move(event));
			if (snapshot)
				++pendingSnapshots;
			lock.unlock();
			wake.notify_one();
		} catch (...) {
			++dropped;
		}
	}
};
std::atomic<std::shared_ptr<State>> monitor; // callbacks retain State independently

std::string Utf8(std::wstring const& value)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	std::string out(n, '\0');
	if (n)
		WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
	return out;
}
std::wstring Stamp(uint64_t tick)
{
	SYSTEMTIME t {};
	GetSystemTime(&t);
	wchar_t stamp[64] {};
	swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	return L"[written-utc=" + std::wstring(stamp) + L" event-tick-ms=" + std::to_wstring(tick) + L"] ";
}

struct Journal {
	State& state;
	std::ofstream stream;
	size_t bytes = 0, historyBytes = 0;
	uint64_t sequence = 0, nextMarker = 0;
	std::deque<std::pair<uint64_t, std::string>> history;
	struct Bundle {
		std::ofstream file;
		uint64_t until;
		size_t bytes;
	};
	std::deque<Bundle> bundles;
	explicit Journal(State& s)
		: state(s)
	{
		Open();
	}
	std::filesystem::path Path() { return state.directory / (state.run + L"-audio.log"); }
	void Open()
	{
		stream.open(Path(), std::ios::binary | std::ios::app);
		std::error_code ec;
		auto size = std::filesystem::file_size(Path(), ec);
		bytes = ec ? 0 : static_cast<size_t>(size);
		if (!stream) {
			++state.ioErrors;
			OutputDebugStringW(L"Aulay diagnostic audio journal open failed\n");
		}
	}
	void Write(uint64_t tick, std::wstring const& text)
	{
		auto line = Utf8(Stamp(tick) + L"seq=" + std::to_wstring(++sequence) + L" " + text + L"\r\n");
		if (bytes + line.size() > 32 * 1024 * 1024) {
			stream.close();
			std::error_code ec;
			for (int i = 3; i > 0; --i) {
				auto from = i == 1 ? Path() : state.directory / (state.run + L"-audio." + std::to_wstring(i - 1) + L".log");
				auto to = state.directory / (state.run + L"-audio." + std::to_wstring(i) + L".log");
				std::filesystem::remove(to, ec);
				ec.clear();
				if (std::filesystem::exists(from, ec))
					std::filesystem::rename(from, to, ec);
				if (ec)
					OutputDebugStringW(L"Aulay diagnostic audio journal rotation failed\n");
			}
			stream.clear();
			Open();
		}
		stream << line;
		bytes += line.size();
		if (!stream) {
			++state.ioErrors;
			OutputDebugStringW(L"Aulay diagnostic audio journal write failed\n");
		}
		auto now = GetTickCount64();
		history.emplace_back(now, line);
		historyBytes += line.size();
		while (!history.empty() && (now - history.front().first > 120000 || historyBytes > 8 * 1024 * 1024)) {
			historyBytes -= history.front().second.size();
			history.pop_front();
		}
		for (auto i = bundles.begin(); i != bundles.end();) {
			if (now > i->until || i->bytes + line.size() > 16 * 1024 * 1024) {
				i->file << "bundle-end reason=" << (now > i->until ? "post-window-complete" : "size-limit") << "\r\n";
				i = bundles.erase(i);
			} else {
				i->file << line;
				i->bytes += line.size();
				++i;
			}
		}
	}
	void Mark(uint64_t tick, std::wstring const& trace)
	{
		// Each run retains 20 explicitly named marker bundles. Never touch other runs.
		auto id = ++nextMarker;
		if (bundles.size() >= 20) {
			bundles.front().file << "bundle-end reason=concurrent-limit\r\n";
			bundles.pop_front();
		}
		if (id > 20) {
			std::error_code ec;
			std::filesystem::remove(state.directory / (state.run + L"-silent-" + std::to_wstring(id - 20) + L".log"), ec);
			if (ec) {
				++state.ioErrors;
				Write(tick, L"bundle-retention-delete-error=" + std::to_wstring(ec.value()));
			}
		}
		auto path = state.directory / (state.run + L"-silent-" + std::to_wstring(id) + L".log");
		Bundle b { std::ofstream(path, std::ios::binary), GetTickCount64() + 30000, 0 };
		b.file << Utf8(L"bundle-start pre-window-ms=120000 pre-limit-bytes=8388608 post-window-ms=30000 total-limit-bytes=16777216 marker-event-tick-ms=" + std::to_wstring(tick) + L" bundle-created-tick-ms=" + std::to_wstring(GetTickCount64()) + L" post-until-tick-ms=" + std::to_wstring(b.until) + L" available-history-ms=" + std::to_wstring(history.empty() ? 0 : GetTickCount64() - history.front().first) + L" " + trace + L"\r\n");
		for (auto const& entry : history) {
			b.file << entry.second;
			b.bytes += entry.second.size();
		}
		b.file.flush();
		bool ok = static_cast<bool>(b.file);
		if (ok)
			bundles.push_back(std::move(b));
		else
			++state.ioErrors;
		Write(tick, L"marker-bundle file=" + path.filename().wstring() + L" result=" + (ok ? L"created" : L"write-failed"));
	}
	void Flush()
	{
		stream.flush();
		if (!stream)
			++state.ioErrors;
		for (auto& b : bundles) {
			b.file.flush();
			if (!b.file)
				++state.ioErrors;
		}
	}
	void Finish()
	{
		Write(GetTickCount64(), L"monitor-stop queue-drained=true post-windows-ended-by-shutdown=true");
		for (auto& b : bundles)
			b.file << "bundle-end reason=shutdown-post-window-truncated\r\n";
		bundles.clear();
		Flush();
	}
};

struct DeviceEvents : winrt::implements<DeviceEvents, IMMNotificationClient> {
	std::shared_ptr<State> state;
	explicit DeviceEvents(std::shared_ptr<State> s)
		: state(std::move(s))
	{
	}
	HRESULT Report(LPCWSTR id, std::wstring text) noexcept
	{
		try {
			auto token = DebugAudioToken(id ? id : L"none", state->salt);
			state->Push({ GetTickCount64(), L"endpoint-event endpoint=" + token + L" " + text, false, false, {}, token }, true);
		} catch (...) {
			++state->dropped;
		}
		return S_OK;
	}
	HRESULT __stdcall OnDeviceStateChanged(LPCWSTR id, DWORD v) noexcept override { return Report(id, L"state=" + std::to_wstring(v)); }
	HRESULT __stdcall OnDeviceAdded(LPCWSTR id) noexcept override { return Report(id, L"added"); }
	HRESULT __stdcall OnDeviceRemoved(LPCWSTR id) noexcept override { return Report(id, L"removed"); }
	HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) noexcept override
	{
		return Report(id, L"default-changed flow=" + std::to_wstring(flow) + L" role=" + std::to_wstring(role));
	}
	HRESULT __stdcall OnPropertyValueChanged(LPCWSTR id, PROPERTYKEY key) noexcept override
	{
		return Report(id, L"property-changed pid=" + std::to_wstring(key.pid));
	}
};
struct SessionEvents : winrt::implements<SessionEvents, IAudioSessionEvents> {
	std::shared_ptr<State> state;
	std::wstring tag;
	SessionEvents(std::shared_ptr<State> s, std::wstring t)
		: state(std::move(s))
		, tag(std::move(t))
	{
	}
	HRESULT Report(std::wstring text) noexcept
	{
		try {
			state->Push({ GetTickCount64(), L"session-event " + tag + L" " + text }, true);
		} catch (...) {
			++state->dropped;
		}
		return S_OK;
	}
	HRESULT __stdcall OnDisplayNameChanged(LPCWSTR, LPCGUID) noexcept override { return Report(L"display-name-changed"); }
	HRESULT __stdcall OnIconPathChanged(LPCWSTR, LPCGUID) noexcept override { return Report(L"icon-changed"); }
	HRESULT __stdcall OnSimpleVolumeChanged(float v, BOOL m, LPCGUID) noexcept override { return Report(L"volume=" + std::to_wstring(v) + L" mute=" + std::to_wstring(m)); }
	HRESULT __stdcall OnChannelVolumeChanged(DWORD count, float values[], DWORD changed, LPCGUID) noexcept override
	{
		try {
			auto text = L"channel-volume-changed channels=" + std::to_wstring(count) + L" channel=" + std::to_wstring(changed);
			if (values)
				for (DWORD i = 0; i < count && i < 64; ++i)
					text += L" ch" + std::to_wstring(i) + L"=" + std::to_wstring(values[i]);
			return Report(text);
		} catch (...) {
			++state->dropped;
			return S_OK;
		}
	}
	HRESULT __stdcall OnGroupingParamChanged(LPCGUID, LPCGUID) noexcept override { return Report(L"grouping-changed"); }
	HRESULT __stdcall OnStateChanged(AudioSessionState v) noexcept override { return Report(L"state=" + std::to_wstring(v)); }
	HRESULT __stdcall OnSessionDisconnected(AudioSessionDisconnectReason v) noexcept override { return Report(L"disconnected reason=" + std::to_wstring(v)); }
};
struct VolumeEvents : winrt::implements<VolumeEvents, IAudioEndpointVolumeCallback> {
	std::shared_ptr<State> state;
	std::wstring endpoint;
	VolumeEvents(std::shared_ptr<State> s, std::wstring e)
		: state(std::move(s))
		, endpoint(std::move(e))
	{
	}
	HRESULT __stdcall OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) noexcept override
	{
		try {
			if (!data)
				return E_POINTER;
			auto text = L"endpoint-volume-event endpoint=" + endpoint + L" volume=" + std::to_wstring(data->fMasterVolume) + L" mute=" + std::to_wstring(data->bMuted) + L" channels=" + std::to_wstring(data->nChannels);
			for (UINT i = 0; i < data->nChannels && i < 64; ++i)
				text += L" ch" + std::to_wstring(i) + L"=" + std::to_wstring(data->afChannelVolumes[i]);
			state->Push({ GetTickCount64(), text }, true);
		} catch (...) {
			++state->dropped;
		}
		return S_OK;
	}
};
struct NewSessionEvents : winrt::implements<NewSessionEvents, IAudioSessionNotification> {
	std::shared_ptr<State> state;
	std::wstring endpoint;
	NewSessionEvents(std::shared_ptr<State> s, std::wstring e)
		: state(std::move(s))
		, endpoint(std::move(e))
	{
	}
	HRESULT __stdcall OnSessionCreated(IAudioSessionControl* session) noexcept override
	{
		try {
			Event e;
			e.text = L"session-created endpoint=" + endpoint;
			e.endpoint = endpoint;
			e.session.copy_from(session);
			state->Push(std::move(e), true);
		} catch (...) {
			++state->dropped;
		}
		return S_OK;
	}
};
struct Peak {
	float maximum = 0;
	uint32_t samples = 0, nonzero = 0, signal = 0, errors = 0, consecutiveErrors = 0;
	HRESULT lastError = S_OK;
	void Read(IAudioMeterInformation* meter)
	{
		float v = 0;
		auto hr = meter ? meter->GetPeakValue(&v) : E_NOINTERFACE;
		if (FAILED(hr)) {
			++errors;
			++consecutiveErrors;
			lastError = hr;
			return;
		}
		consecutiveErrors = 0;
		++samples;
		if (v > 0)
			++nonzero;
		if (v >= 0.00001f)
			++signal;
		maximum = (std::max)(maximum, v);
	}
	std::wstring Take()
	{
		wchar_t value[40] {};
		swprintf_s(value, L"%.9g", static_cast<double>(maximum));
		auto text = L" peak-max=" + std::wstring(value) + L" signal-samples=" + std::to_wstring(signal) + L" samples=" + std::to_wstring(samples) + L" nonzero-samples=" + std::to_wstring(nonzero) + L" meter-errors=" + std::to_wstring(errors) + L" meter-last-hr=" + DebugAudioHresult(lastError);
		auto consecutive = consecutiveErrors;
		*this = {};
		consecutiveErrors = consecutive;
		return text;
	}
};
struct Session {
	winrt::com_ptr<IAudioSessionControl> control;
	winrt::com_ptr<SessionEvents> events;
	winrt::com_ptr<IAudioMeterInformation> meter;
	winrt::com_ptr<ISimpleAudioVolume> volume;
	std::wstring tag;
	Peak peak;
	~Session()
	{
		if (events)
			control->UnregisterAudioSessionNotification(events.get());
	}
};
struct Endpoint {
	winrt::com_ptr<IMMDevice> device;
	winrt::com_ptr<IAudioSessionManager2> manager;
	winrt::com_ptr<NewSessionEvents> events;
	winrt::com_ptr<IAudioMeterInformation> meter;
	winrt::com_ptr<IAudioEndpointVolume> volume;
	Peak peak;
	winrt::com_ptr<VolumeEvents> volumeEvents;
	~Endpoint()
	{
		if (events)
			manager->UnregisterSessionNotification(events.get());
		if (volumeEvents)
			volume->UnregisterControlChangeNotify(volumeEvents.get());
	}
};
struct Observer {
	std::shared_ptr<State> state;
	Journal& journal;
	winrt::com_ptr<IMMDeviceEnumerator> enumerator;
	winrt::com_ptr<DeviceEvents> notifications;
	std::unordered_map<std::wstring, std::unique_ptr<Endpoint>> endpoints;
	std::unordered_map<std::wstring, std::unique_ptr<Session>> sessions;
	Observer(std::shared_ptr<State> s, Journal& j)
		: state(std::move(s))
		, journal(j)
	{
	}
	void Log(std::wstring const& s) { journal.Write(GetTickCount64(), s); }
	void AddSession(IAudioSessionControl* raw, std::wstring const& endpoint)
	{
		auto control = winrt::com_ptr<IAudioSessionControl>();
		control.copy_from(raw);
		auto extended = control.try_as<IAudioSessionControl2>();
		if (!extended) {
			Log(L"session-control2-unavailable endpoint=" + endpoint);
			return;
		}
		wil::unique_cotaskmem_string id;
		auto hr = extended->GetSessionInstanceIdentifier(id.put());
		if (FAILED(hr) || !id) {
			Log(L"session-id-error hr=" + DebugAudioHresult(hr));
			return;
		}
		auto token = DebugAudioToken(id.get(), state->salt);
		auto key = endpoint + L":" + token;
		if (sessions.count(key))
			return;
		if (sessions.size() >= 1024) {
			Log(L"coverage session-limit=1024 reached=true");
			return;
		}
		auto s = std::make_unique<Session>();
		s->control = control;
		DWORD pid = 0;
		hr = extended->GetProcessId(&pid);
		s->tag = L"endpoint=" + endpoint + L" session-token=" + token + L" pid=" + std::to_wstring(pid) + L" pid-hr=" + DebugAudioHresult(hr);
		s->meter = control.try_as<IAudioMeterInformation>();
		s->volume = control.try_as<ISimpleAudioVolume>();
		s->events = winrt::make_self<SessionEvents>(state, s->tag);
		hr = control->RegisterAudioSessionNotification(s->events.get());
		Log(L"session-observed " + s->tag + L" events-hr=" + DebugAudioHresult(hr) + L" meter-interface=" + (s->meter ? L"available" : L"unavailable") + L" volume-interface=" + (s->volume ? L"available" : L"unavailable"));
		if (FAILED(hr))
			s->events = nullptr;
		wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
		if (process) {
			wchar_t path[32768];
			DWORD size = 32768;
			FILETIME created {}, exited {}, kernel {}, user {};
			auto gotPath = QueryFullProcessImageNameW(process.get(), 0, path, &size);
			auto pathError = gotPath ? 0 : GetLastError();
			auto gotTime = GetProcessTimes(process.get(), &created, &exited, &kernel, &user);
			Log(L"process pid=" + std::to_wstring(pid) + L" image=" + (gotPath ? std::filesystem::path(path).filename().wstring() : L"unavailable") + L" image-error=" + std::to_wstring(pathError) + L" creation-filetime=" + std::to_wstring((uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime) + L" time-available=" + std::to_wstring(gotTime));
		} else
			Log(L"process pid=" + std::to_wstring(pid) + L" query-error=" + std::to_wstring(GetLastError()));
		sessions.emplace(key, std::move(s));
	}
	void Invalidate(std::wstring const& token)
	{
		for (auto i = sessions.begin(); i != sessions.end();)
			if (i->first.starts_with(token + L":")) {
				Log(L"session-retired reason=endpoint-invalidated " + i->second->tag);
				i = sessions.erase(i);
			} else
				++i;
		if (endpoints.erase(token))
			Log(L"endpoint-invalidated endpoint=" + token);
	}
	void Refresh()
	{
		if (!enumerator) {
			winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
			notifications = winrt::make_self<DeviceEvents>(state);
			auto hr = enumerator->RegisterEndpointNotificationCallback(notifications.get());
			Log(L"endpoint-notifications hr=" + DebugAudioHresult(hr));
			if (FAILED(hr))
				notifications = nullptr;
		}
		for (auto role : { eConsole, eMultimedia, eCommunications }) {
			winrt::com_ptr<IMMDevice> e;
			auto hr = enumerator->GetDefaultAudioEndpoint(eRender, role, e.put());
			Log(L"default-output role=" + std::to_wstring(role) + L" hr=" + DebugAudioHresult(hr) + (SUCCEEDED(hr) ? L" endpoint=" + DebugEndpointToken(e.get(), state->salt) : L""));
		}
		winrt::com_ptr<IMMDeviceCollection> list;
		winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, list.put()));
		UINT n = 0;
		winrt::check_hresult(list->GetCount(&n));
		Log(L"inventory outputs=" + std::to_wstring(n) + L" endpoint-limit=64 session-limit=1024");
		std::unordered_map<std::wstring, bool> active;
		for (UINT i = 0; i < n && i < 64; ++i) {
			std::wstring token;
			try {
				winrt::com_ptr<IMMDevice> device;
				winrt::check_hresult(list->Item(i, device.put()));
				token = DebugEndpointToken(device.get(), state->salt);
				DWORD status = 0;
				winrt::check_hresult(device->GetState(&status));
				Log(L"inventory endpoint=" + token + L" state=" + std::to_wstring(status));
				if (status != DEVICE_STATE_ACTIVE)
					continue;
				active[token] = true;
				if (endpoints.count(token)) {
					auto const& previous = *endpoints.at(token);
					if (!previous.manager || !previous.meter || !previous.volume || !previous.events || !previous.volumeEvents || previous.peak.consecutiveErrors >= 3)
						Invalidate(token);
				}
				if (!endpoints.count(token)) {
					auto e = std::make_unique<Endpoint>();
					e->device = device;
					auto hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, e->meter.put_void());
					Log(L"endpoint=" + token + L" meter-hr=" + DebugAudioHresult(hr));
					hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, e->volume.put_void());
					Log(L"endpoint=" + token + L" volume-hr=" + DebugAudioHresult(hr));
					if (e->volume) {
						e->volumeEvents = winrt::make_self<VolumeEvents>(state, token);
						hr = e->volume->RegisterControlChangeNotify(e->volumeEvents.get());
						Log(L"endpoint=" + token + L" volume-events-hr=" + DebugAudioHresult(hr));
						if (FAILED(hr))
							e->volumeEvents = nullptr;
					}
					winrt::com_ptr<IAudioClient> client;
					hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
					if (SUCCEEDED(hr)) {
						WAVEFORMATEX* rawFormat = nullptr;
						hr = client->GetMixFormat(&rawFormat);
						wil::unique_cotaskmem_ptr<WAVEFORMATEX> format(rawFormat);
						Log(L"endpoint=" + token + L" mix-format-hr=" + DebugAudioHresult(hr) + (SUCCEEDED(hr) ? L" rate=" + std::to_wstring(format->nSamplesPerSec) + L" channels=" + std::to_wstring(format->nChannels) + L" bits=" + std::to_wstring(format->wBitsPerSample) + L" format-tag=" + std::to_wstring(format->wFormatTag) + L" block-align=" + std::to_wstring(format->nBlockAlign) : L""));
					} else
						Log(L"endpoint=" + token + L" audio-client-hr=" + DebugAudioHresult(hr));
					hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, e->manager.put_void());
					Log(L"endpoint=" + token + L" manager-hr=" + DebugAudioHresult(hr));
					if (SUCCEEDED(hr)) {
						e->events = winrt::make_self<NewSessionEvents>(state, token);
						hr = e->manager->RegisterSessionNotification(e->events.get());
						Log(L"endpoint=" + token + L" new-session-notifications-hr=" + DebugAudioHresult(hr));
						if (FAILED(hr))
							e->events = nullptr;
					}
					endpoints.emplace(token, std::move(e));
				}
				auto& e = *endpoints.at(token);
				if (e.manager) {
					winrt::com_ptr<IAudioSessionEnumerator> found;
					winrt::check_hresult(e.manager->GetSessionEnumerator(found.put()));
					int count = 0;
					winrt::check_hresult(found->GetCount(&count)); // arms new-session notifications
					Log(L"endpoint=" + token + L" enumerated-sessions=" + std::to_wstring(count));
					for (int j = 0; j < count && j < 1024; ++j) {
						winrt::com_ptr<IAudioSessionControl> s;
						auto hr = found->GetSession(j, s.put());
						if (SUCCEEDED(hr))
							AddSession(s.get(), token);
						else
							Log(L"session-enumeration-hr=" + DebugAudioHresult(hr));
					}
				}
			} catch (...) {
				Log(L"inventory-endpoint-error endpoint=" + token + L" hr=" + DebugAudioHresult(winrt::to_hresult()));
				if (!token.empty())
					Invalidate(token);
			}
		}
		for (auto i = endpoints.begin(); i != endpoints.end();)
			if (!active.count(i->first))
				i = endpoints.erase(i);
			else
				++i;
		for (auto i = sessions.begin(); i != sessions.end();) {
			AudioSessionState status {};
			auto hr = i->second->control->GetState(&status);
			if (FAILED(hr) || status == AudioSessionStateExpired || !active.count(i->first.substr(0, i->first.find(L':')))) {
				Log(L"session-retired " + i->second->tag + L" state=" + std::to_wstring(status) + L" hr=" + DebugAudioHresult(hr));
				i = sessions.erase(i);
			} else {
				if (!i->second->events) {
					auto events = winrt::make_self<SessionEvents>(state, i->second->tag);
					hr = i->second->control->RegisterAudioSessionNotification(events.get());
					Log(L"session-events-retry " + i->second->tag + L" hr=" + DebugAudioHresult(hr));
					if (SUCCEEDED(hr))
						i->second->events = std::move(events);
				}
				++i;
			}
		}
		SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
		if (!scm) {
			Log(L"service-manager-error=" + std::to_wstring(GetLastError()));
			return;
		}
		auto closeScm = wil::scope_exit([&] { CloseServiceHandle(scm); });
		for (auto name : { L"Audiosrv", L"AudioEndpointBuilder", L"bthserv" }) {
			SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
			if (!service) {
				Log(L"service=" + std::wstring(name) + L" open-error=" + std::to_wstring(GetLastError()));
				continue;
			}
			auto close = wil::scope_exit([&] { CloseServiceHandle(service); });
			SERVICE_STATUS_PROCESS status {};
			DWORD bytes = 0;
			auto ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes);
			Log(L"service=" + std::wstring(name) + L" query-error=" + std::to_wstring(ok ? 0 : GetLastError()) + L" state=" + std::to_wstring(status.dwCurrentState) + L" pid=" + std::to_wstring(status.dwProcessId));
		}
	}
	void Sample(bool report)
	{
		for (auto& item : endpoints) {
			auto& e = *item.second;
			e.peak.Read(e.meter.get());
			if (report)
				Log(L"output-window endpoint=" + item.first + e.peak.Take() + (e.volume ? L" volume=" + DebugAudioValue<float>([&](auto p) { return e.volume->GetMasterVolumeLevelScalar(p); }) + L" mute=" + DebugAudioValue<BOOL>([&](auto p) { return e.volume->GetMute(p); }) : L" volume=unavailable"));
		}
		for (auto& item : sessions) {
			auto& s = *item.second;
			s.peak.Read(s.meter.get());
			if (report)
				Log(L"session-window " + s.tag + s.peak.Take() + L" state=" + DebugAudioValue<AudioSessionState>([&](auto p) { return s.control->GetState(p); }) + (s.volume ? L" volume=" + DebugAudioValue<float>([&](auto p) { return s.volume->GetMasterVolume(p); }) + L" mute=" + DebugAudioValue<BOOL>([&](auto p) { return s.volume->GetMute(p); }) : L" volume=unavailable"));
		}
	}
	~Observer()
	{
		sessions.clear();
		endpoints.clear();
		if (notifications)
			enumerator->UnregisterEndpointNotificationCallback(notifications.get());
	}
};

void Run(std::shared_ptr<State> state) noexcept
{
	auto finish = wil::scope_exit([&] {state->finished=true;SetEvent(state->finishedSignal.get());state->wake.notify_all(); });
	try {
		Journal journal(*state);
		journal.Write(GetTickCount64(), L"monitor-start schema=2 sample-period-ms=500 report-period-ms=5000 inventory-period-ms=30000 detailed-sample-period-ms=50 detailed-report-period-ms=1000 detailed-inventory-period-ms=2000 detailed-duration-ms=30000 pre-window-ms=120000 post-window-ms=30000 audio-content=not-recorded signal-threshold=1e-5 interpretation=not-proof-of-audibility;unmarked-is-not-automatically-audible;phone-play-button-not-observable;transport-channel-not-observable");
		auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		auto apartment = wil::scope_exit([&] {if(SUCCEEDED(hr))CoUninitialize(); });
		journal.Write(GetTickCount64(), L"monitor-mta-hr=" + DebugAudioHresult(hr));
		winrt::check_hresult(hr);
		Observer observer(state, journal);
		aulay::diagnostics::Sampling sampling;
		bool wasDetailed = false;
		uint64_t inventory = 0, report = 0, sample = 0, lastFlush = 0, lastSample = 0, maxSampleGap = 0, lastReport = 0;
		std::wstring lastAction = L"startup";
		for (;;) {
			std::deque<Event> events;
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				if (state->queue.empty() && !state->stopping) {
					auto now = GetTickCount64();
					auto due = (std::min)({ sample, inventory, lastFlush + 1000 });
					if (sampling.Detailed(now))
						due = (std::min)(due, sampling.detailedUntil);
					if (due > now)
						state->wake.wait_for(lock, std::chrono::milliseconds(due - now), [&] { return state->stopping || !state->queue.empty(); });
				}
				events.swap(state->queue);
			}
			bool refresh = false;
			for (auto& e : events) {
				if (e.marker || e.snapshot || e.text.find(L"disconnect=unexpected") != std::wstring::npos || e.text.find(L"silent-recovery begin") != std::wstring::npos) {
					sampling.Boost(e.tick); // expiration anchored to event, not delayed queue processing
					sample = report = inventory = 0;
				}
				journal.Write(e.tick, e.text + L" queue-lag-ms=" + std::to_wstring(GetTickCount64() - e.tick));
				if (e.marker)
					journal.Mark(e.tick, e.text);
				if (e.session) {
					try {
						observer.AddSession(e.session.get(), e.endpoint);
					} catch (...) {
						journal.Write(e.tick, L"new-session-error hr=" + DebugAudioHresult(winrt::to_hresult()));
					}
				}
				if (e.text.find(L"endpoint-event") == 0) {
					refresh = true;
					if (e.text.find(L" state=") != std::wstring::npos || e.text.find(L" removed") != std::wstring::npos)
						observer.Invalidate(e.endpoint);
				}
				if (e.snapshot) {
					lastAction = e.text;
					try {
						for (auto const& line : CollectDebugAudioSnapshot(state->salt))
							journal.Write(GetTickCount64(), e.text + L" " + line);
					} catch (...) {
						journal.Write(e.tick, L"snapshot-error hr=" + DebugAudioHresult(winrt::to_hresult()));
					}
					--state->pendingSnapshots;
				}
			}
			auto dropped = state->dropped.exchange(0);
			if (dropped)
				journal.Write(GetTickCount64(), L"coverage queue-dropped=" + std::to_wstring(dropped));
			if (state->stopping) {
				bool empty = false;
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					empty = state->queue.empty();
				}
				if (empty) {
					journal.Finish();
					break;
				}
				continue; // Include requests queued between the earlier swap and Stop.
			}
			auto now = GetTickCount64();
			auto started = now;
			bool detailed = sampling.Detailed(now);
			if (detailed != wasDetailed) {
				journal.Write(now, L"sampling-mode=" + std::wstring(detailed ? L"detailed" : L"normal") + L" detailed-until-tick-ms=" + std::to_wstring(sampling.detailedUntil));
				wasDetailed = detailed;
				sample = report = inventory = 0;
			}
			try {
				if (now >= inventory || refresh) {
					observer.Refresh();
					inventory = GetTickCount64() + sampling.InventoryMs(GetTickCount64());
				}
				if (now >= sample) {
					bool emit = now >= report;
					if (lastSample)
						maxSampleGap = (std::max)(maxSampleGap, now - lastSample);
					lastSample = now;
					observer.Sample(emit);
					auto period = sampling.SampleMs(now);
					sample = sample && now - sample < period ? sample + period : GetTickCount64() + period;
					if (emit) {
						journal.Write(now, L"heartbeat report-gap-ms=" + std::to_wstring(report ? now - lastReport : 0) + L" max-sample-gap-ms=" + std::to_wstring(maxSampleGap) + L" worker-pass-ms=" + std::to_wstring(GetTickCount64() - started) + L" last-action={" + lastAction + L"}");
						lastReport = now;
						report = now + sampling.ReportMs(now);
						maxSampleGap = 0;
					}
				}
			} catch (...) {
				journal.Write(now, L"monitor-pass-error hr=" + DebugAudioHresult(winrt::to_hresult()));
				inventory = now + sampling.InventoryMs(now);
				sample = now + sampling.SampleMs(now);
			}
			if (now - lastFlush >= 1000) {
				journal.Flush();
				lastFlush = now;
			}
		}
	} catch (...) {
		state->fatal = true;
		try {
			std::ofstream failure(state->directory / (state->run + L"-failure.log"), std::ios::binary | std::ios::app);
			failure << Utf8(Stamp(GetTickCount64()) + L"monitor-fatal hr=" + DebugAudioHresult(winrt::to_hresult()) + L" pending-snapshots=" + std::to_wstring(state->pendingSnapshots.load()) + L"\r\n");
		} catch (...) {
		}
		OutputDebugStringW(L"Aulay audio monitor terminated unexpectedly\n");
	}
}
} // namespace DebugAudio

void StartDebugAudioMonitor(std::filesystem::path const& directory, uint64_t salt)
{
	if (directory.empty())
		return;
	auto s = std::make_shared<DebugAudio::State>();
	s->directory = directory;
	s->salt = salt;
	s->run = L"Aulay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
	DebugAudio::monitor.store(s);
	try {
		std::thread([s] { DebugAudio::Run(s); }).detach();
	} catch (...) {
		s->fatal = true;
		s->finished = true;
		SetEvent(s->finishedSignal.get());
		OutputDebugStringW(L"Aulay audio monitor thread creation failed\n");
	}
}
void EnqueueDebugAudioEvent(std::wstring const& text, bool snapshot = false, bool marker = false)
{
	if (auto s = DebugAudio::monitor.load())
		s->Push({ GetTickCount64(), text, snapshot, marker });
}
std::wstring DebugAudioMonitorStatus()
{
	auto s = DebugAudio::monitor.load();
	return !s ? L"not-started" : L"finished=" + std::to_wstring(s->finished.load()) + L" io-errors=" + std::to_wstring(s->ioErrors.load()) + L" fatal=" + std::to_wstring(s->fatal.load()) + L" pending-snapshots=" + std::to_wstring(s->pendingSnapshots.load());
}
uint32_t PendingDebugAudioSnapshots()
{
	auto s = DebugAudio::monitor.load();
	return s ? s->pendingSnapshots.load() : 0;
}
void StopDebugAudioMonitor()
{
	if (auto s = DebugAudio::monitor.load()) {
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			s->stopping = true;
		}
		s->wake.notify_all();
	}
}
bool DebugAudioMonitorFinished()
{
	auto s = DebugAudio::monitor.load();
	return !s || s->finished.load();
}

winrt::Windows::Foundation::IAsyncAction WaitDebugAudioMonitorFinished()
{
	if (auto s = DebugAudio::monitor.load())
		co_await winrt::resume_on_signal(s->finishedSignal.get(), std::chrono::seconds(5));
}
