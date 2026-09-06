#pragma once
#include "BuildVariant.hpp"
#include "Version.h"
#include "BackgroundLog.hpp"

constexpr uint64_t DIAGNOSTIC_LOG_MAX_SIZE = 4 * 1024 * 1024;
constexpr uint32_t DIAGNOSTIC_LOG_BACKUP_COUNT = 2;
constexpr size_t DIAGNOSTIC_RECENT_ENTRY_COUNT = 4096;

std::wstring FormatDiagnosticHresult(HRESULT value)
{
	wchar_t buffer[16]{};
	swprintf_s(buffer, L"0x%08X", static_cast<uint32_t>(value));
	return buffer;
}

std::wstring FormatDiagnosticWin32Error(DWORD value)
{
	wchar_t buffer[16]{};
	swprintf_s(buffer, L"win32=%lu", value);
	return buffer;
}

std::wstring DiagnosticTimestampUtc()
{
	SYSTEMTIME time{};
	GetSystemTime(&time);

	wchar_t buffer[32]{};
	swprintf_s(
		buffer,
		L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
		time.wYear,
		time.wMonth,
		time.wDay,
		time.wHour,
		time.wMinute,
		time.wSecond,
		time.wMilliseconds);
	return buffer;
}

const wchar_t* DiagnosticArchitectureName()
{
#if defined(_M_ARM64)
	return L"arm64";
#elif defined(_M_ARM)
	return L"arm";
#elif defined(_M_X64)
	return L"x64";
#elif defined(_M_IX86)
	return L"x86";
#else
	return L"unknown";
#endif
}

std::wstring DiagnosticOperatingSystemVersion()
{
	using RtlGetVersionFunction = LONG(WINAPI*)(LPOSVERSIONINFOW);
	auto module = GetModuleHandleW(L"ntdll.dll");
	auto rtlGetVersion = module
		? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(module, "RtlGetVersion"))
		: nullptr;
	if (!rtlGetVersion)
		return L"unknown";

	OSVERSIONINFOW version{};
	version.dwOSVersionInfoSize = sizeof(version);
	if (rtlGetVersion(&version) != 0)
		return L"unknown";

	return std::to_wstring(version.dwMajorVersion) + L"." +
		std::to_wstring(version.dwMinorVersion) + L"." +
		std::to_wstring(version.dwBuildNumber);
}

fs::path ResolveDiagnosticLogDirectory()
{
	PWSTR localAppData = nullptr;
	fs::path directory;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)) && localAppData)
	{
		directory = fs::path(localAppData) / APP_DATA_DIRECTORY / L"Logs";
		CoTaskMemFree(localAppData);
	}

	if (directory.empty())
		directory = GetModuleFsPath(g_hInst).remove_filename() / APP_FALLBACK_LOG_DIRECTORY;

	std::error_code error;
	fs::create_directories(directory, error);
	if (!error)
		return directory;

	auto fallback = GetModuleFsPath(g_hInst).remove_filename() / APP_FALLBACK_LOG_DIRECTORY;
	error.clear();
	fs::create_directories(fallback, error);
	return error ? fs::path{} : fallback;
}

struct DiagnosticFileState {
    wil::unique_hfile logFile;
    fs::path logDirectory, logPath;
    uint64_t logSize=0;
};
std::shared_ptr<aulay::logging::Queue> g_diagnosticWriter;

fs::path RotatedDiagnosticLogPath(DiagnosticFileState& diagnostic, uint32_t index)
{
	return diagnostic.logDirectory /
		(diagnostic.logPath.stem().wstring() + L"." + std::to_wstring(index) + L".log");
}

void OpenDiagnosticLogLocked(DiagnosticFileState& diagnostic)
{
	diagnostic.logFile.reset();
	diagnostic.logSize = 0;
	if (diagnostic.logPath.empty())
		return;

	auto handle = CreateFileW(
		diagnostic.logPath.c_str(),
		FILE_APPEND_DATA | GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE)
		return;

	diagnostic.logFile.reset(handle);
	LARGE_INTEGER size{};
	if (GetFileSizeEx(diagnostic.logFile.get(), &size) && size.QuadPart > 0)
		diagnostic.logSize = static_cast<uint64_t>(size.QuadPart);
}

void RotateDiagnosticLogLocked(DiagnosticFileState& diagnostic)
{
	diagnostic.logFile.reset();

	for (uint32_t index = DIAGNOSTIC_LOG_BACKUP_COUNT; index > 0; --index)
	{
		auto source = index == 1
			? diagnostic.logPath
			: RotatedDiagnosticLogPath(diagnostic, index - 1);
		auto target = RotatedDiagnosticLogPath(diagnostic, index);
		if (GetFileAttributesW(source.c_str()) != INVALID_FILE_ATTRIBUTES)
			MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	}

	OpenDiagnosticLogLocked(diagnostic);
}

