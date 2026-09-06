#pragma once

// Cross-module forward declarations. The domain headers below carry no
// mutual include cycles; the declarations here cover the remaining
// call-across-boundary cases and the application shell in Aulay.cpp.
#include "Aulay.h"

// Session teardown is shared by the connection flow and Bluetooth recovery.
bool CloseConnectionSession(std::wstring const& deviceId, std::optional<uint64_t> generation, bool markRecoveryNeeded, bool radioWillReset = false);

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
void BeginShutdown();
void ShutdownApplication();
bool IsStopping();
bool IsRadioDeactivationGraceActive();
bool HasConnectedSessions();
bool HasConnectionActivity();
void QueueRememberedDevices();
void MaybeQueuePendingReconnect(DeviceInformation const& device);
void QueueConnection(DeviceInformation const& device, ConnectionRequestMode mode = ConnectionRequestMode::Quick);
void QueueConnection(std::wstring const& deviceId, ConnectionRequestMode mode = ConnectionRequestMode::Automatic);
void DisconnectDevice(std::wstring const& deviceId);
void SetupDeviceList();
void StartDeviceWatcher();
void StopDeviceWatcher();
void UpsertDeviceRow(DeviceInformation const& device);
void RemoveDeviceRow(std::wstring const& deviceId);
void UpdateDeviceRowStatus(std::wstring const& deviceId, std::wstring_view statusMsg, std::wstring_view buttonLabel, bool buttonEnabled, bool showForceAction = true, bool showNoSound = false);
void ApplySessionStatusToRow(std::wstring const& deviceId);
bool IsConnectionQueued(std::wstring const& deviceId);
void MarkNoSound(std::wstring const& deviceId);
bool PrepareForcedConnection(std::wstring const& deviceId);
void RefreshEmptyState();
void ScheduleDeviceReconciliation();
void SetupTrayIcon();
void UpdateNotifyIcon();
bool IsBluetoothRecoveryActive();
void ShowDeviceFlyoutAtTray(HWND hWnd);
void ToggleDeviceFlyoutAtTray(HWND hWnd);
Style CreateAcrylicFlyoutStyle(winrt::Windows::UI::Xaml::Interop::TypeName const& targetType);
std::optional<Point> GetNotifyIconPosition(HWND hWnd);

winrt::fire_and_forget ProcessConnectionQueue();
winrt::Windows::Foundation::IAsyncOperation<int32_t> ConnectDeviceAsync(ConnectionRequest request);
winrt::Windows::Foundation::IAsyncOperation<int32_t> EnsureBluetoothReady(bool forceRecovery, bool allowActiveSessionReset, std::wstring deviceId,
	std::shared_ptr<AsyncCancellation> cancellation = {}, AsyncDeadline deadline = (AsyncDeadline::max)());
winrt::Windows::Foundation::IAsyncAction ReconcileDevicesAsync();
winrt::fire_and_forget UpdateEmptyStateFromRadio(uint64_t generation);
winrt::fire_and_forget RestartDeviceWatcherAfterDelay();
winrt::fire_and_forget FinishShutdownWhenReady();
