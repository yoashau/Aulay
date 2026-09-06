#pragma once

#include "resource.h"
#include "BuildVariant.hpp"
#include "AsyncUtil.hpp"
#include "ConnectionTiming.hpp"
#include "BluetoothRouting.hpp"
#include "AudioFlowRecovery.hpp"

using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Audio;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Windows::UI::Text;
namespace fs = std::filesystem;

constexpr UINT WM_NOTIFYICON = WM_APP + 1;
constexpr UINT WM_CONNECTDEVICE = WM_APP + 2;
constexpr UINT WM_FINISHSHUTDOWN = WM_APP + 3;
constexpr UINT WM_DISMISSDEVICEFLYOUT = WM_APP + 4;

HINSTANCE g_hInst;
HWND g_hWnd;
HWND g_hWndXaml;
Canvas g_xamlCanvas = nullptr;
winrt::Windows::UI::Core::CoreDispatcher g_uiDispatcher = nullptr;
Flyout g_xamlFlyout = nullptr;
MenuFlyout g_xamlMenu = nullptr;
FocusState g_menuFocusState = FocusState::Unfocused;
DeviceWatcher g_deviceWatcher = nullptr;
Flyout g_xamlDeviceFlyout = nullptr;
StackPanel g_deviceListPanel = nullptr;
TextBlock g_emptyStateText = nullptr;
HICON g_hTrayIcon = nullptr;
NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
NOTIFYICONIDENTIFIER g_niid = {
	.cbSize = sizeof(g_niid)
};
UINT WM_TASKBAR_CREATED = 0;

enum class ConnectionPhase
{
	RecoveringBluetooth,
	Starting,
	Opening,
	WaitingForOpened,
	Connected,
	Closing,
};

enum class BluetoothRecoveryStage
{
	Idle,
	RequestingAccess,
	FindingRadio,
	TurningOff,
	WaitingForOff,
	TurningOn,
	WaitingForOn,
	Succeeded,
	Failed,
};

enum class BluetoothRecoveryResult
{
	NotRequired,
	Success,
	AccessDenied,
	RadioUnavailable,
	TurnOffDenied,
	TurnOffTimedOut,
	TurnOnDenied,
	TurnOnTimedOut,
	UnexpectedFailure,
	OperationTimedOut,
	Cancelled,
};

enum class ConnectionAttemptResult
{
	Cancelled,
	Success,
	BluetoothRecoveryFailed,
	DeviceUnavailable,
	StartFailed,
	OpenTimedOut,
	DeniedBySystem,
	OpenFailed,
	ClosedUnexpectedly,
	UnexpectedFailure,
	DeadlineExceeded,
};

enum class ConnectionRequestMode
{
	Automatic,
	Quick,
	Forced,
};

enum class ConnectionRecoveryStrategy
{
	Direct,
	RestartBluetooth,
};

struct ConnectionSession
{
	std::shared_ptr<AudioFlow::State> audioFlow;
	DeviceInformation device{ nullptr };
	AudioPlaybackConnection connection{ nullptr };
	winrt::event_token stateChangedToken{};
	uint64_t generation = 0;
	uint64_t attemptId = 0;
	uint32_t retryCount = 0;
	ConnectionPhase phase = ConnectionPhase::RecoveringBluetooth;
	std::chrono::steady_clock::time_point connectedAt{};
	bool hasStateChangedToken = false;
	std::shared_ptr<wil::unique_event> openedSignal;
	std::shared_ptr<AsyncCancellation> cancellation;
	ConnectionRequestMode requestMode = ConnectionRequestMode::Automatic;
	uint32_t noSoundReports = 0;
};

struct ConnectionRequest
{
	std::wstring deviceId;
	DeviceInformation device{ nullptr };
	uint64_t generation = 0;
	uint64_t attemptId = 0;
	uint32_t retryCount = 0;
	ConnectionRequestMode mode = ConnectionRequestMode::Automatic;
	ConnectionRecoveryStrategy recoveryStrategy = ConnectionRecoveryStrategy::Direct;
	std::chrono::steady_clock::time_point notBefore{};
};

struct DeviceErrorState
{
	std::wstring message;
	uint64_t attemptId = 0;
};

struct DiagnosticState
{
	std::mutex mutex;
	std::deque<std::wstring> recentEntries;
	fs::path logDirectory;
	fs::path logPath;
	uint64_t privacySalt = 0;
	bool initialized = false;
};

struct BluetoothRecoveryCompletion
{
	wil::unique_event signal{ wil::EventOptions::ManualReset };
	BluetoothRecoveryResult result = BluetoothRecoveryResult::UnexpectedFailure;
};

struct BluetoothRecoveryState
{
	// A pending explicit or unexpected-disconnect reset, never a startup reset.
	bool needed = true;
	bool inProgress = false;
	bool lastSucceeded = false;
	uint64_t generation = 0;
	BluetoothRecoveryStage stage = BluetoothRecoveryStage::Idle;
	BluetoothRecoveryResult lastResult = BluetoothRecoveryResult::NotRequired;
	// The device whose request owns the running recovery; only its row reports
	// the restart while other rows keep their real state.
	std::wstring deviceId;
	std::shared_ptr<BluetoothRecoveryCompletion> completion;
	std::shared_ptr<AsyncCancellation> cancellation;
};

struct AppRuntimeState
{
	std::atomic_bool shutdownRequested{ false };
	std::atomic_bool shuttingDown{ false };
	bool reconnectEnabled = false;
	bool connectionWorkerRunning = false;
	bool watcherHandlersAttached = false;
	bool watcherRestartScheduled = false;
	uint32_t watcherRestartCount = 0;
	bool deviceEnumerationComplete = false;
	bool reconciliationRunning = false;
	bool reconciliationPending = false;
	uint64_t emptyStateGeneration = 0;
	uint64_t nextConnectionAttemptId = 0;
	BluetoothRecoveryState bluetooth;
	std::optional<winrt::Windows::Devices::Radios::RadioAccessStatus> radioAccess;
	std::unordered_map<std::wstring, ConnectionSession> sessions;
	std::unordered_map<std::wstring, uint64_t> connectGenerations;
	std::unordered_map<std::wstring, DeviceErrorState> deviceErrors;
	std::deque<ConnectionRequest> connectionQueue;
	wil::unique_event connectionQueueChanged{ wil::EventOptions::ManualReset };
	wil::unique_event deviceChanged{ wil::EventOptions::ManualReset };
	wil::unique_event activityChanged{ wil::EventOptions::ManualReset };
	wil::unique_event stoppingSignal{ wil::EventOptions::ManualReset };
	std::vector<std::wstring> desiredDevices;
	std::unordered_set<std::wstring> pendingAutoReconnects;
	bool unexpectedDisconnectRecoveryPending = false;
	bool deviceFlyoutVisible = false;
	winrt::event_token watcherAddedToken{};
	winrt::event_token watcherRemovedToken{};
	winrt::event_token watcherUpdatedToken{};
	winrt::event_token watcherEnumerationCompletedToken{};
	winrt::event_token watcherStoppedToken{};
	DiagnosticState diagnostic;
};

AppRuntimeState g_app;

// Defined in ConnectionState.hpp; declared here because the settings worker
// below runs on a background thread and checks it before resuming the UI.
bool IsStopping();

#include "Util.hpp"
#include "Diagnostics.hpp"
#include "I18n.hpp"
#include "SettingsUtil.hpp"
