#!/usr/bin/env python3
"""Exercise the real input hooks and dismissal decisions with fake Win32 state."""
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
helpers = runpy.run_path(str(ROOT / 'tests/test_regressions.py'))
extract = helpers['function']
tray = (ROOT / 'TrayPanel.hpp').read_text(encoding='utf-8-sig')
wndproc = extract('WndProc', (ROOT / 'Aulay.cpp').read_text(encoding='utf-8-sig'))
dismiss_branch = wndproc.split('\tcase WM_DISMISSDEVICEFLYOUT:\n', 1)[1].split('\tcase WM_TIMER:', 1)[0]
timer_branch = wndproc.split('\tcase WM_TIMER:\n', 1)[1].split('\tcase WM_FINISHSHUTDOWN:', 1)[0]
activity_branch = helpers['block'](wndproc, r'^\t\tif \(message >= WM_DEVICEFLYOUTACTIVITY_FIRST\b')
hook = (ROOT / 'FlyoutInputHook.hpp').read_text(encoding='utf-8-sig')
hook = hook.replace('#pragma once', '').replace('#include <windows.h>', '').replace('#include <wil/resource.h>', '')
# Expose the actual callbacks in this isolated harness; production stays private.
hook = hook.replace('class FlyoutInputHook {', 'class FlyoutInputHook { public:', 1)

