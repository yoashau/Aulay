// Native diagnostics tests. No Bluetooth connection/radio/default-device change.
// The optional --audio integration creates its OWN silent WASAPI render session.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.h>
#include "../DebugAudioMonitor.hpp"

struct FakeMeter : winrt::implements<FakeMeter, IAudioMeterInformation> {
	float value = 0.5f;
	HRESULT hr = S_OK;
	HRESULT __stdcall GetPeakValue(float* out) noexcept override
	{
		*out = value;
		return hr;
	}
	HRESULT __stdcall GetMeteringChannelCount(UINT* out) noexcept override
	{
		*out = 1;
		return S_OK;
	}
	HRESULT __stdcall GetChannelsPeakValues(UINT, float* out) noexcept override
	{
		*out = value;
		return hr;
	}
	HRESULT __stdcall QueryHardwareSupport(DWORD* out) noexcept override
	{
		*out = 0;
		return S_OK;
	}
};
std::string Read(std::filesystem::path const& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(f), {});
}
int main(int argc, char** argv)
{
	int checks = 0, failed = 0;
	auto check = [&](bool ok, const char* label) {++checks;if(!ok){++failed;std::cout<<"FAIL "<<label<<'\n';} };
	auto dir = std::filesystem::temp_directory_path() / (L"Aulay-monitor-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	std::filesystem::create_directories(dir);
	auto cleanup = wil::scope_exit([&] {if(!failed){std::error_code ec;std::filesystem::remove_all(dir,ec);} });
	try {
		auto s = std::make_shared<DebugAudio::State>();
		s->directory = dir;
		s->run = L"fixture";
		s->salt = 42;
		for (int i = 0; i < 4097; ++i)
			s->Push({ GetTickCount64(), L"load" });
		check(s->queue.size() == 4096 && s->dropped == 1, "bounded ordinary queue reports overflow");
		s->Push({ GetTickCount64(), L"marker=1", true, true });
		check(s->queue.size() == 4097 && s->pendingSnapshots == 1, "explicit marker preserved when ordinary queue full");
		s->queue.clear();
		s->pendingSnapshots = 0;
		s->mutex.lock();
		std::thread callback([&] { s->Push({ GetTickCount64(), L"callback" }, true); });
		callback.join();
		s->mutex.unlock();
		check(s->dropped == 2 && s->queue.empty(), "callback does not block on queue lock; loss counted");
		{
			DebugAudio::Journal journal(*s);
			journal.Write(GetTickCount64(), L"PREVIOUS_DISCONNECT attempt=7");
			journal.Write(GetTickCount64(), L"OPENED attempt=8");
			journal.Mark(GetTickCount64(), L"USER_NO_SOUND marker=1 attempt=8");
			journal.Write(GetTickCount64(), L"POST_MARKER_PLAYING_SAMPLE");
			journal.Flush();
			auto body = Read(dir / L"fixture-silent-1.log");
			check(body.find("PREVIOUS_DISCONNECT") != std::string::npos && body.find("OPENED") != std::string::npos, "marker bundle preserves prehistory");
			check(body.find("USER_NO_SOUND marker=1 attempt=8") != std::string::npos && body.find("POST_MARKER_PLAYING_SAMPLE") != std::string::npos, "bundle marker identity and posthistory");
			journal.bundles.front().until = 0;
			journal.Write(GetTickCount64(), L"after-window");
			check(journal.bundles.empty() && Read(dir / L"fixture-silent-1.log").find("post-window-complete") != std::string::npos, "completed postwindow closes bundle");
			journal.Mark(GetTickCount64(), L"USER_NO_SOUND marker=2");
			journal.bundles.back().bytes = 16 * 1024 * 1024;
			journal.Write(GetTickCount64(), L"limit");
			check(journal.bundles.empty() && Read(dir / L"fixture-silent-2.log").find("size-limit") != std::string::npos, "bundle size bound is explicit");
			journal.bytes = 32 * 1024 * 1024;
			journal.Write(GetTickCount64(), L"ROTATED");
			journal.Flush();
			check(std::filesystem::exists(dir / L"fixture-audio.1.log") && Read(dir / L"fixture-audio.log").find("ROTATED") != std::string::npos, "timeline rotation preserves old log");
			journal.Mark(GetTickCount64(), L"USER_NO_SOUND marker=3");
			journal.Finish();
			check(Read(dir / L"fixture-silent-3.log").find("shutdown-post-window-truncated") != std::string::npos, "shutdown labels shortened postwindow");
		}
		{
			DebugAudio::State history;
			history.directory = dir;
			history.run = L"history";
			DebugAudio::Journal journal(history);
			journal.history.emplace_back(GetTickCount64() - 120001, "EXPIRED");
			journal.historyBytes = 7;
			journal.Write(GetTickCount64(), L"CURRENT");
			check(journal.history.size() == 1 && journal.history.front().second.find("CURRENT") != std::string::npos, "prehistory time window bounded");
			journal.history.emplace_back(GetTickCount64(), std::string(8 * 1024 * 1024, 'x'));
			journal.historyBytes += 8 * 1024 * 1024;
			journal.Write(GetTickCount64(), L"AFTER_SIZE_LIMIT");
			check(journal.historyBytes <= 8 * 1024 * 1024, "prehistory memory bounded");
			std::ofstream(dir / L"unrelated-silent-1.log") << "sentinel";
			for (int i = 0; i < 21; ++i)
				journal.Mark(GetTickCount64(), L"USER_NO_SOUND retention-test");
			check(journal.bundles.size() == 20 && !std::filesystem::exists(dir / L"history-silent-1.log") && std::filesystem::exists(dir / L"history-silent-21.log"), "retention closes old handle before delete on Windows");
			check(Read(dir / L"unrelated-silent-1.log") == "sentinel", "retention never removes another run");
			journal.Finish();
		}
		{
			DebugAudio::State bad;
			bad.directory = dir / L"missing-directory";
			bad.run = L"bad";
			DebugAudio::Journal journal(bad);
			journal.Write(GetTickCount64(), L"write failure");
			journal.Mark(GetTickCount64(), L"marker failure");
			journal.Flush();
			check(bad.ioErrors >= 3, "file open/write/marker errors counted rather than reported as saved");
		}
		{
			auto sessionEvents = winrt::make_self<DebugAudio::SessionEvents>(s, L"fixture-session");
			sessionEvents->OnStateChanged(AudioSessionStateActive);
			sessionEvents->OnSimpleVolumeChanged(0.25f, TRUE, nullptr);
			sessionEvents->OnSessionDisconnected(DisconnectReasonServerShutdown);
			check(s->queue.size() == 3 && s->queue[0].text.find(L"state=1") != std::wstring::npos, "state event callback preserves value");
			check(s->queue[1].text.find(L"mute=1") != std::wstring::npos && s->queue[2].text.find(L"disconnected reason=") != std::wstring::npos, "volume and disconnect callbacks preserved");
			auto dev = winrt::make_self<DebugAudio::DeviceEvents>(s);
			dev->OnDefaultDeviceChanged(eRender, eMultimedia, L"fixture-device");
			check(s->queue.back().text.find(L"default-changed flow=0 role=1") != std::wstring::npos && s->queue.back().text.find(L"fixture-device") == std::wstring::npos, "endpoint change hashed and role recorded");
			auto vol = winrt::make_self<DebugAudio::VolumeEvents>(s, L"fixture-endpoint");
			AUDIO_VOLUME_NOTIFICATION_DATA data {};
			data.fMasterVolume = 0.4f;
			data.bMuted = TRUE;
			data.nChannels = 1;
			data.afChannelVolumes[0] = 0.3f;
			vol->OnNotify(&data);
			check(s->queue.back().text.find(L"ch0=0.300000") != std::wstring::npos, "per-channel endpoint volume callback");
		}
		s->stopping = true;
		auto count = s->queue.size();
		s->Push({ GetTickCount64(), L"after-stop", true, true });
		check(s->queue.size() == count && s->pendingSnapshots == 0, "stop rejects late work");
		DebugAudio::Peak peak;
		peak.Read(nullptr);
		auto missing = peak.Take();
		check(missing.find(L"meter-errors=1") != std::wstring::npos && missing.find(L"samples=0") != std::wstring::npos, "missing meter never becomes measured zero");
		check(peak.samples == 0 && peak.errors == 0, "window counters reset after report");

		auto fake = winrt::make_self<FakeMeter>();
		peak.Read(fake.get());
		fake->value = 0;
		peak.Read(fake.get());
		fake->value = 1e-9f;
		peak.Read(fake.get());
		auto signal = peak.Take();
		check(signal.find(L"peak-max=0.5") != std::wstring::npos && signal.find(L"signal-samples=1") != std::wstring::npos && signal.find(L"samples=3") != std::wstring::npos, "window max and signal count preserve positive and zero samples");
		peak.Read(fake.get());
		check(peak.Take().find(L"peak-max=0 ") == std::wstring::npos, "tiny positive peaks not rounded to zero");
		// Actual worker starts before optional playback, then observes later playback.
		StartDebugAudioMonitor(dir, 42);
		auto worker = DebugAudio::monitor.load();
		EnqueueDebugAudioEvent(L"attempt=80 reason=OPENED_BASELINE", true);
		std::this_thread::sleep_for(std::chrono::milliseconds(2200));
		bool integration = argc > 1 && std::string(argv[1]) == "--audio";
		if (integration) {
			winrt::check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
			auto uninit = wil::scope_exit([] { CoUninitialize(); });
			winrt::com_ptr<IMMDeviceEnumerator> enumerator;
			winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
			winrt::com_ptr<IMMDevice> endpoint;
			winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, endpoint.put()));
			winrt::com_ptr<IAudioClient> client;
			winrt::check_hresult(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()));
			WAVEFORMATEX* raw = nullptr;
			winrt::check_hresult(client->GetMixFormat(&raw));
			wil::unique_cotaskmem_ptr<WAVEFORMATEX> format(raw);
			GUID sessionId {};
			winrt::check_hresult(CoCreateGuid(&sessionId));
			winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format.get(), &sessionId));
			winrt::com_ptr<IAudioRenderClient> render;
			winrt::check_hresult(client->GetService(__uuidof(IAudioRenderClient), render.put_void()));
			winrt::com_ptr<ISimpleAudioVolume> volume;
			winrt::check_hresult(client->GetService(__uuidof(ISimpleAudioVolume), volume.put_void()));
			// Muting only this test session: no change to existing sessions/device.
			winrt::check_hresult(volume->SetMute(TRUE, nullptr));
			UINT32 size = 0;
			winrt::check_hresult(client->GetBufferSize(&size));
			BYTE* buffer = nullptr;
			winrt::check_hresult(render->GetBuffer(size, &buffer));
			winrt::check_hresult(render->ReleaseBuffer(size, AUDCLNT_BUFFERFLAGS_SILENT));
			EnqueueDebugAudioEvent(L"TEST_USER_PLAY after-open=true own-silent-session=true");
			winrt::check_hresult(client->Start());
			for (int i = 0; i < 250; ++i) {
				UINT32 used = 0;
				winrt::check_hresult(client->GetCurrentPadding(&used));
				if (size > used) {
					winrt::check_hresult(render->GetBuffer(size - used, &buffer));
					winrt::check_hresult(render->ReleaseBuffer(size - used, AUDCLNT_BUFFERFLAGS_SILENT));
				}
				if (i == 75)
					EnqueueDebugAudioEvent(L"attempt=80 marker=1 reason=USER_NO_SOUND", true, true);
				if (i == 125)
					winrt::check_hresult(volume->SetMasterVolume(0.4f, nullptr));
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			winrt::check_hresult(client->Stop());
			EnqueueDebugAudioEvent(L"TEST_USER_STOP");
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
		} else
			EnqueueDebugAudioEvent(L"attempt=80 marker=1 reason=USER_NO_SOUND", true, true);
		EnqueueDebugAudioEvent(L"attempt=80 reason=CLOSE_AFTER_RETURN", true);
		StopDebugAudioMonitor();
		for (int i = 0; i < 1000 && !DebugAudioMonitorFinished(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		check(DebugAudioMonitorFinished() && PendingDebugAudioSnapshots() == 0, "actual worker drains queued snapshots on shutdown");
		auto body = Read(dir / (worker->run + L"-audio.log"));
		check(body.find("monitor-start schema=2 sample-period-ms=50") != std::string::npos, "high-frequency diagnostic schema emitted");
		check(body.find("heartbeat") != std::string::npos && body.find("monitor-stop queue-drained=true") != std::string::npos, "ongoing monitor and graceful completion");
		check(body.find("marker=1 reason=USER_NO_SOUND") != std::string::npos && body.find("capture-end") != std::string::npos, "queued marker audio snapshot present");
		check(body.find("reason=CLOSE_AFTER_RETURN") != std::string::npos, "post-close snapshot drains too");
		auto bundle = Read(dir / (worker->run + L"-silent-1.log"));
		check(bundle.find("OPENED_BASELINE") != std::string::npos && bundle.find("CLOSE_AFTER_RETURN") != std::string::npos, "real bundle spans open marker close");
		if (integration) {
			std::string pid = "pid=" + std::to_string(GetCurrentProcessId());
			auto hasLine = [&](std::string a, std::string b, std::string c) {std::istringstream lines(body);std::string line;while(std::getline(lines,line))if(line.find(a)!=std::string::npos&&line.find(b)!=std::string::npos&&line.find(c)!=std::string::npos)return true;return false; };
			check(hasLine("session-window", pid, "state=1"), "real later playback active session sampled");
			check(hasLine("session-event", pid, "volume=0.400000"), "real own-session volume event delivered");
			check(hasLine("session-event", pid, "state=0"), "real stopped playback state event delivered");
			check(body.find("session-created endpoint=") != std::string::npos, "new session notification delivered after GetCount");
			check(body.find("service=Audiosrv query-error=0") != std::string::npos, "audio service PID mapping captured");
		}
		std::cout << "MONITOR_EVIDENCE=" << dir.string() << '\n';
		// Preserve successful integration evidence for inspection outside project.
		if (integration)
			cleanup.release();
	} catch (...) {
		++failed;
		std::wcout << L"EXCEPTION " << DebugAudioHresult(winrt::to_hresult()) << L'\n';
	}
	std::cout << "DEBUG_MONITOR checks=" << checks << " failures=" << failed << '\n';
	return failed ? 1 : 0;
}
