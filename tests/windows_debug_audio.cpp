// Read-only native smoke test of the same diagnostic collector used by the UI.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>
#include <winrt/Windows.Foundation.h>
#include "../DebugAudioDiagnostics.hpp"

int main()
{
	int checks = 0, failed = 0;
	auto check = [&](bool ok) { ++checks; if (!ok) ++failed; };
	check(DebugAudioHresult(E_ACCESSDENIED) == L"0x80070005");
	check(DebugAudioToken(L"endpoint-a", 42) == DebugAudioToken(L"endpoint-a", 42));
	check(DebugAudioToken(L"endpoint-a", 42) != DebugAudioToken(L"endpoint-a", 43));
	check(DebugAudioToken(L"endpoint-a", 42) != DebugAudioToken(L"endpoint-b", 42));
	check(DebugAudioValue<float>([](float* p) { *p = 0.5f; return S_OK; }) == L"0.500000");
	check(DebugAudioValue<BOOL>([](BOOL*) { return E_ACCESSDENIED; }) == L"unavailable:0x80070005");
	auto lines = CollectDebugAudioSnapshot(42);
	check(lines.size() >= 2);
	check(lines.front().find(L"capture-start") == 0);
	check(lines.back().find(L"capture-end") == 0);
	check(lines.back().find(L"not-proof-of-audibility") != std::wstring::npos);
	for (auto const& line : lines) std::wcout << line << L'\n';
	std::cout << "DEBUG_AUDIO checks=" << checks << " failures=" << failed << '\n';
	return failed ? 1 : 0;
}
