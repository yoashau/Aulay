#pragma once

// Tray shell UI: tray icon, the device flyout with its outside-click
// dismissal hooks, the exit confirmation flyout and the context menu.
#include "Aulay.h"

Style CreateAcrylicFlyoutStyle(winrt::Windows::UI::Xaml::Interop::TypeName const& targetType)
{
	AcrylicBrush ab;
	ab.BackgroundSource(AcrylicBackgroundSource::Backdrop);
	ab.TintOpacity(0.85);

	Style s;
	s.TargetType(targetType);

	Setter bg;
	bg.Property(Control::BackgroundProperty());
	bg.Value(winrt::box_value(ab));
	s.Setters().Append(bg);

	Setter cr;
	cr.Property(Control::CornerRadiusProperty());
	cr.Value(winrt::box_value(CornerRadius { 8, 8, 8, 8 }));
	s.Setters().Append(cr);

	return s;
}

std::optional<Point> GetNotifyIconPosition(HWND hWnd)
{
	RECT iconRect;
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr)) {
		LOG_HR(hr);
		return std::nullopt;
	}

	auto dpi = GetDpiForWindow(hWnd);
	return Point {
		static_cast<float>((iconRect.left + (iconRect.right - iconRect.left) / 2) * USER_DEFAULT_SCREEN_DPI / dpi),
		static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi) - 12.0f
	};
}

void DismissDeviceFlyoutFromHook()
{
	PostMessageW(g_hWnd, WM_DISMISSDEVICEFLYOUT, 0, 0);
}

LRESULT CALLBACK DeviceFlyoutMouseHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0 && g_app.deviceFlyoutVisible) {
		UINT message = static_cast<UINT>(wParam);
		if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN
			|| message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN
			|| message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN) {
			auto info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
			// A click on our own tray icon is the shell's toggle gesture; it must
			// not race a hook-initiated dismissal or the flyout reopens itself.
			RECT iconRect {};
			bool onTrayIcon = SUCCEEDED(Shell_NotifyIconGetRect(&g_niid, &iconRect)) && PtInRect(&iconRect, info->pt);
			if (!onTrayIcon) {
				HWND target = WindowFromPoint(info->pt);
				DWORD processId = 0;
				GetWindowThreadProcessId(target, &processId);
				if (processId != GetCurrentProcessId())
					DismissDeviceFlyoutFromHook();
			}
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void CALLBACK DeviceFlyoutForegroundHook(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
	LONG idObject, LONG idChild, DWORD eventThread, DWORD eventTime)
{
	UNREFERENCED_PARAMETER(hook);
	UNREFERENCED_PARAMETER(eventThread);
	UNREFERENCED_PARAMETER(eventTime);
	if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && idChild == 0
		&& hwnd && g_app.deviceFlyoutVisible && !IsRadioDeactivationGraceActive()) {
		DWORD processId = 0;
		GetWindowThreadProcessId(hwnd, &processId);
		if (processId != GetCurrentProcessId())
			DismissDeviceFlyoutFromHook();
	}
}

void InstallDeviceFlyoutDismissHooks()
{
	if (IsStopping() || g_deviceFlyoutMouseHook || g_deviceFlyoutForegroundHook)
		return;
	// LL mouse hooks and OUTOFCONTEXT event hooks are delivered on the
	// installing thread while it pumps messages; the UI thread qualifies.
	g_deviceFlyoutMouseHook = SetWindowsHookExW(WH_MOUSE_LL, DeviceFlyoutMouseHook, nullptr, 0);
	if (!g_deviceFlyoutMouseHook)
		LOG_LAST_ERROR();
	g_deviceFlyoutForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
		EVENT_SYSTEM_FOREGROUND, nullptr, DeviceFlyoutForegroundHook, 0, 0, WINEVENT_OUTOFCONTEXT);
	if (!g_deviceFlyoutForegroundHook)
		LOG_LAST_ERROR();
}

