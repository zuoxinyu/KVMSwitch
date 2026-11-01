# AGENTS.md — KVMSwitch

便携式 Windows 托盘工具：通过 DDC/CI 切换显示器输入源 (HDMI / DP / Type-C)。单文件 Win32 C++，无框架；所有界面为托盘图标 + 弹出菜单，代码注释与 UI 文案均为中文，新增代码保持一致。

## 结构

### Windows 端 (`windows/`)
- `windows/KVMSwitch.cpp` — 全部应用逻辑（约 1200 行）：入口 `wWinMain`、配置、DDC/CI、托盘窗口、菜单、CLI。
- `windows/KVMSwitch.sln` / `KVMSwitch.vcxproj` — VS 工程，构建 x64 Release。
- `windows/build.bat` — Windows 目录本地构建脚本。
- `build_release.bat`（根目录）— 根目录一键构建脚本（vcvars64 + msbuild），构建成功后自动同步 `windows/x64/Release/KVMSwitch.exe` 至根目录。
- `KVMSwitch.exe`（根目录）— **用户实际双击的部署副本**。
- `config.ini` — 首次运行时生成在 exe 旁（便携设计）；`mode = tray | menu | toggle`。
- `tools/` — 诊断辅助（DDC/CI 计时、窗口枚举），不属于应用本体。

### macOS 端 (`macos/`)
- `macos/build.sh` — 纯命令行构建脚本，直接调用 `swiftc` 打包生成 `KVMSwitch.app`。
- `macos/Info.plist` — 声明 `LSUIElement = true`（Menu Bar 常驻）。
- `macos/Sources/KVMSwitchApp.swift` — `@main` 入口，基于 AppKit `NSStatusItem` + `NSMenuDelegate` 构建，具备菜单展开自动物理状态探测与矢量图标渲染。
- `macos/Sources/MonitorManager.swift` — 基于私有 `IOAVService` 与 `DCPAVServiceProxy` 节点的底层 DDC/CI 通信、多显示器枚举与重试控制。
- `macos/Sources/PresetManager.swift` — 多显示器输入源预设管理与持久化。
- `macos/Sources/SettingsView.swift` & `SettingsWindowManager.swift` — 独立设置管理窗口。

### Linux 端 (`linux/`)
- `linux/build.sh` — 纯命令行构建脚本，直接调用 `g++`；自动探测 GTK3 / Ayatana AppIndicator / libnotify，缺失 GUI 依赖时退化为纯 CLI。
- `linux/install.sh` — 安装脚本：安装到 `/usr/local`（或 `PREFIX`），部署图标、应用菜单和 udev 规则，并迁移用户配置；支持 `DESTDIR` 打包。
- `linux/src/kvmswitch.h` — 公共数据结构与接口声明。
- `linux/src/config.cpp` — INI 配置读写、路径解析、输入源别名与 UTF-8 处理。
- `linux/src/ddc.cpp` — 基于 `i2c-dev` 的 DDC/CI VCP 读写、Capabilities 读取与 EDID 解析。
- `linux/src/monitor.cpp` — sysfs 显示器枚举、主屏探测、切换、预设、轮换与状态缓存。
- `linux/src/system.cpp` — 桌面通知、联动命令、XDG 自启动、`.desktop` 快捷方式、单实例 socket。
- `linux/src/gui.cpp` — GTK3 托盘 / 快速切换窗口 / 图形设置窗口；未定义 `HAVE_GTK` 时提供 CLI 桩函数。
- `linux/60-kvmswitch-i2c.rules` — udev 规则，授予当前登录用户访问 `/dev/i2c-*` 的权限。

## 构建

- Windows CLI：根目录运行 `cmd /c build_release.bat`（自动编译并同步生成根目录 `KVMSwitch.exe`）。
- macOS CLI：进入 `macos/` 目录运行 `bash build.sh`。
- Linux CLI：进入 `linux/` 目录运行 `bash build.sh`（可选 `KVMSWITCH_NO_GUI=1` 强制纯 CLI）；`sudo bash linux/install.sh` 安装到系统。


## 架构要点（改代码前必读）

