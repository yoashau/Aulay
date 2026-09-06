#!/usr/bin/env python3
"""Generate a test-only Aulay.cpp with a fake connected session, no radio actions.
Usage: python tests/generate_debug_ui_fixture.py OUTPUT_CPP
Compile the output only in a disposable diagnostic-build source copy.
The delivery EXE must be built from the uninstrumented Aulay.cpp instead.
"""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'Aulay.cpp').read_text(encoding='utf-8-sig')
anchor='\tg_deviceRows.emplace(id, DeviceRow{ cardContent, nameText, statusText, quickConnectButton, forceConnectButton });\n'
assert source.count(anchor)==1
fixture=r'''
	// Test-only state: deliberately no AudioPlaybackConnection is opened.
	static bool fixtureSeeded = false;
	if (!fixtureSeeded)
	{
		fixtureSeeded = true;
		ConnectionSession fixture;
		fixture.device = device;
		fixture.generation = 42;
		fixture.attemptId = 4242;
		fixture.phase = ConnectionPhase::Connected;
		fixture.requestMode = ConnectionRequestMode::Quick;
		fixture.connectedAt = std::chrono::steady_clock::now();
		g_app.sessions.emplace(id, std::move(fixture));
		g_debugLastDisconnect[id] = { 4241, 41, GetTickCount64() - 500 };
		RecordDiagnostic(L"ui-test", L"synthetic-connected-session no-real-audio-connection");
	}
'''
out=Path(sys.argv[1]).resolve()
assert out != (root/'Aulay.cpp').resolve(), 'Use a disposable output copy'
source=source.replace(anchor,anchor+fixture)
source=source.replace('bool cleanAudio=wasConnected', 'bool cleanAudio=false && wasConnected')
source=source.replace('void QueueConnectionInternal(', 'int fixtureForcedRequests=0;\nvoid QueueConnectionInternal(',1)
start=source.index('void QueueConnectionInternal(')
opening=source.index('\n{',start)+2
source=source[:opening]+'\n    if(mode==ConnectionRequestMode::Forced){++fixtureForcedRequests;return;}\n'+source[opening:]

