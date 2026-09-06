#pragma once

// Audio connection flow: session lifecycle, the connect attempt state
// machine, unexpected-disconnect handling and the serialized request queue.
#include "Aulay.h"

void MarkNoSound(std::wstring const& deviceId)
{
	if (IsStopping())
		return;
	auto session = g_app.sessions.find(deviceId);
	if (session == g_app.sessions.end() || session->second.phase != ConnectionPhase::Connected)
		return;
	++session->second.noSoundReports;
	RecordDebugConnectionSnapshot(deviceId, L"USER_NO_SOUND", ++g_debugNextMarker, true);
	ApplySessionStatusToRow(deviceId);
	QueueConnection(deviceId, ConnectionRequestMode::Forced);
}

void RevokeConnectionHandler(ConnectionSession& session)
{
	if (!session.connection || !session.hasStateChangedToken)
		return;

	try {
		session.connection.StateChanged(session.stateChangedToken);
	} catch (...) {
		LOG_CAUGHT_EXCEPTION();
	}
	session.hasStateChangedToken = false;
}

winrt::fire_and_forget CloseSessionInBackground(ConnectionSession retired,
	std::wstring deviceId, std::shared_ptr<AudioFlow::State> flow, bool workerStarted,
	std::shared_ptr<AudioFlow::State> predecessor)
{
	auto started = GetTickCount64();
	try {
		if (predecessor && predecessor != flow)
			co_await AudioFlow::WaitFinished(predecessor);
		co_await winrt::resume_background();
		RevokeConnectionHandler(retired);
		if (retired.connection)
			retired.connection.Close();
		RecordConnectionDiagnostic(retired.attemptId, deviceId, L"CLOSE_RETURN hr=0x00000000 duration-ms=" + std::to_wstring(GetTickCount64() - started));
	} catch (...) {
		RecordConnectionDiagnostic(retired.attemptId, deviceId, L"CLOSE_RETURN hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
	}
	RequestDebugAudioSnapshot(L"attempt=" + std::to_wstring(retired.attemptId) + L" device=" + DiagnosticDeviceToken(deviceId) + L" reason=CLOSE_AFTER_RETURN requested-tick-ms=" + std::to_wstring(GetTickCount64()));
	AudioFlow::SignalClosed(*flow);
	if (!workerStarted) {
		flow->done = true;
		SetEvent(flow->finishedSignal.get());
	}
}

bool CloseConnectionSession(std::wstring const& deviceId, std::optional<uint64_t> generation, bool markRecoveryNeeded, bool radioWillReset)
{
	auto session = g_app.sessions.find(deviceId);
	if (session == g_app.sessions.end() || (generation && session->second.generation != *generation))
		return false;
	RecordDebugConnectionSnapshot(deviceId, L"CLOSE_REQUEST_ASYNC", 0, true);
	auto wasConnected = session->second.phase == ConnectionPhase::Connected;
	auto retired = std::move(session->second);
	g_app.sessions.erase(session); // invalidates every queued callback before worker-side unsubscribe
	if (retired.cancellation)
		retired.cancellation->Request();
	if (retired.openedSignal)
		retired.openedSignal->SetEvent();
	bool cleanAudio = wasConnected && !IsStopping() && !radioWillReset && !g_app.bluetooth.inProgress && !g_app.unexpectedDisconnectRecoveryPending;
	auto flow = retired.audioFlow;
	auto oldTask = g_audioFlowTasks.find(deviceId);
	auto predecessor = oldTask == g_audioFlowTasks.end() ? nullptr : oldTask->second;
	bool workerStarted = true;
	if (!flow || flow->done) {
		try {
			flow = AudioFlow::Start(deviceId, [id = deviceId, attempt = retired.attemptId](auto const& text) { RecordConnectionDiagnostic(attempt, id, text); }, true, false);
		} catch (...) {
			flow = std::make_shared<AudioFlow::State>();
			workerStarted = false;
			RecordConnectionDiagnostic(retired.attemptId, deviceId, L"audio-flow cleanup launch failed");
		}
	}
	flow->cleanup = cleanAudio;
	AudioFlow::RequestStop(*flow);
	g_audioFlowTasks[deviceId] = flow;
	CloseSessionInBackground(std::move(retired), deviceId, flow, workerStarted, predecessor);
	if (markRecoveryNeeded && wasConnected && !HasConnectedSessions())
		g_app.bluetooth.needed = true;
	return wasConnected;
}

bool PrepareForcedConnection(std::wstring const& deviceId)
{
	auto it = g_app.sessions.find(deviceId);
	if (it == g_app.sessions.end())
		return true;
	if (it->second.phase != ConnectionPhase::Connected)
		return false;
	// Forced entry point owns teardown. No user-disconnect, forget, or endpoint cycle first.
	CloseConnectionSession(deviceId, std::nullopt, false, true);
	return true;
}

void DisconnectDevice(std::wstring const& deviceId)
{
	if (IsStopping())
		return;

	auto previous = g_app.sessions.find(deviceId);
	if (previous != g_app.sessions.end())
		g_debugLastDisconnect[deviceId] = { previous->second.attemptId, previous->second.generation, GetTickCount64() };
	++g_app.connectGenerations[deviceId];
	g_app.connectionQueue.erase(
		std::remove_if(g_app.connectionQueue.begin(), g_app.connectionQueue.end(),
			[&deviceId](const ConnectionRequest& request) { return request.deviceId == deviceId; }),
		g_app.connectionQueue.end());
	g_app.pendingAutoReconnects.erase(deviceId);
	g_app.connectionQueueChanged.SetEvent();
	ForgetDevice(deviceId);
	CloseConnectionSession(deviceId, std::nullopt, false);
	ClearDeviceError(deviceId);
	UpdateDeviceRowStatus(deviceId, L"", _(L"Quick Connect"), true);
	ScheduleDeviceReconciliation();
	RecordDiagnostic(L"connection", L"user disconnected a device");
}

std::wstring FormatUserErrorMessage(HRESULT code)
{
	std::wstring message = _(L"Unknown error");
	message += L" (";
	message += FormatDiagnosticHresult(code);
	message += L")";
	return message;
}

bool IsCurrentConnectionSession(std::wstring const& deviceId, uint64_t generation, AudioPlaybackConnection connection = nullptr)
{
	auto session = g_app.sessions.find(deviceId);
	if (session == g_app.sessions.end() || session->second.generation != generation)
		return false;
	return !connection || session->second.connection == connection;
}

winrt::fire_and_forget BeginUnexpectedDisconnectRecovery(
	std::wstring deviceId,
	uint64_t attemptId,
	int64_t connectedDuration)
{
	auto dispatcher = g_uiDispatcher;
	auto generation = g_app.connectGenerations[deviceId];
	BluetoothRecoveryResult result = BluetoothRecoveryResult::UnexpectedFailure;
	HRESULT recoveryError = S_OK;
	try {
		result = static_cast<BluetoothRecoveryResult>(co_await EnsureBluetoothReady(true, true, deviceId));
	} catch (...) {
		recoveryError = static_cast<HRESULT>(winrt::to_hresult());
		result = BluetoothRecoveryResult::UnexpectedFailure;
	}

	try {
		co_await winrt::resume_foreground(dispatcher);
	} catch (...) {
		co_return;
	}

	g_app.unexpectedDisconnectRecoveryPending = false;
	g_app.activityChanged.SetEvent();
	RecordConnectionDiagnostic(
		attemptId,
		deviceId,
		L"disconnect=unexpected auto-radio-recovery=" + std::wstring(BluetoothRecoveryResultName(result)) + L" recovery-hr=" + FormatDiagnosticHresult(recoveryError) + L" duration-ms=" + std::to_wstring(connectedDuration));

	if (IsStopping())
		co_return;

	if (result == BluetoothRecoveryResult::Success)
		ClearDeviceError(deviceId);
	else
		SetDeviceError(deviceId, _(L"Unable to restart Bluetooth"), attemptId);

	for (const auto& row : g_deviceRows)
		ApplySessionStatusToRow(row.first);
	if (result == BluetoothRecoveryResult::Success && g_app.connectGenerations[deviceId] == generation && g_app.reconnectEnabled && IsDesiredDevice(deviceId)) {
		g_app.pendingAutoReconnects.insert(deviceId);
		auto found = g_discoveredDevices.find(deviceId);
		if (found != g_discoveredDevices.end())
			MaybeQueuePendingReconnect(found->second);
	}
	ScheduleDeviceReconciliation();
	RefreshEmptyState();
}

void HandleConnectionClosed(std::wstring deviceId, uint64_t generation, AudioPlaybackConnection sender)
{
	if (IsStopping())
		return;

	try {
		(void)g_uiDispatcher.RunAsync(
			winrt::Windows::UI::Core::CoreDispatcherPriority::High,
			[deviceId = std::move(deviceId), generation, sender]() {
				if (IsStopping())
					return;

				auto session = g_app.sessions.find(deviceId);
				if (session == g_app.sessions.end() || session->second.generation != generation || session->second.connection != sender) {
					return;
				}

				// The callback can be queued during OpenAsync and delivered only after
				// the session has opened. Do not reset an already-reopened connection.
				try {
					if (sender.State() != AudioPlaybackConnectionState::Closed)
						return;
				} catch (...) {
					// The event already reported Closed. A disposed sender can make
					// the follow-up query fail; retain the original observation.
				}

				auto attemptId = session->second.attemptId;
				auto phase = session->second.phase;
				if (phase != ConnectionPhase::Connected) {
					RecordConnectionDiagnostic(
						attemptId,
						deviceId,
						L"closed observed during phase=" + std::wstring(ConnectionPhaseName(phase)) + L"; connection attempt retains cleanup ownership");
					return;
				}

				auto connectedDuration = ElapsedMilliseconds(session->second.connectedAt);
				if (g_app.unexpectedDisconnectRecoveryPending || g_app.bluetooth.inProgress) {
					CloseConnectionSession(deviceId, generation, false);
					ClearDeviceError(deviceId);
					ApplySessionStatusToRow(deviceId);
					RecordConnectionDiagnostic(
						attemptId,
						deviceId,
						L"disconnect=recovery-induced auto-radio-recovery=already-running duration-ms=" + std::to_wstring(connectedDuration));
					return;
				}

				// The user-disconnect path revokes this handler before calling Close().
				// Reaching this branch therefore means that Windows or the remote device
				// closed an established connection. A targeted link close proved unable
				// to clear Windows' stale aggregate device state, so deterministically
				// reset the Bluetooth adapter before the user's next connection.
				g_app.unexpectedDisconnectRecoveryPending = true;
				g_app.bluetooth.deviceId = deviceId;
				g_app.bluetooth.needed = true;
				g_app.connectionQueue.clear();
				g_app.connectionQueueChanged.SetEvent();
				g_app.pendingAutoReconnects.clear();
				for (auto& item : g_app.connectGenerations)
					++item.second;

				std::vector<std::wstring> sessionIds;
				sessionIds.reserve(g_app.sessions.size());
				for (const auto& item : g_app.sessions)
					sessionIds.push_back(item.first);
				for (const auto& sessionId : sessionIds)
					CloseConnectionSession(sessionId, std::nullopt, false);

				ClearDeviceError(deviceId);
				for (const auto& row : g_deviceRows)
					ApplySessionStatusToRow(row.first);
				RecordConnectionDiagnostic(
					attemptId,
					deviceId,
					L"disconnect=unexpected auto-radio-recovery=started active-sessions-closed=" + std::to_wstring(sessionIds.size()) + L" duration-ms=" + std::to_wstring(connectedDuration));
				BeginUnexpectedDisconnectRecovery(
					deviceId,
					attemptId,
					connectedDuration);
			});
	} catch (...) {
		if (!IsStopping())
			LOG_CAUGHT_EXCEPTION();
	}
}

winrt::Windows::Foundation::IAsyncOperation<int32_t> ConnectDeviceAsync(ConnectionRequest request)
{
	auto dispatcher = g_uiDispatcher;
	co_await winrt::resume_foreground(dispatcher);

	auto const& deviceId = request.deviceId;
	auto attemptStarted = std::chrono::steady_clock::now();
	auto attemptDeadline = attemptStarted + std::chrono::milliseconds(CONNECTION_ATTEMPT_TIMEOUT_MS);
	auto attemptCancellation = std::make_shared<AsyncCancellation>();
	if (deviceId.empty() || IsStopping() || g_app.connectGenerations[deviceId] != request.generation || g_app.sessions.count(deviceId) != 0) {
		co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
	}

	auto device = request.device;
	if (device)
		UpsertDeviceRow(device);

	ConnectionSession newSession;
	newSession.cancellation = attemptCancellation;
	newSession.device = device;
	newSession.generation = request.generation;
	newSession.attemptId = request.attemptId;
	newSession.retryCount = request.retryCount;
	newSession.requestMode = request.mode;
	newSession.phase = request.recoveryStrategy == ConnectionRecoveryStrategy::Direct
		? ConnectionPhase::Starting
		: ConnectionPhase::RecoveringBluetooth;
	g_app.sessions.emplace(deviceId, std::move(newSession));
	ApplySessionStatusToRow(deviceId);

	RecordConnectionDiagnostic(
		request.attemptId,
		deviceId,
		L"attempt-start mode=" + std::wstring(ConnectionRequestModeName(request.mode)) + L" try=" + std::to_wstring(request.retryCount + 1) + L" recovery-strategy=" + std::wstring(ConnectionRecoveryStrategyName(request.recoveryStrategy)));

	// Serialize the old generation's restore/cleanup before touching the new link.
	auto previousFlow = g_audioFlowTasks.find(deviceId);
	auto previousTask = previousFlow == g_audioFlowTasks.end() ? nullptr : previousFlow->second;
	if (previousTask) {
		co_await AwaitBounded(AudioFlow::WaitFinished(previousTask), attemptDeadline, attemptCancellation);
		co_await winrt::resume_foreground(dispatcher);
		if (!IsCurrentConnectionSession(deviceId, request.generation) || IsStopping())
			co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
	}
	if (previousTask)
		g_audioFlowTasks.erase(deviceId);
	if (request.mode != ConnectionRequestMode::Forced || request.retryCount != 0)
		request.recoveryStrategy = ConnectionRecoveryStrategy::Direct;

	auto recoveryStarted = std::chrono::steady_clock::now();
	auto recoveryResult = BluetoothRecoveryResult::NotRequired;
	if (request.recoveryStrategy == ConnectionRecoveryStrategy::Direct) {
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"Bluetooth recovery skipped for direct connection");
	} else {
		auto allowActiveSessionReset = request.mode == ConnectionRequestMode::Forced;
		recoveryResult = static_cast<BluetoothRecoveryResult>(co_await EnsureBluetoothReady(
			true,
			allowActiveSessionReset, deviceId, attemptCancellation, attemptDeadline));
	}
	co_await winrt::resume_foreground(dispatcher);
	RecordConnectionDiagnostic(
		request.attemptId,
		deviceId,
		L"recovery-result=" + std::wstring(BluetoothRecoveryResultName(recoveryResult)) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(recoveryStarted)));

	if (!IsCurrentConnectionSession(deviceId, request.generation) || IsStopping()) {
		CloseConnectionSession(deviceId, request.generation, false);
		RecordConnectionDiagnostic(request.attemptId, deviceId, L"attempt cancelled after recovery");
		co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
	}

	if (recoveryResult != BluetoothRecoveryResult::Success && recoveryResult != BluetoothRecoveryResult::NotRequired) {
		CloseConnectionSession(deviceId, request.generation, false);
		SetDeviceError(deviceId, _(L"Unable to restart Bluetooth"), request.attemptId);
		ApplySessionStatusToRow(deviceId);
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"attempt-complete result=bluetooth-recovery-failed recovery-result=" + std::wstring(BluetoothRecoveryResultName(recoveryResult)) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(attemptStarted)));
		co_return static_cast<int32_t>(ConnectionAttemptResult::BluetoothRecoveryFailed);
	}

	// A radio power cycle invalidates the device-interface snapshot that initiated
	// the attempt. Resolve it again before TryCreateFromId so the retry never relies
	// on a stale watcher object.
	if (!device || recoveryResult == BluetoothRecoveryResult::Success || request.recoveryStrategy == ConnectionRecoveryStrategy::RestartBluetooth) {
		auto lookupStarted = std::chrono::steady_clock::now();
		RecordConnectionDiagnostic(request.attemptId, deviceId, L"device re-resolution requested");
		auto previousSnapshot = device;
		DeviceInformation refreshedDevice { nullptr };
		auto lookupDeadline = (std::min)(attemptDeadline, std::chrono::steady_clock::now() + std::chrono::seconds(3));
		for (uint32_t lookupTry = 1; !refreshedDevice && std::chrono::steady_clock::now() < lookupDeadline; ++lookupTry) {
			g_app.deviceChanged.ResetEvent();
			try {
				refreshedDevice = co_await AwaitBounded(DeviceInformation::CreateFromIdAsync(deviceId), lookupDeadline, attemptCancellation);
			} catch (winrt::hresult_error const& error) {
				if (error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || error.code() == HRESULT_FROM_WIN32(ERROR_CANCELLED) || error.code() == E_ABORT)
					throw;
				RecordConnectionDiagnostic(
					request.attemptId,
					deviceId,
					L"device re-resolution try=" + std::to_wstring(lookupTry) + L" exception hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(error.code())));
				LOG_CAUGHT_EXCEPTION();
			} catch (...) {
				auto error = static_cast<HRESULT>(winrt::to_hresult());
				RecordConnectionDiagnostic(
					request.attemptId,
					deviceId,
					L"device re-resolution try=" + std::to_wstring(lookupTry) + L" exception hr=" + FormatDiagnosticHresult(error));
				LOG_CAUGHT_EXCEPTION();
			}

			co_await winrt::resume_foreground(dispatcher);
			if (IsStopping() || g_app.connectGenerations[deviceId] != request.generation)
				break;
			if (!refreshedDevice && std::chrono::steady_clock::now() < lookupDeadline) {
				auto delay = (std::min)(std::chrono::milliseconds(aulay::timing::RetryDelayMs(lookupTry - 1)),
					std::chrono::ceil<std::chrono::milliseconds>(lookupDeadline - std::chrono::steady_clock::now()));
				co_await winrt::resume_on_signal(g_app.deviceChanged.get(), delay);
				co_await winrt::resume_foreground(dispatcher);
			}
		}

		if (refreshedDevice)
			device = refreshedDevice;
		else
			device = previousSnapshot;
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"device re-resolution found=" + std::wstring(refreshedDevice ? L"true" : L"false") + L" retained-previous-snapshot=" + (device && !refreshedDevice ? L"true" : L"false") + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(lookupStarted)));
	}

	if (!IsCurrentConnectionSession(deviceId, request.generation) || IsStopping()) {
		CloseConnectionSession(deviceId, request.generation, false);
		co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
	}

	if (device)
		UpsertDeviceRow(device);
	auto session = g_app.sessions.find(deviceId);
	session->second.device = device;
	session->second.phase = ConnectionPhase::Starting;
	ApplySessionStatusToRow(deviceId);

	AudioPlaybackConnection currentConnection { nullptr };
	ConnectionAttemptResult result = ConnectionAttemptResult::UnexpectedFailure;
	std::wstring errorMessage;
	std::wstring failureStage = L"try-create";

	try {
		auto createStarted = std::chrono::steady_clock::now();
		co_await winrt::resume_background();
		currentConnection = AudioPlaybackConnection::TryCreateFromId(deviceId);
		co_await winrt::resume_foreground(dispatcher);
		if (!IsCurrentConnectionSession(deviceId, request.generation) || IsStopping()) {
			co_await winrt::resume_background();
			if (currentConnection)
				currentConnection.Close();
			co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
		}
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"try-create created=" + std::wstring(currentConnection ? L"true" : L"false") + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(createStarted)));
		if (!currentConnection) {
			result = ConnectionAttemptResult::DeviceUnavailable;
			errorMessage = _(L"The device is unavailable");
		} else {
			session = g_app.sessions.find(deviceId);
			session->second.connection = currentConnection;
			auto openedSignal = std::make_shared<wil::unique_event>(wil::EventOptions::ManualReset);
			session->second.openedSignal = openedSignal;
			auto stateChangedToken = currentConnection.StateChanged(
				[deviceId, generation = request.generation, attemptId = request.attemptId, openedSignal](const auto& sender, const auto&) {
					try {
						auto state = sender.State();
						if (state == AudioPlaybackConnectionState::Opened)
							openedSignal->SetEvent();
						RecordConnectionDiagnostic(
							attemptId,
							deviceId,
							L"state-changed state=" + std::wstring(AudioConnectionStateName(state)));
						if (state == AudioPlaybackConnectionState::Closed)
							HandleConnectionClosed(deviceId, generation, sender);
					} catch (...) {
						if (!IsStopping()) {
							auto error = static_cast<HRESULT>(winrt::to_hresult());
							RecordConnectionDiagnostic(
								attemptId,
								deviceId,
								L"StateChanged query exception hr=" + FormatDiagnosticHresult(error));
							LOG_CAUGHT_EXCEPTION();
						}
					}
				});
			session->second.stateChangedToken = stateChangedToken;
			session->second.hasStateChangedToken = true;

			failureStage = L"start";
			result = ConnectionAttemptResult::StartFailed;
			auto startStarted = std::chrono::steady_clock::now();
			RecordConnectionDiagnostic(request.attemptId, deviceId, L"StartAsync requested");
			CheckAsyncDeadline(attemptDeadline, attemptCancellation);
			co_await AwaitBounded(StartConnectionInBackground(currentConnection), attemptDeadline, attemptCancellation);
			co_await winrt::resume_foreground(dispatcher);
			RecordConnectionDiagnostic(
				request.attemptId,
				deviceId,
				L"StartAsync completed state=" + std::wstring(AudioConnectionStateName(currentConnection.State())) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(startStarted)));
			if (!IsCurrentConnectionSession(deviceId, request.generation, currentConnection) || IsStopping()) {
				auto cancelled = IsStopping() || g_app.connectGenerations[deviceId] != request.generation;
				CloseConnectionSession(deviceId, request.generation, false);
				co_return static_cast<int32_t>(cancelled
						? ConnectionAttemptResult::Cancelled
						: ConnectionAttemptResult::ClosedUnexpectedly);
			}

			session = g_app.sessions.find(deviceId);
			session->second.phase = ConnectionPhase::Opening;
			ApplySessionStatusToRow(deviceId);
			failureStage = L"open";
			result = ConnectionAttemptResult::OpenFailed;
			auto openStarted = std::chrono::steady_clock::now();
			RecordConnectionDiagnostic(request.attemptId, deviceId, L"OpenAsync requested");
			openedSignal->ResetEvent();
			CheckAsyncDeadline(attemptDeadline, attemptCancellation);
			auto openResult = co_await AwaitBounded(OpenConnectionInBackground(currentConnection), attemptDeadline, attemptCancellation);
			co_await winrt::resume_foreground(dispatcher);
			auto openStatus = openResult.Status();
			auto extendedError = static_cast<HRESULT>(openResult.ExtendedError());
			auto stateAfterOpen = currentConnection.State();
			RecordConnectionDiagnostic(
				request.attemptId,
				deviceId,
				L"OpenAsync completed status=" + std::wstring(AudioOpenStatusName(openStatus)) + L" extended-error=" + FormatDiagnosticHresult(extendedError) + L" state=" + std::wstring(AudioConnectionStateName(stateAfterOpen)) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(openStarted)));
			if (!IsCurrentConnectionSession(deviceId, request.generation, currentConnection) || IsStopping()) {
				auto cancelled = IsStopping() || g_app.connectGenerations[deviceId] != request.generation;
				CloseConnectionSession(deviceId, request.generation, false);
				co_return static_cast<int32_t>(cancelled
						? ConnectionAttemptResult::Cancelled
						: ConnectionAttemptResult::ClosedUnexpectedly);
			}

			switch (openStatus) {
			case AudioPlaybackConnectionOpenResultStatus::Success: {
				session = g_app.sessions.find(deviceId);
				session->second.phase = ConnectionPhase::WaitingForOpened;
				ApplySessionStatusToRow(deviceId);

				auto openedWaitStarted = std::chrono::steady_clock::now();
				auto observedState = stateAfterOpen;
				RecordConnectionDiagnostic(
					request.attemptId,
					deviceId,
					L"opened-state wait started initial-state=" + std::wstring(AudioConnectionStateName(observedState)) + L" timeout-ms=" + std::to_wstring(CONNECTION_OPENED_WAIT_MS));

				if (observedState != AudioPlaybackConnectionState::Opened) {
					CheckAsyncDeadline(attemptDeadline, attemptCancellation);
					auto remaining = attemptDeadline - std::chrono::steady_clock::now();
					if (remaining <= std::chrono::steady_clock::duration::zero())
						winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
					auto openedWait = (std::min)(std::chrono::milliseconds(CONNECTION_OPENED_WAIT_MS),
						std::chrono::ceil<std::chrono::milliseconds>(remaining));
					co_await winrt::resume_on_signal(openedSignal->get(), openedWait);
					co_await winrt::resume_foreground(dispatcher);
					if (!IsCurrentConnectionSession(deviceId, request.generation, currentConnection) || IsStopping()) {
						CloseConnectionSession(deviceId, request.generation, false);
						co_return static_cast<int32_t>(ConnectionAttemptResult::Cancelled);
					}
					observedState = currentConnection.State();
				}

				RecordConnectionDiagnostic(
					request.attemptId,
					deviceId,
					L"opened-state wait completed state=" + std::wstring(AudioConnectionStateName(observedState)) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(openedWaitStarted)));

				if (observedState != AudioPlaybackConnectionState::Opened) {
					result = ConnectionAttemptResult::ClosedUnexpectedly;
					errorMessage = _(L"The connection closed unexpectedly");
					break;
				}

				// OpenAsync/Opened confirms the link, not delivery of audio frames.
				// The independent flow worker verifies supply without delaying success.
				result = ConnectionAttemptResult::Success;
				break;
			}
			case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
				result = ConnectionAttemptResult::OpenTimedOut;
				errorMessage = _(L"The request timed out");
				break;
			case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
				result = ConnectionAttemptResult::DeniedBySystem;
				errorMessage = _(L"The operation was denied by the system");
				break;
			case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
				result = ConnectionAttemptResult::OpenFailed;
				if (FAILED(extendedError))
					errorMessage = FormatUserErrorMessage(extendedError);
				else
					errorMessage = _(L"Unknown error");
				break;
			default:
				result = ConnectionAttemptResult::OpenFailed;
				errorMessage = _(L"Unknown error");
				break;
			}
		}
	} catch (winrt::hresult_error const& error) {
		auto value = static_cast<HRESULT>(error.code());
		if (value == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
			result = ConnectionAttemptResult::DeadlineExceeded;
		else if (value == HRESULT_FROM_WIN32(ERROR_CANCELLED) || value == E_ABORT)
			result = ConnectionAttemptResult::Cancelled;
		errorMessage = FormatUserErrorMessage(value);
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"stage=" + failureStage + L" exception hr=" + FormatDiagnosticHresult(value));
		LOG_CAUGHT_EXCEPTION();
	} catch (...) {
		auto value = static_cast<HRESULT>(winrt::to_hresult());
		errorMessage = _(L"Unknown error");
		errorMessage += L" (" + FormatDiagnosticHresult(value) + L")";
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"stage=" + failureStage + L" exception hr=" + FormatDiagnosticHresult(value));
		LOG_CAUGHT_EXCEPTION();
	}

	co_await winrt::resume_foreground(dispatcher);
	if (!IsCurrentConnectionSession(deviceId, request.generation, currentConnection) || IsStopping()) {
		auto cancelled = IsStopping() || g_app.connectGenerations[deviceId] != request.generation;
		CloseConnectionSession(deviceId, request.generation, false);
		co_return static_cast<int32_t>(cancelled
				? ConnectionAttemptResult::Cancelled
				: ConnectionAttemptResult::ClosedUnexpectedly);
	}

	// The final foreground hop may have delivered a Closed callback while this
	// attempt still owned cleanup. Re-read before committing the success state.
	if (result == ConnectionAttemptResult::Success) {
		try {
			if (currentConnection.State() != AudioPlaybackConnectionState::Opened)
				result = ConnectionAttemptResult::ClosedUnexpectedly;
		} catch (...) {
			result = ConnectionAttemptResult::ClosedUnexpectedly;
		}
		if (result != ConnectionAttemptResult::Success)
			errorMessage = _(L"The connection closed unexpectedly");
	}

	if (result == ConnectionAttemptResult::Success) {
		auto currentSession = g_app.sessions.find(deviceId);
		currentSession->second.phase = ConnectionPhase::Connected;
		currentSession->second.connectedAt = std::chrono::steady_clock::now();
		try {
			auto id = deviceId;
			auto attempt = request.attemptId;
			auto task = AudioFlow::Start(id, [id, attempt](auto const& text) { RecordConnectionDiagnostic(attempt, id, text); });
			currentSession->second.audioFlow = task;
			g_audioFlowTasks[id] = task;
		} catch (...) {
			RecordConnectionDiagnostic(request.attemptId, deviceId, L"audio-flow monitor launch failed");
		}

		g_app.bluetooth.needed = false;
		ClearDeviceError(deviceId);
		RememberDevice(deviceId);
		g_app.pendingAutoReconnects.erase(deviceId);
		ApplySessionStatusToRow(deviceId);
		RecordConnectionDiagnostic(
			request.attemptId,
			deviceId,
			L"attempt-complete result=success try=" + std::to_wstring(request.retryCount + 1) + L" duration-ms=" + std::to_wstring(ElapsedMilliseconds(attemptStarted)));

		RecordDebugConnectionSnapshot(deviceId, L"OPENED_BASELINE", 0, true);
		co_return static_cast<int32_t>(ConnectionAttemptResult::Success);
	}

	if (errorMessage.empty())
		errorMessage = _(L"Unknown error");
	CloseConnectionSession(deviceId, request.generation, false);
	g_app.bluetooth.needed = true;
	SetDeviceError(deviceId, std::move(errorMessage), request.attemptId);
	ApplySessionStatusToRow(deviceId);
	ScheduleDeviceReconciliation();
	RecordConnectionDiagnostic(
		request.attemptId,
		deviceId,
		L"attempt-complete result=" + std::wstring(ConnectionAttemptResultName(result)) + L" try=" + std::to_wstring(request.retryCount + 1) + L" recovery-needed=true duration-ms=" + std::to_wstring(ElapsedMilliseconds(attemptStarted)));
	co_return static_cast<int32_t>(result);
}

