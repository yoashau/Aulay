#pragma once

// Device enumeration: the DeviceWatcher lifecycle, restart backoff and
// reconciliation of the device list against actual discoverable devices.
#include "Aulay.h"

winrt::Windows::Foundation::IAsyncAction ReconcileDevicesAsync()
{
	auto dispatcher = g_uiDispatcher;
	auto reconciliationStarted = std::chrono::steady_clock::now();
	RecordDiagnostic(L"watcher", L"device reconciliation started");

	try {
		for (;;) {
			g_app.reconciliationPending = false;
			auto devices = co_await DeviceInformation::FindAllAsync(AudioPlaybackConnection::GetDeviceSelector());
			co_await winrt::resume_foreground(dispatcher);
			RecordDiagnostic(
				L"watcher",
				L"device reconciliation enumeration-count=" + std::to_wstring(devices.Size()));

			if (IsStopping())
				break;
			if (g_app.bluetooth.inProgress) {
				g_app.reconciliationPending = true;
				break;
			}

			std::unordered_set<std::wstring> seenDevices;
			for (const auto& device : devices) {
				auto deviceId = std::wstring(device.Id());
				seenDevices.insert(deviceId);
				UpsertDeviceRow(device);
				MaybeQueuePendingReconnect(device);
			}

			std::vector<std::wstring> staleDevices;
			staleDevices.reserve(g_discoveredDevices.size());
			for (const auto& discovered : g_discoveredDevices) {
				if (seenDevices.count(discovered.first) == 0)
					staleDevices.push_back(discovered.first);
			}
			for (const auto& deviceId : staleDevices)
				RemoveDeviceRow(deviceId);

			g_app.deviceEnumerationComplete = true;
			g_app.watcherRestartCount = 0;
			RefreshEmptyState();
			if (!g_app.reconciliationPending)
				break;
		}
	} catch (...) {
		if (!IsStopping()) {
			auto error = static_cast<HRESULT>(winrt::to_hresult());
			RecordDiagnostic(L"watcher", L"device reconciliation exception hr=" + FormatDiagnosticHresult(error));
			LOG_CAUGHT_EXCEPTION();
		}
	}

	try {
		co_await winrt::resume_foreground(dispatcher);
		g_app.reconciliationRunning = false;
		RecordDiagnostic(
			L"watcher",
			L"device reconciliation completed duration-ms=" + std::to_wstring(ElapsedMilliseconds(reconciliationStarted)));
		if (g_app.reconciliationPending && !g_app.bluetooth.inProgress && !IsStopping())
			ScheduleDeviceReconciliation();
	} catch (...) {
		if (!IsStopping())
			LOG_CAUGHT_EXCEPTION();
	}
}

void ScheduleDeviceReconciliation()
{
	if (IsStopping())
		return;

	g_app.reconciliationPending = true;
	if (g_app.bluetooth.inProgress || g_app.reconciliationRunning)
		return;

	g_app.reconciliationRunning = true;
	(void)ReconcileDevicesAsync();
}

void StopDeviceWatcher()
{
	if (!g_deviceWatcher)
		return;

	auto watcher = g_deviceWatcher;
	if (g_app.watcherHandlersAttached) {
		try {
			watcher.Added(g_app.watcherAddedToken);
			watcher.Removed(g_app.watcherRemovedToken);
			watcher.Updated(g_app.watcherUpdatedToken);
			watcher.EnumerationCompleted(g_app.watcherEnumerationCompletedToken);
			watcher.Stopped(g_app.watcherStoppedToken);
		} catch (...) {
			LOG_CAUGHT_EXCEPTION();
		}
		g_app.watcherHandlersAttached = false;
	}

	try {
		auto status = watcher.Status();
		RecordDiagnostic(L"watcher", L"stop requested status=" + std::wstring(DeviceWatcherStatusName(status)));
		if (status == DeviceWatcherStatus::Started || status == DeviceWatcherStatus::EnumerationCompleted)
			watcher.Stop();
	} catch (...) {
		auto error = static_cast<HRESULT>(winrt::to_hresult());
		RecordDiagnostic(L"watcher", L"stop exception hr=" + FormatDiagnosticHresult(error));
		LOG_CAUGHT_EXCEPTION();
	}

	g_deviceWatcher = nullptr;
}

