#pragma once

// Coalescing background-save wrapper over SettingsUtil.hpp's synchronous
// SaveSettings core. The UI thread only serializes the snapshot; the disk
// write and its forced flushes run on a background thread so a portable
// installation on slow storage never stalls connection handling. While a
// write is in flight, a newer snapshot replaces the pending one and the
// worker persists the latest state. Shutdown drains the pending snapshot
// asynchronously via FlushPendingSettings before destroying the UI. The native hardening test
// exercises the synchronous core directly and must keep compiling without
// this header.
#include <winrt/Windows.UI.Core.h>
#include "Aulay.h"
#include "SettingsUtil.hpp"

struct PendingSettingsSave {
	std::string utf8;
	size_t deviceCount = 0;
};
PendingSettingsSave g_settingsPending; // UI-thread only
bool g_settingsSaveWorkerRunning = false; // UI-thread only
bool g_settingsSaveFailed = false; // UI-thread only
wil::unique_event g_settingsSaveIdle { wil::EventOptions::ManualReset };

winrt::fire_and_forget SaveSettingsWorker();

bool QueueSaveSettings()
{
	try {
		g_settingsPending = PendingSettingsSave { BuildSettingsJson(), g_app.desiredDevices.size() };
		g_settingsSaveFailed = false;
		if (g_settingsSaveWorkerRunning)
			return true;
		g_settingsSaveWorkerRunning = true;
		g_settingsSaveIdle.ResetEvent();
		SaveSettingsWorker();
		return true;
	} catch (...) {
		g_settingsSaveFailed = true;
		RecordDiagnostic(L"settings", L"settings snapshot failed hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
		LOG_CAUGHT_EXCEPTION();
		return false;
	}
}

winrt::fire_and_forget SaveSettingsWorker()
{
	auto dispatcher = g_uiDispatcher;
	bool saveFailed = false;
	try {
		for (;;) {
			if (g_settingsPending.utf8.empty())
				break;
			auto pending = std::move(g_settingsPending);
			g_settingsPending = {};
			auto path = g_settingsStorage.activePath.empty()
				? g_settingsStorage.portablePath
				: g_settingsStorage.activePath;
			co_await winrt::resume_background();
			try {
				WriteSettingsAtomically(path, pending.utf8);
			} catch (...) {
				if (path == g_settingsStorage.fallbackPath || g_settingsStorage.fallbackPath.empty())
					throw;
				RecordDiagnostic(L"settings", L"portable save failed; trying localappdata hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
				path = g_settingsStorage.fallbackPath;
				fs::create_directories(path.parent_path());
				WriteSettingsAtomically(path, pending.utf8);
			}
			co_await winrt::resume_foreground(dispatcher);
			g_settingsStorage.activePath = path;
			RecordDiagnostic(L"settings", std::wstring(L"settings saved store=") + (path == g_settingsStorage.fallbackPath ? L"localappdata" : L"portable") + L" remembered-device-count=" + std::to_wstring(pending.deviceCount));
		}
	} catch (...) {
		RecordDiagnostic(L"settings", L"settings save failed hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
		LOG_CAUGHT_EXCEPTION();
		saveFailed = true;
	}
	co_await winrt::resume_foreground(dispatcher);
	g_settingsSaveFailed = saveFailed;
	if (saveFailed && !IsStopping() && !g_settingsStorage.saveErrorShown) {
		g_settingsStorage.saveErrorShown = true;
		TaskDialog(IsWindow(g_hWnd) ? g_hWnd : nullptr, nullptr, _(L"Aulay"), nullptr,
			_(L"Settings could not be saved. Check folder access or available disk space."),
			TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
	}
	g_settingsSaveWorkerRunning = false;
	g_settingsSaveIdle.SetEvent();
	// A newer snapshot may have arrived while the worker was finishing.
	if (!g_settingsPending.utf8.empty() && !IsStopping()) {
		g_settingsSaveWorkerRunning = true;
		g_settingsSaveIdle.ResetEvent();
		SaveSettingsWorker();
	}
}

// Keep the dispatcher alive and pumping while the worker commits its latest
// snapshot. A timeout bounds shutdown, but is not reported as a successful save.
winrt::Windows::Foundation::IAsyncAction FlushPendingSettings()
{
	auto dispatcher = g_uiDispatcher;
	co_await winrt::resume_foreground(dispatcher);
	if (!QueueSaveSettings()) {
		RecordDiagnostic(L"settings", L"shutdown settings drain failed latest-snapshot-may-be-unsaved");
		co_return;
	}
	bool finished = true;
	if (g_settingsSaveWorkerRunning)
		finished = co_await winrt::resume_on_signal(g_settingsSaveIdle.get(), std::chrono::milliseconds(5000));
	co_await winrt::resume_foreground(dispatcher);
	if (!finished)
		RecordDiagnostic(L"settings", L"shutdown settings drain timeout-ms=5000 latest-snapshot-may-be-unsaved");
	else if (g_settingsSaveFailed || !g_settingsPending.utf8.empty())
		RecordDiagnostic(L"settings", L"shutdown settings drain failed latest-snapshot-may-be-unsaved");
	else
		RecordDiagnostic(L"settings", L"shutdown settings drain complete");
}