bool IsConnectionQueued(std::wstring const& deviceId)
{
	return std::find_if(g_app.connectionQueue.begin(), g_app.connectionQueue.end(),
			   [&deviceId](const ConnectionRequest& request) { return request.deviceId == deviceId; })
		!= g_app.connectionQueue.end();
}

bool IsRetryableConnectionResult(ConnectionAttemptResult result)
{
	return result != ConnectionAttemptResult::Success && result != ConnectionAttemptResult::Cancelled;
}

void QueueConnectionInternal(
	std::wstring const& deviceId,
	DeviceInformation device,
	ConnectionRequestMode mode,
	uint32_t retryCount,
	std::optional<uint64_t> existingGeneration = std::nullopt,
	std::optional<uint64_t> existingAttemptId = std::nullopt,
	std::optional<ConnectionRecoveryStrategy> recoveryStrategy = std::nullopt,
	std::chrono::steady_clock::time_point notBefore = {})
{
	if (deviceId.empty() || IsStopping())
		return;
	// Manual requests issued while Bluetooth recovery owns the adapter are
	// queued and start once it completes; user intent is never dropped
	// silently. Automatic requests keep their defer semantics: the recovery
	// completion path re-issues them only when reconnect is still wanted.
	if ((g_app.unexpectedDisconnectRecoveryPending || g_app.bluetooth.inProgress) && mode == ConnectionRequestMode::Automatic)
		return;
	if (mode == ConnectionRequestMode::Forced && !PrepareForcedConnection(deviceId))
		return;
	if (g_app.sessions.count(deviceId) != 0)
		return;

	// Consume discovery-triggered reconnect once; the single live retry chain owns it.
	g_app.pendingAutoReconnects.erase(deviceId);

	for (auto& queued : g_app.connectionQueue) {
		if (queued.deviceId != deviceId)
			continue;

		if (device)
			queued.device = device;
		if (mode != ConnectionRequestMode::Automatic && !existingAttemptId) {
			queued.mode = mode;
			queued.recoveryStrategy = SelectInitialRecoveryStrategy(mode);
			queued.notBefore = {};
			queued.generation = ++g_app.connectGenerations[deviceId];
			queued.attemptId = ++g_app.nextConnectionAttemptId;
			queued.retryCount = 0;
			ClearDeviceError(deviceId);
			ApplySessionStatusToRow(deviceId);
			g_app.connectionQueueChanged.SetEvent();
		}
		return;
	}

	auto generation = existingGeneration ? *existingGeneration : ++g_app.connectGenerations[deviceId];
	auto attemptId = existingAttemptId ? *existingAttemptId : ++g_app.nextConnectionAttemptId;
	auto resolvedRecoveryStrategy = recoveryStrategy.value_or(SelectInitialRecoveryStrategy(mode));
	if (!existingAttemptId) {
		ClearDeviceError(deviceId);
		ApplySessionStatusToRow(deviceId);
	}

	ConnectionRequest request;
	request.deviceId = deviceId;
	request.device = device;
	request.generation = generation;
	request.attemptId = attemptId;
	request.retryCount = retryCount;
	request.mode = mode;
	request.recoveryStrategy = resolvedRecoveryStrategy;
	request.notBefore = notBefore;
	RecordConnectionDiagnostic(
		attemptId,
		deviceId,
		L"request-queued mode=" + std::wstring(ConnectionRequestModeName(mode)) + L" try=" + std::to_wstring(retryCount + 1) + L" recovery-strategy=" + std::wstring(ConnectionRecoveryStrategyName(resolvedRecoveryStrategy)));
	g_app.connectionQueue.push_back(std::move(request));
	ApplySessionStatusToRow(deviceId);
	g_app.connectionQueueChanged.SetEvent();

	if (!g_app.connectionWorkerRunning) {
		g_app.connectionWorkerRunning = true;
		ProcessConnectionQueue();
	}
}

