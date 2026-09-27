# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

## Build & Run

### macOS
```bash
cd macos && bash build.sh         # compile to KVMSwitch.app
open KVMSwitch.app                # launch (appears in menu bar, not Dock)
```

There is no Xcode project — `build.sh` invokes `swiftc` directly, linking against `IOKit`, `ApplicationServices`, and `SwiftUI`. All Swift source files must be listed explicitly in `build.sh` when new ones are added.

### Windows
```cmd
cmd /c build_release.bat
```

## Architecture

**KVMSwitch** is a cross-platform DDC/CI monitor input switcher for Windows (Win32 tray tool) and macOS (Apple Silicon Menu Bar tool, `LSUIElement = true`). On Apple Silicon Macs, it uses private `IOAVService` APIs instead of `IOFramebuffer` because the standard I2C path is broken on Apple Silicon hardware.

### Key components (`macos/Sources/`)

| File | Role |
|---|---|
| `KVMSwitchApp.swift` | `@main` entry; AppKit `NSStatusItem` + `NSMenuDelegate`, port vector icons & SF Symbols |
| `MonitorManager.swift` | Discovers displays via I/O Registry (`DCPAVServiceProxy` nodes), reads EDID, reads/writes VCP codes through `IOAVServiceReadI2C` / `IOAVServiceWriteI2C` |
| `PresetManager.swift` | Stores `[Preset]` to `UserDefaults` as JSON; a `Preset` maps monitor name → input source (`UInt16`, 0 = "Don't Change") |
| `ContentView.swift` | SwiftUI view fallback |
| `SettingsView.swift` | Detached settings window for preset CRUD |
| `SettingsWindowManager.swift` | Singleton that manages the detached `NSWindow`; works around SwiftUI `Settings` scene limitations in background apps |
| `Arm64DDC.swift` | Reference DDC/CI protocol implementation (from MonitorControl) in `macos/` |

### DDC/CI details

- Private API linkage via `@_silgen_name` (`IOAVServiceCreateWithService`, `IOAVServiceReadI2C`, `IOAVServiceWriteI2C`)
- Chip address `0x37`, data address `0x51`; VCP code `0x60` = Input Source
- Hardware writes use multi-iteration retry loops with `usleep` delays (10–50 ms) — DDC on Apple Silicon is unreliable without them
- Common input source values: USB-C `0x1B`, DP1 `0x0F`, DP2 `0x10`, HDMI1 `0x11`, HDMI2 `0x12`

## Development Conventions

- **No Xcode**: never add `.xcodeproj` or `.xcworkspace` files.
- **Private API changes**: any modification to DDC payloads must maintain correct DDC/CI checksum calculation as implemented in `Arm64DDC.swift`.
- **Hardware reliability**: keep retry loops and `usleep` delays in `MonitorManager.swift`; removing them causes flaky I2C writes on Apple Silicon.
