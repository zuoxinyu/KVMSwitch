# KVMSwitch

轻量、便携的 Windows 显示器输入源快速切换工具 (基于 Windows 原生 DDC/CI API)。

无需依赖任何第三方运行库或外部工具，即可一键在 **DisplayPort (DP)**、**HDMI 1/2** 以及 **USB Type-C** 之间快速切换。

---

## 核心特性

- **纯原生实现**：基于 Windows 原生 `dxva2.dll` 与 VESA MCCS DDC/CI 协议开发，体积仅约 200 KB，零外部依赖，不弹黑框。
- **常驻系统托盘 (Tray 模式)**：默认常驻 Windows 任务栏通知区域，鼠标悬浮即可查看当前激活的输入源，单击或右击随时唤出快捷切换菜单。多显示器环境下自动汇总展示各屏状态。
- **多显示器预设方案 (Presets)**：与 macOS 端深度对齐！在多显示器场景下一键联动切换各台显示器的输入端口（例如：“办公模式”切为 1:DP, 2:TypeC；“娱乐模式”切为 1:HDMI1, 2:HDMI2）。
- **多显示器独立子菜单**：连接多台显示器时，托盘菜单自动为每台物理显示器生成独立二级子菜单，清晰标明各自的当前输入状态及可选端口。
- **一键保存当前状态为预设**：直接在托盘菜单点击“💾 保存当前状态为多显示器预设...”，弹出原生轻量对话框输入名称，即刻保存至配置文件。
- **开机自启动支持**：在托盘菜单中一键勾选“开机自启动 (常驻系统托盘)”，或通过命令行快速配置，开机登录后静默常驻后台。
- **单实例与通信机制**：已在托盘运行时再次双击不会重复创建图标，而是直接呼出切换菜单；通过快捷方式或命令行切换输入源时，常驻托盘会自动同步更新状态与悬浮提示。
- **快捷弹出菜单 (Menu 模式)**：鼠标光标位置即刻弹出快捷选择菜单，标明当前激活的输入源，点击即可切换。
- **一键轮换 (Toggle 模式)**：在常用输入源（如 DP ⇄ Type-C ⇄ HDMI）之间顺序轮流切换。
- **桌面快捷方式一键生成**：内置 `--create-shortcuts` 功能，一键在桌面生成对应各输入源、所有已配置的多屏预设方案以及托盘常驻的 `.lnk` 快捷方式，可自由设置全局热键。
- **联动执行命令**：支持在切换到指定输入源后，在后台静默执行系统命令（例如联动唤醒睡眠中的 Mac 等）。
- **Per-Monitor 高 DPI 感知**：完美适配 4K 及多屏混合缩放。

---

## 默认输入源映射 (泰坦军团 P275MV 及通用显示器)

| 输入源名称 | VCP 代码 (十进制) | 十六进制代码 | 说明 |
| :--- | :---: | :---: | :--- |
| **DisplayPort (DP)** | **`16`** | `0x10` | 对应物理 DP 接口 |
| **USB Type-C** | **`15`** | `0x0F` | 对应物理 Type-C 接口 |
| **HDMI 1** | **`17`** | `0x11` | 对应 HDMI 1 接口 |
| **HDMI 2** | **`18`** | `0x12` | 对应 HDMI 2 接口 |

> 若您的显示器代码不同，可在 `config.ini` 的 `[Inputs]` 区域自由自定义映射。

---

## 命令行参数

