#pragma once

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wil/resource.h>

// These helpers own only local COM objects and scalar copies. They never touch
// app/UI globals, so a slow audio driver cannot hold up connect or app shutdown.
std::wstring DebugAudioHresult(HRESULT value)
{
	wchar_t text[16] {};
	swprintf_s(text, L"0x%08X", static_cast<unsigned>(value));
	return text;
}

std::wstring DebugAudioToken(std::wstring_view value, uint64_t salt)
{
	uint64_t hash = 14695981039346656037ull ^ salt;
	for (auto c : value) {
		hash ^= static_cast<uint16_t>(c);
		hash *= 1099511628211ull;
	}
	wchar_t text[20] {};
	swprintf_s(text, L"%016llX", static_cast<unsigned long long>(hash));
	return text;
}

template <typename T, typename Reader>
std::wstring DebugAudioValue(Reader reader)
{
	T value {};
	auto hr = reader(&value);
	return SUCCEEDED(hr) ? std::to_wstring(value) : L"unavailable:" + DebugAudioHresult(hr);
}

std::wstring DebugEndpointToken(IMMDevice* endpoint, uint64_t salt)
{
	wil::unique_cotaskmem_string id;
	winrt::check_hresult(endpoint->GetId(id.put()));
	return DebugAudioToken(id.get(), salt);
}

std::vector<std::wstring> CollectDebugAudioSnapshot(uint64_t salt)
{
	std::vector<std::wstring> lines;
	auto started = GetTickCount64();
	lines.push_back(L"capture-start tick-ms=" + std::to_wstring(started) + L" scope=system-render-observation audio-content=not-recorded");
	auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	auto uninitialize = wil::scope_exit([apartment] { if (SUCCEEDED(apartment)) CoUninitialize(); });
	try {
		winrt::check_hresult(apartment);
		winrt::com_ptr<IMMDeviceEnumerator> enumerator;
		winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
			__uuidof(IMMDeviceEnumerator), enumerator.put_void()));
		for (auto role : { eConsole, eMultimedia, eCommunications }) {
			winrt::com_ptr<IMMDevice> endpoint;
			auto hr = enumerator->GetDefaultAudioEndpoint(eRender, role, endpoint.put());
			lines.push_back(L"default-output role=" + std::to_wstring(static_cast<int>(role)) + L" hr=" + DebugAudioHresult(hr) + (SUCCEEDED(hr) ? L" endpoint=" + DebugEndpointToken(endpoint.get(), salt) : L""));
		}
		winrt::com_ptr<IMMDeviceCollection> endpoints;
		winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, endpoints.put()));
		UINT count = 0;
		winrt::check_hresult(endpoints->GetCount(&count));
		lines.push_back(L"active-output-count=" + std::to_wstring(count) + L" endpoint-limit=16 session-limit=64");
		for (UINT i = 0; i < count && i < 16; ++i) {
			try {
				winrt::com_ptr<IMMDevice> endpoint;
				winrt::check_hresult(endpoints->Item(i, endpoint.put()));
				auto prefix = L"endpoint=" + DebugEndpointToken(endpoint.get(), salt);
				lines.push_back(prefix + L" state=" + DebugAudioValue<DWORD>([&](auto p) { return endpoint->GetState(p); }));
				winrt::com_ptr<IAudioEndpointVolume> volume;
				auto hr = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, volume.put_void());
				lines.push_back(prefix + L" volume-hr=" + DebugAudioHresult(hr) + (SUCCEEDED(hr) ? L" volume=" + DebugAudioValue<float>([&](auto p) { return volume->GetMasterVolumeLevelScalar(p); }) + L" mute=" + DebugAudioValue<BOOL>([&](auto p) { return volume->GetMute(p); }) : L""));
				winrt::com_ptr<IAudioMeterInformation> meter;
				hr = endpoint->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, meter.put_void());
				lines.push_back(prefix + L" meter-hr=" + DebugAudioHresult(hr) + (SUCCEEDED(hr) ? L" peak=" + DebugAudioValue<float>([&](auto p) { return meter->GetPeakValue(p); }) : L""));
				winrt::com_ptr<IAudioSessionManager2> manager;
				hr = endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void());
				if (FAILED(hr)) {
					lines.push_back(prefix + L" sessions-hr=" + DebugAudioHresult(hr));
					continue;
				}
				winrt::com_ptr<IAudioSessionEnumerator> sessions;
				winrt::check_hresult(manager->GetSessionEnumerator(sessions.put()));
				int sessionCount = 0;
				winrt::check_hresult(sessions->GetCount(&sessionCount));
				lines.push_back(prefix + L" observed-session-count=" + std::to_wstring(sessionCount));
				for (int j = 0; j < sessionCount && j < 64; ++j) {
					winrt::com_ptr<IAudioSessionControl> session;
					hr = sessions->GetSession(j, session.put());
					if (FAILED(hr)) {
						lines.push_back(prefix + L" session-read-hr=" + DebugAudioHresult(hr));
						continue;
					}
					auto entry = prefix + L" session-index=" + std::to_wstring(j) + L" state=" + DebugAudioValue<AudioSessionState>([&](auto p) { return session->GetState(p); });
					if (auto control = session.try_as<IAudioSessionControl2>()) {
						DWORD pid = 0;
						hr = control->GetProcessId(&pid);
						entry += L" pid=" + std::to_wstring(pid) + L" pid-hr=" + DebugAudioHresult(hr);
						wil::unique_cotaskmem_string instance;
						if (SUCCEEDED(control->GetSessionInstanceIdentifier(instance.put())) && instance)
							entry += L" session-token=" + DebugAudioToken(instance.get(), salt);
					}
					if (auto simple = session.try_as<ISimpleAudioVolume>())
						entry += L" volume=" + DebugAudioValue<float>([&](auto p) { return simple->GetMasterVolume(p); }) + L" mute=" + DebugAudioValue<BOOL>([&](auto p) { return simple->GetMute(p); });
					if (auto sessionMeter = session.try_as<IAudioMeterInformation>())
						entry += L" peak=" + DebugAudioValue<float>([&](auto p) { return sessionMeter->GetPeakValue(p); });
					lines.push_back(std::move(entry));
				}
			} catch (...) {
				lines.push_back(L"endpoint-read-error hr=" + DebugAudioHresult(winrt::to_hresult()));
			}
		}
	} catch (...) {
		lines.push_back(L"capture-error hr=" + DebugAudioHresult(winrt::to_hresult()));
	}
	lines.push_back(L"capture-end duration-ms=" + std::to_wstring(GetTickCount64() - started) + L" interpretation=instantaneous-observation-not-proof-of-audibility;session-enumeration-may-be-incomplete");
	return lines;
}
