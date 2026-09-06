#pragma once

// Shared connection/recovery state helpers: timing budgets, status names,
// debug snapshot helpers, the radio-transition grace window and session
// and error state accessors. Pure state and helpers; no UI, no coroutines.
#include "Aulay.h"
#include "DebugAudioMonitor.hpp"
#include "SettingsSave.hpp"

constexpr uint32_t CONNECTION_OPENED_WAIT_MS = 1500;
// Retry indefinitely until success/cancellation; only the interval is capped.
constexpr uint32_t CONNECTION_ATTEMPT_TIMEOUT_MS = 45000;
constexpr uint32_t BLUETOOTH_RECOVERY_TIMEOUT_MS = 15000;
constexpr uint32_t BLUETOOTH_RESTORE_TIMEOUT_MS = 8000;
constexpr uint32_t BLUETOOTH_OFF_TIMEOUT_MS = 5000;
constexpr uint32_t BLUETOOTH_ON_TIMEOUT_MS = 8000;
constexpr uint32_t RADIO_DEACTIVATION_GRACE_TAIL_MS = 1500;

bool IsBluetoothRecoveryActive()
{
	return g_app.unexpectedDisconnectRecoveryPending || g_app.bluetooth.inProgress;
}

// A radio turn-off/turn-on transiently deactivates the hidden host window, which
// must not dismiss the device flyout the user is looking at. The grace window is
// stamped only around an in-flight transition (its bounded budget, then a short
// tail for late deactivation messages), so the rest of recovery, including long

std::atomic<ULONGLONG> g_radioTransitionGraceUntilMs { 0 };

void NoteRadioTransitionStarted(uint32_t budgetMs)
{
	g_radioTransitionGraceUntilMs.store(
		GetTickCount64() + budgetMs + RADIO_DEACTIVATION_GRACE_TAIL_MS);
}

void NoteRadioTransitionSettled()
{
	g_radioTransitionGraceUntilMs.store(
		GetTickCount64() + RADIO_DEACTIVATION_GRACE_TAIL_MS);
}

bool IsRadioDeactivationGraceActive()
{
	return GetTickCount64() < g_radioTransitionGraceUntilMs.load();
}

// WM_ACTIVATEAPP only reports the transition from active to inactive: once the
// host window is already inactive, later clicks elsewhere raise no further
// message and the device flyout would stay stuck on screen (for example after
// a deactivation deferred by the radio-transition grace window). While the
// flyout is visible, a low-level mouse hook and a foreground-event hook watch
// for interaction with windows outside this process and dismiss it through
// WM_DISMISSDEVICEFLYOUT. Both hooks are installed on and pump through the UI
// thread only while the flyout is open. The mouse hook treats every outside
// click as explicit user intent, so it deliberately ignores the grace window.
HHOOK g_deviceFlyoutMouseHook = nullptr;
HWINEVENTHOOK g_deviceFlyoutForegroundHook = nullptr;

bool IsStopping()
{
	return g_app.shutdownRequested.load() || g_app.shuttingDown.load();
}

const wchar_t* BluetoothStageName(BluetoothRecoveryStage stage)
{
	switch (stage) {
	case BluetoothRecoveryStage::Idle:
		return L"idle";
	case BluetoothRecoveryStage::RequestingAccess:
		return L"requesting-access";
	case BluetoothRecoveryStage::FindingRadio:
		return L"finding-radio";
	case BluetoothRecoveryStage::TurningOff:
		return L"turning-off";
	case BluetoothRecoveryStage::WaitingForOff:
		return L"waiting-for-off";
	case BluetoothRecoveryStage::TurningOn:
		return L"turning-on";
	case BluetoothRecoveryStage::WaitingForOn:
		return L"waiting-for-on";
	case BluetoothRecoveryStage::Succeeded:
		return L"succeeded";
	case BluetoothRecoveryStage::Failed:
		return L"failed";
	default:
		return L"unknown";
	}
}

const wchar_t* BluetoothRecoveryResultName(BluetoothRecoveryResult result)
{
	switch (result) {
	case BluetoothRecoveryResult::NotRequired:
		return L"not-required";
	case BluetoothRecoveryResult::Success:
		return L"success";
	case BluetoothRecoveryResult::AccessDenied:
		return L"access-denied";
	case BluetoothRecoveryResult::RadioUnavailable:
		return L"radio-unavailable";
	case BluetoothRecoveryResult::TurnOffDenied:
		return L"turn-off-denied";
	case BluetoothRecoveryResult::TurnOffTimedOut:
		return L"turn-off-timed-out";
	case BluetoothRecoveryResult::TurnOnDenied:
		return L"turn-on-denied";
	case BluetoothRecoveryResult::TurnOnTimedOut:
		return L"turn-on-timed-out";
	case BluetoothRecoveryResult::UnexpectedFailure:
		return L"unexpected-failure";
	case BluetoothRecoveryResult::OperationTimedOut:
		return L"operation-timed-out";
	case BluetoothRecoveryResult::Cancelled:
		return L"cancelled";
	default:
		return L"unknown";
	}
}