void WriteDiagnosticEntryLocked(DiagnosticFileState& diagnostic, std::string const& utf8)
{
	if (!diagnostic.logFile)
		return;

	if (diagnostic.logSize > 0 && diagnostic.logSize + utf8.size() > DIAGNOSTIC_LOG_MAX_SIZE)
		RotateDiagnosticLogLocked(diagnostic);
	if (!diagnostic.logFile)
		return;

	size_t offset = 0;
	while (offset < utf8.size())
	{
		DWORD written = 0;
		auto remaining = utf8.size() - offset;
		auto chunk = static_cast<DWORD>(std::min<size_t>(remaining, MAXDWORD));
		if (!WriteFile(diagnostic.logFile.get(), utf8.data() + offset, chunk, &written, nullptr) || written == 0)
		{
			diagnostic.logFile.reset();
			return;
		}
		offset += written;
		diagnostic.logSize += written;
	}

	// Buffered writes keep per-event disk flushes off the UI/callback path.
	// CloseDiagnostics flushes the session; the OS handles normal writeback.
}

void RecordDiagnostic(std::wstring_view area, std::wstring_view message)
{
	std::wstring entry = L"[";
	entry += DiagnosticTimestampUtc();
	entry += L" +";
	entry += std::to_wstring(GetTickCount64());
	entry += L"ms] [";
	entry.append(area.data(), area.size());
	entry += L"] ";
	entry.append(message.data(), message.size());
	entry += L"\r\n";

    // Keep clipboard history immediately available; only enqueue under short locks.
    EnqueueDebugAudioEvent(L"app " + entry.substr(0, entry.size() - 2));
    std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
    if(g_app.diagnostic.recentEntries.size()>=DIAGNOSTIC_RECENT_ENTRY_COUNT)
        g_app.diagnostic.recentEntries.pop_front();
    g_app.diagnostic.recentEntries.push_back(entry);
    if(g_diagnosticWriter)g_diagnosticWriter->Push(std::move(entry));

}

std::wstring DiagnosticDeviceToken(std::wstring_view deviceId)
{
	uint64_t salt = 0;
	{
		std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
		salt = g_app.diagnostic.privacySalt;
	}

	uint64_t hash = 14695981039346656037ull ^ salt;
	for (auto character : deviceId)
	{
		hash ^= static_cast<uint16_t>(character);
		hash *= 1099511628211ull;
	}

	wchar_t token[16]{};
	swprintf_s(token, L"%012llX", static_cast<unsigned long long>(hash & 0xFFFFFFFFFFFFull));
	return token;
}

void RecordConnectionDiagnostic(uint64_t attemptId, std::wstring_view deviceId, std::wstring_view message)
{
	std::wstring detail = L"attempt=";
	detail += std::to_wstring(attemptId);
	detail += L" device=";
	detail += DiagnosticDeviceToken(deviceId);
	detail += L" ";
	detail.append(message.data(), message.size());
	RecordDiagnostic(L"connection", detail);
}

void InitializeDiagnostics()
{
	auto directory = ResolveDiagnosticLogDirectory();
	LARGE_INTEGER counter{};
	QueryPerformanceCounter(&counter);


	{
		std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
		g_app.diagnostic.logDirectory = std::move(directory);
		if (!g_app.diagnostic.logDirectory.empty())
		{
			g_app.diagnostic.logPath = g_app.diagnostic.logDirectory /
				(L"Aulay-" + std::to_wstring(GetCurrentProcessId()) + L".log");
		}
		g_app.diagnostic.privacySalt =
			static_cast<uint64_t>(counter.QuadPart) ^
			(GetTickCount64() << 17) ^
			static_cast<uint64_t>(GetCurrentProcessId());
		g_app.diagnostic.initialized = true;

	}

    auto file=std::make_shared<DiagnosticFileState>();
    file->logPath=g_app.diagnostic.logPath;file->logDirectory=g_app.diagnostic.logDirectory;
    auto writer=std::make_shared<aulay::logging::Queue>();
    try {
        writer->Start([file](auto const& batch) {
            if(!file->logFile && !file->logPath.empty())OpenDiagnosticLogLocked(*file);
            if(!file->logFile)OutputDebugStringW(L"Aulay application log unavailable; memory history retained\n");
            for(auto const& entry:batch) {
                OutputDebugStringW(entry.c_str());
                WriteDiagnosticEntryLocked(*file,Utf16ToUtf8(entry));
            }
        },[file] {
            if(file->logFile)FlushFileBuffers(file->logFile.get());
            file->logFile.reset();
        });
        std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
        g_diagnosticWriter=writer;
    } catch(...) {OutputDebugStringW(L"Aulay application log worker failed to start\n");}

	StartDebugAudioMonitor(g_app.diagnostic.logDirectory, g_app.diagnostic.privacySalt);
	std::wstring startup = L"session-start version=" AULAY_VERSION_TEXT L" architecture=";
	startup += DiagnosticArchitectureName();
	startup += L" os=";
	startup += DiagnosticOperatingSystemVersion();
	startup += L" pid=";
	startup += std::to_wstring(GetCurrentProcessId());
	startup += L" edition=" + std::wstring(APP_DISPLAY_NAME);
	RecordDiagnostic(L"lifecycle", startup);
	RecordDiagnostic(
		L"diagnostics",
		L"connection-policy=event-driven radio-wait=async-state-event off-timeout-ms=5000 on-timeout-ms=8000 opened-wait-ms=1500 post-open-stability-ms=0 retry-base-ms=200 retry-cap-ms=300000 queue-wait=interruptible max-tries=unlimited retry-total-deadline=none attempt-deadline-ms=45000 recovery-deadline-ms=15000 restore-deadline-ms=8000 radio-routing=device-ancestor-cache");

    RecordDiagnostic(L"diagnostics",L"persistent-log=background-writer location="+std::wstring(APP_LOG_FOLDER));
}

