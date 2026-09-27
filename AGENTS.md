# AGENTS.md — KVMSwitch

便携式 Windows 托盘工具：通过 DDC/CI 切换显示器输入源 (HDMI / DP / Type-C)。单文件 Win32 C++，无框架；所有界面为托盘图标 + 弹出菜单，代码注释与 UI 文案均为中文，新增代码保持一致。

## 结构

### Windows 端
- `KVMSwitch.cpp` — 全部应用逻辑（约 1200 行）：入口 `wWinMain`、配置、DDC/CI、托盘窗口、菜单、CLI。
- `KVMSwitch.sln` / `KVMSwitch.vcxproj` — VS 工程，构建 x64 Release。
- `KVMSwitch.exe`（仓库根目录）— **用户实际双击的部署副本**，构建后需手动从 `x64\Release\KVMSwitch.exe` 覆盖同步。
- `config.ini` — 首次运行时生成在 exe 旁（便携设计）；`mode = tray | menu | toggle`。
- `tools/` — 诊断辅助（DDC/CI 计时、窗口枚举），不属于应用本体。
- `build_release.bat` — 命令行构建脚本（vcvars64 + msbuild）。

### macOS 端 (`macos/`)
- `macos/build.sh` — 纯命令行构建脚本，直接调用 `swiftc` 打包生成 `KVMSwitch.app`。
- `macos/Info.plist` — 声明 `LSUIElement = true`（Menu Bar 常驻）。
- `macos/Sources/KVMSwitchApp.swift` — `@main` 入口，基于 AppKit `NSStatusItem` + `NSMenuDelegate` 构建，具备菜单展开自动物理状态探测与矢量图标渲染。
- `macos/Sources/MonitorManager.swift` — 基于私有 `IOAVService` 与 `DCPAVServiceProxy` 节点的底层 DDC/CI 通信、多显示器枚举与重试控制。
- `macos/Sources/PresetManager.swift` — 多显示器输入源预设管理与持久化。
- `macos/Sources/SettingsView.swift` & `SettingsWindowManager.swift` — 独立设置管理窗口。

## 构建

- CLI：`cmd /c build_release.bat`（Git Bash 下 `cmd //c build_release.bat`）。
- 构建后同步根目录 exe，否则用户运行的是旧版本。

## 架构要点（改代码前必读）

- **性能关键**：`GetCapabilitiesStringLength` 实测约 1.5 秒（DDC/CI I2C 慢速事务）。Capabilities 结果按 `设备名|描述` 缓存在 `g_capsCache`（进程生命周期，`WM_DISPLAYCHANGE` 时清空）。任何高频路径（菜单点击、tooltip 定时器）只允许调 `GetVCPFeatureAndVCPFeatureReply(0x60)`（约 60ms），禁止重复读 caps。
- **快速路径**：托盘已常驻时再次运行 exe，只做 `FindWindowW` + `PostMessageW(KVMSwitch_ShowMenu)` 后退出，不做硬件查询。不要在这条路径之前插入任何枚举/查询。
- **单实例**：互斥量 `Local\KVMSwitch_SingleInstance_Mutex_zuoxinyu`。注意：常驻进程若卡死，会占住互斥量导致后续启动全部静默退出——诊断"图标不显示"时先查进程与托盘窗口是否存活。
- **DDC/CI 无超时**：显示器正被 KVM 切走时，读操作可能长时间阻塞。查询目前在 UI 线程同步执行（菜单点击时 `QueryAllMonitors`），重构前不要增加阻塞点。
- 显示器为泰坦军团 P275MV，VCP 0x60 代码非标准：DP=16、Type-C=15、HDMI1=17（启动时检测，config.ini 别名可覆盖）。
- 托盘图标通过全局 `g_trayNid` + `Shell_NotifyIconW(NIM_MODIFY)` 更新 tooltip/气泡，保持 `uID`/`hWnd` 字段不被覆盖。`EnsureTrayIconRegistered()` 负责自愈：15 秒定时器 tick 与 `TaskbarCreated` 时用 no-op `NIM_MODIFY` 探测图标，丢失则带全 `NIF_ICON|NIF_MESSAGE|NIF_TIP` 重新 `NIM_ADD`（重加必须重设 uFlags，`UpdateTrayTooltip` 会把它改成仅 `NIF_TIP`），持续失败写 `%TEMP%\KVMSwitch_tray.log`。
- 配置每次菜单点击都会重新 `LoadConfig`（故意为之，支持免重启改配置）；新增配置项遵循该模式。

## 环境坑（本机）

- Git Bash 直接引 `vcvars64.bat` 会被引号转义坑掉，用仓库里的 .bat 包装。
- 单独用 cl 编译含中文注释的源码必须加 `/utf-8`（源码为 UTF-8 无 BOM，GBK 默认代码页会把注释尾字节和换行合并，导致 `windows.h` 被吞进注释）。
- PowerShell 的 P/Invoke（`FindWindowW` 等）在本机返回过错误结果；诊断窗口/进程用 `tools/` 里的 C++ 工具。跨进程用 `NIM_MODIFY` 探测他人托盘图标会返回 ACCESS_DENIED（所有权限制），**不能**作为图标存在性判断。
- Windows 11 Insider（build 28120）托盘排障经验：本机出现过"应用活着、`NIM_ADD` 返回成功、设置 `IsPromoted=1` 完好、但图标不渲染"的 Explorer 任务栏 wedge，`taskkill /F /IM explorer.exe` + 重启 explorer 后由应用的 `TaskbarCreated` 处理器自动恢复。截屏验证托盘不可靠（多显示器 + Wallpaper Engine 动态壁纸 + 任务栏可能自动隐藏）。强杀进程（`taskkill /F`）不执行 `NIM_DELETE`，会留下幽灵图标。

## 验证方式

无测试套件。改动的验证手段：`KVMSwitch.exe --query`（stdout 为管道时输出文本）、`--help`、再次运行 exe 的转发耗时、托盘交互截图。测试完结束常驻进程，不留后台实例。

## 提交

遵循 conventional commits（现有历史：`feat: ...`）。提交信息可用中文或英文，与现有一致即可。
