#!/usr/bin/env python3
"""Instrument a disposable Aulay source copy for native flyout style/motion checks.

Usage: python tests/generate_flyout_ui_fixture.py DISPOSABLE_SOURCE_DIRECTORY
Build that directory normally, then run its EXE. Never distribute the fixture.
No watcher, Bluetooth connection, radio reset or user settings are used.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]).resolve()
assert out != root and (out / 'Aulay.vcxproj').exists(), 'Use an existing disposable source copy'
source = (root / 'Aulay.cpp').read_text(encoding='utf-8-sig')
fixture = r'''
bool HasRoundedPaintedSurface(DependencyObject node)
{
    if (auto border = node.try_as<Border>()) {
        if (border.Background() && border.CornerRadius().TopLeft == 8) return true;
    }
    if (auto content = node.try_as<ContentPresenter>()) {
        if (content.Background() && content.CornerRadius().TopLeft == 8) return true;
    }
    if (auto grid = node.try_as<Grid>()) {
        if (grid.Background() && grid.CornerRadius().TopLeft == 8) return true;
    }
    for (int i = 0; i < VisualTreeHelper::GetChildrenCount(node); ++i)
        if (HasRoundedPaintedSurface(VisualTreeHelper::GetChild(node, i))) return true;
    return false;
}

winrt::fire_and_forget RunFlyoutStyleFixture()
{
    try {
        auto dispatcher = g_uiDispatcher;
        auto require = [](bool ok, wchar_t const* label) {
            RecordDiagnostic(L"style-test", std::wstring(ok ? L"PASS " : L"FAIL ") + label);
            if (!ok) winrt::throw_hresult(E_FAIL);
        };
        co_await winrt::resume_after(std::chrono::milliseconds(150));
        co_await winrt::resume_foreground(dispatcher);
        TextBlock title;
        title.Text(L"Bluetooth audio");
        title.FontSize(15);
        title.FontWeight(FontWeights::SemiBold());
        title.Margin({12, 8, 12, 12});
        Button quick, forced;
        quick.Content(winrt::box_value(_(L"Quick Connect")));
        forced.Content(winrt::box_value(_(L"Force Connect")));
        ApplyFlyoutButtonStyle(quick);
        ApplyFlyoutButtonStyle(forced);
        quick.Width(DEVICE_ACTION_BUTTON_WIDTH);
        forced.Width(DEVICE_ACTION_BUTTON_WIDTH);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(DEVICE_ACTION_SPACING);
        buttons.HorizontalAlignment(HorizontalAlignment::Center);
        buttons.Children().Append(quick);
        buttons.Children().Append(forced);
        g_emptyStateText.Visibility(Visibility::Collapsed);
        g_deviceListPanel.Children().Append(title);
        g_deviceListPanel.Children().Append(buttons);
        // Exercise the actual production Flyout with no device/backend activity.
        auto point = GetNotifyIconPosition(g_hWnd);
        require(point.has_value(), L"tray anchor available");
        SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
        SetForegroundWindow(g_hWnd);
        winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutShowOptions options;
        options.Position(*point);
        options.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
        g_xamlDeviceFlyout.ShowAt(g_xamlCanvas, options);
        if (FlyoutAnimationsEnabled()) {
            co_await winrt::resume_after(std::chrono::milliseconds(65));
            co_await winrt::resume_foreground(dispatcher);
            require(g_deviceFlyoutMotion.translation && g_deviceFlyoutMotion.translation.Y() > 0 && g_deviceFlyoutMotion.translation.Y() < g_deviceFlyoutMotion.presenter.ActualHeight(), L"entrance rises from below instead of fading in place");
        }
        co_await winrt::resume_after(std::chrono::milliseconds(400));
        co_await winrt::resume_foreground(dispatcher);
        FrameworkElement presenter = nullptr;
        for (DependencyObject node = g_deviceListPanel; node; node = VisualTreeHelper::GetParent(node)) {
            if (auto control = node.try_as<FlyoutPresenter>()) { presenter = control; break; }
        }
        require(presenter && presenter.ActualHeight() > 0, L"production presenter laid out");
        require(quick.ActualWidth() == DEVICE_ACTION_BUTTON_WIDTH && forced.ActualWidth() == quick.ActualWidth(), L"shared action button width");
        TextBlock labelProbe;
        labelProbe.FontFamily(quick.FontFamily());
        labelProbe.FontSize(quick.FontSize());
        labelProbe.FontWeight(quick.FontWeight());
        labelProbe.Text(L"Quick Connect");
        labelProbe.Measure({ 10000, 10000 });
        auto labelChrome = quick.Padding().Left + quick.Padding().Right + quick.BorderThickness().Left + quick.BorderThickness().Right;
        require(labelProbe.DesiredSize().Width + labelChrome <= quick.ActualWidth() + 0.5, L"English Quick Connect fits button chrome");
        labelProbe.Text(winrt::unbox_value<winrt::hstring>(quick.Content()));
        labelProbe.Measure({ 10000, 10000 });
        require(labelProbe.DesiredSize().Width + labelChrome <= quick.ActualWidth() + 0.5, L"localized action label fits button chrome");
        require(quick.CornerRadius().TopLeft == 8 && forced.CornerRadius().TopLeft == 8 && quick.FontSize() == 15, L"shared button style");
        auto control = presenter.as<FlyoutPresenter>();
        require(control.CornerRadius().TopLeft == 8 && control.BorderThickness().Left == 1 && control.FontSize() == 15, L"native presenter style");
        require(g_deviceFlyoutMotion.translation && g_deviceFlyoutMotion.translation.Y() == 0, L"entrance finishes at rest");
        require(HasRoundedPaintedSurface(quick) && HasRoundedPaintedSurface(forced) && HasRoundedPaintedSurface(presenter), L"painted button and presenter surfaces have actual rounded corners");
        auto closingAt = GetTickCount64();
        auto closedAt = std::make_shared<ULONGLONG>(0);
        g_xamlDeviceFlyout.Closed([closedAt](auto const&, auto const&) { *closedAt = GetTickCount64(); });
        g_xamlDeviceFlyout.Hide();
        co_await winrt::resume_after(std::chrono::milliseconds(450));
        co_await winrt::resume_foreground(dispatcher);
        require(!g_app.deviceFlyoutVisible && *closedAt != 0, L"dismissal completes");
        RecordDiagnostic(L"style-test", L"close-duration-ms=" + std::to_wstring(*closedAt - closingAt));
        if (FlyoutAnimationsEnabled()) require(*closedAt - closingAt >= 100, L"slide-out runs before native popup closes");
        g_xamlDeviceFlyout.ShowAt(g_xamlCanvas, options);
        co_await winrt::resume_after(std::chrono::milliseconds(350));
        co_await winrt::resume_foreground(dispatcher);
        require(g_app.deviceFlyoutVisible, L"reopen succeeds");
        g_xamlDeviceFlyout.Hide();
        if (FlyoutAnimationsEnabled()) {
            co_await winrt::resume_after(std::chrono::milliseconds(65));
            co_await winrt::resume_foreground(dispatcher);
            require(g_deviceFlyoutMotion.closing && g_deviceFlyoutMotion.translation.Y() > 0, L"closing moves whole presenter downwards");
            ToggleDeviceFlyoutAtTray(g_hWnd);
            co_await winrt::resume_after(std::chrono::milliseconds(350));
            co_await winrt::resume_foreground(dispatcher);
            require(g_app.deviceFlyoutVisible && !g_deviceFlyoutMotion.closing && g_deviceFlyoutMotion.translation.Y() == 0, L"rapid reopen cancels pending close");
            g_xamlDeviceFlyout.Hide();
            g_deviceFlyoutMotion.storyboard.Completed([](auto const&, auto const&) { ToggleDeviceFlyoutAtTray(g_hWnd); });
            co_await winrt::resume_after(std::chrono::milliseconds(650));
            co_await winrt::resume_foreground(dispatcher);
            require(g_app.deviceFlyoutVisible && !g_deviceFlyoutMotion.closing, L"reopen at native close completion is preserved");
            g_xamlDeviceFlyout.Hide();
        }
        co_await winrt::resume_after(std::chrono::milliseconds(450));
        co_await winrt::resume_foreground(dispatcher);
        SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
        SetForegroundWindow(g_hWnd);
        g_menuFocusState = FocusState::Keyboard;
        g_xamlMenu.ShowAt(g_xamlCanvas, options);
        co_await winrt::resume_after(std::chrono::milliseconds(350));
        co_await winrt::resume_foreground(dispatcher);
        for (auto const& item : g_xamlMenu.Items()) {
            if (auto action = item.try_as<MenuFlyoutItem>())
                require(action.CornerRadius().TopLeft == 8 && action.FontSize() == 15 && HasRoundedPaintedSurface(action), L"native menu item style and painted corners");
        }
        g_xamlMenu.Hide();
        RecordDiagnostic(L"style-test", L"FLYOUT_STYLE_RESULT=PASS no-device-connections no-radio-writes");
    } catch (...) {
        RecordDiagnostic(L"style-test", L"FLYOUT_STYLE_RESULT=FAIL hr=" + FormatDiagnosticHresult(winrt::to_hresult()));
    }
    PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
}
'''
source = source.replace('int APIENTRY wWinMain', fixture + '\nint APIENTRY wWinMain', 1)
source = source.replace('L"Local\\\\Aulay.SingleInstance"', 'L"Local\\\\Aulay.FlyoutStyleFixture"')
source = source.replace('\tLoadSettings();', '''\tDefaultSettings();
\tg_settingsStorage.portablePath = GetModuleFsPath(g_hInst).remove_filename() / L"fixture-settings.json";
\tg_settingsStorage.fallbackPath = GetModuleFsPath(g_hInst).remove_filename() / L"fixture-fallback.json";''')
source = source.replace('\tPostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);', '\tRunFlyoutStyleFixture();')
(out / 'Aulay.cpp').write_text(source, encoding='utf-8-sig')
tray = (root / 'TrayPanel.hpp').read_text(encoding='utf-8-sig')
assert tray.count('\tStartDeviceWatcher();') == 1
(out / 'TrayPanel.hpp').write_text(tray.replace('\tStartDeviceWatcher();', '\t// Fixture: no device watcher or real connections.'), encoding='utf-8-sig')
print('FLYOUT_STYLE_FIXTURE=' + str(out))