void CloseDiagnostics()
{
    std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
    if(g_diagnosticWriter)g_diagnosticWriter->Stop();
    g_app.diagnostic.initialized=false;
}

winrt::Windows::Foundation::IAsyncAction WaitDiagnosticsFinished()
{
    std::shared_ptr<aulay::logging::Queue> writer;
    {std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);writer=g_diagnosticWriter;}
    co_await winrt::resume_background();
    if(writer && !writer->WaitFor(std::chrono::seconds(5)))
        OutputDebugStringW(L"Aulay application log shutdown timed out; queued tail may be truncated\n");
}

std::wstring BuildDiagnosticReport()
{
	std::wstring report = std::wstring(APP_DISPLAY_NAME) + L" diagnostics\r\n";
	std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
	if (!g_app.diagnostic.logPath.empty())
	{
		report += L"Log folder: " + std::wstring(APP_LOG_FOLDER) + L"\r\n";
		report += L"Log file: " + g_app.diagnostic.logPath.filename().wstring() + L"\r\n";
	}
    if(g_diagnosticWriter) {
        std::lock_guard<std::mutex> writerLock(g_diagnosticWriter->mutex);
        report+=L"Background log: queued="+std::to_wstring(g_diagnosticWriter->pending.size())+
            L" dropped="+std::to_wstring(g_diagnosticWriter->totalDropped)+L" errors="+
            std::to_wstring(g_diagnosticWriter->errors)+L" finished="+std::to_wstring(g_diagnosticWriter->finished)+L"\r\n";
    }
    report += L"Raw device identifiers and names are intentionally omitted.\r\n\r\n";
	for (const auto& entry : g_app.diagnostic.recentEntries)
		report += entry;
	return report;
}

bool CopyDiagnosticsToClipboard()
{
	std::wstring report;
	try
	{
		report = BuildDiagnosticReport();
	}
	catch (...)
	{
		RecordDiagnostic(L"diagnostics", L"clipboard report allocation failed");
		return false;
	}

	if (!OpenClipboard(g_hWnd))
	{
		RecordDiagnostic(L"diagnostics", L"OpenClipboard failed " + FormatDiagnosticWin32Error(GetLastError()));
		return false;
	}

	bool success = false;
	HGLOBAL memory = nullptr;
	if (EmptyClipboard())
	{
		auto byteCount = (report.size() + 1) * sizeof(wchar_t);
		memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
		if (memory)
		{
			auto data = GlobalLock(memory);
			if (data)
			{
				memcpy(data, report.c_str(), byteCount);
				GlobalUnlock(memory);
				if (SetClipboardData(CF_UNICODETEXT, memory))
				{
					success = true;
					memory = nullptr;
				}
			}
		}
	}

	if (memory)
		GlobalFree(memory);
	CloseClipboard();
	RecordDiagnostic(L"diagnostics", success ? L"diagnostics copied to clipboard" : L"clipboard copy failed");
	return success;
}

bool OpenDiagnosticLogFolder()
{
	fs::path directory;
	{
		std::lock_guard<std::mutex> lock(g_app.diagnostic.mutex);
		directory = g_app.diagnostic.logDirectory;
	}
	if (directory.empty())
		return false;

	auto result = ShellExecuteW(g_hWnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	auto success = reinterpret_cast<INT_PTR>(result) > 32;
	RecordDiagnostic(L"diagnostics", success ? L"log folder opened" : L"failed to open log folder");
	return success;
}
