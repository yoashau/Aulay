#pragma once

#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <cwctype>
#include <winrt/Windows.Devices.Bluetooth.h>
#include "AsyncUtil.hpp"
#pragma comment(lib, "cfgmgr32.lib")

inline bool SameDeviceInstance(std::wstring const& left, std::wstring const& right)
{
	return !left.empty() && left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](wchar_t a, wchar_t b) { return std::towupper(a) == std::towupper(b); });
}

inline std::optional<size_t> SelectBluetoothAdapterIndex(
	std::vector<std::wstring> const& ancestors,
	std::vector<std::wstring> const& adapterInstances)
{
	// Ancestors are ordered nearest-first. Never select a different adapter just
	// because it happens to be powered on. Unknown routes are ambiguous on multi-radio PCs.
	for (auto const& ancestor : ancestors) {
		std::optional<size_t> match;
		for (size_t i = 0; i < adapterInstances.size(); ++i) {
			if (!SameDeviceInstance(ancestor, adapterInstances[i]))
				continue;
			if (match)
				return std::nullopt;
			match = i;
		}
		if (match)
			return match;
	}
	return adapterInstances.size() == 1 ? std::optional<size_t>(0) : std::nullopt;
}

inline std::wstring DeviceInterfaceInstanceId(std::wstring const& interfaceId)
{
	DEVPROPTYPE type = 0;
	wchar_t instance[MAX_DEVICE_ID_LEN] {};
	ULONG size = sizeof(instance);
	if (CM_Get_Device_Interface_PropertyW(interfaceId.c_str(), &DEVPKEY_Device_InstanceId,
			&type, reinterpret_cast<PBYTE>(instance), &size, 0)
			!= CR_SUCCESS
		|| type != DEVPROP_TYPE_STRING || size > sizeof(instance))
		return {};
	instance[MAX_DEVICE_ID_LEN - 1] = L'\0';
	return instance;
}

inline std::vector<std::wstring> DeviceAncestorIds(std::wstring const& interfaceId)
{
	std::vector<std::wstring> ancestors;
	auto instance = DeviceInterfaceInstanceId(interfaceId);
	if (instance.empty())
		return ancestors;
	DEVINST node = 0;
	if (CM_Locate_DevNodeW(&node, instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
		return ancestors;
	for (size_t depth = 0; depth < 32; ++depth) {
		wchar_t id[MAX_DEVICE_ID_LEN] {};
		if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
			break;
		if (std::any_of(ancestors.begin(), ancestors.end(),
				[&](auto const& previous) { return SameDeviceInstance(previous, id); }))
			break;
		ancestors.emplace_back(id);
		DEVINST parent = 0;
		if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
			break;
		node = parent;
	}
	return ancestors;
}

struct BluetoothRoute {
	wil::unique_event ready { wil::EventOptions::ManualReset };
	std::atomic_bool completed { false };
	winrt::Windows::Devices::Radios::Radio radio { nullptr };
	std::wstring adapterInstance;
	HRESULT error = E_PENDING;
};

struct BluetoothRouteCache {
	std::mutex mutex;
	std::unordered_map<std::wstring, std::shared_ptr<BluetoothRoute>> entries;
};

inline auto GetBluetoothRouteCache()
{
	static auto cache = std::make_shared<BluetoothRouteCache>();
	return cache;
}

inline void InvalidateBluetoothRoute(std::wstring const& deviceId)
{
	auto cache = GetBluetoothRouteCache();
	std::lock_guard<std::mutex> lock(cache->mutex);
	cache->entries.erase(deviceId);
}

inline winrt::fire_and_forget PopulateBluetoothRoute(std::wstring deviceId, std::shared_ptr<BluetoothRoute> route)
{
	try {
		co_await winrt::resume_background();
		using namespace winrt::Windows::Devices::Bluetooth;
		using namespace winrt::Windows::Devices::Enumeration;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		auto ancestors = DeviceAncestorIds(deviceId);
		auto adapters = co_await AwaitBounded(DeviceInformation::FindAllAsync(BluetoothAdapter::GetDeviceSelector()), deadline);
		std::vector<std::wstring> instances;
		instances.reserve(adapters.Size());
		for (auto const& adapter : adapters)
			instances.push_back(DeviceInterfaceInstanceId(std::wstring(adapter.Id())));
		auto index = SelectBluetoothAdapterIndex(ancestors, instances);
		if (!index)
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
		auto adapter = co_await AwaitBounded(BluetoothAdapter::FromIdAsync(adapters.GetAt(static_cast<uint32_t>(*index)).Id()), deadline);
		if (!adapter)
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
		route->radio = co_await AwaitBounded(adapter.GetRadioAsync(), deadline);
		if (!route->radio)
			winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
		route->adapterInstance = instances[*index];
		route->error = S_OK;
	} catch (...) {
		route->error = static_cast<HRESULT>(winrt::to_hresult());
	}
	route->completed.store(true, std::memory_order_release);
	route->ready.SetEvent();
}

inline std::shared_ptr<BluetoothRoute> PrewarmBluetoothRoute(std::wstring const& deviceId)
{
	auto cache = GetBluetoothRouteCache();
	std::shared_ptr<BluetoothRoute> route;
	{
		std::lock_guard<std::mutex> lock(cache->mutex);
		auto existing = cache->entries.find(deviceId);
		if (existing != cache->entries.end())
			return existing->second;
		route = std::make_shared<BluetoothRoute>();
		cache->entries.emplace(deviceId, route);
	}
	// The coroutine yields before any PnP / WinRT discovery work. Quick Connect
	// never awaits this metadata lookup, even on a cold cache.
	PopulateBluetoothRoute(deviceId, route);
	return route;
}

inline winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Radios::Radio>
ReadBluetoothRoute(std::shared_ptr<BluetoothRoute> route)
{
	if (!route->completed.load(std::memory_order_acquire))
		co_await winrt::resume_on_signal(route->ready.get());
	(void)route->completed.load(std::memory_order_acquire);
	winrt::check_hresult(route->error);
	co_return route->radio;
}

inline winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Radios::Radio>
ResolveBluetoothRadio(std::wstring deviceId, AsyncDeadline deadline, std::shared_ptr<AsyncCancellation> cancellation)
{
	co_await winrt::resume_background();
	auto route = PrewarmBluetoothRoute(deviceId);
	if (route->completed.load(std::memory_order_acquire)) {
		DEVINST node = 0;
		if (FAILED(route->error) || (!route->adapterInstance.empty() && CM_Locate_DevNodeW(&node, route->adapterInstance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)) {
			InvalidateBluetoothRoute(deviceId);
			route = PrewarmBluetoothRoute(deviceId);
		}
	}
	co_return co_await AwaitBounded(ReadBluetoothRoute(route), deadline, std::move(cancellation));
}