```text
KVMSwitch.exe                     根据 config.ini 配置运行 (默认启动并常驻系统托盘)
KVMSwitch.exe --tray              强制以系统托盘常驻模式启动
KVMSwitch.exe dp                  直接切换到 DisplayPort (DP)
KVMSwitch.exe hdmi1               直接切换到 HDMI 1
KVMSwitch.exe hdmi2               直接切换到 HDMI 2
KVMSwitch.exe typec               直接切换到 USB Type-C
KVMSwitch.exe <数值>              直接切换到指定 VCP 60 数值 (如 15, 16, 17, 18)
KVMSwitch.exe --preset <名称> (-p)  一键应用指定的多显示器预设方案 (如: KVMSwitch.exe -p 办公模式)
KVMSwitch.exe --presets             列出所有已配置的多显示器预设方案及对应端口
KVMSwitch.exe --toggle (-t)       在常用输入源之间轮换切换
KVMSwitch.exe --menu (-m)         强制弹出快速选择菜单 (单次模式)
KVMSwitch.exe --query (-q)        探测并显示当前所有显示器及输入源状态
KVMSwitch.exe --autostart-enable   开启开机自启动 (常驻系统托盘)
KVMSwitch.exe --autostart-disable  关闭开机自启动
KVMSwitch.exe --autostart-status   查询开机自启动状态
KVMSwitch.exe --create-shortcuts  在桌面生成一键切换快捷方式 (包含所有预设快捷方式)
KVMSwitch.exe --help (-h)         查看帮助信息
```

---

## 配置文件 (config.ini)

程序运行时会在当前目录自动生成 `config.ini`：

```ini
[General]
; 运行模式: tray (常驻系统托盘，推荐)、menu (弹出快捷菜单) 或 toggle (一键轮换)
mode = tray

; 切换成功后是否显示气泡提示
notify = true

; 目标显示器: primary (主显示器) 或 all (所有显示器)
target_monitor = primary

; toggle 模式下循环切换的输入源列表
toggle_inputs = DP, HDMI1, TypeC

[Inputs]
; 输入源名称与 VCP 0x60 数值映射
DP = 16
DP1 = 16
HDMI1 = 17
HDMI2 = 18
TypeC = 15

[Commands]
; 切换到指定输入源后自动在后台执行的系统命令 (可选)
on_switch_to_typec = ssh mac caffeinate -u -t 2
on_switch_to_hdmi1 = 
on_switch_to_hdmi2 = 
on_switch_to_dp = 

[Presets]
; 多显示器联动预设方案 (与 macOS 端保持一致)
; 格式: 预设名称 = 显示器标识:输入源, 显示器标识:输入源 ...
; 显示器标识支持: 1, 2, primary, secondary, 设备名或显示器描述
; 输入源支持: DP, HDMI1, HDMI2, TypeC 或具体数值 (15, 16, 17 等)，0 表示保持不变
双屏办公 = 1:DP, 2:TypeC
娱乐影音 = 1:HDMI1, 2:HDMI2
```

---

## 编译方法

### Windows
使用 Visual Studio 打开 `windows/KVMSwitch.sln`，选择 `Release` 与 `x64` 进行生成；或在根目录下执行一键构建脚本（自动编译并将输出同步至根目录 `KVMSwitch.exe`）：

```cmd
cmd /c build_release.bat
```
也可以直接在 `windows/` 目录下运行 `build.bat`。


### macOS (Apple Silicon)
macOS 版本位于 `macos/` 目录，采用原生 Swift + AppKit 实现，无需庞大的 Xcode 项目文件，直接通过轻量编译脚本调用 `swiftc` 编译打包：

```bash
cd macos
bash build.sh
open KVMSwitch.app
```

---

## macOS 版本特性 (`macos/`)

- **Apple Silicon 原生硬件通信**：针对 M1/M2/M3/M4 芯片硬件特性，通过私有 `IOAVService` 接口与 I/O Registry 中的 `DCPAVServiceProxy` 节点直接通信，实现毫秒级硬件 DDC/CI 读写，彻底摆脱传统 `IOFramebuffer` 失效的问题。
- **状态栏原生集成**：基于 AppKit `NSStatusItem` 打造，启动后优雅常驻顶部状态栏。
- **点击自动硬件刷新**：每次点击状态栏图标时，底层自动同步从物理显示器读取 VCP 0x60 最新状态并刷新对勾，保证状态实时精准。
- **极简矢量硬件图标**：专属手绘 Type-C、DP、HDMI 物理端口矢量图标，完美配合 Apple SF Symbols，纯净无冗余 Emoji。
- **跨平台一致的联动与预设**：支持一键轮换 (Toggle)、预设方案保存以及切换输入源后的系统自动化命令执行。

