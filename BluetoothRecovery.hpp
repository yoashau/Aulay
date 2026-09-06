#pragma once

// Bluetooth adapter recovery: radio access, off/on power transitions with
// bounded deadlines and the EnsureBluetoothReady entry point.
#include "Aulay.h"


winrt::Windows::Foundation::IAsyncAction SetBluetoothRecoveryStage(BluetoothRecoveryStage stage)
{
	co_await winrt::resume_foreground(g_uiDispatcher);
	g_app.bluetooth.stage = stage;
	RecordDiagnostic(L"bluetooth", BluetoothStageName(stage));
	for (const auto& row : g_deviceRows)
		ApplySessionStatusToRow(row.first);
	RefreshEmptyState();
}

winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Radios::RadioAccessStatus> SetRadioStateInBackground(
    winrt::Windows::Devices::Radios::Radio radio,winrt::Windows::Devices::Radios::RadioState state)
{
    auto cancellation=co_await winrt::get_cancellation_token();
    co_await winrt::resume_background();
    auto operation=radio.SetStateAsync(state);
    cancellation.callback([operation]{CancelAsyncInBackground(operation);});
    if(cancellation()) {
        CancelAsyncInBackground(operation);
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    co_return co_await operation;
}

winrt::Windows::Foundation::IAsyncAction StartConnectionInBackground(AudioPlaybackConnection connection)
{
    auto cancellation=co_await winrt::get_cancellation_token();
    co_await winrt::resume_background();
    auto operation=connection.StartAsync();
    cancellation.callback([operation]{CancelAsyncInBackground(operation);});
    if(cancellation()) {
        CancelAsyncInBackground(operation);
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    co_await operation;
}

winrt::Windows::Foundation::IAsyncOperation<AudioPlaybackConnectionOpenResult> OpenConnectionInBackground(AudioPlaybackConnection connection)
{
    auto cancellation=co_await winrt::get_cancellation_token();
    co_await winrt::resume_background();
    auto operation=connection.OpenAsync();
    cancellation.callback([operation]{CancelAsyncInBackground(operation);});
    if(cancellation()) {
        CancelAsyncInBackground(operation);
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    co_return co_await operation;
}

winrt::Windows::Foundation::IAsyncOperation<bool> WaitForRadioState(
	winrt::Windows::Devices::Radios::Radio radio,
	winrt::Windows::Devices::Radios::RadioState targetState,
	std::chrono::milliseconds timeout)
{
	using namespace winrt::Windows::Devices::Radios;

	if (!radio)
		co_return false;

	// Subscribe before checking the current value so a transition between the
	// subscription and the check cannot be lost. The Win32 event wakes the waiter
	// immediately; the timeout is only a guard for a driver that never reports the
	// requested terminal state.
	auto stateChanged = std::make_shared<wil::unique_event>(wil::EventOptions::ManualReset);
	auto stateChangedRevoker = radio.StateChanged(winrt::auto_revoke,
		[stateChanged, targetState](Radio const& sender, winrt::Windows::Foundation::IInspectable const&) noexcept {
			try
			{
				if (sender.State() == targetState)
					stateChanged->SetEvent();
			}
			catch (...)
			{
				// A failed event callback must not escape into the WinRT event source.
			}
		});
	(void)stateChangedRevoker;

	if (radio.State() == targetState)
		stateChanged->SetEvent();

	// Register an asynchronous kernel wait instead of occupying a worker thread.
	auto signaled = co_await winrt::resume_on_signal(stateChanged->get(), timeout);
	co_return signaled && radio.State() == targetState;
}

winrt::Windows::Foundation::IAsyncOperation<int32_t> RunBluetoothRecoveryTransaction(
	winrt::Windows::Devices::Radios::Radio radio,
	AsyncDeadline primaryDeadline, std::shared_ptr<AsyncCancellation> cancellation)
{
	using namespace winrt::Windows::Devices::Radios;
	auto transactionStarted = std::chrono::steady_clock::now();
	bool offConfirmed = false;
	bool offRequested = false;
	BluetoothRecoveryResult primaryResult = BluetoothRecoveryResult::UnexpectedFailure;
	RecordDiagnostic(L"bluetooth", L"transaction-start route=device-associated");
	try
	{
		CheckAsyncDeadline(primaryDeadline, cancellation);
		co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::RequestingAccess);
		RadioAccessStatus access = RadioAccessStatus::Unspecified;
		if (g_app.radioAccess == RadioAccessStatus::Allowed)
			access = *g_app.radioAccess;
		else
		{
			CheckAsyncDeadline(primaryDeadline, cancellation);
			access = co_await AwaitBounded(Radio::RequestAccessAsync(), primaryDeadline, cancellation);
			if (access == RadioAccessStatus::Allowed)
				g_app.radioAccess = access;
			else
				g_app.radioAccess.reset();
		}
		RecordDiagnostic(L"bluetooth", L"request-access status=" + std::wstring(RadioAccessStatusName(access)));
		if (access != RadioAccessStatus::Allowed)
			primaryResult = BluetoothRecoveryResult::AccessDenied;
		else if (!radio)
			primaryResult = BluetoothRecoveryResult::RadioUnavailable;
		else if (radio.State() == RadioState::Off)
		{
			offConfirmed = true;
			primaryResult = BluetoothRecoveryResult::NotRequired;
		}
		else
		{
			co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::TurningOff);
			CheckAsyncDeadline(primaryDeadline, cancellation);
			auto offStarted = std::chrono::steady_clock::now();
			auto offDeadline = (std::min)(primaryDeadline, std::chrono::steady_clock::now() +
				std::chrono::milliseconds(BLUETOOTH_OFF_TIMEOUT_MS));
			offRequested = true;
			NoteRadioTransitionStarted(BLUETOOTH_OFF_TIMEOUT_MS);
			auto offStatus = co_await AwaitBounded(SetRadioStateInBackground(radio, RadioState::Off), offDeadline, cancellation);
			RecordDiagnostic(L"bluetooth", L"turn-off-request status=" + std::wstring(RadioAccessStatusName(offStatus)) +
				L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(offStarted)));
			if (offStatus != RadioAccessStatus::Allowed)
			{
				g_app.radioAccess.reset();
				primaryResult = BluetoothRecoveryResult::TurnOffDenied;
			}
			else
			{
				co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::WaitingForOff);
				offConfirmed = co_await AwaitBounded(
					WaitForRadioState(radio, RadioState::Off, std::chrono::milliseconds(BLUETOOTH_OFF_TIMEOUT_MS)),
					offDeadline, cancellation);
				RecordDiagnostic(L"bluetooth", L"turn-off-confirmed=" + std::wstring(offConfirmed ? L"true" : L"false"));
				primaryResult = offConfirmed ? BluetoothRecoveryResult::NotRequired : BluetoothRecoveryResult::TurnOffTimedOut;
			}
		}
	}
	catch (...)
	{
		auto error = static_cast<HRESULT>(winrt::to_hresult());
		RecordDiagnostic(L"bluetooth", L"primary-phase exception hr=" + FormatDiagnosticHresult(error));
		primaryResult = error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? BluetoothRecoveryResult::OperationTimedOut
			: (error == HRESULT_FROM_WIN32(ERROR_CANCELLED) || error == E_ABORT) ? BluetoothRecoveryResult::Cancelled
			: BluetoothRecoveryResult::UnexpectedFailure;
	}

	// Cleanup has an independent bounded budget and deliberately ignores the
	// connection/shutdown cancellation. Once Off was sent, always compensate On.
	bool onConfirmed = false;
	BluetoothRecoveryResult onFailure = BluetoothRecoveryResult::TurnOnTimedOut;
	if (radio && (offRequested || offConfirmed))
	{
		auto restoreDeadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(BLUETOOTH_RESTORE_TIMEOUT_MS);
		try
		{
			if (!offRequested && radio.State() == RadioState::On)
				onConfirmed = true;
			else
			{
				co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::TurningOn);
				auto onStarted = std::chrono::steady_clock::now();
				NoteRadioTransitionStarted(BLUETOOTH_RESTORE_TIMEOUT_MS);
				auto onStatus = co_await AwaitBounded(SetRadioStateInBackground(radio, RadioState::On), restoreDeadline);
				RecordDiagnostic(L"bluetooth", L"turn-on-request status=" + std::wstring(RadioAccessStatusName(onStatus)) +
					L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(onStarted)));
				if (onStatus != RadioAccessStatus::Allowed)
				{
					g_app.radioAccess.reset();
					onFailure = BluetoothRecoveryResult::TurnOnDenied;
				}
				else
				{
					co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::WaitingForOn);
					onConfirmed = co_await AwaitBounded(
						WaitForRadioState(radio, RadioState::On, std::chrono::milliseconds(BLUETOOTH_ON_TIMEOUT_MS)),
						restoreDeadline);
					RecordDiagnostic(L"bluetooth", L"turn-on-confirmed=" + std::wstring(onConfirmed ? L"true" : L"false"));
				}
			}
		}
		catch (...)
		{
			auto error = static_cast<HRESULT>(winrt::to_hresult());
			RecordDiagnostic(L"bluetooth", L"turn-on exception hr=" + FormatDiagnosticHresult(error));
			onFailure = error == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
				? BluetoothRecoveryResult::TurnOnTimedOut : BluetoothRecoveryResult::UnexpectedFailure;
		}
	}
	NoteRadioTransitionSettled();
	auto result = primaryResult;
	if (offConfirmed && onConfirmed && primaryResult == BluetoothRecoveryResult::NotRequired)
		result = BluetoothRecoveryResult::Success;
	else if ((offRequested || offConfirmed) && !onConfirmed)
		result = onFailure;
	RecordDiagnostic(L"bluetooth", L"transaction-complete result=" + std::wstring(BluetoothRecoveryResultName(result)) +
		L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(transactionStarted)));
	co_return static_cast<int32_t>(result);
}