const wchar_t* RadioAccessStatusName(winrt::Windows::Devices::Radios::RadioAccessStatus status)
{
	using winrt::Windows::Devices::Radios::RadioAccessStatus;
	switch (status) {
	case RadioAccessStatus::Unspecified:
		return L"unspecified";
	case RadioAccessStatus::Allowed:
		return L"allowed";
	case RadioAccessStatus::DeniedByUser:
		return L"denied-by-user";
	case RadioAccessStatus::DeniedBySystem:
		return L"denied-by-system";
	default:
		return L"unknown";
	}
}

const wchar_t* RadioStateName(winrt::Windows::Devices::Radios::RadioState state)
{
	using winrt::Windows::Devices::Radios::RadioState;
	switch (state) {
	case RadioState::Unknown:
		return L"unknown";
	case RadioState::On:
		return L"on";
	case RadioState::Off:
		return L"off";
	case RadioState::Disabled:
		return L"disabled";
	default:
		return L"invalid";
	}
}

const wchar_t* ConnectionAttemptResultName(ConnectionAttemptResult result)
{
	switch (result) {
	case ConnectionAttemptResult::Cancelled:
		return L"cancelled";
	case ConnectionAttemptResult::Success:
		return L"success";
	case ConnectionAttemptResult::BluetoothRecoveryFailed:
		return L"bluetooth-recovery-failed";
	case ConnectionAttemptResult::DeviceUnavailable:
		return L"device-unavailable";
	case ConnectionAttemptResult::StartFailed:
		return L"start-failed";
	case ConnectionAttemptResult::OpenTimedOut:
		return L"open-timed-out";
	case ConnectionAttemptResult::DeniedBySystem:
		return L"denied-by-system";
	case ConnectionAttemptResult::OpenFailed:
		return L"open-failed";
	case ConnectionAttemptResult::ClosedUnexpectedly:
		return L"closed-unexpectedly";
	case ConnectionAttemptResult::UnexpectedFailure:
		return L"unexpected-failure";
	case ConnectionAttemptResult::DeadlineExceeded:
		return L"deadline-exceeded";
	default:
		return L"unknown";
	}
}

const wchar_t* ConnectionRequestModeName(ConnectionRequestMode mode)
{
	switch (mode) {
	case ConnectionRequestMode::Automatic:
		return L"automatic";
	case ConnectionRequestMode::Quick:
		return L"quick";
	case ConnectionRequestMode::Forced:
		return L"forced";
	default:
		return L"unknown";
	}
}

const wchar_t* ConnectionRecoveryStrategyName(ConnectionRecoveryStrategy strategy)
{
	switch (strategy) {
	case ConnectionRecoveryStrategy::Direct:
		return L"direct";
	case ConnectionRecoveryStrategy::RestartBluetooth:
		return L"restart-bluetooth";
	default:
		return L"unknown";
	}
}

ConnectionRecoveryStrategy SelectInitialRecoveryStrategy(ConnectionRequestMode mode)
{
	if (mode == ConnectionRequestMode::Forced)
		return ConnectionRecoveryStrategy::RestartBluetooth;

	return ConnectionRecoveryStrategy::Direct;
}

ConnectionRecoveryStrategy SelectRetryRecoveryStrategy(
	ConnectionRequest const& request,
	ConnectionAttemptResult result)
{
	// Only the explicit Forced initial attempt may reset the adapter.
	(void)request;
	(void)result;
	return ConnectionRecoveryStrategy::Direct;
}

const wchar_t* AudioOpenStatusName(AudioPlaybackConnectionOpenResultStatus status)
{
	switch (status) {
	case AudioPlaybackConnectionOpenResultStatus::Success:
		return L"success";
	case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
		return L"request-timed-out";
	case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
		return L"denied-by-system";
	case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
		return L"unknown-failure";
	default:
		return L"unknown";
	}
}

const wchar_t* AudioConnectionStateName(AudioPlaybackConnectionState state)
{
	switch (state) {
	case AudioPlaybackConnectionState::Closed:
		return L"closed";
	case AudioPlaybackConnectionState::Opened:
		return L"opened";
	default:
		return L"unknown";
	}
}

const wchar_t* ConnectionPhaseName(ConnectionPhase phase)
{
	switch (phase) {
	case ConnectionPhase::RecoveringBluetooth:
		return L"recovering-bluetooth";
	case ConnectionPhase::Starting:
		return L"starting";
	case ConnectionPhase::Opening:
		return L"opening";
	case ConnectionPhase::WaitingForOpened:
		return L"waiting-for-opened";
	case ConnectionPhase::Connected:
		return L"connected";
	case ConnectionPhase::Closing:
		return L"closing";
	default:
		return L"unknown";
	}
}

const wchar_t* DeviceWatcherStatusName(DeviceWatcherStatus status)
{
	switch (status) {
	case DeviceWatcherStatus::Created:
		return L"created";
	case DeviceWatcherStatus::Started:
		return L"started";
	case DeviceWatcherStatus::EnumerationCompleted:
		return L"enumeration-completed";
	case DeviceWatcherStatus::Stopping:
		return L"stopping";
	case DeviceWatcherStatus::Stopped:
		return L"stopped";
	case DeviceWatcherStatus::Aborted:
		return L"aborted";
	default:
		return L"unknown";
	}
}

uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start)
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start)
			.count());
}

struct DebugDisconnectRecord {
	uint64_t attemptId = 0;
	uint64_t generation = 0;
	uint64_t tick = 0;
};
std::unordered_map<std::wstring, DebugDisconnectRecord> g_debugLastDisconnect;
uint64_t g_debugNextMarker = 0;
void RequestDebugAudioSnapshot(std::wstring trace)
{
	// The worker preserves explicit snapshots instead of dropping busy requests.
	EnqueueDebugAudioEvent(trace, true, trace.find(L"reason=USER_NO_SOUND") != std::wstring::npos);
}

void RecordDebugConnectionSnapshot(std::wstring const& deviceId, std::wstring_view reason,
	uint64_t marker, bool captureAudio)
{
	auto session = g_app.sessions.find(deviceId);
	if (session == g_app.sessions.end())
		return;
	auto const& current = session->second;
	std::wstring state = L"unavailable";
	try {
		if (current.connection)
			state = AudioConnectionStateName(current.connection.State());
	} catch (...) {
		state += L":" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult()));
	}
	auto detail = std::wstring(reason) + L" marker=" + std::to_wstring(marker) + L" generation=" + std::to_wstring(current.generation) + L" mode=" + ConnectionRequestModeName(current.requestMode) + L" phase=" + ConnectionPhaseName(current.phase) + L" state=" + state + L" reports=" + std::to_wstring(current.noSoundReports) + L" connected-ms=" + std::to_wstring(current.phase == ConnectionPhase::Connected ? ElapsedMilliseconds(current.connectedAt) : 0) + L" radio-stage=" + BluetoothStageName(g_app.bluetooth.stage) + L" radio-result=" + BluetoothRecoveryResultName(g_app.bluetooth.lastResult) + L" audio-monitor={" + DebugAudioMonitorStatus() + L"}";
	auto previous = g_debugLastDisconnect.find(deviceId);
	if (previous != g_debugLastDisconnect.end()) {
		detail += L" previous-disconnect-attempt=" + std::to_wstring(previous->second.attemptId) + L" previous-disconnect-generation=" + std::to_wstring(previous->second.generation) + L" since-disconnect-ms=" + std::to_wstring(GetTickCount64() - previous->second.tick);
	} else
		detail += L" previous-disconnect=none-in-this-run";
	RecordConnectionDiagnostic(current.attemptId, deviceId, detail);
	if (captureAudio)
		RequestDebugAudioSnapshot(L"attempt=" + std::to_wstring(current.attemptId) + L" device=" + DiagnosticDeviceToken(deviceId) + L" marker=" + std::to_wstring(marker) + L" reason=" + std::wstring(reason) + L" requested-tick-ms=" + std::to_wstring(GetTickCount64()));
}

bool HasConnectedSessions()
{
	for (const auto& item : g_app.sessions) {
		if (item.second.phase != ConnectionPhase::Connected || !item.second.connection)
			continue;

		try {
			if (item.second.connection.State() != AudioPlaybackConnectionState::Closed)
				return true;
		} catch (...) {
			// Treat an unreadable established session as active. Resetting the adapter
			// while it may still be streaming is more harmful than deferring recovery.
			return true;
		}
	}
	return false;
}

bool HasConnectionActivity()
{
	return !g_app.sessions.empty() || !g_app.connectionQueue.empty() || g_app.connectionWorkerRunning || g_app.unexpectedDisconnectRecoveryPending;
}

bool IsDesiredDevice(std::wstring const& deviceId)
{
	return std::find(g_app.desiredDevices.begin(), g_app.desiredDevices.end(), deviceId) != g_app.desiredDevices.end();
}

void RememberDevice(std::wstring const& deviceId)
{
	if (deviceId.empty() || IsDesiredDevice(deviceId))
		return;

	g_app.desiredDevices.push_back(deviceId);
	if (g_app.reconnectEnabled)
		QueueSaveSettings();
}

void ForgetDevice(std::wstring const& deviceId)
{
	auto oldSize = g_app.desiredDevices.size();
	g_app.desiredDevices.erase(
		std::remove(g_app.desiredDevices.begin(), g_app.desiredDevices.end(), deviceId),
		g_app.desiredDevices.end());
	if (oldSize != g_app.desiredDevices.size() && g_app.reconnectEnabled)
		QueueSaveSettings();
}

void ClearDeviceError(std::wstring const& deviceId)
{
	g_app.deviceErrors.erase(deviceId);
}

void SetDeviceError(std::wstring const& deviceId, std::wstring message, uint64_t attemptId)
{
	g_app.deviceErrors.insert_or_assign(deviceId, DeviceErrorState { std::move(message), attemptId });
}

std::wstring FormatDeviceError(DeviceErrorState const& error)
{
	std::wstring message = error.message;
	if (error.attemptId != 0) {
		message += L"  [#";
		message += std::to_wstring(error.attemptId);
		message += L"]";
	}
	return message;
}

// The tray tooltip mirrors the overall state: what is connected, or what the
