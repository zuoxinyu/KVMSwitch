#!/usr/bin/env bash
# KVMSwitch Linux 构建脚本
# 直接调用 g++，无需 CMake / Meson；GTK3 与 Ayatana AppIndicator 为可选依赖。
set -euo pipefail

cd "$(dirname "$0")"

CXX="${CXX:-g++}"
OUT="${OUT:-KVMSwitch}"
SOURCES=(
    src/main.cpp
    src/config.cpp
    src/ddc.cpp
    src/monitor.cpp
    src/system.cpp
    src/gui.cpp
)

BASE_FLAGS=(-std=c++17 -O2 -pipe -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
DEFINES=()
CFLAGS=()
LIBS=()

have_pkg() {
    command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$1" 2>/dev/null
}

GUI_ENABLED=0
APPINDICATOR_ENABLED=0
NOTIFY_ENABLED=0

if [ "${KVMSWITCH_NO_GUI:-0}" != "1" ] && have_pkg gtk+-3.0; then
    GUI_ENABLED=1
    DEFINES+=(-DHAVE_GTK)
    CFLAGS+=($(pkg-config --cflags gtk+-3.0))
    LIBS+=($(pkg-config --libs gtk+-3.0))

    if have_pkg ayatana-appindicator3-0.1; then
        APPINDICATOR_ENABLED=1
        DEFINES+=(-DHAVE_APPINDICATOR -DAPPINDICATOR_AYATANA)
        CFLAGS+=($(pkg-config --cflags ayatana-appindicator3-0.1))
        LIBS+=($(pkg-config --libs ayatana-appindicator3-0.1))
    elif have_pkg libappindicator3-0.1; then
        APPINDICATOR_ENABLED=1
        DEFINES+=(-DHAVE_APPINDICATOR)
        CFLAGS+=($(pkg-config --cflags libappindicator3-0.1))
        LIBS+=($(pkg-config --libs libappindicator3-0.1))
    fi
fi

if have_pkg libnotify; then
    NOTIFY_ENABLED=1
    DEFINES+=(-DHAVE_LIBNOTIFY)
    CFLAGS+=($(pkg-config --cflags libnotify))
    LIBS+=($(pkg-config --libs libnotify))
fi

echo "==> 编译 KVMSwitch (Linux)"
echo "    GUI:          $([ "$GUI_ENABLED" = 1 ] && echo yes || echo 'no (仅命令行)')"
echo "    AppIndicator: $([ "$APPINDICATOR_ENABLED" = 1 ] && echo yes || echo 'no (托盘回退/不可用)')"
echo "    libnotify:    $([ "$NOTIFY_ENABLED" = 1 ] && echo yes || echo 'no (使用 notify-send)')"

"$CXX" "${BASE_FLAGS[@]}" "${DEFINES[@]}" "${CFLAGS[@]}" \
    "${SOURCES[@]}" -o "$OUT" "${LIBS[@]}"

# 将图标复制到可执行文件旁，供托盘 / 快捷方式使用
if [ -f ../KVMSwitch.png ] && [ ! -f KVMSwitch.png ]; then
    cp ../KVMSwitch.png KVMSwitch.png
fi

if [[ "$OUT" = /* ]]; then
    echo "==> 构建完成: $OUT"
else
    echo "==> 构建完成: $(pwd)/$OUT"
fi
echo "    运行 ./KVMSwitch --help 查看用法；首次运行会在程序旁生成 config.ini。"
if [ "$APPINDICATOR_ENABLED" != 1 ] && [ "$GUI_ENABLED" = 1 ]; then
    echo "    提示: 未找到 Ayatana AppIndicator，托盘将回退到 GTK StatusIcon (仅 X11 可用)。"
fi
