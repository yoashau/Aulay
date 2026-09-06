#pragma once

#include "BuildVariant.hpp"

constexpr auto CONFIG_NAME = APP_CONFIG_NAME;
constexpr DWORD CONFIG_BUFFER_SIZE = 4096;
constexpr LONGLONG MAX_CONFIG_SIZE = 1024 * 1024;

struct SettingsStorageState {
	fs::path portablePath;
	fs::path fallbackPath;
	fs::path activePath;
	bool saveErrorShown = false;
};

SettingsStorageState g_settingsStorage;

void DefaultSettings()
{
	g_app.reconnectEnabled = false;
	g_app.desiredDevices.clear();
}

void InitializeSettingsPaths()
{
	if (!g_settingsStorage.portablePath.empty())
		return;
	auto directory = GetModuleFsPath(g_hInst).remove_filename().lexically_normal();
	g_settingsStorage.portablePath = directory / CONFIG_NAME;

	// Separate installations must not overwrite one another's portable settings.
	// This stable directory key is only a file name, not a privacy/security token.
	auto normalized = directory.wstring();
	CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
	uint64_t hash = 14695981039346656037ull;
	for (auto character : normalized) {
		hash ^= static_cast<uint16_t>(character);
		hash *= 1099511628211ull;
	}
	wchar_t name[32] {};
	swprintf_s(name, L"%016llX.json", static_cast<unsigned long long>(hash));
	PWSTR localAppData = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
		wil::unique_cotaskmem_string owner(localAppData);
		if (localAppData)
			g_settingsStorage.fallbackPath = fs::path(localAppData) / APP_DATA_DIRECTORY / L"Settings" / name;
	}
}

bool ReadSettingsFile(fs::path const& settingsPath)
{
	if (settingsPath.empty())
		return false;
	wil::unique_hfile hFile(CreateFileW(settingsPath.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!hFile) {
		auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return false;
		winrt::throw_hresult(HRESULT_FROM_WIN32(error));
	}
	LARGE_INTEGER fileSize {};
	THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(hFile.get(), &fileSize));
	THROW_HR_IF(E_INVALIDARG, fileSize.QuadPart < 0 || fileSize.QuadPart > MAX_CONFIG_SIZE);

	std::string string(static_cast<size_t>(fileSize.QuadPart), '\0');
	size_t offset = 0;
	while (offset < string.size()) {
		DWORD read = 0;
		auto chunk = static_cast<DWORD>(std::min<size_t>(string.size() - offset, CONFIG_BUFFER_SIZE));
		THROW_IF_WIN32_BOOL_FALSE(ReadFile(hFile.get(), string.data() + offset, chunk, &read, nullptr));
		if (read == 0)
			break;
		offset += read;
	}
	string.resize(offset);
	auto jsonObj = JsonObject::Parse(Utf8ToUtf16(string));
	if (jsonObj.HasKey(L"reconnect")) {
		auto value = jsonObj.Lookup(L"reconnect");
		if (value.ValueType() == JsonValueType::Boolean)
			g_app.reconnectEnabled = value.GetBoolean();
	}
	if (jsonObj.HasKey(L"lastDevices")) {
		auto value = jsonObj.Lookup(L"lastDevices");
		if (value.ValueType() == JsonValueType::Array) {
			std::unordered_set<std::wstring> seen;
			for (auto const& item : value.GetArray()) {
				if (item.ValueType() != JsonValueType::String)
					continue;
				auto id = std::wstring(item.GetString());
				if (!id.empty() && seen.insert(id).second)
					g_app.desiredDevices.push_back(std::move(id));
			}
		}
	}
	return true;
}

bool PreferFallbackSettings(fs::path const& portable, fs::path const& fallback)
{
	std::error_code error;
	if (fallback.empty() || !fs::is_regular_file(fallback, error))
		return false;
	auto fallbackTime = fs::last_write_time(fallback, error);
	if (error)
		return false;
	auto portableTime = fs::last_write_time(portable, error);
	return error || fallbackTime > portableTime;
}