void RemoveDeviceFlyoutDismissHooks()
{
	if (g_deviceFlyoutMouseHook) {
		if (!UnhookWindowsHookEx(g_deviceFlyoutMouseHook))
			LOG_LAST_ERROR();
		g_deviceFlyoutMouseHook = nullptr;
	}
	if (g_deviceFlyoutForegroundHook) {
		if (!UnhookWinEvent(g_deviceFlyoutForegroundHook))
			LOG_LAST_ERROR();
		g_deviceFlyoutForegroundHook = nullptr;
	}
}

void ShowDeviceFlyoutAtTray(HWND hWnd)
{
	if (IsStopping() || !g_xamlDeviceFlyout || !g_xamlCanvas)
		return;

	auto point = GetNotifyIconPosition(hWnd);
	if (!point) {
		RecordDiagnostic(L"ui", L"device-flyout request failed: tray position unavailable");
		return;
	}

	// Keep the content current even while the watcher is paused by a radio reset.
	// This path is deliberately independent from the connection worker.
	for (const auto& row : g_deviceRows)
		ApplySessionStatusToRow(row.first);
	RefreshEmptyState();

	try {
		SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
		SetForegroundWindow(hWnd);

		winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
		options.Position(*point);
		options.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
		g_xamlDeviceFlyout.ShowAt(g_xamlCanvas, options);
		RecordDiagnostic(
			L"ui",
			L"device-flyout request recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false") + L" visible=" + (g_app.deviceFlyoutVisible ? L"true" : L"false"));
	} catch (...) {
		RecordDiagnostic(
			L"ui",
			L"device-flyout request exception hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
		LOG_CAUGHT_EXCEPTION();
	}
}

