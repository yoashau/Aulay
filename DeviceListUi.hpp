#pragma once

// Tray device list UI: device rows, per-row status text, empty state and
// the stateful tray tooltip. Owns the discovered-device and row maps.
#include "Aulay.h"


struct DeviceRow {
	UIElement panel;
	TextBlock nameText;
	TextBlock statusText;
	Button actionButton;
	Button forceConnectButton;
};

std::unordered_map<std::wstring, DeviceRow> g_deviceRows;
std::unordered_map<std::wstring, DeviceInformation> g_discoveredDevices;

// UI owns this map; workers only own their independent state and COM objects.
std::unordered_map<std::wstring, std::shared_ptr<AudioFlow::State>> g_audioFlowTasks;
bool AudioFlowTasksFinished()
{
    return std::all_of(g_audioFlowTasks.begin(), g_audioFlowTasks.end(),
        [](auto const& item) { return item.second->done.load(); });
}

constexpr double DEVICE_LIST_WIDTH = 264.0;
constexpr double DEVICE_ACTION_BUTTON_WIDTH = 104.0;
constexpr double DEVICE_ACTION_SPACING = 8.0;
constexpr double DEVICE_ACTION_ROW_WIDTH = DEVICE_ACTION_BUTTON_WIDTH * 2 + DEVICE_ACTION_SPACING;
constexpr double DEVICE_PANEL_HORIZONTAL_PADDING = 16.0;
constexpr double DEVICE_PANEL_TOP_PADDING = 12.0;
constexpr double DEVICE_PANEL_BOTTOM_PADDING = 16.0;
constexpr double DEVICE_ROW_LEADING_INSET = 8.0;
constexpr double DEVICE_TEXT_EDGE_INSET = 4.0;
constexpr double DEVICE_INFO_ACTION_SPACING = 12.0;
constexpr double DEVICE_CARD_VERTICAL_MARGIN = 8.0;
constexpr double DEVICE_STATUS_MAX_WIDTH = 104.0;

