#!/usr/bin/env bash
# KVMSwitch Linux 安装脚本
# 默认安装到 /usr/local；使用 sudo 运行可同时安装 udev 规则。
# 用户级安装示例: PREFIX="$HOME/.local" bash install.sh
set -euo pipefail

cd "$(dirname "$0")"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"
BIN_DIR="${DESTDIR}${PREFIX}/bin"
APP_DIR="${DESTDIR}${PREFIX}/share/applications"
ICON_DIR="${DESTDIR}${PREFIX}/share/icons/hicolor/256x256/apps"
UDEV_DIR="${DESTDIR}/etc/udev/rules.d"

if [ ! -x ./KVMSwitch ]; then
    echo "错误: 未找到 linux/KVMSwitch，请先运行 bash build.sh" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ] && [ ! -w "$PREFIX" ]; then
    echo "错误: 当前用户对 $PREFIX 没有写权限。" >&2
    echo "  系统级安装: sudo bash install.sh" >&2
    echo "  用户级安装: PREFIX=\"\$HOME/.local\" bash install.sh" >&2
    exit 1
fi

echo "==> 安装 KVMSwitch 到 $PREFIX"
install -d -m 755 "$BIN_DIR" "$APP_DIR" "$ICON_DIR"
install -m 755 KVMSwitch "$BIN_DIR/KVMSwitch"

ICON_SOURCE="KVMSwitch.png"
if [ ! -f "$ICON_SOURCE" ]; then
    ICON_SOURCE="../KVMSwitch.png"
fi
if [ -f "$ICON_SOURCE" ]; then
    install -m 644 "$ICON_SOURCE" "$ICON_DIR/kvmswitch.png"
    ICON_NAME="kvmswitch"
else
    ICON_NAME="video-display"
fi

DESKTOP_FILE="${APP_DIR}/kvmswitch.desktop"
cat > "$DESKTOP_FILE" <<DESKTOP
[Desktop Entry]
Version=1.0
Type=Application
Name=KVMSwitch
Comment=显示器输入源快速切换 (DDC/CI)
Exec="${PREFIX}/bin/KVMSwitch"
Icon=${ICON_NAME}
Terminal=false
StartupNotify=false
Categories=Utility;
Keywords=DDC;CI;monitor;display;HDMI;DisplayPort;Type-C;KVM;
DESKTOP
chmod 644 "$DESKTOP_FILE"

if [ -z "$DESTDIR" ]; then
    if [ "$(id -u)" -eq 0 ]; then
        echo "==> 安装 udev 规则"
        install -m 644 60-kvmswitch-i2c.rules "$UDEV_DIR/60-kvmswitch-i2c.rules"
        command -v udevadm >/dev/null 2>&1 && {
            udevadm control --reload-rules || true
            udevadm trigger --subsystem-match=i2c-dev || true
        }
    else
        echo "提示: 未使用 root 安装，跳过 udev 规则。"
        echo "      若 DDC/CI 提示权限不足，请执行:"
        echo "      sudo cp linux/60-kvmswitch-i2c.rules /etc/udev/rules.d/"
        echo "      sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=i2c-dev"
    fi

    command -v gtk-update-icon-cache >/dev/null 2>&1 && \
        gtk-update-icon-cache -q -t -f "${PREFIX}/share/icons/hicolor" 2>/dev/null || true
    command -v update-desktop-database >/dev/null 2>&1 && \
        update-desktop-database "$APP_DIR" 2>/dev/null || true

    # 把当前程序旁的配置迁移到用户配置目录 (仅当目标不存在时)
    TARGET_USER="${SUDO_USER:-$(id -un)}"
    TARGET_HOME="$(getent passwd "$TARGET_USER" | cut -d: -f6 || true)"
    if [ -n "$TARGET_HOME" ] && [ -f config.ini ]; then
        TARGET_CONFIG_DIR="${TARGET_HOME}/.config/kvmswitch"
        TARGET_CONFIG="${TARGET_CONFIG_DIR}/config.ini"
        if [ ! -f "$TARGET_CONFIG" ]; then
            install -d -m 700 "$TARGET_CONFIG_DIR"
            install -m 600 config.ini "$TARGET_CONFIG"
            if [ "$(id -u)" -eq 0 ] && [ "$TARGET_USER" != "root" ]; then
                chown -R "$TARGET_USER:$(id -gn "$TARGET_USER")" "$TARGET_CONFIG_DIR"
            fi
            echo "==> 已迁移配置到 $TARGET_CONFIG"
        else
            echo "==> 保留已有配置: $TARGET_CONFIG"
        fi
    fi
fi

echo
echo "安装完成。"
echo "  可执行文件: ${BIN_DIR}/KVMSwitch"
echo "  桌面菜单:   ${DESKTOP_FILE}"
echo "  图标:       ${ICON_DIR}/kvmswitch.png"
if [ -n "$DESTDIR" ]; then
    echo "  注意: 使用了 DESTDIR=${DESTDIR}，未执行 udev/缓存/配置迁移。"
fi
echo
echo "现在可以从应用菜单启动 KVMSwitch，或运行:"
echo "  KVMSwitch --tray"
echo "可选: 开启开机自启动:"
echo "  KVMSwitch --autostart-enable"