void ToggleDeviceFlyoutAtTray(HWND hWnd)
{
	if (IsStopping() || !g_xamlDeviceFlyout)
		return;

	if (g_app.deviceFlyoutVisible) {
		RecordDiagnostic(
			L"ui",
			L"device-flyout toggle-close recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false"));
		g_xamlDeviceFlyout.Hide();
		return;
	}

	ShowDeviceFlyoutAtTray(hWnd);
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_app.reconnectEnabled);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.CornerRadius({ 4, 4, 4, 4 });
	button.Click([](const auto&, const auto&) {
		g_app.reconnectEnabled = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);
	flyout.FlyoutPresenterStyle(CreateAcrylicFlyoutStyle(winrt::xaml_typename<FlyoutPresenter>()));

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon copyDiagnosticsIcon;
	copyDiagnosticsIcon.Glyph(L"\xE8C8");

	MenuFlyoutItem copyDiagnosticsItem;
	copyDiagnosticsItem.Text(_(L"Copy Diagnostics"));
	copyDiagnosticsItem.Icon(copyDiagnosticsIcon);
	copyDiagnosticsItem.Click([](const auto&, const auto&) {
		auto success = CopyDiagnosticsToClipboard();
		TaskDialog(
			g_hWnd,
			nullptr,
			_(L"Aulay"),
			nullptr,
			success
				? _(L"Diagnostic information was copied to the clipboard.")
				: _(L"Unable to copy diagnostic information."),
			TDCBF_OK_BUTTON,
			success ? TD_INFORMATION_ICON : TD_ERROR_ICON,
			nullptr);
	});

	FontIcon logFolderIcon;
	logFolderIcon.Glyph(L"\xE8B7");

	MenuFlyoutItem logFolderItem;
	logFolderItem.Text(_(L"Open Log Folder"));
	logFolderItem.Icon(logFolderIcon);
	logFolderItem.Click([](const auto&, const auto&) {
		if (!OpenDiagnosticLogFolder()) {
			TaskDialog(
				g_hWnd,
				nullptr,
				_(L"Aulay"),
				nullptr,
				_(L"Unable to open the log folder."),
				TDCBF_OK_BUTTON,
				TD_ERROR_ICON,
				nullptr);
		}
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		if (!HasConnectionActivity()) {
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr)) {
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);
		auto iconWidthDip = static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi);
		auto iconHeightDip = static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(iconWidthDip);
		g_xamlCanvas.Height(iconHeightDip);

		Point exitPoint { iconWidthDip / 2.0f, -12.0f };
		winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions exitOptions;
		exitOptions.Position(exitPoint);
		exitOptions.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
		g_xamlFlyout.ShowAt(g_xamlCanvas, exitOptions);
	});

	MenuFlyout menu;
	menu.MenuFlyoutPresenterStyle(CreateAcrylicFlyoutStyle(winrt::xaml_typename<MenuFlyoutPresenter>()));
	MenuFlyoutSeparator exitSeparator;

	menu.Items().Append(settingsItem);
	menu.Items().Append(copyDiagnosticsItem);
	menu.Items().Append(logFolderItem);
	menu.Items().Append(exitSeparator);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0) {
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

void SetupDeviceList()
{
	g_emptyStateText = TextBlock();
	g_emptyStateText.Text(_(L"Searching for Bluetooth audio devices..."));
	g_emptyStateText.FontSize(13.0);
	g_emptyStateText.Opacity(0.68);
	g_emptyStateText.HorizontalAlignment(HorizontalAlignment::Center);
	g_emptyStateText.Margin({ 8, 20, 8, 20 });
	g_emptyStateText.TextWrapping(TextWrapping::Wrap);
	g_emptyStateText.TextAlignment(TextAlignment::Center);

	g_deviceListPanel = StackPanel();
	// FlyoutPresenter has a bounded default width. Width applies to the panel's
	// content box, so leave room for both the panel and presenter padding instead
	// of letting the right edge be clipped at high DPI.
	g_deviceListPanel.Width(DEVICE_LIST_WIDTH);
	g_deviceListPanel.Padding({ DEVICE_PANEL_HORIZONTAL_PADDING, DEVICE_PANEL_TOP_PADDING, DEVICE_PANEL_HORIZONTAL_PADDING, DEVICE_PANEL_BOTTOM_PADDING });
	g_deviceListPanel.Children().Append(g_emptyStateText);

	g_xamlDeviceFlyout = Flyout();
	g_xamlDeviceFlyout.ShouldConstrainToRootBounds(false);
	g_xamlDeviceFlyout.Content(g_deviceListPanel);
	g_xamlDeviceFlyout.Opened([](const auto&, const auto&) {
		g_app.deviceFlyoutVisible = true;
		InstallDeviceFlyoutDismissHooks();
		RecordDiagnostic(
			L"ui",
			L"device-flyout opened recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false"));
	});
	g_xamlDeviceFlyout.Closed([](const auto&, const auto&) {
		g_app.deviceFlyoutVisible = false;
		RemoveDeviceFlyoutDismissHooks();
		RecordDiagnostic(
			L"ui",
			L"device-flyout closed recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false"));
		ShowWindow(g_hWnd, SW_HIDE);
	});
	Style deviceFlyoutStyle = CreateAcrylicFlyoutStyle(winrt::xaml_typename<FlyoutPresenter>());
	Setter deviceFlyoutPadding;
	deviceFlyoutPadding.Property(Control::PaddingProperty());
	deviceFlyoutPadding.Value(winrt::box_value(Thickness { 0, 0, 0, 0 }));
	deviceFlyoutStyle.Setters().Append(deviceFlyoutPadding);
	g_xamlDeviceFlyout.FlyoutPresenterStyle(deviceFlyoutStyle);

	StartDeviceWatcher();
}

void SetupTrayIcon()
{
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);
	g_hTrayIcon = static_cast<HICON>(LoadImageW(
		g_hInst,
		MAKEINTRESOURCEW(IDI_AULAY_TRAY),
		IMAGE_ICON,
		width,
		height,
		LR_DEFAULTCOLOR));
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hTrayIcon);
}

void UpdateNotifyIcon()
{
	g_nid.hIcon = g_hTrayIcon;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
		if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		} else {
			LOG_LAST_ERROR();
		}
	}
}
