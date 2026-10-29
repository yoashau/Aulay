# Aulay

[English](README.md) | **简体中文**

轻量级 Windows 托盘工具：配对的手机/平板作为音源，电脑作为 A2DP Sink 音箱播放其音频，全程使用 Windows 自带的蓝牙音频栈。

## 使用要求

- Windows 10（2004+）或 Windows 11
- 蓝牙适配器
- 已在 Windows 设置中配对的蓝牙音源设备（手机、平板）

## 功能

- **托盘设备列表** — 列出所有可发现的 A2DP 音源，实时状态 + 一键操作。
- **Quick Connect** — 本次运行中首次连接先完成一次准备，之后正常情况下直接建立 A2DP Sink 连接；远端断连后的下一次手动连接会先清理一次残留状态，再建立链路。
- **Disconnect** — 取消排队中的操作、清除重连偏好并断开链路。
- **Force Connect** — 先复位关联的蓝牙适配器再连接；准备失败时允许补做一次；连接重试不会重复已成功的复位。
- **No sound** — 记录诊断标记与音频时间线快照，然后走强制路径重连。
- **Cancel** — 连接过程中或等待重试期间随时可取消。
- **启动重连** — 退出确认框中的 "Reconnect on next start" 控制下次启动时重连已记住的设备。手机关闭连接后，该设备暂停自动重连，直到手动点击 Quick Connect / Force Connect 或重启应用，避免切到耳机后被抢回播放。启动重连和手动首次连接都验证初始音频帧，采集就绪后连续 0.5 秒无帧时允许一次端点修复，收到音频帧、暂停/关闭或超时即结束验证。远端断连后延后蓝牙清理：下次手动 Quick Connect 在没有其他活动音频会话时先复位一次，再建立连接。蓝牙准备暂时失败时允许补做一次，权限拒绝或连续两次准备失败则停止本次请求。准备完成后的连接重试从 200ms 退避到 300s 封顶，直到成功、取消或退出。连接创建受可取消的截止期保护，迟到结果在后台关闭，同一设备不会重叠创建。
- **状态化托盘提示** — 悬停即见已连接设备、连接中或恢复中状态。
- **单实例保护** — 重复启动只弹提示，不会出现两个托盘图标。

## 诊断

- *Copy Diagnostics* 将结构化快照复制到剪贴板；*Open Log Folder* 打开 `%LOCALAPPDATA%\Aulay\Logs`。
- 应用日志 4MiB 轮询（2 个备份），音频时间线 32MiB（3 个备份）；无声标记包保留标记前 120 秒/8MiB、标记后 30 秒。
- 绝不录制音频内容；日志编码与落盘全部走有界后台队列，UI 永不因磁盘阻塞。

## 架构

C++20、单一编译单元：Win32 承载托盘宿主，C++/WinRT XAML island 承载面板。`Aulay.cpp` 只是应用壳，代码按领域放在按依赖顺序包含的头文件里。

| 模块 | 职责 |
| --- | --- |
| `Aulay.cpp` | 应用壳：入口、窗口过程、关机流程 |
| `AppModules.hpp` | 跨模块前向声明 |
| `ConnectionState.hpp` | 时序预算、状态名、调试快照助手、无线电切换宽限窗口、会话/错误状态助手 |
| `DeviceListUi.hpp` | 设备行、行状态文本、空状态、托盘 tooltip |
| `BluetoothRecovery.hpp` | 无线电访问与有界的关/开适配器恢复 |
| `ConnectionFlow.hpp` | 会话生命周期、连接尝试状态机、串行请求队列 |
| `DeviceWatcher.hpp` | 设备 watcher 生命周期、重启退避、列表对账 |
| `TrayPanel.hpp` | 托盘图标、设备 flyout 及其外部点击关闭钩子、菜单 |

支撑头文件：`SettingsUtil.hpp`（`Aulay.json` 原子设置，带 LocalAppData 回退存储）、`Diagnostics.hpp` / `DebugAudioMonitor.hpp` / `BackgroundLog.hpp`（有界后台日志）、`AudioFlowPolicy.hpp` / `AudioFlowRecovery.hpp`（帧流恢复 worker）、`BluetoothRouting.hpp`（基于 PnP 的适配器路由）、`AsyncUtil.hpp` / `ConnectionTiming.hpp`（截止期与取消原语）、`I18n.hpp`（翻译）、`BuildVariant.hpp`（产品标识）。

## 已知限制

传入的 A2DP 流由 Windows 自行处理。手机若同时连接着另一个蓝牙音箱，可能出现卡顿——多数 A2DP 音源不能可靠处理并发 sink 链路；断开或禁用另一台设备的媒体音频是最可靠的规避方式。

## 构建

需要 Visual Studio 2022（**使用 C++ 的桌面开发**）、Windows 10 SDK、NuGet 与 Python 3。在 VS 开发者命令行中：

```sh
python -m pip install -r translate/requirements.txt
(cd translate && sh gen_rc.sh)
nuget restore Aulay.sln
msbuild Aulay.sln "-p:Configuration=Release;Platform=x64"
```

`build.cmd` 执行同样的构建并自动恢复 NuGet 包。产物在 `x64/Release/Aulay64.exe`；解决方案同时定义 x86、ARM、ARM64 配置（ARM32 需要 Windows SDK 10.0.22621.0，工程已显式选择）。

图标使用 16 DIP 矢量轮廓、1 DIP 笔画和 `#202020` 前景色，各 DPI 尺寸直接渲染。需要重新生成时，在 Windows PowerShell 中运行 `powershell -STA -File tools/make-icon.ps1`；只依赖系统 WPF，不需要额外图像库。

托盘界面保留原有设备行结构，统一采用 8 DIP 圆角、15 DIP 主要文字、主题感知的 Acrylic 和细边框；操作按钮宽 124 DIP，避免 15 DIP 标签被按钮边框和内边距裁切。右键菜单使用平面悬停／按下状态。设备面板整体向上滑入（250 ms）、向下滑出（167 ms），支持收起中重新展开，并遵守系统动画、高对比度及退出清理设置。

## 测试

```sh
python tests/run_tests.py
```

运行可移植的一致性与回归套件（需要 Python 3、Git 与 C++17 编译器）。Windows 原生故障测试用 `tests\run_windows_tests.cmd` 运行；完整矩阵见 [tests/README.md](tests/README.md)。CI 在每次推送与 PR 时构建全部四种架构并执行相同检查。

## 许可

MIT — 见 [LICENSE](LICENSE)。基于 Richard Yu（ysc3839）的 [AudioPlaybackConnector](https://github.com/ysc3839/AudioPlaybackConnector)；由 [yoashau](https://github.com/yoashau) 维护。
