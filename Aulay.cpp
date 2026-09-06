#include "pch.h"
#include "DebugAudioMonitor.hpp"
#include "Aulay.h"
#include <winrt/Windows.Foundation.h>

#include "AppModules.hpp"
#include "ConnectionState.hpp"
#include "DeviceListUi.hpp"
#include "BluetoothRecovery.hpp"
#include "ConnectionFlow.hpp"
#include "DeviceWatcher.hpp"
#include "TrayPanel.hpp"

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment(winrt::apartment_type::single_threaded);
	LoadTranslateData();

	// A second instance would duplicate the tray icon, the device watcher and
	// settings writes. The mutex is per-session (Local\), so separate Windows
	// sessions can each run their own Aulay.
	wil::unique_handle singleInstance(CreateMutexW(nullptr, TRUE, L"Local\\Aulay.SingleInstance"));
	if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
	{
		TaskDialog(nullptr, nullptr, _(L"Aulay"), nullptr,
			_(L"Aulay is already running. Check the notification tray for its icon."),
			TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
		return EXIT_FAILURE;
	}

	InitializeDiagnostics();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		RecordDiagnostic(L"lifecycle", L"unsupported operating system");
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"Aulay is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		CloseDiagnostics();
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AULAY)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"Aulay",
		.hIconSm = wcex.hIcon
	};

	FAIL_FAST_LAST_ERROR_IF(RegisterClassExW(&wcex) == 0);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"Aulay", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	g_uiDispatcher = g_xamlCanvas.Dispatcher();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDeviceList();
	SetupTrayIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"Aulay"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg{};
	BOOL getMessageResult = FALSE;
	while ((getMessageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0)
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	if (getMessageResult == -1)
		LOG_LAST_ERROR();

	try
	{
		desktopSource.Close();
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
	}
	g_xamlCanvas = nullptr;
	g_uiDispatcher = nullptr;

	return getMessageResult == -1 ? EXIT_FAILURE : static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_ACTIVATEAPP:
		if (!wParam && !IsStopping())
		{
			// Radio power transitions transiently deactivate the hidden host
			// window; the grace window exempts only those bounded moments, so
			// deactivation from clicking elsewhere still closes the device UI.
			if (g_xamlDeviceFlyout)
			{
				if (IsRadioDeactivationGraceActive())
					RecordDiagnostic(L"ui", L"device-flyout dismiss deferred: radio transition grace active");
				else
					g_xamlDeviceFlyout.Hide();
			}
			if (g_xamlFlyout) g_xamlFlyout.Hide();
			if (g_xamlMenu) g_xamlMenu.Hide();
		}
		break;
	case WM_CLOSE:
		BeginShutdown();
		return 0;
	case WM_DESTROY:
		ShutdownApplication();
		return 0;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			ToggleDeviceFlyoutAtTray(hWnd);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto point = GetNotifyIconPosition(hWnd);
			if (!point) break;

			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions menuOptions;
			menuOptions.Position(*point);
			menuOptions.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
			g_xamlMenu.ShowAt(g_xamlCanvas, menuOptions);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		QueueRememberedDevices();
		break;
	case WM_DISMISSDEVICEFLYOUT:
		// An outside click or foreground change is explicit user intent; the
		// radio-transition grace window does not apply here.
		if (g_xamlDeviceFlyout && g_app.deviceFlyoutVisible && !IsStopping())
		{
			RecordDiagnostic(L"ui", L"device-flyout dismissed by outside interaction");
			g_xamlDeviceFlyout.Hide();
		}
		break;
	case WM_FINISHSHUTDOWN:
		DestroyWindow(hWnd);
		return 0;
	default:
		if (!IsStopping() && WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void BeginShutdown()
{
	if (g_app.shutdownRequested.exchange(true))
		return;

	RecordDiagnostic(L"lifecycle", L"shutdown requested");
	RemoveDeviceFlyoutDismissHooks();
	g_app.stoppingSignal.SetEvent();
	g_app.deviceChanged.SetEvent();
	g_app.activityChanged.SetEvent();
	if (g_app.bluetooth.cancellation)
		g_app.bluetooth.cancellation->Request();
	g_app.connectionQueue.clear();
	g_app.connectionQueueChanged.SetEvent();
	for (auto& generation : g_app.connectGenerations)
		++generation.second;

	std::vector<std::wstring> sessionIds;
	sessionIds.reserve(g_app.sessions.size());
	for (const auto& session : g_app.sessions)
		sessionIds.push_back(session.first);
	for (const auto& deviceId : sessionIds)
		CloseConnectionSession(deviceId, std::nullopt, false);

	if (g_xamlDeviceFlyout) g_xamlDeviceFlyout.Hide();
	if (g_xamlFlyout) g_xamlFlyout.Hide();
	if (g_xamlMenu) g_xamlMenu.Hide();
	FinishShutdownWhenReady();
}

winrt::fire_and_forget FinishShutdownWhenReady()
{
	try
	{
		auto dispatcher = g_uiDispatcher;
        for (;;) {
            co_await winrt::resume_foreground(dispatcher);
            g_app.activityChanged.ResetEvent();
            if(!g_app.bluetooth.inProgress && !g_app.connectionWorkerRunning && !g_app.unexpectedDisconnectRecoveryPending)break;
            co_await winrt::resume_on_signal(g_app.activityChanged.get());
        }
        std::vector<std::shared_ptr<AudioFlow::State>> tasks;
        for(auto const& item:g_audioFlowTasks)tasks.push_back(item.second);
        // Bounded drain, mirroring the audio monitor's 5s shutdown budget: a
        // driver-level hang inside connection.Close() must not block exit.
        auto flowDrainDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        for(auto const& task:tasks)
        {
            try
            {
                co_await AwaitBounded(AudioFlow::WaitFinished(task), flowDrainDeadline);
            }
            catch (...)
            {
                RecordDiagnostic(L"lifecycle", L"audio-flow drain timeout; continuing shutdown");
            }
        }
        co_await winrt::resume_foreground(dispatcher);

		EnqueueDebugAudioEvent(L"shutdown final-audio-request", true);
		StopDebugAudioMonitor();
        co_await WaitDebugAudioMonitorFinished(); // completion event, bounded at 5s
        co_await winrt::resume_foreground(dispatcher);
		RecordDiagnostic(L"diagnostics", std::wstring(DebugAudioMonitorFinished()
			? L"audio-monitor finished " : L"audio-monitor shutdown-timeout-ms=5000 pending-capture-may-be-truncated ") + DebugAudioMonitorStatus());
		PostMessageW(g_hWnd, WM_FINISHSHUTDOWN, 0, 0);
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		PostMessageW(g_hWnd, WM_FINISHSHUTDOWN, 0, 0);
	}
}

winrt::fire_and_forget FinishDiagnosticShutdown()
{
    auto dispatcher=g_uiDispatcher;
    auto uiThread=GetCurrentThreadId();
    try {co_await WaitDiagnosticsFinished();co_await winrt::resume_foreground(dispatcher);PostQuitMessage(0);}
    catch(...) {PostThreadMessageW(uiThread,WM_QUIT,1,0);}
}

void ShutdownApplication()
{
	if (g_app.shuttingDown.exchange(true))
		return;
	StopDebugAudioMonitor();

	g_app.shutdownRequested = true;
	StopDeviceWatcher();
	g_app.connectionQueue.clear();
    g_app.connectionQueueChanged.SetEvent();

	std::vector<std::wstring> sessionIds;
	sessionIds.reserve(g_app.sessions.size());
	for (const auto& session : g_app.sessions)
		sessionIds.push_back(session.first);
	for (const auto& deviceId : sessionIds)
		CloseConnectionSession(deviceId, std::nullopt, false);

	FlushPendingSettings();
	Shell_NotifyIconW(NIM_DELETE, &g_nid);

	auto trayIcon = g_hTrayIcon;
	if (trayIcon)
	{
		DestroyIcon(trayIcon);
		g_hTrayIcon = nullptr;
	}
	g_nid.hIcon = nullptr;
	g_deviceRows.clear();
	g_discoveredDevices.clear();
	g_app.sessions.clear();
	g_app.deviceErrors.clear();
	g_app.unexpectedDisconnectRecoveryPending = false;
	g_app.activityChanged.SetEvent();
	RecordDiagnostic(L"lifecycle", L"shutdown complete");
	CloseDiagnostics();
    FinishDiagnosticShutdown();
}
