// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

#include "targetver.h"

// Windows Header Files
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

// C++ RunTime Header Files
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <mutex>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <vector>

// wil
#ifndef _DEBUG
#define RESULT_DIAGNOSTICS_LEVEL 1
#endif

#include <wil/common.h>
#include <wil/result.h>
#include <wil/cppwinrt.h>

// C++/WinRT
// Fixes warning C4002: too many arguments for function-like macro invocation 'GetCurrentTime'
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.System.h>
#include <chrono>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

#endif // PCH_H