void QueueConnection(DeviceInformation const& device, ConnectionRequestMode mode)
{
	if (!device)
		return;
	QueueConnectionInternal(std::wstring(device.Id()), device, mode, 0);
}

void QueueConnection(std::wstring const& deviceId, ConnectionRequestMode mode)
{
	QueueConnectionInternal(deviceId, DeviceInformation { nullptr }, mode, 0);
}

auto FindNextConnectionRequest()
{
	return std::min_element(g_app.connectionQueue.begin(), g_app.connectionQueue.end(),
		[](ConnectionRequest const& left, ConnectionRequest const& right) {
			return left.notBefore < right.notBefore;
		});
}

winrt::fire_and_forget ProcessConnectionQueue()
{
	try {
		auto dispatcher = g_uiDispatcher;
		co_await winrt::resume_foreground(dispatcher);

		while (!g_app.connectionQueue.empty() && !IsStopping()) {
			// Keep deferred retries in the queue, so a new click can wake and
			// overtake them. Reset/re-evaluate on the UI thread to avoid lost wakes.
			g_app.connectionQueueChanged.ResetEvent();
			if (g_app.unexpectedDisconnectRecoveryPending || g_app.bluetooth.inProgress) {
				// Recovery owns the adapter; hold queued requests until it
				// completes. Both flags end with an activityChanged signal.
				co_await winrt::resume_on_signal(g_app.activityChanged.get());
				co_await winrt::resume_foreground(dispatcher);
				continue;
			}
			auto next = FindNextConnectionRequest();
			auto now = std::chrono::steady_clock::now();
			if (next->notBefore > now) {
				auto delay = std::chrono::ceil<std::chrono::milliseconds>(next->notBefore - now);
				co_await winrt::resume_on_signal(g_app.connectionQueueChanged.get(), delay);
				co_await winrt::resume_foreground(dispatcher);
				continue;
			}
			auto request = std::move(*next);
			g_app.connectionQueue.erase(next);

			if (g_app.connectGenerations[request.deviceId] != request.generation || g_app.sessions.count(request.deviceId) != 0) {
				RecordConnectionDiagnostic(request.attemptId, request.deviceId, L"queued request discarded as stale");
				continue;
			}

			if (IsStopping() || g_app.connectGenerations[request.deviceId] != request.generation)
				break;

			auto result = ConnectionAttemptResult::UnexpectedFailure;
			HRESULT attemptError = S_OK;
			try {
				result = static_cast<ConnectionAttemptResult>(co_await ConnectDeviceAsync(request));
			} catch (...) {
				attemptError = static_cast<HRESULT>(winrt::to_hresult());
			}
			co_await winrt::resume_foreground(dispatcher);
			if (FAILED(attemptError)) {
				if (attemptError == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
					result = ConnectionAttemptResult::DeadlineExceeded;
				// Failures before ConnectDeviceAsync's Open/Start try block must
				// release the placeholder session too; otherwise the row stays busy.
				CloseConnectionSession(request.deviceId, request.generation, false);
				if (IsStopping() || g_app.connectGenerations[request.deviceId] != request.generation)
					result = ConnectionAttemptResult::Cancelled;
				else {
					SetDeviceError(request.deviceId,
						std::wstring(_(L"Unknown error")) + L" (" + FormatDiagnosticHresult(attemptError) + L")",
						request.attemptId);
					ApplySessionStatusToRow(request.deviceId);
				}
				RecordConnectionDiagnostic(request.attemptId, request.deviceId,
					L"attempt exception cleaned up hr=" + FormatDiagnosticHresult(attemptError));
			}

			if (result != ConnectionAttemptResult::Success && result != ConnectionAttemptResult::Cancelled)
				g_app.bluetooth.needed = true;

			if (IsRetryableConnectionResult(result) && !IsStopping() && g_app.connectGenerations[request.deviceId] == request.generation && !IsConnectionQueued(request.deviceId) && g_app.sessions.count(request.deviceId) == 0) {
				auto nextStrategy = SelectRetryRecoveryStrategy(request, result);
				auto retryNotBefore = std::chrono::steady_clock::now() + std::chrono::milliseconds(aulay::timing::RetryDelayMs(request.retryCount));
				RecordConnectionDiagnostic(
					request.attemptId,
					request.deviceId,
					L"scheduling retry strategy=" + std::wstring(ConnectionRecoveryStrategyName(nextStrategy)) + L" delay-ms=" + std::to_wstring(aulay::timing::RetryDelayMs(request.retryCount)) + L" after result=" + std::wstring(ConnectionAttemptResultName(result)));
				QueueConnectionInternal(
					request.deviceId,
					DeviceInformation { nullptr },
					request.mode,
					(std::min)(request.retryCount, UINT32_MAX - 1) + 1,
					request.generation,
					request.attemptId,
					nextStrategy,
					retryNotBefore);
			} else if (result != ConnectionAttemptResult::Success && result != ConnectionAttemptResult::Cancelled) {
				RecordConnectionDiagnostic(
					request.attemptId,
					request.deviceId,
					L"no further retry result=" + std::wstring(ConnectionAttemptResultName(result)) + L" retries-used=" + std::to_wstring(request.retryCount));
			}
		}

	} catch (...) {
		auto error = static_cast<HRESULT>(winrt::to_hresult());
		RecordDiagnostic(L"connection", L"connection worker exception hr=" + FormatDiagnosticHresult(error));
		LOG_CAUGHT_EXCEPTION();
	}

	try {
		co_await winrt::resume_foreground(g_uiDispatcher);
		g_app.connectionWorkerRunning = false;
		g_app.activityChanged.SetEvent();
		if (!g_app.connectionQueue.empty() && !IsStopping()) {
			g_app.connectionWorkerRunning = true;
			ProcessConnectionQueue();
		}
	} catch (...) {
		LOG_CAUGHT_EXCEPTION();
	}
}

void QueueRememberedDevices()
{
	if (IsStopping() || !g_app.reconnectEnabled)
		return;

	for (const auto& deviceId : g_app.desiredDevices) {
		if (deviceId.empty())
			continue;
		g_app.pendingAutoReconnects.insert(deviceId);
		auto discovered = g_discoveredDevices.find(deviceId);
		if (discovered != g_discoveredDevices.end())
			MaybeQueuePendingReconnect(discovered->second);
	}
}

void MaybeQueuePendingReconnect(DeviceInformation const& device)
{
	if (!device || !g_app.reconnectEnabled || IsStopping())
		return;

	auto deviceId = std::wstring(device.Id());
	if (g_app.pendingAutoReconnects.count(deviceId) == 0 || g_app.sessions.count(deviceId) != 0 || IsConnectionQueued(deviceId)) {
		return;
	}

	QueueConnection(device, ConnectionRequestMode::Automatic);
}