prefix = r'''
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
using HWND = void*;
using HHOOK = void*;
using HWINEVENTHOOK = void*;
using HMONITOR = void*;
using HANDLE = void*;
using UINT = unsigned;
using DWORD = uint32_t;
using LONG = int32_t;
using INT_PTR = intptr_t;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using UINT_PTR = uintptr_t;
#define CALLBACK
#define UNREFERENCED_PARAMETER(x) (void)(x)
constexpr int FALSE = 0, TRUE = 1;
constexpr DWORD PM_NOREMOVE = 0, PM_REMOVE = 1, QS_ALLINPUT = 2;
constexpr DWORD INFINITE = 0xffffffff, WAIT_OBJECT_0 = 0;
constexpr UINT WM_QUIT = 0x12, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201;
constexpr UINT WM_RBUTTONDOWN = 0x204, WM_MBUTTONDOWN = 0x207, WM_XBUTTONDOWN = 0x20b;
constexpr UINT WM_NCLBUTTONDOWN = 0xa1, WM_NCRBUTTONDOWN = 0xa4;
constexpr UINT WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105;
constexpr UINT WM_TIMER = 0x113, WM_DISMISSDEVICEFLYOUT = 0x8004;
constexpr UINT WM_DEVICEFLYOUTACTIVITY_FIRST = 0x9100, WM_DEVICEFLYOUTMOUSE_COUNT = 0x1000;
constexpr int WH_MOUSE_LL = 14, WH_KEYBOARD_LL = 13;
constexpr DWORD EVENT_SYSTEM_FOREGROUND = 3;
constexpr LONG OBJID_WINDOW = 0;
constexpr UINT MONITOR_DEFAULTTONEAREST = 2;
struct POINT { LONG x, y; };
struct RECT { LONG left, top, right, bottom; };
struct MONITORINFO { DWORD cbSize; RECT rcMonitor; };
struct MSLLHOOKSTRUCT { POINT pt; };
struct MSG { UINT message; };
struct Post { UINT message; WPARAM wParam; LPARAM lParam; };
std::vector<Post> posts;
HWND hwnd(int id) { return reinterpret_cast<HWND>(static_cast<uintptr_t>(id)); }
HWND foreground = hwnd(3), hit = hwnd(2);
RECT bounds { 0, 0, 1920, 1080 }, monitorBounds { 0, 0, 1920, 1080 };
bool visible = true, iconic = false, cursorValid = true, isStopping = false, radioGrace = false;
bool postSucceeds = true;
int forwarded = 0;
DWORD GetCurrentProcessId() { return 7; }
HWND GetForegroundWindow() { return foreground; }
DWORD GetWindowThreadProcessId(HWND window, DWORD* process) {
    if (window == hwnd(1) || window == hwnd(2)) *process = 7;
    else if (window == hwnd(3)) *process = 9;
    else if (window == hwnd(4)) *process = 11;
    else if (window == hwnd(5)) *process = 13;
    else *process = 0;
    return *process;
}
bool PostMessageW(HWND, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!postSucceeds) return false;
    posts.push_back({ message, wParam, lParam });
    return true;
}
LRESULT CallNextHookEx(HHOOK, int, WPARAM, LPARAM) { ++forwarded; return 42; }
bool PeekMessageW(MSG*, HWND, UINT, UINT, UINT) { return false; }
HHOOK SetWindowsHookExW(int, LRESULT (*)(int, WPARAM, LPARAM), void*, DWORD) { return hwnd(1); }
void OutputDebugStringW(wchar_t const*) {}
DWORD MsgWaitForMultipleObjects(DWORD, HANDLE*, int, DWORD, DWORD) { return 0; }
void TranslateMessage(MSG*) {}
void DispatchMessageW(MSG*) {}
void UnhookWindowsHookEx(HHOOK) {}
DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_OBJECT_0; }
namespace wil {
enum class EventOptions { ManualReset };
struct unique_event {
    explicit unique_event(EventOptions) {}
    void SetEvent() {}
    HANDLE get() { return nullptr; }
};
}
HWND GetShellWindow() { return hwnd(4); }
bool IsWindowVisible(HWND) { return visible; }
bool IsIconic(HWND) { return iconic; }
bool GetCursorPos(POINT* point) { *point = { 4, 5 }; return cursorValid; }
HWND WindowFromPoint(POINT) { return hit; }
HMONITOR MonitorFromWindow(HWND, UINT) { return hwnd(1); }
bool GetWindowRect(HWND, RECT* rect) { *rect = bounds; return true; }
bool GetMonitorInfoW(HMONITOR, MONITORINFO* info) { info->rcMonitor = monitorBounds; return true; }
struct { bool deviceFlyoutVisible = true; } g_app;
int hideRequests = 0;
struct FakeFlyout { explicit operator bool() const { return true; } void Hide() { ++hideRequests; } } g_xamlDeviceFlyout;
struct { bool reopenAfterClose = true; } g_deviceFlyoutMotion;
bool IsStopping() { return isStopping; }
bool IsRadioDeactivationGraceActive() { return radioGrace; }
void RecordDiagnostic(wchar_t const*, wchar_t const*) {}
int dismissals = 0;
void DismissDeviceFlyoutFromHook() { ++dismissals; }
UINT g_deviceFlyoutActivityMessage = WM_DEVICEFLYOUTACTIVITY_FIRST + 2;
UINT_PTR timerArmed = 0;
bool timerSucceeds = true;
UINT_PTR SetTimer(HWND, UINT_PTR id, UINT, void*) { timerArmed = timerSucceeds ? id : 0; return timerArmed; }
void KillTimer(HWND, UINT_PTR) { timerArmed = 0; }
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return -1; }
'''