### Windows 端
- **性能关键**：`GetCapabilitiesStringLength` 实测约 1.5 秒（DDC/CI I2C 慢速事务）。Capabilities 结果按 `设备名|描述` 缓存在 `g_capsCache`（进程生命周期，`WM_DISPLAYCHANGE` 时清空）。任何高频路径（菜单点击、tooltip 定时器）只允许调 `GetVCPFeatureAndVCPFeatureReply(0x60)`（约 60ms），禁止重复读 caps。
- **快速路径**：托盘已常驻时再次运行 exe，只做 `FindWindowW` + `PostMessageW(KVMSwitch_ShowMenu)` 后退出，不做硬件查询。不要在这条路径之前插入任何枚举/查询。
- **单实例**：互斥量 `Local\KVMSwitch_SingleInstance_Mutex_zuoxinyu`。注意：常驻进程若卡死，会占住互斥量导致后续启动全部静默退出——诊断"图标不显示"时先查进程与托盘窗口是否存活。
- **DDC/CI 无超时**：显示器正被 KVM 切走时，读操作可能长时间阻塞。查询目前在 UI 线程同步执行（菜单点击时 `QueryAllMonitors`），重构前不要增加阻塞点。
- 显示器为泰坦军团 P275MV，VCP 0x60 代码非标准：DP=16、Type-C=15、HDMI1=17（启动时检测，config.ini 别名可覆盖）。
- 托盘图标通过全局 `g_trayNid` + `Shell_NotifyIconW(NIM_MODIFY)` 更新 tooltip/气泡，保持 `uID`/`hWnd` 字段不被覆盖。`EnsureTrayIconRegistered()` 负责自愈：15 秒定时器 tick 与 `TaskbarCreated` 时用 no-op `NIM_MODIFY` 探测图标，丢失则带全 `NIF_ICON|NIF_MESSAGE|NIF_TIP` 重新 `NIM_ADD`（重加必须重设 uFlags，`UpdateTrayTooltip` 会把它改成仅 `NIF_TIP`），持续失败写 `%TEMP%\KVMSwitch_tray.log`。
- 配置每次菜单点击都会重新 `LoadConfig`（故意为之，支持免重启改配置）；新增配置项遵循该模式。`ReadFileAsWideString` 与 `WriteFileAsUtf8WithBom` 统一处理 UTF-8（带/不带 BOM）及 UTF-16 编码，避免中文预设名称乱码。
- **多显示器预设与子菜单**：配置 `[Presets]` 预设方案，支持显示器序号 (1, 2)、角色 (primary, secondary)、设备名或描述子串灵活匹配；连接多台显示器时自动为各显示器生成二级子菜单，并支持在托盘菜单直接保存当前各屏状态为新预设（轻量纯 Win32 模态对话框）。
- **图形化设置面板 (Settings Dialog)**：与 macOS 端 `SettingsView` 深度对齐，纯 Win32 + Common Controls v6（`SysTabControl32`、`Microsoft YaHei UI` 字体、ClearType 渲染、`IsDialogMessageW` 键盘导航支持）。包含 Tab 0“预设方案”（预设列表、增删移序、实时重命名、物理显示器目标源 ComboBox）及 Tab 1“通用偏好与联动”（通知、自启动、运行模式、目标屏、DP/Type-C/HDMI 联动命令），支持托盘菜单 `⚙️ 设置...` 与命令行 `KVMSwitch.exe --settings (-s)` 唤出。

### Linux 端
- **DDC/CI 访问**：不依赖 `ddcutil`，直接打开 `/dev/i2c-*` 并通过 `i2c-dev` 发送 VCP 0x60 报文。显示器从 `/sys/class/drm/card*-*/` 枚举 (`status == connected`)，DDC 总线优先取连接器目录下的 `i2c-*` 子节点（不是 `ddc` 符号链接，AMDGPU 下后者可能指向错误的硬件 I2C 总线），并跳过 eDP/LVDS/DSI 等内置屏。
- **回复扫描与校验**：AMDGPU 的 AUX 通道会把回复放在循环缓冲区的任意偏移，`DdcBus::readReply` 会读取较大的缓冲区、扫描 `0x6E/0x50` 同步头并校验 XOR 校验和；请求校验种子为 `0x6E`，回复校验种子为 `0x50`。不要简化成固定读取 11 字节。
- **Capabilities 缓存**：`GetCapabilities` 较慢（实测约 0.6–1 秒），按 EDID FNV 哈希缓存到 `$XDG_CACHE_HOME/kvmswitch/caps/`；托盘菜单等高频路径只读取当前 VCP 0x60（约 50–100ms）。设置 `KVMSWITCH_NO_CAPS_CACHE=1` 可禁用缓存。
- **主显示器**：通过 `xrandr --query` 的 `primary` 或 `kscreen-doctor -o` 的 `priority 1` 匹配 DRM 连接器；可用 `KVMSWITCH_PRIMARY` 覆盖。Wayland 下没有可靠的全局坐标，`--menu` / 二次启动统一显示轻量快速窗口，而不是强行 `gtk_menu_popup_at_pointer`。
- **GTK 托盘**：优先 `libayatana-appindicator`（`HAVE_APPINDICATOR`），否则回退 `GtkStatusIcon`（仅 X11）。托盘菜单的“打开菜单时刷新”依赖 `GtkMenu::show`，并额外使用 15 秒定时刷新；不要在菜单回调里做耗时 Caps 查询。
- **单实例**：`$XDG_RUNTIME_DIR/kvmswitch.sock` 传 `show-menu` / `show-settings` / `refresh` / `quit`，并用 `kvmswitch.lock` 的 `flock` 防止双托盘。二次启动在 `main.cpp` 的快速路径中直接转发，不枚举硬件。
- **配置路径**：优先可执行文件旁的 `config.ini`；若目录只读则回退 `$XDG_CONFIG_HOME/kvmswitch/config.ini`。`KVMSWITCH_CONFIG` 可覆盖（测试时务必使用，避免污染仓库配置）。
- **权限**：`/dev/i2c-*` 需要当前用户可读写；仓库提供 `60-kvmswitch-i2c.rules`（`TAG+="uaccess"`），或把用户加入 `i2c` 组。

