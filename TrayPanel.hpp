#pragma once

// Tray shell UI: tray icon, the device flyout with its outside-click
// dismissal hooks, the exit confirmation flyout and the context menu.
#include "Aulay.h"
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>

// GetDpiForWindow returns 0 for an invalid window. Callers divide by this value
// and pass it to GetSystemMetricsForDpi, so a failure must not become zero.
UINT WindowDpi(HWND window)
{
	auto dpi = window ? GetDpiForWindow(window) : 0;
	return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

AcrylicBrush g_flyoutSurface = nullptr;
TextBlock g_exitPrompt = nullptr;
CheckBox g_exitReconnect = nullptr;
SolidColorBrush g_flyoutStroke = nullptr;
SolidColorBrush g_flyoutText = nullptr;
SolidColorBrush g_flyoutHover = nullptr;
SolidColorBrush g_flyoutPressed = nullptr;

void UpdateFlyoutTheme()
{
	using namespace winrt::Windows::UI::ViewManagement;
	UISettings settings;
	auto background = settings.GetColorValue(UIColorType::Background);
	bool dark = background.R + background.G + background.B < 384;
	bool highContrast = AccessibilitySettings().HighContrast();
	g_xamlCanvas.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
	if (!g_flyoutSurface)
		return;
	g_flyoutSurface.TintColor(dark ? winrt::Windows::UI::Color { 255, 44, 44, 44 } : winrt::Windows::UI::Color { 255, 252, 252, 252 });
	g_flyoutSurface.TintOpacity(0.85);
	g_flyoutSurface.TintLuminosityOpacity(dark ? 0.96 : 0.90);
	g_flyoutSurface.AlwaysUseFallback(highContrast);
	auto fallback = dark ? winrt::Windows::UI::Color { 255, 40, 40, 40 } : winrt::Windows::UI::Color { 255, 249, 249, 249 };
	auto foreground = dark ? winrt::Windows::UI::Color { 255, 245, 245, 245 } : winrt::Windows::UI::Color { 255, 32, 32, 32 };
	auto stroke = dark ? winrt::Windows::UI::Color { 40, 255, 255, 255 } : winrt::Windows::UI::Color { 38, 0, 0, 0 };
	g_flyoutSurface.FallbackColor(highContrast ? background : fallback);
	g_flyoutText.Color(highContrast ? settings.GetColorValue(UIColorType::Foreground) : foreground);
	g_flyoutStroke.Color(highContrast ? settings.GetColorValue(UIColorType::Foreground) : stroke);
	g_flyoutHover.Color(highContrast ? settings.UIElementColor(UIElementType::Highlight) : dark ? winrt::Windows::UI::Color { 15, 255, 255, 255 }
																								: winrt::Windows::UI::Color { 9, 0, 0, 0 });
	g_flyoutPressed.Color(highContrast ? settings.UIElementColor(UIElementType::Highlight) : dark ? winrt::Windows::UI::Color { 24, 255, 255, 255 }
																								  : winrt::Windows::UI::Color { 20, 0, 0, 0 });
	RefreshSecondaryTextContrast();
}

Style CreateAcrylicFlyoutStyle(winrt::Windows::UI::Xaml::Interop::TypeName const& targetType)
{
	if (!g_flyoutSurface) {
		g_flyoutSurface = AcrylicBrush();
		g_flyoutSurface.BackgroundSource(AcrylicBackgroundSource::Backdrop);
		g_flyoutStroke = SolidColorBrush();
		g_flyoutText = SolidColorBrush();
		g_flyoutHover = SolidColorBrush();
		g_flyoutPressed = SolidColorBrush();
	}
	UpdateFlyoutTheme();

	Style s;
	s.TargetType(targetType);
	auto set = [&](DependencyProperty const& property, winrt::Windows::Foundation::IInspectable const& value) {
		Setter setter;
		setter.Property(property);
		setter.Value(value);
		s.Setters().Append(setter);
	};
	set(Control::BackgroundProperty(), g_flyoutSurface);
	set(Control::ForegroundProperty(), g_flyoutText);
	set(Control::BorderBrushProperty(), g_flyoutStroke);
	set(Control::BorderThicknessProperty(), winrt::box_value(Thickness { 1, 1, 1, 1 }));
	set(Control::CornerRadiusProperty(), winrt::box_value(CornerRadius { 8, 8, 8, 8 }));
	set(Control::FontSizeProperty(), winrt::box_value(15.0));
	set(Control::FontWeightProperty(), winrt::box_value(FontWeights::SemiBold()));

	return s;
}

struct DeviceFlyoutMotion {
	FrameworkElement presenter { nullptr };
	TranslateTransform translation { nullptr };
	winrt::Windows::UI::Xaml::Media::Animation::Storyboard storyboard { nullptr };
	uint64_t generation = 0;
	bool closing = false;
	bool completeClose = false;
	bool reopenAfterClose = false;
};
DeviceFlyoutMotion g_deviceFlyoutMotion;

bool FlyoutAnimationsEnabled()
{
	using namespace winrt::Windows::UI::ViewManagement;
	return UISettings().AnimationsEnabled() && !AccessibilitySettings().HighContrast() && !IsStopping();
}

void AnimateDeviceFlyout(bool closing)
{
	using namespace winrt::Windows::UI::Xaml::Media::Animation;
	auto& motion = g_deviceFlyoutMotion;
	auto generation = ++motion.generation;
	double from = motion.translation.Y();
	if (motion.storyboard)
		motion.storyboard.Stop();
	motion.closing = closing;
	motion.translation.Y(from);
	DoubleAnimation animation;
	animation.From(from);
	animation.To(closing ? motion.presenter.ActualHeight() : 0);
	animation.Duration({ std::chrono::milliseconds(closing ? 167 : 250), DurationType::TimeSpan });
	CubicEase ease;
	ease.EasingMode(closing ? EasingMode::EaseIn : EasingMode::EaseOut);
	animation.EasingFunction(ease);
	Storyboard::SetTarget(animation, motion.translation);
	Storyboard::SetTargetProperty(animation, L"Y");
	motion.storyboard = Storyboard();
	motion.storyboard.Children().Append(animation);
	motion.storyboard.Completed([generation, closing](auto const&, auto const&) {
		auto& current = g_deviceFlyoutMotion;
		if (generation != current.generation)
			return;
		if (closing) {
			current.completeClose = true;
			g_xamlDeviceFlyout.Hide();
		} else {
			current.storyboard.Stop();
			current.translation.Y(0);
		}
	});
	motion.storyboard.Begin();
}

void BeginDeviceFlyoutEntrance()
{
	if (g_deviceFlyoutMotion.presenter)
		return;
	for (DependencyObject node = g_deviceListPanel; node; node = VisualTreeHelper::GetParent(node)) {
		if (auto presenter = node.try_as<FlyoutPresenter>()) {
			auto& motion = g_deviceFlyoutMotion;
			motion.presenter = presenter;
			motion.translation = TranslateTransform();
			presenter.RenderTransform(motion.translation);
			motion.translation.Y(FlyoutAnimationsEnabled() ? presenter.ActualHeight() : 0);
			if (FlyoutAnimationsEnabled())
				AnimateDeviceFlyout(false);
			// The background and content stay hidden until their starting position is ready.
			presenter.Opacity(1);
			return;
		}
	}
}

std::optional<Point> GetNotifyIconPosition(HWND hWnd)
{
	RECT iconRect;
	auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
	if (FAILED(hr)) {
		LOG_HR(hr);
		return std::nullopt;
	}

	auto dpi = WindowDpi(hWnd);
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
		if (g_deviceFlyoutMotion.completeClose) {
			g_deviceFlyoutMotion.reopenAfterClose = !g_deviceFlyoutMotion.reopenAfterClose;
			return;
		}
		if (g_deviceFlyoutMotion.closing && g_deviceFlyoutMotion.translation) {
			AnimateDeviceFlyout(false);
			return;
		}
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
	g_exitPrompt = TextBlock();
	g_exitPrompt.Text(_(L"All connections will be closed.\nExit anyway?"));
	g_exitPrompt.Margin({ 0, 0, 0, 12 });

	g_exitReconnect = CheckBox();
	g_exitReconnect.IsChecked(g_app.reconnectEnabled);
	g_exitReconnect.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	ApplyFlyoutButtonStyle(button);
	button.Click([](const auto&, const auto&) {
		auto reconnect = g_exitReconnect.IsChecked();
		g_app.reconnectEnabled = reconnect ? reconnect.Value() : false;
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(g_exitPrompt);
	stackPanel.Children().Append(g_exitReconnect);
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
		auto hasActivity = HasConnectionActivity();
		// The confirmation is also the only control for the startup preference.
		// Skipping it whenever nothing is connected made that preference impossible
		// to turn off without a live session or a hand-edited settings file.
		if (!hasActivity && !g_app.reconnectEnabled) {
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}
		g_exitPrompt.Text(hasActivity
				? _(L"All connections will be closed.\nExit anyway?")
				: _(L"Exit Aulay?"));
		g_exitReconnect.IsChecked(g_app.reconnectEnabled);

		RECT iconRect {};
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		// Exit is explicit. A missing tray rect must not swallow it: anchor the
		// same confirmation at the cursor, which is where the menu click happened.
		if (FAILED(hr) || iconRect.right <= iconRect.left || iconRect.bottom <= iconRect.top) {
			if (FAILED(hr))
				LOG_HR(hr);
			RecordDiagnostic(L"ui", L"exit confirmation tray position unavailable; using cursor");
			POINT cursor {};
			if (!GetCursorPos(&cursor)) {
				cursor.x = 0;
				cursor.y = 0;
			}
			iconRect.left = cursor.x;
			iconRect.top = cursor.y;
			iconRect.right = cursor.x + 1;
			iconRect.bottom = cursor.y + 1;
		}

		auto dpi = WindowDpi(g_hWnd);
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
	for (auto const& item : menu.Items()) {
		if (auto action = item.try_as<MenuFlyoutItem>()) {
			SolidColorBrush clear(winrt::Windows::UI::Color { 0, 0, 0, 0 });
			action.Background(clear);
			action.BorderBrush(clear);
			action.Resources().Insert(winrt::box_value(L"MenuFlyoutItemRevealBackgroundPointerOver"), g_flyoutHover);
			action.Resources().Insert(winrt::box_value(L"MenuFlyoutItemRevealBackgroundPressed"), g_flyoutPressed);
			action.Resources().Insert(winrt::box_value(L"MenuFlyoutItemRevealBorderBrushPointerOver"), clear);
			action.Resources().Insert(winrt::box_value(L"MenuFlyoutItemRevealBorderBrushPressed"), clear);
			action.CornerRadius({ 8, 8, 8, 8 });
			action.FontSize(15.0);
			action.FontWeight(FontWeights::SemiBold());
		}
	}
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
	g_emptyStateText.FontSize(14.0);
	g_emptyStateText.Opacity(SecondaryTextOpacity());
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
	g_deviceListPanel.Loaded([](auto const&, auto const&) { BeginDeviceFlyoutEntrance(); });

	g_xamlDeviceFlyout = Flyout();
	g_xamlDeviceFlyout.ShouldConstrainToRootBounds(false);
	g_xamlDeviceFlyout.AreOpenCloseAnimationsEnabled(false);
	g_xamlDeviceFlyout.Content(g_deviceListPanel);
	g_xamlDeviceFlyout.Opened([](const auto&, const auto&) {
		BeginDeviceFlyoutEntrance();
		g_app.deviceFlyoutVisible = true;
		InstallDeviceFlyoutDismissHooks();
		RecordDiagnostic(
			L"ui",
			L"device-flyout opened recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false"));
	});
	g_xamlDeviceFlyout.Closing([](auto const&, winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutBaseClosingEventArgs const& args) {
		auto& motion = g_deviceFlyoutMotion;
		if (motion.completeClose || !motion.presenter || !FlyoutAnimationsEnabled())
			return;
		args.Cancel(true);
		if (!motion.closing)
			AnimateDeviceFlyout(true);
	});
	g_xamlDeviceFlyout.Closed([](const auto&, const auto&) {
		auto& motion = g_deviceFlyoutMotion;
		bool reopen = std::exchange(motion.reopenAfterClose, false);
		++motion.generation;
		if (motion.storyboard)
			motion.storyboard.Stop();
		if (motion.presenter) {
			motion.presenter.ClearValue(UIElement::OpacityProperty());
			motion.presenter.ClearValue(UIElement::RenderTransformProperty());
		}
		motion.presenter = nullptr;
		motion.translation = nullptr;
		motion.storyboard = nullptr;
		motion.closing = motion.completeClose = false;
		g_app.deviceFlyoutVisible = false;
		RemoveDeviceFlyoutDismissHooks();
		RecordDiagnostic(
			L"ui",
			L"device-flyout closed recovery=" + std::wstring(IsBluetoothRecoveryActive() ? L"true" : L"false"));
		ShowWindow(g_hWnd, SW_HIDE);
		if (reopen && !IsStopping())
			g_uiDispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [] { ShowDeviceFlyoutAtTray(g_hWnd); });
	});
	Style deviceFlyoutStyle = CreateAcrylicFlyoutStyle(winrt::xaml_typename<FlyoutPresenter>());
	Setter hiddenBeforeEntrance;
	hiddenBeforeEntrance.Property(UIElement::OpacityProperty());
	hiddenBeforeEntrance.Value(winrt::box_value(0.0));
	deviceFlyoutStyle.Setters().Append(hiddenBeforeEntrance);
	Setter deviceFlyoutPadding;
	deviceFlyoutPadding.Property(Control::PaddingProperty());
	deviceFlyoutPadding.Value(winrt::box_value(Thickness { 0, 0, 0, 0 }));
	deviceFlyoutStyle.Setters().Append(deviceFlyoutPadding);
	g_xamlDeviceFlyout.FlyoutPresenterStyle(deviceFlyoutStyle);

	StartDeviceWatcher();
}

void SetupTrayIcon()
{
	// The hidden XAML host may be on a different monitor from the taskbar.
	auto taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
	auto dpi = WindowDpi(taskbar ? taskbar : g_hWnd);
	const int width = GetSystemMetricsForDpi(SM_CXSMICON, dpi), height = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
	HICON icon = nullptr;
	winrt::check_hresult(LoadIconWithScaleDown(
		g_hInst,
		MAKEINTRESOURCEW(IDI_AULAY_TRAY),
		width,
		height,
		&icon));
	if (g_hTrayIcon)
		DestroyIcon(g_hTrayIcon);
	g_hTrayIcon = icon;
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
