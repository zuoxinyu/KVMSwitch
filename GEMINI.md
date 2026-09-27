# KVMSwitch

## Project Overview
**KVMSwitch** is a multi-platform DDC/CI monitor input switcher supporting Windows and macOS (Apple Silicon M1/M2/M3/M4).

The macOS version (`macos/`) is a native menu bar application written in Swift and AppKit. It provides multi-monitor DDC/CI control specifically tailored for Apple Silicon Macs. 

The primary feature is **Input Source Switching** with **Preset Management**, allowing users to define specific input source configurations for multiple displays and switch between them seamlessly. Because traditional I2C methods over `IOFramebuffer` fail on Apple Silicon hardware (like the Mac mini's built-in HDMI port), this app uses undocumented, private Apple `IOAVService` APIs (specifically interfacing with `DCPAVServiceProxy` nodes in the I/O Registry) to reliably read EDID information and send VCP codes.

### Key Technologies:
*   **Swift & AppKit / SwiftUI**: Used for the native status item menu (`NSStatusItem` + `NSMenuDelegate`) and settings window.
*   **IOKit**: Used to traverse the I/O Registry and locate `DCPAVServiceProxy` nodes.
*   **IOAVService**: Private Apple framework used to send I2C DDC commands (read EDID, read/write VCP codes) over the Apple Silicon Display Controller (DCP).
*   **App Architecture**: MVVM pattern with `MonitorManager` handling hardware states and `PresetManager` handling persistent user configurations via `UserDefaults`.

## Building and Running

### macOS
To compile the application into a `.app` bundle, navigate to the `macos` directory and run the build script:
```bash
cd macos
bash build.sh
```

### Run Instructions
After building, you can launch the app directly from the terminal. As it is a Menu Bar (UI Element) application, it will not appear in the Dock. Look for the display icon in your macOS status bar.
```bash
open KVMSwitch.app
```

## Development Conventions
*   **No Xcode Project**: The project relies on the `build.sh` script for compilation using the command-line Swift compiler (`swiftc`).
*   **Private APIs**: The codebase relies on linking private C functions to Swift using `@_silgen_name` (e.g., `IOAVServiceWriteI2C`, `IOAVServiceCreateWithService`). Ensure modifications to the DDC payload align strictly with the reverse-engineered `Arm64DDC` logic, including proper DDC/CI checksum calculations.
*   **UI Implementation**: The UI is entirely written in SwiftUI. The main dropdown resides in the Menu Bar, while a custom `SettingsWindowManager` handles the presentation of the detached Settings window to circumvent standard SwiftUI `Settings` scene limitations in background applications.
*   **Hardware Fallbacks**: Since DDC on Apple Silicon is notoriously finicky, hardware reads and writes in `MonitorManager.swift` employ multi-iteration retry loops and `usleep` delays to ensure high reliability.
