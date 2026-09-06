# Aulay tests

Run all portable checks from the repository root:

```sh
python tests/run_tests.py
```

Requirements: Python 3, Git, and a C++17 compiler (GCC or Clang for the portable runner; MSVC for the native Windows scripts). The runner works from any directory and returns nonzero if any suite fails. Compiler selection supports `CXX`.

## CI coverage

| Entry point | Environment | Coverage |
| --- | --- | --- |
| `test_project_layout.py` | Portable | Source references, IDE filters, icon inventory, build paths, version metadata, resource encoding, ignore rules and CI wiring |
| `test_regressions.py` | Portable + C++17 | Executes extracted connection-policy and queue functions; checks event integration constraints |
| `test_audio_flow.py` | Portable + C++17 | Frame-recovery policy, unbounded retry backoff and cleanup guards |
| `test_diagnostic_overhead.py` | Portable + C++17 | 30s detail expiry, asynchronous log queue, bounded backlog and drain |
| `test_hardening.py` | Portable | Source-level deadline, routing and settings constraints |
| `test_debug_markers.py` | Portable + C++17 | Executes marker/disconnect functions with fake UI/connections; checks unified diagnostics and Forced request dispatch |
| `test_debug_timeline.py` | Portable | Source-level collector and timeline constraints |
| `run_windows_tests.cmd` | Windows, Release x64 build | Native deadlines, routing selection, settings persistence, asynchronous shutdown drain and generated recovery fault harness |
| `run_debug_audio_test.cmd` | Windows, unified Release x64 build | Diagnostic snapshot formatting and collection |
| `run_debug_monitor_test.cmd` | Windows, unified Release x64 build | Native journal, queue, rotation, retention and callback behavior |
| `check_format.py` | Portable | clang-format compliance over the app sources (CI pins clang-format 19.1.5) |

CI uses `windows-2022` and Visual Studio 2022 (17.x) for the project's `v143` toolset, including ARM/ARM64 cross-compilers.

CI runs the format check, the portable suites, builds Release x64, runs the fault tests, then runs both native diagnostic suites. Pull requests and pushes to `master` run these checks; tags also build the other release architectures.

## Formatting

`.clang-format` is authoritative: tabs at width 4, WebKit-style braces, `SortIncludes: Never` (include order is load-bearing in this single translation unit) and `ColumnLimit: 0` (no reflow; some guard tests match source text verbatim). CI runs `tests/check_format.py` with clang-format pinned to 19.1.5 and fails on drift, so formatting is not optional. Locally run `python3 tests/check_format.py` with any clang-format 19+ on PATH. From WSL, note that `clang-format.exe` cannot resolve the style file through `\\wsl.localhost` paths and silently falls back to defaults: format a Windows-side copy of the tree with native paths and copy the results back.

Native scripts run in a VS x64 developer shell with C++20 support and restored NuGet packages. They use standard C++20 coroutines and the Windows SDK C++/WinRT headers from the developer shell's include path, for isolated native test compilation. Do not override them with the package-generated WinRT headers, which use experimental coroutines. CI builds one Aulay edition before running its native tests.

## Local integration checks

These are deliberate opt-in checks, not CI coverage:

- `run_windows_tests.cmd --probe`: read-only routing discovery on the local computer.
- `run_debug_monitor_test.cmd --audio`: creates the test program's own silent WASAPI session to verify endpoint/session events.
- `generate_debug_ui_fixture.py OUTPUT_CPP` and `run_debug_ui_smoke.ps1 -ExePath FIXTURE_EXE`: generate, build and exercise an instrumented UI fixture in a disposable source copy. The fixture source replaces `Aulay.cpp` only in that copy; build it with the normal unified build. Use a dedicated executable directory because the smoke script writes `Aulay.json` beside the fixture EXE. Never distribute this instrumented executable.

`generate_recovery_test.py` and `generate_settings_test.py` are helpers invoked by `run_windows_tests.cmd`, not independent test suites. The settings harness executes the actual worker and drain with a pumped dispatcher and controlled I/O: fast completion, an in-flight older snapshot, a hung write, and write/snapshot failures.

Portable source checks are not full application builds. Native fault tests do not power-cycle Bluetooth. Real-device connection, audible playback, unexpected disconnects and exit during recovery need Windows hardware testing.

## 中文说明

`python tests/run_tests.py` 是统一跨平台入口，包含项目整洁性检查和六组功能/源码测试。Windows CI 构建统一的 Aulay，并执行三个非交互原生测试脚本。代码风格由 `.clang-format` 唯一约束，CI 通过 `tests/check_format.py`（固定 clang-format 19.1.5）强制执行。

`--probe`、`--audio` 和 UI 夹具属于显式选择的本地集成检查。UI 测试必须使用独立源码及 EXE 目录，它会写入专用配置；测试夹具不得作为产品发布。源码约束检查、原生故障模拟与真实蓝牙播放验证是不同层次的测试。

## Release assets

A tag push builds all four release architectures and creates a draft release containing the executable files directly: `Aulay64.exe`, `Aulay32.exe`, `AulayARM64.exe`, and `AulayARM.exe`. Missing files fail the release step. Publish the draft manually after checking it. All executable assets include diagnostics; there is no separate diagnostic edition.

The tested x64 executable is uploaded as an Actions artifact before the additional architecture builds, so it remains downloadable if a later build fails. A failed tag run does not create a complete release. Re-running a failed run uses that run's original commit; include workflow fixes in the commit referenced by the release tag before retrying.

Release downloads are the four EXE assets directly; no separately maintained local ZIP or `dist/` packaging step is required.