### macOS 端
- **Apple Silicon DDC/CI**：Apple Silicon (M1/M2/M3/M4) 无法使用传统的 `IOFramebuffer` I2C 接口；使用私有 `IOAVService`（通过 `@_silgen_name` 链接 `IOAVServiceCreateWithService`, `IOAVServiceReadI2C`, `IOAVServiceWriteI2C`）与 I/O Registry 中的 `DCPAVServiceProxy` 节点直接通信。
- **重试机制与延时**：Apple Silicon 上的 DDC 硬件通信极不稳定，`MonitorManager.swift` 中保留了多轮重试循环与 `usleep` 延迟（10–50ms），不可随意移除。
- **预设管理**：`PresetManager` 将预设以 JSON 形式持久化到 `UserDefaults`，以显示器名称为键。
- **免 Xcode 约定**：macOS 端依赖 `macos/build.sh` 直接调用命令行 `swiftc` 编译打包，请勿引入 `.xcodeproj` 或 `.xcworkspace`。新增 Swift 源文件时需同步在 `build.sh` 中显式指定。


## 环境坑（本机）

- Git Bash 直接引 `vcvars64.bat` 会被引号转义坑掉，用仓库里的 .bat 包装。
- 单独用 cl 编译含中文注释的源码必须加 `/utf-8`（源码为 UTF-8 无 BOM，GBK 默认代码页会把注释尾字节和换行合并，导致 `windows.h` 被吞进注释）。
- PowerShell 的 P/Invoke（`FindWindowW` 等）在本机返回过错误结果；诊断窗口/进程用 `tools/` 里的 C++ 工具。跨进程用 `NIM_MODIFY` 探测他人托盘图标会返回 ACCESS_DENIED（所有权限制），**不能**作为图标存在性判断。
- Linux 下 `xrandr` 在 Wayland 会话中可能只是 XWayland 的警告输出，但仍可用于探测 `primary`；`kscreen-doctor` 是 KDE 的补充路径。不要假设 `GDK_IS_WINDOW` 或全局鼠标坐标可用。
- `/sys/class/drm/card*-*/ddc` 在 AMDGPU 上可能指向硬件 I2C 总线（如 `i2c-4`）而不是 DDC AUX 总线（如 `i2c-7`）；必须优先使用连接器目录下的 `i2c-*` 子节点。
- Linux 用 `g++` 编译含中文源码时显式加 `-finput-charset=UTF-8 -fexec-charset=UTF-8`（`build.sh` 已包含）。
- Windows 11 Insider（build 28120）托盘排障经验：本机出现过"应用活着、`NIM_ADD` 返回成功、设置 `IsPromoted=1` 完好、但图标不渲染"的 Explorer 任务栏 wedge，`taskkill /F /IM explorer.exe` + 重启 explorer 后由应用的 `TaskbarCreated` 处理器自动恢复。截屏验证托盘不可靠（多显示器 + Wallpaper Engine 动态壁纸 + 任务栏可能自动隐藏）。强杀进程（`taskkill /F`）不执行 `NIM_DELETE`，会留下幽灵图标。

## 验证方式

无测试套件。改动的验证手段：`KVMSwitch.exe --query`（stdout 为管道时输出文本）、`--help`、再次运行 exe 的转发耗时、托盘交互截图。测试完结束常驻进程，不留后台实例。

Linux 端验证：

```bash
cd linux
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini bash build.sh
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini ./KVMSwitch --query
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini ./KVMSwitch --help
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini ./KVMSwitch --tray &
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini ./KVMSwitch --settings
KVMSWITCH_CONFIG=/tmp/kvmswitch-test/config.ini ./KVMSwitch --quit
```

`--query` 应显示 EDID 型号、连接器、`/dev/i2c-*`、当前输入和 Capabilities 解析出的支持输入；切换测试优先使用当前输入（如 `hdmi1`）避免显示器黑屏。测试完必须 `--quit` 或结束托盘进程，并清理 `/tmp` 测试配置。

## 提交

遵循 conventional commits（现有历史：`feat: ...`）。提交信息可用中文或英文，与现有一致即可。