void StartDeviceWatcher()
{
	if (IsStopping())
		return;

	StopDeviceWatcher();
	g_app.deviceEnumerationComplete = false;
	g_deviceWatcher = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
	RecordDiagnostic(L"watcher", L"watcher created");

	g_app.watcherAddedToken = g_deviceWatcher.Added([](DeviceWatcher const& sender, DeviceInformation const& device) {
		if (IsStopping())
			return;
		auto deviceId = std::wstring(device.Id());
		RecordDiagnostic(
			L"watcher",
			L"device-added token=" + DiagnosticDeviceToken(deviceId));

		try {
			(void)g_uiDispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
				[device, sender]() {
					if (IsStopping() || g_deviceWatcher != sender)
						return;
					if (g_app.bluetooth.inProgress) {
						g_app.reconciliationPending = true;
						return;
					}
					UpsertDeviceRow(device);
					MaybeQueuePendingReconnect(device);
				});
		} catch (...) {
			if (!IsStopping())
				LOG_CAUGHT_EXCEPTION();
		}
	});

	g_app.watcherRemovedToken = g_deviceWatcher.Removed([](DeviceWatcher const& sender, DeviceInformationUpdate const& update) {
		if (IsStopping())
			return;

		auto deviceId = std::wstring(update.Id());
		RecordDiagnostic(L"watcher", L"device-removed token=" + DiagnosticDeviceToken(deviceId));
		try {
			(void)g_uiDispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
				[deviceId = std::move(deviceId), sender]() {
					if (IsStopping() || g_deviceWatcher != sender)
						return;
					if (g_app.bluetooth.inProgress) {
						g_app.reconciliationPending = true;
						return;
					}
					if (g_app.sessions.count(deviceId) != 0 || IsConnectionQueued(deviceId)) {
						g_discoveredDevices.erase(deviceId);
						g_app.reconciliationPending = true;
						return;
					}
					RemoveDeviceRow(deviceId);
				});
		} catch (...) {
			if (!IsStopping())
				LOG_CAUGHT_EXCEPTION();
		}
	});

	g_app.watcherUpdatedToken = g_deviceWatcher.Updated([](DeviceWatcher const& sender, DeviceInformationUpdate const& update) {
		if (IsStopping())
			return;

		auto deviceId = std::wstring(update.Id());
		try {
			(void)g_uiDispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
				[deviceId = std::move(deviceId), update, sender]() {
					if (IsStopping() || g_deviceWatcher != sender)
						return;
					if (g_app.bluetooth.inProgress) {
						g_app.reconciliationPending = true;
						return;
					}

					auto discovered = g_discoveredDevices.find(deviceId);
					if (discovered == g_discoveredDevices.end()) {
						ScheduleDeviceReconciliation();
						return;
					}

					discovered->second.Update(update);
					auto row = g_deviceRows.find(deviceId);
					if (row != g_deviceRows.end())
						row->second.nameText.Text(discovered->second.Name());
				});
		} catch (...) {
			if (!IsStopping())
				LOG_CAUGHT_EXCEPTION();
		}
	});

	g_app.watcherEnumerationCompletedToken = g_deviceWatcher.EnumerationCompleted([](DeviceWatcher const& sender, const auto&) {
		if (IsStopping())
			return;

		try {
			(void)g_uiDispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
				[sender]() {
					if (IsStopping() || g_deviceWatcher != sender)
						return;
					g_app.deviceEnumerationComplete = true;
					g_app.watcherRestartCount = 0;
					RecordDiagnostic(
						L"watcher",
						L"initial enumeration completed discovered-count=" + std::to_wstring(g_discoveredDevices.size()));
					ScheduleDeviceReconciliation();
					RefreshEmptyState();
				});
		} catch (...) {
			if (!IsStopping())
				LOG_CAUGHT_EXCEPTION();
		}
	});

	g_app.watcherStoppedToken = g_deviceWatcher.Stopped([](DeviceWatcher const& sender, const auto&) {
		if (IsStopping())
			return;
		try {
			RecordDiagnostic(
				L"watcher",
				L"watcher stopped status=" + std::wstring(DeviceWatcherStatusName(sender.Status())));
		} catch (...) {
			RecordDiagnostic(L"watcher", L"watcher stopped; status unavailable");
		}

		try {
			(void)g_uiDispatcher.RunAsync(
				winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
				[sender]() {
					if (IsStopping() || g_deviceWatcher != sender || g_app.watcherRestartScheduled)
						return;
					g_app.watcherRestartScheduled = true;
					RestartDeviceWatcherAfterDelay();
				});
		} catch (...) {
			if (!IsStopping())
				LOG_CAUGHT_EXCEPTION();
		}
	});

	g_app.watcherHandlersAttached = true;
	try {
		g_deviceWatcher.Start();
		RecordDiagnostic(L"watcher", L"watcher start requested");
		RefreshEmptyState();
	} catch (...) {
		auto error = static_cast<HRESULT>(winrt::to_hresult());
		RecordDiagnostic(L"watcher", L"watcher start exception hr=" + FormatDiagnosticHresult(error));
		LOG_CAUGHT_EXCEPTION();
		StopDeviceWatcher();
		if (!g_app.watcherRestartScheduled && !IsStopping()) {
			g_app.watcherRestartScheduled = true;
			RestartDeviceWatcherAfterDelay();
		}
	}
}

winrt::fire_and_forget RestartDeviceWatcherAfterDelay()
{
	try {
		auto dispatcher = g_uiDispatcher;
		co_await winrt::resume_on_signal(g_app.stoppingSignal.get(), std::chrono::milliseconds(aulay::timing::RetryDelayMs(g_app.watcherRestartCount)));
		co_await winrt::resume_foreground(dispatcher);
		if (g_app.watcherRestartCount < 11)
			++g_app.watcherRestartCount;
		while (g_app.bluetooth.inProgress && !IsStopping()) {
			auto completion = g_app.bluetooth.completion;
			co_await winrt::resume_on_signal(completion->signal.get());
			co_await winrt::resume_foreground(dispatcher);
		}
		g_app.watcherRestartScheduled = false;
		if (!IsStopping()) {
			RecordDiagnostic(L"watcher", L"restarting stopped watcher");
			StartDeviceWatcher();
		}
	} catch (...) {
		if (!IsStopping())
			LOG_CAUGHT_EXCEPTION();
	}
}
