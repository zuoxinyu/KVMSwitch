#!/bin/bash
set -e

APP_NAME="KVMSwitch"
APP_BUNDLE="${APP_NAME}.app"
MAC_OS_DIR="${APP_BUNDLE}/Contents/MacOS"
RESOURCES_DIR="${APP_BUNDLE}/Contents/Resources"

echo "Cleaning up old build..."
rm -rf "$APP_BUNDLE"
rm -f Sources/DDC.c Sources/DDC.h

echo "Creating App Bundle Structure..."
mkdir -p "$MAC_OS_DIR"
mkdir -p "$RESOURCES_DIR"

echo "Copying Info.plist & Resources..."
cp Info.plist "${APP_BUNDLE}/Contents/"
if [ -f "AppIcon.icns" ]; then
    cp AppIcon.icns "$RESOURCES_DIR/"
fi

# Locate compatible macOS SDK (preferring stable SDKs without unbundled macro plugins)
SDK_FLAG=""
for sdk in \
    "/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk" \
    "/Library/Developer/CommandLineTools/SDKs/MacOSX26.2.sdk" \
    "/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk"; do
    if [ -d "$sdk" ]; then
        SDK_FLAG="-sdk $sdk"
        break
    fi
done

echo "Compiling Swift code..."
swiftc $SDK_FLAG -parse-as-library Sources/KVMSwitchApp.swift Sources/ContentView.swift Sources/MonitorManager.swift Sources/PresetManager.swift Sources/SettingsView.swift Sources/SettingsWindowManager.swift -o "$MAC_OS_DIR/$APP_NAME" -framework ApplicationServices -framework SwiftUI -framework IOKit -framework AppKit

echo "Build complete: $APP_BUNDLE"