void LoadSettings()
{
	DefaultSettings();
	try {
		InitializeSettingsPaths();
		auto first = g_settingsStorage.portablePath;
		auto second = g_settingsStorage.fallbackPath;
		if (PreferFallbackSettings(first, second))
			std::swap(first, second);
		g_settingsStorage.activePath.clear();
		for (auto const& path : { first, second }) {
			try {
				DefaultSettings();
				if (!ReadSettingsFile(path))
					continue;
				g_settingsStorage.activePath = path;
				RecordDiagnostic(L"settings",
					std::wstring(L"settings loaded store=") + (path == g_settingsStorage.fallbackPath ? L"localappdata" : L"portable") + L" remembered-device-count=" + std::to_wstring(g_app.desiredDevices.size()));
				return;
			} catch (...) {
				RecordDiagnostic(L"settings", L"settings candidate load failed hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
			}
		}
		DefaultSettings();
		RecordDiagnostic(L"settings", L"no readable valid settings; defaults loaded");
	} catch (...) {
		DefaultSettings();
		RecordDiagnostic(L"settings", L"settings load failed hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
		LOG_CAUGHT_EXCEPTION();
	}
}

void WriteSettingsAtomically(fs::path const& settingsPath, std::string const& utf8)
{
	auto temporaryPath = settingsPath;
	temporaryPath += L".tmp";
	bool removeTemporaryFile = false;
	try {
		{
			wil::unique_hfile hFile(CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
			THROW_LAST_ERROR_IF(!hFile);
			removeTemporaryFile = true;
			size_t offset = 0;
			while (offset < utf8.size()) {
				DWORD written = 0;
				auto chunk = static_cast<DWORD>(std::min<size_t>(utf8.size() - offset, CONFIG_BUFFER_SIZE));
				THROW_IF_WIN32_BOOL_FALSE(WriteFile(hFile.get(), utf8.data() + offset, chunk, &written, nullptr));
				THROW_HR_IF(E_FAIL, written == 0);
				offset += written;
			}
			THROW_IF_WIN32_BOOL_FALSE(FlushFileBuffers(hFile.get()));
		}
		BOOL replaced = FALSE;
		if (GetFileAttributesW(settingsPath.c_str()) != INVALID_FILE_ATTRIBUTES)
			replaced = ReplaceFileW(settingsPath.c_str(), temporaryPath.c_str(), nullptr, 0, nullptr, nullptr);
		if (!replaced)
			THROW_IF_WIN32_BOOL_FALSE(MoveFileExW(temporaryPath.c_str(), settingsPath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
		removeTemporaryFile = false;
	} catch (...) {
		if (removeTemporaryFile)
			DeleteFileW(temporaryPath.c_str());
		throw;
	}
}

// The save snapshot is serialized here synchronously; the application wraps
// this core with a coalescing background worker (SettingsSave.hpp) so slow
// storage never stalls the UI. The native hardening test exercises this core
// directly.

std::string BuildSettingsJson()
{
	InitializeSettingsPaths();
	JsonObject jsonObj;
	jsonObj.Insert(L"reconnect", JsonValue::CreateBooleanValue(g_app.reconnectEnabled));
	JsonArray lastDevices;
	if (g_app.reconnectEnabled)
		for (auto const& deviceId : g_app.desiredDevices)
			lastDevices.Append(JsonValue::CreateStringValue(deviceId));
	jsonObj.Insert(L"lastDevices", lastDevices);
	return Utf16ToUtf8(jsonObj.Stringify());
}

void SaveSettings()
{
	try {
		InitializeSettingsPaths();
		auto utf8 = BuildSettingsJson();
		auto path = g_settingsStorage.activePath.empty()
			? g_settingsStorage.portablePath
			: g_settingsStorage.activePath;
		try {
			WriteSettingsAtomically(path, utf8);
		} catch (...) {
			if (path == g_settingsStorage.fallbackPath || g_settingsStorage.fallbackPath.empty())
				throw;
			RecordDiagnostic(L"settings", L"portable save failed; trying localappdata hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
			path = g_settingsStorage.fallbackPath;
			fs::create_directories(path.parent_path());
			WriteSettingsAtomically(path, utf8);
		}
		g_settingsStorage.activePath = path;
		RecordDiagnostic(L"settings", std::wstring(L"settings saved store=") + (path == g_settingsStorage.fallbackPath ? L"localappdata" : L"portable") + L" remembered-device-count=" + std::to_wstring(g_app.desiredDevices.size()));
	} catch (...) {
		RecordDiagnostic(L"settings", L"settings save failed hr=" + FormatDiagnosticHresult(static_cast<HRESULT>(winrt::to_hresult())));
		LOG_CAUGHT_EXCEPTION();
		if (!g_settingsStorage.saveErrorShown) {
			g_settingsStorage.saveErrorShown = true;
			TaskDialog(IsWindow(g_hWnd) ? g_hWnd : nullptr, nullptr, _(L"Aulay"), nullptr,
				_(L"Settings could not be saved. Check folder access or available disk space."),
				TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		}
	}
}
