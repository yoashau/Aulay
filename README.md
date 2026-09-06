# Aulay

**English** | [简体中文](README.zh_CN.md)

A lightweight Windows tray utility that lets your PC play audio sent by a paired Bluetooth device: the phone or tablet becomes the source, the PC acts as an A2DP Sink speaker, using the built-in Windows audio stack.

## Requirements

- Windows 10 (2004+) or Windows 11
- A Bluetooth adapter
- A Bluetooth audio source (phone, tablet) paired in Windows Settings

## Features

- **Tray device list** — every discoverable A2DP source with live status and one-click actions.
- **Quick Connect** — opens the A2DP sink link without touching the adapter.
- **Disconnect** — cancels pending work, forgets the reconnect preference and closes the link.
- **Force Connect** — resets the associated Bluetooth adapter first, then connects; retries never repeat the reset.
- **No sound** — records a diagnostic marker with an audio-timeline snapshot, then reconnects through the forced path.
- **Cancel** — available while connecting or while waiting for a retry.
- **Auto reconnect** — an unexpected disconnect of an established link triggers adapter-reset recovery, then automatic reconnection when enabled. The switch is the check box in the exit-confirmation flyout, and it also controls startup reconnect. Retries back off from 200 ms to a 300 s cap and continue until success, cancellation or exit.
- **Stateful tray tooltip** — connected devices, connecting or recovery at a glance.
- **Single instance** — launching a second copy shows a notice instead of duplicating the tray icon.

## Diagnostics

- *Copy Diagnostics* puts a structured snapshot on the clipboard; *Open Log Folder* opens `%LOCALAPPDATA%\Aulay\Logs`.
- App logs rotate at 4 MiB (2 backups), audio timelines at 32 MiB (3 backups); no-sound bundles keep up to 120 s / 8 MiB before and 30 s after a marker.
- No audio content is ever recorded; all log encoding and disk writes run on a bounded background queue, so the UI never blocks on disk.

## Architecture

C++20, single translation unit: Win32 for the tray host, C++/WinRT XAML islands for the panel. `Aulay.cpp` is only the application shell; the code lives in domain headers included in dependency order.

| Module | Responsibility |
| --- | --- |
| `Aulay.cpp` | Application shell: entry point, window procedure, shutdown |
| `AppModules.hpp` | Cross-module forward declarations |
| `ConnectionState.hpp` | Timing budgets, status names, debug snapshot helpers, radio-transition grace window, session/error state helpers |
| `DeviceListUi.hpp` | Device rows, per-row status text, empty state, tray tooltip |
| `BluetoothRecovery.hpp` | Radio access and bounded off/on adapter recovery |
| `ConnectionFlow.hpp` | Session lifecycle, connect attempt state machine, serialized request queue |
| `DeviceWatcher.hpp` | Device watcher lifecycle, restart backoff, list reconciliation |
| `TrayPanel.hpp` | Tray icon, device flyout with outside-click dismissal hooks, menus |

Supporting headers: `SettingsUtil.hpp` (atomic `Aulay.json` settings with a LocalAppData fallback store), `Diagnostics.hpp` / `DebugAudioMonitor.hpp` / `BackgroundLog.hpp` (bounded background logging), `AudioFlowPolicy.hpp` / `AudioFlowRecovery.hpp` (frame-flow recovery worker), `BluetoothRouting.hpp` (PnP-based adapter routing), `AsyncUtil.hpp` / `ConnectionTiming.hpp` (deadline and cancellation primitives), `I18n.hpp` (translations), `BuildVariant.hpp` (product identity).

## Known limitations

Windows processes the incoming A2DP stream itself. A phone that is simultaneously connected to another Bluetooth speaker may play choppy audio, because many A2DP sources do not handle concurrent sink links reliably; disconnecting or disabling media audio for the other sink is the reliable workaround.

## Build

Visual Studio 2022 with **Desktop development with C++**, a Windows 10 SDK, NuGet and Python 3. From a VS developer shell:

```sh
python -m pip install -r translate/requirements.txt
(cd translate && sh gen_rc.sh)
nuget restore Aulay.sln
msbuild Aulay.sln "-p:Configuration=Release;Platform=x64"
```

`build.cmd` runs the same build and restores NuGet packages automatically. The executable lands at `x64/Release/Aulay64.exe`; the solution also defines x86, ARM and ARM64 configurations (ARM32 requires the Windows SDK 10.0.22621.0, which the project selects explicitly).

## Tests

```sh
python tests/run_tests.py
```

runs the portable consistency and regression suites (Python 3, Git and a C++17 compiler). Native Windows fault tests run via `tests\run_windows_tests.cmd`; see [tests/README.md](tests/README.md) for the full matrix. CI builds all four architectures and runs the same checks on every push and pull request.

## License

MIT — see [LICENSE](LICENSE). Based on [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector) by Richard Yu (ysc3839); maintained by [yoashau](https://github.com/yoashau).
