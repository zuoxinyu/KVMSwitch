// system.cpp : 桌面通知、命令执行、自启动、快捷方式与进程间通信
#include "kvmswitch.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

namespace fs = std::filesystem;

namespace kvm {

namespace {

std::string runCapture(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    ::pclose(pipe);
    return output;
}

std::string autostartPath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg) base = xdg;
    else if (home && *home) base = fs::path(home) / ".config";
    else base = "/tmp";
    return (base / "autostart" / "kvmswitch.desktop").string();
}

std::string desktopDir() {
    const std::string fromTool = trim(runCapture("xdg-user-dir DESKTOP 2>/dev/null"));
    if (!fromTool.empty() && fromTool != "/") return fromTool;
    if (const char* env = std::getenv("XDG_DESKTOP_DIR"); env && *env) return env;
    if (const char* home = std::getenv("HOME"); home && *home) return (fs::path(home) / "Desktop").string();
    return "/tmp";
}

std::string escapeDesktopValue(const std::string& value) {
    std::string out;
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '`': out += "\\`"; break;
        case '$': out += "\\$"; break;
        case '%': out += "%%"; break;
        case '\n': out += "\\n"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string quoteExec(const std::string& value) {
    return "\"" + escapeDesktopValue(value) + "\"";
}

bool writeTextFile(const std::string& path, const std::string& content, mode_t mode = 0644) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << content;
    file.close();
    ::chmod(path.c_str(), mode);
    return true;
}

std::string desktopEntry(const std::string& name, const std::string& comment,
                         const std::string& execArgs, const std::string& icon) {
    std::string content;
    content += "[Desktop Entry]\n";
    content += "Version=1.0\n";
    content += "Type=Application\n";
    content += "Name=" + name + "\n";
    if (!comment.empty()) content += "Comment=" + comment + "\n";
    content += "Exec=" + quoteExec(getExePath());
    if (!execArgs.empty()) content += " " + execArgs;
    content += "\n";
    content += "Icon=" + icon + "\n";
    content += "Terminal=false\n";
    content += "StartupNotify=false\n";
    content += "Categories=Utility;Settings;\n";
    return content;
}

std::string installIcon() {
    const std::string source = getIconPath();
    if (source.empty()) return "video-display";

    // 如果源图标已经位于用户图标目录，直接使用
    const char* xdgData = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path dataHome;
    if (xdgData && *xdgData) dataHome = xdgData;
    else if (home && *home) dataHome = fs::path(home) / ".local" / "share";
    else return source;

    const fs::path destination = dataHome / "icons" / "hicolor" / "256x256" / "apps" / "kvmswitch.png";
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (!ec) {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
        if (!ec) return destination.string();
    }
    return source;
}

std::string shortcutIcon() {
    const std::string installed = installIcon();
    if (!installed.empty()) return installed;
    return "video-display";
}

} // namespace

void logMsg(const std::string& msg) {
    static const bool debug = std::getenv("KVMSWITCH_DEBUG") != nullptr;
    if (debug || ::isatty(STDERR_FILENO)) {
        std::fprintf(stderr, "[KVMSwitch] %s\n", msg.c_str());
    }
}

void notifyDesktop(const std::string& title, const std::string& message) {
#ifdef HAVE_LIBNOTIFY
    if (!notify_is_initted()) notify_init("KVMSwitch");
    NotifyNotification* notification = notify_notification_new(title.c_str(), message.c_str(), nullptr);
    if (notification) {
        const std::string icon = getIconPath();
        if (!icon.empty()) {
            GError* error = nullptr;
            GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(icon.c_str(), &error);
            if (pixbuf) {
                notify_notification_set_image_from_pixbuf(notification, pixbuf);
                g_object_unref(pixbuf);
            }
            if (error) g_error_free(error);
        }
        notify_notification_set_timeout(notification, 3000);
        GError* error = nullptr;
        const gboolean shown = notify_notification_show(notification, &error);
        if (error) g_error_free(error);
        g_object_unref(notification);
        if (shown) return;
    }
#endif

    // 无 libnotify 时回退到 notify-send（大多数桌面环境自带）
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        const std::string icon = getIconPath();
        if (!icon.empty()) {
            ::execlp("notify-send", "notify-send", "-a", "KVMSwitch", "-i", icon.c_str(),
                     "-t", "3000", title.c_str(), message.c_str(), nullptr);
        } else {
            ::execlp("notify-send", "notify-send", "-a", "KVMSwitch",
                     "-t", "3000", title.c_str(), message.c_str(), nullptr);
        }
        _exit(127);
    } else if (pid > 0) {
        ::waitpid(pid, nullptr, 0);
    }
}