winrt::Windows::Foundation::IAsyncOperation<int32_t> EnsureBluetoothReady(
	bool forceRecovery, bool allowActiveSessionReset, std::wstring deviceId,
	std::shared_ptr<AsyncCancellation> cancellation, AsyncDeadline deadline)
{
	auto dispatcher = g_uiDispatcher;
	co_await winrt::resume_foreground(dispatcher);

	if (forceRecovery)
		g_app.bluetooth.needed = true;

	if (HasConnectedSessions() && !allowActiveSessionReset)
	{
		RecordDiagnostic(L"bluetooth", L"recovery deferred because an active audio session exists");
		co_return static_cast<int32_t>(BluetoothRecoveryResult::NotRequired);
	}
	if (!g_app.bluetooth.needed)
	{
		RecordDiagnostic(L"bluetooth", L"recovery not required");
		co_return static_cast<int32_t>(BluetoothRecoveryResult::NotRequired);
	}

	if (g_app.bluetooth.inProgress)
	{
		auto completion = g_app.bluetooth.completion;
		RecordDiagnostic(L"bluetooth", L"joining active recovery by completion event");
		co_await winrt::resume_on_signal(completion->signal.get());
		co_return static_cast<int32_t>(completion->result);
	}

	if (IsStopping())
		co_return static_cast<int32_t>(BluetoothRecoveryResult::UnexpectedFailure);

	auto completion = std::make_shared<BluetoothRecoveryCompletion>();
	g_app.bluetooth.completion = completion;
	if (!cancellation)
		cancellation = std::make_shared<AsyncCancellation>();
	g_app.bluetooth.cancellation = cancellation;
	g_app.bluetooth.deviceId = deviceId;
	g_app.bluetooth.inProgress = true;

	g_app.bluetooth.lastSucceeded = false;
	++g_app.bluetooth.generation;
	auto recoveryStarted = std::chrono::steady_clock::now();

	BluetoothRecoveryResult result = BluetoothRecoveryResult::UnexpectedFailure;
	try
	{
		deadline = (std::min)(deadline, std::chrono::steady_clock::now() +
			std::chrono::milliseconds(BLUETOOTH_RECOVERY_TIMEOUT_MS));
		co_await SetBluetoothRecoveryStage(BluetoothRecoveryStage::FindingRadio);
		CheckAsyncDeadline(deadline, cancellation);
		auto radio = co_await AwaitBounded(ResolveBluetoothRadio(deviceId, deadline, cancellation), deadline, cancellation);
		co_await winrt::resume_foreground(dispatcher);
		CheckAsyncDeadline(deadline, cancellation);
		if (allowActiveSessionReset)
		{
			// Revoke old sessions before requesting Off. Otherwise delayed Closed
			// callbacks can arrive after recovery and trigger a second adapter reset.
			std::vector<std::wstring> activeIds;
			for (const auto& item : g_app.sessions)
				if (item.second.phase == ConnectionPhase::Connected)
					activeIds.push_back(item.first);
			for (const auto& id : activeIds)
				CloseConnectionSession(id, std::nullopt, false);
		}
        std::vector<std::shared_ptr<AudioFlow::State>> tasks;
        for(auto const& item:g_audioFlowTasks)tasks.push_back(item.second);
        for(auto const& task:tasks) {
            co_await AwaitBounded(AudioFlow::WaitFinished(task),deadline,cancellation);
            co_await winrt::resume_foreground(dispatcher);
        }
		RefreshEmptyState();
		result = static_cast<BluetoothRecoveryResult>(co_await RunBluetoothRecoveryTransaction(radio, deadline, cancellation));
	}
	catch (...)
	{
		auto error = static_cast<HRESULT>(winrt::to_hresult());
		RecordDiagnostic(L"bluetooth", L"recovery coroutine exception hr=" + FormatDiagnosticHresult(error));
		LOG_CAUGHT_EXCEPTION();
		result = error == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? BluetoothRecoveryResult::OperationTimedOut
			: (error == HRESULT_FROM_WIN32(ERROR_CANCELLED) || error == E_ABORT) ? BluetoothRecoveryResult::Cancelled
			: error == E_ACCESSDENIED ? BluetoothRecoveryResult::AccessDenied
			: error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? BluetoothRecoveryResult::RadioUnavailable
			: BluetoothRecoveryResult::UnexpectedFailure;
	}

	co_await winrt::resume_foreground(dispatcher);
	g_app.bluetooth.lastResult = result;
	g_app.bluetooth.lastSucceeded = result == BluetoothRecoveryResult::Success;
	g_app.bluetooth.needed = result != BluetoothRecoveryResult::Success;
	g_app.bluetooth.stage = g_app.bluetooth.lastSucceeded
		? BluetoothRecoveryStage::Succeeded
		: BluetoothRecoveryStage::Failed;
	g_app.bluetooth.inProgress = false;
	g_app.activityChanged.SetEvent();
	g_app.bluetooth.cancellation.reset();
	completion->result = result;
	completion->signal.SetEvent();
	RecordDiagnostic(
		L"bluetooth",
		L"recovery-complete generation=" + std::to_wstring(g_app.bluetooth.generation) +
		L" result=" + BluetoothRecoveryResultName(result) +
		L" needed=" + (g_app.bluetooth.needed ? L"true" : L"false") +
		L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(recoveryStarted)));

	if (!IsStopping())
	{
		g_app.reconciliationPending = true;
		ScheduleDeviceReconciliation();
		for (const auto& row : g_deviceRows)
			ApplySessionStatusToRow(row.first);
		RefreshEmptyState();
	}

	co_return static_cast<int32_t>(result);
}

// Row status text is localized: failures show the generic localized text plus
// the raw code instead of the system's English message. Full failure details