void UpdateTrayTooltip()
{
	if (!g_hTrayIcon)
		return;

	std::wstring tip;
	if (IsBluetoothRecoveryActive())
		tip = _(L"Restarting Bluetooth...");
	else
	{
		std::wstring names;
		try
		{
			for (const auto& item : g_app.sessions)
			{
				if (item.second.phase != ConnectionPhase::Connected || !item.second.device)
					continue;
				auto name = item.second.device.Name();
				if (name.empty())
					continue;
				if (!names.empty())
					names += L", ";
				names += name;
			}
		}
		catch (...)
		{
		}
		if (!names.empty())
			tip = std::wstring(_(L"Connected")) + L": " + names;
		else if (HasConnectionActivity())
			tip = _(L"Connecting...");
		else
			tip = _(L"Aulay");
	}
	if (tip.size() > 127)
		tip.resize(127);
	wcscpy_s(g_nid.szTip, tip.c_str());
	Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// 1-based position of the device in the pending queue ordered by not-before
// time. Positions above 1 mean earlier requests must finish first.
size_t ConnectionQueuePosition(std::wstring const& deviceId)
{
	auto mine = std::find_if(g_app.connectionQueue.begin(), g_app.connectionQueue.end(),
		[&deviceId](ConnectionRequest const& request) { return request.deviceId == deviceId; });
	if (mine == g_app.connectionQueue.end())
		return 1;
	size_t position = 1;
	for (auto it = g_app.connectionQueue.begin(); it != g_app.connectionQueue.end(); ++it)
	{
		if (it == mine)
			continue;
		if (it->notBefore < mine->notBefore || (it->notBefore == mine->notBefore && it < mine))
			++position;
	}
	return position;
}

void ApplySessionStatusToRow(std::wstring const& deviceId)
{
	if ((g_app.unexpectedDisconnectRecoveryPending || g_app.bluetooth.inProgress) &&
		deviceId == g_app.bluetooth.deviceId)
	{
		// Only the device that owns the adapter reset reports the restart;
		// every other row keeps its real state and stays actionable. Cancel
		// opts out of the follow-up reconnect without stopping the radio
		// restore itself.
		UpdateDeviceRowStatus(deviceId, _(L"Restarting Bluetooth..."), _(L"Cancel"), true);
		UpdateTrayTooltip();
		return;
	}

	auto session = g_app.sessions.find(deviceId);
	if (session == g_app.sessions.end())
	{
        if(IsConnectionQueued(deviceId)) {
            auto error=g_app.deviceErrors.find(deviceId);
            std::wstring status=_(L"Waiting to retry...");
            auto position=ConnectionQueuePosition(deviceId);
            if(position>1)status+=L" (#"+std::to_wstring(position)+L")";
            if(error!=g_app.deviceErrors.end())status+=L" "+FormatDeviceError(error->second);
            UpdateDeviceRowStatus(deviceId,status,_(L"Cancel"),true);
            UpdateTrayTooltip();
            return;
        }
        auto error = g_app.deviceErrors.find(deviceId);
		UpdateDeviceRowStatus(
			deviceId,
			error == g_app.deviceErrors.end() ? std::wstring{} : FormatDeviceError(error->second),
			_(L"Quick Connect"),
			true);
		return;
	}

	switch (session->second.phase)
	{
	case ConnectionPhase::RecoveringBluetooth:
		UpdateDeviceRowStatus(deviceId, _(L"Restarting Bluetooth..."), _(L"Cancel"), true);
		break;
	case ConnectionPhase::Starting:
	case ConnectionPhase::Opening:
	case ConnectionPhase::WaitingForOpened:
		UpdateDeviceRowStatus(deviceId, _(L"Connecting..."), _(L"Cancel"), true, false);
		break;
	case ConnectionPhase::Connected:
		UpdateDeviceRowStatus(deviceId, _(L"Connected"),
			_(L"Disconnect"), true, false, true);
		break;
	case ConnectionPhase::Closing:
		UpdateDeviceRowStatus(deviceId, _(L"Disconnecting..."), _(L"Disconnect"), false, false);
		break;
	}
	UpdateTrayTooltip();
}

void RefreshEmptyState()
{
	if (!g_emptyStateText)
		return;

	auto generation = ++g_app.emptyStateGeneration;
	if (!g_deviceRows.empty())
	{
		g_emptyStateText.Visibility(Visibility::Collapsed);
		return;
	}

	g_emptyStateText.Visibility(Visibility::Visible);
	if (IsBluetoothRecoveryActive())
	{
		g_emptyStateText.Text(_(L"Restarting Bluetooth..."));
		return;
	}

	if (!g_app.deviceEnumerationComplete)
		g_emptyStateText.Text(_(L"Searching for Bluetooth audio devices..."));
	else
		g_emptyStateText.Text(_(L"No Bluetooth audio devices found.\nMake sure Bluetooth is turned on and your device is paired."));

	UpdateEmptyStateFromRadio(generation);
}

winrt::fire_and_forget UpdateEmptyStateFromRadio(uint64_t generation)
{
	using namespace winrt::Windows::Devices::Radios;

	try
	{
		auto dispatcher = g_uiDispatcher;
		auto radios = co_await Radio::GetRadiosAsync();
		bool bluetoothOn = false;
		for (const auto& radio : radios)
		{
			if (radio.Kind() == RadioKind::Bluetooth && radio.State() == RadioState::On)
			{
				bluetoothOn = true;
				break;
			}
		}

		co_await winrt::resume_foreground(dispatcher);
		if (IsStopping() || generation != g_app.emptyStateGeneration || !g_deviceRows.empty() || IsBluetoothRecoveryActive())
			co_return;

		if (!bluetoothOn)
			g_emptyStateText.Text(_(L"Bluetooth is turned off. Turn it on to discover audio devices."));
		else if (!g_app.deviceEnumerationComplete)
			g_emptyStateText.Text(_(L"Searching for Bluetooth audio devices..."));
		else
			g_emptyStateText.Text(_(L"No Bluetooth audio devices found.\nMake sure Bluetooth is turned on and your device is paired."));
	}
	catch (...)
	{
		if (!IsStopping())
			LOG_CAUGHT_EXCEPTION();
	}
}

void UpsertDeviceRow(DeviceInformation const& device)
{
	if (!device || IsStopping())
		return;

	auto id = std::wstring(device.Id());
    bool newlyPresent=g_discoveredDevices.count(id)==0;
    g_discoveredDevices.insert_or_assign(id, device);
    g_app.deviceChanged.SetEvent();
    if(newlyPresent) {
        for(auto& queued:g_app.connectionQueue)if(queued.deviceId==id)queued.notBefore={};
        g_app.connectionQueueChanged.SetEvent();
    }
	PrewarmBluetoothRoute(id);

	auto existingRow = g_deviceRows.find(id);
	if (existingRow != g_deviceRows.end())
	{
		existingRow->second.nameText.Text(device.Name());
		ApplySessionStatusToRow(id);
		RefreshEmptyState();
		return;
	}

	TextBlock nameText;
	nameText.Text(device.Name());
	nameText.FontSize(14.0);
	nameText.FontWeight(FontWeights::SemiBold());
	nameText.VerticalAlignment(VerticalAlignment::Center);
	nameText.TextTrimming(TextTrimming::CharacterEllipsis);

	TextBlock statusText;
	statusText.FontSize(11.5);
	statusText.Opacity(0.6);
	statusText.Margin({ 8, 0, 0, 0 });
	statusText.VerticalAlignment(VerticalAlignment::Center);
	statusText.HorizontalAlignment(HorizontalAlignment::Right);
	statusText.TextAlignment(TextAlignment::Right);
	statusText.TextTrimming(TextTrimming::CharacterEllipsis);
	statusText.MaxWidth(DEVICE_STATUS_MAX_WIDTH);
	statusText.Visibility(Visibility::Collapsed);

	Grid deviceInfoGrid;
	deviceInfoGrid.VerticalAlignment(VerticalAlignment::Center);
	deviceInfoGrid.Margin({ DEVICE_ROW_LEADING_INSET + DEVICE_TEXT_EDGE_INSET, 0, DEVICE_ROW_LEADING_INSET + DEVICE_TEXT_EDGE_INSET, DEVICE_INFO_ACTION_SPACING });
	ColumnDefinition deviceNameColumn, deviceStatusColumn;
	deviceNameColumn.Width({ 1, GridUnitType::Star });
	deviceStatusColumn.Width({ 1, GridUnitType::Auto });
	deviceInfoGrid.ColumnDefinitions().Append(deviceNameColumn);
	deviceInfoGrid.ColumnDefinitions().Append(deviceStatusColumn);
	Grid::SetColumn(nameText, 0);
	Grid::SetColumn(statusText, 1);
	deviceInfoGrid.Children().Append(nameText);
	deviceInfoGrid.Children().Append(statusText);

	Button quickConnectButton;
	quickConnectButton.Content(winrt::box_value(_(L"Quick Connect")));
	quickConnectButton.Width(DEVICE_ACTION_BUTTON_WIDTH);
	quickConnectButton.CornerRadius({ 4, 4, 4, 4 });
	quickConnectButton.Click([id](const auto&, const auto&) {
		if (IsStopping())
			return;

		auto session = g_app.sessions.find(id);
		if (session != g_app.sessions.end())
		{
            DisconnectDevice(id);
            return;
		}

        if(IsConnectionQueued(id)){DisconnectDevice(id);return;}
        auto discovered = g_discoveredDevices.find(id);
        if (discovered != g_discoveredDevices.end())
            QueueConnection(discovered->second, ConnectionRequestMode::Quick);
	});

	Button forceConnectButton;
	forceConnectButton.Content(winrt::box_value(_(L"Force Connect")));
	forceConnectButton.Width(DEVICE_ACTION_BUTTON_WIDTH);
	forceConnectButton.CornerRadius({ 4, 4, 4, 4 });
	forceConnectButton.Click([id](const auto&, const auto&) {
		if (IsStopping()) return;
		if (g_app.sessions.count(id) != 0)
		{
			MarkNoSound(id);
			return;
		}

        QueueConnection(id, ConnectionRequestMode::Forced);
	});

	Grid actionGrid;
	actionGrid.Width(DEVICE_ACTION_ROW_WIDTH);
	actionGrid.HorizontalAlignment(HorizontalAlignment::Center);
	actionGrid.ColumnSpacing(DEVICE_ACTION_SPACING);
	ColumnDefinition quickActionColumn, forceActionColumn;
	quickActionColumn.Width({ DEVICE_ACTION_BUTTON_WIDTH, GridUnitType::Pixel });
	forceActionColumn.Width({ DEVICE_ACTION_BUTTON_WIDTH, GridUnitType::Pixel });
	actionGrid.ColumnDefinitions().Append(quickActionColumn);
	actionGrid.ColumnDefinitions().Append(forceActionColumn);
	Grid::SetColumn(quickConnectButton, 0);
	Grid::SetColumn(forceConnectButton, 1);
	actionGrid.Children().Append(quickConnectButton);
	actionGrid.Children().Append(forceConnectButton);

	Grid cardContent;
	cardContent.Margin({ 0, DEVICE_CARD_VERTICAL_MARGIN, 0, DEVICE_CARD_VERTICAL_MARGIN });
	RowDefinition deviceInfoRow, actionRow;
	deviceInfoRow.Height({ 1, GridUnitType::Auto });
	actionRow.Height({ 1, GridUnitType::Auto });
	cardContent.RowDefinitions().Append(deviceInfoRow);
	cardContent.RowDefinitions().Append(actionRow);
	Grid::SetRow(deviceInfoGrid, 0);
	Grid::SetRow(actionGrid, 1);
	cardContent.Children().Append(deviceInfoGrid);
	cardContent.Children().Append(actionGrid);

	g_deviceListPanel.Children().Append(cardContent);
	g_deviceRows.emplace(id, DeviceRow{ cardContent, nameText, statusText, quickConnectButton, forceConnectButton });
	ApplySessionStatusToRow(id);
	RefreshEmptyState();
}

void RemoveDeviceRow(std::wstring const& deviceId)
{
	// Keep an actionable row while a session is opening or connected. A watcher
	// removal can be transient during radio churn; reconciliation removes the row
	// after the session has actually ended if the device is still absent.
	if (g_app.sessions.count(deviceId) != 0 || IsConnectionQueued(deviceId) || g_app.unexpectedDisconnectRecoveryPending)
		return;

	auto row = g_deviceRows.find(deviceId);
	if (row == g_deviceRows.end())
		return;

	auto children = g_deviceListPanel.Children();
	uint32_t index = 0;
	if (children.IndexOf(row->second.panel, index))
		children.RemoveAt(index);

	// A genuinely absent remembered device may get a new bounded attempt when it
	// appears again. A mere reconciliation of an unchanged list must not rearm it.
	if (g_app.reconnectEnabled && IsDesiredDevice(deviceId))
		g_app.pendingAutoReconnects.insert(deviceId);
	InvalidateBluetoothRoute(deviceId);
	g_deviceRows.erase(row);
	g_discoveredDevices.erase(deviceId);
	RefreshEmptyState();
}

void UpdateDeviceRowStatus(
	std::wstring const& deviceId,
	std::wstring_view statusMessage,
	std::wstring_view buttonLabel,
	bool buttonEnabled,
	bool showForceAction,
	bool showNoSound)
{
	auto row = g_deviceRows.find(deviceId);
	if (row == g_deviceRows.end())
		return;

	row->second.statusText.Text(statusMessage);
	row->second.statusText.Visibility(statusMessage.empty() ? Visibility::Collapsed : Visibility::Visible);
	row->second.actionButton.Content(winrt::box_value(std::wstring(buttonLabel)));
	row->second.actionButton.IsEnabled(buttonEnabled);
	row->second.forceConnectButton.Content(winrt::box_value(showNoSound ? _(L"No sound") : _(L"Force Connect")));
	ToolTipService::SetToolTip(row->second.forceConnectButton, showNoSound &&
		g_app.sessions.count(deviceId) && g_app.sessions.at(deviceId).noSoundReports
		? winrt::box_value(_(L"No sound recorded")) : nullptr);
	auto showSecondary = showForceAction || showNoSound;
	row->second.actionButton.Width(showSecondary ? DEVICE_ACTION_BUTTON_WIDTH : DEVICE_ACTION_ROW_WIDTH);
	Grid::SetColumnSpan(row->second.actionButton, showSecondary ? 1 : 2);
	row->second.forceConnectButton.Visibility(showSecondary ? Visibility::Visible : Visibility::Collapsed);
	row->second.forceConnectButton.IsEnabled(showSecondary && buttonEnabled);
}