void executeShellCommand(const std::string& command) {
    const std::string cmd = trim(command);
    if (cmd.empty()) return;

    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        const pid_t child = ::fork();
        if (child == 0) {
            const int devNull = ::open("/dev/null", O_RDWR);
            if (devNull >= 0) {
                ::dup2(devNull, STDIN_FILENO);
                ::dup2(devNull, STDOUT_FILENO);
                ::dup2(devNull, STDERR_FILENO);
                if (devNull > STDERR_FILENO) ::close(devNull);
            }
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        ::waitpid(pid, nullptr, 0);
    }
}

// ------------------------------------------------------------
// 开机自启动（XDG Autostart）
// ------------------------------------------------------------

bool isAutostartEnabled() {
    const std::string path = autostartPath();
    std::ifstream file(path);
    if (!file) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (toUpper(content).find("HIDDEN=TRUE") != std::string::npos) return false;
    if (toUpper(content).find("X-GNOME-AUTOSTART-ENABLED=FALSE") != std::string::npos) return false;
    return true;
}

bool setAutostart(bool enable, std::string* error) {
    const std::string path = autostartPath();
    if (!enable) {
        std::error_code ec;
        if (fs::exists(path, ec)) {
            fs::remove(path, ec);
            if (ec) {
                if (error) *error = "删除自启动文件失败: " + ec.message();
                return false;
            }
        }
        return true;
    }

    std::string content = desktopEntry("KVMSwitch", "KVMSwitch 显示器输入源快速切换 (常驻托盘)",
                                       "--tray", shortcutIcon());
    content += "X-GNOME-Autostart-enabled=true\n";
    if (!writeTextFile(path, content, 0644)) {
        if (error) *error = "写入自启动文件失败: " + path;
        return false;
    }
    return true;
}

// ------------------------------------------------------------
// 桌面快捷方式
// ------------------------------------------------------------

bool createDesktopShortcuts(const AppConfig& cfg, std::string* message) {
    const std::string dir = desktopDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (message) *message = "无法创建桌面目录: " + dir + " (" + ec.message() + ")";
        return false;
    }

    const std::string icon = shortcutIcon();
    struct Shortcut {
        std::string file;
        std::string name;
        std::string comment;
        std::string args;
    };
    std::vector<Shortcut> shortcuts = {
        {"切换显示器 - DP.desktop", "切换显示器 - DP", "一键切换显示器至 DisplayPort 输入", "dp"},
        {"切换显示器 - HDMI 1.desktop", "切换显示器 - HDMI 1", "一键切换显示器至 HDMI 1 输入", "hdmi1"},
        {"切换显示器 - HDMI 2.desktop", "切换显示器 - HDMI 2", "一键切换显示器至 HDMI 2 输入", "hdmi2"},
        {"切换显示器 - Type-C.desktop", "切换显示器 - Type-C", "一键切换显示器至 USB Type-C 输入", "typec"},
        {"切换显示器 - 轮流切换.desktop", "切换显示器 - 轮流切换", "在常用输入源之间轮流快速切换", "--toggle"},
        {"KVMSwitch - 托盘常驻.desktop", "KVMSwitch (系统托盘常驻)", "启动 KVMSwitch 并常驻系统托盘", "--tray"},
    };
    for (const auto& preset : cfg.presets) {
        shortcuts.push_back({"预设 - " + preset.name + ".desktop",
                             "预设 - " + preset.name,
                             "一键应用多显示器预设方案: " + preset.name,
                             "--preset " + quoteExec(preset.name)});
    }

    std::vector<std::string> created;
    for (const auto& shortcut : shortcuts) {
        const std::string path = (fs::path(dir) / shortcut.file).string();
        if (writeTextFile(path, desktopEntry(shortcut.name, shortcut.comment, shortcut.args, icon), 0755)) {
            created.push_back(shortcut.name);
        }
    }

    if (message) {
        std::string text = "已在桌面创建 " + std::to_string(created.size()) + " 个快捷方式：\n\n";
        for (const auto& name : created) text += "• " + name + "\n";
        text += "\n如果桌面环境提示快捷方式不可信，请右键选择“允许启动”或执行:";
        text += "\n  gio set <文件> metadata::trusted true";
        *message = text;
    }
    return !created.empty();
}

// ------------------------------------------------------------
// 单实例 IPC
// ------------------------------------------------------------

std::string traySocketPath() {
    return (fs::path(getRuntimeDir()) / "kvmswitch.sock").string();
}

bool sendTrayCommand(const std::string& command) {
    const std::string path = traySocketPath();
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return false;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        const std::string line = command + "\n";
        ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size());
    }
    ::close(fd);
    return ok;
}

bool trayIsRunning() {
    return sendTrayCommand("ping");
}

void openPathWithDesktop(const std::string& path) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setsid();
        ::execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
        ::execlp("gio", "gio", "open", path.c_str(), nullptr);
        _exit(127);
    } else if (pid > 0) {
        ::waitpid(pid, nullptr, 0);
    }
}

} // namespace kvm