checks = r'''
int main() {
    int failures = 0, count = 0;
    auto check = [&](bool ok, char const* name) {
        ++count;
        if (!ok) { ++failures; std::cout << "FAIL " << name << '\n'; }
    };
    constexpr UINT clickMessage = 101, activityMessage = 102;
    auto state = std::make_shared<FlyoutInputHook::State>(hwnd(1), clickMessage, activityMessage);
    auto& owner = g_deviceFlyoutInputHook;
    owner.state = state;
    FlyoutInputHook::current = state.get();
    MSLLHOOKSTRUCT mouse { { -5, 23 } };
    auto mouseData = reinterpret_cast<LPARAM>(&mouse);
    auto keyData = reinterpret_cast<LPARAM>(&mouse); // Key details are intentionally ignored.

    // Bluetooth can deactivate the host while the cursor remains on the panel.
    radioGrace = true;
    DeviceFlyoutForegroundHook(nullptr, EVENT_SYSTEM_FOREGROUND, hwnd(3), OBJID_WINDOW, 0, 0, 0);
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 0, "radio deactivation and pointer on the panel keep it open");
    radioGrace = false;
    check(dismissals == 0, "the end of radio grace alone does not close the panel");

    radioGrace = true;
    hit = hwnd(3);
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 1, "moving over the foreground fullscreen game dismisses during grace");
    hit = hwnd(2);
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 1, "movement over the flyout does not dismiss");
    hit = hwnd(3);
    bounds = { 10, 10, 1910, 1030 };
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 1, "movement over a windowed app does not dismiss");
    bounds = { -1920, 0, 0, 1080 };
    monitorBounds = bounds;
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 2, "fullscreen movement works on a monitor with negative coordinates");
    hit = hwnd(5);
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 2, "movement over another process does not count as game input");
    foreground = hwnd(4); hit = hwnd(4);
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 2, "the desktop shell is not treated as a fullscreen game");

    foreground = hwnd(3);
    HandleDeviceFlyoutKeyboardActivity(hwnd(3));
    check(dismissals == 3, "keyboard activity in the foreground game dismisses during grace");
    HandleDeviceFlyoutKeyboardActivity(hwnd(1));
    HandleDeviceFlyoutKeyboardActivity(hwnd(5));
    check(dismissals == 3, "own or stale keyboard targets do not dismiss");
    g_app.deviceFlyoutVisible = false;
    HandleDeviceFlyoutKeyboardActivity(hwnd(3));
    HandleDeviceFlyoutFullscreenMovement();
    check(dismissals == 3, "closed flyouts ignore input");
    g_app.deviceFlyoutVisible = true;
    radioGrace = false;
    DeviceFlyoutForegroundHook(nullptr, EVENT_SYSTEM_FOREGROUND, hwnd(3), OBJID_WINDOW, 0, 0, 0);
    check(dismissals == 4, "ordinary foreground changes still dismiss after grace");

    // Exercise the production callbacks, including message coalescing and key-up.
    posts.clear();
    foreground = hwnd(1);
    FlyoutInputHook::MouseHookProc(0, WM_MOUSEMOVE, mouseData);
    FlyoutInputHook::KeyboardHookProc(0, WM_KEYDOWN, keyData);
    check(posts.empty(), "input addressed to the flyout is not forwarded as external activity");
    foreground = hwnd(3);
    FlyoutInputHook::MouseHookProc(0, WM_MOUSEMOVE, mouseData);
    FlyoutInputHook::MouseHookProc(0, WM_MOUSEMOVE, mouseData);
    check(posts.size() == 1 && posts[0].message == activityMessage && posts[0].lParam == 0,
        "mouse movement is coalesced until the UI acknowledges it");
    owner.AcknowledgeMovement();
    FlyoutInputHook::MouseHookProc(0, WM_MOUSEMOVE, mouseData);
    check(posts.size() == 2, "movement is observed again after acknowledgment");
    FlyoutInputHook::KeyboardHookProc(0, WM_KEYDOWN, keyData);
    check(posts.size() == 3 && posts.back().message == activityMessage && posts.back().lParam == 1
        && posts.back().wParam == reinterpret_cast<WPARAM>(hwnd(3)),
        "keyboard input in the game forwards only the destination window");
    FlyoutInputHook::KeyboardHookProc(0, WM_KEYUP, keyData);
    check(posts.size() == 4 && posts.back().message == activityMessage && posts.back().lParam == 1
        && posts.back().wParam == reinterpret_cast<WPARAM>(hwnd(3)),
        "key release after switching windows forwards only the destination window");
    FlyoutInputHook::MouseHookProc(0, WM_LBUTTONDOWN, mouseData);
    check(posts.size() == 5 && posts.back().message == clickMessage
        && static_cast<INT_PTR>(posts.back().wParam) == -5 && posts.back().lParam == 23,
        "outside-click coordinates retain negative screen positions");
    owner.AcknowledgeMovement();
    postSucceeds = false;
    FlyoutInputHook::MouseHookProc(0, WM_MOUSEMOVE, mouseData);
    check(!state->movementPending, "a failed post releases the movement gate");
    state->active = false;
    postSucceeds = true;
    FlyoutInputHook::KeyboardHookProc(0, WM_KEYDOWN, keyData);
    check(posts.size() == 5 && forwarded == 10, "stopped hooks ignore input and always forward it");

    // Run the actual window-procedure branches with a fake timer and foreground.
    state->active = true;
    state->movementPending = true;
    hit = hwnd(2);
    foreground = hwnd(3);
    radioGrace = true;
    auto activity = g_deviceFlyoutActivityMessage;
    DispatchInput(hwnd(1), WM_DISMISSDEVICEFLYOUT, activity - 1, 0);
    check(hideRequests == 0 && g_deviceFlyoutMotion.reopenAfterClose,
        "a dismissal queued before reopening cannot close the new flyout");
    DispatchInput(hwnd(1), WM_DISMISSDEVICEFLYOUT, activity, 0);
    check(hideRequests == 1 && !g_deviceFlyoutMotion.reopenAfterClose,
        "a dismissal for the current opening reaches the flyout");
    DispatchInput(hwnd(1), activity - 1, 0, 0);
    check(timerArmed == 0 && state->movementPending && dismissals == 4,
        "activity queued by an earlier flyout opening is ignored");
    DispatchInput(hwnd(1), activity, 0, 0);
    check(timerArmed == activity && state->movementPending && dismissals == 4,
        "movement over the panel arms one throttled recheck without dismissing");
    hit = hwnd(3);
    DispatchInput(hwnd(1), WM_TIMER, activity, 0);
    check(timerArmed == 0 && !state->movementPending && dismissals == 5,
        "the timer observes final movement into the game and releases the gate");
    DispatchInput(hwnd(1), activity, reinterpret_cast<WPARAM>(hwnd(3)), 1);
    check(dismissals == 6, "current external keyboard activity reaches dismissal during grace");
    hit = hwnd(2);
    state->movementPending = true;
    timerSucceeds = false;
    DispatchInput(hwnd(1), activity, 0, 0);
    check(!state->movementPending && dismissals == 6, "timer failure releases the gate without closing the panel");
    FlyoutInputHook::current = nullptr;
    std::cout << "FLYOUT_DISMISSAL checks=" << count << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
'''