source=source.replace('#include \"DebugAudioMonitor.hpp\"', '#include \"DebugAudioMonitor.hpp\"\n#include <winrt/Windows.UI.Xaml.Automation.Peers.h>\n#include <winrt/Windows.UI.Xaml.Automation.Provider.h>',1)
actions=r'''
winrt::fire_and_forget RunDebugUiFixture()
{
	static bool ran = false;
	if (ran) co_return;
	ran = true;
	try
	{
		auto dispatcher = g_uiDispatcher;
		co_await winrt::resume_after(std::chrono::milliseconds(150));
		co_await winrt::resume_foreground(dispatcher);
		if (g_deviceRows.empty() || g_app.sessions.empty()) winrt::throw_hresult(E_FAIL);
		auto id = g_app.sessions.begin()->first;
		auto left = g_deviceRows.at(id).actionButton;
		auto right = g_deviceRows.at(id).forceConnectButton;
		auto require = [](bool ok) { if (!ok) winrt::throw_hresult(E_FAIL); };
		auto label = [](Button const& b) { return winrt::unbox_value<winrt::hstring>(b.Content()); };
		require(label(left) == _(L"Disconnect") && label(right) == _(L"No sound"));
		require(left.IsEnabled() && right.IsEnabled() && right.Visibility() == Visibility::Visible);
		require(left.ActualWidth() > 0 && left.ActualWidth() == right.ActualWidth());
		require(Grid::GetColumn(left) == 0 && Grid::GetColumn(right) == 1 && Grid::GetColumnSpan(left) == 1);
		require(g_deviceListPanel.Children().GetAt(0) == g_emptyStateText);
		require(std::wstring(g_nid.szTip) == _(L"Aulay"));
		RecordDiagnostic(L"ui-test", L"UI_NORMAL_DESIGN PASS: no debug heading; normal tooltip and status");
		RecordDiagnostic(L"ui-test", L"UI_LAYOUT PASS: Disconnect/No sound equal-width adjacent buttons");
		auto invoke = [](Button const& b) {
			using namespace winrt::Windows::UI::Xaml::Automation::Peers;
			ButtonAutomationPeer peer(b);
			peer.GetPattern(PatternInterface::Invoke).as<winrt::Windows::UI::Xaml::Automation::Provider::IInvokeProvider>().Invoke();
		};
		invoke(right);
		co_await winrt::resume_after(std::chrono::milliseconds(100));
		co_await winrt::resume_foreground(dispatcher);
		require(g_app.sessions.at(id).noSoundReports == 1);
		require(g_deviceRows.at(id).statusText.Text() == _(L"Connected"));
		invoke(right);
		co_await winrt::resume_after(std::chrono::milliseconds(100));
		co_await winrt::resume_foreground(dispatcher);
		require(g_app.sessions.at(id).noSoundReports == 2 && g_app.sessions.at(id).phase == ConnectionPhase::Connected);
		require(!g_app.bluetooth.inProgress && g_app.connectionQueue.empty() && fixtureForcedRequests==2);
        // Recovery state must not block hiding/reopening the panel.
        g_app.unexpectedDisconnectRecoveryPending=true;
        for(auto const& row:g_deviceRows)ApplySessionStatusToRow(row.first);
        g_xamlDeviceFlyout.Hide();
        co_await winrt::resume_after(std::chrono::milliseconds(100));
        co_await winrt::resume_foreground(dispatcher);
        ShowDeviceFlyoutAtTray(g_hWnd);
        co_await winrt::resume_after(std::chrono::milliseconds(100));
        co_await winrt::resume_foreground(dispatcher);
        require(g_app.deviceFlyoutVisible);
        ToggleDeviceFlyoutAtTray(g_hWnd);
        co_await winrt::resume_after(std::chrono::milliseconds(100));
        co_await winrt::resume_foreground(dispatcher);
        require(!g_app.deviceFlyoutVisible);
        ShowDeviceFlyoutAtTray(g_hWnd);
        co_await winrt::resume_after(std::chrono::milliseconds(100));
        co_await winrt::resume_foreground(dispatcher);
        require(g_app.deviceFlyoutVisible);
        g_app.unexpectedDisconnectRecoveryPending=false;
        ApplySessionStatusToRow(id);
        RecordDiagnostic(L"ui-test",L"UI_RECOVERY_TRAY PASS: open/close/reopen during recovery, live-radio-writes=0");
		for (int i = 0; i < 100 && PendingDebugAudioSnapshots() != 0; ++i)
		{
			co_await winrt::resume_after(std::chrono::milliseconds(50));
			co_await winrt::resume_foreground(dispatcher);
		}
		require(PendingDebugAudioSnapshots() == 0);
		RecordDiagnostic(L"ui-test", L"UI_MARKER PASS: two real button invocations; reports=2; forced-requests=2; radio interception verified");
		invoke(left);
		co_await winrt::resume_after(std::chrono::milliseconds(100));
		co_await winrt::resume_foreground(dispatcher);
		require(g_app.sessions.count(id) == 0);
		require(label(left) == _(L"Quick Connect") && label(right) == _(L"Force Connect"));
		require(right.Visibility() == Visibility::Visible);
		RecordDiagnostic(L"ui-test", L"UI_DISCONNECT PASS: Quick/Force restored");
		RecordDiagnostic(L"ui-test", L"UI_FIXTURE_RESULT=PASS");
	}
	catch (...) { RecordDiagnostic(L"ui-test", L"UI_FIXTURE_RESULT=FAIL hr=" + FormatDiagnosticHresult(winrt::to_hresult())); }
	PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
}

'''
source=source.replace('void SetupDeviceList()\n{',actions+'void SetupDeviceList()\n{',1)
source=source.replace('g_xamlDeviceFlyout.Opened([](const auto&, const auto&) {','g_xamlDeviceFlyout.Opened([](const auto&, const auto&) {\n\t\tRunDebugUiFixture();',1)
out.write_text(source,encoding='utf-8-sig')
print('DEBUG_UI_FIXTURE='+str(out))
