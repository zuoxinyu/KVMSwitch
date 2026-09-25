# KVMSwitch

轻量、便携的 Windows 显示器输入源快速切换工具 (基于 Windows 原生 DDC/CI API)。

无需依赖任何第三方运行库或外部工具，即可一键在 **DisplayPort (DP)**、**HDMI 1/2** 以及 **USB Type-C** 之间快速切换。

---

## 核心特性

- **纯原生实现**：基于 Windows 原生 `dxva2.dll` 与 VESA MCCS DDC/CI 协议开发，体积仅约 190 KB，零外部依赖，不弹黑框。
- **快捷弹出菜单 (Menu 模式)**：双击程序在鼠标指针位置弹出快捷选择菜单，标明当前激活的输入源，点击即可切换。
- **一键轮换 (Toggle 模式)**：在常用输入源（如 DP ⇄ Type-C ⇄ HDMI）之间顺序轮流切换。
- **桌面快捷方式一键生成**：内置 `--create-shortcuts` 功能，一键在桌面生成对应各输入源的 `.lnk` 快捷方式，可自由设置全局热键。
- **联动执行命令**：支持在切换到指定输入源时，在后台静默执行系统命令（例如联动唤醒睡眠中的 Mac 等）。
- **Per-Monitor 高 DPI 感知**：完美适配 4K 及高分屏。

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
KVMSwitch.exe                     根据 config.ini 配置运行 (默认弹出菜单)
KVMSwitch.exe dp                  直接切换到 DisplayPort (DP)
KVMSwitch.exe hdmi1               直接切换到 HDMI 1
KVMSwitch.exe hdmi2               直接切换到 HDMI 2
KVMSwitch.exe typec               直接切换到 USB Type-C
KVMSwitch.exe <数值>              直接切换到指定 VCP 60 数值 (如 15, 16, 17, 18)
KVMSwitch.exe --toggle (-t)       在常用输入源之间轮换切换
KVMSwitch.exe --menu (-m)         强制弹出快速选择菜单
KVMSwitch.exe --query (-q)        探测并显示当前所有显示器及输入源状态
KVMSwitch.exe --create-shortcuts  在桌面生成一键切换快捷方式
KVMSwitch.exe --help (-h)         查看帮助信息
```

---

## 配置文件 (config.ini)

程序运行时会在当前目录自动生成 `config.ini`：

```ini
[General]
; 运行模式: menu (弹出快捷菜单) 或 toggle (一键轮换)
mode = menu

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
on_switch_to_hdmi1 = ssh mac caffeinate -u -t 2
on_switch_to_hdmi2 = 
on_switch_to_dp = 
```

---

## 编译方法

使用 Visual Studio 2022 / 2026 打开 `KVMSwitch.sln`，选择 `Release` 与 `x64` 进行生成；或通过命令行生成：

```powershell
MSBuild KVMSwitch.sln /p:Configuration=Release /p:Platform=x64
```