dispatch = ('LRESULT DispatchInput(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {\n'
            'switch (message) {\ncase WM_DISMISSDEVICEFLYOUT:\n' + dismiss_branch
            + '\ncase WM_TIMER:\n' + timer_branch + '\ndefault:\n'
            + activity_branch + '\nreturn -1;\n}\nreturn 0;\n}\n')
source = prefix + '\n' + hook + '\nFlyoutInputHook g_deviceFlyoutInputHook;\n' + '\n'.join(
    extract(name, tray) for name in (
        'IsFullscreenForegroundAtCursor', 'HandleDeviceFlyoutFullscreenMovement',
        'HandleDeviceFlyoutKeyboardActivity', 'DeviceFlyoutForegroundHook')) + '\n' + dispatch + '\n' + checks

with tempfile.TemporaryDirectory(prefix='aulay-flyout-') as directory:
    source_path = Path(directory) / 'flyout.cpp'
    source_path.write_text(source, encoding='utf-8')
    binary = Path(directory) / ('flyout.exe' if os.name == 'nt' else 'flyout')
    compiler = os.environ.get('CXX') or next((c for c in ('g++', 'clang++', 'cl') if shutil.which(c)), None)
    if not compiler:
        raise RuntimeError('Set CXX or run in a C++ compiler developer environment')
    if Path(compiler).stem.lower() == 'cl':
        command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8', str(source_path), '/Fe:' + str(binary)]
    else:
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', str(source_path), '-o', str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    sys.exit(subprocess.run([str(binary)]).returncode)
