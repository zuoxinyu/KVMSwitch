# Copilot Instructions

## Build and validation commands

```bash
cd macos && bash build.sh
```

Rebuilds the app bundle at `macos/KVMSwitch.app`.

```bash
cd macos && open KVMSwitch.app
```

Launches the menu bar app. It is an `LSUIElement` app, so it appears in the macOS menu bar instead of the Dock.

## High-level architecture (macOS)

- `macos/build.sh` is the build script for the shipping macOS app. It compiles directly with `swiftc` from files under `macos/Sources/` and bundles the result into `KVMSwitch.app`.
- `macos/Sources/KVMSwitchApp.swift` is the app entry point using AppKit `NSStatusItem` + `NSMenuDelegate`, supporting instant physical monitor sync and custom vector port icons.
- `macos/Sources/SettingsWindowManager.swift` and `macos/Sources/SettingsView.swift` implement preset editing in a detached `NSWindow`.
- `macos/Sources/PresetManager.swift` persists presets as JSON in `UserDefaults` under the `presets` key.
- `macos/Sources/MonitorManager.swift` provides hardware DDC/CI communication on Apple Silicon via private `IOAVService` and `DCPAVServiceProxy` nodes.
- Each `Monitor` instance owns a serial DDC queue. Reads and writes happen off the main thread, while published UI state is pushed back to the main thread.
- `MonitorSuit/Arm64DDC.swift`, `MonitorSuit/Sources/Dummy.swift`, and `MonitorSuit/testAppleSiliconDDC*.swift` are low-level or experimental DDC utilities kept in the repo for reference, but they are not compiled into the app by `build.sh`.

## Key conventions

- If you add, remove, or rename a shipping Swift source file, update `MonitorSuit/build.sh`. The app build does not discover source files automatically.
- Preserve the split between main-thread AppKit access and background DDC/process work. `discoverMonitors()` intentionally reads `NSScreen` data on the main thread before doing `ddcctl` work in the background.
- Presets are keyed by `Monitor.name`, not by a stable hardware identifier. Changes to monitor naming or discovery behavior can break previously saved presets.
- Input-source constants are part of the app's contract: USB-C `0x1B`, DisplayPort 1 `0x0F`, DisplayPort 2 `0x10`, HDMI 1 `0x11`, HDMI 2 `0x12`. In preset editing, `0` means "Don't Change".
- The input-source list is duplicated in `MonitorSuit/Sources/ContentView.swift` and `MonitorSuit/Sources/MonitorManager.swift`; keep both definitions in sync unless you refactor them into a single source.
- Runtime monitor control assumes `ddcctl` is installed at `/opt/homebrew/bin/ddcctl` or `/usr/local/bin/ddcctl`. If you change that integration, update discovery, read, and write paths consistently.
